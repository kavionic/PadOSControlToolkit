// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QString>

#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"

struct SDCardSyncDownloadFile
{
    QString RelativePath;
    QString LocalPath;
    SDCardSyncModel::FileInfo RemoteInfo;
    bool ApplyToModel = true;
};

struct SDCardSyncDownloadDirectory
{
    QString RelativePath;
    QString LocalPath;
    SDCardSyncModel::FileInfo RemoteInfo;
    bool ApplyToModel = true;
};
