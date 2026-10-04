// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include <QString>

class SDCardSyncModel
{
public:
    enum class SyncStatus : int
    {
        Unknown,
        Identical,
        Different,
        LocalOnly,
        RemoteOnly,
        TypeConflict,
        Comparing,
        Ignored
    };

    struct FileInfo
    {
        QString Name;
        bool    IsDirectory = false;
        int64_t Size = 0;
        int64_t ModificationTimeNanos = 0;
    };

    struct SyncNode
    {
        QString RelativePath;
        QString Name;
        std::optional<FileInfo> Local;
        std::optional<FileInfo> Remote;
        SyncStatus Status = SyncStatus::Unknown;
        bool Ignored = false;
        bool LocalSeen = false;
        std::map<QString, std::unique_ptr<SyncNode>> Children;
    };

    std::map<QString, std::unique_ptr<SyncNode>>& GetRootNodes();
    const std::map<QString, std::unique_ptr<SyncNode>>& GetRootNodes() const;

    void Clear();
    void ClearLocalInfo(SyncNode& node);
    void RemoveEmptyNodes();

    SyncNode& FindOrCreateNode(const QString& relativePath);
    SyncNode* FindNode(const QString& relativePath);
    const SyncNode* FindNode(const QString& relativePath) const;

    static bool FileInfoMatches(const FileInfo& lhs, const FileInfo& rhs);
    static bool IsDirectoryNode(const SyncNode& node);
    static bool CompareNodeDisplayOrder(const SyncNode* lhs, const SyncNode* rhs);

private:
    void RemoveEmptyNodes(std::map<QString, std::unique_ptr<SyncNode>>& nodes);

    std::map<QString, std::unique_ptr<SyncNode>> m_RootNodes;
};
