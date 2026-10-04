// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialDirectoryReader.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncLocalScanner.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"

class SDStateCrawlingRemote : public SDCardSyncState
{
public:
    SDStateCrawlingRemote(
        Context context,
        SDCardSyncModel& model,
        const SDCardSyncLocalScanner& localScanner,
        const QString& remoteRootPath
    );

    virtual void Start() override;
    virtual void Cancel() override;
    virtual QString GetStatusText() const override;

private:
    using FileInfo = SDCardSyncModel::FileInfo;
    using SyncNode = SDCardSyncModel::SyncNode;

    void SendNextRemoteDirectoryRequest();
    bool StartCurrentRemoteDirectoryRead();
    SerialDirectoryReader::Result HandleRemoteDirectoryEntries(const SerialProtocol::GetDirectoryReply& packet);
    void FinishRemoteDirectoryRead(SerialDirectoryReader::Result result);
    void UpdateProgress();
    QString GetRemotePath(const QString& relativePath) const;
    std::optional<float> CalculateProgress() const;
    int64_t CountEstimatedRemoteDirectories() const;
    int64_t CountEstimatedRemoteDirectories(const SyncNode& node) const;

    SDCardSyncModel& m_Model;
    const SDCardSyncLocalScanner& m_LocalScanner;
    QString m_RemoteRootPath;
    std::unique_ptr<SerialDirectoryReader> m_CurrentDirectoryReader;
    std::vector<QString> m_RemoteDirectoryQueue;
    QString m_CurrentRemoteDirectory;
    QString m_LastProgressPath;
    int m_CurrentRemoteDirectoryRetryCount = 0;
    int64_t m_RemoteDirectoriesRead = 0;
    int64_t m_RemoteDirectoriesEstimated = 0;
    float m_RemoteCrawlProgress = 0.0f;
};
