// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>

#include <QByteArray>
#include <QString>

#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"

class QFileInfo;

class SDCardSyncFileUtils
{
public:
    static constexpr int64_t NanosecondsPerMillisecond = 1000000LL;
    static constexpr int64_t NanosecondsPerSecond = 1000LL * NanosecondsPerMillisecond;
    static constexpr int MaxFilesystemPathBytes = 1024;

    static SDCardSyncModel::FileInfo BuildLocalFileInfo(const QFileInfo& fileInfo);
    static bool FileEndsWithLineBreak(const QString& path);
    static QString JoinRelativePath(const QString& parentPath, const QString& childName);
    static QString NormalizeRemotePath(const QString& path);
    static QString FormatByteCount(int64_t byteCount);
    static QString FormatFileTime(int64_t modificationTimeNanos);
    static int64_t RoundToFATModificationTime(int64_t modificationTimeNanos);
    static bool FileTimesMatch(int64_t localTimeNanos, int64_t remoteTimeNanos);
    static void OpenExplorerPath(const QString& path, bool isDirectory);
    static QByteArray CalculateFileHash(const QString& path);
};
