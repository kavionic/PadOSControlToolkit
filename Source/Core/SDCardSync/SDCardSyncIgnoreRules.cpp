// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncIgnoreRules.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncIgnoreRules::Clear()
{
    m_Rules.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncIgnoreRules::RuleList SDCardSyncIgnoreRules::BuildRootRules(const QStringList& rootIgnoreFileNames)
{
    RuleList result;
    for (const QString& ignoreFileName : rootIgnoreFileNames)
    {
        const std::optional<Rule> syncIgnoreRule = ParseRule(ignoreFileName, "");
        if (syncIgnoreRule.has_value())
        {
            result.push_back(*syncIgnoreRule);
            m_Rules.push_back(*syncIgnoreRule);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncIgnoreRules::RuleList SDCardSyncIgnoreRules::LoadDirectoryRules(const QString& absolutePath, const QString& relativePath, const RuleList& parentRules, const QString& ignoreFileName)
{
    RuleList result = parentRules;
    QFile ignoreFile(QDir(absolutePath).absoluteFilePath(ignoreFileName));
    if (ignoreFile.open(QFile::ReadOnly | QFile::Text))
    {
        QTextStream stream(&ignoreFile);
        while (!stream.atEnd())
        {
            const std::optional<Rule> rule = ParseRule(stream.readLine(), relativePath);
            if (rule.has_value())
            {
                result.push_back(*rule);
                m_Rules.push_back(*rule);
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncIgnoreRules::IsIgnored(const QString& relativePath, bool isDirectory) const
{
    return IsIgnored(relativePath, isDirectory, m_Rules);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncIgnoreRules::IsIgnored(const QString& relativePath, bool isDirectory, const RuleList& rules) const
{
    bool result = false;
    for (const Rule& rule : rules)
    {
        const bool exactMatch = rule.Expression.match(relativePath).hasMatch();
        const bool descendantMatch = rule.DirectoryOnly && rule.DescendantExpression.match(relativePath).hasMatch();
        if ((exactMatch && (!rule.DirectoryOnly || isDirectory)) || descendantMatch)
        {
            result = !rule.IsNegation;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

const SDCardSyncIgnoreRules::RuleList& SDCardSyncIgnoreRules::GetRules() const
{
    return m_Rules;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncIgnoreRules::EscapeWildcardCharacters(const QString& text)
{
    QString result;
    result.reserve(text.size());
    for (qsizetype index = 0; index < text.size(); ++index)
    {
        const QChar character = text[index];
        if (character == '*')
        {
            result += "[*]";
        }
        else if (character == '?')
        {
            result += "[?]";
        }
        else if (character == '[')
        {
            result += "[[]";
        }
        else if (character == ']')
        {
            result += "[]]";
        }
        else
        {
            result += character;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncIgnoreRules::EscapePattern(const QString& pattern)
{
    QString result = EscapeWildcardCharacters(pattern);
    if (result.startsWith('#') || result.startsWith('!'))
    {
        result.prepend('\\');
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncIgnoreRules::JoinPatterns(const QStringList& patterns)
{
    return patterns.join('\n');
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList SDCardSyncIgnoreRules::SplitPatterns(const QString& text)
{
    QStringList result;
    const QStringList lines = text.split('\n');
    for (QString line : lines)
    {
        line.remove('\r');
        line = line.trimmed();
        if (!line.isEmpty())
        {
            result.push_back(line);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<SDCardSyncIgnoreRules::Rule> SDCardSyncIgnoreRules::ParseRule(const QString& line, const QString& basePath) const
{
    QString pattern = line.trimmed();
    if (pattern.isEmpty() || pattern.startsWith('#'))
    {
        return {};
    }

    bool escapedFirstCharacter = false;
    if (pattern.startsWith("\\#") || pattern.startsWith("\\!"))
    {
        pattern.remove(0, 1);
        escapedFirstCharacter = true;
    }

    Rule rule;
    rule.BasePath = basePath;
    if (!escapedFirstCharacter && pattern.startsWith('!'))
    {
        rule.IsNegation = true;
        pattern.remove(0, 1);
        pattern = pattern.trimmed();
    }

    pattern.replace('\\', '/');
    while (pattern.endsWith('/'))
    {
        rule.DirectoryOnly = true;
        pattern.chop(1);
    }
    while (pattern.startsWith('/'))
    {
        pattern.remove(0, 1);
    }
    if (pattern.isEmpty())
    {
        return {};
    }

    rule.Expression = QRegularExpression(BuildRegularExpression(basePath, pattern, false));
    if (rule.DirectoryOnly)
    {
        rule.DescendantExpression = QRegularExpression(BuildRegularExpression(basePath, pattern, true));
    }
    return rule;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncIgnoreRules::BuildRegularExpression(const QString& basePath, const QString& pattern, bool allowDescendants)
{
    QString expression = "^";
    QString normalizedBasePath = basePath;
    normalizedBasePath.replace('\\', '/');

    if (pattern.contains('/'))
    {
        if (!normalizedBasePath.isEmpty())
        {
            expression += QRegularExpression::escape(normalizedBasePath) + "/";
        }
        expression += WildcardToRegularExpression(pattern);
    }
    else
    {
        if (!normalizedBasePath.isEmpty())
        {
            expression += QRegularExpression::escape(normalizedBasePath) + "/(?:.*/)?";
        }
        else
        {
            expression += "(?:.*/)?";
        }
        expression += WildcardToRegularExpression(pattern);
    }

    if (allowDescendants)
    {
        expression += "/.*";
    }
    expression += "$";
    return expression;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSyncIgnoreRules::WildcardToRegularExpression(const QString& pattern)
{
    QString result;
    for (qsizetype index = 0; index < pattern.size(); ++index)
    {
        const QChar character = pattern[index];
        if (character == '*')
        {
            if (index + 1 < pattern.size() && pattern[index + 1] == '*')
            {
                result += ".*";
                ++index;
            }
            else
            {
                result += "[^/]*";
            }
        }
        else if (character == '?')
        {
            result += "[^/]";
        }
        else if (character == '[')
        {
            const qsizetype closingBracket = pattern.indexOf(']', index + 1);
            if (closingBracket > index)
            {
                result += pattern.mid(index, closingBracket - index + 1);
                index = closingBracket;
            }
            else
            {
                result += "\\[";
            }
        }
        else
        {
            result += QRegularExpression::escape(QString(1, character));
        }
    }
    return result;
}
