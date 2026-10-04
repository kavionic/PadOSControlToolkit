// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <QFileSystemWatcher>
#include <QString>
#include <QStringList>

#include "PadOSControl/Core/SDCardSync/SDCardSyncIgnoreRules.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"

class SDCardSyncLocalScanner
{
public:
    SDCardSyncLocalScanner();

    struct WatchPaths
    {
        QStringList Files;
        QStringList Directories;
    };

    void SetLocalRefreshNeededHandler(std::function<void()> handler);
    void SetLocalRootPath(const QString& localRootPath, const QString& syncIgnoreFileName, const QStringList& rootMetadataFileNames);
    WatchPaths RemoveWatchedPathsInTree(const QString& absolutePath);
    void RestoreWatchedPaths(const WatchPaths& watchPaths, const QString& oldRootPath, const QString& newRootPath);
    void BeginRefresh(bool readDevice);
    void PrepareModelForLocalRefresh(SDCardSyncModel& model);
    void ScanLocalTree(SDCardSyncModel& model);
    void ClearMissingLocalInfo(SDCardSyncModel& model);
    void ApplyIgnoreRulesToRemoteNodes(SDCardSyncModel& model);

    bool IsTargetedLocalRefresh() const;
    bool IsIgnored(const QString& relativePath, bool isDirectory) const;
    const std::set<QString>& GetLocalChangedPaths() const;

private:
    using SyncNode = SDCardSyncModel::SyncNode;
    using IgnoreRule = SDCardSyncIgnoreRules::Rule;

    void HandleFilesystemChanged(const QString& path);
    std::optional<QString> GetRelativePathForLocalPath(const QString& path) const;
    void QueueLocalRefreshPath(const QString& path);
    void ScanLocalDirectory(SDCardSyncModel& model, const QString& absolutePath, const QString& relativePath, const std::vector<IgnoreRule>& parentIgnoreRules);
    void MarkLocalNodesUnseen(std::map<QString, std::unique_ptr<SyncNode>>& nodes);
    void ClearMissingLocalInfo(std::map<QString, std::unique_ptr<SyncNode>>& nodes, SDCardSyncModel& model);
    void ApplyIgnoreRulesToRemoteNodes(SyncNode& node);
    void MarkLocalPathChanged(const QString& relativePath);
    bool IsLocalRefreshPathInScope(const QString& relativePath) const;
    bool IsRootMetadataFile(const QString& relativePath) const;

    QFileSystemWatcher    m_FileWatcher;
    SDCardSyncIgnoreRules m_IgnoreRules;
    std::function<void()> m_LocalRefreshNeededHandler;
    std::set<QString>    m_PendingLocalRefreshPaths;
    std::set<QString>    m_PendingLocalChangedPaths;
    std::set<QString>    m_CurrentLocalRefreshPaths;
    std::set<QString>    m_LocalChangedPaths;
    QString              m_LocalRootPath;
    QString              m_SyncIgnoreFileName;
    QStringList          m_RootMetadataFileNames;
    bool                 m_HasLocalRefreshNeededHandler = false;
};
