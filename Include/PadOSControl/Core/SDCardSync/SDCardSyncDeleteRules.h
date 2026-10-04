// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <map>
#include <optional>

#include <QString>
#include <QStringList>

class SDCardSyncDeleteRules
{
public:
    enum class Mode : int
    {
        Inherit,
        Disabled,
        AllowThis,
        AllowRecursive
    };

    void Clear();
    void Load(const QString& filePath);
    bool Save(const QString& filePath) const;

    void SetRule(const QString& relativePath, Mode mode);
    Mode GetRule(const QString& relativePath) const;

    bool IsAllowed(const QString& relativePath, bool isDirectory) const;
    bool IsInheritedAllowed(const QString& relativePath, bool isDirectory) const;

    static QString GetModeText(Mode mode);
    static std::optional<Mode> ParseMode(const QString& text);

private:
    static QString NormalizeRelativePath(const QString& relativePath);
    static QString GetParentRelativePath(const QString& relativePath);
    bool IsAllowedWithParts(const QStringList& pathParts, bool isDirectory) const;
    static bool IsModeAllowed(Mode mode, bool isDirectory);

    std::map<QString, Mode> m_Rules;
};
