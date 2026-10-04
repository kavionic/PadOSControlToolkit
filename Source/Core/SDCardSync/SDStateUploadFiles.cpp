// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDStateUploadFiles.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"
#include "SerialConsole/FilesystemMessages.h"

#include <algorithm>
#include <utility>

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateUploadFiles::SDStateUploadFiles(
    Context context,
    const SDCardSyncModel& model,
    const QString& localRootPath,
    const QString& remoteRootPath,
    DirectoryCompletedDelegate directoryCompleted,
    FileCompletedDelegate fileCompleted
)
    : SDCardSyncState(std::move(context))
    , m_OperationMode(OperationMode::Updating)
    , m_LocalRootPath(localRootPath)
    , m_RemoteRootPath(remoteRootPath)
    , m_DirectoryCompleted(std::move(directoryCompleted))
    , m_FileCompleted(std::move(fileCompleted))
{
    BuildUploadQueue(model);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateUploadFiles::SDStateUploadFiles(
    Context context,
    const SDCardSyncModel& model,
    const QString& localRootPath,
    const QString& remoteRootPath,
    const std::set<QString>& selectedPaths,
    DirectoryCompletedDelegate directoryCompleted,
    FileCompletedDelegate fileCompleted
)
    : SDCardSyncState(std::move(context))
    , m_OperationMode(OperationMode::Updating)
    , m_LocalRootPath(localRootPath)
    , m_RemoteRootPath(remoteRootPath)
    , m_DirectoryCompleted(std::move(directoryCompleted))
    , m_FileCompleted(std::move(fileCompleted))
{
    BuildUploadQueue(model, selectedPaths);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateUploadFiles::SDStateUploadFiles(
    Context context,
    const QStringList& localPaths,
    const QString& targetRelativeFolder,
    const QString& remoteRootPath,
    DirectoryCompletedDelegate directoryCompleted,
    FileCompletedDelegate fileCompleted
)
    : SDCardSyncState(std::move(context))
    , m_OperationMode(OperationMode::Uploading)
    , m_RemoteRootPath(remoteRootPath)
    , m_DirectoryCompleted(std::move(directoryCompleted))
    , m_FileCompleted(std::move(fileCompleted))
{
    BuildUploadQueue(localPaths, targetRelativeFolder);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateUploadFiles::SDStateUploadFiles(
    Context context,
    const QString& localPath,
    const QString& relativePath,
    const QString& remoteRootPath,
    DirectoryCompletedDelegate directoryCompleted,
    FileCompletedDelegate fileCompleted
)
    : SDCardSyncState(std::move(context))
    , m_OperationMode(OperationMode::Uploading)
    , m_RemoteRootPath(remoteRootPath)
    , m_DirectoryCompleted(std::move(directoryCompleted))
    , m_FileCompleted(std::move(fileCompleted))
{
    AddUploadTarget(localPath, relativePath);
    SortUploadQueue();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::Start()
{
    m_CurrentCreateDirectory.reset();
    m_CurrentFileWriter.reset();
    m_CurrentUploadLocalFile.close();
    m_CurrentUploadDirectoryIndex = 0;
    m_CurrentUploadFileIndex = 0;
    m_UploadTotalWork = static_cast<int64_t>(m_UploadDirectories.size());
    m_UploadCompletedWork = 0;
    m_CurrentUploadFileWork = 0;
    m_CurrentUploadFileCompletedWork = 0;
    m_UpdateFailedCount = 0;
    for (const UploadFile& uploadFile : m_UploadFiles) {
        m_UploadTotalWork += std::max<int64_t>(1, uploadFile.Size);
    }
    UpdateStatusText();
    UpdateProgress();
    StartNextUploadOperation();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::Cancel()
{
    m_CurrentCreateDirectory.reset();
    m_CurrentFileWriter.reset();
    m_CurrentUploadLocalFile.close();
    SDCardSyncState::Cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateUploadFiles::GetStatusText() const
{
    if (m_CurrentUploadDirectoryIndex < m_UploadDirectories.size()) {
        return QString("Creating folder %1/%2...").arg(m_CurrentUploadDirectoryIndex + 1).arg(m_UploadDirectories.size());
    }
    if (m_CurrentUploadFileIndex < m_UploadFiles.size())
    {
        const QString operationText = (m_OperationMode == OperationMode::Uploading) ? "Uploading" : "Updating";
        return QString("%1 file %2/%3...").arg(operationText).arg(m_CurrentUploadFileIndex + 1).arg(m_UploadFiles.size());
    }
    return (m_OperationMode == OperationMode::Uploading) ? "Uploading file..." : "Updating file...";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::StartNextUploadOperation()
{
    if (m_CurrentUploadDirectoryIndex < m_UploadDirectories.size())
    {
        StartNextDirectoryCreate();
    }
    else if (m_CurrentUploadFileIndex < m_UploadFiles.size())
    {
        StartNextFileUpload();
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

void SDStateUploadFiles::StartNextDirectoryCreate()
{
    m_CurrentCreateDirectory = std::make_unique<SerialCreateDirectory>(GetSerialHandler(), GetSessionID());
    m_CurrentCreateDirectory->SetFinishedHandler([this](SerialCreateDirectory::Result result)
    {
        m_CurrentCreateDirectory.reset();
        if (result == SerialCreateDirectory::Result::OK && m_CurrentUploadDirectoryIndex < m_UploadDirectories.size())
        {
            if (m_DirectoryCompleted) {
                m_DirectoryCompleted(m_UploadDirectories[m_CurrentUploadDirectoryIndex]);
            }
        }
        else
        {
            ++m_UpdateFailedCount;
        }
        ++m_UploadCompletedWork;
        ++m_CurrentUploadDirectoryIndex;
        UpdateStatusText();
        UpdateProgress();
        StartNextUploadOperation();
    });

    UpdateStatusText();
    UpdateProgress();
    const SerialCreateDirectory::Result startResult = m_CurrentCreateDirectory->Start(GetRemotePath(m_UploadDirectories[m_CurrentUploadDirectoryIndex].RelativePath));
    if (startResult != SerialCreateDirectory::Result::OK)
    {
        m_CurrentCreateDirectory.reset();
        ++m_UploadCompletedWork;
        ++m_CurrentUploadDirectoryIndex;
        ++m_UpdateFailedCount;
        UpdateStatusText();
        UpdateProgress();
        StartNextUploadOperation();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::StartNextFileUpload()
{
    const UploadFile& uploadFile = m_UploadFiles[m_CurrentUploadFileIndex];
    m_CurrentUploadLocalFile.setFileName(uploadFile.LocalPath);
    m_CurrentUploadError.clear();
    m_CurrentUploadFileWork = std::max<int64_t>(1, uploadFile.Size);
    m_CurrentUploadFileCompletedWork = 0;

    UpdateStatusText();
    if (!m_CurrentUploadLocalFile.open(QFile::ReadOnly))
    {
        m_CurrentUploadError = QString("failed to open local file: %1").arg(m_CurrentUploadLocalFile.errorString());
        FinishCurrentUploadFile(SerialFileWriter::Result::DataProviderFailed);
        return;
    }

    m_CurrentFileWriter = std::make_unique<SerialFileWriter>(GetSerialHandler(), GetSessionID());
    m_CurrentFileWriter->SetDataNeededHandler([this](int64_t offset, int32_t maxSize, QByteArray& data) {
        return HandleUploadDataNeeded(offset, maxSize, data);
    });
    m_CurrentFileWriter->SetFinishedHandler([this](SerialFileWriter::Result result)
    {
        m_CurrentFileWriter.reset();
        FinishCurrentUploadFile(result);
    });
    UpdateProgress();
    const SerialFileWriter::Result startResult = m_CurrentFileWriter->Start(GetRemotePath(uploadFile.RelativePath), uploadFile.Size, uploadFile.ModificationTimeNanos);
    if (startResult != SerialFileWriter::Result::OK)
    {
        m_CurrentFileWriter.reset();
        FinishCurrentUploadFile(startResult);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileWriter::Result SDStateUploadFiles::HandleUploadDataNeeded(int64_t offset, int32_t maxSize, QByteArray& data)
{
    if (m_CurrentUploadFileIndex >= m_UploadFiles.size())
    {
        m_CurrentUploadError = "upload queue ended unexpectedly";
        return SerialFileWriter::Result::DataProviderFailed;
    }

    if (!m_CurrentUploadLocalFile.seek(offset))
    {
        m_CurrentUploadError = QString("failed to seek local file: %1").arg(m_CurrentUploadLocalFile.errorString());
        return SerialFileWriter::Result::DataProviderFailed;
    }

    data = m_CurrentUploadLocalFile.read(maxSize);
    if (data.size() <= 0)
    {
        m_CurrentUploadError = QString("failed to read local file: %1").arg(m_CurrentUploadLocalFile.errorString());
        return SerialFileWriter::Result::DataProviderFailed;
    }
    m_CurrentUploadFileCompletedWork = std::max(m_CurrentUploadFileCompletedWork, offset + static_cast<int64_t>(data.size()));
    UpdateProgress();
    return SerialFileWriter::Result::OK;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::FinishCurrentUploadFile(SerialFileWriter::Result result)
{
    if (result == SerialFileWriter::Result::OK && m_CurrentUploadFileIndex < m_UploadFiles.size())
    {
        if (m_FileCompleted) {
            m_FileCompleted(m_UploadFiles[m_CurrentUploadFileIndex]);
        }
    }
    else if (result != SerialFileWriter::Result::OK)
    {
        ++m_UpdateFailedCount;
        qDebug() << "SDCardSync upload failed"
                 << ((m_CurrentUploadFileIndex < m_UploadFiles.size()) ? m_UploadFiles[m_CurrentUploadFileIndex].RelativePath : QString())
                 << (m_CurrentUploadError.isEmpty() ? SerialFileWriter::GetResultText(result) : m_CurrentUploadError);
    }
    m_CurrentUploadLocalFile.close();
    m_CurrentUploadError.clear();
    m_UploadCompletedWork += m_CurrentUploadFileWork;
    m_CurrentUploadFileWork = 0;
    m_CurrentUploadFileCompletedWork = 0;
    ++m_CurrentUploadFileIndex;
    UpdateStatusText();
    UpdateProgress();
    StartNextUploadOperation();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::UpdateProgress()
{
    const int64_t completedWork = m_UploadCompletedWork + std::min(m_CurrentUploadFileCompletedWork, m_CurrentUploadFileWork);
    SetProgress(CalculateProgress(completedWork, m_UploadTotalWork), CalculateProgress(m_CurrentUploadFileCompletedWork, m_CurrentUploadFileWork), GetProgressText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::UpdateStatusText()
{
    SetStatusText(GetStatusText());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::BuildUploadQueue(const SDCardSyncModel& model)
{
    m_UploadDirectories.clear();
    m_UploadFiles.clear();

    for (const auto& nodeEntry : model.GetRootNodes()) {
        CollectUploadTargets(*nodeEntry.second);
    }
    SortUploadQueue();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::BuildUploadQueue(const SDCardSyncModel& model, const std::set<QString>& selectedPaths)
{
    m_UploadDirectories.clear();
    m_UploadFiles.clear();

    if (!selectedPaths.empty())
    {
        for (const auto& nodeEntry : model.GetRootNodes()) {
            CollectSelectedUploadTargets(*nodeEntry.second, selectedPaths);
        }
    }
    SortUploadQueue();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::BuildUploadQueue(const QStringList& localPaths, const QString& targetRelativeFolder)
{
    m_UploadDirectories.clear();
    m_UploadFiles.clear();

    for (const QString& localPath : localPaths)
    {
        const QFileInfo localInfo(localPath);
        if (localInfo.exists()) {
            AddUploadTarget(localInfo.absoluteFilePath(), SDCardSyncFileUtils::JoinRelativePath(targetRelativeFolder, localInfo.fileName()));
        }
    }
    SortUploadQueue();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::CollectUploadTargets(const SyncNode& node)
{
    if (node.Status == SyncStatus::Ignored) {
        return;
    }

    AddUploadTarget(node);

    for (const auto& childEntry : node.Children) {
        CollectUploadTargets(*childEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::CollectSelectedUploadTargets(const SyncNode& node, const std::set<QString>& selectedPaths)
{
    if (selectedPaths.contains(node.RelativePath))
    {
        CollectUploadTargets(node);
    }
    else
    {
        for (const auto& childEntry : node.Children) {
            CollectSelectedUploadTargets(*childEntry.second, selectedPaths);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::AddUploadTarget(const SyncNode& node)
{
    if (node.Status != SyncStatus::Ignored && node.Local.has_value())
    {
        if (node.Local->IsDirectory)
        {
            if (!node.Remote.has_value() || !node.Remote->IsDirectory)
            {
                UploadDirectory uploadDirectory;
                uploadDirectory.RelativePath = node.RelativePath;
                uploadDirectory.UploadedInfo = *node.Local;
                uploadDirectory.AssumeIdenticalToLocal = true;
                m_UploadDirectories.push_back(uploadDirectory);
            }
        }
        else if (node.Status != SyncStatus::Identical && node.Status != SyncStatus::Comparing)
        {
            UploadFile uploadFile;
            uploadFile.RelativePath = node.RelativePath;
            uploadFile.LocalPath = GetLocalPath(node.RelativePath);
            uploadFile.Size = node.Local->Size;
            uploadFile.ModificationTimeNanos = SDCardSyncFileUtils::RoundToFATModificationTime(node.Local->ModificationTimeNanos);
            uploadFile.UploadedInfo = *node.Local;
            uploadFile.UploadedInfo.ModificationTimeNanos = uploadFile.ModificationTimeNanos;
            uploadFile.AssumeIdenticalToLocal = true;
            m_UploadFiles.push_back(uploadFile);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::AddUploadTarget(const QString& localPath, const QString& relativePath)
{
    const QFileInfo localInfo(localPath);
    if (localInfo.isDir())
    {
        UploadDirectory uploadDirectory;
        uploadDirectory.RelativePath = relativePath;
        uploadDirectory.UploadedInfo = SDCardSyncFileUtils::BuildLocalFileInfo(localInfo);
        uploadDirectory.AssumeIdenticalToLocal = false;
        m_UploadDirectories.push_back(uploadDirectory);

        QDirIterator directoryIterator(localInfo.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot);
        while (directoryIterator.hasNext())
        {
            directoryIterator.next();
            AddUploadTarget(directoryIterator.filePath(), SDCardSyncFileUtils::JoinRelativePath(relativePath, directoryIterator.fileName()));
        }
    }
    else if (localInfo.isFile())
    {
        UploadFile uploadFile;
        uploadFile.RelativePath = relativePath;
        uploadFile.LocalPath = localInfo.absoluteFilePath();
        uploadFile.Size = localInfo.size();
        uploadFile.ModificationTimeNanos = SDCardSyncFileUtils::RoundToFATModificationTime(localInfo.fileTime(QFileDevice::FileModificationTime).toUTC().toMSecsSinceEpoch() * SDCardSyncFileUtils::NanosecondsPerMillisecond);
        uploadFile.UploadedInfo = SDCardSyncFileUtils::BuildLocalFileInfo(localInfo);
        uploadFile.UploadedInfo.ModificationTimeNanos = uploadFile.ModificationTimeNanos;
        uploadFile.AssumeIdenticalToLocal = false;
        m_UploadFiles.push_back(uploadFile);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateUploadFiles::SortUploadQueue()
{
    std::sort(m_UploadDirectories.begin(), m_UploadDirectories.end(), [](const UploadDirectory& lhs, const UploadDirectory& rhs)
    {
        const qsizetype lhsDepth = lhs.RelativePath.count('/');
        const qsizetype rhsDepth = rhs.RelativePath.count('/');
        if (lhsDepth != rhsDepth) {
            return lhsDepth < rhsDepth;
        }
        return lhs.RelativePath < rhs.RelativePath;
    });

    std::sort(m_UploadFiles.begin(), m_UploadFiles.end(), [](const UploadFile& lhs, const UploadFile& rhs) {
        return lhs.RelativePath < rhs.RelativePath;
    });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateUploadFiles::GetLocalPath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_LocalRootPath;
    }
    return m_LocalRootPath + "/" + relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateUploadFiles::GetRemotePath(const QString& relativePath) const
{
    if (relativePath.isEmpty()) {
        return m_RemoteRootPath;
    }
    return SDCardSyncFileUtils::NormalizeRemotePath(m_RemoteRootPath + "/" + relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateUploadFiles::GetProgressText() const
{
    if (m_CurrentUploadDirectoryIndex < m_UploadDirectories.size()) {
        return m_UploadDirectories[m_CurrentUploadDirectoryIndex].RelativePath;
    }
    if (m_CurrentUploadFileIndex < m_UploadFiles.size()) {
        return m_UploadFiles[m_CurrentUploadFileIndex].RelativePath;
    }
    return QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<float> SDStateUploadFiles::CalculateProgress(int64_t completed, int64_t total)
{
    if (total <= 0) {
        return std::nullopt;
    }
    return static_cast<float>(static_cast<double>(completed) / static_cast<double>(total));
}
