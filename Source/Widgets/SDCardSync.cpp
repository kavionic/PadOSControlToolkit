// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Widgets/SDCardSync.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncIgnoreRules.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"
#include "PadOSControl/Core/SDCardSync/SDStateCrawlingRemote.h"
#include "PadOSControl/Core/SDCardSync/SDStateCreateDirectory.h"
#include "PadOSControl/Core/SDCardSync/SDStateDeleteFiles.h"
#include "PadOSControl/Core/SDCardSync/SDStateDownloadFiles.h"
#include "PadOSControl/Core/SDCardSync/SDStateTimestampUpdate.h"
#include "PadOSControl/Core/SDCardSync/SDStateUploadFiles.h"
#include "SerialConsole/FilesystemMessages.h"

#include "PadOSControl/Core/DeviceSession.h"
#include "PadOSControl/Core/SerialHandler.h"

#ifdef ERROR
#undef ERROR
#endif
#include <Utils/LogSeverity.h>

#include <algorithm>
#include <random>
#include <utility>

#ifdef Q_OS_WIN
#include <objbase.h>
#include <shobjidl.h>
#endif

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSize>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWheelEvent>

static constexpr int REFRESH_DEBOUNCE_MILLISECONDS = 500;
static constexpr int MAX_RECENT_LOCAL_ROOTS = 10;
static constexpr const char* DEVICE_ROOT_PATH = "/sdcard";
static constexpr const char* SYNC_IGNORE_FILE_NAME = ".syncignore";
static constexpr const char* DELETE_RULES_FILE_NAME = ".deleterules";
static constexpr const char* DIFF_TOOL_PROGRAM = "WinMergeU.exe";
static constexpr const char* WINMERGE_START_MENU_SHORTCUT = "C:/ProgramData/Microsoft/Windows/Start Menu/Programs/WinMerge/WinMerge.lnk";

class SDCardSyncNameColumnDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    virtual QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (index.column() == 0)
        {
            return QStyledItemDelegate::createEditor(parent, option, index);
        }
        return nullptr;
    }
};

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static QString ResolveWindowsShortcut(const QString& shortcutPath)
{
    QString result;
#ifdef Q_OS_WIN
    if (QFileInfo(shortcutPath).exists())
    {
        HRESULT resultCode = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool uninitializeCom = false;
        if (SUCCEEDED(resultCode))
        {
            uninitializeCom = true;
        }

        if (SUCCEEDED(resultCode) || resultCode == RPC_E_CHANGED_MODE)
        {
            IShellLinkW* shellLink = nullptr;
            resultCode = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&shellLink));
            if (SUCCEEDED(resultCode) && shellLink != nullptr)
            {
                IPersistFile* persistFile = nullptr;
                resultCode = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
                if (SUCCEEDED(resultCode) && persistFile != nullptr)
                {
                    const std::wstring nativeShortcutPath = QDir::toNativeSeparators(shortcutPath).toStdWString();
                    resultCode = persistFile->Load(nativeShortcutPath.c_str(), STGM_READ);
                    if (SUCCEEDED(resultCode))
                    {
                        wchar_t targetPath[MAX_PATH] = {};
                        resultCode = shellLink->GetPath(targetPath, MAX_PATH, nullptr, SLGP_UNCPRIORITY);
                        if (SUCCEEDED(resultCode) && targetPath[0] != L'\0')
                        {
                            result = QString::fromWCharArray(targetPath);
                        }
                    }
                    persistFile->Release();
                }
                shellLink->Release();
            }
        }

        if (uninitializeCom)
        {
            CoUninitialize();
        }
    }
#else
    Q_UNUSED(shortcutPath);
#endif
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static QString FindDiffToolProgram()
{
    const QString pathExecutable = QStandardPaths::findExecutable(QString::fromUtf8(DIFF_TOOL_PROGRAM));
    if (!pathExecutable.isEmpty())
    {
        return pathExecutable;
    }

    QStringList candidatePaths;
    candidatePaths << QCoreApplication::applicationDirPath() + "/" + QString::fromUtf8(DIFF_TOOL_PROGRAM);
    candidatePaths << "C:/Program Files/WinMerge/WinMergeU.exe";
    candidatePaths << "C:/Program Files (x86)/WinMerge/WinMergeU.exe";
    candidatePaths << ResolveWindowsShortcut(QString::fromUtf8(WINMERGE_START_MENU_SHORTCUT));
    candidatePaths << ResolveWindowsShortcut(QDir::homePath() + "/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/WinMerge/WinMerge.lnk");

    for (const QString& candidatePath : candidatePaths)
    {
        if (!candidatePath.isEmpty() && QFileInfo(candidatePath).isFile())
        {
            return candidatePath;
        }
    }
    return QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSync::SDCardSync(QWidget* parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    m_ClientToken = std::random_device{}();

    const QStyle* style = QApplication::style();
    ui.m_RefreshButton->setIcon(style->standardIcon(QStyle::SP_BrowserReload));
    ui.m_BrowseButton->setIcon(style->standardIcon(QStyle::SP_DirOpenIcon));
    const QSize treeIconSize(style->pixelMetric(QStyle::PM_SmallIconSize), style->pixelMetric(QStyle::PM_SmallIconSize));

    ui.m_LocalTree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui.m_DeviceTree->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui.m_LocalTree->setIconSize(treeIconSize);
    ui.m_DeviceTree->setIconSize(treeIconSize);
    ui.m_LocalTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui.m_DeviceTree->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui.m_LocalTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui.m_DeviceTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui.m_LocalTree->setContextMenuPolicy(Qt::CustomContextMenu);
    ui.m_DeviceTree->setContextMenuPolicy(Qt::CustomContextMenu);
    ui.m_LocalTree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    ui.m_DeviceTree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    ui.m_LocalTree->setItemDelegate(new SDCardSyncNameColumnDelegate(ui.m_LocalTree));
    ui.m_DeviceTree->setItemDelegate(new SDCardSyncNameColumnDelegate(ui.m_DeviceTree));
    ui.m_LocalTree->setSortingEnabled(false);
    ui.m_DeviceTree->setSortingEnabled(false);
    ui.m_LocalTree->setColumnWidth(1, 130);
    ui.m_DeviceTree->setColumnWidth(1, 130);
    ui.m_LocalTree->setColumnWidth(2, 90);
    ui.m_DeviceTree->setColumnWidth(2, 90);
    ui.m_LocalTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui.m_DeviceTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui.m_LocalTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    ui.m_DeviceTree->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    ui.m_LocalTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui.m_DeviceTree->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    ui.m_LocalTree->viewport()->installEventFilter(this);
    ui.m_DeviceTree->viewport()->installEventFilter(this);
    ui.m_ProgressBar->setVisible(false);
    ui.m_ProgressBar->setRange(0, 1000);
    ui.m_ProgressBar->setValue(0);
    ui.m_FileProgressBar->setVisible(false);
    ui.m_FileProgressBar->setRange(0, 1000);
    ui.m_FileProgressBar->setValue(0);
    ui.m_ProgressItemEdit->clear();

    m_RefreshDebounceTimer.setSingleShot(true);
    m_RefreshDebounceTimer.setInterval(REFRESH_DEBOUNCE_MILLISECONDS);
    m_ScrollUpdateTimer.setSingleShot(true);
    m_ScrollUpdateTimer.setInterval(0);

    LoadSettings();

    connect(ui.m_BrowseButton, &QAbstractButton::clicked, this, &SDCardSync::SlotBrowseButtonClicked);
    connect(ui.m_RefreshButton, &QAbstractButton::clicked, this, &SDCardSync::SlotRefreshButtonClicked);
    connect(ui.m_UpdateButton, &QAbstractButton::clicked, this, &SDCardSync::SlotUpdateButtonClicked);
    connect(ui.m_UpdateSelectedButton, &QAbstractButton::clicked, this, &SDCardSync::SlotUpdateSelectedButtonClicked);
    connect(ui.m_LocalRootCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SDCardSync::SlotLocalRootComboSelectionChanged);
    connect(ui.m_LocalRootCombo->lineEdit(), &QLineEdit::editingFinished, this, &SDCardSync::SlotLocalRootComboTextChanged);
    connect(ui.m_HideIdenticalCheckBox, &QCheckBox::checkStateChanged, this, &SDCardSync::SlotHideIdenticalChanged);
    connect(ui.m_HideDeviceOnlyCheckBox, &QCheckBox::checkStateChanged, this, &SDCardSync::SlotHideDeviceOnlyChanged);
    connect(ui.m_CompareContentCheckBox, &QCheckBox::checkStateChanged, this, &SDCardSync::SlotCompareContentChanged);
    connect(ui.m_SyncDeletesCheckBox, &QCheckBox::checkStateChanged, this, &SDCardSync::SlotSyncDeletesChanged);
    m_LocalScanner.SetLocalRefreshNeededHandler([this]()
    {
        ScheduleRefresh(false);
    });
    connect(&m_RefreshDebounceTimer, &QTimer::timeout, this, &SDCardSync::SlotRefreshDebounced);
    connect(ui.m_LocalTree, &QTreeWidget::itemExpanded, this, &SDCardSync::SlotLocalItemExpanded);
    connect(ui.m_LocalTree, &QTreeWidget::itemCollapsed, this, &SDCardSync::SlotLocalItemCollapsed);
    connect(ui.m_DeviceTree, &QTreeWidget::itemExpanded, this, &SDCardSync::SlotDeviceItemExpanded);
    connect(ui.m_DeviceTree, &QTreeWidget::itemCollapsed, this, &SDCardSync::SlotDeviceItemCollapsed);
    connect(ui.m_LocalTree, &QTreeWidget::itemSelectionChanged, this, &SDCardSync::SlotLocalSelectionChanged);
    connect(ui.m_DeviceTree, &QTreeWidget::itemSelectionChanged, this, &SDCardSync::SlotDeviceSelectionChanged);
    connect(ui.m_LocalTree, &QTreeWidget::itemChanged, this, &SDCardSync::SlotTreeItemChanged);
    connect(ui.m_DeviceTree, &QTreeWidget::itemChanged, this, &SDCardSync::SlotTreeItemChanged);
    connect(ui.m_LocalTree, &QWidget::customContextMenuRequested, this, &SDCardSync::SlotLocalTreeContextMenuRequested);
    connect(ui.m_DeviceTree, &QWidget::customContextMenuRequested, this, &SDCardSync::SlotDeviceTreeContextMenuRequested);
    connect(ui.m_SyncScrollBar, &QScrollBar::valueChanged, this, &SDCardSync::SlotSharedScrollValueChanged);
    connect(ui.m_LocalTree->verticalScrollBar(), &QScrollBar::valueChanged, this, &SDCardSync::SlotTreeScrollValueChanged);
    connect(ui.m_DeviceTree->verticalScrollBar(), &QScrollBar::valueChanged, this, &SDCardSync::SlotTreeScrollValueChanged);
    connect(ui.m_LocalTree->verticalScrollBar(), &QScrollBar::rangeChanged, this, &SDCardSync::SlotTreeScrollRangeChanged);
    connect(ui.m_DeviceTree->verticalScrollBar(), &QScrollBar::rangeChanged, this, &SDCardSync::SlotTreeScrollRangeChanged);
    connect(&m_ScrollUpdateTimer, &QTimer::timeout, this, &SDCardSync::UpdateSharedScrollBar);

    const QString initialRootPath = ui.m_LocalRootCombo->currentText();
    if (!initialRootPath.isEmpty())
    {
        SetLocalRoot(initialRootPath);
    }
    else
    {
        UpdateControls();
        UpdateStatusLabel();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSync::~SDCardSync()
{
    SetActiveState(nullptr);
    m_CurrentFileReader.reset();
    m_CurrentRenameFile.reset();
    if (m_DeviceSession != nullptr) {
        m_DeviceSession->GetSerialHandler().UnregisterAllPacketHandlers(this);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetDeviceSession(DeviceSession* deviceSession)
{
    Q_ASSERT(deviceSession != nullptr);
    Q_ASSERT(m_DeviceSession == nullptr);
    m_DeviceSession = deviceSession;

    SerialHandler& serialHandler = m_DeviceSession->GetSerialHandler();
    serialHandler.RegisterPacketHandler<SerialProtocol::OpenSessionReply>(this, &SDCardSync::HandleOpenSessionReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::FilesystemStatusReply>(this, &SDCardSync::HandleFilesystemStatusReply);

    connect(m_DeviceSession, &DeviceSession::SignalMainStateChanged, this, &SDCardSync::SlotMainStateChanged);
    SlotMainStateChanged(m_DeviceSession->GetMainState());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SaveSettings()
{
    QSettings settings;

    settings.beginWriteArray("SDCardSync/LocalRootList");
    for (int itemIndex = 0; itemIndex < ui.m_LocalRootCombo->count(); ++itemIndex)
    {
        settings.setArrayIndex(itemIndex);
        settings.setValue("Path", ui.m_LocalRootCombo->itemText(itemIndex));
    }
    settings.endArray();
    settings.setValue("SDCardSync/LocalRootList/Selection", ui.m_LocalRootCombo->currentIndex());
    settings.setValue("SDCardSync/HideIdentical", ui.m_HideIdenticalCheckBox->isChecked());
    settings.setValue("SDCardSync/HideDeviceOnly", ui.m_HideDeviceOnlyCheckBox->isChecked());
    settings.setValue("SDCardSync/CompareContent", ui.m_CompareContentCheckBox->isChecked());
    settings.setValue("SDCardSync/SyncDeletes", ui.m_SyncDeletesCheckBox->isChecked());
    settings.setValue("SDCardSync/LastDownloadPath", m_LastDownloadPath);
    settings.setValue("SDCardSync/LastUploadPath", m_LastUploadPath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::eventFilter(QObject* watchedObject, QEvent* event)
{
    if ((watchedObject == ui.m_LocalTree->viewport() || watchedObject == ui.m_DeviceTree->viewport()) && event->type() == QEvent::Wheel)
    {
        const QWheelEvent* wheelEvent = static_cast<const QWheelEvent*>(event);
        int wheelSteps = wheelEvent->angleDelta().y() / 120;
        if (wheelSteps == 0 && wheelEvent->pixelDelta().y() != 0)
        {
            wheelSteps = (wheelEvent->pixelDelta().y() > 0) ? 1 : -1;
        }
        if (wheelSteps != 0)
        {
            const int scrollStep = std::max(1, ui.m_SyncScrollBar->singleStep());
            ui.m_SyncScrollBar->setValue(ui.m_SyncScrollBar->value() - wheelSteps * scrollStep * 3);
            return true;
        }
    }
    return QWidget::eventFilter(watchedObject, event);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotBrowseButtonClicked()
{
    const QString selectedPath = QFileDialog::getExistingDirectory(this, "Select SD-card root", m_LocalRootPath);
    if (!selectedPath.isEmpty())
    {
        SetLocalRoot(selectedPath);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotRefreshButtonClicked()
{
    StartRefresh();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotUpdateButtonClicked()
{
    if (m_State == State::Idle)
    {
        StartUploadQueue(std::make_unique<SDStateUploadFiles>(
            CreateStateContext(),
            m_Model,
            m_LocalRootPath,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            [this](const UploadDirectory& uploadDirectory)
            {
                ApplyCompletedUploadDirectory(uploadDirectory);
            },
            [this](const UploadFile& uploadFile)
            {
                ApplyCompletedUploadFile(uploadFile);
            }
        ), [this]()
        {
            std::unique_ptr<SDCardSyncState> deleteState = CreateSyncDeleteState();
            if (deleteState != nullptr)
            {
                StartDeleteQueue(std::move(deleteState), [this]()
                {
                    FinishUpdateOperation();
                });
            }
            else
            {
                FinishUpdateOperation();
            }
        });
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotUpdateSelectedButtonClicked()
{
    if (m_State == State::Idle)
    {
        StartUploadQueue(std::make_unique<SDStateUploadFiles>(
            CreateStateContext(),
            m_Model,
            m_LocalRootPath,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            GetSelectedPaths(),
            [this](const UploadDirectory& uploadDirectory)
            {
                ApplyCompletedUploadDirectory(uploadDirectory);
            },
            [this](const UploadFile& uploadFile)
            {
                ApplyCompletedUploadFile(uploadFile);
            }
        ));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotLocalRootComboSelectionChanged(int index)
{
    Q_UNUSED(index);
    SetLocalRoot(ui.m_LocalRootCombo->currentText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotLocalRootComboTextChanged()
{
    SetLocalRoot(ui.m_LocalRootCombo->currentText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotHideIdenticalChanged(Qt::CheckState state)
{
    Q_UNUSED(state);
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotHideDeviceOnlyChanged(Qt::CheckState state)
{
    Q_UNUSED(state);
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotCompareContentChanged(Qt::CheckState state)
{
    Q_UNUSED(state);
    StartRefresh(!m_DeviceReadComplete);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotSyncDeletesChanged(Qt::CheckState state)
{
    Q_UNUSED(state);
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotRefreshDebounced()
{
    const bool readDevice = m_DebouncedRefreshReadDevice;
    m_DebouncedRefreshReadDevice = false;
    StartRefresh(readDevice);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotLocalItemExpanded(QTreeWidgetItem* item)
{
    SetPairedItemExpanded(item, true);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotLocalItemCollapsed(QTreeWidgetItem* item)
{
    SetPairedItemExpanded(item, false);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotDeviceItemExpanded(QTreeWidgetItem* item)
{
    SetPairedItemExpanded(item, true);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotDeviceItemCollapsed(QTreeWidgetItem* item)
{
    SetPairedItemExpanded(item, false);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotLocalSelectionChanged()
{
    SyncTreeSelection(ui.m_LocalTree, ui.m_DeviceTree);
    UpdateControls();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotDeviceSelectionChanged()
{
    SyncTreeSelection(ui.m_DeviceTree, ui.m_LocalTree);
    UpdateControls();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotTreeItemChanged(QTreeWidgetItem* item, int column)
{
    if (item != nullptr && column == 0 && !m_UpdatingTreeItemText)
    {
        StartRename(item, item->text(0));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotLocalTreeContextMenuRequested(const QPoint& position)
{
    ShowTreeContextMenu(ui.m_LocalTree, position);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotDeviceTreeContextMenuRequested(const QPoint& position)
{
    ShowTreeContextMenu(ui.m_DeviceTree, position);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotSharedScrollValueChanged(int value)
{
    if (!m_UpdatingScrollBars)
    {
        m_UpdatingScrollBars = true;
        ui.m_LocalTree->verticalScrollBar()->setValue(value);
        ui.m_DeviceTree->verticalScrollBar()->setValue(value);
        m_UpdatingScrollBars = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotTreeScrollValueChanged(int value)
{
    if (!m_UpdatingScrollBars)
    {
        m_UpdatingScrollBars = true;
        ui.m_SyncScrollBar->setValue(value);
        ui.m_LocalTree->verticalScrollBar()->setValue(value);
        ui.m_DeviceTree->verticalScrollBar()->setValue(value);
        m_UpdatingScrollBars = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotTreeScrollRangeChanged(int minimum, int maximum)
{
    Q_UNUSED(minimum);
    Q_UNUSED(maximum);
    m_ScrollUpdateTimer.start();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SlotMainStateChanged(MainState state)
{
    if (state == MainState::ConnectedApplication)
    {
        m_SessionID = -1;
        m_SessionRequestPending = true;
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
    }
    else
    {
        m_SessionID = -1;
        m_SessionRequestPending = false;
        m_RefreshAfterSession = false;
        m_RefreshAfterSessionReadDevice = true;
        m_RefreshPending = false;
        m_RefreshPendingReadDevice = false;
        m_DebouncedRefreshReadDevice = false;
        m_RefreshDebounceTimer.stop();
        m_ContentCompareQueue.clear();
        if (m_State != State::Idle && m_State != State::DiffToolRunning)
        {
            SetActiveState(nullptr);
            m_CurrentFileReader.reset();
            SetState(State::Idle);
            m_CurrentCompareLocalFile.close();
            m_CurrentDiffRemoteFile.close();
            m_CurrentCompareRelativePath.clear();
            CleanupDiffState();
        }
    }
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleOpenSessionReply(const SerialProtocol::OpenSessionReply& packet)
{
    if (packet.m_ClientToken == m_ClientToken)
    {
        m_SessionID = packet.m_SessionID;
        m_SessionRequestPending = false;
        if (m_State == State::OpeningSession && m_RefreshAfterSession)
        {
            const bool readDevice = m_RefreshAfterSessionReadDevice;
            SetState(State::Idle);
            m_RefreshAfterSession = false;
            m_RefreshAfterSessionReadDevice = true;
            StartRefresh(readDevice);
        }
        UpdateControls();
        UpdateStatusLabel();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet)
{
    if (packet.m_SessionID == m_SessionID && packet.m_Status == SerialProtocol::FilesystemError::UnknownSession)
    {
        m_SessionID = -1;
        m_SessionRequestPending = false;
        if (m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication)
        {
            if (!m_SessionRequestPending)
            {
                m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
                m_SessionRequestPending = true;
            }
            const bool readDevice = m_State == State::CrawlingRemote;
            SetState(State::OpeningSession);
            m_RefreshAfterSession = true;
            m_RefreshAfterSessionReadDevice = readDevice;
            UpdateControls();
            UpdateStatusLabel();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleStateCompleted(SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
{
    if (&state != m_ActiveState.get()) {
        return;
    }

    if (status == SDCardSyncState::CompletionStatus::Failed && !statusText.isEmpty()) {
        m_StatusOverride = statusText;
    }

    m_StateTotalProgress.reset();
    m_StateSubProgress.reset();
    m_StateProgressText.clear();
    m_StateStatusText.clear();

    SetState(State::Idle);
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleStateProgress(SDCardSyncState& state, std::optional<float> totalProgress, std::optional<float> subProgress, const QString& progressText)
{
    if (&state == m_ActiveState.get()) {
        ApplyStateProgress(totalProgress, subProgress, progressText);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleStateStatusText(SDCardSyncState& state, const QString& statusText)
{
    if (&state == m_ActiveState.get())
    {
        m_StateStatusText = statusText;
        UpdateStatusLabel();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LoadSettings()
{
    QSettings settings;

    const int pathCount = settings.beginReadArray("SDCardSync/LocalRootList");
    for (int itemIndex = 0; itemIndex < pathCount; ++itemIndex)
    {
        settings.setArrayIndex(itemIndex);
        const QString path = settings.value("Path", "").toString();
        if (!path.isEmpty())
        {
            ui.m_LocalRootCombo->addItem(path);
        }
    }
    settings.endArray();

    if (pathCount > 0)
    {
        const int selectedIndex = std::max(0, std::min(pathCount - 1, settings.value("SDCardSync/LocalRootList/Selection", 0).toInt()));
        ui.m_LocalRootCombo->setCurrentIndex(selectedIndex);
    }

    ui.m_HideIdenticalCheckBox->setChecked(settings.value("SDCardSync/HideIdentical", true).toBool());
    ui.m_HideDeviceOnlyCheckBox->setChecked(settings.value("SDCardSync/HideDeviceOnly", true).toBool());
    ui.m_CompareContentCheckBox->setChecked(settings.value("SDCardSync/CompareContent", false).toBool());
    ui.m_SyncDeletesCheckBox->setChecked(settings.value("SDCardSync/SyncDeletes", false).toBool());
    m_LastDownloadPath = settings.value("SDCardSync/LastDownloadPath", settings.value("SDCardSync/LastContextDownloadPath", QDir::homePath())).toString();
    m_LastUploadPath = settings.value("SDCardSync/LastUploadPath", settings.value("SDCardSync/LastContextUploadPath", QDir::homePath())).toString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LoadDeleteSyncRules()
{
    m_DeleteSyncRules.Load(m_LocalRootPath.isEmpty() ? QString() : QDir(m_LocalRootPath).absoluteFilePath(DELETE_RULES_FILE_NAME));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::SaveDeleteSyncRules()
{
    bool result = false;
    if (!m_LocalRootPath.isEmpty())
    {
        result = m_DeleteSyncRules.Save(QDir(m_LocalRootPath).absoluteFilePath(DELETE_RULES_FILE_NAME));
        if (!result && m_DeviceSession != nullptr)
        {
            m_DeviceSession->AddLogMessage(PLogSeverity::ERROR, "SD-card sync: failed to update .deleterules");
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetDeleteSyncRule(const std::vector<QString>& relativePaths, DeleteSyncRuleMode mode)
{
    if (!m_LocalRootPath.isEmpty())
    {
        for (const QString& relativePath : relativePaths)
        {
            const SyncNode* node = FindNode(relativePath);
            if (node != nullptr && IsDirectoryNode(*node))
            {
                m_DeleteSyncRules.SetRule(relativePath, mode);
            }
        }

        if (SaveDeleteSyncRules())
        {
            RebuildTrees();
            UpdateControls();
            UpdateStatusLabel();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::AddRecentLocalRoot(const QString& path)
{
    const QSignalBlocker signalBlocker(ui.m_LocalRootCombo);
    const int existingIndex = ui.m_LocalRootCombo->findText(path);
    if (existingIndex >= 0)
    {
        ui.m_LocalRootCombo->removeItem(existingIndex);
    }
    ui.m_LocalRootCombo->insertItem(0, path);
    while (ui.m_LocalRootCombo->count() > MAX_RECENT_LOCAL_ROOTS)
    {
        ui.m_LocalRootCombo->removeItem(ui.m_LocalRootCombo->count() - 1);
    }
    ui.m_LocalRootCombo->setCurrentIndex(0);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetLocalRoot(const QString& path)
{
    QString normalizedPath = QDir::fromNativeSeparators(path.trimmed());
    if (!normalizedPath.isEmpty())
    {
        QDir directory(normalizedPath);
        const QString canonicalPath = directory.canonicalPath();
        normalizedPath = canonicalPath.isEmpty() ? QDir::cleanPath(directory.absolutePath()) : canonicalPath;
    }

    if (normalizedPath != m_LocalRootPath)
    {
        m_LocalRootPath = normalizedPath;
        m_LocalScanner.SetLocalRootPath(m_LocalRootPath, QString::fromUtf8(SYNC_IGNORE_FILE_NAME), QStringList({
            QString::fromUtf8(SYNC_IGNORE_FILE_NAME),
            QString::fromUtf8(DELETE_RULES_FILE_NAME)
        }));
        LoadDeleteSyncRules();
        if (!m_LocalRootPath.isEmpty())
        {
            AddRecentLocalRoot(m_LocalRootPath);
        }
        StartRefresh();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartRefresh(bool readDevice)
{
    if (m_State != State::Idle)
    {
        m_RefreshPending = true;
        m_RefreshPendingReadDevice = m_RefreshPendingReadDevice || readDevice;
        return;
    }

    m_RefreshDebounceTimer.stop();
    m_LocalScanner.BeginRefresh(readDevice);
    LoadDeleteSyncRules();
    m_DebouncedRefreshReadDevice = false;
    m_RefreshPending = false;
    m_RefreshPendingReadDevice = false;
    m_StatusOverride.clear();
    ResetProgressCounters();
    m_ContentCompareQueue.clear();
    SetActiveState(nullptr);
    m_CurrentFileReader.reset();
    m_CurrentCompareLocalFile.close();
    if (readDevice)
    {
        m_DeviceReadComplete = false;
        m_Model.Clear();
    }
    else
    {
        m_LocalScanner.PrepareModelForLocalRefresh(m_Model);
    }

    m_LocalScanner.ScanLocalTree(m_Model);

    if (!readDevice)
    {
        if (m_LocalScanner.IsTargetedLocalRefresh())
        {
            m_LocalScanner.ClearMissingLocalInfo(m_Model);
        }
        m_LocalScanner.ApplyIgnoreRulesToRemoteNodes(m_Model);
        RemoveEmptyNodes();
        if (m_LocalScanner.IsTargetedLocalRefresh())
        {
            const std::set<QString>& localChangedPaths = m_LocalScanner.GetLocalChangedPaths();
            RecalculateSyncStatus(localChangedPaths);
            CollectContentCompareFiles(localChangedPaths);
        }
        else
        {
            RecalculateSyncStatus();
            CollectContentCompareFiles();
        }

        const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication;
        if (!m_ContentCompareQueue.empty() && connected)
        {
            if (m_SessionID < 0)
            {
                if (!m_SessionRequestPending)
                {
                    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
                    m_SessionRequestPending = true;
                }
                SetState(State::OpeningSession);
                m_RefreshAfterSession = true;
                m_RefreshAfterSessionReadDevice = false;
                UpdateControls();
                UpdateStatusLabel();
            }
            else
            {
                StartNextContentCompare();
            }
        }
        else
        {
            FinishRefresh();
        }
        return;
    }

    if (m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication)
    {
        if (m_SessionID < 0)
        {
            if (!m_SessionRequestPending)
            {
                m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
                m_SessionRequestPending = true;
            }
            SetState(State::OpeningSession);
            m_RefreshAfterSession = true;
            m_RefreshAfterSessionReadDevice = true;
            UpdateControls();
            UpdateStatusLabel();
        }
        else
        {
            StartRemoteCrawl();
        }
    }
    else
    {
        RecalculateSyncStatus();
        FinishRefresh();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishRefresh()
{
    SetState(State::Idle);
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();

    if (m_RefreshPending)
    {
        const bool readDevice = m_RefreshPendingReadDevice;
        m_RefreshPending = false;
        m_RefreshPendingReadDevice = false;
        ScheduleRefresh(readDevice);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ScheduleRefresh(bool readDevice)
{
    if (!m_LocalRootPath.isEmpty())
    {
        m_DebouncedRefreshReadDevice = m_DebouncedRefreshReadDevice || readDevice;
        m_RefreshDebounceTimer.start();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ClearLocalInfo(SyncNode& node)
{
    m_Model.ClearLocalInfo(node);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::RemoveEmptyNodes()
{
    m_Model.RemoveEmptyNodes();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSync::SyncNode& SDCardSync::FindOrCreateNode(const QString& relativePath)
{
    return m_Model.FindOrCreateNode(relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSync::SyncNode* SDCardSync::FindNode(const QString& relativePath)
{
    return m_Model.FindNode(relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartRemoteCrawl()
{
    std::unique_ptr<SDStateCrawlingRemote> state = std::make_unique<SDStateCrawlingRemote>(
        CreateStateContext(),
        m_Model,
        m_LocalScanner,
        QString::fromUtf8(DEVICE_ROOT_PATH)
    );

    SetState(State::CrawlingRemote);
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();

    SetActiveState(std::move(state),
        [this](SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
        {
            Q_UNUSED(state);
            Q_UNUSED(statusText);
            if (status == SDCardSyncState::CompletionStatus::Succeeded)
            {
                FinishRemoteCrawl();
            }
        }
    );
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishRemoteCrawl()
{
    m_DeviceReadComplete = true;
    RecalculateSyncStatus();
    CollectContentCompareFiles();

    if (!m_ContentCompareQueue.empty())
    {
        RebuildTrees();
        StartNextContentCompare();
    }
    else
    {
        FinishRefresh();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::AbortRemoteOperation(const QString& statusText)
{
    SetActiveState(nullptr);
    m_ContentCompareQueue.clear();
    m_CurrentFileReader.reset();
    m_CurrentCompareLocalFile.close();
    m_CurrentDiffRemoteFile.close();
    m_StatusOverride = statusText;
    SetState(State::Idle);
    CleanupDiffState();
    RecalculateSyncStatus();
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::RecalculateSyncStatus()
{
    for (auto& nodeEntry : m_Model.GetRootNodes())
    {
        RecalculateSyncStatus(*nodeEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::RecalculateSyncStatus(const std::set<QString>& relativePaths)
{
    if (relativePaths.contains(QString()))
    {
        RecalculateSyncStatus();
    }
    else
    {
        for (const QString& relativePath : relativePaths)
        {
            SyncNode* node = FindNode(relativePath);
            if (node != nullptr)
            {
                RecalculateSyncStatus(*node);
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::RecalculateSyncStatus(SyncNode& node)
{
    for (auto& childEntry : node.Children)
    {
        RecalculateSyncStatus(*childEntry.second);
    }

    if (node.Ignored)
    {
        node.Status = SyncStatus::Ignored;
    }
    else if (node.Local.has_value() && node.Remote.has_value())
    {
        if (node.Local->IsDirectory != node.Remote->IsDirectory)
        {
            node.Status = SyncStatus::TypeConflict;
        }
        else if (node.Local->IsDirectory)
        {
            node.Status = SyncStatus::Identical;
        }
        else if (node.Local->Size != node.Remote->Size || (!ui.m_CompareContentCheckBox->isChecked() && !SDCardSyncFileUtils::FileTimesMatch(node.Local->ModificationTimeNanos, node.Remote->ModificationTimeNanos)))
        {
            qDebug() << "SDCardSync different"
                     << node.RelativePath
                     << "localSize" << node.Local->Size
                     << "remoteSize" << node.Remote->Size
                     << "localTime" << node.Local->ModificationTimeNanos
                     << "remoteTime" << node.Remote->ModificationTimeNanos
                     << "deltaMs" << ((node.Local->ModificationTimeNanos - node.Remote->ModificationTimeNanos) / SDCardSyncFileUtils::NanosecondsPerMillisecond)
                     << "roundedLocal" << SDCardSyncFileUtils::RoundToFATModificationTime(node.Local->ModificationTimeNanos)
                     << "roundedRemote" << SDCardSyncFileUtils::RoundToFATModificationTime(node.Remote->ModificationTimeNanos)
                     << "localDisplay" << SDCardSyncFileUtils::FormatFileTime(node.Local->ModificationTimeNanos)
                     << "remoteDisplay" << SDCardSyncFileUtils::FormatFileTime(node.Remote->ModificationTimeNanos)
                     << "compareContent" << ui.m_CompareContentCheckBox->isChecked();
            node.Status = SyncStatus::Different;
        }
        else
        {
            node.Status = ui.m_CompareContentCheckBox->isChecked() ? SyncStatus::Comparing : SyncStatus::Identical;
        }
    }
    else if (node.Local.has_value())
    {
        node.Status = SyncStatus::LocalOnly;
    }
    else if (node.Remote.has_value())
    {
        node.Status = SyncStatus::RemoteOnly;
    }
    else
    {
        node.Status = SyncStatus::Unknown;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::CollectContentCompareFiles()
{
    m_ContentCompareQueue.clear();
    m_ContentCompareTotalBytes = 0;
    m_ContentCompareCompletedBytes = 0;
    for (auto& nodeEntry : m_Model.GetRootNodes())
    {
        CollectContentCompareFiles(*nodeEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::CollectContentCompareFiles(const std::set<QString>& relativePaths)
{
    m_ContentCompareQueue.clear();
    m_ContentCompareTotalBytes = 0;
    m_ContentCompareCompletedBytes = 0;
    if (relativePaths.contains(QString()))
    {
        for (auto& nodeEntry : m_Model.GetRootNodes())
        {
            CollectContentCompareFiles(*nodeEntry.second);
        }
    }
    else
    {
        for (const QString& relativePath : relativePaths)
        {
            SyncNode* node = FindNode(relativePath);
            if (node != nullptr)
            {
                CollectContentCompareFiles(*node);
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::CollectContentCompareFiles(SyncNode& node)
{
    if (node.Status == SyncStatus::Comparing && node.Local.has_value() && !node.Local->IsDirectory)
    {
        m_ContentCompareQueue.push_back(node.RelativePath);
        m_ContentCompareTotalBytes += std::max<int64_t>(1, node.Local->Size);
    }
    for (auto& childEntry : node.Children)
    {
        CollectContentCompareFiles(*childEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartNextContentCompare()
{
    if (m_ContentCompareQueue.empty())
    {
        FinishRefresh();
        return;
    }

    const QString relativePath = m_ContentCompareQueue.back();
    m_ContentCompareQueue.pop_back();
    SyncNode* node = FindNode(relativePath);
    if (node != nullptr)
    {
        StartContentCompare(*node);
    }
    else
    {
        StartNextContentCompare();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartContentCompare(const SyncNode& node)
{
    const int64_t localSize = node.Local.has_value() ? node.Local->Size : 0;
    m_CurrentCompareRelativePath = node.RelativePath;
    m_CurrentCompareLocalFile.setFileName(GetLocalPath(m_CurrentCompareRelativePath));
    m_CurrentCompareIdentical = true;
    m_CurrentCompareSize = std::max<int64_t>(1, localSize);
    m_CurrentCompareBytesRead = 0;

    if (!m_CurrentCompareLocalFile.open(QFile::ReadOnly))
    {
        FinishCurrentContentCompare(false);
        return;
    }

    const QByteArray utf8Path = GetRemotePath(m_CurrentCompareRelativePath).toUtf8();
    if (utf8Path.size() >= SDCardSyncFileUtils::MaxFilesystemPathBytes)
    {
        FinishCurrentContentCompare(false);
        return;
    }

    m_CurrentFileReader = std::make_unique<SerialFileReader>(m_DeviceSession->GetSerialHandler(), m_SessionID);
    m_CurrentFileReader->SetDataReadyHandler([this](int64_t startPos, const char* data, int32_t size)
    {
        return HandleContentCompareData(startPos, data, size);
    });
    m_CurrentFileReader->SetFinishedHandler([this](SerialFileReader::Result result)
    {
        m_CurrentFileReader.reset();
        if (result != SerialFileReader::Result::OK && result != SerialFileReader::Result::DataHandlerFailed)
        {
            m_StatusOverride = QString("Failed to compare file content: %1").arg(GetSerialFileReaderResultText(result));
        }
        FinishCurrentContentCompare(result == SerialFileReader::Result::OK && m_CurrentCompareIdentical);
    });
    SetState(State::ComparingContentReadingFile);
    UpdateControls();
    UpdateStatusLabel();
    const SerialFileReader::Result startResult = m_CurrentFileReader->Start(GetRemotePath(m_CurrentCompareRelativePath), localSize);
    if (startResult != SerialFileReader::Result::OK)
    {
        m_CurrentFileReader.reset();
        m_StatusOverride = QString("Failed to compare file content: %1").arg(GetSerialFileReaderResultText(startResult));
        FinishCurrentContentCompare(false);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::DataResult SDCardSync::HandleContentCompareData(int64_t startPos, const char* data, int32_t size)
{
    SerialFileReader::DataResult result = SerialFileReader::DataResult::Continue;
    m_CurrentCompareBytesRead = std::max(m_CurrentCompareBytesRead, startPos + size);
    UpdateProgressBarValues();
    if (m_CurrentCompareIdentical)
    {
        if (m_CurrentCompareLocalFile.seek(startPos))
        {
            const QByteArray localData = m_CurrentCompareLocalFile.read(size);
            if (localData.size() == size)
            {
                m_CurrentCompareIdentical = memcmp(localData.constData(), data, size) == 0;
                if (!m_CurrentCompareIdentical)
                {
                    result = SerialFileReader::DataResult::Stop;
                }
            }
            else
            {
                m_CurrentCompareIdentical = false;
                result = SerialFileReader::DataResult::Stop;
            }
        }
        else
        {
            m_CurrentCompareIdentical = false;
            result = SerialFileReader::DataResult::Stop;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishCurrentContentCompare(bool identical)
{
    m_CurrentCompareLocalFile.close();

    m_ContentCompareCompletedBytes += m_CurrentCompareSize;
    m_CurrentCompareBytesRead = 0;
    m_CurrentCompareSize = 0;

    SyncNode* node = FindNode(m_CurrentCompareRelativePath);
    if (node != nullptr)
    {
        node->Status = identical ? SyncStatus::Identical : SyncStatus::Different;
    }
    m_CurrentCompareRelativePath.clear();
    SetState(State::Idle);
    UpdateProgressBarValues();
    StartNextContentCompare();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDiff(const SyncNode& node)
{
    if (!CanDiffNode(node))
    {
        return;
    }

    m_StatusOverride.clear();
    CleanupDiffState();

    const QString& directoryName = m_DeviceSession->GetOptions().TemporaryDirectoryName;
    const QString directoryTemplate = QDir(QDir::tempPath()).filePath(directoryName + "_SDCardDiff_XXXXXX");
    m_DiffTempDirectory = std::make_unique<QTemporaryDir>(directoryTemplate);
    if (!m_DiffTempDirectory->isValid())
    {
        QMessageBox::warning(this, "Diff", "Failed to create temporary diff folder.");
        return;
    }

    m_CurrentDiffRelativePath = node.RelativePath;
    m_CurrentDiffLocalPath = GetLocalPath(node.RelativePath);
    m_CurrentDiffRemoteTempPath = m_DiffTempDirectory->path() + "/device/" + node.RelativePath;
    m_CurrentDiffOutputPath = m_DiffTempDirectory->path() + "/output/" + node.RelativePath;
    m_CurrentDiffDownloadError.clear();
    m_CurrentDiffSize = node.Remote.has_value() ? std::max<int64_t>(1, node.Remote->Size) : 0;
    m_CurrentDiffBytesRead = 0;

    QDir().mkpath(QFileInfo(m_CurrentDiffRemoteTempPath).path());
    QDir().mkpath(QFileInfo(m_CurrentDiffOutputPath).path());
    m_CurrentDiffRemoteFile.setFileName(m_CurrentDiffRemoteTempPath);
    if (!m_CurrentDiffRemoteFile.open(QFile::WriteOnly | QFile::Truncate))
    {
        QMessageBox::warning(this, "Diff", QString("Failed to create temporary file '%1'.").arg(m_CurrentDiffRemoteTempPath));
        CleanupDiffState();
        return;
    }

    const QByteArray utf8Path = GetRemotePath(m_CurrentDiffRelativePath).toUtf8();
    if (utf8Path.size() >= SDCardSyncFileUtils::MaxFilesystemPathBytes)
    {
        FinishDiffDownload(SerialFileReader::Result::PathTooLong);
        return;
    }

    m_CurrentFileReader = std::make_unique<SerialFileReader>(m_DeviceSession->GetSerialHandler(), m_SessionID);
    m_CurrentFileReader->SetDataReadyHandler([this](int64_t startPos, const char* data, int32_t size)
    {
        return HandleDiffDownloadData(startPos, data, size);
    });
    m_CurrentFileReader->SetFinishedHandler([this](SerialFileReader::Result result)
    {
        m_CurrentFileReader.reset();
        FinishDiffDownload(result);
    });
    SetState(State::DiffReadingFile);
    UpdateControls();
    UpdateStatusLabel();
    const SerialFileReader::Result startResult = m_CurrentFileReader->Start(GetRemotePath(m_CurrentDiffRelativePath));
    if (startResult != SerialFileReader::Result::OK)
    {
        m_CurrentFileReader.reset();
        FinishDiffDownload(startResult);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::DataResult SDCardSync::HandleDiffDownloadData(int64_t startPos, const char* data, int32_t size)
{
    SerialFileReader::DataResult result = SerialFileReader::DataResult::Error;
    m_CurrentDiffBytesRead = std::max(m_CurrentDiffBytesRead, startPos + size);
    UpdateProgressBarValues();
    if (m_CurrentDiffRemoteFile.seek(startPos))
    {
        const qint64 bytesWritten = m_CurrentDiffRemoteFile.write(data, size);
        if (bytesWritten == size)
        {
            result = SerialFileReader::DataResult::Continue;
        }
        else
        {
            m_CurrentDiffDownloadError = QString("failed to write temporary file: %1").arg(m_CurrentDiffRemoteFile.errorString());
        }
    }
    else
    {
        m_CurrentDiffDownloadError = QString("failed to seek temporary file: %1").arg(m_CurrentDiffRemoteFile.errorString());
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishDiffDownload(SerialFileReader::Result result)
{
    m_CurrentDiffRemoteFile.close();

    if (result == SerialFileReader::Result::OK)
    {
        StartDiffTool();
    }
    else
    {
        m_StatusOverride = m_CurrentDiffDownloadError.isEmpty()
            ? QString("Failed to download remote file for diff: %1").arg(GetSerialFileReaderResultText(result))
            : QString("Failed to download remote file for diff: %1").arg(m_CurrentDiffDownloadError);
        FinishDiffOperation();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDiffTool()
{
    m_CurrentDiffOriginalRemoteHash = SDCardSyncFileUtils::CalculateFileHash(m_CurrentDiffRemoteTempPath);
    if (m_CurrentDiffOriginalRemoteHash.isEmpty())
    {
        m_StatusOverride = "Failed to read downloaded remote file";
        FinishDiffOperation();
        return;
    }

    QFile::remove(m_CurrentDiffOutputPath);
    QFile remoteTempFile(m_CurrentDiffRemoteTempPath);
    if (!remoteTempFile.copy(m_CurrentDiffOutputPath))
    {
        QMessageBox::warning(this, "Diff", QString("Failed to create editable diff output file '%1': %2").arg(m_CurrentDiffOutputPath).arg(remoteTempFile.errorString()));
        m_StatusOverride = "Failed to create editable diff output file";
        FinishDiffOperation();
        return;
    }
    m_CurrentDiffOutputInitialModificationTimeNanos = QFileInfo(m_CurrentDiffOutputPath).fileTime(QFileDevice::FileModificationTime).toUTC().toMSecsSinceEpoch() * SDCardSyncFileUtils::NanosecondsPerMillisecond;

    m_DiffProcess = new QProcess(this);
    connect(m_DiffProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int exitCode, QProcess::ExitStatus exitStatus)
    {
        HandleDiffProcessFinished(exitCode, exitStatus);
    });

    const QString remoteLabel = GetRemotePath(m_CurrentDiffRelativePath);
    QStringList arguments;
    arguments << "/s-";
    arguments << "/e";
    arguments << "/u";
    arguments << "/wl";
    arguments << "/dl";
    arguments << "Local";
    arguments << "/dr";
    arguments << remoteLabel;
    arguments << QDir::toNativeSeparators(m_CurrentDiffLocalPath);
    arguments << QDir::toNativeSeparators(m_CurrentDiffOutputPath);

    const QString diffToolProgram = FindDiffToolProgram();
    if (diffToolProgram.isEmpty())
    {
        QMessageBox::warning(this, "Diff", QString("Failed to find WinMerge. Checked PATH, common install folders, and '%1'.").arg(QString::fromUtf8(WINMERGE_START_MENU_SHORTCUT)));
        m_StatusOverride = "Failed to find WinMerge";
        FinishDiffOperation();
        return;
    }

    qDebug() << "SDCardSync diff start"
             << "tool" << diffToolProgram
             << "arguments" << arguments
             << "remoteTemp" << m_CurrentDiffRemoteTempPath
             << "output" << m_CurrentDiffOutputPath
             << "originalHash" << m_CurrentDiffOriginalRemoteHash.toHex();

    SetState(State::DiffToolRunning);
    UpdateControls();
    UpdateStatusLabel();

    m_DiffProcess->start(diffToolProgram, arguments);
    if (!m_DiffProcess->waitForStarted(3000))
    {
        const QString errorText = m_DiffProcess->errorString();
        m_DiffProcess->deleteLater();
        m_DiffProcess = nullptr;
        QMessageBox::warning(this, "Diff", QString("Failed to start WinMerge: %1").arg(errorText));
        m_StatusOverride = "Failed to start WinMerge";
        FinishDiffOperation();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleDiffProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_DiffProcess != nullptr)
    {
        m_DiffProcess->deleteLater();
        m_DiffProcess = nullptr;
    }

    const QByteArray currentRemoteHash = SDCardSyncFileUtils::CalculateFileHash(m_CurrentDiffRemoteTempPath);
    const QFileInfo currentRemoteFileInfo(m_CurrentDiffRemoteTempPath);
    const QFileInfo outputFileInfo(m_CurrentDiffOutputPath);
    const QByteArray outputHash = outputFileInfo.exists() ? SDCardSyncFileUtils::CalculateFileHash(m_CurrentDiffOutputPath) : QByteArray();
    const int64_t outputModificationTimeNanos = outputFileInfo.exists() ? outputFileInfo.fileTime(QFileDevice::FileModificationTime).toUTC().toMSecsSinceEpoch() * SDCardSyncFileUtils::NanosecondsPerMillisecond : 0;
    const bool outputWasTouched = outputModificationTimeNanos != 0 && outputModificationTimeNanos != m_CurrentDiffOutputInitialModificationTimeNanos;
    qDebug() << "SDCardSync diff finished"
             << "path" << m_CurrentDiffRelativePath
             << "exitCode" << exitCode
             << "exitStatus" << exitStatus
             << "tempPath" << m_CurrentDiffRemoteTempPath
             << "originalHash" << m_CurrentDiffOriginalRemoteHash.toHex()
             << "currentHash" << currentRemoteHash.toHex()
             << "size" << currentRemoteFileInfo.size()
             << "mtime" << currentRemoteFileInfo.fileTime(QFileDevice::FileModificationTime).toString(Qt::ISODateWithMs)
             << "outputPath" << m_CurrentDiffOutputPath
             << "outputHash" << outputHash.toHex()
             << "outputSize" << outputFileInfo.size()
             << "outputMtime" << outputFileInfo.fileTime(QFileDevice::FileModificationTime).toString(Qt::ISODateWithMs)
             << "outputInitialTime" << m_CurrentDiffOutputInitialModificationTimeNanos
             << "outputWasTouched" << outputWasTouched;

    if (currentRemoteHash.isEmpty())
    {
        QMessageBox::warning(this, "Diff", "Failed to read the edited remote temp file.");
        m_StatusOverride = "Failed to read edited remote temp file";
        FinishDiffOperation();
        return;
    }

    m_CurrentDiffUpdateSourcePath.clear();
    QString updateQuestionText;
    if (!outputHash.isEmpty() && outputHash != m_CurrentDiffOriginalRemoteHash)
    {
        m_CurrentDiffUpdateSourcePath = m_CurrentDiffOutputPath;
        updateQuestionText = QString("The device-side file was changed in WinMerge.\n\nUpdate '%1' on the device?");
    }
    else if (currentRemoteHash != m_CurrentDiffOriginalRemoteHash)
    {
        m_CurrentDiffUpdateSourcePath = m_CurrentDiffRemoteTempPath;
        updateQuestionText = QString("The device-side file was changed in WinMerge.\n\nUpdate '%1' on the device?");
    }
    else if (outputWasTouched)
    {
        const QByteArray localHash = SDCardSyncFileUtils::CalculateFileHash(m_CurrentDiffLocalPath);
        if (!localHash.isEmpty() && localHash != m_CurrentDiffOriginalRemoteHash)
        {
            m_CurrentDiffUpdateSourcePath = m_CurrentDiffLocalPath;
            updateQuestionText = QString("WinMerge saved the target file without changing its bytes.\n\nThis can happen for line-ending-only merges. Update '%1' on the device with the local file instead?");
        }
    }

    if (!m_CurrentDiffUpdateSourcePath.isEmpty())
    {
        const QString remotePath = GetRemotePath(m_CurrentDiffRelativePath);
        const QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            "Update device file?",
            updateQuestionText.arg(remotePath),
            QMessageBox::StandardButtons(QMessageBox::Yes | QMessageBox::No)
        );
        if (reply == QMessageBox::Yes)
        {
            StartDiffRemoteUpdate();
            return;
        }
    }
    FinishDiffOperation();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDiffRemoteUpdate()
{
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    if (!connected)
    {
        QMessageBox::warning(this, "Diff", "The device is no longer connected.");
        m_StatusOverride = "Device disconnected before diff update";
        FinishDiffOperation();
        return;
    }

    const QString updateSourcePath = m_CurrentDiffUpdateSourcePath.isEmpty() ? m_CurrentDiffRemoteTempPath : m_CurrentDiffUpdateSourcePath;
    const QFileInfo remoteFileInfo(updateSourcePath);
    if (!remoteFileInfo.exists() || !remoteFileInfo.isFile())
    {
        QMessageBox::warning(this, "Diff", "The edited remote temp file no longer exists.");
        m_StatusOverride = "Edited remote temp file is missing";
        FinishDiffOperation();
        return;
    }

    m_CleanupDiffTempAfterUpload = true;
    SetState(State::Idle);
    StartUploadQueue(std::make_unique<SDStateUploadFiles>(
        CreateStateContext(),
        updateSourcePath,
        m_CurrentDiffRelativePath,
        QString::fromUtf8(DEVICE_ROOT_PATH),
        [this](const UploadDirectory& uploadDirectory)
        {
            ApplyCompletedUploadDirectory(uploadDirectory);
        },
        [this](const UploadFile& uploadFile)
        {
            ApplyCompletedUploadFile(uploadFile);
        }
    ));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishDiffOperation()
{
    SetState(State::Idle);
    CleanupDiffState();
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartTimestampUpdate(const std::vector<QString>& relativePaths, QTreeWidget* targetTree, bool requireIdentical)
{
    if (m_State != State::Idle)
    {
        return;
    }

    if (targetTree != ui.m_LocalTree && targetTree != ui.m_DeviceTree)
    {
        return;
    }

    m_StatusOverride.clear();
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    const bool updateLocalTimestamp = targetTree == ui.m_LocalTree;

    SetState(State::UpdatingRemoteTimestamp);
    UpdateControls();
    UpdateStatusLabel();
    SetActiveState(std::make_unique<SDStateTimestampUpdate>(
        CreateStateContext(),
        m_Model,
        m_LocalRootPath,
        QString::fromUtf8(DEVICE_ROOT_PATH),
        relativePaths,
        updateLocalTimestamp,
        requireIdentical,
        connected,
        [this](const QString& relativePath, bool updatedLocalTimestamp, int64_t timestampNanos)
        {
            ApplyCompletedTimestampUpdate(relativePath, updatedLocalTimestamp, timestampNanos);
        },
        [this](PLogSeverity severity, const QString& relativePath, const QString& resultText)
        {
            LogTimestampUpdateResult(severity, relativePath, resultText);
        },
        [this](const QString& statusText)
        {
            m_StatusOverride = statusText;
        }
    ), [this](SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
    {
        Q_UNUSED(state);
        Q_UNUSED(status);
        Q_UNUSED(statusText);
        RebuildTrees();
        UpdateControls();
        UpdateStatusLabel();
        if (m_RefreshPending)
        {
            const bool readDevice = m_RefreshPendingReadDevice;
            m_RefreshPending = false;
            m_RefreshPendingReadDevice = false;
            ScheduleRefresh(readDevice);
        }
    });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedTimestampUpdate(const QString& relativePath, bool updatedLocalTimestamp, int64_t timestampNanos)
{
    SyncNode* node = FindNode(relativePath);
    if (node == nullptr)
    {
        return;
    }
    if (updatedLocalTimestamp)
    {
        if (node->Local.has_value())
        {
            node->Local = SDCardSyncFileUtils::BuildLocalFileInfo(QFileInfo(GetLocalPath(relativePath)));
            RecalculateSyncStatus(*node);
        }
    }
    else if (node->Remote.has_value())
    {
        node->Remote->ModificationTimeNanos = timestampNanos;
        RecalculateSyncStatus(*node);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartContentUpdate(const std::vector<QString>& relativePaths, QTreeWidget* targetTree)
{
    if (m_State != State::Idle)
    {
        return;
    }

    if (targetTree == ui.m_DeviceTree)
    {
        m_StatusOverride.clear();
        StartUploadQueue(std::make_unique<SDStateUploadFiles>(
            CreateStateContext(),
            m_Model,
            m_LocalRootPath,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            std::set<QString>(relativePaths.begin(), relativePaths.end()),
            [this](const UploadDirectory& uploadDirectory)
            {
                ApplyCompletedUploadDirectory(uploadDirectory);
            },
            [this](const UploadFile& uploadFile)
            {
                ApplyCompletedUploadFile(uploadFile);
            }
        ));
    }
    else if (targetTree == ui.m_LocalTree)
    {
        m_StatusOverride.clear();
        StartDownloadQueue(std::make_unique<SDStateDownloadFiles>(
            CreateStateContext(),
            m_Model,
            m_LocalRootPath,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            std::set<QString>(relativePaths.begin(), relativePaths.end()),
            true,
            [this](const DownloadDirectory& downloadDirectory)
            {
                ApplyCompletedDownloadDirectory(downloadDirectory);
            },
            [this](const DownloadFile& downloadFile)
            {
                ApplyCompletedDownloadFile(downloadFile);
            },
            [this](PLogSeverity severity, const QString& relativePath, const QString& resultText)
            {
                LogContentUpdateResult(severity, relativePath, resultText);
            },
            [this](bool applyToModel)
            {
                HandleDownloadFailure(applyToModel);
            }
        ));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDownloadToFolder(const std::vector<QString>& relativePaths)
{
    if (m_State != State::Idle)
    {
        return;
    }

    const QString targetPath = QFileDialog::getExistingDirectory(this, "Download to folder", m_LastDownloadPath);
    if (!targetPath.isEmpty())
    {
        m_LastDownloadPath = targetPath;
        m_StatusOverride.clear();
        StartDownloadQueue(std::make_unique<SDStateDownloadFiles>(
            CreateStateContext(),
            m_Model,
            targetPath,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            std::set<QString>(relativePaths.begin(), relativePaths.end()),
            false,
            [this](const DownloadDirectory& downloadDirectory)
            {
                ApplyCompletedDownloadDirectory(downloadDirectory);
            },
            [this](const DownloadFile& downloadFile)
            {
                ApplyCompletedDownloadFile(downloadFile);
            },
            [this](PLogSeverity severity, const QString& relativePath, const QString& resultText)
            {
                LogContentUpdateResult(severity, relativePath, resultText);
            },
            [this](bool applyToModel)
            {
                HandleDownloadFailure(applyToModel);
            }
        ));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartUploadFiles(const SyncNode& clickedNode)
{
    if (m_State != State::Idle)
    {
        return;
    }

    const QStringList localPaths = SelectUploadFiles();
    if (!localPaths.isEmpty())
    {
        m_StatusOverride.clear();
        StartUploadQueue(std::make_unique<SDStateUploadFiles>(
            CreateStateContext(),
            localPaths,
            GetUploadTargetFolder(clickedNode),
            QString::fromUtf8(DEVICE_ROOT_PATH),
            [this](const UploadDirectory& uploadDirectory)
            {
                ApplyCompletedUploadDirectory(uploadDirectory);
            },
            [this](const UploadFile& uploadFile)
            {
                ApplyCompletedUploadFile(uploadFile);
            }
        ));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartUploadFolder(const SyncNode& clickedNode)
{
    if (m_State != State::Idle)
    {
        return;
    }

    const QStringList localPaths = SelectUploadFolder();
    if (!localPaths.isEmpty())
    {
        m_StatusOverride.clear();
        StartUploadQueue(std::make_unique<SDStateUploadFiles>(
            CreateStateContext(),
            localPaths,
            GetUploadTargetFolder(clickedNode),
            QString::fromUtf8(DEVICE_ROOT_PATH),
            [this](const UploadDirectory& uploadDirectory)
            {
                ApplyCompletedUploadDirectory(uploadDirectory);
            },
            [this](const UploadFile& uploadFile)
            {
                ApplyCompletedUploadFile(uploadFile);
            }
        ));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDownloadQueue(std::unique_ptr<SDCardSyncState> state)
{
    if (state != nullptr)
    {
        m_UpdateFailedCount = 0;
        SetState(State::UpdatingLocalContentReadingFile);
        UpdateControls();
        UpdateStatusLabel();

        SetActiveState(std::move(state),
            [this](SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
            {
                Q_UNUSED(state);
                Q_UNUSED(statusText);
                if (status == SDCardSyncState::CompletionStatus::Succeeded)
                {
                    RebuildTrees();
                    UpdateControls();
                    UpdateStatusLabel();
                    if (m_RefreshPending)
                    {
                        const bool readDevice = m_RefreshPendingReadDevice;
                        m_RefreshPending = false;
                        m_RefreshPendingReadDevice = false;
                        ScheduleRefresh(readDevice);
                    }
                }
            }
        );
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedDownloadDirectory(const DownloadDirectory& downloadDirectory)
{
    if (downloadDirectory.ApplyToModel)
    {
        SyncNode& node = FindOrCreateNode(downloadDirectory.RelativePath);
        node.Local = SDCardSyncFileUtils::BuildLocalFileInfo(QFileInfo(downloadDirectory.LocalPath));
        RecalculateSyncStatus(node);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedDownloadFile(const DownloadFile& downloadFile)
{
    if (downloadFile.ApplyToModel)
    {
        SyncNode* node = FindNode(downloadFile.RelativePath);
        if (node != nullptr && !downloadFile.RemoteInfo.IsDirectory)
        {
            node->Local = downloadFile.RemoteInfo;
            node->Status = SyncStatus::Identical;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::HandleDownloadFailure(bool applyToModel)
{
    ++m_UpdateFailedCount;
    m_StatusOverride = applyToModel ? "Failed to update local content" : "Failed to download";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ShowDeleteConfirmation(const std::vector<QString>& relativePaths, QTreeWidget* tree)
{
    const bool deleteLocalFromFirstButton = tree == ui.m_LocalTree;
    const QString sideName = deleteLocalFromFirstButton ? "PC" : "Device";
    QString questionText;
    if (relativePaths.size() == 1)
    {
        questionText = QString("Are you sure you want to delete %1?").arg(relativePaths.front());
    }
    else
    {
        questionText = QString("Are you sure you want to delete the %1 selected item(s)?").arg(relativePaths.size());
    }
    const bool hasLocalTargets = HasDeleteTargets(relativePaths, ui.m_LocalTree);
    const bool hasRemoteTargets = HasDeleteTargets(relativePaths, ui.m_DeviceTree);
    const bool hasPairedTargets = HasPairedDeleteTargets(relativePaths);
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;

    QMessageBox messageBox(this);
    messageBox.setWindowTitle("Delete");
    messageBox.setText(questionText);
    QPushButton* sideButton = messageBox.addButton(QString("Delete from %1").arg(sideName), QMessageBox::DestructiveRole);
    QPushButton* bothButton = messageBox.addButton("Delete both", QMessageBox::DestructiveRole);
    QPushButton* cancelButton = messageBox.addButton("Cancel", QMessageBox::RejectRole);
    messageBox.setDefaultButton(cancelButton);
    messageBox.setEscapeButton(cancelButton);

    sideButton->setEnabled(deleteLocalFromFirstButton ? hasLocalTargets : (connected && hasRemoteTargets));
    bothButton->setEnabled(hasPairedTargets && connected);

    messageBox.exec();
    QAbstractButton* clickedButton = messageBox.clickedButton();
    if (clickedButton == sideButton)
    {
        const bool deleteLocal = deleteLocalFromFirstButton;
        const bool deleteRemote = !deleteLocalFromFirstButton;
        StartDelete(relativePaths, deleteLocal, deleteRemote);
    }
    else if (clickedButton == bothButton)
    {
        StartDelete(relativePaths, true, true);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDelete(const std::vector<QString>& relativePaths, bool deleteLocal, bool deleteRemote)
{
    if (m_State != State::Idle)
    {
        return;
    }

    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    const bool hasRemoteTargets = deleteRemote && HasDeleteTargets(relativePaths, ui.m_DeviceTree);
    if (hasRemoteTargets && !connected)
    {
        m_StatusOverride = "Device is not connected";
        UpdateControls();
        UpdateStatusLabel();
        return;
    }

    m_StatusOverride.clear();
    const std::set<QString> selectedPaths(relativePaths.begin(), relativePaths.end());
    StartDeleteQueue(std::make_unique<SDStateDeleteFiles>(
        CreateStateContext(),
        m_Model,
        m_LocalScanner,
        m_LocalRootPath,
        QString::fromUtf8(DEVICE_ROOT_PATH),
        selectedPaths,
        deleteLocal,
        deleteRemote,
        [this](const QString& relativePath)
        {
            ApplyCompletedLocalDelete(relativePath);
        },
        [this](const QString& relativePath)
        {
            ApplyCompletedRemoteDelete(relativePath);
        },
        [this](PLogSeverity severity, const QString& relativePath, const QString& resultText)
        {
            LogDeleteResult(severity, relativePath, resultText);
        },
        [this](const QString& statusText)
        {
            m_StatusOverride = statusText;
        }
    ));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::unique_ptr<SDCardSyncState> SDCardSync::CreateSyncDeleteState()
{
    std::unique_ptr<SDCardSyncState> result;
    if (ui.m_SyncDeletesCheckBox->isChecked() && HasSyncDeleteTargets())
    {
        result = std::make_unique<SDStateDeleteFiles>(
            CreateStateContext(),
            m_Model,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            m_DeleteSyncRules,
            [this](const QString& relativePath)
            {
                ApplyCompletedRemoteDelete(relativePath);
            },
            [this](PLogSeverity severity, const QString& relativePath, const QString& resultText)
            {
                LogDeleteResult(severity, relativePath, resultText);
            },
            [this](const QString& statusText)
            {
                m_StatusOverride = statusText;
            }
        );
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartDeleteQueue(std::unique_ptr<SDCardSyncState> state, StateFinishedDelegate finished)
{
    if (state != nullptr)
    {
        m_UpdateFailedCount = 0;
        SetState(State::DeletingRemote);
        UpdateControls();
        UpdateStatusLabel();

        SetActiveState(std::move(state),
            [this, finished = std::move(finished)](SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
            {
                Q_UNUSED(state);
                Q_UNUSED(statusText);
                if (status == SDCardSyncState::CompletionStatus::Succeeded)
                {
                    if (finished)
                    {
                        finished();
                    }
                    else
                    {
                        FinishUpdateOperation();
                    }
                }
            }
        );
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishUpdateOperation()
{
    SetState(State::Idle);
    RemoveEmptyNodes();
    RecalculateSyncStatus();
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();
    if (m_RefreshPending)
    {
        const bool readDevice = m_RefreshPendingReadDevice;
        m_RefreshPending = false;
        m_RefreshPendingReadDevice = false;
        ScheduleRefresh(readDevice);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedLocalDelete(const QString& relativePath)
{
    SyncNode* node = FindNode(relativePath);
    if (node != nullptr)
    {
        ClearLocalInfo(*node);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedRemoteDelete(const QString& relativePath)
{
    SyncNode* node = FindNode(relativePath);
    if (node != nullptr)
    {
        node->Remote.reset();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedCreateDirectory(const QString& relativePath, const std::optional<FileInfo>& localInfo, const std::optional<FileInfo>& remoteInfo)
{
    SyncNode& node = FindOrCreateNode(relativePath);
    if (localInfo.has_value())
    {
        node.Local = localInfo;
    }
    if (remoteInfo.has_value())
    {
        node.Remote = remoteInfo;
    }
    RecalculateSyncStatus(node);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartRename(QTreeWidgetItem* item, const QString& newName)
{
    auto pathIterator = m_ItemPaths.find(item);
    SyncNode* node = (pathIterator != m_ItemPaths.end()) ? FindNode(pathIterator->second) : nullptr;
    if (node == nullptr)
    {
        return;
    }

    const QString oldName = node->Name;
    const QSignalBlocker treeSignalBlocker(item->treeWidget());
    m_UpdatingTreeItemText = true;
    item->setText(0, oldName);
    m_UpdatingTreeItemText = false;

    if (newName == oldName)
    {
        return;
    }

    if (m_State != State::Idle)
    {
        QMessageBox::warning(this, "Rename", "Wait until the current SD-card sync operation has finished before renaming.");
        return;
    }

    const bool renameLocal = node->Local.has_value();
    const bool renameRemote = node->Remote.has_value();
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    if (renameRemote && !connected)
    {
        QMessageBox::warning(this, "Rename", "The device is not connected.");
        m_StatusOverride = "Device is not connected";
        UpdateControls();
        UpdateStatusLabel();
        return;
    }

    QString newRelativePath;
    QString errorText;
    if (!ValidateRenameTarget(*node, newName, newRelativePath, errorText))
    {
        QMessageBox::warning(this, "Rename", errorText);
        return;
    }

    m_StatusOverride.clear();
    if (renameLocal && !RenameLocalNode(*node, newRelativePath, errorText))
    {
        QMessageBox::warning(this, "Rename", errorText);
        m_StatusOverride = "Failed to rename local file";
        UpdateControls();
        UpdateStatusLabel();
        return;
    }

    if (renameRemote)
    {
        StartRemoteRename(node->RelativePath, newRelativePath, renameLocal);
    }
    else
    {
        const QString oldRelativePath = node->RelativePath;
        if (ApplyCompletedRename(oldRelativePath, newRelativePath, true, false))
        {
            LogRenameResult(PLogSeverity::INFO_LOW_VOL, oldRelativePath, QString("renamed to %1 on PC").arg(newRelativePath));
        }
        else
        {
            m_StatusOverride = "Renamed local file, but failed to update SD-card sync view";
            LogRenameResult(PLogSeverity::ERROR, oldRelativePath, "renamed on PC, but failed to update view");
            UpdateControls();
            UpdateStatusLabel();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::ValidateRenameTarget(const SyncNode& node, const QString& newName, QString& newRelativePath, QString& errorText)
{
    const QString trimmedName = newName.trimmed();
    if (trimmedName.isEmpty())
    {
        errorText = "File name cannot be empty.";
        return false;
    }
    if (trimmedName != newName)
    {
        errorText = "File name cannot begin or end with whitespace.";
        return false;
    }
    if (newName == "." || newName == ".." || newName.contains('/') || newName.contains('\\'))
    {
        errorText = "File name must be a single file or folder name.";
        return false;
    }

    const int separatorIndex = node.RelativePath.lastIndexOf('/');
    const QString parentRelativePath = (separatorIndex >= 0) ? node.RelativePath.left(separatorIndex) : QString();
    newRelativePath = SDCardSyncFileUtils::JoinRelativePath(parentRelativePath, newName);

    const QByteArray utf8RemotePath = GetRemotePath(newRelativePath).toUtf8();
    if (utf8RemotePath.size() >= SDCardSyncFileUtils::MaxFilesystemPathBytes)
    {
        errorText = "Device path is too long.";
        return false;
    }

    SyncNode* targetNode = FindNode(newRelativePath);
    if (targetNode != nullptr && targetNode != &node)
    {
        errorText = "A file or folder with that name already exists in SD-card sync.";
        return false;
    }

    if (node.Local.has_value() && QFileInfo(GetLocalPath(newRelativePath)).exists())
    {
        errorText = "A local file or folder with that name already exists.";
        return false;
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::RenameLocalNode(const SyncNode& node, const QString& newRelativePath, QString& errorText)
{
    const QString oldLocalPath = GetLocalPath(node.RelativePath);
    const QString newLocalPath = GetLocalPath(newRelativePath);
    if (node.Local.has_value() && node.Local->IsDirectory)
    {
        const QFileInfo oldLocalInfo(oldLocalPath);
        const QFileInfo newLocalInfo(newLocalPath);
        const SDCardSyncLocalScanner::WatchPaths watchPaths = m_LocalScanner.RemoveWatchedPathsInTree(oldLocalPath);
        QDir parentDirectory(oldLocalInfo.absolutePath());
        if (parentDirectory.rename(oldLocalInfo.fileName(), newLocalInfo.fileName()))
        {
            m_LocalScanner.RestoreWatchedPaths(watchPaths, oldLocalPath, newLocalPath);
            return true;
        }
        m_LocalScanner.RestoreWatchedPaths(watchPaths, oldLocalPath, oldLocalPath);
        errorText = "Failed to rename local folder.";
        return false;
    }

    QFile localFile(oldLocalPath);
    if (localFile.rename(newLocalPath))
    {
        return true;
    }

    errorText = QString("Failed to rename local file: %1").arg(localFile.errorString());
    return false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartRemoteRename(const QString& oldRelativePath, const QString& newRelativePath, bool localRenamed)
{
    m_CurrentRemoteRenameOldRelativePath = oldRelativePath;
    m_CurrentRemoteRenameNewRelativePath = newRelativePath;
    m_CurrentRemoteRenameLocalRenamed = localRenamed;
    m_CurrentRemoteRenameWatchPaths = localRenamed ? m_LocalScanner.RemoveWatchedPathsInTree(GetLocalPath(newRelativePath)) : SDCardSyncLocalScanner::WatchPaths();

    m_CurrentRenameFile = std::make_unique<SerialRenameFile>(m_DeviceSession->GetSerialHandler(), m_SessionID);
    m_CurrentRenameFile->SetFinishedHandler([this](SerialRenameFile::Result result)
    {
        m_CurrentRenameFile.reset();
        FinishRemoteRename(result == SerialRenameFile::Result::OK);
    });
    SetState(State::RenamingRemote);
    UpdateControls();
    UpdateStatusLabel();
    const SerialRenameFile::Result startResult = m_CurrentRenameFile->Start(GetRemotePath(oldRelativePath), GetRemotePath(newRelativePath));
    if (startResult != SerialRenameFile::Result::OK)
    {
        m_CurrentRenameFile.reset();
        FinishRemoteRename(false);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::FinishRemoteRename(bool success)
{
    const QString oldRelativePath = m_CurrentRemoteRenameOldRelativePath;
    const QString newRelativePath = m_CurrentRemoteRenameNewRelativePath;
    const bool localRenamed = m_CurrentRemoteRenameLocalRenamed;
    const SDCardSyncLocalScanner::WatchPaths watchPaths = m_CurrentRemoteRenameWatchPaths;
    m_CurrentRemoteRenameOldRelativePath.clear();
    m_CurrentRemoteRenameNewRelativePath.clear();
    m_CurrentRemoteRenameLocalRenamed = false;
    m_CurrentRemoteRenameWatchPaths = SDCardSyncLocalScanner::WatchPaths();

    if (success)
    {
        if (localRenamed)
        {
            m_LocalScanner.RestoreWatchedPaths(watchPaths, GetLocalPath(newRelativePath), GetLocalPath(newRelativePath));
        }
        if (ApplyCompletedRename(oldRelativePath, newRelativePath, localRenamed, true))
        {
            LogRenameResult(PLogSeverity::INFO_LOW_VOL, oldRelativePath, QString("renamed to %1").arg(newRelativePath));
        }
        else
        {
            m_StatusOverride = "Renamed device file, but failed to update SD-card sync view";
            LogRenameResult(PLogSeverity::ERROR, oldRelativePath, "renamed on device, but failed to update view");
        }
    }
    else
    {
        m_StatusOverride = "Failed to rename device file";
        LogRenameResult(PLogSeverity::ERROR, oldRelativePath, "failed to rename on device");
        if (localRenamed)
        {
            QString rollbackError;
            if (RollbackLocalRename(oldRelativePath, newRelativePath, rollbackError))
            {
                m_LocalScanner.RestoreWatchedPaths(watchPaths, GetLocalPath(newRelativePath), GetLocalPath(oldRelativePath));
                LogRenameResult(PLogSeverity::INFO_LOW_VOL, newRelativePath, QString("rolled back PC rename to %1").arg(oldRelativePath));
            }
            else
            {
                m_LocalScanner.RestoreWatchedPaths(watchPaths, GetLocalPath(newRelativePath), GetLocalPath(newRelativePath));
                m_StatusOverride = QString("Failed to rename device file and failed to roll back PC rename: %1").arg(rollbackError);
                LogRenameResult(PLogSeverity::ERROR, newRelativePath, QString("failed to roll back PC rename: %1").arg(rollbackError));
            }
        }
    }

    SetState(State::Idle);
    UpdateControls();
    UpdateStatusLabel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::ApplyCompletedRename(const QString& oldRelativePath, const QString& newRelativePath, bool renameLocal, bool renameRemote)
{
    std::map<QString, std::unique_ptr<SyncNode>>* oldSiblings = FindNodeSiblingMap(oldRelativePath);
    std::map<QString, std::unique_ptr<SyncNode>>* newSiblings = FindNodeSiblingMap(newRelativePath);
    if (oldSiblings == nullptr || newSiblings == nullptr)
    {
        return false;
    }

    const QString oldName = QFileInfo(oldRelativePath).fileName();
    const QString newName = QFileInfo(newRelativePath).fileName();
    auto oldIterator = oldSiblings->find(oldName);
    if (oldIterator == oldSiblings->end() || oldIterator->second.get() == nullptr || newSiblings->contains(newName))
    {
        return false;
    }

    std::unique_ptr<SyncNode> renamedNode = std::move(oldIterator->second);
    oldSiblings->erase(oldIterator);
    renamedNode->Name = newName;
    if (renameLocal && renamedNode->Local.has_value())
    {
        renamedNode->Local->Name = newName;
    }
    if (renameRemote && renamedNode->Remote.has_value())
    {
        renamedNode->Remote->Name = newName;
    }
    UpdateRenamedNodePaths(*renamedNode, oldRelativePath, newRelativePath);
    (*newSiblings)[newName] = std::move(renamedNode);
    UpdateRenamedExpandedPaths(oldRelativePath, newRelativePath);
    RebuildTrees();
    UpdateControls();
    UpdateStatusLabel();
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::RollbackLocalRename(const QString& oldRelativePath, const QString& newRelativePath, QString& errorText)
{
    const QString oldLocalPath = GetLocalPath(oldRelativePath);
    const QString newLocalPath = GetLocalPath(newRelativePath);
    const QFileInfo renamedInfo(newLocalPath);

    if (renamedInfo.isDir())
    {
        const QFileInfo oldLocalInfo(oldLocalPath);
        const QFileInfo newLocalInfo(newLocalPath);
        QDir parentDirectory(newLocalInfo.absolutePath());
        if (parentDirectory.rename(newLocalInfo.fileName(), oldLocalInfo.fileName()))
        {
            return true;
        }
        errorText = "failed to rename local folder back";
        return false;
    }

    QFile localFile(newLocalPath);
    if (localFile.rename(oldLocalPath))
    {
        return true;
    }

    errorText = localFile.errorString();
    return false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::map<QString, std::unique_ptr<SDCardSync::SyncNode>>* SDCardSync::FindNodeSiblingMap(const QString& relativePath)
{
    QStringList pathParts = relativePath.split('/', Qt::SkipEmptyParts);
    if (pathParts.empty())
    {
        return nullptr;
    }

    std::map<QString, std::unique_ptr<SyncNode>>* siblings = &m_Model.GetRootNodes();
    for (qsizetype partIndex = 0; partIndex < pathParts.size() - 1; ++partIndex)
    {
        auto nodeIterator = siblings->find(pathParts[partIndex]);
        if (nodeIterator == siblings->end() || nodeIterator->second.get() == nullptr)
        {
            return nullptr;
        }
        siblings = &nodeIterator->second->Children;
    }
    return siblings;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateRenamedNodePaths(SyncNode& node, const QString& oldRelativePath, const QString& newRelativePath)
{
    if (node.RelativePath == oldRelativePath)
    {
        node.RelativePath = newRelativePath;
    }
    else if (node.RelativePath.startsWith(oldRelativePath + "/"))
    {
        node.RelativePath = newRelativePath + node.RelativePath.sliced(oldRelativePath.size());
    }

    for (auto& childEntry : node.Children)
    {
        UpdateRenamedNodePaths(*childEntry.second, oldRelativePath, newRelativePath);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateRenamedExpandedPaths(const QString& oldRelativePath, const QString& newRelativePath)
{
    std::set<QString> updatedPaths;
    for (const QString& expandedPath : m_ExpandedPaths)
    {
        if (expandedPath == oldRelativePath)
        {
            updatedPaths.insert(newRelativePath);
        }
        else if (expandedPath.startsWith(oldRelativePath + "/"))
        {
            updatedPaths.insert(newRelativePath + expandedPath.sliced(oldRelativePath.size()));
        }
        else
        {
            updatedPaths.insert(expandedPath);
        }
    }
    m_ExpandedPaths = std::move(updatedPaths);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::StartUploadQueue(std::unique_ptr<SDCardSyncState> state, StateFinishedDelegate finished)
{
    if (state != nullptr)
    {
        m_UpdateFailedCount = 0;
        SetState(State::UploadingFile);
        UpdateControls();
        UpdateStatusLabel();

        SetActiveState(std::move(state),
            [this, finished = std::move(finished)](SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
            {
                Q_UNUSED(state);
                Q_UNUSED(statusText);
                if (status == SDCardSyncState::CompletionStatus::Succeeded)
                {
                    if (m_CleanupDiffTempAfterUpload)
                    {
                        CleanupDiffState();
                    }
                    if (finished)
                    {
                        finished();
                    }
                    else
                    {
                        FinishUpdateOperation();
                    }
                }
            }
        );
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedUploadDirectory(const UploadDirectory& uploadDirectory)
{
    SyncNode& node = FindOrCreateNode(uploadDirectory.RelativePath);
    if (uploadDirectory.AssumeIdenticalToLocal && node.Local.has_value() && node.Local->IsDirectory)
    {
        node.Remote = node.Local;
        node.Status = SyncStatus::Identical;
    }
    else
    {
        node.Remote = uploadDirectory.UploadedInfo;
        RecalculateSyncStatus(node);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyCompletedUploadFile(const UploadFile& uploadFile)
{
    if (!uploadFile.UploadedInfo.IsDirectory)
    {
        SyncNode& node = FindOrCreateNode(uploadFile.RelativePath);
        node.Remote = uploadFile.UploadedInfo;
        if (uploadFile.AssumeIdenticalToLocal)
        {
            node.Status = SyncStatus::Identical;
        }
        else
        {
            RecalculateSyncStatus(node);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::RebuildTrees()
{
    const QSignalBlocker localTreeSignalBlocker(ui.m_LocalTree);
    const QSignalBlocker deviceTreeSignalBlocker(ui.m_DeviceTree);

    ui.m_LocalTree->setUpdatesEnabled(false);
    ui.m_DeviceTree->setUpdatesEnabled(false);
    ui.m_LocalTree->clear();
    ui.m_DeviceTree->clear();
    m_ItemPairs.clear();
    m_ItemPaths.clear();

    std::vector<const SyncNode*> sortedNodes;
    sortedNodes.reserve(m_Model.GetRootNodes().size());
    for (const auto& nodeEntry : m_Model.GetRootNodes())
    {
        sortedNodes.push_back(nodeEntry.second.get());
    }
    std::sort(sortedNodes.begin(), sortedNodes.end(), CompareNodeDisplayOrder);
    for (const SyncNode* node : sortedNodes)
    {
        AddTreeNode(*node, nullptr, nullptr);
    }

    ui.m_LocalTree->setUpdatesEnabled(true);
    ui.m_DeviceTree->setUpdatesEnabled(true);
    m_ScrollUpdateTimer.start();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::AddTreeNode(const SyncNode& node, QTreeWidgetItem* localParent, QTreeWidgetItem* deviceParent)
{
    if (!ShouldShowNode(node))
    {
        return false;
    }

    const QString localName = node.Local.has_value() ? node.Local->Name : node.Name;
    const QString deviceName = node.Remote.has_value() ? node.Remote->Name : node.Name;
    const QString localTime = (node.Local.has_value() && !node.Local->IsDirectory) ? SDCardSyncFileUtils::FormatFileTime(node.Local->ModificationTimeNanos) : "";
    const QString deviceTime = (node.Remote.has_value() && !node.Remote->IsDirectory) ? SDCardSyncFileUtils::FormatFileTime(node.Remote->ModificationTimeNanos) : "";
    const QString localSize = (node.Local.has_value() && !node.Local->IsDirectory) ? SDCardSyncFileUtils::FormatByteCount(node.Local->Size) : "";
    const QString deviceSize = (node.Remote.has_value() && !node.Remote->IsDirectory) ? SDCardSyncFileUtils::FormatByteCount(node.Remote->Size) : "";

    QTreeWidgetItem* localItem = new QTreeWidgetItem(QStringList({ localName, localTime, localSize }));
    QTreeWidgetItem* deviceItem = new QTreeWidgetItem(QStringList({ deviceName, deviceTime, deviceSize }));

    localItem->setFlags(localItem->flags() | Qt::ItemIsEditable);
    deviceItem->setFlags(deviceItem->flags() | Qt::ItemIsEditable);

    static const QSize treeRowSize(0, QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize) + 4);
    for (int column = 0; column < localItem->columnCount(); ++column)
    {
        localItem->setSizeHint(column, treeRowSize);
        deviceItem->setSizeHint(column, treeRowSize);
    }

    static const QIcon directoryIcon = QApplication::style()->standardIcon(QStyle::SP_DirClosedIcon);
    static const QIcon fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    if (node.Local.has_value())
    {
        localItem->setIcon(0, node.Local->IsDirectory ? directoryIcon : fileIcon);
    }
    if (node.Remote.has_value())
    {
        deviceItem->setIcon(0, node.Remote->IsDirectory ? directoryIcon : fileIcon);
    }

    localItem->setToolTip(0, GetStatusText(node, node.Local.has_value()));
    deviceItem->setToolTip(0, GetStatusText(node, node.Remote.has_value()));
    ApplyItemStyle(localItem, node, node.Local.has_value());
    ApplyItemStyle(deviceItem, node, node.Remote.has_value());

    if (localParent != nullptr)
    {
        localParent->addChild(localItem);
    }
    else
    {
        ui.m_LocalTree->addTopLevelItem(localItem);
    }

    if (deviceParent != nullptr)
    {
        deviceParent->addChild(deviceItem);
    }
    else
    {
        ui.m_DeviceTree->addTopLevelItem(deviceItem);
    }

    m_ItemPairs[localItem] = deviceItem;
    m_ItemPairs[deviceItem] = localItem;
    m_ItemPaths[localItem] = node.RelativePath;
    m_ItemPaths[deviceItem] = node.RelativePath;

    if (NodeHasVisibleChildren(node))
    {
        localItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        deviceItem->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }

    if (m_ExpandedPaths.contains(node.RelativePath))
    {
        PopulateTreeChildren(node, localItem, deviceItem);
        localItem->setExpanded(true);
        deviceItem->setExpanded(true);
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::PopulateTreeChildren(const SyncNode& node, QTreeWidgetItem* localParent, QTreeWidgetItem* deviceParent)
{
    if (localParent != nullptr && deviceParent != nullptr && localParent->childCount() == 0 && deviceParent->childCount() == 0)
    {
        std::vector<const SyncNode*> sortedNodes;
        sortedNodes.reserve(node.Children.size());
        for (const auto& childEntry : node.Children)
        {
            sortedNodes.push_back(childEntry.second.get());
        }
        std::sort(sortedNodes.begin(), sortedNodes.end(), CompareNodeDisplayOrder);
        for (const SyncNode* childNode : sortedNodes)
        {
            AddTreeNode(*childNode, localParent, deviceParent);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::NodeHasVisibleChildren(const SyncNode& node) const
{
    bool result = false;
    for (const auto& childEntry : node.Children)
    {
        if (ShouldShowNode(*childEntry.second))
        {
            result = true;
            break;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::ShouldShowNode(const SyncNode& node) const
{
    if (node.Status == SyncStatus::Ignored)
    {
        return !ui.m_HideIdenticalCheckBox->isChecked();
    }

    if (ui.m_HideDeviceOnlyCheckBox->isChecked() && node.Status == SyncStatus::RemoteOnly)
    {
        return false;
    }

    if (!ui.m_HideIdenticalCheckBox->isChecked())
    {
        return true;
    }

    if (node.Status != SyncStatus::Identical)
    {
        return true;
    }

    for (const auto& childEntry : node.Children)
    {
        if (ShouldShowNode(*childEntry.second))
        {
            return true;
        }
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyItemStyle(QTreeWidgetItem* item, const SyncNode& node, bool hasSide) const
{
    QBrush foregroundBrush = item->foreground(0);
    if (!hasSide)
    {
        foregroundBrush = QBrush(QColor(180, 48, 48));
    }
    else if (node.Status == SyncStatus::Ignored)
    {
        foregroundBrush = QBrush(QColor(112, 84, 160));
    }
    else if (node.Status == SyncStatus::Identical)
    {
        foregroundBrush = QBrush(QColor(130, 130, 130));
    }
    else if (node.Status == SyncStatus::Comparing)
    {
        foregroundBrush = QBrush(QColor(128, 96, 0));
    }
    else if (node.Status == SyncStatus::LocalOnly || node.Status == SyncStatus::RemoteOnly || node.Status == SyncStatus::Different)
    {
        foregroundBrush = QBrush(QColor(24, 92, 160));
    }
    else if (node.Status == SyncStatus::TypeConflict)
    {
        foregroundBrush = QBrush(QColor(180, 80, 0));
    }

    for (int column = 0; column < item->columnCount(); ++column)
    {
        item->setForeground(column, foregroundBrush);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetPairedItemExpanded(QTreeWidgetItem* item, bool expanded)
{
    if (!m_UpdatingTreeExpansion)
    {
        m_UpdatingTreeExpansion = true;
        auto pathIterator = m_ItemPaths.find(item);
        if (pathIterator != m_ItemPaths.end())
        {
            const QString relativePath = pathIterator->second;
            if (expanded)
            {
                m_ExpandedPaths.insert(relativePath);
            }
            else
            {
                m_ExpandedPaths.erase(relativePath);
            }

            auto itemIterator = m_ItemPairs.find(item);
            QTreeWidgetItem* pairedItem = (itemIterator != m_ItemPairs.end()) ? itemIterator->second : nullptr;
            SyncNode* node = FindNode(relativePath);
            if (expanded && node != nullptr && pairedItem != nullptr)
            {
                QTreeWidgetItem* localItem = item;
                QTreeWidgetItem* deviceItem = pairedItem;
                if (item->treeWidget() == ui.m_DeviceTree)
                {
                    localItem = pairedItem;
                    deviceItem = item;
                }
                PopulateTreeChildren(*node, localItem, deviceItem);
            }
            if (pairedItem != nullptr)
            {
                pairedItem->setExpanded(expanded);
            }
        }
        m_UpdatingTreeExpansion = false;
        m_ScrollUpdateTimer.start();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SyncTreeSelection(QTreeWidget* sourceTree, QTreeWidget* targetTree)
{
    if (!m_UpdatingTreeSelection)
    {
        m_UpdatingTreeSelection = true;
        targetTree->clearSelection();

        const int currentColumn = std::max(0, sourceTree->currentColumn());
        for (QTreeWidgetItem* sourceItem : sourceTree->selectedItems())
        {
            if (sourceItem != nullptr)
            {
                auto itemIterator = m_ItemPairs.find(sourceItem);
                QTreeWidgetItem* pairedItem = (itemIterator != m_ItemPairs.end()) ? itemIterator->second : nullptr;
                if (pairedItem != nullptr)
                {
                    pairedItem->setSelected(true);
                }
            }
        }

        QTreeWidgetItem* sourceItem = sourceTree->currentItem();
        if (sourceItem != nullptr)
        {
            auto itemIterator = m_ItemPairs.find(sourceItem);
            QTreeWidgetItem* pairedItem = (itemIterator != m_ItemPairs.end()) ? itemIterator->second : nullptr;
            if (pairedItem != nullptr)
            {
                targetTree->setCurrentItem(pairedItem, currentColumn, QItemSelectionModel::NoUpdate);
            }
        }
        m_UpdatingTreeSelection = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ShowTreeContextMenu(QTreeWidget* tree, const QPoint& position)
{
    QTreeWidgetItem* item = tree->itemAt(position);
    if (item == nullptr)
    {
        return;
    }

    auto pathIterator = m_ItemPaths.find(item);
    if (pathIterator == m_ItemPaths.end())
    {
        return;
    }

    SyncNode* node = FindNode(pathIterator->second);
    if (node == nullptr)
    {
        return;
    }

    bool isDirectory = false;
    const QString explorerPath = GetExplorerPath(tree, *node, isDirectory);
    const std::vector<QString> actionPaths = GetContextActionPaths(tree, item);
    const QString sourceName = (tree == ui.m_LocalTree) ? "device" : "PC";
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;

    QMenu menu(this);
    QAction* showInExplorerAction = menu.addAction("Show in explorer...");
    QAction* downloadAction = menu.addAction("Download to PC...");
    QAction* uploadFilesAction = menu.addAction("Upload files to device...");
    QAction* uploadFolderAction = menu.addAction("Upload folder to device...");
    QAction* diffAction = menu.addAction("Diff...");
    QAction* createDirectoryAction = menu.addAction("New folder...");
    QAction* deleteAction = menu.addAction("Delete...");
    QAction* ignoreAction = menu.addAction("Ignore...");
    QMenu* deleteSyncMenu = menu.addMenu("Delete syncing");
    QActionGroup* deleteSyncActionGroup = new QActionGroup(&menu);
    QAction* inheritDeleteSyncAction = deleteSyncMenu->addAction(GetDeleteSyncRuleActionText(*node, DeleteSyncRuleMode::Inherit));
    QAction* disabledDeleteSyncAction = deleteSyncMenu->addAction(GetDeleteSyncRuleActionText(*node, DeleteSyncRuleMode::Disabled));
    QAction* allowThisDeleteSyncAction = deleteSyncMenu->addAction(GetDeleteSyncRuleActionText(*node, DeleteSyncRuleMode::AllowThis));
    QAction* allowRecursiveDeleteSyncAction = deleteSyncMenu->addAction(GetDeleteSyncRuleActionText(*node, DeleteSyncRuleMode::AllowRecursive));
    QAction* updateContentAction = menu.addAction(QString("Update content from %1").arg(sourceName));
    QAction* updateTimestampAction = menu.addAction(QString("Update timestamp from %1").arg(sourceName));
    QAction* updateTimestampIfIdenticalAction = menu.addAction(QString("Update timestamp from %1 if content identical").arg(sourceName));
    deleteSyncActionGroup->addAction(inheritDeleteSyncAction);
    deleteSyncActionGroup->addAction(disabledDeleteSyncAction);
    deleteSyncActionGroup->addAction(allowThisDeleteSyncAction);
    deleteSyncActionGroup->addAction(allowRecursiveDeleteSyncAction);
    inheritDeleteSyncAction->setCheckable(true);
    disabledDeleteSyncAction->setCheckable(true);
    allowThisDeleteSyncAction->setCheckable(true);
    allowRecursiveDeleteSyncAction->setCheckable(true);
    showInExplorerAction->setEnabled(!explorerPath.isEmpty());
    downloadAction->setEnabled(tree == ui.m_DeviceTree && connected && !actionPaths.empty() && HasDeleteTargets(actionPaths, ui.m_DeviceTree));
    uploadFilesAction->setEnabled(connected);
    uploadFolderAction->setEnabled(connected);
    diffAction->setEnabled(CanDiffNode(*node));
    createDirectoryAction->setEnabled(m_State == State::Idle && (connected || !m_LocalRootPath.isEmpty()));

    bool canUpdateContent = false;
    bool canUpdateTimestamp = false;
    bool canUpdateTimestampIfIdentical = false;
    bool canDeleteSync = !m_LocalRootPath.isEmpty() && !actionPaths.empty();
    const bool canDelete = HasDeleteTargets(actionPaths, ui.m_LocalTree) || (connected && HasDeleteTargets(actionPaths, ui.m_DeviceTree));
    const bool canIgnore = !m_LocalRootPath.isEmpty() && !actionPaths.empty();
    for (const QString& relativePath : actionPaths)
    {
        SyncNode* selectedNode = FindNode(relativePath);
        if (selectedNode != nullptr)
        {
            canUpdateContent = canUpdateContent || CanUpdateContent(*selectedNode, tree);
            canUpdateTimestamp = canUpdateTimestamp || CanUpdateTimestamp(*selectedNode, tree, false);
            canUpdateTimestampIfIdentical = canUpdateTimestampIfIdentical || CanUpdateTimestamp(*selectedNode, tree, true);
            canDeleteSync = canDeleteSync && IsDirectoryNode(*selectedNode);
        }
        else
        {
            canDeleteSync = false;
        }
    }
    switch (GetDeleteSyncRule(node->RelativePath))
    {
        case DeleteSyncRuleMode::Inherit:
            inheritDeleteSyncAction->setChecked(true);
            break;
        case DeleteSyncRuleMode::Disabled:
            disabledDeleteSyncAction->setChecked(true);
            break;
        case DeleteSyncRuleMode::AllowThis:
            allowThisDeleteSyncAction->setChecked(true);
            break;
        case DeleteSyncRuleMode::AllowRecursive:
            allowRecursiveDeleteSyncAction->setChecked(true);
            break;
    }
    deleteAction->setEnabled(canDelete);
    ignoreAction->setEnabled(canIgnore);
    deleteSyncMenu->setEnabled(canDeleteSync);
    updateContentAction->setEnabled(canUpdateContent);
    updateTimestampAction->setEnabled(canUpdateTimestamp);
    updateTimestampIfIdenticalAction->setEnabled(canUpdateTimestampIfIdentical);

    QAction* selectedAction = menu.exec(tree->viewport()->mapToGlobal(position));
    if (selectedAction == showInExplorerAction && !explorerPath.isEmpty())
    {
        SDCardSyncFileUtils::OpenExplorerPath(explorerPath, isDirectory);
    }
    else if (selectedAction == downloadAction)
    {
        StartDownloadToFolder(actionPaths);
    }
    else if (selectedAction == uploadFilesAction)
    {
        StartUploadFiles(*node);
    }
    else if (selectedAction == uploadFolderAction)
    {
        StartUploadFolder(*node);
    }
    else if (selectedAction == diffAction)
    {
        StartDiff(*node);
    }
    else if (selectedAction == createDirectoryAction)
    {
        const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
        std::unique_ptr<SDCardSyncState> createDirectoryState = std::make_unique<SDStateCreateDirectory>(
            CreateStateContext(),
            this,
            *node,
            tree == ui.m_LocalTree,
            m_Model,
            m_LocalRootPath,
            QString::fromUtf8(DEVICE_ROOT_PATH),
            connected,
            [this](const SDStateCreateDirectory::Result& result)
            {
                if (result.LocalCreated)
                {
                    LogCreateDirectoryResult(PLogSeverity::INFO_LOW_VOL, result.RelativePath, "created on PC");
                }
                if (result.RemoteCreated)
                {
                    LogCreateDirectoryResult(PLogSeverity::INFO_LOW_VOL, result.RelativePath, "created on device");
                }
                else if (result.RemoteRequested)
                {
                    LogCreateDirectoryResult(PLogSeverity::ERROR, result.RelativePath, "failed to create on device");
                }
                ApplyCompletedCreateDirectory(result.RelativePath, result.LocalInfo, result.RemoteInfo);
            }
        );

        SetState(State::CreatingDirectory);
        UpdateControls();
        UpdateStatusLabel();
        SetActiveState(std::move(createDirectoryState),
            [this](SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText)
            {
                Q_UNUSED(state);
                Q_UNUSED(status);
                Q_UNUSED(statusText);
                RebuildTrees();
                UpdateControls();
                UpdateStatusLabel();
            }
        );
    }
    else if (selectedAction == deleteAction)
    {
        ShowDeleteConfirmation(actionPaths, tree);
    }
    else if (selectedAction == ignoreAction)
    {
        ShowIgnoreDialog(actionPaths, *node);
    }
    else if (selectedAction == inheritDeleteSyncAction)
    {
        SetDeleteSyncRule(actionPaths, DeleteSyncRuleMode::Inherit);
    }
    else if (selectedAction == disabledDeleteSyncAction)
    {
        SetDeleteSyncRule(actionPaths, DeleteSyncRuleMode::Disabled);
    }
    else if (selectedAction == allowThisDeleteSyncAction)
    {
        SetDeleteSyncRule(actionPaths, DeleteSyncRuleMode::AllowThis);
    }
    else if (selectedAction == allowRecursiveDeleteSyncAction)
    {
        SetDeleteSyncRule(actionPaths, DeleteSyncRuleMode::AllowRecursive);
    }
    else if (selectedAction == updateContentAction)
    {
        StartContentUpdate(actionPaths, tree);
    }
    else if (selectedAction == updateTimestampAction)
    {
        StartTimestampUpdate(actionPaths, tree, false);
    }
    else if (selectedAction == updateTimestampIfIdenticalAction)
    {
        StartTimestampUpdate(actionPaths, tree, true);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ShowIgnoreDialog(const std::vector<QString>& relativePaths, const SyncNode& clickedNode)
{
    const QStringList exactPatterns = BuildExactIgnorePatterns(relativePaths);
    const QStringList extensionPatterns = BuildExtensionIgnorePatterns(relativePaths);
    const QStringList parentFolders = BuildParentIgnoreFolders(clickedNode);

    QDialog dialog(this);
    dialog.setWindowTitle("Ignore");

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* patternLabel = new QLabel("Add to .syncignore:", &dialog);
    layout->addWidget(patternLabel);

    QPlainTextEdit* patternEdit = new QPlainTextEdit(&dialog);
    patternEdit->setMinimumWidth(520);
    patternEdit->setMinimumHeight(90);
    layout->addWidget(patternEdit);

    QButtonGroup* buttonGroup = new QButtonGroup(&dialog);
    QRadioButton* exactButton = new QRadioButton("Ignore exact filename(s)", &dialog);
    QRadioButton* extensionButton = new QRadioButton("Ignore all files with this extension(s)", &dialog);
    QRadioButton* beneathButton = new QRadioButton("Ignore everything beneath:", &dialog);
    QRadioButton* customButton = new QRadioButton("Custom pattern", &dialog);

    buttonGroup->addButton(exactButton);
    buttonGroup->addButton(extensionButton);
    buttonGroup->addButton(beneathButton);
    buttonGroup->addButton(customButton);

    layout->addWidget(exactButton);
    layout->addWidget(extensionButton);

    QHBoxLayout* beneathLayout = new QHBoxLayout();
    beneathLayout->addWidget(beneathButton);
    QComboBox* folderCombo = new QComboBox(&dialog);
    for (const QString& folder : parentFolders)
    {
        folderCombo->addItem(folder, SDCardSyncIgnoreRules::EscapePattern(folder) + "/");
    }
    beneathLayout->addWidget(folderCombo, 1);
    layout->addLayout(beneathLayout);
    layout->addWidget(customButton);

    exactButton->setEnabled(!exactPatterns.isEmpty());
    extensionButton->setEnabled(!extensionPatterns.isEmpty());
    beneathButton->setEnabled(!parentFolders.isEmpty());
    folderCombo->setEnabled(!parentFolders.isEmpty());

    if (exactButton->isEnabled())
    {
        exactButton->setChecked(true);
    }
    else if (extensionButton->isEnabled())
    {
        extensionButton->setChecked(true);
    }
    else if (beneathButton->isEnabled())
    {
        beneathButton->setChecked(true);
    }
    else
    {
        customButton->setChecked(true);
    }

    QDialogButtonBox* buttonBox = new QDialogButtonBox(&dialog);
    QPushButton* okButton = buttonBox->addButton("Ok", QDialogButtonBox::AcceptRole);
    buttonBox->addButton("Cancel", QDialogButtonBox::RejectRole);
    layout->addWidget(buttonBox);

    auto updateOkButton = [&]()
    {
        okButton->setEnabled(!SDCardSyncIgnoreRules::SplitPatterns(patternEdit->toPlainText()).isEmpty());
    };

    auto updatePatternText = [&]()
    {
        if (customButton->isChecked())
        {
            patternEdit->setReadOnly(false);
        }
        else
        {
            QStringList patterns;
            patternEdit->setReadOnly(true);
            if (exactButton->isChecked())
            {
                patterns = exactPatterns;
            }
            else if (extensionButton->isChecked())
            {
                patterns = extensionPatterns;
            }
            else if (beneathButton->isChecked() && folderCombo->currentIndex() >= 0)
            {
                patterns.push_back(folderCombo->currentData().toString());
            }

            const QString patternText = SDCardSyncIgnoreRules::JoinPatterns(patterns);
            if (patternEdit->toPlainText() != patternText)
            {
                patternEdit->setPlainText(patternText);
            }
        }
        updateOkButton();
    };

    connect(patternEdit, &QPlainTextEdit::textChanged, &dialog, [&]()
    {
        updateOkButton();
    });
    connect(exactButton, &QRadioButton::toggled, &dialog, [&](bool checked)
    {
        if (checked)
        {
            updatePatternText();
        }
    });
    connect(extensionButton, &QRadioButton::toggled, &dialog, [&](bool checked)
    {
        if (checked)
        {
            updatePatternText();
        }
    });
    connect(beneathButton, &QRadioButton::toggled, &dialog, [&](bool checked)
    {
        if (checked)
        {
            updatePatternText();
        }
    });
    connect(customButton, &QRadioButton::toggled, &dialog, [&](bool checked)
    {
        if (checked)
        {
            updatePatternText();
        }
    });
    connect(folderCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, [&](int index)
    {
        Q_UNUSED(index);
        if (beneathButton->isChecked())
        {
            updatePatternText();
        }
    });
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    updatePatternText();
    if (dialog.exec() == QDialog::Accepted)
    {
        AppendSyncIgnorePatterns(SDCardSyncIgnoreRules::SplitPatterns(patternEdit->toPlainText()));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::AppendSyncIgnorePatterns(const QStringList& patterns)
{
    bool result = false;
    if (!patterns.isEmpty() && !m_LocalRootPath.isEmpty())
    {
        const QString ignoreFilePath = QDir(m_LocalRootPath).absoluteFilePath(SYNC_IGNORE_FILE_NAME);
        const bool needsLeadingNewline = !SDCardSyncFileUtils::FileEndsWithLineBreak(ignoreFilePath);
        QFile ignoreFile(ignoreFilePath);
        QString errorText;
        if (ignoreFile.open(QFile::WriteOnly | QFile::Append | QFile::Text))
        {
            QTextStream stream(&ignoreFile);
            if (needsLeadingNewline)
            {
                stream << '\n';
            }
            for (const QString& pattern : patterns)
            {
                stream << pattern << '\n';
            }
            stream.flush();
            result = stream.status() == QTextStream::Ok;
            errorText = ignoreFile.errorString();
        }
        else
        {
            errorText = ignoreFile.errorString();
        }

        if (result)
        {
            if (m_DeviceSession != nullptr)
            {
                m_DeviceSession->AddLogMessage(PLogSeverity::INFO_LOW_VOL, "SD-card sync: added {} pattern(s) to .syncignore", static_cast<size_t>(patterns.size()));
            }
            ScheduleRefresh(false);
        }
        else
        {
            QMessageBox::warning(this, "Ignore", QString("Failed to update %1:\n%2").arg(ignoreFilePath, errorText));
            if (m_DeviceSession != nullptr)
            {
                m_DeviceSession->AddLogMessage(PLogSeverity::ERROR, "SD-card sync: failed to update .syncignore: {}", errorText.toStdString());
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList SDCardSync::BuildExactIgnorePatterns(const std::vector<QString>& relativePaths)
{
    QStringList result;
    std::set<QString> uniquePatterns;
    for (const QString& relativePath : relativePaths)
    {
        const SyncNode* node = FindNode(relativePath);
        if (node != nullptr)
        {
            QString pattern = SDCardSyncIgnoreRules::EscapePattern(node->RelativePath);
            bool isDirectory = false;
            if (node->Local.has_value())
            {
                isDirectory = node->Local->IsDirectory;
            }
            else if (node->Remote.has_value())
            {
                isDirectory = node->Remote->IsDirectory;
            }
            if (isDirectory)
            {
                pattern += "/";
            }
            if (uniquePatterns.insert(pattern).second)
            {
                result.push_back(pattern);
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList SDCardSync::BuildExtensionIgnorePatterns(const std::vector<QString>& relativePaths)
{
    QStringList result;
    std::set<QString> uniquePatterns;
    for (const QString& relativePath : relativePaths)
    {
        const SyncNode* node = FindNode(relativePath);
        if (node != nullptr)
        {
            bool isDirectory = false;
            if (node->Local.has_value())
            {
                isDirectory = node->Local->IsDirectory;
            }
            else if (node->Remote.has_value())
            {
                isDirectory = node->Remote->IsDirectory;
            }
            const QString extension = QFileInfo(node->Name).suffix();
            if (!isDirectory && !extension.isEmpty())
            {
                const QString pattern = "*." + SDCardSyncIgnoreRules::EscapeWildcardCharacters(extension);
                if (uniquePatterns.insert(pattern).second)
                {
                    result.push_back(pattern);
                }
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList SDCardSync::BuildParentIgnoreFolders(const SyncNode& clickedNode) const
{
    QStringList result;
    bool isDirectory = false;
    if (clickedNode.Local.has_value())
    {
        isDirectory = clickedNode.Local->IsDirectory;
    }
    else if (clickedNode.Remote.has_value())
    {
        isDirectory = clickedNode.Remote->IsDirectory;
    }

    QString folderPath;
    if (isDirectory)
    {
        folderPath = clickedNode.RelativePath;
    }
    else
    {
        const qsizetype separatorIndex = clickedNode.RelativePath.lastIndexOf('/');
        if (separatorIndex >= 0)
        {
            folderPath = clickedNode.RelativePath.left(separatorIndex);
        }
    }

    while (!folderPath.isEmpty())
    {
        result.push_back(folderPath);
        const qsizetype separatorIndex = folderPath.lastIndexOf('/');
        if (separatorIndex < 0)
        {
            folderPath.clear();
        }
        else
        {
            folderPath = folderPath.left(separatorIndex);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetExplorerPath(QTreeWidget* tree, const SyncNode& node, bool& isDirectory) const
{
    QString result;
    isDirectory = false;

    if (tree == ui.m_LocalTree)
    {
        if (node.Local.has_value())
        {
            result = GetLocalPath(node.RelativePath);
            isDirectory = node.Local->IsDirectory;
        }
    }
    else if (tree == ui.m_DeviceTree && node.Remote.has_value() && m_DeviceSession != nullptr)
    {
        QString mountRootPath = m_DeviceSession->GetDeviceFilesystemMountRootPath();
        if (!mountRootPath.isEmpty())
        {
            if (!mountRootPath.endsWith('/') && !mountRootPath.endsWith('\\'))
            {
                mountRootPath += "/";
            }
            result = mountRootPath + node.RelativePath;
            isDirectory = node.Remote->IsDirectory;
        }
    }
    return QDir::toNativeSeparators(result);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetUploadTargetFolder(const SyncNode& clickedNode) const
{
    bool isDirectory = IsDirectoryNode(clickedNode);
    if (clickedNode.Local.has_value())
    {
        isDirectory = clickedNode.Local->IsDirectory;
    }
    else if (clickedNode.Remote.has_value())
    {
        isDirectory = clickedNode.Remote->IsDirectory;
    }

    if (isDirectory)
    {
        return clickedNode.RelativePath;
    }

    const qsizetype separatorIndex = clickedNode.RelativePath.lastIndexOf('/');
    return (separatorIndex >= 0) ? clickedNode.RelativePath.left(separatorIndex) : QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList SDCardSync::SelectUploadFiles()
{
    const QStringList result = QFileDialog::getOpenFileNames(this, "Upload files", m_LastUploadPath);
    if (!result.isEmpty())
    {
        m_LastUploadPath = QFileInfo(result.back()).absolutePath();
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QStringList SDCardSync::SelectUploadFolder()
{
    QStringList result;
    const QString selectedPath = QFileDialog::getExistingDirectory(this, "Upload folder", m_LastUploadPath);
    if (!selectedPath.isEmpty())
    {
        m_LastUploadPath = QFileInfo(selectedPath).absolutePath();
        result.push_back(selectedPath);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::CanDiffNode(const SyncNode& node) const
{
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    bool result = false;
    if (m_State == State::Idle && connected && node.Local.has_value() && node.Remote.has_value())
    {
        if (!node.Local->IsDirectory && !node.Remote->IsDirectory)
        {
            result = QFileInfo(GetLocalPath(node.RelativePath)).isFile();
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::CanUpdateContent(const SyncNode& node, QTreeWidget* targetTree) const
{
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    bool result = false;
    if (m_State == State::Idle && connected && m_DeviceReadComplete && node.Status != SyncStatus::Ignored)
    {
        if (targetTree == ui.m_DeviceTree)
        {
            result = HasUploadTargets(node);
        }
        else if (targetTree == ui.m_LocalTree)
        {
            result = HasLocalContentUpdateTargets(node);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasDeleteTargets(const std::vector<QString>& relativePaths, QTreeWidget* targetTree)
{
    bool result = false;
    for (const QString& relativePath : relativePaths)
    {
        const SyncNode* node = FindNode(relativePath);
        if (node != nullptr && HasDeleteTargets(*node, targetTree))
        {
            result = true;
            break;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasDeleteTargets(const SyncNode& node, QTreeWidget* targetTree) const
{
    bool result = HasDeleteTarget(node, targetTree);
    if (!result)
    {
        for (const auto& childEntry : node.Children)
        {
            if (HasDeleteTargets(*childEntry.second, targetTree))
            {
                result = true;
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasDeleteTarget(const SyncNode& node, QTreeWidget* targetTree) const
{
    bool result = false;
    if (targetTree == ui.m_LocalTree)
    {
        result = node.Local.has_value();
    }
    else if (targetTree == ui.m_DeviceTree)
    {
        result = node.Remote.has_value();
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasPairedDeleteTargets(const std::vector<QString>& relativePaths)
{
    bool result = !relativePaths.empty();
    for (const QString& relativePath : relativePaths)
    {
        const SyncNode* node = FindNode(relativePath);
        if (node == nullptr || !HasPairedDeleteTarget(*node))
        {
            result = false;
            break;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasPairedDeleteTarget(const SyncNode& node) const
{
    return node.Local.has_value() && node.Remote.has_value();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasLocalContentUpdateTargets(const SyncNode& node) const
{
    bool result = HasLocalContentUpdateTarget(node);
    if (!result)
    {
        for (const auto& childEntry : node.Children)
        {
            if (HasLocalContentUpdateTargets(*childEntry.second))
            {
                result = true;
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasLocalContentUpdateTarget(const SyncNode& node) const
{
    bool result = false;
    if (node.Status != SyncStatus::Ignored && node.Remote.has_value() && !node.Remote->IsDirectory)
    {
        result = (!node.Local.has_value() || !node.Local->IsDirectory) && node.Status != SyncStatus::Identical && node.Status != SyncStatus::Comparing;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::CanUpdateTimestamp(const SyncNode& node, QTreeWidget* targetTree, bool requireIdentical) const
{
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    const bool localDirectory = node.Local.has_value() && node.Local->IsDirectory;
    const bool remoteDirectory = node.Remote.has_value() && node.Remote->IsDirectory;
    bool result = false;
    if (m_State == State::Idle && node.Status != SyncStatus::Ignored)
    {
        if (localDirectory || remoteDirectory)
        {
            for (const auto& childEntry : node.Children)
            {
                if (CanUpdateTimestamp(*childEntry.second, targetTree, requireIdentical))
                {
                    result = true;
                    break;
                }
            }
        }
        else if (node.Local.has_value() && node.Remote.has_value() && QFileInfo(GetLocalPath(node.RelativePath)).isFile())
        {
            if (targetTree == ui.m_LocalTree)
            {
                result = !requireIdentical || connected;
            }
            else if (targetTree == ui.m_DeviceTree)
            {
                result = connected;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::vector<QString> SDCardSync::GetContextActionPaths(QTreeWidget* tree, QTreeWidgetItem* clickedItem) const
{
    std::vector<QString> result;
    std::set<QString> uniquePaths;

    if (clickedItem != nullptr && clickedItem->isSelected())
    {
        for (QTreeWidgetItem* item : tree->selectedItems())
        {
            if (item != nullptr)
            {
                auto pathIterator = m_ItemPaths.find(item);
                if (pathIterator != m_ItemPaths.end() && uniquePaths.insert(pathIterator->second).second)
                {
                    result.push_back(pathIterator->second);
                }
            }
        }
    }
    else if (clickedItem != nullptr)
    {
        auto pathIterator = m_ItemPaths.find(clickedItem);
        if (pathIterator != m_ItemPaths.end())
        {
            result.push_back(pathIterator->second);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LogContentUpdateResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const
{
    if (m_DeviceSession != nullptr)
    {
        m_DeviceSession->AddLogMessage(severity, "SD-card sync: {}: {}", relativePath.toStdString(), resultText.toStdString());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LogDeleteResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const
{
    if (m_DeviceSession != nullptr)
    {
        m_DeviceSession->AddLogMessage(severity, "SD-card sync: {}: {}", relativePath.toStdString(), resultText.toStdString());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LogCreateDirectoryResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const
{
    if (m_DeviceSession != nullptr)
    {
        m_DeviceSession->AddLogMessage(severity, "SD-card sync: {}: {}", relativePath.toStdString(), resultText.toStdString());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LogRenameResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const
{
    if (m_DeviceSession != nullptr)
    {
        m_DeviceSession->AddLogMessage(severity, "SD-card sync: {}: {}", relativePath.toStdString(), resultText.toStdString());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::LogTimestampUpdateResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const
{
    if (m_DeviceSession != nullptr)
    {
        m_DeviceSession->AddLogMessage(severity, "SD-card sync: {}: {}", relativePath.toStdString(), resultText.toStdString());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::set<QString> SDCardSync::GetSelectedPaths() const
{
    std::set<QString> result;
    for (QTreeWidgetItem* item : ui.m_LocalTree->selectedItems())
    {
        if (item != nullptr)
        {
            auto pathIterator = m_ItemPaths.find(item);
            if (pathIterator != m_ItemPaths.end())
            {
                result.insert(pathIterator->second);
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::CleanupDiffState()
{
    if (m_DiffProcess != nullptr)
    {
        m_DiffProcess->deleteLater();
        m_DiffProcess = nullptr;
    }
    m_CurrentDiffRemoteFile.close();
    m_CurrentDiffRelativePath.clear();
    m_CurrentDiffLocalPath.clear();
    m_CurrentDiffRemoteTempPath.clear();
    m_CurrentDiffOutputPath.clear();
    m_CurrentDiffUpdateSourcePath.clear();
    m_CurrentDiffDownloadError.clear();
    m_CurrentDiffOutputInitialModificationTimeNanos = 0;
    m_CurrentDiffOriginalRemoteHash.clear();
    m_DiffTempDirectory.reset();
    m_CleanupDiffTempAfterUpload = false;

    if (m_RefreshPending)
    {
        const bool readDevice = m_RefreshPendingReadDevice;
        m_RefreshPending = false;
        m_RefreshPendingReadDevice = false;
        ScheduleRefresh(readDevice);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetState(State state)
{
    if (m_State == state)
    {
        return;
    }

    m_State = state;

    switch (m_State)
    {
        case State::Idle:
            ConfigureProgressBar(ui.m_ProgressBar, false, false);
            ConfigureProgressBar(ui.m_FileProgressBar, false, false);
            break;
        case State::OpeningSession:
        case State::DiffToolRunning:
            ConfigureProgressBar(ui.m_ProgressBar, true, true);
            ConfigureProgressBar(ui.m_FileProgressBar, false, false);
            UpdateProgressBarValues();
            break;
        case State::CrawlingRemote:
        case State::UpdatingRemoteTimestamp:
        case State::DeletingRemote:
        case State::RenamingRemote:
        case State::CreatingDirectory:
            ConfigureProgressBar(ui.m_ProgressBar, true, false);
            ConfigureProgressBar(ui.m_FileProgressBar, false, false);
            UpdateProgressBarValues();
            break;
        case State::ComparingContentReadingFile:
        case State::DiffReadingFile:
        case State::UpdatingLocalContentReadingFile:
        case State::UploadingFile:
            ConfigureProgressBar(ui.m_ProgressBar, true, false);
            ConfigureProgressBar(ui.m_FileProgressBar, true, false);
            UpdateProgressBarValues();
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetActiveState(std::unique_ptr<SDCardSyncState> state, SDCardSyncState::StateCompletedDelegate stateCompleted)
{
    if (m_ActiveState != nullptr)
    {
        std::unique_ptr<SDCardSyncState> previousState = std::move(m_ActiveState);
        previousState->StateCompleted = nullptr;
        previousState->ProgressChanged = nullptr;
        previousState->StatusTextChanged = nullptr;
        previousState->Cancel();
    }
    m_StateTotalProgress.reset();
    m_StateSubProgress.reset();
    m_StateProgressText.clear();
    m_StateStatusText.clear();

    if (state != nullptr)
    {
        state->StateCompleted = [this, stateCompleted = std::move(stateCompleted)](
            SDCardSyncState& completedState,
            SDCardSyncState::CompletionStatus status,
            const QString& statusText
        ) mutable
        {
            if (&completedState != m_ActiveState.get()) {
                return;
            }

            HandleStateCompleted(completedState, status, statusText);
            if (stateCompleted) {
                stateCompleted(completedState, status, statusText);
            }
            if (&completedState == m_ActiveState.get()) {
                m_ActiveState.reset();
            }
        };

        state->ProgressChanged = [this](
            SDCardSyncState& progressState,
            std::optional<float> totalProgress,
            std::optional<float> subProgress,
            const QString& progressText
        )
        {
            HandleStateProgress(progressState, totalProgress, subProgress, progressText);
        };

        state->StatusTextChanged = [this](SDCardSyncState& statusState, const QString& statusText)
        {
            HandleStateStatusText(statusState, statusText);
        };
    }

    m_ActiveState = std::move(state);
    if (m_ActiveState != nullptr) {
        m_ActiveState->Start();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncState* SDCardSync::GetActiveState() const
{
    return m_ActiveState.get();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncState::Context SDCardSync::CreateStateContext() const
{
    SDCardSyncState::Context context;
    context.SerialHandler = (m_DeviceSession != nullptr) ? &m_DeviceSession->GetSerialHandler() : nullptr;
    context.SessionID = m_SessionID;
    return context;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ApplyStateProgress(std::optional<float> totalProgress, std::optional<float> subProgress, const QString& progressText)
{
    m_StateTotalProgress = totalProgress;
    m_StateSubProgress = subProgress;
    m_StateProgressText = progressText;
    UpdateProgressItemText();
    UpdateProgressBarValues();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateSharedScrollBar()
{
    const QScrollBar* localScrollBar = ui.m_LocalTree->verticalScrollBar();
    const QScrollBar* deviceScrollBar = ui.m_DeviceTree->verticalScrollBar();
    const int minimum = std::min(localScrollBar->minimum(), deviceScrollBar->minimum());
    const int maximum = std::max(localScrollBar->maximum(), deviceScrollBar->maximum());
    const int pageStep = std::min(localScrollBar->pageStep(), deviceScrollBar->pageStep());

    m_UpdatingScrollBars = true;
    ui.m_SyncScrollBar->setRange(minimum, maximum);
    ui.m_SyncScrollBar->setPageStep(pageStep);
    ui.m_SyncScrollBar->setSingleStep(std::max(1, localScrollBar->singleStep()));
    ui.m_SyncScrollBar->setValue(localScrollBar->value());
    ui.m_SyncScrollBar->setEnabled(maximum > minimum);
    m_UpdatingScrollBars = false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateControls()
{
    const bool idle = m_State == State::Idle;
    const bool hasLocalRoot = !m_LocalRootPath.isEmpty();
    const bool connected = m_DeviceSession != nullptr && m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0;
    const bool canUpdate = idle && connected && m_DeviceReadComplete;

    ui.m_LocalRootCombo->setEnabled(idle);
    ui.m_BrowseButton->setEnabled(idle);
    ui.m_HideIdenticalCheckBox->setEnabled(idle);
    ui.m_HideDeviceOnlyCheckBox->setEnabled(idle);
    ui.m_CompareContentCheckBox->setEnabled(idle);
    ui.m_SyncDeletesCheckBox->setEnabled(idle);
    ui.m_RefreshButton->setEnabled(idle && hasLocalRoot);
    ui.m_UpdateSelectedButton->setEnabled(canUpdate && HasSelectedUploadTargets());
    ui.m_UpdateButton->setEnabled(canUpdate && (HasUploadTargets() || HasSyncDeleteTargets()));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateStatusLabel()
{
    if (!m_StatusOverride.isEmpty())
    {
        ui.m_StatusLabel->setText(m_StatusOverride);
        return;
    }
    if (!m_StateStatusText.isEmpty())
    {
        ui.m_StatusLabel->setText(m_StateStatusText);
        UpdateProgressItemText();
        return;
    }

    switch (m_State)
    {
        case State::Idle:
        {
            if (m_LocalRootPath.isEmpty())
            {
                ui.m_StatusLabel->setText("Select local root");
            }
            else
            {
                const size_t uploadFileCount = CountUploadFiles();
                const size_t deleteFileCount = ui.m_SyncDeletesCheckBox->isChecked() ? CountSyncDeleteFiles() : 0;
                if (uploadFileCount == 0 && deleteFileCount == 0)
                {
                    ui.m_StatusLabel->setText("Up to date");
                }
                else if (uploadFileCount != 0 && deleteFileCount != 0)
                {
                    ui.m_StatusLabel->setText(QString("%1 file(s) need update, %2 file(s) need delete").arg(uploadFileCount).arg(deleteFileCount));
                }
                else if (uploadFileCount != 0)
                {
                    ui.m_StatusLabel->setText(QString("%1 file(s) need update").arg(uploadFileCount));
                }
                else
                {
                    ui.m_StatusLabel->setText(QString("%1 file(s) need delete").arg(deleteFileCount));
                }
            }
            break;
        }
        case State::OpeningSession:
            ui.m_StatusLabel->setText("Opening session...");
            break;
        case State::CrawlingRemote:
            ui.m_StatusLabel->setText("Reading device...");
            break;
        case State::ComparingContentReadingFile:
            ui.m_StatusLabel->setText(QString("Comparing content (%1 left)...").arg(m_ContentCompareQueue.size()));
            break;
        case State::DiffReadingFile:
            ui.m_StatusLabel->setText("Downloading remote file for diff...");
            break;
        case State::DiffToolRunning:
            ui.m_StatusLabel->setText("Waiting for diff tool...");
            break;
        case State::UpdatingRemoteTimestamp:
            ui.m_StatusLabel->setText("Updating timestamp...");
            break;
        case State::UpdatingLocalContentReadingFile:
            ui.m_StatusLabel->setText("Updating local file...");
            break;
        case State::DeletingRemote:
            ui.m_StatusLabel->setText("Deleting item...");
            break;
        case State::RenamingRemote:
            ui.m_StatusLabel->setText("Renaming device item...");
            break;
        case State::CreatingDirectory:
            ui.m_StatusLabel->setText("Creating folder...");
            break;
        case State::UploadingFile:
            ui.m_StatusLabel->setText("Updating file...");
            break;
    }
    UpdateProgressItemText();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateProgressItemText()
{
    const QString itemText = GetCurrentProgressItemText();
    ui.m_ProgressItemEdit->setText(itemText);
    ui.m_ProgressItemEdit->setToolTip(itemText);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::UpdateProgressBarValues()
{
    switch (m_State)
    {
        case State::Idle:
            break;
        case State::OpeningSession:
            break;
        case State::CrawlingRemote:
            if (m_StateTotalProgress.has_value())
            {
                SetProgressBarValue(ui.m_ProgressBar, *m_StateTotalProgress);
            }
            break;
        case State::ComparingContentReadingFile:
        {
            SetProgressBarValue(ui.m_ProgressBar, m_ContentCompareCompletedBytes + std::min(m_CurrentCompareBytesRead, m_CurrentCompareSize), m_ContentCompareTotalBytes);
            SetProgressBarValue(ui.m_FileProgressBar, m_CurrentCompareBytesRead, m_CurrentCompareSize);
            break;
        }
        case State::DiffReadingFile:
            SetProgressBarValue(ui.m_ProgressBar, m_CurrentDiffBytesRead, m_CurrentDiffSize);
            SetProgressBarValue(ui.m_FileProgressBar, m_CurrentDiffBytesRead, m_CurrentDiffSize);
            break;
        case State::DiffToolRunning:
            break;
        case State::UpdatingRemoteTimestamp:
            if (m_StateTotalProgress.has_value())
            {
                SetProgressBarValue(ui.m_ProgressBar, *m_StateTotalProgress);
            }
            break;
        case State::UpdatingLocalContentReadingFile:
            if (m_StateTotalProgress.has_value())
            {
                SetProgressBarValue(ui.m_ProgressBar, *m_StateTotalProgress);
            }
            if (m_StateSubProgress.has_value())
            {
                SetProgressBarValue(ui.m_FileProgressBar, *m_StateSubProgress);
            }
            break;
        case State::DeletingRemote:
            if (m_StateTotalProgress.has_value())
            {
                SetProgressBarValue(ui.m_ProgressBar, *m_StateTotalProgress);
            }
            break;
        case State::RenamingRemote:
            SetProgressBarValue(ui.m_ProgressBar, 0, 1);
            break;
        case State::CreatingDirectory:
            SetProgressBarValue(ui.m_ProgressBar, 0, 1);
            break;
        case State::UploadingFile:
            if (m_StateTotalProgress.has_value())
            {
                SetProgressBarValue(ui.m_ProgressBar, *m_StateTotalProgress);
            }
            if (m_StateSubProgress.has_value())
            {
                SetProgressBarValue(ui.m_FileProgressBar, *m_StateSubProgress);
            }
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ConfigureProgressBar(QProgressBar* progressBar, bool visible, bool indeterminate)
{
    progressBar->setVisible(visible);
    progressBar->setRange(0, indeterminate ? 0 : 1000);
    progressBar->setValue(0);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetProgressBarValue(QProgressBar* progressBar, int64_t completed, int64_t total)
{
    if (total > 0)
    {
        progressBar->setValue(CalculateProgressValue(completed, total));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::SetProgressBarValue(QProgressBar* progressBar, float progress)
{
    progressBar->setValue(CalculateProgressValue(static_cast<int64_t>(progress * 1000.0f), 1000));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSync::ResetProgressCounters()
{
    m_StateTotalProgress.reset();
    m_StateSubProgress.reset();
    m_StateProgressText.clear();
    m_StateStatusText.clear();
    m_ContentCompareTotalBytes = 0;
    m_ContentCompareCompletedBytes = 0;
    m_CurrentCompareSize = 0;
    m_CurrentCompareBytesRead = 0;
    m_CurrentDiffSize = 0;
    m_CurrentDiffBytesRead = 0;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetCurrentProgressItemText() const
{
    if (m_ActiveState != nullptr) {
        return m_StateProgressText;
    }

    switch (m_State)
    {
        case State::CrawlingRemote:
            return FormatProgressItemText(m_StateProgressText);
        case State::ComparingContentReadingFile:
            return FormatProgressItemText(m_CurrentCompareRelativePath);
        case State::DiffReadingFile:
        case State::DiffToolRunning:
            return FormatProgressItemText(m_CurrentDiffRelativePath);
        case State::UpdatingRemoteTimestamp:
            return QString();
        case State::UpdatingLocalContentReadingFile:
            return QString();
        case State::DeletingRemote:
            return QString();
        case State::RenamingRemote:
            return FormatProgressItemText(m_CurrentRemoteRenameOldRelativePath);
        case State::CreatingDirectory:
            return FormatProgressItemText(m_StateProgressText);
        case State::UploadingFile:
            return QString();
        case State::Idle:
        case State::OpeningSession:
        default:
            return QString();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::FormatProgressItemText(const QString& relativePath) const
{
    if (relativePath.isEmpty())
    {
        return "/";
    }
    return relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasUploadTargets() const
{
    bool result = false;
    for (const auto& nodeEntry : m_Model.GetRootNodes())
    {
        if (HasUploadTargets(*nodeEntry.second))
        {
            result = true;
            break;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasUploadTargets(const SyncNode& node) const
{
    if (node.Status == SyncStatus::Ignored)
    {
        return false;
    }

    bool result = HasUploadTarget(node);
    if (!result)
    {
        for (const auto& childEntry : node.Children)
        {
            if (HasUploadTargets(*childEntry.second))
            {
                result = true;
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasSyncDeleteTargets() const
{
    bool result = false;
    if (ui.m_SyncDeletesCheckBox->isChecked())
    {
        result = CountSyncDeleteTargets() != 0;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasSelectedUploadTargets() const
{
    bool result = false;
    const std::set<QString> selectedPaths = GetSelectedPaths();
    if (!selectedPaths.empty())
    {
        for (const auto& nodeEntry : m_Model.GetRootNodes())
        {
            if (HasSelectedUploadTargets(*nodeEntry.second, selectedPaths))
            {
                result = true;
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasSelectedUploadTargets(const SyncNode& node, const std::set<QString>& selectedPaths) const
{
    bool result = false;
    if (selectedPaths.contains(node.RelativePath))
    {
        result = HasUploadTargets(node);
    }
    else
    {
        for (const auto& childEntry : node.Children)
        {
            if (HasSelectedUploadTargets(*childEntry.second, selectedPaths))
            {
                result = true;
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasUploadTarget(const SyncNode& node) const
{
    bool result = false;
    if (node.Status != SyncStatus::Ignored && node.Local.has_value())
    {
        if (node.Local->IsDirectory)
        {
            result = !node.Remote.has_value() || !node.Remote->IsDirectory;
        }
        else
        {
            result = node.Status != SyncStatus::Identical && node.Status != SyncStatus::Comparing;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::HasSyncDeleteTarget(const SyncNode& node) const
{
    return node.Status != SyncStatus::Ignored && !node.Local.has_value() && node.Remote.has_value() && IsDeleteSyncAllowed(node);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t SDCardSync::CountUploadFiles() const
{
    size_t result = 0;
    for (const auto& nodeEntry : m_Model.GetRootNodes())
    {
        result += CountUploadFiles(*nodeEntry.second);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t SDCardSync::CountUploadFiles(const SyncNode& node) const
{
    if (node.Status == SyncStatus::Ignored)
    {
        return 0;
    }

    size_t result = 0;
    if (node.Local.has_value() && !node.Local->IsDirectory && node.Status != SyncStatus::Identical && node.Status != SyncStatus::Comparing && node.Status != SyncStatus::Ignored)
    {
        result = 1;
    }

    for (const auto& childEntry : node.Children)
    {
        result += CountUploadFiles(*childEntry.second);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t SDCardSync::CountSyncDeleteTargets() const
{
    size_t result = 0;
    for (const auto& nodeEntry : m_Model.GetRootNodes())
    {
        result += CountSyncDeleteTargets(*nodeEntry.second);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t SDCardSync::CountSyncDeleteTargets(const SyncNode& node) const
{
    if (node.Status == SyncStatus::Ignored)
    {
        return 0;
    }

    size_t result = 0;
    bool countNode = HasSyncDeleteTarget(node);
    for (const auto& childEntry : node.Children)
    {
        const size_t childCount = CountSyncDeleteTargets(*childEntry.second);
        result += childCount;
        if (countNode && childEntry.second->Remote.has_value() && childCount == 0)
        {
            countNode = false;
        }
    }

    if (countNode)
    {
        ++result;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t SDCardSync::CountSyncDeleteFiles() const
{
    size_t result = 0;
    for (const auto& nodeEntry : m_Model.GetRootNodes())
    {
        result += CountSyncDeleteFiles(*nodeEntry.second);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

size_t SDCardSync::CountSyncDeleteFiles(const SyncNode& node) const
{
    if (node.Status == SyncStatus::Ignored)
    {
        return 0;
    }

    size_t result = 0;
    if (HasSyncDeleteTarget(node) && !node.Remote->IsDirectory)
    {
        ++result;
    }

    for (const auto& childEntry : node.Children)
    {
        result += CountSyncDeleteFiles(*childEntry.second);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::IsDeleteSyncAllowed(const SyncNode& node) const
{
    return m_DeleteSyncRules.IsAllowed(node.RelativePath, IsDirectoryNode(node));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSync::DeleteSyncRuleMode SDCardSync::GetDeleteSyncRule(const QString& relativePath) const
{
    return m_DeleteSyncRules.GetRule(relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetLocalPath(const QString& relativePath) const
{
    if (relativePath.isEmpty())
    {
        return m_LocalRootPath;
    }
    return m_LocalRootPath + "/" + relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetRemotePath(const QString& relativePath) const
{
    if (relativePath.isEmpty())
    {
        return QString::fromUtf8(DEVICE_ROOT_PATH);
    }
    return SDCardSyncFileUtils::NormalizeRemotePath(QString::fromUtf8(DEVICE_ROOT_PATH) + "/" + relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetStatusText(const SyncNode& node, bool hasSide) const
{
    switch (node.Status)
    {
        case SyncStatus::Identical:
            return "Identical";
        case SyncStatus::Different:
            return "Different";
        case SyncStatus::LocalOnly:
            return "Missing on device";
        case SyncStatus::RemoteOnly:
            return "Missing locally";
        case SyncStatus::TypeConflict:
            return "File/folder type differs";
        case SyncStatus::Comparing:
            return "Comparing content";
        case SyncStatus::Ignored:
            return hasSide ? "Ignored by .syncignore" : "Ignored by .syncignore (missing)";
        case SyncStatus::Unknown:
        default:
            return "";
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetDeleteSyncRuleActionText(const SyncNode& node, DeleteSyncRuleMode mode) const
{
    QString result;
    switch (mode)
    {
        case DeleteSyncRuleMode::Inherit:
            result = QString("Inherit from parent (%1)").arg(m_DeleteSyncRules.IsInheritedAllowed(node.RelativePath, IsDirectoryNode(node)) ? "enabled" : "disabled");
            break;
        case DeleteSyncRuleMode::Disabled:
            result = "Disabled";
            break;
        case DeleteSyncRuleMode::AllowThis:
            result = "Allow for this";
            break;
        case DeleteSyncRuleMode::AllowRecursive:
            result = "Allow for this and all sub-folders";
            break;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDCardSync::GetSerialFileReaderResultText(SerialFileReader::Result result)
{
    switch (result)
    {
        case SerialFileReader::Result::OK:
            return "ok";
        case SerialFileReader::Result::Busy:
            return "reader is already busy";
        case SerialFileReader::Result::PathTooLong:
            return "device path is too long";
        case SerialFileReader::Result::OpenFailed:
            return "failed to open device file";
        case SerialFileReader::Result::OpenedHandlerFailed:
            return "failed to prepare local file";
        case SerialFileReader::Result::ReadFailed:
            return "failed to read device file";
        case SerialFileReader::Result::UnexpectedEndOfFile:
            return "unexpected end of device file";
        case SerialFileReader::Result::DataHandlerFailed:
            return "failed to process downloaded data";
        case SerialFileReader::Result::CloseFailed:
            return "failed to close device file";
        case SerialFileReader::Result::Canceled:
            return "operation canceled";
        case SerialFileReader::Result::Timeout:
            return "device operation timed out";
    }
    return "unknown error";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int SDCardSync::CalculateProgressValue(int64_t completed, int64_t total)
{
    if (total <= 0)
    {
        return 0;
    }

    const int64_t clampedCompleted = std::max<int64_t>(0, std::min(completed, total));
    return static_cast<int>((static_cast<long double>(clampedCompleted) * 1000.0L) / static_cast<long double>(total));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::FileInfoMatches(const FileInfo& lhs, const FileInfo& rhs)
{
    return SDCardSyncModel::FileInfoMatches(lhs, rhs);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::IsDirectoryNode(const SyncNode& node)
{
    return SDCardSyncModel::IsDirectoryNode(node);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSync::CompareNodeDisplayOrder(const SyncNode* lhs, const SyncNode* rhs)
{
    return SDCardSyncModel::CompareNodeDisplayOrder(lhs, rhs);
}
