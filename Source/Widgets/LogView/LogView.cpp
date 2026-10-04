// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/LogView/LogView.h"
#include "PadOSControl/Widgets/LogView/LogViewDelegate.h"
#include "PadOSControl/Widgets/LogView/LogViewTableView.h"
#include "PadOSControl/Widgets/CheckableComboBox.h"
#include "PadOSControl/Core/DeviceSession.h"
#include "PadOSControl/Core/SerialHandler.h"
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <SerialConsole/LogMessages.h>

#undef ERROR
#include <Utils/LogSeverity.h>

const LogCategoryInfo LogView::SYSTEM_CATEGORY_INFO = { "System", "System" };

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

LogView::LogView(QWidget* parent)
    : QWidget(parent)
{
    m_Model = new LogViewModel(this);
    m_Delegate = new LogViewDelegate(m_Model, this);

    // Register the System category so it appears in the dropdowns
    std::map<uint32_t, LogCategoryInfo> systemCategories;
    systemCategories[SYSTEM_CATEGORY_HASH] = SYSTEM_CATEGORY_INFO;
    m_Model->UpdateCategories(systemCategories);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);

    // --- Control row ---
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(6);

    QLabel* categoryLabel = new QLabel("Category:", this);
    m_CategoryCombo = new CheckableComboBox(this);
    m_CategoryCombo->setMinimumWidth(120);

    QLabel* verbosityLabel = new QLabel("Verbosity:", this);
    m_VerbosityCombo = new QComboBox(this);
    m_VerbosityCombo->setMinimumWidth(120);
    m_VerbosityCombo->setPlaceholderText("Mixed");

    QLabel* filterLabel = new QLabel("Filter:", this);
    m_FilterCombo = new CheckableComboBox(this);
    m_FilterCombo->setMinimumWidth(140);

    controlLayout->addWidget(categoryLabel);
    controlLayout->addWidget(m_CategoryCombo);
    controlLayout->addWidget(verbosityLabel);
    controlLayout->addWidget(m_VerbosityCombo);
    controlLayout->addSpacing(16);
    controlLayout->addWidget(filterLabel);
    controlLayout->addWidget(m_FilterCombo);

    controlLayout->addSpacing(16);

    m_SearchEdit = new QLineEdit(this);
    m_SearchEdit->setPlaceholderText("Search...");
    m_SearchEdit->setMinimumWidth(160);
    m_SearchEdit->setClearButtonEnabled(true);
    controlLayout->addWidget(m_SearchEdit);

    m_SearchPrevButton = new QPushButton("◀", this);
    m_SearchPrevButton->setFixedWidth(28);
    m_SearchPrevButton->setToolTip("Previous match (Shift+F3)");
    controlLayout->addWidget(m_SearchPrevButton);

    m_SearchNextButton = new QPushButton("▶", this);
    m_SearchNextButton->setFixedWidth(28);
    m_SearchNextButton->setToolTip("Next match (F3)");
    controlLayout->addWidget(m_SearchNextButton);

    m_SearchMetadataCheckbox = new QCheckBox("Search metadata", this);
    controlLayout->addWidget(m_SearchMetadataCheckbox);

    m_SearchFilterCheckbox = new QCheckBox("Hide non-matching", this);
    controlLayout->addWidget(m_SearchFilterCheckbox);

    m_ClearButton = new QPushButton(this);
    m_ClearButton->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
    m_ClearButton->setToolTip("Clear log history");
    m_ClearButton->setFixedWidth(28);
    controlLayout->addWidget(m_ClearButton);

    controlLayout->addStretch();

    mainLayout->addLayout(controlLayout);

    // --- Table view ---
    m_TableView = new LogViewTableView(this);
    m_TableView->setModel(m_Model);
    m_TableView->setItemDelegate(m_Delegate);
    m_TableView->setFont(QFont("Consolas", 9));
    m_TableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_TableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_TableView->setEditTriggers(QAbstractItemView::DoubleClicked);
    m_TableView->setAlternatingRowColors(false);
    m_TableView->setWordWrap(true);
    m_TableView->horizontalHeader()->setStretchLastSection(true);
    m_TableView->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_TableView->verticalHeader()->hide();
    m_TableView->verticalHeader()->setDefaultSectionSize(20);
    m_TableView->setShowGrid(false);

    // Default column widths
    m_TableView->setColumnWidth(std::to_underlying(LogViewModel::Column::Timestamp), 150);
    m_TableView->setColumnWidth(std::to_underlying(LogViewModel::Column::Category),   70);
    m_TableView->setColumnWidth(std::to_underlying(LogViewModel::Column::Severity),   100);

    mainLayout->addWidget(m_TableView);

    connect(m_TableView->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &LogView::OnScrollValueChanged);

    LoadSettings();

    // Build initial dropdown content
    RebuildCategoryDropdowns();
    RebuildVerbosityDropdown();

    // Connections
    connect(m_CategoryCombo, &CheckableComboBox::CheckStateChanged,
            this, &LogView::OnCategoryConfigChanged);

    connect(m_VerbosityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogView::OnVerbosityComboChanged);

    connect(m_FilterCombo, &CheckableComboBox::CheckStateChanged,
            this, &LogView::OnFilterCheckStateChanged);

    connect(m_TableView->horizontalHeader(), &QHeaderView::customContextMenuRequested,
            this, &LogView::OnHeaderContextMenu);

    connect(m_TableView, &LogViewTableView::CopyRequested,
            this, &LogView::CopySelectionToClipboard);

    connect(m_Model, &QAbstractItemModel::modelAboutToBeReset,
            m_TableView, &LogViewTableView::ClearSelection);

    connect(m_SearchEdit, &QLineEdit::textChanged,
            this, &LogView::OnSearchTextChanged);

    connect(m_SearchNextButton, &QPushButton::clicked,
            this, &LogView::OnSearchNext);

    connect(m_SearchPrevButton, &QPushButton::clicked,
            this, &LogView::OnSearchPrev);

    connect(m_SearchMetadataCheckbox, &QCheckBox::toggled,
            this, &LogView::OnSearchMetadataCheckboxChanged);

    connect(m_SearchFilterCheckbox, &QCheckBox::toggled,
            this, &LogView::OnSearchFilterCheckboxChanged);

    connect(m_ClearButton, &QPushButton::clicked,
            this, &LogView::OnClearLog);

    connect(m_Model, &QAbstractItemModel::modelReset,
            this, &LogView::OnModelReset);

    connect(m_Model, &QAbstractItemModel::rowsInserted,
            this, &LogView::OnRowsInserted);

    QShortcut* nextShortcut = new QShortcut(QKeySequence(Qt::Key_F3), this);
    connect(nextShortcut, &QShortcut::activated, this, &LogView::OnSearchNext);

    QShortcut* prevShortcut = new QShortcut(QKeySequence(Qt::ShiftModifier | Qt::Key_F3), this);
    connect(prevShortcut, &QShortcut::activated, this, &LogView::OnSearchPrev);

    // Apply loaded checkbox states to model (signals not connected during LoadSettings).
    if (m_SearchMetadataCheckbox->isChecked())
    {
        m_Model->SetSearchMetadataEnabled(true);
        m_TableView->SetSearchMetadataEnabled(true);
    }
    if (m_SearchFilterCheckbox->isChecked()) {
        m_Model->SetSearchFilterEnabled(true);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

LogView::~LogView()
{
    if (m_DeviceSession != nullptr) {
        m_DeviceSession->GetSerialHandler().UnregisterAllPacketHandlers(this);
    }
}

void LogView::SetDeviceSession(DeviceSession* deviceSession)
{
    Q_ASSERT(deviceSession != nullptr);
    Q_ASSERT(m_DeviceSession == nullptr);
    m_DeviceSession = deviceSession;
    connect(m_DeviceSession, &DeviceSession::SignalLogMessage, this, &LogView::AddSystemMessage);

    SerialHandler& serialHandler = m_DeviceSession->GetSerialHandler();
    serialHandler.RegisterPacketHandler<SerialProtocol::LogMessage>(this, &LogView::ProcessLogMessage);
    serialHandler.RegisterPacketHandler<SerialProtocol::LogCategoriesReply>(this, &LogView::ProcessLogCategoriesReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::LogSeveritiesReply>(this, &LogView::ProcessLogSeveritiesReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::LogHistoryComplete>(this, &LogView::ProcessLogHistoryComplete);

    connect(m_DeviceSession, &DeviceSession::SignalMainStateChanged, this, &LogView::SlotMainStateChanged);
    SlotMainStateChanged(m_DeviceSession->GetMainState());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::AddEntry(LogEntry entry)
{
    const bool atBottom = IsAtBottom();
    m_Model->AddEntry(std::move(entry));
    if (atBottom && m_TableView->GetTextSelection().IsEmpty()) {
        m_TableView->scrollToBottom();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::AddSystemMessage(PLogSeverity severity, const QString& text)
{
    AddEntry({ QDateTime::currentDateTime().toMSecsSinceEpoch() * 1000000LL, SYSTEM_CATEGORY_HASH, std::to_underlying(severity), text });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::UpdateCategories(const std::map<uint32_t, LogCategoryInfo>& categoryMap)
{
    // Merge: keep the System pseudo-category, add/replace device categories
    std::map<uint32_t, LogCategoryInfo> merged = categoryMap;
    merged[SYSTEM_CATEGORY_HASH] = SYSTEM_CATEGORY_INFO;
    m_Model->UpdateCategories(merged);
    RebuildCategoryDropdowns();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::UpdateSeverities(const std::map<uint8_t, QString>& severityMap)
{
    m_Model->UpdateSeverities(severityMap);
    RebuildVerbosityDropdown();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::RebuildCategoryDropdowns()
{
    m_RebuildingDropdowns = true;

    const int prevConfigCount = m_CategoryCombo->GetCheckableItemCount();
    std::set<uint32_t> prevConfigAll;
    std::set<uint32_t> prevConfigChecked;
    for (int i = 0; i < prevConfigCount; ++i)
    {
        const uint32_t hash = m_CategoryCombo->GetItemUserData(i).value<uint32_t>();
        prevConfigAll.insert(hash);
        if (m_CategoryCombo->IsItemChecked(i)) {
            prevConfigChecked.insert(hash);
        }
    }

    m_CategoryCombo->ClearCheckableItems();

    const int prevFilterCount = m_FilterCombo->GetCheckableItemCount();
    std::set<uint32_t> prevFilterAll;
    std::set<uint32_t> prevFilterChecked;
    for (int i = 0; i < prevFilterCount; ++i)
    {
        const uint32_t hash = m_FilterCombo->GetItemUserData(i).value<uint32_t>();
        prevFilterAll.insert(hash);
        if (m_FilterCombo->IsItemChecked(i)) {
            prevFilterChecked.insert(hash);
        }
    }

    m_FilterCombo->ClearCheckableItems();

    std::vector<std::pair<uint32_t, const LogCategoryInfo*>> sortedCategories;
    for (const auto& [hash, info] : m_Model->GetKnownCategories()) {
        sortedCategories.emplace_back(hash, &info);
    }
    std::sort(sortedCategories.begin(), sortedCategories.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second->DisplayName.compare(rhs.second->DisplayName, Qt::CaseInsensitive) < 0; });

    for (const auto& [hash, info] : sortedCategories)
    {
        const bool configChecked = prevConfigAll.contains(hash)
            ? prevConfigChecked.contains(hash)
            : (m_SavedConfigChecked.has_value() && m_SavedConfigChecked->contains(hash));
        m_CategoryCombo->AddCheckableItem(info->DisplayName, QVariant::fromValue(hash), configChecked);

        const bool filterChecked = prevFilterAll.contains(hash)
            ? prevFilterChecked.contains(hash)
            : (!m_SavedFilterChecked.has_value() || m_SavedFilterChecked->contains(hash));
        m_FilterCombo->AddCheckableItem(info->DisplayName, QVariant::fromValue(hash), filterChecked);
    }

    m_RebuildingDropdowns = false;
    UpdateVerbositySelection();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::RebuildVerbosityDropdown()
{
    m_RebuildingDropdowns = true;

    m_VerbosityCombo->clear();

    m_VerbosityCombo->addItem("Off", QVariant::fromValue<uint8_t>(0xFFu));

    for (const auto& [id, name] : m_Model->GetKnownSeverities()) {
        m_VerbosityCombo->addItem(name, QVariant::fromValue(id));
    }

    m_RebuildingDropdowns = false;
    UpdateVerbositySelection();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnCategoryConfigChanged()
{
    if (m_RebuildingDropdowns) { return; }
    UpdateVerbositySelection();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnVerbosityComboChanged(int index)
{
    if (m_RebuildingDropdowns || index < 0) {
        return;
    }

    const uint8_t minSeverity = m_VerbosityCombo->itemData(index).value<uint8_t>();

    if (m_CategoryCombo->IsAllChecked())
    {
        std::vector<uint32_t> allHashes;
        for (const auto& [hash, info] : m_Model->GetKnownCategories()) {
            allHashes.push_back(hash);
        }
        m_Model->SetCategoriesMinSeverity(allHashes, minSeverity);
    }
    else
    {
        std::vector<uint32_t> checkedHashes;
        const int count = m_CategoryCombo->GetCheckableItemCount();
        for (int i = 0; i < count; ++i)
        {
            if (m_CategoryCombo->IsItemChecked(i)) {
                checkedHashes.push_back(m_CategoryCombo->GetItemUserData(i).value<uint32_t>());
            }
        }
        m_Model->SetCategoriesMinSeverity(checkedHashes, minSeverity);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::UpdateVerbositySelection()
{
    const bool wasRebuilding = m_RebuildingDropdowns;
    m_RebuildingDropdowns = true;

    std::vector<uint32_t> checkedHashes;
    const int count = m_CategoryCombo->GetCheckableItemCount();
    for (int i = 0; i < count; ++i)
    {
        if (m_CategoryCombo->IsItemChecked(i)) {
            checkedHashes.push_back(m_CategoryCombo->GetItemUserData(i).value<uint32_t>());
        }
    }

    if (checkedHashes.empty() || m_VerbosityCombo->count() <= 1)
    {
        m_VerbosityCombo->setCurrentIndex(-1);
        m_RebuildingDropdowns = wasRebuilding;
        return;
    }

    const auto& knownSeverities = m_Model->GetKnownSeverities();
    const uint8_t displayDefault = knownSeverities.empty() ? 0xFE : knownSeverities.rbegin()->first;

    auto displaySeverity = [&](uint32_t hash) -> uint8_t
        {
            const uint8_t sev = m_Model->GetCategoryMinSeverity(hash);
            return (sev == 0xFE) ? displayDefault : sev;
        };

    const uint8_t firstSeverity = displaySeverity(checkedHashes[0]);
    bool allSame = true;
    for (size_t i = 1; i < checkedHashes.size(); ++i)
    {
        if (displaySeverity(checkedHashes[i]) != firstSeverity)
        {
            allSame = false;
            break;
        }
    }

    if (!allSame)
    {
        m_VerbosityCombo->setCurrentIndex(-1);
    }
    else
    {
        int matchIndex = -1;
        for (int i = 0; i < m_VerbosityCombo->count(); ++i)
        {
            if (m_VerbosityCombo->itemData(i).value<uint8_t>() == firstSeverity)
            {
                matchIndex = i;
                break;
            }
        }
        m_VerbosityCombo->setCurrentIndex(matchIndex);
    }

    m_RebuildingDropdowns = wasRebuilding;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnFilterCheckStateChanged()
{
    if (m_RebuildingDropdowns) {
        return;
    }

    const int count = m_FilterCombo->GetCheckableItemCount();
    if (m_FilterCombo->IsAllChecked())
    {
        m_Model->SetAllCategoriesVisible(true);
        return;
    }

    std::set<uint32_t> visibleHashes;
    for (int i = 0; i < count; ++i)
    {
        if (m_FilterCombo->IsItemChecked(i)) {
            visibleHashes.insert(m_FilterCombo->GetItemUserData(i).value<uint32_t>());
        }
    }
    m_Model->SetVisibleCategorySet(std::move(visibleHashes));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnHeaderContextMenu(const QPoint& pos)
{
    QMenu menu(this);
    for (int col = 0; col < m_Model->columnCount(); ++col)
    {
        const QString name = m_Model->headerData(col, Qt::Horizontal, Qt::DisplayRole).toString();
        QAction* action = menu.addAction(name);
        action->setCheckable(true);
        action->setChecked(!m_TableView->isColumnHidden(col));
        action->setData(col);
    }

    QAction* chosen = menu.exec(m_TableView->horizontalHeader()->mapToGlobal(pos));
    if (chosen != nullptr)
    {
        const int col = chosen->data().toInt();
        m_TableView->setColumnHidden(col, !chosen->isChecked());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::CopySelectionToClipboard()
{
    const LogViewTextSelection& selection = m_TableView->GetTextSelection();
    if (selection.IsEmpty()) {
        return;
    }

    const auto selStart = selection.Start();
    const auto selEnd = selection.End();

    QString result;
    for (int row = selStart.Row; row <= selEnd.Row; ++row)
    {
        bool firstCol = true;
        for (int col = 0; col < m_Model->columnCount(); ++col)
        {
            if (m_TableView->isColumnHidden(col)) {
                continue;
            }

            const QString cellText = m_Model->data(m_Model->index(row, col), Qt::DisplayRole).toString();
            const auto [startChar, endChar] = selection.GetCellCharRange(row, col, cellText.length());

            if (startChar == endChar && startChar == 0 && !selection.IntersectsCell(row, col)) {
                continue;
            }

            if (!firstCol) {
                result += '\t';
            }
            result += cellText.mid(startChar, endChar - startChar);
            firstCol = false;
        }
        result += '\n';
    }

    QApplication::clipboard()->setText(result);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool LogView::IsAtBottom() const
{
    const QScrollBar* bar = m_TableView->verticalScrollBar();
    return bar->value() >= bar->maximum() - 4;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::SlotMainStateChanged(MainState state)
{
    if (state == MainState::ConnectedApplication)
    {
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::RequestLogCategories>();
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::RequestLogSeverities>();

        const std::optional<int64_t> newestTimestamp = m_Model->GetNewestEntryTimestamp();
        if (newestTimestamp.has_value())
        {
            m_CatchUpInProgress        = true;
            m_HistoryRequestInProgress = true;
            m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::RequestLogHistory>(
                int64_t(0), 10 * 1024);
        }
        else
        {
            CheckAndRequestHistory();
        }
    }
    else
    {
        m_HistoryRequestInProgress = false;
        m_HasMoreHistory           = true;
        m_CatchUpInProgress        = false;
        m_PendingHistoryEntries.clear();

        if (m_ClearPending)
        {
            m_ClearPending                       = false;
            m_CatchUpPreviousSessionMaxTimestamp = 0;
            m_Model->Clear();
        }
        else
        {
            m_CatchUpPreviousSessionMaxTimestamp = m_Model->GetNewestEntryTimestamp().value_or(0);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::ProcessLogMessage(const SerialProtocol::LogMessage& packet)
{
    const qsizetype messageLength = packet.PackageLength - sizeof(packet);
    const QString message = QString::fromUtf8(reinterpret_cast<const char*>(&packet + 1), messageLength).replace('\0', '?');

    if (m_HistoryRequestInProgress) {
        m_PendingHistoryEntries.push_back({ packet.Timestamp, packet.CategoryHash, packet.Severity, message });
    } else {
        AddEntry({ packet.Timestamp, packet.CategoryHash, packet.Severity, message });
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::ProcessLogCategoriesReply(const SerialProtocol::LogCategoriesReply& packet)
{
    m_LogCategoryMap.clear();
    const SerialProtocol::LogCategoryEntry* entries = reinterpret_cast<const SerialProtocol::LogCategoryEntry*>(&packet + 1);

    std::map<uint32_t, uint8_t> initialSeverities;
    for (uint32_t i = 0; i < packet.CategoryCount; ++i)
    {
        const uint32_t hash = entries[i].CategoryHash;
        m_LogCategoryMap[hash] =
        {
            QString::fromUtf8(entries[i].CategoryName),
            QString::fromUtf8(entries[i].DisplayName)
        };

        if (m_Model->GetCategoryMinSeverity(hash) == 0xFE)
        {
            auto savedIt = m_SavedCategorySeverities.find(hash);
            if (savedIt != m_SavedCategorySeverities.end()) {
                initialSeverities[hash] = savedIt->second;
            } else {
                initialSeverities[hash] = entries[i].MinSeverity;
            }
        }
    }
    UpdateCategories(m_LogCategoryMap);

    if (!initialSeverities.empty()) {
        m_Model->MergeCategorySeverities(initialSeverities);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::ProcessLogSeveritiesReply(const SerialProtocol::LogSeveritiesReply& packet)
{
    m_LogSeverityMap.clear();
    const SerialProtocol::LogSeverityEntry* entries = reinterpret_cast<const SerialProtocol::LogSeverityEntry*>(&packet + 1);

    for (uint32_t i = 0; i < packet.SeverityCount; ++i) {
        m_LogSeverityMap[entries[i].SeverityID] = QString::fromUtf8(entries[i].Name);
    }
    UpdateSeverities(m_LogSeverityMap);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::ProcessLogHistoryComplete(const SerialProtocol::LogHistoryComplete& packet)
{
    m_HistoryRequestInProgress = false;

    if (m_ClearPending)
    {
        m_ClearPending                       = false;
        m_CatchUpInProgress                  = false;
        m_HasMoreHistory                     = true;
        m_CatchUpPreviousSessionMaxTimestamp = 0;
        m_PendingHistoryEntries.clear();
        m_Model->Clear();
        CheckAndRequestHistory();
        return;
    }

    if (m_CatchUpInProgress)
    {
        const bool wasAtBottom = IsAtBottom();
        int64_t oldestPageTimestamp = 0;
        if (!m_PendingHistoryEntries.empty())
        {
            oldestPageTimestamp = std::min_element(
                m_PendingHistoryEntries.begin(), m_PendingHistoryEntries.end(),
                [](const LogEntry& lhs, const LogEntry& rhs) { return lhs.TimestampNS < rhs.TimestampNS; })->TimestampNS;
        }

        const bool hasOverlap = oldestPageTimestamp <= m_CatchUpPreviousSessionMaxTimestamp;

        for (LogEntry& entry : m_PendingHistoryEntries) {
            m_Model->AddEntry(std::move(entry));
        }
        m_PendingHistoryEntries.clear();

        if (wasAtBottom && m_TableView->GetTextSelection().IsEmpty()) {
            m_TableView->scrollToBottom();
        }

        if (!hasOverlap && packet.HasMorePages != 0)
        {
            m_HistoryRequestInProgress = true;
            m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::RequestLogHistory>(oldestPageTimestamp, 10 * 1024);
        }
        else
        {
            m_CatchUpInProgress = false;
            m_HasMoreHistory    = true;
            CheckAndRequestHistory();
        }
        return;
    }

    m_HasMoreHistory = (packet.HasMorePages != 0);

    size_t insertedRows = 0;

    if (!m_PendingHistoryEntries.empty())
    {
        const QModelIndex topIndex = m_TableView->indexAt(QPoint(0, 0));
        const int topRow = topIndex.isValid() ? topIndex.row() : 0;

        insertedRows = m_Model->PrependEntries(std::move(m_PendingHistoryEntries));
        m_PendingHistoryEntries.clear();

        if (insertedRows > 0)
        {
            const int targetRow = topRow + static_cast<int>(insertedRows);
            if (targetRow < m_Model->rowCount()) {
                m_TableView->scrollTo(m_Model->index(targetRow, 0), QAbstractItemView::PositionAtTop);
            } else {
                m_TableView->scrollToBottom();
            }
        }
    }

    if (insertedRows == 0) {
        CheckAndRequestHistory();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnScrollValueChanged(int value)
{
    if (value < m_PreviousScrollValue) {
        CheckAndRequestHistory();
    }
    m_PreviousScrollValue = value;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::CheckAndRequestHistory()
{
    if (m_HistoryRequestInProgress || !m_HasMoreHistory || m_DeviceSession == nullptr) { return; }

    if (m_TableView->indexAt(QPoint(0, 0)).row() > 0) { return; }

    m_HistoryRequestInProgress = true;
    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::RequestLogHistory>(
        m_Model->GetOldestEntryTimestamp().value_or(0), 10 * 1024);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnSearchTextChanged()
{
    m_Model->SetSearchText(m_SearchEdit->text());
    RecomputeSearchMatches();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnSearchNext()
{
    if (m_SearchMatches.empty()) { return; }
    if (m_CurrentMatchIndex == INVALID_INDEX) {
        m_CurrentMatchIndex = 0;
    } else {
        m_CurrentMatchIndex = (m_CurrentMatchIndex + 1) % m_SearchMatches.size();
    }
    JumpToMatch(m_CurrentMatchIndex);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnSearchPrev()
{
    if (m_SearchMatches.empty()) { return; }
    if (m_CurrentMatchIndex == INVALID_INDEX) {
        m_CurrentMatchIndex = m_SearchMatches.size() - 1;
    } else {
        m_CurrentMatchIndex = (m_CurrentMatchIndex + m_SearchMatches.size() - 1) % m_SearchMatches.size();
    }
    JumpToMatch(m_CurrentMatchIndex);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnSearchFilterCheckboxChanged(bool checked)
{
    m_Model->SetSearchFilterEnabled(checked);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnSearchMetadataCheckboxChanged(bool checked)
{
    m_Model->SetSearchMetadataEnabled(checked);
    m_TableView->SetSearchMetadataEnabled(checked);
    RecomputeSearchMatches();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnClearLog()
{
    if (m_HistoryRequestInProgress)
    {
        m_ClearPending = true;
        return;
    }

    m_HasMoreHistory                     = true;
    m_CatchUpPreviousSessionMaxTimestamp = 0;
    m_Model->Clear();
    CheckAndRequestHistory();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::RecomputeSearchMatches()
{
    m_SearchMatches.clear();
    m_CurrentMatchIndex = INVALID_INDEX;

    const QString searchText = m_SearchEdit->text();
    if (searchText.isEmpty())
    {
        m_TableView->SetSearchHighlight({}, -1, -1, -1);
        return;
    }

    const int rowCount = m_Model->rowCount();

    CollectSearchMatchesForRows(0, rowCount - 1, searchText, m_SearchMatches);

    JumpToFirstMatchAfterTop();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::CollectSearchMatchesForRows(int firstRow, int lastRow, const QString& searchText, std::vector<LogViewCellPosition>& matches) const
{
    if (searchText.isEmpty()) { return; }

    const int rowCount = m_Model->rowCount();
    if (rowCount <= 0) { return; }

    const int firstValidRow = (firstRow > 0) ? firstRow : 0;
    const int lastValidRow = (lastRow < rowCount) ? lastRow : rowCount - 1;

    if (firstValidRow > lastValidRow) { return; }

    const bool searchMetadata = m_SearchMetadataCheckbox->isChecked();
    const int colCount = std::to_underlying(LogViewModel::Column::Count);
    const int messageCol = std::to_underlying(LogViewModel::Column::Message);
    const int startCol = searchMetadata ? 0 : messageCol;
    const int endCol = searchMetadata ? colCount : messageCol + 1;

    for (int row = firstValidRow; row <= lastValidRow; ++row)
    {
        for (int col = startCol; col < endCol; ++col)
        {
            const QString text = m_Model->data(m_Model->index(row, col), Qt::DisplayRole).toString();
            int pos = 0;

            for (;;)
            {
                const int matchPos = text.indexOf(searchText, pos, Qt::CaseInsensitive);
                if (matchPos < 0) { break; }
                matches.push_back({row, col, matchPos});
                pos = matchPos + static_cast<int>(searchText.length());
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::JumpToMatch(size_t matchIndex)
{
    m_CurrentMatchIndex = matchIndex;
    const LogViewCellPosition& match = m_SearchMatches[matchIndex];
    m_TableView->scrollTo(m_Model->index(match.Row, 0), QAbstractItemView::EnsureVisible);
    m_TableView->SetSearchHighlight(m_SearchEdit->text(), match.Row, match.Column, match.CharOffset);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::JumpToFirstMatchAfterTop()
{
    if (m_SearchMatches.empty())
    {
        m_TableView->SetSearchHighlight(m_SearchEdit->text(), -1, -1, -1);
        return;
    }

    const QModelIndex topIndex = m_TableView->indexAt(QPoint(0, 0));
    const int firstVisible = topIndex.isValid() ? topIndex.row() : 0;

    const auto it = std::lower_bound(m_SearchMatches.begin(), m_SearchMatches.end(),
                                     firstVisible, [](const LogViewCellPosition& match, int row) {
                                         return match.Row < row;
                                     });
    if (it != m_SearchMatches.end()) {
        m_CurrentMatchIndex = static_cast<size_t>(it - m_SearchMatches.begin());
    } else {
        m_CurrentMatchIndex = 0;
    }

    JumpToMatch(m_CurrentMatchIndex);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnModelReset()
{
    RecomputeSearchMatches();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::OnRowsInserted(const QModelIndex& /*parent*/, int first, int last)
{
    const QString searchText = m_SearchEdit->text();
    if (searchText.isEmpty()) { return; }

    const int insertedRowCount = last - first + 1;

    LogViewCellPosition currentMatch;
    bool hadCurrentMatch = false;

    if (m_CurrentMatchIndex != INVALID_INDEX && m_CurrentMatchIndex < m_SearchMatches.size())
    {
        currentMatch = m_SearchMatches[m_CurrentMatchIndex];
        hadCurrentMatch = true;

        if (currentMatch.Row >= first) {
            currentMatch.Row += insertedRowCount;
        }
    }

    for (LogViewCellPosition& match : m_SearchMatches)
    {
        if (match.Row >= first) {
            match.Row += insertedRowCount;
        }
    }

    std::vector<LogViewCellPosition> insertedMatches;
    CollectSearchMatchesForRows(first, last, searchText, insertedMatches);

    if (!insertedMatches.empty())
    {
        const auto insertPosition = std::lower_bound(m_SearchMatches.begin(), m_SearchMatches.end(), insertedMatches.front());
        m_SearchMatches.insert(insertPosition, insertedMatches.begin(), insertedMatches.end());
    }

    if (hadCurrentMatch)
    {
        const auto currentMatchPosition = std::lower_bound(m_SearchMatches.begin(), m_SearchMatches.end(), currentMatch);

        if (currentMatchPosition != m_SearchMatches.end() && *currentMatchPosition == currentMatch)
        {
            m_CurrentMatchIndex = static_cast<size_t>(currentMatchPosition - m_SearchMatches.begin());
            m_TableView->SetSearchHighlight(searchText, currentMatch.Row, currentMatch.Column, currentMatch.CharOffset);
        }
        else
        {
            m_CurrentMatchIndex = INVALID_INDEX;
            JumpToFirstMatchAfterTop();
        }

        m_TableView->viewport()->update();
        return;
    }

    if (!insertedMatches.empty())
    {
        m_TableView->viewport()->update();
        JumpToFirstMatchAfterTop();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::SaveSettings()
{
    QSettings settings;
    settings.beginGroup("LogView");

    settings.beginGroup("CategorySeverities");
    settings.remove("");
    for (const auto& [hash, info] : m_Model->GetKnownCategories())
    {
        const uint8_t severity = m_Model->GetCategoryMinSeverity(hash);
        if (severity != 0xFE) {
            settings.setValue(QString::number(hash, 16), severity);
        }
    }
    settings.endGroup();

    {
        QStringList checkedList;
        const int count = m_CategoryCombo->GetCheckableItemCount();
        for (int i = 0; i < count; ++i)
        {
            if (m_CategoryCombo->IsItemChecked(i)) {
                checkedList.append(QString::number(m_CategoryCombo->GetItemUserData(i).value<uint32_t>(), 16));
            }
        }
        settings.setValue("ConfigChecked", checkedList);
    }

    {
        QStringList checkedList;
        const int count = m_FilterCombo->GetCheckableItemCount();
        bool allChecked = m_FilterCombo->IsAllChecked();
        if (!allChecked)
        {
            for (int i = 0; i < count; ++i)
            {
                if (m_FilterCombo->IsItemChecked(i)) {
                    checkedList.append(QString::number(m_FilterCombo->GetItemUserData(i).value<uint32_t>(), 16));
                }
            }
        }
        settings.setValue("FilterChecked", allChecked ? QStringList{"all"} : checkedList);
    }

    settings.setValue("CategorySearchText", m_CategoryCombo->GetSearchText());
    settings.setValue("FilterSearchText", m_FilterCombo->GetSearchText());

    settings.setValue("SearchMetadataEnabled", m_SearchMetadataCheckbox->isChecked());
    settings.setValue("SearchFilterEnabled", m_SearchFilterCheckbox->isChecked());

    settings.endGroup();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void LogView::LoadSettings()
{
    QSettings settings;
    settings.beginGroup("LogView");

    settings.beginGroup("CategorySeverities");
    const QStringList severityKeys = settings.childKeys();
    for (const QString& key : severityKeys)
    {
        bool validHash = false;
        const uint32_t hash = key.toUInt(&validHash, 16);
        if (validHash) {
            m_SavedCategorySeverities[hash] = static_cast<uint8_t>(settings.value(key).toUInt());
        }
    }
    settings.endGroup();

    if (settings.contains("ConfigChecked"))
    {
        const QStringList configList = settings.value("ConfigChecked").toStringList();
        std::set<uint32_t> configSet;
        for (const QString& entry : configList)
        {
            bool valid = false;
            const uint32_t hash = entry.toUInt(&valid, 16);
            if (valid) {
                configSet.insert(hash);
            }
        }
        m_SavedConfigChecked = std::move(configSet);
    }

    if (settings.contains("FilterChecked"))
    {
        const QStringList filterList = settings.value("FilterChecked").toStringList();
        if (filterList.size() == 1 && filterList[0] == "all")
        {
            // nullopt means "all checked" as default
        }
        else
        {
            std::set<uint32_t> filterSet;
            for (const QString& entry : filterList)
            {
                bool valid = false;
                const uint32_t hash = entry.toUInt(&valid, 16);
                if (valid) {
                    filterSet.insert(hash);
                }
            }
            m_SavedFilterChecked = std::move(filterSet);
        }
    }

    m_CategoryCombo->SetSearchText(settings.value("CategorySearchText").toString());
    m_FilterCombo->SetSearchText(settings.value("FilterSearchText").toString());

    m_SearchMetadataCheckbox->setChecked(settings.value("SearchMetadataEnabled", false).toBool());
    m_SearchFilterCheckbox->setChecked(settings.value("SearchFilterEnabled", false).toBool());

    settings.endGroup();
}
