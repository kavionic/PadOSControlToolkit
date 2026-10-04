// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QString>

struct DeviceSessionOptions
{
    QString FilesystemName = "PadOS";
    QString FilesystemPrefix = "\\PadOS\\sdcard";
    QString TemporaryDirectoryName = "PadOS";
};
