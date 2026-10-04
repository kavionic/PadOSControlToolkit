// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDStateDeleteFiles.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"

#ifdef ERROR
#undef ERROR
#endif
#include <Utils/LogSeverity.h>

#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateDeleteFiles::SDStateDeleteFiles(
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
)
    : SDCardSyncState(std::move(context))
    , m_Model(&model)
    , m_LocalScanner(&localScanner)
    , m_LocalRootPath(localRootPath)
    , m_RemoteRootPath(remoteRootPath)
    , m_LocalDeleted(std::move(localDeleted))
    , m_RemoteDeleted(std::move(remoteDeleted))
    , m_LogResult(std::move(logResult))
    , m_Failed(std::move(failed))
{
    BuildSelectedDeleteQueue(model, selectedPaths, deleteLocal, deleteRemote);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateDeleteFiles::SDStateDeleteFiles(
    Context context,
    const SDCardSyncModel& model,
    const QString& remoteRootPath,
    const SDCardSyncDeleteRules& deleteRules,
    PathCompletedDelegate remoteDeleted,
    LogDelegate logResult,
    FailureDelegate failed
)
    : SDCardSyncState(std::move(context))
    , m_RemoteRootPath(remoteRootPath)
    , m_RemoteDeleted(std::move(remoteDeleted))
    , m_LogResult(std::move(logResult))
    , m_Failed(std::move(failed))
{
    BuildSyncDeleteQueue(model, deleteRules);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::Start()
{
    m_Canceled = false;
    m_CurrentDeleteFile.reset();
    m_CurrentLocalDeleteIndex = 0;
    m_CurrentRemoteDeleteIndex = 0;
    m_DeleteTotalCount = static_cast<int64_t>(m_LocalDeleteQueue.size() + m_RemoteDeleteQueue.size());
    m_DeleteCompletedCount = 0;
    UpdateStatusText();
    UpdateProgress();
    RemoveLocalWatchPaths();
    ScheduleNextLocalDelete();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::Cancel()
{
    m_Canceled = true;
    RestoreLocalWatchPaths();
    m_CurrentDeleteFile.reset();
    SDCardSyncState::Cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDeleteFiles::GetStatusText() const
{
    if (m_CurrentLocalDeleteIndex < m_LocalDeleteQueue.size()) {
        return QString("Deleting PC item %1/%2...").arg(m_CurrentLocalDeleteIndex + 1).arg(m_LocalDeleteQueue.size());
    }
    if (m_CurrentRemoteDeleteIndex < m_RemoteDeleteQueue.size()) {
        return QString("Deleting device item %1/%2...").arg(m_CurrentRemoteDeleteIndex + 1).arg(m_RemoteDeleteQueue.size());
    }
    return "Deleting item...";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::StartNextDelete()
{
    if (m_CurrentRemoteDeleteIndex < m_RemoteDeleteQueue.size())
    {
        StartCurrentRemoteDelete();
    }
    else
    {
        SetProgress(1.0f, std::nullopt, QString());
        FinishSucceeded();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::StartCurrentRemoteDelete()
{
    m_CurrentDeleteFile = std::make_unique<SerialDeleteFile>(GetSerialHandler(), GetSessionID());
    m_CurrentDeleteFile->SetFinishedHandler([this](SerialDeleteFile::Result result)
    {
        m_CurrentDeleteFile.reset();
        FinishCurrentRemoteDelete(result == SerialDeleteFile::Result::OK);
    });

    UpdateStatusText();
    UpdateProgress();
    const SerialDeleteFile::Result startResult = m_CurrentDeleteFile->Start(GetRemotePath(m_RemoteDeleteQueue[m_CurrentRemoteDeleteIndex].RelativePath));
    if (startResult != SerialDeleteFile::Result::OK)
    {
        m_CurrentDeleteFile.reset();
        FinishCurrentRemoteDelete(false);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::FinishCurrentRemoteDelete(bool success)
{
    const DeleteTask& deleteTask = m_RemoteDeleteQueue[m_CurrentRemoteDeleteIndex];
    if (success)
    {
        if (m_RemoteDeleted) {
            m_RemoteDeleted(deleteTask.RelativePath);
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, deleteTask.RelativePath, "deleted from device");
        }
    }
    else
    {
        if (m_Failed) {
            m_Failed("Failed to delete device file");
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, deleteTask.RelativePath, "failed to delete from device");
        }
    }

    ++m_DeleteCompletedCount;
    ++m_CurrentRemoteDeleteIndex;
    UpdateStatusText();
    UpdateProgress();
    StartNextDelete();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::RemoveLocalWatchPaths()
{
    m_LocalWatchRemovals.clear();
    if (m_LocalScanner != nullptr)
    {
        for (const DeleteTask& deleteTask : m_LocalWatchRemovalTargets)
        {
            LocalWatchRemoval watchRemoval;
            watchRemoval.LocalPath = GetLocalPath(deleteTask.RelativePath);
            watchRemoval.WatchPaths = m_LocalScanner->RemoveWatchedPathsInTree(watchRemoval.LocalPath);
            m_LocalWatchRemovals.push_back(watchRemoval);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::RestoreLocalWatchPaths()
{
    if (m_LocalScanner != nullptr)
    {
        for (const LocalWatchRemoval& watchRemoval : m_LocalWatchRemovals)
        {
            m_LocalScanner->RestoreWatchedPaths(watchRemoval.WatchPaths, watchRemoval.LocalPath, watchRemoval.LocalPath);
        }
    }
    m_LocalWatchRemovals.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::ScheduleNextLocalDelete()
{
    UpdateStatusText();
    UpdateProgress();
    QTimer::singleShot(0, &m_LocalDeleteTimerContext, [this]()
    {
        DeleteNextLocalFile();
    });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::DeleteNextLocalFile()
{
    if (m_Canceled) {
        return;
    }

    if (m_CurrentLocalDeleteIndex >= m_LocalDeleteQueue.size())
    {
        RestoreLocalWatchPaths();
        StartNextDelete();
        return;
    }

    const DeleteTask& deleteTask = m_LocalDeleteQueue[m_CurrentLocalDeleteIndex];
    QString errorText;
    const bool success = DeleteLocalFile(deleteTask, errorText);
    if (success)
    {
        ClearCompletedLocalDelete(deleteTask.RelativePath);
        if (m_LocalDeleted) {
            m_LocalDeleted(deleteTask.RelativePath);
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, deleteTask.RelativePath, "deleted from PC");
        }
    }
    else
    {
        if (m_Failed) {
            m_Failed("Failed to delete local file");
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, deleteTask.RelativePath, errorText);
        }
    }

    ++m_DeleteCompletedCount;
    ++m_CurrentLocalDeleteIndex;
    ScheduleNextLocalDelete();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDeleteFiles::DeleteLocalFile(const DeleteTask& deleteTask, QString& errorText)
{
    const QString localPath = GetLocalPath(deleteTask.RelativePath);
    if (deleteTask.IsDirectory)
    {
        return DeleteLocalDirectory(localPath, errorText);
    }
    return DeleteLocalRegularFile(localPath, errorText);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDeleteFiles::DeleteLocalDirectory(const QString& localPath, QString& errorText)
{
    bool result = false;
    QDir parentDirectory(QFileInfo(localPath).absolutePath());
    if (parentDirectory.rmdir(QFileInfo(localPath).fileName()))
    {
        result = true;
    }
    else
    {
        errorText = "failed to delete local folder";
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDeleteFiles::DeleteLocalRegularFile(const QString& localPath, QString& errorText)
{
    bool result = QFile::moveToTrash(localPath);
    if (!result)
    {
        errorText = "failed to move local file to trash";
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::ClearCompletedLocalDelete(const QString& relativePath)
{
    if (m_Model != nullptr)
    {
        SyncNode* node = m_Model->FindNode(relativePath);
        if (node != nullptr)
        {
            m_Model->ClearLocalInfo(*node);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::UpdateProgress()
{
    SetProgress(CalculateProgress(m_DeleteCompletedCount, m_DeleteTotalCount), std::nullopt, GetProgressText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::UpdateStatusText()
{
    SetStatusText(GetStatusText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::BuildSelectedDeleteQueue(const SDCardSyncModel& model, const std::set<QString>& selectedPaths, bool deleteLocal, bool deleteRemote)
{
    m_LocalDeleteQueue.clear();
    m_RemoteDeleteQueue.clear();

    if (!selectedPaths.empty())
    {
        for (const auto& nodeEntry : model.GetRootNodes()) {
            CollectSelectedDeleteTargets(*nodeEntry.second, selectedPaths, deleteLocal, deleteRemote);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::BuildSyncDeleteQueue(const SDCardSyncModel& model, const SDCardSyncDeleteRules& deleteRules)
{
    m_LocalDeleteQueue.clear();
    m_RemoteDeleteQueue.clear();

    for (const auto& nodeEntry : model.GetRootNodes()) {
        CollectSyncDeleteTargets(*nodeEntry.second, deleteRules);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::CollectSelectedDeleteTargets(const SyncNode& node, const std::set<QString>& selectedPaths, bool deleteLocal, bool deleteRemote)
{
    if (selectedPaths.contains(node.RelativePath))
    {
        CollectDeleteTargets(node, deleteLocal, deleteRemote);
    }
    else
    {
        for (const auto& childEntry : node.Children) {
            CollectSelectedDeleteTargets(*childEntry.second, selectedPaths, deleteLocal, deleteRemote);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::CollectDeleteTargets(const SyncNode& node, bool deleteLocal, bool deleteRemote)
{
    if (deleteRemote && node.Remote.has_value())
    {
        for (const auto& childEntry : node.Children) {
            CollectDeleteTargets(*childEntry.second, false, true);
        }
        AddRemoteDeleteTask(node);
    }

    if (deleteLocal && node.Local.has_value())
    {
        AddLocalWatchRemovalTarget(node);
        CollectLocalDeleteTargets(node);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::CollectLocalDeleteTargets(const SyncNode& node)
{
    if (node.Local.has_value() && node.Local->IsDirectory)
    {
        for (const auto& childEntry : node.Children)
        {
            CollectLocalDeleteTargets(*childEntry.second);
        }
    }
    AddLocalDeleteTask(node);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDeleteFiles::CollectSyncDeleteTargets(const SyncNode& node, const SDCardSyncDeleteRules& deleteRules)
{
    if (node.Status == SyncStatus::Ignored) {
        return false;
    }

    bool result = node.Status != SyncStatus::Ignored && !node.Local.has_value() && node.Remote.has_value() && deleteRules.IsAllowed(node.RelativePath, IsDirectoryNode(node));
    for (const auto& childEntry : node.Children)
    {
        const bool childCanBeDeleted = CollectSyncDeleteTargets(*childEntry.second, deleteRules);
        if (result && childEntry.second->Remote.has_value() && !childCanBeDeleted)
        {
            result = false;
        }
    }
    if (result) {
        AddRemoteDeleteTask(node);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::AddRemoteDeleteTask(const SyncNode& node)
{
    if (node.Remote.has_value())
    {
        DeleteTask deleteTask;
        deleteTask.RelativePath = node.RelativePath;
        deleteTask.IsDirectory = node.Remote->IsDirectory;
        m_RemoteDeleteQueue.push_back(deleteTask);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::AddLocalDeleteTask(const SyncNode& node)
{
    if (node.Local.has_value())
    {
        DeleteTask deleteTask;
        deleteTask.RelativePath = node.RelativePath;
        deleteTask.IsDirectory = node.Local->IsDirectory;
        m_LocalDeleteQueue.push_back(deleteTask);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDeleteFiles::AddLocalWatchRemovalTarget(const SyncNode& node)
{
    DeleteTask deleteTask;
    deleteTask.RelativePath = node.RelativePath;
    deleteTask.IsDirectory = node.Local.has_value() && node.Local->IsDirectory;
    m_LocalWatchRemovalTargets.push_back(deleteTask);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDeleteFiles::GetLocalPath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_LocalRootPath;
    }
    return m_LocalRootPath + "/" + relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDeleteFiles::GetRemotePath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_RemoteRootPath;
    }
    return SDCardSyncFileUtils::NormalizeRemotePath(m_RemoteRootPath + "/" + relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDeleteFiles::GetProgressText() const
{
    if (m_CurrentLocalDeleteIndex < m_LocalDeleteQueue.size()) {
        return m_LocalDeleteQueue[m_CurrentLocalDeleteIndex].RelativePath;
    }
    if (m_CurrentRemoteDeleteIndex < m_RemoteDeleteQueue.size()) {
        return m_RemoteDeleteQueue[m_CurrentRemoteDeleteIndex].RelativePath;
    }
    return QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDeleteFiles::IsDirectoryNode(const SyncNode& node)
{
    return (node.Local.has_value() && node.Local->IsDirectory) || (node.Remote.has_value() && node.Remote->IsDirectory);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<float> SDStateDeleteFiles::CalculateProgress(int64_t completed, int64_t total)
{
    if (total <= 0) {
        return std::nullopt;
    }
    return static_cast<float>(static_cast<double>(completed) / static_cast<double>(total));
}
