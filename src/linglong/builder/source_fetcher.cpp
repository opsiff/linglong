/*
 * SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "source_fetcher.h"

#include "linglong/builder/file.h"
#include "linglong/utils/command/env.h"
#include "linglong/utils/error/error.h"
#include "linglong/utils/global/initialize.h"

#include <qeventloop.h>
#include <qnetworkaccessmanager.h>
#include <qnetworkreply.h>

#include <QCryptographicHash>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <utility>

namespace linglong::builder {
namespace {
static constexpr auto CompressedFileTarXz = "tar.xz";
static constexpr auto CompressedFileTarGz = "tar.gz";
static constexpr auto CompressedFileTarBz2 = "tar.bz2";
static constexpr auto CompressedFileTgz = "tgz";
static constexpr auto CompressedFileTar = "tar";
static constexpr auto CompressedFileZip = "zip";

auto fixSuffix(const QFileInfo &fi) -> QString
{
    for (const char *suffix : { CompressedFileTarXz,
                                CompressedFileTarBz2,
                                CompressedFileTarGz,
                                CompressedFileTgz,
                                CompressedFileTar }) {
        if (fi.completeSuffix().endsWith(suffix)) {
            return CompressedFileTar;
        }
    }
    return fi.suffix();
}

auto extractFile(const QString &path, const QDir &dir) -> utils::error::Result<void>
{
    LINGLONG_TRACE("extract file");

    auto tarxz = [](const QString &path, const QDir &dir) -> utils::error::Result<QString> {
        return utils::command::Exec("tar", { "-C", dir.absolutePath(), "-xvf", path });
    };
    auto targz = [](const QString &path, const QDir &dir) -> utils::error::Result<QString> {
        return utils::command::Exec("tar", { "-C", dir.absolutePath(), "-zxvf", path });
    };
    auto tarbz2 = [](const QString &path, const QDir &dir) -> utils::error::Result<QString> {
        return utils::command::Exec("tar", { "-C", dir.absolutePath(), "-jxvf", path });
    };
    auto zip = [](const QString &path, const QDir &dir) -> utils::error::Result<QString> {
        return utils::command::Exec("unzip", { "-d", dir.absolutePath(), path });
    };

    QFileInfo fi(path);

    QMap<QString,
         std::function<utils::error::Result<QString>(const QString &path, const QDir &dir)>>
      subcommandMap = {
          { CompressedFileTarXz, tarxz },   { CompressedFileTarGz, targz },
          { CompressedFileTarBz2, tarbz2 }, { CompressedFileZip, zip },
          { CompressedFileTgz, targz },     { CompressedFileTar, tarxz },
      };

    auto suffix = fixSuffix(fi);

    if (!subcommandMap.contains(suffix)) {
        return LINGLONG_ERR("unsupported suffix " + path + " " + fi.completeSuffix() + " "
                            + fi.suffix() + " " + fi.bundleName());
    }

    auto subcommand = subcommandMap[suffix];
    auto output = subcommand(path, dir);
    if (!output) {
        return LINGLONG_ERR(output);
    }

    return LINGLONG_OK;
}

auto needDownload(const api::types::v1::BuilderProjectSource &source, QFile &file) noexcept -> bool
{
    if (!file.exists()) {
        return true;
    }

    if (!source.digest) {
        return true;
    }

    auto digest = util::fileHash(file, QCryptographicHash::Sha256).toStdString();
    return digest != *source.digest;
}

auto fetchFile(const api::types::v1::BuilderProjectSource &source, QDir destination) noexcept
  -> utils::error::Result<QString>
{
    LINGLONG_TRACE("fetch file");

    if (!source.url) {
        return LINGLONG_ERR("url missing");
    }

    if (!source.digest) {
        return LINGLONG_ERR("digest missing");
    }

    QUrl url(source.url->c_str());

    auto path = destination.absoluteFilePath(url.fileName());

    QFile file = path;

    if (needDownload(source, file)) {
        file.remove();
    }

    if (!file.open(QIODevice::WriteOnly)) {
        return LINGLONG_ERR(file);
    }

    QNetworkRequest request;
    request.setUrl(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, "Wget/1.21.4");

    QNetworkAccessManager mgr;
    auto reply = mgr.get(request);

    QObject::connect(reply, &QNetworkReply::metaDataChanged, [reply]() {
        qDebug() << reply->header(QNetworkRequest::ContentLengthHeader);
    });

    QObject::connect(reply, &QNetworkReply::readyRead, [reply, &file]() {
        file.write(reply->readAll());
    });

    QEventLoop loop;
    QEventLoop::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    // 将缓存写入文件
    file.close();

    auto digest = util::fileHash(file, QCryptographicHash::Sha256).toStdString();
    if (digest != *source.digest) {
        return LINGLONG_ERR(
          QString::fromStdString("digest mismatched: " + digest + " vs " + *source.digest));
    }

    return QFileInfo(file).absolutePath();
}

auto fetchGitRepo(const api::types::v1::BuilderProjectSource &source, QDir destination)
  -> utils::error::Result<void>
{
    LINGLONG_TRACE("fetch git repo");

    if (!source.url) {
        return LINGLONG_ERR("URL is missing");
    }

    // 如果二进制安装在系统目录中，优先使用系统中安装的脚本文件（便于用户更改），否则使用二进制内嵌的脚本（便于开发调试）
    auto scriptFile = QString(LINGLONG_LIBEXEC_DIR) + "/fetch-git-repo.sh";
    auto useInstalledFile = utils::global::linglongInstalled() && QFile(scriptFile).exists();
    QScopedPointer<QTemporaryDir> dir;
    if (!useInstalledFile) {
        qWarning() << "Dumping fetch-git-repo from qrc...";
        dir.reset(new QTemporaryDir);
        // 便于在执行失败时进行调试
        dir->setAutoRemove(false);
        scriptFile = dir->filePath("fetch-git-repo");
        QFile::copy(":/scripts/fetch-git-repo", scriptFile);
    }

    auto output = utils::command::Exec(
      "sh",
      QStringList() << scriptFile
                    << destination.absoluteFilePath(QUrl(source.url->c_str()).fileName())
                    << QString::fromStdString(*source.url)
                    << QString::fromStdString(source.commit.value_or(""))
                    << QString::fromStdString(source.version.value_or("")));
    if (!output) {
        return LINGLONG_ERR(output);
    }

    if (!dir.isNull()) {
        dir->remove();
    }

    return LINGLONG_OK;
}

auto fetchDscRepo(const api::types::v1::BuilderProjectSource &source, QDir destination) noexcept
  -> utils::error::Result<void>
{
    LINGLONG_TRACE("fetch dsc");

    if (!source.url) {
        return LINGLONG_ERR("URL is missing");
    }

    // 如果二进制安装在系统目录中，优先使用系统中安装的脚本文件（便于用户更改），否则使用二进制内嵌的脚本（便于开发调试）
    auto scriptFile = QString(LINGLONG_LIBEXEC_DIR) + "/fetch-dsc-repo";
    auto useInstalledFile = utils::global::linglongInstalled() && QFile(scriptFile).exists();
    QScopedPointer<QTemporaryDir> dir;
    if (!useInstalledFile) {
        qWarning() << "Dumping fetch-dsc-repo from qrc...";
        dir.reset(new QTemporaryDir);
        // 便于在执行失败时进行调试
        dir->setAutoRemove(false);
        scriptFile = dir->filePath("fetch-dsc-repo");
        QFile::copy(":/scripts/fetch-dsc-repo", scriptFile);
    }
    auto output = utils::command::Exec("sh",
                                       QStringList() << scriptFile << destination.absolutePath()
                                                     << QString::fromStdString(*source.url));
    if (!output) {
        return LINGLONG_ERR(output);
    }

    if (!dir.isNull()) {
        dir->remove();
    }
    return LINGLONG_OK;
}

} // namespace

auto SourceFetcher::fetch(QDir destination) noexcept -> utils::error::Result<void>
{
    LINGLONG_TRACE("fetch source");

    if (this->source.kind == "git") {
        auto ret = fetchGitRepo(this->source, destination);
        if (!ret) {
            return LINGLONG_ERR(ret);
        }
        return LINGLONG_OK;
    }

    if (this->source.kind == "dsc") {
        auto ret = fetchDscRepo(this->source, destination);
        if (!ret) {
            return LINGLONG_ERR(ret);
        }
        return LINGLONG_OK;
    }

    if (this->source.kind == "archive") {
        auto cacheDir = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                             + "/linglong/builder/archives");
        if (this->cfg.cache) {
            cacheDir.setPath(QString::fromStdString(*this->cfg.cache));
        }

        if (!cacheDir.mkpath(".")) {
            return LINGLONG_ERR("create " + cacheDir.absolutePath() + ": failed");
        }

        auto archivePath = fetchFile(this->source, cacheDir);
        if (!archivePath) {
            return LINGLONG_ERR(archivePath);
        }

        auto ret = extractFile(*archivePath, destination);
        if (!ret) {
            return LINGLONG_ERR(ret);
        }
        return LINGLONG_OK;
    }

    if (source.kind == "file") {
        auto path = fetchFile(this->source, destination);
        if (!path) {
            return LINGLONG_ERR(path);
        }
        return LINGLONG_OK;
    }

    return LINGLONG_ERR("unknown source kind");
}

SourceFetcher::SourceFetcher(api::types::v1::BuilderProjectSource s,
                             api::types::v1::BuilderConfig cfg)
    : source(std::move(s))
    , cfg(std::move(cfg))
{
}

} // namespace linglong::builder
