// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>

#include <QString>

#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"

struct SDCardSyncUploadFile
{
    QString RelativePath;
    QString LocalPath;
    int64_t Size = 0;
    int64_t ModificationTimeNanos = 0;
    SDCardSyncModel::FileInfo UploadedInfo;
    bool AssumeIdenticalToLocal = true;
};

struct SDCardSyncUploadDirectory
{
    QString RelativePath;
    SDCardSyncModel::FileInfo UploadedInfo;
    bool AssumeIdenticalToLocal = true;
};
