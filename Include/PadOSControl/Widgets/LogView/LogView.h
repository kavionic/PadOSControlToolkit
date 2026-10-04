// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <limits>

#include <map>
#include <optional>
#include <set>
#include <vector>

#include <QWidget>

#include "PadOSControl/Widgets/LogView/LogViewModel.h"
#include "PadOSControl/Widgets/LogView/LogViewTableView.h"

class LogViewDelegate;
class CheckableComboBox;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QScrollBar;
class DeviceSession;
enum class MainState : int;
enum class PLogSeverity : uint8_t;

namespace SerialProtocol
{
struct LogMessage;
struct LogCategoriesReply;
struct LogSeveritiesReply;
struct LogHistoryComplete;
}

class LogView : public QWidget
{
    Q_OBJECT

public:
    explicit LogView(QWidget* parent = nullptr);
    ~LogView() override;

    void SetDeviceSession(DeviceSession* deviceSession);

    void AddEntry(LogEntry entry);
    void AddSystemMessage(PLogSeverity severity, const QString& text);

    void UpdateCategories(const std::map<uint32_t, LogCategoryInfo>& categoryMap);
    void UpdateSeverities(const std::map<uint8_t, QString>& severityMap);

    void SaveSettings();
    void LoadSettings();

private slots:
    void SlotMainStateChanged(MainState state);
    void OnScrollValueChanged(int value);

private:
    void RebuildCategoryDropdowns();
    void RebuildVerbosityDropdown();

    void OnCategoryConfigChanged();
    void OnVerbosityComboChanged(int index);
    void OnFilterCheckStateChanged();
    void UpdateVerbositySelection();
    void OnHeaderContextMenu(const QPoint& pos);
    void CopySelectionToClipboard();

    void OnSearchTextChanged();
    void OnSearchNext();
    void OnSearchPrev();
    void OnSearchFilterCheckboxChanged(bool checked);
    void OnSearchMetadataCheckboxChanged(bool checked);
    void OnClearLog();
    void RecomputeSearchMatches();
    void CollectSearchMatchesForRows(int firstRow, int lastRow, const QString& searchText, std::vector<LogViewCellPosition>& matches) const;
    void JumpToMatch(size_t matchIndex);
    void JumpToFirstMatchAfterTop();
    void OnModelReset();
    void OnRowsInserted(const QModelIndex& parent, int first, int last);

    void ProcessLogMessage(const SerialProtocol::LogMessage& packet);
    void ProcessLogCategoriesReply(const SerialProtocol::LogCategoriesReply& packet);
    void ProcessLogSeveritiesReply(const SerialProtocol::LogSeveritiesReply& packet);
    void ProcessLogHistoryComplete(const SerialProtocol::LogHistoryComplete& packet);

    void CheckAndRequestHistory();

    bool IsAtBottom() const;

    static constexpr uint32_t SYSTEM_CATEGORY_HASH = 0xFFFFFFFFu;
    static const LogCategoryInfo SYSTEM_CATEGORY_INFO;

    DeviceSession*     m_DeviceSession     = nullptr;
    LogViewModel*       m_Model              = nullptr;
    LogViewDelegate*    m_Delegate           = nullptr;
    LogViewTableView*   m_TableView          = nullptr;
    CheckableComboBox*  m_CategoryCombo      = nullptr;
    QComboBox*          m_VerbosityCombo     = nullptr;
    CheckableComboBox*  m_FilterCombo        = nullptr;
    QLineEdit*          m_SearchEdit         = nullptr;
    QPushButton*        m_SearchPrevButton   = nullptr;
    QPushButton*        m_SearchNextButton   = nullptr;
    QCheckBox*          m_SearchFilterCheckbox   = nullptr;
    QCheckBox*          m_SearchMetadataCheckbox = nullptr;
    QPushButton*        m_ClearButton            = nullptr;

    bool                m_RebuildingDropdowns                = false;
    bool                m_ClearPending                       = false;
    bool                m_HistoryRequestInProgress           = false;
    bool                m_HasMoreHistory                     = true;
    bool                m_CatchUpInProgress                  = false;
    int64_t             m_CatchUpPreviousSessionMaxTimestamp = 0;
    int                 m_PreviousScrollValue                = 0;

    std::vector<LogEntry> m_PendingHistoryEntries;

    std::vector<LogViewCellPosition>     m_SearchMatches;
    size_t                               m_CurrentMatchIndex  = std::numeric_limits<size_t>::max();

    std::map<uint32_t, LogCategoryInfo>  m_LogCategoryMap;
    std::map<uint8_t, QString>           m_LogSeverityMap;

    std::map<uint32_t, uint8_t>          m_SavedCategorySeverities;
    std::optional<std::set<uint32_t>>    m_SavedConfigChecked;
    std::optional<std::set<uint32_t>>    m_SavedFilterChecked;
};
