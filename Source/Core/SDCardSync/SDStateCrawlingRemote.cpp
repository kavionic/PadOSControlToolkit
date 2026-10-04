// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDStateCrawlingRemote.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"
#include "SerialConsole/FilesystemMessages.h"

#include <algorithm>

#include <QDir>

static constexpr int MAX_REMOTE_DIRECTORY_RETRIES = 2;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateCrawlingRemote::SDStateCrawlingRemote(
    Context context,
    SDCardSyncModel& model,
    const SDCardSyncLocalScanner& localScanner,
    const QString& remoteRootPath
)
    : SDCardSyncState(std::move(context))
    , m_Model(model)
    , m_LocalScanner(localScanner)
    , m_RemoteRootPath(remoteRootPath)
{
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCrawlingRemote::Start()
{
    m_CurrentDirectoryReader.reset();
    m_RemoteDirectoryQueue.clear();
    m_RemoteDirectoryQueue.push_back("");
    m_CurrentRemoteDirectory.clear();
    m_LastProgressPath.clear();
    m_CurrentRemoteDirectoryRetryCount = 0;
    m_RemoteDirectoriesRead = 0;
    m_RemoteDirectoriesEstimated = CountEstimatedRemoteDirectories();
    m_RemoteCrawlProgress = 0.0f;
    UpdateProgress();
    SendNextRemoteDirectoryRequest();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCrawlingRemote::Cancel()
{
    m_RemoteDirectoryQueue.clear();
    m_CurrentDirectoryReader.reset();
    SDCardSyncState::Cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCrawlingRemote::GetStatusText() const
{
    return "Reading device...";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCrawlingRemote::SendNextRemoteDirectoryRequest()
{
    if (m_RemoteDirectoryQueue.empty())
    {
        SetProgress(1.0f, std::nullopt, QString());
        FinishSucceeded();
        return;
    }

    m_CurrentRemoteDirectory = m_RemoteDirectoryQueue.back();
    m_RemoteDirectoryQueue.pop_back();
    m_CurrentRemoteDirectoryRetryCount = 0;
    UpdateProgress();
    StartCurrentRemoteDirectoryRead();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateCrawlingRemote::StartCurrentRemoteDirectoryRead()
{
    m_CurrentDirectoryReader = std::make_unique<SerialDirectoryReader>(GetSerialHandler(), GetSessionID());
    m_CurrentDirectoryReader->SetEntriesReadyHandler([this](const SerialProtocol::GetDirectoryReply& packet)
    {
        return HandleRemoteDirectoryEntries(packet);
    });
    m_CurrentDirectoryReader->SetFinishedHandler([this](SerialDirectoryReader::Result result)
    {
        m_CurrentDirectoryReader.reset();
        FinishRemoteDirectoryRead(result);
    });

    const SerialDirectoryReader::Result startResult = m_CurrentDirectoryReader->Start(GetRemotePath(m_CurrentRemoteDirectory));
    if (startResult != SerialDirectoryReader::Result::OK)
    {
        m_CurrentDirectoryReader.reset();
        FinishRemoteDirectoryRead(startResult);
        return false;
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDirectoryReader::Result SDStateCrawlingRemote::HandleRemoteDirectoryEntries(const SerialProtocol::GetDirectoryReply& packet)
{
    const SerialProtocol::GetDirectoryReplyDirEnt* entries = reinterpret_cast<const SerialProtocol::GetDirectoryReplyDirEnt*>(&packet + 1);
    for (int entryIndex = 0; entryIndex < packet.m_EntryCount; ++entryIndex)
    {
        const SerialProtocol::GetDirectoryReplyDirEnt& entry = entries[entryIndex];
        const QString name = QString::fromUtf8(entry.m_Name);
        if (name == "." || name == "..")
        {
            continue;
        }

        const QString relativePath = SDCardSyncFileUtils::JoinRelativePath(m_CurrentRemoteDirectory, name);
        m_LastProgressPath = relativePath;
        SyncNode& node = m_Model.FindOrCreateNode(relativePath);
        const bool ignored = m_LocalScanner.IsIgnored(relativePath, entry.m_IsDirectory);
        const bool countedByLocalEstimate = entry.m_IsDirectory && !ignored && node.Local.has_value() && node.Local->IsDirectory;

        FileInfo remoteInfo;
        remoteInfo.Name = name;
        remoteInfo.IsDirectory = entry.m_IsDirectory;
        remoteInfo.Size = entry.m_IsDirectory ? 0 : entry.m_Size;
        remoteInfo.ModificationTimeNanos = entry.m_ModificationTimeNanos;
        node.Remote = remoteInfo;
        node.Ignored = ignored;

        if (entry.m_IsDirectory && !node.Ignored)
        {
            m_RemoteDirectoryQueue.push_back(relativePath);
            if (!countedByLocalEstimate)
            {
                ++m_RemoteDirectoriesEstimated;
            }
        }
    }
    UpdateProgress();
    return SerialDirectoryReader::Result::OK;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCrawlingRemote::FinishRemoteDirectoryRead(SerialDirectoryReader::Result result)
{
    if (result == SerialDirectoryReader::Result::OK)
    {
        ++m_RemoteDirectoriesRead;
        UpdateProgress();
        SendNextRemoteDirectoryRequest();
    }
    else if (result == SerialDirectoryReader::Result::Timeout && m_CurrentRemoteDirectoryRetryCount < MAX_REMOTE_DIRECTORY_RETRIES)
    {
        ++m_CurrentRemoteDirectoryRetryCount;
        StartCurrentRemoteDirectoryRead();
    }
    else
    {
        FinishFailed(QString("Device read failed: %1").arg(SerialDirectoryReader::GetResultText(result)));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCrawlingRemote::UpdateProgress()
{
    const QString progressPath = !m_LastProgressPath.isEmpty() ? m_LastProgressPath : m_CurrentRemoteDirectory;
    const std::optional<float> progress = CalculateProgress();
    if (progress.has_value())
    {
        m_RemoteCrawlProgress = std::max(m_RemoteCrawlProgress, *progress);
    }
    SetProgress(m_RemoteCrawlProgress, std::nullopt, progressPath.isEmpty() ? "/" : progressPath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCrawlingRemote::GetRemotePath(const QString& relativePath) const
{
    if (relativePath.isEmpty())
    {
        return m_RemoteRootPath;
    }
    return SDCardSyncFileUtils::NormalizeRemotePath(m_RemoteRootPath + "/" + relativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<float> SDStateCrawlingRemote::CalculateProgress() const
{
    if (m_RemoteDirectoriesEstimated <= 0)
    {
        return std::nullopt;
    }
    return static_cast<float>(m_RemoteDirectoriesRead) / static_cast<float>(m_RemoteDirectoriesEstimated);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int64_t SDStateCrawlingRemote::CountEstimatedRemoteDirectories() const
{
    int64_t result = 1;
    for (const auto& nodeEntry : m_Model.GetRootNodes())
    {
        result += CountEstimatedRemoteDirectories(*nodeEntry.second);
    }
    return std::max<int64_t>(1, result);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int64_t SDStateCrawlingRemote::CountEstimatedRemoteDirectories(const SyncNode& node) const
{
    int64_t result = 0;
    if (!node.Ignored && node.Local.has_value() && node.Local->IsDirectory)
    {
        result = 1;
        for (const auto& childEntry : node.Children)
        {
            result += CountEstimatedRemoteDirectories(*childEntry.second);
        }
    }
    return result;
}
