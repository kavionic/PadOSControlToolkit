// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDStateTimestampUpdate.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"
#include "SerialConsole/FilesystemMessages.h"

#ifdef ERROR
#undef ERROR
#endif
#include <Utils/LogSeverity.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <QDateTime>
#include <QFileInfo>
#include <QTimeZone>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateTimestampUpdate::SDStateTimestampUpdate(
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
)
    : SDCardSyncState(std::move(context))
    , m_Model(&model)
    , m_Target(updateLocalTimestamp ? Target::Local : Target::Remote)
    , m_LocalRootPath(localRootPath)
    , m_RemoteRootPath(remoteRootPath)
    , m_TimestampUpdated(std::move(timestampUpdated))
    , m_LogResult(std::move(logResult))
    , m_Failed(std::move(failed))
{
    BuildTimestampUpdateQueue(relativePaths, requireIdentical, connected);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::Start()
{
    m_CurrentFileReader.reset();
    m_CurrentSetFileStat.reset();
    m_CurrentCompareLocalFile.close();
    m_CurrentTask.reset();
    m_Operation = Operation::Idle;
    m_CurrentTaskIndex = 0;
    m_TimestampUpdateTotalCount = static_cast<int64_t>(m_TimestampUpdateQueue.size());
    m_TimestampUpdateCompletedCount = 0;
    m_CurrentCompareSize = 0;
    m_CurrentCompareBytesRead = 0;
    m_CurrentCompareIdentical = true;
    UpdateStatusText();
    UpdateProgress();
    StartNextTimestampUpdate();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::Cancel()
{
    m_CurrentFileReader.reset();
    m_CurrentSetFileStat.reset();
    m_CurrentCompareLocalFile.close();
    SDCardSyncState::Cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateTimestampUpdate::GetStatusText() const
{
    if (m_Operation == Operation::ComparingContent) {
        return "Checking file content...";
    }
    if (m_CurrentTaskIndex < m_TimestampUpdateQueue.size() && m_TimestampUpdateTotalCount > 0) {
        return QString("Updating timestamp %1/%2...").arg(m_CurrentTaskIndex + 1).arg(m_TimestampUpdateTotalCount);
    }
    return "Updating timestamp...";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::StartNextTimestampUpdate()
{
    m_CurrentFileReader.reset();
    m_CurrentSetFileStat.reset();
    m_CurrentCompareLocalFile.close();
    m_CurrentTask.reset();
    m_Operation = Operation::Idle;
    m_CurrentCompareSize = 0;
    m_CurrentCompareBytesRead = 0;
    m_CurrentCompareIdentical = true;

    if (m_CurrentTaskIndex >= m_TimestampUpdateQueue.size())
    {
        SetProgress(1.0f, std::nullopt, QString());
        FinishSucceeded();
        return;
    }

    m_CurrentTask = m_TimestampUpdateQueue[m_CurrentTaskIndex];
    const SyncNode* node = (m_Model != nullptr) ? m_Model->FindNode(m_CurrentTask->RelativePath) : nullptr;
    if (node == nullptr)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, m_CurrentTask->RelativePath, "skipped: file disappeared");
        }
        CompleteCurrentTask();
        return;
    }

    UpdateStatusText();
    UpdateProgress();
    if (m_CurrentTask->RequireIdentical)
    {
        StartContentCompare(*m_CurrentTask);
    }
    else if (m_CurrentTask->TargetSide == Target::Local)
    {
        StartLocalTimestampUpdate(*m_CurrentTask);
    }
    else
    {
        StartRemoteTimestampUpdate(*m_CurrentTask);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::StartContentCompare(const Task& task)
{
    m_CurrentCompareLocalFile.close();
    m_CurrentCompareLocalFile.setFileName(GetLocalPath(task.RelativePath));
    m_CurrentCompareIdentical = true;
    m_CurrentCompareSize = std::max<int64_t>(1, task.Size);
    m_CurrentCompareBytesRead = 0;
    m_Operation = Operation::ComparingContent;
    UpdateStatusText();
    UpdateProgress();

    if (!m_CurrentCompareLocalFile.open(QFile::ReadOnly))
    {
        m_CurrentCompareIdentical = false;
        FinishContentCompare(SerialFileReader::Result::OK);
        return;
    }

    const QByteArray utf8Path = GetRemotePath(task.RelativePath).toUtf8();
    if (utf8Path.size() >= SDCardSyncFileUtils::MaxFilesystemPathBytes)
    {
        m_CurrentCompareIdentical = false;
        FinishContentCompare(SerialFileReader::Result::OK);
        return;
    }

    m_CurrentFileReader = std::make_unique<SerialFileReader>(GetSerialHandler(), GetSessionID());
    m_CurrentFileReader->SetDataReadyHandler([this](int64_t startPos, const char* data, int32_t size)
    {
        return HandleContentCompareData(startPos, data, size);
    });
    m_CurrentFileReader->SetFinishedHandler([this](SerialFileReader::Result result)
    {
        m_CurrentFileReader.reset();
        FinishContentCompare(result);
    });

    const SerialFileReader::Result startResult = m_CurrentFileReader->Start(GetRemotePath(task.RelativePath), task.Size);
    if (startResult != SerialFileReader::Result::OK)
    {
        m_CurrentFileReader.reset();
        FinishContentCompare(startResult);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::DataResult SDStateTimestampUpdate::HandleContentCompareData(int64_t startPos, const char* data, int32_t size)
{
    SerialFileReader::DataResult result = SerialFileReader::DataResult::Continue;
    m_CurrentCompareBytesRead = std::max(m_CurrentCompareBytesRead, startPos + size);
    UpdateProgress();

    if (m_CurrentCompareIdentical)
    {
        if (m_CurrentCompareLocalFile.seek(startPos))
        {
            const QByteArray localData = m_CurrentCompareLocalFile.read(size);
            if (localData.size() == size)
            {
                m_CurrentCompareIdentical = std::memcmp(localData.constData(), data, size) == 0;
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

void SDStateTimestampUpdate::FinishContentCompare(SerialFileReader::Result result)
{
    m_CurrentCompareLocalFile.close();
    const bool identical = result == SerialFileReader::Result::OK && m_CurrentCompareIdentical;

    if (result != SerialFileReader::Result::OK && result != SerialFileReader::Result::DataHandlerFailed && m_Failed) {
        m_Failed(QString("Failed to compare file content: %1").arg(GetSerialFileReaderResultText(result)));
    }

    if (!m_CurrentTask.has_value())
    {
        FinishFailed("Timestamp update task ended unexpectedly");
        return;
    }

    const SyncNode* node = (m_Model != nullptr) ? m_Model->FindNode(m_CurrentTask->RelativePath) : nullptr;
    if (node == nullptr)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, m_CurrentTask->RelativePath, "skipped: file disappeared");
        }
    }
    else if (identical)
    {
        if (m_CurrentTask->TargetSide == Target::Local)
        {
            StartLocalTimestampUpdate(*m_CurrentTask);
        }
        else
        {
            StartRemoteTimestampUpdate(*m_CurrentTask);
        }
        return;
    }
    else if (m_LogResult)
    {
        m_LogResult(PLogSeverity::WARNING, m_CurrentTask->RelativePath, "skipped: content differs");
    }

    CompleteCurrentTask();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::StartLocalTimestampUpdate(const Task& task)
{
    const SyncNode* node = (m_Model != nullptr) ? m_Model->FindNode(task.RelativePath) : nullptr;
    if (node == nullptr)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, task.RelativePath, "skipped: file disappeared");
        }
        CompleteCurrentTask();
        return;
    }
    if (!CanApplyTimestamp(*node, Target::Local, false, true))
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::WARNING, task.RelativePath, "skipped: local timestamp update is not available");
        }
        CompleteCurrentTask();
        return;
    }

    m_Operation = Operation::UpdatingLocalTimestamp;
    UpdateStatusText();
    UpdateProgress();

    const QString localPath = GetLocalPath(task.RelativePath);
    QFile localFile(localPath);
    const QDateTime timestamp = QDateTime::fromMSecsSinceEpoch(task.TimestampNanos / SDCardSyncFileUtils::NanosecondsPerMillisecond, QTimeZone(Qt::UTC));
    QString resultText;
    bool success = false;
    if (localFile.open(QFile::ReadWrite))
    {
        if (localFile.setFileTime(timestamp, QFileDevice::FileModificationTime))
        {
            success = true;
        }
        else
        {
            resultText = QString("failed to update local timestamp: %1").arg(localFile.errorString());
        }
    }
    else
    {
        resultText = QString("failed to open local file: %1").arg(localFile.errorString());
    }

    if (success)
    {
        if (m_TimestampUpdated) {
            m_TimestampUpdated(task.RelativePath, true, task.TimestampNanos);
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, task.RelativePath, "updated local timestamp");
        }
    }
    else
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, task.RelativePath, resultText);
        }
        if (m_Failed) {
            m_Failed("Failed to update local timestamp");
        }
    }
    CompleteCurrentTask();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::StartRemoteTimestampUpdate(const Task& task)
{
    const SyncNode* node = (m_Model != nullptr) ? m_Model->FindNode(task.RelativePath) : nullptr;
    if (node == nullptr)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, task.RelativePath, "skipped: file disappeared");
        }
        CompleteCurrentTask();
        return;
    }
    if (!CanApplyTimestamp(*node, Target::Remote, false, true))
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::WARNING, task.RelativePath, "skipped: device timestamp update is not available");
        }
        CompleteCurrentTask();
        return;
    }

    m_Operation = Operation::UpdatingRemoteTimestamp;
    m_CurrentTask->TimestampNanos = SDCardSyncFileUtils::RoundToFATModificationTime(task.TimestampNanos);
    UpdateStatusText();
    UpdateProgress();

    m_CurrentSetFileStat = std::make_unique<SerialSetFileStat>(GetSerialHandler(), GetSessionID());
    m_CurrentSetFileStat->SetFinishedHandler([this](SerialSetFileStat::Result result)
    {
        m_CurrentSetFileStat.reset();
        FinishRemoteTimestampUpdate(result == SerialSetFileStat::Result::OK);
    });
    const SerialSetFileStat::Result startResult = m_CurrentSetFileStat->Start(
        GetRemotePath(task.RelativePath),
        SerialProtocol::FilesystemStatMask::ModificationTime,
        0,
        0,
        0,
        m_CurrentTask->TimestampNanos,
        0
    );
    if (startResult != SerialSetFileStat::Result::OK)
    {
        m_CurrentSetFileStat.reset();
        FinishRemoteTimestampUpdate(false);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::FinishRemoteTimestampUpdate(bool success)
{
    if (!m_CurrentTask.has_value())
    {
        FinishFailed("Timestamp update task ended unexpectedly");
        return;
    }

    if (success)
    {
        if (m_TimestampUpdated) {
            m_TimestampUpdated(m_CurrentTask->RelativePath, false, m_CurrentTask->TimestampNanos);
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, m_CurrentTask->RelativePath, "updated device timestamp");
        }
    }
    else
    {
        if (m_Failed) {
            m_Failed("Failed to update remote timestamp");
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, m_CurrentTask->RelativePath, "failed to update device timestamp");
        }
    }
    CompleteCurrentTask();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::CompleteCurrentTask()
{
    m_CurrentFileReader.reset();
    m_CurrentSetFileStat.reset();
    m_CurrentCompareLocalFile.close();
    m_CurrentTask.reset();
    m_Operation = Operation::Idle;
    ++m_TimestampUpdateCompletedCount;
    ++m_CurrentTaskIndex;
    UpdateStatusText();
    UpdateProgress();
    StartNextTimestampUpdate();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::UpdateProgress()
{
    std::optional<float> subProgress;
    if (m_Operation == Operation::ComparingContent) {
        subProgress = CalculateProgress(m_CurrentCompareBytesRead, m_CurrentCompareSize);
    }
    SetProgress(CalculateProgress(m_TimestampUpdateCompletedCount, m_TimestampUpdateTotalCount), subProgress, GetProgressText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::UpdateStatusText()
{
    SetStatusText(GetStatusText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::BuildTimestampUpdateQueue(const std::vector<QString>& relativePaths, bool requireIdentical, bool connected)
{
    m_TimestampUpdateQueue.clear();
    for (const QString& relativePath : relativePaths)
    {
        const SyncNode* node = (m_Model != nullptr) ? m_Model->FindNode(relativePath) : nullptr;
        if (node != nullptr)
        {
            CollectTimestampUpdateTargets(*node, requireIdentical, connected);
        }
        else if (m_LogResult)
        {
            m_LogResult(PLogSeverity::ERROR, relativePath, "skipped: file disappeared");
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::CollectTimestampUpdateTargets(const SyncNode& node, bool requireIdentical, bool connected)
{
    const bool localDirectory = node.Local.has_value() && node.Local->IsDirectory;
    const bool remoteDirectory = node.Remote.has_value() && node.Remote->IsDirectory;

    if (localDirectory || remoteDirectory)
    {
        for (const auto& childEntry : node.Children)
        {
            CollectTimestampUpdateTargets(*childEntry.second, requireIdentical, connected);
        }
    }
    else
    {
        AddTimestampUpdateTask(node, requireIdentical, connected);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateTimestampUpdate::AddTimestampUpdateTask(const SyncNode& node, bool requireIdentical, bool connected)
{
    if (node.Status == SyncStatus::Ignored)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, node.RelativePath, "skipped: ignored");
        }
    }
    else if (!node.Local.has_value() || !node.Remote.has_value())
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, node.RelativePath, "skipped: file missing on one side");
        }
    }
    else if (node.Local->IsDirectory || node.Remote->IsDirectory)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, node.RelativePath, "skipped: not a file");
        }
    }
    else if (!QFileInfo(GetLocalPath(node.RelativePath)).isFile())
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::WARNING, node.RelativePath, "skipped: local file is not accessible");
        }
    }
    else if ((m_Target == Target::Remote || requireIdentical) && !connected)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, node.RelativePath, "skipped: device is not connected");
        }
    }
    else if (requireIdentical && node.Local->Size != node.Remote->Size)
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::WARNING, node.RelativePath, "skipped: content differs");
        }
    }
    else
    {
        Task task;
        task.RelativePath = node.RelativePath;
        task.TargetSide = m_Target;
        task.RequireIdentical = requireIdentical;
        task.TimestampNanos = (m_Target == Target::Local) ? node.Remote->ModificationTimeNanos : node.Local->ModificationTimeNanos;
        task.Size = node.Local->Size;
        m_TimestampUpdateQueue.push_back(task);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateTimestampUpdate::CanApplyTimestamp(const SyncNode& node, Target target, bool requireIdentical, bool connected) const
{
    const bool localDirectory = node.Local.has_value() && node.Local->IsDirectory;
    const bool remoteDirectory = node.Remote.has_value() && node.Remote->IsDirectory;
    bool result = false;
    if (node.Status != SyncStatus::Ignored)
    {
        if (localDirectory || remoteDirectory)
        {
            for (const auto& childEntry : node.Children)
            {
                if (CanApplyTimestamp(*childEntry.second, target, requireIdentical, connected))
                {
                    result = true;
                    break;
                }
            }
        }
        else if (node.Local.has_value() && node.Remote.has_value() && QFileInfo(GetLocalPath(node.RelativePath)).isFile())
        {
            if (target == Target::Local)
            {
                result = !requireIdentical || connected;
            }
            else
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

QString SDStateTimestampUpdate::GetLocalPath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_LocalRootPath;
    }
    return m_LocalRootPath + "/" + relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateTimestampUpdate::GetRemotePath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_RemoteRootPath;
    }
    return SDCardSyncFileUtils::NormalizeRemotePath(m_RemoteRootPath + "/" + relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateTimestampUpdate::GetProgressText() const
{
    if (m_CurrentTask.has_value()) {
        return m_CurrentTask->RelativePath;
    }
    return QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateTimestampUpdate::GetSerialFileReaderResultText(SerialFileReader::Result result)
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

std::optional<float> SDStateTimestampUpdate::CalculateProgress(int64_t completed, int64_t total)
{
    if (total <= 0) {
        return std::nullopt;
    }
    return static_cast<float>(static_cast<double>(completed) / static_cast<double>(total));
}
