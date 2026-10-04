// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncDeleteRules.h"

#include <QDir>
#include <QSettings>
#include <QStringList>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncDeleteRules::Clear()
{
    m_Rules.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncDeleteRules::Load(const QString& filePath)
{
    Clear();
    if (!filePath.isEmpty())
    {
        QSettings settings(filePath, QSettings::IniFormat);
        const int ruleCount = settings.beginReadArray("Rules");
        for (int ruleIndex = 0; ruleIndex < ruleCount; ++ruleIndex)
        {
            settings.setArrayIndex(ruleIndex);
            const QString relativePath = NormalizeRelativePath(settings.value("Path", "").toString());
            const std::optional<Mode> mode = ParseMode(settings.value("Mode", "").toString());
            if (!relativePath.isEmpty() && mode.has_value() && *mode != Mode::Inherit)
            {
                m_Rules[relativePath] = *mode;
            }
        }
        settings.endArray();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncDeleteRules::Save(const QString& filePath) const
{
    bool result = false;
    if (!filePath.isEmpty())
    {
        QSettings settings(filePath, QSettings::IniFormat);
        settings.clear();
        settings.beginWriteArray("Rules", static_cast<int>(m_Rules.size()));
        int ruleIndex = 0;
        for (const auto& ruleEntry : m_Rules)
        {
            settings.setArrayIndex(ruleIndex);
            settings.setValue("Path", ruleEntry.first);
            settings.setValue("Mode", GetModeText(ruleEntry.second));
            ++ruleIndex;
        }
        settings.endArray();
        settings.sync();
        result = settings.status() == QSettings::NoError;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncDeleteRules::SetRule(const QString& relativePath, Mode mode)
{
    const QString normalizedPath = NormalizeRelativePath(relativePath);
    if (!normalizedPath.isEmpty())
    {
        if (mode == Mode::Inherit)
        {
            m_Rules.erase(normalizedPath);
        }
        else
        {
            m_Rules[normalizedPath] = mode;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncDeleteRules::Mode SDCardSyncDeleteRules::GetRule(const QString& relativePath) const
{
    Mode result = Mode::Inherit;
    const auto ruleIterator = m_Rules.find(NormalizeRelativePath(relativePath));
    if (ruleIterator != m_Rules.end())
    {
        result = ruleIterator->second;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncDeleteRules::IsAllowed(const QString& relativePath, bool isDirectory) const
{
    return IsAllowedWithParts(NormalizeRelativePath(relativePath).split('/', Qt::SkipEmptyParts), isDirectory);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncDeleteRules::IsInheritedAllowed(const QString& relativePath, bool isDirectory) const
{
    const QString parentPath = GetParentRelativePath(NormalizeRelativePath(relativePath));
    return IsAllowedWithParts(parentPath.split('/', Qt::SkipEmptyParts), isDirectory);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncDeleteRules::GetModeText(Mode mode)
{
    QString result;
    switch (mode)
    {
        case Mode::Inherit:
            result = "inherit";
            break;
        case Mode::Disabled:
            result = "disabled";
            break;
        case Mode::AllowThis:
            result = "allow-this";
            break;
        case Mode::AllowRecursive:
            result = "allow-recursive";
            break;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<SDCardSyncDeleteRules::Mode> SDCardSyncDeleteRules::ParseMode(const QString& text)
{
    std::optional<Mode> result;
    const QString normalizedText = text.trimmed().toLower();
    if (normalizedText == "inherit")
    {
        result = Mode::Inherit;
    }
    else if (normalizedText == "disabled")
    {
        result = Mode::Disabled;
    }
    else if (normalizedText == "allow-this")
    {
        result = Mode::AllowThis;
    }
    else if (normalizedText == "allow-recursive")
    {
        result = Mode::AllowRecursive;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncDeleteRules::NormalizeRelativePath(const QString& relativePath)
{
    QString result = QDir::fromNativeSeparators(relativePath.trimmed());
    while (result.startsWith('/'))
    {
        result.remove(0, 1);
    }
    while (result.endsWith('/'))
    {
        result.chop(1);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncDeleteRules::GetParentRelativePath(const QString& relativePath)
{
    const qsizetype separatorIndex = relativePath.lastIndexOf('/');
    return (separatorIndex >= 0) ? relativePath.left(separatorIndex) : QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncDeleteRules::IsAllowedWithParts(const QStringList& pathParts, bool isDirectory) const
{
    Mode inheritedMode = Mode::Disabled;
    QString currentPath;
    for (int pathIndex = 0; pathIndex < pathParts.size(); ++pathIndex)
    {
        currentPath = currentPath.isEmpty() ? pathParts[pathIndex] : currentPath + "/" + pathParts[pathIndex];
        const bool isTarget = pathIndex == pathParts.size() - 1;
        const bool currentIsDirectory = !isTarget || isDirectory;
        const Mode currentRule = GetRule(currentPath);
        if (currentRule != Mode::Inherit)
        {
            inheritedMode = currentRule;
        }
        else if (inheritedMode == Mode::AllowThis && currentIsDirectory)
        {
            inheritedMode = Mode::Disabled;
        }
    }
    return IsModeAllowed(inheritedMode, isDirectory);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncDeleteRules::IsModeAllowed(Mode mode, bool isDirectory)
{
    bool result = false;
    switch (mode)
    {
        case Mode::Inherit:
        case Mode::Disabled:
            result = false;
            break;
        case Mode::AllowThis:
            result = !isDirectory;
            break;
        case Mode::AllowRecursive:
            result = true;
            break;
    }
    return result;
}
