// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include <functional>

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QPoint>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialDirectoryReader.h"
#include "PadOSControl/Core/AsyncSerialHandlers/SerialFileReader.h"
#include "PadOSControl/Core/AsyncSerialHandlers/SerialRenameFile.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncDeleteRules.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncDownloadQueue.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncLocalScanner.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncUploadQueue.h"
#include "ui_SDCardSync.h"

class QTreeWidgetItem;
class QTreeWidget;
class QProgressBar;
class QTemporaryDir;
class DeviceSession;
enum class MainState : int;
enum class PLogSeverity : uint8_t;

namespace SerialProtocol
{
struct OpenSessionReply;
struct FilesystemStatusReply;
struct GetDirectoryReply;
}

class SDCardSync : public QWidget
{
    Q_OBJECT

public:
    explicit SDCardSync(QWidget* parent = nullptr);
    ~SDCardSync();

    void SetDeviceSession(DeviceSession* deviceSession);
    void SaveSettings();

protected:
    virtual bool eventFilter(QObject* watchedObject, QEvent* event) override;

private slots:
    void SlotBrowseButtonClicked();
    void SlotRefreshButtonClicked();
    void SlotUpdateButtonClicked();
    void SlotUpdateSelectedButtonClicked();
    void SlotLocalRootComboSelectionChanged(int index);
    void SlotLocalRootComboTextChanged();
    void SlotHideIdenticalChanged(Qt::CheckState state);
    void SlotHideDeviceOnlyChanged(Qt::CheckState state);
    void SlotCompareContentChanged(Qt::CheckState state);
    void SlotSyncDeletesChanged(Qt::CheckState state);
    void SlotRefreshDebounced();
    void SlotLocalItemExpanded(QTreeWidgetItem* item);
    void SlotLocalItemCollapsed(QTreeWidgetItem* item);
    void SlotDeviceItemExpanded(QTreeWidgetItem* item);
    void SlotDeviceItemCollapsed(QTreeWidgetItem* item);
    void SlotLocalSelectionChanged();
    void SlotDeviceSelectionChanged();
    void SlotTreeItemChanged(QTreeWidgetItem* item, int column);
    void SlotLocalTreeContextMenuRequested(const QPoint& position);
    void SlotDeviceTreeContextMenuRequested(const QPoint& position);
    void SlotSharedScrollValueChanged(int value);
    void SlotTreeScrollValueChanged(int value);
    void SlotTreeScrollRangeChanged(int minimum, int maximum);
    void SlotMainStateChanged(MainState state);

private:
    enum class State : int
    {
        Idle,
        OpeningSession,
        CrawlingRemote,
        ComparingContentReadingFile,
        DiffReadingFile,
        DiffToolRunning,
        UpdatingRemoteTimestamp,
        UpdatingLocalContentReadingFile,
        DeletingRemote,
        RenamingRemote,
        CreatingDirectory,
        UploadingFile
    };

    using SyncStatus = SDCardSyncModel::SyncStatus;
    using FileInfo = SDCardSyncModel::FileInfo;
    using SyncNode = SDCardSyncModel::SyncNode;
    using DeleteSyncRuleMode = SDCardSyncDeleteRules::Mode;
    using DownloadFile = SDCardSyncDownloadFile;
    using DownloadDirectory = SDCardSyncDownloadDirectory;
    using UploadFile = SDCardSyncUploadFile;
    using UploadDirectory = SDCardSyncUploadDirectory;
    using StateFinishedDelegate = std::function<void()>;

    void HandleOpenSessionReply(const SerialProtocol::OpenSessionReply& packet);
    void HandleFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet);
    void HandleStateCompleted(SDCardSyncState& state, SDCardSyncState::CompletionStatus status, const QString& statusText);
    void HandleStateProgress(SDCardSyncState& state, std::optional<float> totalProgress, std::optional<float> subProgress, const QString& progressText);
    void HandleStateStatusText(SDCardSyncState& state, const QString& statusText);

    void LoadSettings();
    void LoadDeleteSyncRules();
    bool SaveDeleteSyncRules();
    void SetDeleteSyncRule(const std::vector<QString>& relativePaths, DeleteSyncRuleMode mode);
    void AddRecentLocalRoot(const QString& path);
    void SetLocalRoot(const QString& path);
    void StartRefresh(bool readDevice = true);
    void FinishRefresh();
    void ScheduleRefresh(bool readDevice = true);
    void ClearLocalInfo(SyncNode& node);
    void RemoveEmptyNodes();
    SyncNode& FindOrCreateNode(const QString& relativePath);
    SyncNode* FindNode(const QString& relativePath);
    SDCardSyncState::Context CreateStateContext() const;
    void StartRemoteCrawl();
    void FinishRemoteCrawl();
    void AbortRemoteOperation(const QString& statusText);
    void RecalculateSyncStatus();
    void RecalculateSyncStatus(const std::set<QString>& relativePaths);
    void RecalculateSyncStatus(SyncNode& node);
    void CollectContentCompareFiles();
    void CollectContentCompareFiles(const std::set<QString>& relativePaths);
    void CollectContentCompareFiles(SyncNode& node);
    void StartNextContentCompare();
    void StartContentCompare(const SyncNode& node);
    SerialFileReader::DataResult HandleContentCompareData(int64_t startPos, const char* data, int32_t size);
    void FinishCurrentContentCompare(bool identical);
    void StartDiff(const SyncNode& node);
    SerialFileReader::DataResult HandleDiffDownloadData(int64_t startPos, const char* data, int32_t size);
    void FinishDiffDownload(SerialFileReader::Result result);
    void StartDiffTool();
    void HandleDiffProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void StartDiffRemoteUpdate();
    void FinishDiffOperation();
    void StartTimestampUpdate(const std::vector<QString>& relativePaths, QTreeWidget* targetTree, bool requireIdentical);
    void ApplyCompletedTimestampUpdate(const QString& relativePath, bool updatedLocalTimestamp, int64_t timestampNanos);
    void StartContentUpdate(const std::vector<QString>& relativePaths, QTreeWidget* targetTree);
    void StartDownloadToFolder(const std::vector<QString>& relativePaths);
    void StartUploadFiles(const SyncNode& clickedNode);
    void StartUploadFolder(const SyncNode& clickedNode);
    void StartDownloadQueue(std::unique_ptr<SDCardSyncState> state);
    void ApplyCompletedDownloadDirectory(const DownloadDirectory& downloadDirectory);
    void ApplyCompletedDownloadFile(const DownloadFile& downloadFile);
    void HandleDownloadFailure(bool applyToModel);
    void ShowDeleteConfirmation(const std::vector<QString>& relativePaths, QTreeWidget* tree);
    void StartDelete(const std::vector<QString>& relativePaths, bool deleteLocal, bool deleteRemote);
    std::unique_ptr<SDCardSyncState> CreateSyncDeleteState();
    void StartDeleteQueue(std::unique_ptr<SDCardSyncState> state, StateFinishedDelegate finished = nullptr);
    void FinishUpdateOperation();
    void ApplyCompletedLocalDelete(const QString& relativePath);
    void ApplyCompletedRemoteDelete(const QString& relativePath);
    void ApplyCompletedCreateDirectory(const QString& relativePath, const std::optional<FileInfo>& localInfo, const std::optional<FileInfo>& remoteInfo);
    void StartRename(QTreeWidgetItem* item, const QString& newName);
    bool ValidateRenameTarget(const SyncNode& node, const QString& newName, QString& newRelativePath, QString& errorText);
    bool RenameLocalNode(const SyncNode& node, const QString& newRelativePath, QString& errorText);
    void StartRemoteRename(const QString& oldRelativePath, const QString& newRelativePath, bool localRenamed);
    void FinishRemoteRename(bool success);
    bool ApplyCompletedRename(const QString& oldRelativePath, const QString& newRelativePath, bool renameLocal, bool renameRemote);
    bool RollbackLocalRename(const QString& oldRelativePath, const QString& newRelativePath, QString& errorText);
    std::map<QString, std::unique_ptr<SyncNode>>* FindNodeSiblingMap(const QString& relativePath);
    void UpdateRenamedNodePaths(SyncNode& node, const QString& oldRelativePath, const QString& newRelativePath);
    void UpdateRenamedExpandedPaths(const QString& oldRelativePath, const QString& newRelativePath);
    void StartUploadQueue(std::unique_ptr<SDCardSyncState> state, StateFinishedDelegate finished = nullptr);
    void ApplyCompletedUploadDirectory(const UploadDirectory& uploadDirectory);
    void ApplyCompletedUploadFile(const UploadFile& uploadFile);
    void RebuildTrees();
    bool AddTreeNode(const SyncNode& node, QTreeWidgetItem* localParent, QTreeWidgetItem* deviceParent);
    void PopulateTreeChildren(const SyncNode& node, QTreeWidgetItem* localParent, QTreeWidgetItem* deviceParent);
    bool NodeHasVisibleChildren(const SyncNode& node) const;
    bool ShouldShowNode(const SyncNode& node) const;
    void ApplyItemStyle(QTreeWidgetItem* item, const SyncNode& node, bool hasSide) const;
    void SetPairedItemExpanded(QTreeWidgetItem* item, bool expanded);
    void SyncTreeSelection(QTreeWidget* sourceTree, QTreeWidget* targetTree);
    void ShowTreeContextMenu(QTreeWidget* tree, const QPoint& position);
    void ShowIgnoreDialog(const std::vector<QString>& relativePaths, const SyncNode& clickedNode);
    bool AppendSyncIgnorePatterns(const QStringList& patterns);
    QStringList BuildExactIgnorePatterns(const std::vector<QString>& relativePaths);
    QStringList BuildExtensionIgnorePatterns(const std::vector<QString>& relativePaths);
    QStringList BuildParentIgnoreFolders(const SyncNode& clickedNode) const;
    QString GetExplorerPath(QTreeWidget* tree, const SyncNode& node, bool& isDirectory) const;
    QString GetUploadTargetFolder(const SyncNode& clickedNode) const;
    QStringList SelectUploadFiles();
    QStringList SelectUploadFolder();
    bool CanDiffNode(const SyncNode& node) const;
    bool CanUpdateContent(const SyncNode& node, QTreeWidget* targetTree) const;
    bool HasDeleteTargets(const std::vector<QString>& relativePaths, QTreeWidget* targetTree);
    bool HasDeleteTargets(const SyncNode& node, QTreeWidget* targetTree) const;
    bool HasDeleteTarget(const SyncNode& node, QTreeWidget* targetTree) const;
    bool HasPairedDeleteTargets(const std::vector<QString>& relativePaths);
    bool HasPairedDeleteTarget(const SyncNode& node) const;
    bool HasLocalContentUpdateTargets(const SyncNode& node) const;
    bool HasLocalContentUpdateTarget(const SyncNode& node) const;
    bool CanUpdateTimestamp(const SyncNode& node, QTreeWidget* targetTree, bool requireIdentical) const;
    std::vector<QString> GetContextActionPaths(QTreeWidget* tree, QTreeWidgetItem* clickedItem) const;
    void LogContentUpdateResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const;
    void LogDeleteResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const;
    void LogCreateDirectoryResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const;
    void LogRenameResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const;
    void LogTimestampUpdateResult(PLogSeverity severity, const QString& relativePath, const QString& resultText) const;
    std::set<QString> GetSelectedPaths() const;
    void CleanupDiffState();
    void SetState(State state);
    void SetActiveState(std::unique_ptr<SDCardSyncState> state, SDCardSyncState::StateCompletedDelegate stateCompleted = {});
    SDCardSyncState* GetActiveState() const;
    void ApplyStateProgress(std::optional<float> totalProgress, std::optional<float> subProgress, const QString& progressText);
    void UpdateSharedScrollBar();
    void UpdateControls();
    void UpdateStatusLabel();
    void UpdateProgressItemText();
    void UpdateProgressBarValues();
    void ConfigureProgressBar(QProgressBar* progressBar, bool visible, bool indeterminate);
    void SetProgressBarValue(QProgressBar* progressBar, int64_t completed, int64_t total);
    void SetProgressBarValue(QProgressBar* progressBar, float progress);
    void ResetProgressCounters();
    QString GetCurrentProgressItemText() const;
    QString FormatProgressItemText(const QString& relativePath) const;
    bool HasUploadTargets() const;
    bool HasUploadTargets(const SyncNode& node) const;
    bool HasSyncDeleteTargets() const;
    bool HasSelectedUploadTargets() const;
    bool HasSelectedUploadTargets(const SyncNode& node, const std::set<QString>& selectedPaths) const;
    bool HasUploadTarget(const SyncNode& node) const;
    bool HasSyncDeleteTarget(const SyncNode& node) const;
    size_t CountUploadFiles() const;
    size_t CountUploadFiles(const SyncNode& node) const;
    size_t CountSyncDeleteTargets() const;
    size_t CountSyncDeleteTargets(const SyncNode& node) const;
    size_t CountSyncDeleteFiles() const;
    size_t CountSyncDeleteFiles(const SyncNode& node) const;
    bool IsDeleteSyncAllowed(const SyncNode& node) const;
    DeleteSyncRuleMode GetDeleteSyncRule(const QString& relativePath) const;
    QString GetLocalPath(const QString& relativePath) const;
    QString GetRemotePath(const QString& relativePath) const;
    QString GetStatusText(const SyncNode& node, bool hasSide) const;
    QString GetDeleteSyncRuleActionText(const SyncNode& node, DeleteSyncRuleMode mode) const;
    static QString GetSerialFileReaderResultText(SerialFileReader::Result result);
    static int CalculateProgressValue(int64_t completed, int64_t total);

    static bool FileInfoMatches(const FileInfo& lhs, const FileInfo& rhs);
    static bool IsDirectoryNode(const SyncNode& node);
    static bool CompareNodeDisplayOrder(const SyncNode* lhs, const SyncNode* rhs);

    Ui::SDCardSync ui;

    DeviceSession* m_DeviceSession = nullptr;
    State           m_State = State::Idle;
    std::unique_ptr<SDCardSyncState> m_ActiveState;

    QTimer             m_RefreshDebounceTimer;
    QTimer             m_ScrollUpdateTimer;

    SDCardSyncModel m_Model;
    std::map<QTreeWidgetItem*, QTreeWidgetItem*> m_ItemPairs;
    std::map<QTreeWidgetItem*, QString>          m_ItemPaths;
    std::set<QString>                            m_ExpandedPaths;
    SDCardSyncDeleteRules                        m_DeleteSyncRules;
    SDCardSyncLocalScanner                       m_LocalScanner;
    std::unique_ptr<SerialFileReader>            m_CurrentFileReader;
    std::unique_ptr<SerialRenameFile>            m_CurrentRenameFile;

    QString m_LocalRootPath;
    int32_t m_SessionID = -1;
    uint32_t m_ClientToken = 0;
    bool    m_SessionRequestPending = false;
    bool    m_RefreshAfterSession = false;
    bool    m_RefreshAfterSessionReadDevice = true;
    bool    m_RefreshPending = false;
    bool    m_RefreshPendingReadDevice = false;
    bool    m_DebouncedRefreshReadDevice = false;
    bool    m_UpdatingScrollBars = false;
    bool    m_UpdatingTreeExpansion = false;
    bool    m_UpdatingTreeSelection = false;
    bool    m_UpdatingTreeItemText = false;
    bool    m_DeviceReadComplete = false;
    QString m_StatusOverride;

    std::optional<float> m_StateTotalProgress;
    std::optional<float> m_StateSubProgress;
    QString              m_StateProgressText;
    QString              m_StateStatusText;

    std::vector<QString> m_ContentCompareQueue;
    QString              m_CurrentCompareRelativePath;
    QFile                m_CurrentCompareLocalFile;
    bool                 m_CurrentCompareIdentical = true;
    int64_t              m_ContentCompareTotalBytes = 0;
    int64_t              m_ContentCompareCompletedBytes = 0;
    int64_t              m_CurrentCompareSize = 0;
    int64_t              m_CurrentCompareBytesRead = 0;

    QString              m_CurrentDiffRelativePath;
    QString              m_CurrentDiffLocalPath;
    QFile                m_CurrentDiffRemoteFile;
    QString              m_CurrentDiffRemoteTempPath;
    QString              m_CurrentDiffOutputPath;
    QString              m_CurrentDiffUpdateSourcePath;
    QString              m_CurrentDiffDownloadError;
    int64_t              m_CurrentDiffOutputInitialModificationTimeNanos = 0;
    QByteArray           m_CurrentDiffOriginalRemoteHash;
    std::unique_ptr<QTemporaryDir> m_DiffTempDirectory;
    QProcess*            m_DiffProcess = nullptr;
    bool                 m_CleanupDiffTempAfterUpload = false;
    int64_t              m_CurrentDiffSize = 0;
    int64_t              m_CurrentDiffBytesRead = 0;

    QString m_CurrentRemoteRenameOldRelativePath;
    QString m_CurrentRemoteRenameNewRelativePath;
    bool    m_CurrentRemoteRenameLocalRenamed = false;
    SDCardSyncLocalScanner::WatchPaths m_CurrentRemoteRenameWatchPaths;

    QString                 m_LastDownloadPath;
    QString                 m_LastUploadPath;
    size_t                  m_UpdateFailedCount = 0;
};
