// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/LogView/LogViewModel.h"


const QColor LogViewModel::s_DefaultColorPalette[] =
{
    QColor(0x00, 0x00, 0x00),   // Slot 0: NONE/default
    QColor(0xff, 0x20, 0x20),   // Slot 1: FATAL - bright red
    QColor(0xff, 0x64, 0x40),   // Slot 2: CRITICAL - bright red
    QColor(0xff, 0x20, 0x20),   // Slot 3: ERROR - orange-red
    QColor(0xb7, 0x92, 0x00),   // Slot 4: WARNING - gold
    QColor(0x00, 0x00, 0x00),   // Slot 5: NOTICE - light gray
    QColor(0x20, 0x20, 0x20),   // Slot 6: INFO_LOW_VOL - light gray
    QColor(0x40, 0x40, 0x40),   // Slot 7: INFO_HIGH_VOL - gray
    QColor(0x60, 0x60, 0x60),   // Slot 8: INFO_FLOODING - gray
};

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

LogViewModel::LogViewModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool LogViewModel::AddEntry(LogEntry entry)
{
    // Fast path: entry is newer than everything we have — plain append, no searching or shifting.
    if (m_AllEntries.empty() || entry.TimestampNS > m_AllEntries.back().TimestampNS)
    {
        const size_t newIndex = m_AllEntries.size();
        AssignDefaultColorIfNeeded(entry.Severity);
        m_AllEntries.push_back(std::move(entry));

        if (PassesFilter(m_AllEntries.back()))
        {
            const int newRow = static_cast<int>(m_FilteredIndices.size());
            beginInsertRows(QModelIndex(), newRow, newRow);
            m_FilteredIndices.push_back(newIndex);
            endInsertRows();
        }
        return true;
    }

    // Find sorted insert position by timestamp.
    auto insertPos = std::lower_bound(
        m_AllEntries.begin(), m_AllEntries.end(), entry.TimestampNS,
        [](const LogEntry& e, int64_t ts) { return e.TimestampNS < ts; });

    while (insertPos != m_AllEntries.end() && insertPos->TimestampNS == entry.TimestampNS)
    {
        if (insertPos->CategoryHash == entry.CategoryHash)
        {
            return false;
        }
        ++insertPos;
    }

    const size_t insertIndex = static_cast<size_t>(insertPos - m_AllEntries.begin());
    AssignDefaultColorIfNeeded(entry.Severity);
    m_AllEntries.insert(insertPos, std::move(entry));

    // Binary search for the first filtered index that needs shifting, then shift from there.
    const auto shiftStart = std::lower_bound(m_FilteredIndices.begin(), m_FilteredIndices.end(), insertIndex);
    for (auto it = shiftStart; it != m_FilteredIndices.end(); ++it) {
        ++(*it);
    }

    if (PassesFilter(m_AllEntries[insertIndex]))
    {
        const int newFilterRow = static_cast<int>(shiftStart - m_FilteredIndices.begin());
        beginInsertRows(QModelIndex(), newFilterRow, newFilterRow);
        m_FilteredIndices.insert(shiftStart, insertIndex);
        endInsertRows();
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t LogViewModel::PrependEntries(std::vector<LogEntry> entries)
{
    if (entries.empty()) { return 0; }

    const size_t insertCount = entries.size();

    for (size_t& index : m_FilteredIndices) {
        index += insertCount;
    }

    m_AllEntries.insert(m_AllEntries.begin(),
                        std::make_move_iterator(entries.begin()),
                        std::make_move_iterator(entries.end()));

    for (size_t i = 0; i < insertCount; ++i) {
        AssignDefaultColorIfNeeded(m_AllEntries[i].Severity);
    }

    std::vector<size_t> newFilteredIndices;
    for (size_t i = 0; i < insertCount; ++i)
    {
        if (PassesFilter(m_AllEntries[i])) {
            newFilteredIndices.push_back(i);
        }
    }

    const size_t newFilteredCount = newFilteredIndices.size();
    if (newFilteredCount > 0)
    {
        beginInsertRows(QModelIndex(), 0, static_cast<int>(newFilteredCount) - 1);
        m_FilteredIndices.insert(m_FilteredIndices.begin(),
                                  newFilteredIndices.begin(),
                                  newFilteredIndices.end());
        endInsertRows();
    }

    return newFilteredCount;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::Clear()
{
    beginResetModel();
    m_AllEntries.clear();
    m_FilteredIndices.clear();
    endResetModel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::UpdateCategories(const std::map<uint32_t, LogCategoryInfo>& categoryMap)
{
    m_CategoryInfoMap = categoryMap;
    if (!m_FilteredIndices.empty())
    {
        emit dataChanged(index(0, std::to_underlying(Column::Category)),
            index(static_cast<int>(m_FilteredIndices.size()) - 1, std::to_underlying(Column::Category)));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::UpdateSeverities(const std::map<uint8_t, QString>& severityMap)
{
    m_SeverityNames = severityMap;
    for (const auto& [id, name] : severityMap) {
        AssignDefaultColorIfNeeded(id);
    }
    if (!m_FilteredIndices.empty())
    {
        emit dataChanged(index(0, std::to_underlying(Column::Severity)),
            index(static_cast<int>(m_FilteredIndices.size()) - 1, std::to_underlying(Column::Severity)));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetCategoryVisible(uint32_t categoryHash, bool visible)
{
    if (!m_VisibleCategories.has_value())
    {
        // Was "show all" — initialize the set with all known categories
        std::set<uint32_t> allHashes;
        for (const auto& [hash, info] : m_CategoryInfoMap) {
            allHashes.insert(hash);
        }
        m_VisibleCategories = std::move(allHashes);
    }

    if (visible) {
        m_VisibleCategories->insert(categoryHash);
    } else {
        m_VisibleCategories->erase(categoryHash);
    }

    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetAllCategoriesVisible(bool visible)
{
    if (visible) {
        m_VisibleCategories.reset(); // nullopt = show all
    } else {
        m_VisibleCategories = std::set<uint32_t>{}; // empty set = show none
    }
    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetVisibleCategorySet(std::set<uint32_t> visibleHashes)
{
    m_VisibleCategories = std::move(visibleHashes);
    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetCategoryMinSeverity(uint32_t categoryHash, uint8_t minSeverity)
{
    m_CategoryMinSeverity[categoryHash] = minSeverity;
    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetCategoriesMinSeverity(const std::vector<uint32_t>& categoryHashes, uint8_t minSeverity)
{
    for (uint32_t hash : categoryHashes) {
        m_CategoryMinSeverity[hash] = minSeverity;
    }
    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::MergeCategorySeverities(const std::map<uint32_t, uint8_t>& severities)
{
    for (const auto& [hash, severity] : severities) {
        m_CategoryMinSeverity[hash] = severity;
    }
    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetSearchText(const QString& text)
{
    m_SearchText = text;
    if (m_SearchFilterEnabled) {
        Refilter();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetSearchFilterEnabled(bool enabled)
{
    m_SearchFilterEnabled = enabled;
    Refilter();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::SetSearchMetadataEnabled(bool enabled)
{
    m_SearchMetadataEnabled = enabled;
    if (m_SearchFilterEnabled) {
        Refilter();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

uint8_t LogViewModel::GetCategoryMinSeverity(uint32_t categoryHash) const
{
    const auto it = m_CategoryMinSeverity.find(categoryHash);
    return (it != m_CategoryMinSeverity.end()) ? it->second : 0xFE;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<int64_t> LogViewModel::GetOldestEntryTimestamp() const
{
    if (m_AllEntries.empty()) {
        return std::nullopt;
    }
    return m_AllEntries.front().TimestampNS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<int64_t> LogViewModel::GetNewestEntryTimestamp() const
{
    if (m_AllEntries.empty()) {
        return std::nullopt;
    }
    return m_AllEntries.back().TimestampNS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QColor LogViewModel::GetSeverityColor(uint8_t severityId) const
{
    const auto it = m_SeverityColors.find(severityId);
    if (it != m_SeverityColors.end()) {
        return it->second;
    }
    return s_DefaultColorPalette[s_DefaultColorPaletteSize - 1];
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int LogViewModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_FilteredIndices.size());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int LogViewModel::columnCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return std::to_underlying(Column::Count);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QVariant LogViewModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= static_cast<int>(m_FilteredIndices.size())) {
        return {};
    }

    const LogEntry& entry = m_AllEntries[m_FilteredIndices[index.row()]];

    const Column column = Column(index.column());

    if (role == Qt::DisplayRole) {
        return GetColumnText(entry, column);
    }

    if (role == Qt::ForegroundRole)
    {
        if (column == Column::Message) {
            return QBrush(QColor(0, 0, 0));
        } else {
            return QBrush(GetSeverityColor(entry.Severity));
        }
    }

    if (role == Qt::BackgroundRole) {
        return QBrush((index.row() % 2) ? QColor(0xff, 0xff, 0xff) : QColor(0xe0, 0xe0, 0xe0));
    }

    if (role == SeverityIdRole) {
        return entry.Severity;
    }

    if (role == Qt::TextAlignmentRole) {
        return QVariant(Qt::AlignLeft | Qt::AlignTop);
    }

    return {};
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QVariant LogViewModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
        return {};
    }

    switch (static_cast<Column>(section))
    {
        case Column::Timestamp: return "Timestamp";
        case Column::Category:  return "Category";
        case Column::Severity:  return "Severity";
        case Column::Message:   return "Message";
        default:                return {};
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool LogViewModel::PassesFilter(const LogEntry& entry) const
{
    if (m_VisibleCategories.has_value() && !m_VisibleCategories->contains(entry.CategoryHash)) {
        return false;
    }
    const uint8_t minSeverity = GetCategoryMinSeverity(entry.CategoryHash);
    if (minSeverity == 0xFF) {
        return false;
    }
    if (entry.Severity > minSeverity) {
        return false;
    }
    if (m_SearchFilterEnabled && !m_SearchText.isEmpty())
    {
        if (m_SearchMetadataEnabled)
        {
            bool matched = entry.Message.contains(m_SearchText, Qt::CaseInsensitive);
            for (int col = 0; !matched && col < std::to_underlying(Column::Count); ++col)
            {
                if (col != std::to_underlying(Column::Message)) {
                    matched = GetColumnText(entry, Column(col)).contains(m_SearchText, Qt::CaseInsensitive);
                }
            }
            if (!matched) {
                return false;
            }
        }
        else
        {
            if (!entry.Message.contains(m_SearchText, Qt::CaseInsensitive)) {
                return false;
            }
        }
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::Refilter()
{
    beginResetModel();
    m_FilteredIndices.clear();
    for (size_t i = 0; i < m_AllEntries.size(); ++i)
    {
        if (PassesFilter(m_AllEntries[i])) {
            m_FilteredIndices.push_back(i);
        }
    }
    endResetModel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogViewModel::AssignDefaultColorIfNeeded(uint8_t severityId)
{
    if (m_SeverityColors.contains(severityId)) {
        return;
    }

    // Assign colors in arrival order using the palette
    const int slotIndex = std::min(static_cast<int>(severityId), s_DefaultColorPaletteSize - 1);
    m_SeverityColors[severityId] = s_DefaultColorPalette[slotIndex];
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString LogViewModel::GetColumnText(const LogEntry& entry, Column column) const
{
    switch (column)
    {
        case Column::Timestamp:
            if (entry.TimestampTextCache.isEmpty()) {
                entry.TimestampTextCache = FormatTimestamp(entry.TimestampNS);
            }
            return entry.TimestampTextCache;
        case Column::Category:
        {
            const auto it = m_CategoryInfoMap.find(entry.CategoryHash);
            return (it != m_CategoryInfoMap.end())
                ? it->second.DisplayName
                : QString("0x%1").arg(entry.CategoryHash, 8, 16, QChar('0'));
        }
        case Column::Severity:
        {
            const auto it = m_SeverityNames.find(entry.Severity);
            return (it != m_SeverityNames.end()) ? it->second : QString::number(entry.Severity);
        }
        case Column::Message:
            return entry.Message;
        default:
            return {};
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString LogViewModel::FormatTimestamp(int64_t timestampNS)
{
    return QDateTime::fromMSecsSinceEpoch(timestampNS / 1000000LL, QTimeZone::systemTimeZone()).toString("yyMMdd.HH:mm:ss.zzz");
}
