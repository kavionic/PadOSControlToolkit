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

#include <QFile>
#include <QStringList>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialCreateDirectory.h"
#include "PadOSControl/Core/AsyncSerialHandlers/SerialFileWriter.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncUploadQueue.h"

class SDStateUploadFiles : public SDCardSyncState
{
public:
    using FileInfo = SDCardSyncModel::FileInfo;
    using SyncNode = SDCardSyncModel::SyncNode;
    using SyncStatus = SDCardSyncModel::SyncStatus;
    using UploadFile = SDCardSyncUploadFile;
    using UploadDirectory = SDCardSyncUploadDirectory;
    using DirectoryCompletedDelegate = std::function<void(const SDCardSyncUploadDirectory& uploadDirectory)>;
    using FileCompletedDelegate = std::function<void(const SDCardSyncUploadFile& uploadFile)>;

    SDStateUploadFiles(
        Context context,
        const SDCardSyncModel& model,
        const QString& localRootPath,
        const QString& remoteRootPath,
        DirectoryCompletedDelegate directoryCompleted,
        FileCompletedDelegate fileCompleted
    );
    SDStateUploadFiles(
        Context context,
        const SDCardSyncModel& model,
        const QString& localRootPath,
        const QString& remoteRootPath,
        const std::set<QString>& selectedPaths,
        DirectoryCompletedDelegate directoryCompleted,
        FileCompletedDelegate fileCompleted
    );
    SDStateUploadFiles(
        Context context,
        const QStringList& localPaths,
        const QString& targetRelativeFolder,
        const QString& remoteRootPath,
        DirectoryCompletedDelegate directoryCompleted,
        FileCompletedDelegate fileCompleted
    );
    SDStateUploadFiles(
        Context context,
        const QString& localPath,
        const QString& relativePath,
        const QString& remoteRootPath,
        DirectoryCompletedDelegate directoryCompleted,
        FileCompletedDelegate fileCompleted
    );

    virtual void Start() override;
    virtual void Cancel() override;
    virtual QString GetStatusText() const override;

private:
    enum class OperationMode : int
    {
        Updating,
        Uploading
    };

    void StartNextUploadOperation();
    void StartNextDirectoryCreate();
    void StartNextFileUpload();
    SerialFileWriter::Result HandleUploadDataNeeded(int64_t offset, int32_t maxSize, QByteArray& data);
    void FinishCurrentUploadFile(SerialFileWriter::Result result);
    void UpdateProgress();
    void UpdateStatusText();
    void BuildUploadQueue(const SDCardSyncModel& model);
    void BuildUploadQueue(const SDCardSyncModel& model, const std::set<QString>& selectedPaths);
    void BuildUploadQueue(const QStringList& localPaths, const QString& targetRelativeFolder);
    void CollectUploadTargets(const SyncNode& node);
    void CollectSelectedUploadTargets(const SyncNode& node, const std::set<QString>& selectedPaths);
    void AddUploadTarget(const SyncNode& node);
    void AddUploadTarget(const QString& localPath, const QString& relativePath);
    void SortUploadQueue();
    QString GetLocalPath(const QString& relativePath) const;
    QString GetRemotePath(const QString& relativePath) const;
    QString GetProgressText() const;
    static std::optional<float> CalculateProgress(int64_t completed, int64_t total);

    std::vector<UploadDirectory> m_UploadDirectories;
    std::vector<UploadFile>      m_UploadFiles;
    OperationMode                m_OperationMode = OperationMode::Updating;
    QString                      m_LocalRootPath;
    QString                      m_RemoteRootPath;
    DirectoryCompletedDelegate   m_DirectoryCompleted;
    FileCompletedDelegate        m_FileCompleted;
    std::unique_ptr<SerialCreateDirectory> m_CurrentCreateDirectory;
    std::unique_ptr<SerialFileWriter>      m_CurrentFileWriter;
    size_t  m_CurrentUploadDirectoryIndex = 0;
    size_t  m_CurrentUploadFileIndex = 0;
    QFile   m_CurrentUploadLocalFile;
    QString m_CurrentUploadError;
    int64_t m_UploadTotalWork = 0;
    int64_t m_UploadCompletedWork = 0;
    int64_t m_CurrentUploadFileWork = 0;
    int64_t m_CurrentUploadFileCompletedWork = 0;
    size_t  m_UpdateFailedCount = 0;
};
