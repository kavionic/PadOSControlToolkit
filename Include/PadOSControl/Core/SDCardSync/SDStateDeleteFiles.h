// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <QObject>
#include <QString>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialDeleteFile.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncDeleteRules.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncLocalScanner.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"

enum class PLogSeverity : uint8_t;

class SDStateDeleteFiles : public SDCardSyncState
{
public:
    using SyncNode = SDCardSyncModel::SyncNode;
    using SyncStatus = SDCardSyncModel::SyncStatus;
    using LogDelegate = std::function<void(PLogSeverity severity, const QString& relativePath, const QString& resultText)>;
    using PathCompletedDelegate = std::function<void(const QString& relativePath)>;
    using FailureDelegate = std::function<void(const QString& statusText)>;

    SDStateDeleteFiles(
        Context context,
        SDCardSyncModel& model,
        SDCardSyncLocalScanner& localScanner,
        const QString& localRootPath,
        const QString& remoteRootPath,
        const std::set<QString>& selectedPaths,
        bool deleteLocal,
        bool deleteRemote,
        PathCompletedDelegate localDeleted,
        PathCompletedDelegate remoteDeleted,
        LogDelegate logResult,
        FailureDelegate failed
    );
    SDStateDeleteFiles(
        Context context,
        const SDCardSyncModel& model,
        const QString& remoteRootPath,
        const SDCardSyncDeleteRules& deleteRules,
        PathCompletedDelegate remoteDeleted,
        LogDelegate logResult,
        FailureDelegate failed
    );

    virtual void Start() override;
    virtual void Cancel() override;
    virtual QString GetStatusText() const override;

private:
    struct DeleteTask
    {
        QString RelativePath;
        bool IsDirectory = false;
    };

    struct LocalWatchRemoval
    {
        QString LocalPath;
        SDCardSyncLocalScanner::WatchPaths WatchPaths;
    };

    void StartNextDelete();
    void StartCurrentRemoteDelete();
    void FinishCurrentRemoteDelete(bool success);
    void RemoveLocalWatchPaths();
    void RestoreLocalWatchPaths();
    void ScheduleNextLocalDelete();
    void DeleteNextLocalFile();
    bool DeleteLocalFile(const DeleteTask& deleteTask, QString& errorText);
    bool DeleteLocalDirectory(const QString& localPath, QString& errorText);
    bool DeleteLocalRegularFile(const QString& localPath, QString& errorText);
    void ClearCompletedLocalDelete(const QString& relativePath);
    void UpdateProgress();
    void UpdateStatusText();
    void BuildSelectedDeleteQueue(const SDCardSyncModel& model, const std::set<QString>& selectedPaths, bool deleteLocal, bool deleteRemote);
    void BuildSyncDeleteQueue(const SDCardSyncModel& model, const SDCardSyncDeleteRules& deleteRules);
    void CollectSelectedDeleteTargets(const SyncNode& node, const std::set<QString>& selectedPaths, bool deleteLocal, bool deleteRemote);
    void CollectDeleteTargets(const SyncNode& node, bool deleteLocal, bool deleteRemote);
    void CollectLocalDeleteTargets(const SyncNode& node);
    bool CollectSyncDeleteTargets(const SyncNode& node, const SDCardSyncDeleteRules& deleteRules);
    void AddRemoteDeleteTask(const SyncNode& node);
    void AddLocalDeleteTask(const SyncNode& node);
    void AddLocalWatchRemovalTarget(const SyncNode& node);
    QString GetLocalPath(const QString& relativePath) const;
    QString GetRemotePath(const QString& relativePath) const;
    QString GetProgressText() const;
    static bool IsDirectoryNode(const SyncNode& node);
    static std::optional<float> CalculateProgress(int64_t completed, int64_t total);

    SDCardSyncModel*        m_Model = nullptr;
    SDCardSyncLocalScanner* m_LocalScanner = nullptr;
    std::vector<DeleteTask> m_LocalDeleteQueue;
    std::vector<DeleteTask> m_RemoteDeleteQueue;
    std::vector<DeleteTask> m_LocalWatchRemovalTargets;
    std::vector<LocalWatchRemoval> m_LocalWatchRemovals;
    QString                 m_LocalRootPath;
    QString                 m_RemoteRootPath;
    PathCompletedDelegate   m_LocalDeleted;
    PathCompletedDelegate   m_RemoteDeleted;
    LogDelegate             m_LogResult;
    FailureDelegate         m_Failed;
    std::unique_ptr<SerialDeleteFile> m_CurrentDeleteFile;
    QObject m_LocalDeleteTimerContext;
    bool    m_Canceled = false;
    size_t  m_CurrentLocalDeleteIndex = 0;
    size_t  m_CurrentRemoteDeleteIndex = 0;
    int64_t m_DeleteTotalCount = 0;
    int64_t m_DeleteCompletedCount = 0;
};
