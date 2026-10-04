// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"

#include <cstdlib>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimeZone>

static constexpr int64_t FAT_TIME_TOLERANCE_NANOSECONDS = 2LL * SDCardSyncFileUtils::NanosecondsPerSecond;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncModel::FileInfo SDCardSyncFileUtils::BuildLocalFileInfo(const QFileInfo& fileInfo)
{
    SDCardSyncModel::FileInfo result;
    result.Name = fileInfo.fileName();
    result.IsDirectory = fileInfo.isDir();
    result.Size = fileInfo.isDir() ? 0 : fileInfo.size();
    result.ModificationTimeNanos = fileInfo.fileTime(QFileDevice::FileModificationTime).toUTC().toMSecsSinceEpoch() * SDCardSyncFileUtils::NanosecondsPerMillisecond;
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncFileUtils::FileEndsWithLineBreak(const QString& path)
{
    bool result = true;
    QFile file(path);
    if (file.open(QFile::ReadOnly) && file.size() > 0)
    {
        if (file.seek(file.size() - 1))
        {
            char lastCharacter = '\0';
            if (file.getChar(&lastCharacter))
            {
                result = lastCharacter == '\n' || lastCharacter == '\r';
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncFileUtils::JoinRelativePath(const QString& parentPath, const QString& childName)
{
    if (parentPath.isEmpty())
    {
        return childName;
    }
    return parentPath + "/" + childName;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncFileUtils::NormalizeRemotePath(const QString& path)
{
    QString result = path;
    result.replace('\\', '/');
    while (result.contains("//"))
    {
        result.replace("//", "/");
    }
    if (!result.startsWith('/'))
    {
        result.prepend('/');
    }
    if (result.size() > 1 && result.endsWith('/'))
    {
        result.chop(1);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncFileUtils::FormatByteCount(int64_t byteCount)
{
    if (byteCount < 1024)
    {
        return QString("%1 B").arg(byteCount);
    }
    if (byteCount < 1024 * 1024)
    {
        return QString("%1 KB").arg(double(byteCount) / 1024.0, 0, 'f', 1);
    }
    return QString("%1 MB").arg(double(byteCount) / (1024.0 * 1024.0), 0, 'f', 2);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncFileUtils::FormatFileTime(int64_t modificationTimeNanos)
{
    if (modificationTimeNanos <= 0)
    {
        return "";
    }
    return QDateTime::fromMSecsSinceEpoch(modificationTimeNanos / SDCardSyncFileUtils::NanosecondsPerMillisecond, QTimeZone(Qt::UTC)).toLocalTime().toString("yy/MM/dd HH:mm:ss");
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int64_t SDCardSyncFileUtils::RoundToFATModificationTime(int64_t modificationTimeNanos)
{
    int64_t result = modificationTimeNanos;
    if (modificationTimeNanos > 0)
    {
        const int64_t seconds = modificationTimeNanos / SDCardSyncFileUtils::NanosecondsPerSecond;
        result = (seconds - seconds % 2) * SDCardSyncFileUtils::NanosecondsPerSecond;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncFileUtils::FileTimesMatch(int64_t localTimeNanos, int64_t remoteTimeNanos)
{
    const int64_t directDelta = std::llabs(localTimeNanos - remoteTimeNanos);
    const int64_t roundedLocalTimeNanos = RoundToFATModificationTime(localTimeNanos);
    const int64_t roundedRemoteTimeNanos = RoundToFATModificationTime(remoteTimeNanos);
    return directDelta <= FAT_TIME_TOLERANCE_NANOSECONDS || roundedLocalTimeNanos == roundedRemoteTimeNanos;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncFileUtils::OpenExplorerPath(const QString& path, bool isDirectory)
{
    if (!path.isEmpty())
    {
        const QString nativePath = QDir::toNativeSeparators(path);
        if (isDirectory)
        {
            QProcess::startDetached("explorer.exe", QStringList({ nativePath }));
        }
        else
        {
            QProcess::startDetached("explorer.exe", QStringList({ QString("/select,%1").arg(nativePath) }));
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QByteArray SDCardSyncFileUtils::CalculateFileHash(const QString& path)
{
    QFile file(path);
    QByteArray result;
    if (file.open(QFile::ReadOnly))
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!file.atEnd())
        {
            const QByteArray data = file.read(1024 * 64);
            if (!data.isEmpty())
            {
                hash.addData(data);
            }
            else if (!file.atEnd())
            {
                return QByteArray();
            }
        }
        result = hash.result();
    }
    return result;
}
