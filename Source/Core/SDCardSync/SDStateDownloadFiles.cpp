// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDStateDownloadFiles.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"

#ifdef ERROR
#undef ERROR
#endif
#include <Utils/LogSeverity.h>

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTimeZone>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateDownloadFiles::SDStateDownloadFiles(
    Context context,
    const SDCardSyncModel& model,
    const QString& localRootPath,
    const QString& remoteRootPath,
    const std::set<QString>& selectedPaths,
    bool applyToModel,
    DirectoryCompletedDelegate directoryCompleted,
    FileCompletedDelegate fileCompleted,
    LogDelegate logResult,
    FailureDelegate failed
)
    : SDCardSyncState(std::move(context))
    , m_LocalRootPath(localRootPath)
    , m_RemoteRootPath(remoteRootPath)
    , m_DirectoryCompleted(std::move(directoryCompleted))
    , m_FileCompleted(std::move(fileCompleted))
    , m_LogResult(std::move(logResult))
    , m_Failed(std::move(failed))
{
    BuildDownloadQueue(model, selectedPaths, applyToModel);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::Start()
{
    m_CurrentFileReader.reset();
    m_CurrentLocalFile.close();
    m_CurrentDownloadError.clear();
    m_CurrentDownloadFileIndex = 0;
    m_DownloadTotalWork = static_cast<int64_t>(m_DownloadDirectories.size());
    m_DownloadCompletedWork = 0;
    m_CurrentDownloadFileWork = 0;
    m_CurrentDownloadFileCompletedWork = 0;

    for (const DownloadFile& downloadFile : m_DownloadFiles) {
        m_DownloadTotalWork += std::max<int64_t>(1, downloadFile.RemoteInfo.Size);
    }
    UpdateStatusText();
    UpdateProgress();

    for (const DownloadDirectory& downloadDirectory : m_DownloadDirectories)
    {
        if (m_DirectoryCompleted) {
            m_DirectoryCompleted(downloadDirectory);
        }
        ++m_DownloadCompletedWork;
        UpdateProgress();
    }
    StartNextDownload();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::Cancel()
{
    m_CurrentFileReader.reset();
    m_CurrentLocalFile.close();
    SDCardSyncState::Cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDownloadFiles::GetStatusText() const
{
    if (m_CurrentDownloadFileIndex < m_DownloadFiles.size())
    {
        const QString operationText = m_DownloadFiles[m_CurrentDownloadFileIndex].ApplyToModel ? "Updating local" : "Downloading";
        return QString("%1 file %2/%3...").arg(operationText).arg(m_CurrentDownloadFileIndex + 1).arg(m_DownloadFiles.size());
    }
    return "Downloading file...";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::StartNextDownload()
{
    m_CurrentLocalFile.close();
    m_CurrentDownloadError.clear();

    if (m_CurrentDownloadFileIndex >= m_DownloadFiles.size())
    {
        SetProgress(1.0f, std::nullopt, QString());
        FinishSucceeded();
        return;
    }
    StartCurrentDownloadFile();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::StartCurrentDownloadFile()
{
    const DownloadFile& downloadFile = m_DownloadFiles[m_CurrentDownloadFileIndex];
    m_CurrentDownloadFileWork = std::max<int64_t>(1, downloadFile.RemoteInfo.Size);
    m_CurrentDownloadFileCompletedWork = 0;

    const QByteArray utf8Path = GetRemotePath(downloadFile.RelativePath).toUtf8();
    if (utf8Path.size() >= SDCardSyncFileUtils::MaxFilesystemPathBytes)
    {
        FinishCurrentDownloadFile(SerialFileReader::Result::PathTooLong);
        return;
    }

    m_CurrentFileReader = std::make_unique<SerialFileReader>(GetSerialHandler(), GetSessionID());
    m_CurrentFileReader->SetOpenedHandler([this]()
    {
        return OpenCurrentLocalFile() ? SerialFileReader::Result::OK : SerialFileReader::Result::OpenedHandlerFailed;
    });
    m_CurrentFileReader->SetDataReadyHandler([this](int64_t startPos, const char* data, int32_t size)
    {
        return HandleDownloadData(startPos, data, size);
    });
    m_CurrentFileReader->SetFinishedHandler([this](SerialFileReader::Result result)
    {
        m_CurrentFileReader.reset();
        FinishCurrentDownloadFile(result);
    });

    UpdateStatusText();
    UpdateProgress();
    const SerialFileReader::Result startResult = m_CurrentFileReader->Start(GetRemotePath(downloadFile.RelativePath), downloadFile.RemoteInfo.Size);
    if (startResult != SerialFileReader::Result::OK)
    {
        m_CurrentFileReader.reset();
        FinishCurrentDownloadFile(startResult);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDownloadFiles::OpenCurrentLocalFile()
{
    bool result = false;
    const DownloadFile& downloadFile = m_DownloadFiles[m_CurrentDownloadFileIndex];
    const QString localPath = downloadFile.LocalPath.isEmpty() ? GetLocalPath(downloadFile.RelativePath) : downloadFile.LocalPath;
    const QString localDirectoryPath = QFileInfo(localPath).path();
    if (!QDir().mkpath(localDirectoryPath))
    {
        m_CurrentDownloadError = QString("failed to create local folder: %1").arg(localDirectoryPath);
    }
    else
    {
        m_CurrentLocalFile.setFileName(localPath);
        if (m_CurrentLocalFile.open(QFile::WriteOnly | QFile::Truncate))
        {
            result = true;
        }
        else
        {
            m_CurrentDownloadError = QString("failed to open local file: %1").arg(m_CurrentLocalFile.errorString());
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::DataResult SDStateDownloadFiles::HandleDownloadData(int64_t startPos, const char* data, int32_t size)
{
    SerialFileReader::DataResult result = SerialFileReader::DataResult::Error;
    m_CurrentDownloadFileCompletedWork = std::max(m_CurrentDownloadFileCompletedWork, startPos + size);
    UpdateProgress();

    if (m_CurrentLocalFile.seek(startPos))
    {
        const qint64 bytesWritten = m_CurrentLocalFile.write(data, size);
        if (bytesWritten == size)
        {
            result = SerialFileReader::DataResult::Continue;
        }
        else
        {
            m_CurrentDownloadError = QString("failed to write local file: %1").arg(m_CurrentLocalFile.errorString());
        }
    }
    else
    {
        m_CurrentDownloadError = QString("failed to seek local file: %1").arg(m_CurrentLocalFile.errorString());
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::FinishCurrentDownloadFile(SerialFileReader::Result result)
{
    const DownloadFile& downloadFile = m_DownloadFiles[m_CurrentDownloadFileIndex];
    bool finalSuccess = result == SerialFileReader::Result::OK;
    if (finalSuccess)
    {
        const QDateTime timestamp = QDateTime::fromMSecsSinceEpoch(downloadFile.RemoteInfo.ModificationTimeNanos / SDCardSyncFileUtils::NanosecondsPerMillisecond, QTimeZone(Qt::UTC));
        if (!m_CurrentLocalFile.flush())
        {
            finalSuccess = false;
            m_CurrentDownloadError = QString("failed to flush local file: %1").arg(m_CurrentLocalFile.errorString());
        }
        else if (!m_CurrentLocalFile.setFileTime(timestamp, QFileDevice::FileModificationTime))
        {
            finalSuccess = false;
            m_CurrentDownloadError = QString("failed to update local timestamp: %1").arg(m_CurrentLocalFile.errorString());
        }
    }
    m_CurrentLocalFile.close();

    if (finalSuccess)
    {
        if (m_FileCompleted) {
            m_FileCompleted(downloadFile);
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::INFO_LOW_VOL, downloadFile.RelativePath, downloadFile.ApplyToModel ? "updated local content" : "downloaded");
        }
    }
    else
    {
        if (m_CurrentDownloadError.isEmpty()) {
            m_CurrentDownloadError = GetSerialFileReaderResultText(result);
        }
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, downloadFile.RelativePath, m_CurrentDownloadError);
        }
        if (m_Failed) {
            m_Failed(downloadFile.ApplyToModel);
        }
    }

    m_DownloadCompletedWork += m_CurrentDownloadFileWork;
    m_CurrentDownloadFileWork = 0;
    m_CurrentDownloadFileCompletedWork = 0;
    ++m_CurrentDownloadFileIndex;
    UpdateStatusText();
    UpdateProgress();
    StartNextDownload();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::UpdateProgress()
{
    const int64_t completedWork = m_DownloadCompletedWork + std::min(m_CurrentDownloadFileCompletedWork, m_CurrentDownloadFileWork);
    SetProgress(CalculateProgress(completedWork, m_DownloadTotalWork), CalculateProgress(m_CurrentDownloadFileCompletedWork, m_CurrentDownloadFileWork), GetProgressText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::UpdateStatusText()
{
    SetStatusText(GetStatusText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::BuildDownloadQueue(const SDCardSyncModel& model, const std::set<QString>& selectedPaths, bool applyToModel)
{
    m_DownloadDirectories.clear();
    m_DownloadFiles.clear();

    if (!selectedPaths.empty())
    {
        for (const auto& nodeEntry : model.GetRootNodes()) {
            CollectSelectedDownloadTargets(*nodeEntry.second, selectedPaths, applyToModel);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::CollectSelectedDownloadTargets(const SyncNode& node, const std::set<QString>& selectedPaths, bool applyToModel)
{
    if (selectedPaths.contains(node.RelativePath))
    {
        CollectDownloadTargets(node, applyToModel, applyToModel ? node.RelativePath : node.Name);
    }
    else
    {
        for (const auto& childEntry : node.Children) {
            CollectSelectedDownloadTargets(*childEntry.second, selectedPaths, applyToModel);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::CollectDownloadTargets(const SyncNode& node, bool applyToModel, const QString& targetRelativePath)
{
    if (applyToModel && node.Status == SyncStatus::Ignored) {
        return;
    }

    if (!node.Remote.has_value()) {
        return;
    }

    if (node.Remote->IsDirectory)
    {
        if (AddDownloadDirectory(node, applyToModel, targetRelativePath))
        {
            for (const auto& childEntry : node.Children)
            {
                CollectDownloadTargets(*childEntry.second, applyToModel, SDCardSyncFileUtils::JoinRelativePath(targetRelativePath, childEntry.second->Name));
            }
        }
    }
    else
    {
        AddDownloadFile(node, applyToModel, targetRelativePath);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateDownloadFiles::AddDownloadDirectory(const SyncNode& node, bool applyToModel, const QString& targetRelativePath)
{
    bool result = false;
    const QString localPath = GetLocalPath(targetRelativePath);
    const QFileInfo localInfo(localPath);
    if (localInfo.exists() && !localInfo.isDir())
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::WARNING, node.RelativePath, applyToModel ? "skipped: local path is not a folder" : "skipped: target path is not a folder");
        }
    }
    else if (localInfo.exists() || QDir().mkpath(localPath))
    {
        DownloadDirectory downloadDirectory;
        downloadDirectory.RelativePath = node.RelativePath;
        downloadDirectory.LocalPath = localPath;
        downloadDirectory.RemoteInfo = *node.Remote;
        downloadDirectory.ApplyToModel = applyToModel;
        m_DownloadDirectories.push_back(downloadDirectory);
        result = true;
    }
    else
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::ERROR, node.RelativePath, applyToModel ? "failed to create local folder" : "failed to create target folder");
        }
        if (m_Failed) {
            m_Failed(applyToModel);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateDownloadFiles::AddDownloadFile(const SyncNode& node, bool applyToModel, const QString& targetRelativePath)
{
    if (applyToModel && (node.Local.has_value() && node.Local->IsDirectory))
    {
        if (m_LogResult) {
            m_LogResult(PLogSeverity::WARNING, node.RelativePath, "skipped: local path is a folder");
        }
        return;
    }

    if (!applyToModel || ((!node.Local.has_value() || !node.Local->IsDirectory) && node.Status != SyncStatus::Identical && node.Status != SyncStatus::Comparing))
    {
        DownloadFile downloadFile;
        downloadFile.RelativePath = node.RelativePath;
        downloadFile.LocalPath = applyToModel ? QString() : GetLocalPath(targetRelativePath);
        downloadFile.RemoteInfo = *node.Remote;
        downloadFile.ApplyToModel = applyToModel;
        m_DownloadFiles.push_back(downloadFile);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDownloadFiles::GetLocalPath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_LocalRootPath;
    }
    return m_LocalRootPath + "/" + relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDownloadFiles::GetRemotePath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_RemoteRootPath;
    }
    return SDCardSyncFileUtils::NormalizeRemotePath(m_RemoteRootPath + "/" + relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDownloadFiles::GetProgressText() const
{
    if (m_CurrentDownloadFileIndex < m_DownloadFiles.size()) {
        return m_DownloadFiles[m_CurrentDownloadFileIndex].RelativePath;
    }
    return QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateDownloadFiles::GetSerialFileReaderResultText(SerialFileReader::Result result)
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

std::optional<float> SDStateDownloadFiles::CalculateProgress(int64_t completed, int64_t total)
{
    if (total <= 0) {
        return std::nullopt;
    }
    return static_cast<float>(static_cast<double>(completed) / static_cast<double>(total));
}
