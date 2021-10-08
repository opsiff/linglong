/*
 * Copyright (c) 2020-2021. Uniontech Software Ltd. All rights reserved.
 *
 * Author:     huqinghong@uniontech.com
 *
 * Maintainer: huqinghong@uniontech.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <curl/curl.h>
#include <QString>
namespace linglong {
namespace util {

/*
 * http下载进度回调函数
 *
 * @param client: 客户端传递给libcurl的数据
 * @param dltotal: 需要下载的总字节数
 * @param dlnow: 已经下载的字节数
 * @param ultotal: 将要上传的字节数
 * @param ulnow: 已经上传的字节数
 *
 * @return int: CURLE_OK(0)
 */
typedef int (*DOWNLOADCALLBACK)(void *client, double dltotal, double dlnow, double ultotal, double ulnow);

typedef struct _downloadRet {
    long resCode;
    long fileSize;
    long contentSize;
    std::string contentType;
    double spendTime;
    double downloadSpeed;

} DownloadRet;

class HttpClient
{
private:
    HttpClient(): mIsFinish(false), mProgressFun(nullptr), mCurlHandle(nullptr), mData({0})
    {
    }

    /*
     * 获取文件锁
     *
     * @return int: 文件描述符
     */
    int getlock();

    /*
     * 释放文件锁
     *
     * @param fd: 文件描述符
     *
     * @return int: 0:成功 其它:失败
     */
    int releaselock(int fd);

    /*
     * 设置Http传输参数
     *
     * @param url: url地址
     */
    void initHttpParam(const char *url);

    /*
     * 获取目标文件保存的全路径
     *
     * @param url: url地址
     * @param savePath: 文件存储路径
     * @param fullPath: 文件保存全路径
     * @param maxSize: 路径长度阈值
     */
    void getFullPath(const char *url, const char *savePath, char *fullPath, int maxSize);

    /*
     * 校验Http传输参数
     *
     * @param url: url地址
     * @param savePath: 文件保存地址
     *
     * @return bool: true:成功 false:失败
     */
    bool checkPara(const char *url, const char *savePath);

    /*
     * 显示结果信息
     */
    void showInfo();

    static HttpClient *sInstance;
    bool mIsFinish;
    DOWNLOADCALLBACK mProgressFun;
    CURL *mCurlHandle;
    DownloadRet mData;

public:

    /*
     * 获取HttpClient实例
     */
    static HttpClient *getInstance();

    /*
     * 下载文件
     *
     * @param qurl: 目标文件url
     * @param qsavePath: 保存路径
     *
     * @return bool: true:成功 false:失败
     */
    bool loadHttpData(const QString qurl, const QString qsavePath);

    /*
     * 设置下载进度回调
     *
     * @param progressFun: 回调函数
     *
     */
    void setProgressCallback(DOWNLOADCALLBACK progressFun);

    /*
     * 查询下载文件结果信息
     *
     * @param dataInfo: 结果信息结构体
     *
     * @return bool: true:成功 false:失败
     */
    bool getDownloadInfo(DownloadRet &dataInfo)
    {
        return true;
    }

    /*
     * 释放HttpClient实例
     */
    static void release();
};
} // namespace util
} // namespace linglong