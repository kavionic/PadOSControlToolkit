// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <optional>
#include <vector>

#include <QRegularExpression>
#include <QString>
#include <QStringList>

class SDCardSyncIgnoreRules
{
public:
    struct Rule
    {
        QString BasePath;
        QRegularExpression Expression;
        QRegularExpression DescendantExpression;
        bool IsNegation = false;
        bool DirectoryOnly = false;
    };

    using RuleList = std::vector<Rule>;

    void Clear();
    RuleList BuildRootRules(const QStringList& rootIgnoreFileNames);
    RuleList LoadDirectoryRules(const QString& absolutePath, const QString& relativePath, const RuleList& parentRules, const QString& ignoreFileName);
    bool IsIgnored(const QString& relativePath, bool isDirectory) const;
    bool IsIgnored(const QString& relativePath, bool isDirectory, const RuleList& rules) const;
    const RuleList& GetRules() const;

    static QString EscapeWildcardCharacters(const QString& text);
    static QString EscapePattern(const QString& pattern);
    static QString JoinPatterns(const QStringList& patterns);
    static QStringList SplitPatterns(const QString& text);

private:
    std::optional<Rule> ParseRule(const QString& line, const QString& basePath) const;
    static QString BuildRegularExpression(const QString& basePath, const QString& pattern, bool allowDescendants);
    static QString WildcardToRegularExpression(const QString& pattern);

    RuleList m_Rules;
};
