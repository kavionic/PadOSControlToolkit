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
#include <QString>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialFileReader.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncDownloadQueue.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"

enum class PLogSeverity : uint8_t;

class SDStateDownloadFiles : public SDCardSyncState
{
public:
    using FileInfo = SDCardSyncModel::FileInfo;
    using SyncNode = SDCardSyncModel::SyncNode;
    using SyncStatus = SDCardSyncModel::SyncStatus;
    using DownloadFile = SDCardSyncDownloadFile;
    using DownloadDirectory = SDCardSyncDownloadDirectory;
    using DirectoryCompletedDelegate = std::function<void(const DownloadDirectory& downloadDirectory)>;
    using FileCompletedDelegate = std::function<void(const DownloadFile& downloadFile)>;
    using LogDelegate = std::function<void(PLogSeverity severity, const QString& relativePath, const QString& resultText)>;
    using FailureDelegate = std::function<void(bool applyToModel)>;

    SDStateDownloadFiles(
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
    );

    virtual void Start() override;
    virtual void Cancel() override;
    virtual QString GetStatusText() const override;

private:
    void StartNextDownload();
    void StartCurrentDownloadFile();
    bool OpenCurrentLocalFile();
    SerialFileReader::DataResult HandleDownloadData(int64_t startPos, const char* data, int32_t size);
    void FinishCurrentDownloadFile(SerialFileReader::Result result);
    void UpdateProgress();
    void UpdateStatusText();
    void BuildDownloadQueue(const SDCardSyncModel& model, const std::set<QString>& selectedPaths, bool applyToModel);
    void CollectSelectedDownloadTargets(const SyncNode& node, const std::set<QString>& selectedPaths, bool applyToModel);
    void CollectDownloadTargets(const SyncNode& node, bool applyToModel, const QString& targetRelativePath);
    bool AddDownloadDirectory(const SyncNode& node, bool applyToModel, const QString& targetRelativePath);
    void AddDownloadFile(const SyncNode& node, bool applyToModel, const QString& targetRelativePath);
    QString GetLocalPath(const QString& relativePath) const;
    QString GetRemotePath(const QString& relativePath) const;
    QString GetProgressText() const;
    static QString GetSerialFileReaderResultText(SerialFileReader::Result result);
    static std::optional<float> CalculateProgress(int64_t completed, int64_t total);

    std::vector<DownloadDirectory> m_DownloadDirectories;
    std::vector<DownloadFile>      m_DownloadFiles;
    QString                        m_LocalRootPath;
    QString                        m_RemoteRootPath;
    DirectoryCompletedDelegate     m_DirectoryCompleted;
    FileCompletedDelegate          m_FileCompleted;
    LogDelegate                    m_LogResult;
    FailureDelegate                m_Failed;
    std::unique_ptr<SerialFileReader> m_CurrentFileReader;
    QFile   m_CurrentLocalFile;
    QString m_CurrentDownloadError;
    size_t  m_CurrentDownloadFileIndex = 0;
    int64_t m_DownloadTotalWork = 0;
    int64_t m_DownloadCompletedWork = 0;
    int64_t m_CurrentDownloadFileWork = 0;
    int64_t m_CurrentDownloadFileCompletedWork = 0;
};
