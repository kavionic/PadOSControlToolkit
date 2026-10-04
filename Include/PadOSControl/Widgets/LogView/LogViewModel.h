// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <QAbstractTableModel>
#include <QColor>

struct LogEntry
{
    int64_t   TimestampNS;
    uint32_t  CategoryHash;
    uint8_t   Severity;
    QString   Message;
    mutable QString TimestampTextCache;
};

struct LogCategoryInfo
{
    QString CategoryName;
    QString DisplayName;
};

class LogViewModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum class Column : int
    {
        Timestamp = 0,
        Category,
        Severity,
        Message,
        Count
    };

    // Custom data role to expose raw severity ID to the delegate
    static constexpr int SeverityIdRole = Qt::UserRole;

    explicit LogViewModel(QObject* parent = nullptr);

    // Message ingestion
    bool AddEntry(LogEntry entry);
    size_t PrependEntries(std::vector<LogEntry> entries);
    void Clear();

    // Metadata updates (called when device sends new category/severity lists)
    void UpdateCategories(const std::map<uint32_t, LogCategoryInfo>& categoryMap);
    void UpdateSeverities(const std::map<uint8_t, QString>& severityMap);

    // Filter control
    void SetCategoryVisible(uint32_t categoryHash, bool visible);
    void SetAllCategoriesVisible(bool visible);
    void SetVisibleCategorySet(std::set<uint32_t> visibleHashes);
    void SetCategoryMinSeverity(uint32_t categoryHash, uint8_t minSeverity);
    void SetCategoriesMinSeverity(const std::vector<uint32_t>& categoryHashes, uint8_t minSeverity);
    void MergeCategorySeverities(const std::map<uint32_t, uint8_t>& severities);

    // Search filter
    void SetSearchText(const QString& text);
    void SetSearchFilterEnabled(bool enabled);
    void SetSearchMetadataEnabled(bool enabled);

    uint8_t GetCategoryMinSeverity(uint32_t categoryHash) const;

    const std::map<uint32_t, LogCategoryInfo>& GetKnownCategories() const { return m_CategoryInfoMap; }
    const std::map<uint8_t, QString>&          GetKnownSeverities() const { return m_SeverityNames; }
    std::optional<int64_t>                     GetOldestEntryTimestamp() const;
    std::optional<int64_t>                     GetNewestEntryTimestamp() const;

    QColor GetSeverityColor(uint8_t severityId) const;

    // QAbstractTableModel interface
    virtual int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    virtual int      columnCount(const QModelIndex& parent = QModelIndex()) const override;
    virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    virtual QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    bool    PassesFilter(const LogEntry& entry) const;
    void    Refilter();
    void    AssignDefaultColorIfNeeded(uint8_t severityId);
    QString GetColumnText(const LogEntry& entry, Column column) const;
    static QString FormatTimestamp(int64_t timestampNS);

    std::deque<LogEntry>        m_AllEntries;
    std::deque<size_t>          m_FilteredIndices;

    std::map<uint32_t, LogCategoryInfo> m_CategoryInfoMap;
    std::map<uint8_t, QString>          m_SeverityNames;
    std::map<uint8_t, QColor>           m_SeverityColors;

    // Filter state
    // nullopt = show all categories; empty set = show none
    std::optional<std::set<uint32_t>>   m_VisibleCategories;
    std::map<uint32_t, uint8_t>         m_CategoryMinSeverity;

    // Search filter
    QString  m_SearchText;
    bool     m_SearchFilterEnabled   = false;
    bool     m_SearchMetadataEnabled = false;

    static const QColor s_DefaultColorPalette[];
    static constexpr int s_DefaultColorPaletteSize = 9;
};
