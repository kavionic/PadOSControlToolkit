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
#include <vector>

#include <QFile>
#include <QString>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialFileReader.h"
#include "PadOSControl/Core/AsyncSerialHandlers/SerialSetFileStat.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"

enum class PLogSeverity : uint8_t;

class SDStateTimestampUpdate : public SDCardSyncState
{
public:
    using SyncNode = SDCardSyncModel::SyncNode;
    using SyncStatus = SDCardSyncModel::SyncStatus;
    using TimestampUpdatedDelegate = std::function<void(const QString& relativePath, bool updatedLocalTimestamp, int64_t timestampNanos)>;
    using LogDelegate = std::function<void(PLogSeverity severity, const QString& relativePath, const QString& resultText)>;
    using FailureDelegate = std::function<void(const QString& statusText)>;

    SDStateTimestampUpdate(
        Context context,
        const SDCardSyncModel& model,
        const QString& localRootPath,
        const QString& remoteRootPath,
        const std::vector<QString>& relativePaths,
        bool updateLocalTimestamp,
        bool requireIdentical,
        bool connected,
        TimestampUpdatedDelegate timestampUpdated,
        LogDelegate logResult,
        FailureDelegate failed
    );

    virtual void Start() override;
    virtual void Cancel() override;
    virtual QString GetStatusText() const override;

private:
    enum class Target : int
    {
        Local,
        Remote
    };

    enum class Operation : int
    {
        Idle,
        ComparingContent,
        UpdatingLocalTimestamp,
        UpdatingRemoteTimestamp
    };

    struct Task
    {
        QString RelativePath;
        Target TargetSide = Target::Remote;
        bool RequireIdentical = false;
        int64_t TimestampNanos = 0;
        int64_t Size = 0;
    };

    void StartNextTimestampUpdate();
    void StartContentCompare(const Task& task);
    SerialFileReader::DataResult HandleContentCompareData(int64_t startPos, const char* data, int32_t size);
    void FinishContentCompare(SerialFileReader::Result result);
    void StartLocalTimestampUpdate(const Task& task);
    void StartRemoteTimestampUpdate(const Task& task);
    void FinishRemoteTimestampUpdate(bool success);
    void CompleteCurrentTask();
    void UpdateProgress();
    void UpdateStatusText();
    void BuildTimestampUpdateQueue(const std::vector<QString>& relativePaths, bool requireIdentical, bool connected);
    void CollectTimestampUpdateTargets(const SyncNode& node, bool requireIdentical, bool connected);
    void AddTimestampUpdateTask(const SyncNode& node, bool requireIdentical, bool connected);
    bool CanApplyTimestamp(const SyncNode& node, Target target, bool requireIdentical, bool connected) const;
    QString GetLocalPath(const QString& relativePath) const;
    QString GetRemotePath(const QString& relativePath) const;
    QString GetProgressText() const;
    static QString GetSerialFileReaderResultText(SerialFileReader::Result result);
    static std::optional<float> CalculateProgress(int64_t completed, int64_t total);

    const SDCardSyncModel* m_Model = nullptr;
    Target m_Target = Target::Remote;
    QString m_LocalRootPath;
    QString m_RemoteRootPath;
    TimestampUpdatedDelegate m_TimestampUpdated;
    LogDelegate m_LogResult;
    FailureDelegate m_Failed;
    std::vector<Task> m_TimestampUpdateQueue;
    std::optional<Task> m_CurrentTask;
    std::unique_ptr<SerialFileReader> m_CurrentFileReader;
    std::unique_ptr<SerialSetFileStat> m_CurrentSetFileStat;
    QFile m_CurrentCompareLocalFile;
    Operation m_Operation = Operation::Idle;
    size_t m_CurrentTaskIndex = 0;
    int64_t m_TimestampUpdateTotalCount = 0;
    int64_t m_TimestampUpdateCompletedCount = 0;
    int64_t m_CurrentCompareSize = 0;
    int64_t m_CurrentCompareBytesRead = 0;
    bool m_CurrentCompareIdentical = true;
};
