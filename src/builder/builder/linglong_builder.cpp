/*
 * SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "linglong_builder.h"

#include "builder_config.h"
#include "depend_fetcher.h"
#include "module/package/bundle.h"
#include "module/package/info.h"
#include "module/repo/ostree_repo.h"
#include "module/runtime/app.h"
#include "module/runtime/container.h"
#include "module/runtime/oci.h"
#include "module/util/command_helper.h"
#include "module/util/desktop_entry.h"
#include "module/util/env.h"
#include "module/util/error.h"
#include "module/util/file.h"
#include "module/util/qserializer/json.h"
#include "module/util/qserializer/yaml.h"
#include "module/util/runner.h"
#include "module/util/sysinfo.h"
#include "source_fetcher.h"

#include <linux/prctl.h>
#include <sys/prctl.h>
#include <yaml-cpp/yaml.h>

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>

#include <csignal>
#include <fstream>

#include <sys/socket.h>
#include <sys/wait.h>

namespace linglong {
namespace builder {

linglong::util::Error commitBuildOutput(Project *project,
                                        QSharedPointer<AnnotationsOverlayfsRootfs> overlayfs)
{
    auto output = project->config().cacheInstallPath("files");
    linglong::util::ensureDir(output);

    auto upperDir =
            QStringList{ overlayfs->upper, project->config().targetInstallPath("") }.join("/");
    linglong::util::ensureDir(upperDir);

    auto lowerDir = project->config().cacheAbsoluteFilePath({ "overlayfs", "lower" });
    // if combine runtime, lower is ${PROJECT_CACHE}/runtime/files
    if (PackageKindRuntime == project->package->kind) {
        lowerDir = project->config().cacheAbsoluteFilePath({ "runtime", "files" });
    }
    linglong::util::ensureDir(lowerDir);

    QProcess fuseOverlayfs;
    fuseOverlayfs.setProgram("fuse-overlayfs");
    fuseOverlayfs.setArguments({
            "-f",
            "-o",
            "auto_unmount",
            "-o",
            "upperdir=" + upperDir,
            "-o",
            "workdir=" + overlayfs->workdir,
            "-o",
            "lowerdir=" + lowerDir,
            output,
    });

    qint64 fuseOverlayfsPid = -1;
    fuseOverlayfs.startDetached(&fuseOverlayfsPid);
    fuseOverlayfs.waitForStarted(-1);

    // FIXME: must wait fuse mount filesystem
    QThread::sleep(1);

    auto entriesPath = project->config().cacheInstallPath("entries");

    auto modifyConfigFile = [](const QString &srcPath,
                               const QString &targetPath,
                               const QString &fileType,
                               const QString &appId) -> util::Error {
        QDir configDir(srcPath);
        auto configFileInfoList = configDir.entryInfoList({ fileType }, QDir::Files);

        linglong::util::ensureDir(targetPath);

        for (auto const &fileInfo : configFileInfoList) {
            util::DesktopEntry desktopEntry(fileInfo.filePath());

            // set all section
            auto configSections = desktopEntry.sections();
            for (auto section : configSections) {
                auto exec = desktopEntry.rawValue("Exec", section);
                exec = QString("ll-cli run %1 --exec %2").arg(appId, exec);
                desktopEntry.set(section, "Exec", exec);

                // The section TryExec affects starting from the launcher, set it to null.
                auto tryExec = desktopEntry.rawValue("TryExec", section);

                if (!tryExec.isEmpty()) {
                    desktopEntry.set(section, "TryExec", "");
                }

                desktopEntry.set(section, "X-linglong", appId);

                auto err = desktopEntry.save(
                        QStringList{ targetPath, fileInfo.fileName() }.join(QDir::separator()));
                if (err) {
                    return WrapError(err, "save config failed");
                }
            }
        }
        return Success();
    };

    auto appId = project->package->id;
    // modify desktop file
    auto desktopFilePath = QStringList{ output, "share/applications" }.join(QDir::separator());
    auto desktopFileSavePath = QStringList{ entriesPath, "applications" }.join(QDir::separator());
    auto err = modifyConfigFile(desktopFilePath, desktopFileSavePath, "*.desktop", appId);
    if (err) {
        kill(fuseOverlayfsPid, SIGTERM);
        return err;
    }

    // modify context-menus file
    auto contextFilePath =
            QStringList{ output, "share/applications/context-menus" }.join(QDir::separator());
    auto contextFileSavePath = contextFilePath;
    err = modifyConfigFile(contextFilePath, contextFileSavePath, "*.conf", appId);
    if (err) {
        kill(fuseOverlayfsPid, SIGTERM);
        return err;
    }

    auto moveDir = [](const QStringList targetList,
                      const QString &srcPath,
                      const QString &destPath) -> linglong::util::Error {
        for (auto target : targetList) {
            auto srcDir = QStringList{ srcPath, target }.join(QDir::separator());
            auto destDir = QStringList{ destPath, target }.join(QDir::separator());

            if (util::dirExists(srcDir)) {
                util::copyDir(srcDir, destDir);
                util::removeDir(srcDir);
            }
        }

        return Success();
    };

    // link files/share to entries/share/
    linglong::util::linkDirFiles(project->config().cacheInstallPath("files/share"), entriesPath);
    // link files/lib/systemd to entries/systemd
    linglong::util::linkDirFiles(
            project->config().cacheInstallPath("files/lib/systemd/user"),
            QStringList{ entriesPath, "systemd/user" }.join(QDir::separator()));

    // Move runtime-install/files/debug to devel-install/files/debug
    linglong::util::ensureDir(project->config().cacheInstallPath("devel-install", "files"));
    err = moveDir({ "debug", "include", "mkspec ", "cmake", "pkgconfig" },
                  project->config().cacheInstallPath("files"),
                  project->config().cacheInstallPath("devel-install", "files"));
    if (err) {
        kill(fuseOverlayfsPid, SIGTERM);

        return err;
    }

    auto createInfo = [](Project *project) -> linglong::util::Error {
        QSharedPointer<package::Info> info;

        info->kind = project->package->kind;
        info->version = project->package->version;

        info->base = project->baseRef().toLocalRefString();

        info->runtime = project->runtimeRef().toLocalRefString();

        info->appid = project->package->id;
        info->name = project->package->name;
        info->description = project->package->description;
        info->arch = QStringList{ project->config().targetArch() };

        info->module = "runtime";
        info->size = linglong::util::sizeOfDir(project->config().cacheInstallPath(""));
        QFile infoFile(project->config().cacheInstallPath("info.json"));
        if (!infoFile.open(QIODevice::WriteOnly)) {
            return NewError(infoFile.error(), infoFile.errorString() + " " + infoFile.fileName());
        }

        // FIXME(black_desk): handle error
        if (infoFile.write(std::get<0>(util::toJSON(info))) < 0) {
            return NewError(infoFile.error(), infoFile.errorString());
        }
        infoFile.close();

        info->module = "devel";
        info->size = linglong::util::sizeOfDir(project->config().cacheInstallPath("devel-install"));
        QFile develInfoFile(project->config().cacheInstallPath("devel-install", "info.json"));
        if (!develInfoFile.open(QIODevice::WriteOnly)) {
            return NewError(develInfoFile.error(),
                            develInfoFile.errorString() + " " + develInfoFile.fileName());
        }
        if (develInfoFile.write(std::get<0>(util::toJSON(info))) < 0) {
            return NewError(develInfoFile.error(), develInfoFile.errorString());
        }
        develInfoFile.close();

        QFile sourceConfigFile(project->configFilePath());
        if (!sourceConfigFile.copy(project->config().cacheInstallPath("linglong.yaml"))) {
            return NewError(sourceConfigFile.error(), sourceConfigFile.errorString());
        }

        return Success();
    };

    err = createInfo(project);
    if (err) {
        kill(fuseOverlayfsPid, SIGTERM);
        return WrapError(err, "createInfo failed");
    }

    repo::OSTreeRepo repo(BuilderConfig::instance()->repoPath());

    err = repo.importDirectory(project->refWithModule("runtime"),
                               project->config().cacheInstallPath(""));

    if (err) {
        kill(fuseOverlayfsPid, SIGTERM);
        qCritical() << QString("commit %1 filed").arg(project->refWithModule("runtime").toString());
        return err;
    }

    err = repo.importDirectory(project->refWithModule("devel"),
                               project->config().cacheInstallPath("devel-install", ""));

    //        fuseOverlayfs.kill();
    kill(fuseOverlayfsPid, SIGTERM);

    return err;
};

package::Ref fuzzyRef(QSharedPointer<const JsonSerialize> obj)
{
    if (!obj) {
        return package::Ref("");
    }
    auto id = obj->property("id").toString();
    auto version = obj->property("version").toString();
    auto arch = obj->property("arch").toString();

    package::Ref ref(id);

    if (!version.isEmpty()) {
        ref.version = version;
    }

    if (!arch.isEmpty()) {
        ref.arch = arch;
    } else {
        ref.arch = util::hostArch();
    }

    return ref;
}

linglong::util::Error LinglongBuilder::config(const QString &userName, const QString &password)
{
    auto userJsonData =
            QString("{\"username\": \"%1\",\n \"password\": \"%2\"}").arg(userName).arg(password);

    QFile infoFile(util::getUserFile(".linglong/.user.json"));
    if (!infoFile.open(QIODevice::WriteOnly)) {
        return NewError(infoFile.error(), infoFile.errorString() + " " + infoFile.fileName());
    }
    if (infoFile.write(userJsonData.toLocal8Bit()) < 0) {
        return NewError(infoFile.error(), infoFile.errorString());
    }
    infoFile.close();

    return Success();
}

linglong::util::Error LinglongBuilder::initRepo()
{
    const QString defaultRepoName = BuilderConfig::instance()->remoteRepoName;
    const QString configUrl = BuilderConfig::instance()->remoteRepoEndpoint;

    QString repoUrl = configUrl.endsWith("/") ? configUrl + "repos/" + defaultRepoName
                                              : configUrl + "/repos/" + defaultRepoName;

    repo::OSTreeRepo repo(BuilderConfig::instance()->repoPath(), repoUrl, defaultRepoName);
    // if local ostree is not exist, create and init it
    if (!QDir(BuilderConfig::instance()->ostreePath()).exists()) {
        util::ensureDir(BuilderConfig::instance()->ostreePath());

        auto err = repo.init("bare-user-only");
        if (err) {
            return WrapError(err, "init ostree repo failed");
        }
    }

    // async builder.yaml to ostree config
    auto currentRemoteUrl = repo.remoteShowUrl(defaultRepoName);
    if (currentRemoteUrl.isEmpty()) {
        auto err = repo.remoteAdd(defaultRepoName, repoUrl);
        if (err) {
            return WrapError(err, "add ostree remote failed");
        }
    } else if (currentRemoteUrl != repoUrl) {
        repo.remoteDelete(defaultRepoName);
        auto err = repo.remoteAdd(defaultRepoName, repoUrl);
        if (err) {
            return WrapError(err, "add ostree remote failed");
        }
    }

    return Success();
}

// FIXME: should merge with runtime
int startContainer(QSharedPointer<Container> c, QSharedPointer<Runtime> r)
{
#define LL_VAL(str) #str
#define LL_TOSTRING(str) LL_VAL(str)

    QList<QList<quint64>> uidMaps = {
        { getuid(), 0, 1 },
    };
    for (auto const &uidMap : uidMaps) {
        QSharedPointer<IdMap> idMap(new IdMap);
        idMap->hostId = uidMap.value(0);
        idMap->containerId = uidMap.value(1);
        idMap->size = uidMap.value(2);
        r->linux->uidMappings.push_back(idMap);
    }

    QList<QList<quint64>> gidMaps = {
        { getgid(), 0, 1 },
    };
    for (auto const &gidMap : gidMaps) {
        QSharedPointer<IdMap> idMap(new IdMap);
        idMap->hostId = gidMap.value(0);
        idMap->containerId = gidMap.value(1);
        idMap->size = gidMap.value(2);
        r->linux->gidMappings.push_back(idMap);
    }

    r->root->path = c->workingDirectory + "/root";
    util::ensureDir(r->root->path);

    // write pid file
    QFile pidFile(c->workingDirectory + QString("/%1.pid").arg(getpid()));
    pidFile.open(QIODevice::WriteOnly);
    pidFile.close();

    qDebug() << "start container at" << r->root->path;
    // FIXME(black_desk): handle error
    auto data = std::get<0>(util::toJSON(r)).toStdString();

    int sockets[2]; // save file describers of sockets used to communicate with ll-box

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) != 0) {
        return EXIT_FAILURE;
    }

    prctl(PR_SET_PDEATHSIG, SIGKILL);

    pid_t boxPid = fork();
    if (boxPid < 0) {
        return -1;
    }

    if (0 == boxPid) {
        // child process
        (void)close(sockets[1]);
        auto socket = std::to_string(sockets[0]);
        char const *const args[] = { "ll-box", socket.c_str(), NULL };
        int ret = execvp(args[0], (char **)args);
        exit(ret);
    } else {
        close(sockets[0]);
        // FIXME: handle error
        (void)write(sockets[1], data.c_str(), data.size());
        (void)write(sockets[1], "\0", 1); // each data write into sockets should ended with '\0'

        c->pid = boxPid;
        waitpid(boxPid, nullptr, 0);
    }
    // return exit status from ll-box
    QString msg;
    const size_t bufSize = 1024;
    char buf[bufSize] = {};
    size_t ret = 0;

    ret = read(sockets[1], buf, bufSize);
    if (ret > 0) {
        msg.append(buf);
    }

    // FIXME: DO NOT name a class as "message" if it not for an common usage.
    auto result = util::loadJsonString<message>(msg);

    return result->wstatus;
}

linglong::util::Error LinglongBuilder::create(const QString &projectName)
{
    auto projectPath = QStringList{ QDir::currentPath(), projectName }.join(QDir::separator());
    auto configFilePath = QStringList{ projectPath, "linglong.yaml" }.join(QDir::separator());
    auto templeteFilePath =
            QStringList{ BuilderConfig::instance()->templatePath(), "example.yaml" }.join(
                    QDir::separator());

    auto ret = QDir().mkpath(projectPath);
    if (!ret) {
        return NewError(-1, "project already exists");
    }

    if (QFileInfo::exists(templeteFilePath)) {
        QFile::copy(templeteFilePath, configFilePath);
    } else {
        QFile::copy(":/example.yaml", configFilePath);
    }

    return Success();
}

linglong::util::Error LinglongBuilder::track()
{
    auto projectConfigPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "linglong.yaml" }.join(
                    QDir::separator());

    if (!QFileInfo::exists(projectConfigPath)) {
        qCritical() << "ll-builder should run in the root directory of the linglong project";
        return NewError(-1, "linglong.yaml not found");
    }

    auto project = QVariant::fromValue(YAML::LoadFile(projectConfigPath.toStdString()))
                           .value<QSharedPointer<Project>>();

    if ("git" == project->source->kind) {
        auto gitOutput = QSharedPointer<QByteArray>::create();
        QString latestCommit;
        // default git ref is HEAD
        auto gitRef = project->source->version.isEmpty() ? "HEAD" : project->source->version;
        auto err = util::Exec("git", { "ls-remote", project->source->url, gitRef }, -1, gitOutput);
        if (!err) {
            latestCommit = QString::fromLocal8Bit(*gitOutput).trimmed().split("\t").first();
            qDebug() << latestCommit;

            if (project->source->commit == latestCommit) {
                qInfo() << "current commit is the latest, nothing to update";
            } else {
                std::ofstream fout(projectConfigPath.toStdString());
                auto result = util::toYAML(project);
                fout << std::get<0>(result).toStdString();
                qInfo() << "update commit to:" << latestCommit;
            }
        }
    }

    return Success();
}

linglong::util::Error LinglongBuilder::build()
{
    auto err = initRepo();
    if (err) {
        return WrapError(err, "load local repo failed");
    }

    auto projectConfigPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "linglong.yaml" }.join("/");

    if (!QFileInfo::exists(projectConfigPath)) {
        qCritical() << "ll-builder should run in the root directory of the linglong project";
        return NewError(-1, "linglong.yaml not found");
    }

    try {
        auto project = QVariant::fromValue(YAML::LoadFile(projectConfigPath.toStdString()))
                               .value<QSharedPointer<Project>>();

        // convert dependencies which with 'source' and 'build' tag to a project type
        // TODO: building dependencies should be concurrency
        for (auto const &depend : project->depends) {
            if (depend->source && depend->build) {
                YAML::Node node;

                node["package"]["kind"] = "lib";
                node["package"]["id"] = depend->id.toStdString();
                node["package"]["version"] = depend->version.toStdString();

                if (project->runtime) {
                    node["runtime"]["id"] = project->runtime->id.toStdString();
                    node["runtime"]["version"] = project->runtime->version.toStdString();
                }

                if (project->base) {
                    node["base"]["id"] = project->base->id.toStdString();
                    node["base"]["version"] = project->base->version.toStdString();
                }

                auto subProject = QVariant::fromValue(node).value<QSharedPointer<Project>>();

                subProject->variables = depend->variables;
                subProject->source = depend->source;
                subProject->build = depend->build;

                subProject->generateBuildScript();
                subProject->setConfigFilePath(projectConfigPath);

                qInfo() << QString("building target: %1").arg(subProject->package->id);

                err = buildFlow(subProject.get());
                if (err) {
                    return err;
                }

                qInfo() << QString("build %1 success").arg(project->package->id);
            }
        }

        project->generateBuildScript();
        project->setConfigFilePath(projectConfigPath);

        qInfo() << QString("building target: %1").arg(project->package->id);

        err = buildFlow(project.get());
        if (err) {
            return err;
        }

        qInfo() << QString("build %1 success").arg(project->package->id);
    } catch (std::exception &e) {
        qCritical() << e.what();
        return NewError(-1, "failed to parse linglong.yaml");
    }

    return Success();
}

linglong::util::Error LinglongBuilder::buildFlow(Project *project)
{
    linglong::util::Error err;

    linglong::builder::BuilderConfig::instance()->setProjectName(project->package->id);

    if (!project->package || project->package->kind.isEmpty()) {
        return NewError(-1, "unknown package kind");
    }

    SourceFetcher sf(project->source, project);
    if (project->source) {
        qInfo() << QString("fetching source code from: %1").arg(project->source->url);
        auto err = sf.fetch();
        if (err) {
            return NewError(-1, "fetch source failed");
        }
    }
    // initialize some directories
    util::removeDir(project->config().cacheRuntimePath({}));
    util::removeDir(project->config().cacheInstallPath(""));
    util::removeDir(project->config().cacheInstallPath("devel-install", ""));
    util::removeDir(project->config().cacheAbsoluteFilePath({ "overlayfs" }));
    util::ensureDir(project->config().cacheInstallPath(""));
    util::ensureDir(project->config().cacheInstallPath("devel-install", ""));
    util::ensureDir(project->config().cacheAbsoluteFilePath(
            { "overlayfs", "up", project->config().targetInstallPath("") }));

    package::Ref baseRef("");

    QString hostBasePath;
    if (project->base) {
        baseRef = fuzzyRef(project->base);
    }

    if (project->runtime) {
        auto runtimeRef = fuzzyRef(project->runtime);

        QSharedPointer<BuildDepend> runtimeDepend(new BuildDepend);
        runtimeDepend->id = runtimeRef.appId;
        runtimeDepend->version = runtimeRef.version;
        DependFetcher df(runtimeDepend, project);
        err = df.fetch("", project->config().cacheRuntimePath(""));
        if (err) {
            return WrapError(err, "fetch runtime failed");
        }

        if (!project->base) {
            QFile infoFile(project->config().cacheRuntimePath("info.json"));
            if (!infoFile.open(QIODevice::ReadOnly)) {
                return NewError(infoFile.error(), infoFile.errorString());
            }
            // FIXME(black_desk): handle error
            auto info =
                    std::get<0>(util::fromJSON<QSharedPointer<package::Info>>(infoFile.readAll()));

            package::Ref runtimeBaseRef(info->base);

            baseRef.appId = runtimeBaseRef.appId;
            baseRef.version = runtimeBaseRef.version;
        }
    }

    QSharedPointer<BuildDepend> baseDepend(new BuildDepend);
    baseDepend->id = baseRef.appId;
    baseDepend->version = baseRef.version;
    DependFetcher baseFetcher(baseDepend, project);
    // TODO: the base filesystem hierarchy is just an debian now. we should change it
    hostBasePath = BuilderConfig::instance()->layerPath({ baseRef.toLocalRefString(), "" });
    err = baseFetcher.fetch("", hostBasePath);
    if (err) {
        return WrapError(err, "fetch runtime failed");
    }

    // depends fetch
    for (auto const &depend : project->depends) {
        DependFetcher df(depend, project);
        err = df.fetch("files", project->config().cacheRuntimePath("files"));
        if (err) {
            return WrapError(err, "fetch dependency failed");
        }
    }

    // FIXME(black_desk): these code should not be written in here. We should
    // use code in runtime/app.cpp
    QSharedPointer<Runtime> r;
    {
        auto [result, err] = util::fromJSON<QSharedPointer<Runtime>>(QString(":/config.json"));
        // FIXME(black_desk): handle error
        r = result;
    }

    auto container = QSharedPointer<Container>(new Container);
    err = container->create("");
    if (err) {
        return WrapError(err, "create container failed");
    }

    if (!r->annotations) {
        r->annotations.reset(new Annotations);
    }

    if (!r->annotations->overlayfs) {
        r->annotations->overlayfs.reset(new AnnotationsOverlayfsRootfs);

        r->annotations->overlayfs->lowerParent =
                project->config().cacheAbsoluteFilePath({ "overlayfs", "lp" });
        r->annotations->overlayfs->workdir =
                project->config().cacheAbsoluteFilePath({ "overlayfs", "wk" });
        r->annotations->overlayfs->upper =
                project->config().cacheAbsoluteFilePath({ "overlayfs", "up" });
    }

    // mount tmpfs to /tmp in container
    QSharedPointer<Mount> m(new Mount);
    m->type = "tmpfs";
    m->options = QStringList{ "nosuid", "strictatime", "mode=777" };
    m->source = "tmp";
    m->destination = "/tmp";
    r->mounts.push_back(m);

    // get runtime, and parse base from runtime
    for (const auto &rpath : QStringList{ "/usr", "/etc", "/var" }) {
        QSharedPointer<Mount> m(new Mount);
        m->type = "bind";
        m->options = QStringList{ "ro", "rbind" };
        m->source = QStringList{ hostBasePath, "files", rpath }.join("/");
        m->destination = rpath;
        r->annotations->overlayfs->mounts.push_back(m);
    }

    // mount project build result path
    // for runtime/lib, use overlayfs
    // for app, mount to opt
    auto hostInstallPath = project->config().cacheInstallPath("files");
    linglong::util::ensureDir(hostInstallPath);
    QList<QPair<QString, QString>> overlaysMountMap = {};

    if (project->runtime || !project->depends.isEmpty()) {
        overlaysMountMap.push_back({ project->config().cacheRuntimePath("files"), "/runtime" });
    }

    for (const auto &pair : overlaysMountMap) {
        QSharedPointer<Mount> m(new Mount);
        m->type = "bind";
        m->options = QStringList{ "ro" };
        m->source = pair.first;
        m->destination = pair.second;
        r->annotations->overlayfs->mounts.push_back(m);
    }
    r->annotations->containerRootPath = container->workingDirectory;

    auto containerSourcePath = "/source";

    QList<QPair<QString, QString>> mountMap = {
        { sf.sourceRoot(), containerSourcePath },
        { project->buildScriptPath(), BuildScriptPath },
    };

    for (const auto &pair : mountMap) {
        QSharedPointer<Mount> m(new Mount);
        m->type = "bind";
        m->source = pair.first;
        m->destination = pair.second;
        r->mounts.push_back(m);
    }

    r->process->args = QStringList{ "/bin/bash", "-e", BuildScriptPath };
    r->process->cwd = containerSourcePath;
    r->process->env.push_back("PATH=/runtime/bin:/usr/local/bin:/usr/bin:/bin:/usr/local/games:/"
                              "usr/games:/sbin:/usr/sbin");
    r->process->env.push_back("PREFIX=" + project->config().targetInstallPath(""));

    if (project->config().targetArch() == "x86_64") {
        r->process->env.push_back("PKG_CONFIG_PATH=/runtime/lib/x86_64-linux-gnu/pkgconfig");
        r->process->env.push_back("LIBRARY_PATH=/runtime/lib:/runtime/lib/x86_64-linux-gnu");
        r->process->env.push_back("LD_LIBRARY_PATH=/runtime/lib:/runtime/lib/x86_64-linux-gnu");
    } else if (project->config().targetArch() == "arm64") {
        r->process->env.push_back("PKG_CONFIG_PATH=/runtime/lib/aarch64-linux-gnu/pkgconfig");
        r->process->env.push_back("LIBRARY_PATH=/runtime/lib:/runtime/lib/aarch64-linux-gnu");
        r->process->env.push_back("LD_LIBRARY_PATH=/runtime/lib:/runtime/lib/aarch64-linux-gnu");
    }

    if (!r->hooks) {
        r->hooks.reset(new Hooks);
        QSharedPointer<Hook> hook(new Hook);
        hook->path = "/usr/sbin/ldconfig";
        r->hooks->prestart.push_back(hook);
    }

    if (startContainer(container, r)) {
        return NewError(-1, "build task failed in container");
    }

    err = commitBuildOutput(project, r->annotations->overlayfs);
    if (err) {
        return WrapError(err, "commitBuildOutput failed");
    }
    return Success();
}

linglong::util::Error LinglongBuilder::exportBundle(const QString &outputFilePath,
                                                    bool /*useLocalDir*/)
{
    // checkout data from local ostree
    auto projectConfigPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "linglong.yaml" }.join("/");
    if (!QFileInfo::exists(projectConfigPath)) {
        qCritical() << "ll-builder should run in the root directory of the linglong project";
        return NewError(-1, "linglong.yaml not found");
    }

    auto project = QVariant::fromValue(YAML::LoadFile(projectConfigPath.toStdString()))
                           .value<QSharedPointer<Project>>();
    project->setConfigFilePath(projectConfigPath);

    const auto exportPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), project->package->id }.join(
                    "/");

    util::ensureDir(exportPath);

    repo::OSTreeRepo repo(BuilderConfig::instance()->repoPath());
    auto err = repo.checkout(project->refWithModule("runtime"), "", exportPath);
    err = repo.checkout(project->refWithModule("devel"),
                        "",
                        QStringList{ exportPath, "devel" }.join("/"));
    if (err) {
        return WrapError(err, "checkout files failed, you need build first");
    }

    QFile::copy("/usr/libexec/ll-box-static", QStringList{ exportPath, "ll-box" }.join("/"));

    auto loaderFilePath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "loader" }.join("/");
    if (util::fileExists(loaderFilePath)) {
        QFile::copy(loaderFilePath, QStringList{ exportPath, "loader" }.join("/"));
    } else {
        qCritical() << "ll-builder need loader to build a runnable uab";
    }

    package::Ref baseRef("");

    if (!project->base) {
        auto info = std::get<0>(util::fromJSON<QSharedPointer<package::Info>>(
                project->config().cacheRuntimePath("info.json")));

        package::Ref runtimeBaseRef(info->base);

        baseRef.appId = runtimeBaseRef.appId;
        baseRef.version = runtimeBaseRef.version;
    }

    const auto extraFileListPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "extra_files.txt" }.join("/");
    if (util::fileExists(extraFileListPath)) {
        auto copyExtraFile = [baseRef, exportPath, &project](const QString &path) -> util::Error {
            QString containerFilePath;
            QString filePath;
            QString exportFilePath;

            if (path.startsWith("/runtime")) {
                filePath = project->config().cacheRuntimePath(
                        QStringList{ "files", path.right(path.length() - sizeof("/runtime")) }.join(
                                "/"));

                exportFilePath = QStringList{ exportPath, "lib", path }.join("/");
            } else {
                filePath = BuilderConfig::instance()->layerPath(
                        { baseRef.toLocalRefString(), "files", path });
                exportFilePath = QStringList{ exportPath, "lib", path }.join("/");
            }

            if (!util::fileExists(filePath) && !util::dirExists(filePath)) {
                return NewError(-1, QString("file %1 not exist").arg(filePath));
            }

            // ensure parent path
            util::ensureDir(exportFilePath);
            util::removeDir(exportFilePath);

            QProcess p;
            p.setProgram("cp");
            p.setArguments({ "-Pr", filePath, exportFilePath }); // use cp -Pr to keep symlink
            if (!p.startDetached()) {
                return NewError(-1, "failed to start cp");
            }
            p.waitForStarted(1000);
            p.waitForFinished(1000 * 60 * 60); // one hour
            if (p.exitCode() != 0) {
                return NewError(
                        -1,
                        QString("failed to copy %1 to  %2 ").arg(filePath).arg(exportFilePath));
            }
            return Success();
        };

        QFile extraFileList(extraFileListPath);
        if (!extraFileList.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return NewError(-1, "cannot open extra_files.txt");
        }
        QTextStream textStream(&extraFileList);
        while (true) {
            QString line = textStream.readLine();
            if (line.isNull()) {
                break;

            } else {
                auto err = copyExtraFile(line);
                if (err) {
                    return err;
                }
            }
        }

        if (!extraFileList.copy(QStringList{ exportPath, "lib", "extra_files.txt" }.join("/"))) {
            return NewError(-1, "cannot copy extra_files.txt");
        }
    }

    // TODO: if the kind is not app, don't make bundle
    // make bundle package
    linglong::package::Bundle uabBundle;

    err = uabBundle.make(exportPath, outputFilePath);
    if (err) {
        return WrapError(err, "make bundle failed");
    }

    return Success();
}

util::Error LinglongBuilder::push(const QString &repoUrl,
                                  const QString &repoName,
                                  const QString &channel,
                                  bool pushWithDevel)
{
    auto ret = Success();
    auto projectConfigPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "linglong.yaml" }.join("/");

    if (!QFileInfo::exists(projectConfigPath)) {
        qCritical() << "ll-builder should run in the root directory of the linglong project";
        return NewError(-1, "linglong.yaml not found");
    }

    try {
        auto project = QVariant::fromValue(YAML::LoadFile(projectConfigPath.toStdString()))
                               .value<QSharedPointer<Project>>();

        // set remote repo url
        auto remoteRepoEndpoint =
                repoUrl.isEmpty() ? BuilderConfig::instance()->remoteRepoEndpoint : repoUrl;
        auto remoteRepoName =
                repoName.isEmpty() ? BuilderConfig::instance()->remoteRepoName : repoName;
        repo::OSTreeRepo repo(BuilderConfig::instance()->repoPath(),
                              remoteRepoEndpoint,
                              remoteRepoName);

        // Fixme: should be buildArch.
        auto refWithRuntime = package::Ref("",
                                           channel,
                                           project->package->id,
                                           project->package->version,
                                           util::hostArch(),
                                           "runtime");
        auto refWithDevel = package::Ref("",
                                         channel,
                                         project->package->id,
                                         project->package->version,
                                         util::hostArch(),
                                         "devel");

        // push ostree data by ref
        // ret = repo.push(refWithRuntime, false);
        auto err = repo.push(refWithRuntime);

        if (err) {
            qInfo().noquote() << QString("push %1 failed").arg(project->package->id);
            return err;
        } else {
            qInfo().noquote() << QString("push %1 success").arg(project->package->id);
        }

        if (pushWithDevel) {
            err = repo.push(package::Ref(refWithDevel));

            if (err) {
                qInfo().noquote() << QString("push %1 failed").arg(project->package->id);
            } else {
                qInfo().noquote() << QString("push %1 success").arg(project->package->id);
            }
        }
    } catch (std::exception &e) {
        qCritical() << e.what();
        return NewError(-1, "failed to parse linglong.yaml");
    }

    return ret;
}

util::Error LinglongBuilder::import()
{
    auto err = initRepo();
    if (err) {
        return WrapError(err, "load local repo failed");
    }

    auto projectConfigPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "linglong.yaml" }.join("/");

    if (!QFileInfo::exists(projectConfigPath)) {
        qCritical() << "ll-builder should run in the root directory of the linglong project";
        return NewError(-1, "linglong.yaml not found");
    }

    try {
        auto project = QVariant::fromValue(YAML::LoadFile(projectConfigPath.toStdString()))
                               .value<QSharedPointer<Project>>();

        repo::OSTreeRepo repo(BuilderConfig::instance()->repoPath());

        auto refWithRuntime = package::Ref("",
                                           project->package->id,
                                           project->package->version,
                                           util::hostArch(),
                                           "runtime");

        err = repo.importDirectory(refWithRuntime, BuilderConfig::instance()->getProjectRoot());

        if (err) {
            return NewError(-1, "import package failed");
        }

        qInfo().noquote()
                << QString("import %1 success").arg(refWithRuntime.toOSTreeRefLocalString());
    } catch (std::exception &e) {
        qCritical() << e.what();
        return NewError(-1, "failed to parse linglong.yaml");
    }

    return Success();
}

linglong::util::Error LinglongBuilder::run()
{
    repo::OSTreeRepo repo(BuilderConfig::instance()->repoPath(),
                          BuilderConfig::instance()->remoteRepoEndpoint,
                          BuilderConfig::instance()->remoteRepoName);

    auto projectConfigPath =
            QStringList{ BuilderConfig::instance()->getProjectRoot(), "linglong.yaml" }.join("/");

    if (!QFileInfo::exists(projectConfigPath)) {
        qCritical() << "ll-builder should run in the root directory of the linglong project";
        return NewError(-1, "linglong.yaml not found");
    }

    try {
        auto project = QVariant::fromValue(YAML::LoadFile(projectConfigPath.toStdString()))
                               .value<QSharedPointer<Project>>();
        project->setConfigFilePath(projectConfigPath);

        // checkout app
        auto targetPath =
                BuilderConfig::instance()->layerPath({ project->ref().toLocalRefString() });
        linglong::util::ensureDir(targetPath);
        auto err = repo.checkoutAll(project->ref(), "", targetPath);
        if (err) {
            return NewError(-1, "checkout app files failed");
        }

        // checkout runtime
        targetPath =
                BuilderConfig::instance()->layerPath({ project->runtimeRef().toLocalRefString() });
        linglong::util::ensureDir(targetPath);

        auto remoteRuntimeRef = package::Ref(BuilderConfig::instance()->remoteRepoName,
                                             "linglong",
                                             project->runtimeRef().appId,
                                             project->runtimeRef().version,
                                             project->runtimeRef().arch,
                                             "");

        auto latestRuntimeRef = repo.remoteLatestRef(remoteRuntimeRef);
        err = repo.checkoutAll(latestRuntimeRef, "", targetPath);
        if (err) {
            return NewError(-1, "checkout runtime files failed");
        }

        // 获取环境变量
        QStringList userEnvList = COMMAND_HELPER->getUserEnv(linglong::util::envList);

        auto app = runtime::App::load(&repo, project->ref(), BuilderConfig::instance()->getExec());
        if (nullptr == app) {
            return NewError(-1, "load App::load failed");
        }

        app->saveUserEnvList(userEnvList);
        app->start();
    } catch (std::exception &e) {
        qCritical() << e.what();
        return NewError(-1, "failed to parse linglong.yaml");
    }

    return Success();
}

} // namespace builder
} // namespace linglong
