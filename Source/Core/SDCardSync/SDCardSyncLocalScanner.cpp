// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncLocalScanner.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QObject>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncLocalScanner::SDCardSyncLocalScanner()
{
    QObject::connect(&m_FileWatcher, &QFileSystemWatcher::directoryChanged, &m_FileWatcher, [this](const QString& path)
    {
        HandleFilesystemChanged(path);
    });
    QObject::connect(&m_FileWatcher, &QFileSystemWatcher::fileChanged, &m_FileWatcher, [this](const QString& path)
    {
        HandleFilesystemChanged(path);
    });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::SetLocalRefreshNeededHandler(std::function<void()> handler)
{
    m_LocalRefreshNeededHandler = std::move(handler);
    m_HasLocalRefreshNeededHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::SetLocalRootPath(const QString& localRootPath, const QString& syncIgnoreFileName, const QStringList& rootMetadataFileNames)
{
    m_LocalRootPath = localRootPath;
    m_SyncIgnoreFileName = syncIgnoreFileName;
    m_RootMetadataFileNames = rootMetadataFileNames;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncLocalScanner::WatchPaths SDCardSyncLocalScanner::RemoveWatchedPathsInTree(const QString& absolutePath)
{
    WatchPaths result;
    QString normalizedPath = QDir::fromNativeSeparators(QFileInfo(absolutePath).absoluteFilePath());
    while (normalizedPath.endsWith('/') && normalizedPath.size() > 1)
    {
        normalizedPath.chop(1);
    }

    for (const QString& path : m_FileWatcher.files())
    {
        const QString normalizedWatchPath = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
        if (normalizedWatchPath == normalizedPath || normalizedWatchPath.startsWith(normalizedPath + "/"))
        {
            result.Files.push_back(path);
        }
    }

    for (const QString& path : m_FileWatcher.directories())
    {
        const QString normalizedWatchPath = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
        if (normalizedWatchPath == normalizedPath || normalizedWatchPath.startsWith(normalizedPath + "/"))
        {
            result.Directories.push_back(path);
        }
    }

    if (!result.Files.isEmpty())
    {
        m_FileWatcher.removePaths(result.Files);
    }
    if (!result.Directories.isEmpty())
    {
        m_FileWatcher.removePaths(result.Directories);
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::RestoreWatchedPaths(const WatchPaths& watchPaths, const QString& oldRootPath, const QString& newRootPath)
{
    QString normalizedOldRoot = QDir::fromNativeSeparators(QFileInfo(oldRootPath).absoluteFilePath());
    QString normalizedNewRoot = QDir::fromNativeSeparators(QFileInfo(newRootPath).absoluteFilePath());
    while (normalizedOldRoot.endsWith('/') && normalizedOldRoot.size() > 1)
    {
        normalizedOldRoot.chop(1);
    }
    while (normalizedNewRoot.endsWith('/') && normalizedNewRoot.size() > 1)
    {
        normalizedNewRoot.chop(1);
    }

    auto mapPath = [&normalizedOldRoot, &normalizedNewRoot](const QString& path)
    {
        const QString normalizedPath = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
        if (normalizedPath == normalizedOldRoot)
        {
            return normalizedNewRoot;
        }
        if (normalizedPath.startsWith(normalizedOldRoot + "/"))
        {
            return normalizedNewRoot + normalizedPath.sliced(normalizedOldRoot.size());
        }
        return normalizedPath;
    };

    QStringList directories;
    for (const QString& path : watchPaths.Directories)
    {
        const QString mappedPath = mapPath(path);
        if (QFileInfo(mappedPath).isDir())
        {
            directories.push_back(mappedPath);
        }
    }
    if (!directories.isEmpty())
    {
        m_FileWatcher.addPaths(directories);
    }

    QStringList files;
    for (const QString& path : watchPaths.Files)
    {
        const QString mappedPath = mapPath(path);
        if (QFileInfo(mappedPath).isFile())
        {
            files.push_back(mappedPath);
        }
    }
    if (!files.isEmpty())
    {
        m_FileWatcher.addPaths(files);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::BeginRefresh(bool readDevice)
{
    m_CurrentLocalRefreshPaths = readDevice ? std::set<QString>() : m_PendingLocalRefreshPaths;
    m_LocalChangedPaths = readDevice ? std::set<QString>() : m_PendingLocalChangedPaths;
    m_PendingLocalRefreshPaths.clear();
    m_PendingLocalChangedPaths.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::PrepareModelForLocalRefresh(SDCardSyncModel& model)
{
    if (IsTargetedLocalRefresh())
    {
        MarkLocalNodesUnseen(model.GetRootNodes());
    }
    else
    {
        for (auto& nodeEntry : model.GetRootNodes())
        {
            model.ClearLocalInfo(*nodeEntry.second);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::ScanLocalTree(SDCardSyncModel& model)
{
    m_IgnoreRules.Clear();

    const QStringList watchedFiles = m_FileWatcher.files();
    if (!watchedFiles.isEmpty())
    {
        m_FileWatcher.removePaths(watchedFiles);
    }
    const QStringList watchedDirectories = m_FileWatcher.directories();
    if (!watchedDirectories.isEmpty())
    {
        m_FileWatcher.removePaths(watchedDirectories);
    }

    if (!m_LocalRootPath.isEmpty())
    {
        const QFileInfo rootInfo(m_LocalRootPath);
        if (rootInfo.exists() && rootInfo.isDir())
        {
            const std::vector<IgnoreRule> ignoreRules = m_IgnoreRules.BuildRootRules(m_RootMetadataFileNames);
            ScanLocalDirectory(model, rootInfo.absoluteFilePath(), "", ignoreRules);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::ClearMissingLocalInfo(SDCardSyncModel& model)
{
    ClearMissingLocalInfo(model.GetRootNodes(), model);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::ApplyIgnoreRulesToRemoteNodes(SDCardSyncModel& model)
{
    for (auto& nodeEntry : model.GetRootNodes())
    {
        ApplyIgnoreRulesToRemoteNodes(*nodeEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncLocalScanner::IsTargetedLocalRefresh() const
{
    return !m_CurrentLocalRefreshPaths.empty() && !m_CurrentLocalRefreshPaths.contains(QString());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncLocalScanner::IsIgnored(const QString& relativePath, bool isDirectory) const
{
    return m_IgnoreRules.IsIgnored(relativePath, isDirectory);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

const std::set<QString>& SDCardSyncLocalScanner::GetLocalChangedPaths() const
{
    return m_LocalChangedPaths;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::HandleFilesystemChanged(const QString& path)
{
    QueueLocalRefreshPath(path);
    if (m_HasLocalRefreshNeededHandler)
    {
        m_LocalRefreshNeededHandler();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<QString> SDCardSyncLocalScanner::GetRelativePathForLocalPath(const QString& path) const
{
    std::optional<QString> result;
    if (!m_LocalRootPath.isEmpty())
    {
        QString normalizedRootPath = QDir::fromNativeSeparators(m_LocalRootPath);
        while (normalizedRootPath.endsWith('/'))
        {
            normalizedRootPath.chop(1);
        }

        QString normalizedPath = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
        while (normalizedPath.endsWith('/') && normalizedPath.size() > 1)
        {
            normalizedPath.chop(1);
        }

        if (normalizedPath == normalizedRootPath)
        {
            result = QString();
        }
        else if (normalizedPath.startsWith(normalizedRootPath + "/"))
        {
            result = normalizedPath.mid(normalizedRootPath.size() + 1);
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::QueueLocalRefreshPath(const QString& path)
{
    const std::optional<QString> relativePath = GetRelativePathForLocalPath(path);
    if (!relativePath.has_value())
    {
        m_PendingLocalRefreshPaths.clear();
        m_PendingLocalChangedPaths.clear();
        return;
    }

    const QString fileName = relativePath->section('/', -1);
    if (relativePath->isEmpty() || IsRootMetadataFile(*relativePath))
    {
        m_PendingLocalRefreshPaths.clear();
        m_PendingLocalChangedPaths.clear();
        m_PendingLocalRefreshPaths.insert(QString());
        return;
    }

    if (!m_PendingLocalRefreshPaths.contains(QString()))
    {
        m_PendingLocalRefreshPaths.insert(*relativePath);

        const QString normalizedPath = QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
        const QStringList watchedDirectories = m_FileWatcher.directories();
        bool directoryEvent = QFileInfo(path).isDir();
        for (const QString& watchedDirectory : watchedDirectories)
        {
            if (QDir::fromNativeSeparators(watchedDirectory) == normalizedPath)
            {
                directoryEvent = true;
                break;
            }
        }

        if (!directoryEvent)
        {
            m_PendingLocalChangedPaths.insert(*relativePath);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::ScanLocalDirectory(SDCardSyncModel& model, const QString& absolutePath, const QString& relativePath, const std::vector<IgnoreRule>& parentIgnoreRules)
{
    QDir directory(absolutePath);
    m_FileWatcher.addPath(directory.absolutePath());
    const std::vector<IgnoreRule> ignoreRules = m_IgnoreRules.LoadDirectoryRules(absolutePath, relativePath, parentIgnoreRules, m_SyncIgnoreFileName);

    const QFileInfoList entryList = directory.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase
    );

    for (const QFileInfo& entryInfo : entryList)
    {
        const QString childRelativePath = SDCardSyncFileUtils::JoinRelativePath(relativePath, entryInfo.fileName());
        const bool ignored = m_IgnoreRules.IsIgnored(childRelativePath, entryInfo.isDir(), ignoreRules);
        SyncNode& node = model.FindOrCreateNode(childRelativePath);
        const SDCardSyncModel::FileInfo localInfo = SDCardSyncFileUtils::BuildLocalFileInfo(entryInfo);
        const bool localInfoChanged = !node.Local.has_value() || !SDCardSyncModel::FileInfoMatches(*node.Local, localInfo);
        const bool ignoredChanged = node.Ignored != ignored;
        node.Local = localInfo;
        if (IsTargetedLocalRefresh())
        {
            node.LocalSeen = true;
        }
        node.Ignored = ignored;
        if (localInfoChanged || ignoredChanged)
        {
            MarkLocalPathChanged((entryInfo.fileName() == m_SyncIgnoreFileName) ? QString() : childRelativePath);
        }

        if (entryInfo.isDir() && !entryInfo.isSymLink())
        {
            if (!ignored)
            {
                ScanLocalDirectory(model, entryInfo.absoluteFilePath(), childRelativePath, ignoreRules);
            }
        }
        else if (entryInfo.isFile() && (!ignored || IsRootMetadataFile(childRelativePath)))
        {
            m_FileWatcher.addPath(entryInfo.absoluteFilePath());
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::MarkLocalNodesUnseen(std::map<QString, std::unique_ptr<SyncNode>>& nodes)
{
    for (auto& nodeEntry : nodes)
    {
        nodeEntry.second->LocalSeen = false;
        MarkLocalNodesUnseen(nodeEntry.second->Children);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::ClearMissingLocalInfo(std::map<QString, std::unique_ptr<SyncNode>>& nodes, SDCardSyncModel& model)
{
    for (auto& nodeEntry : nodes)
    {
        ClearMissingLocalInfo(nodeEntry.second->Children, model);
        if (nodeEntry.second->Local.has_value() && !nodeEntry.second->LocalSeen && IsLocalRefreshPathInScope(nodeEntry.second->RelativePath))
        {
            MarkLocalPathChanged(nodeEntry.second->RelativePath);
            model.ClearLocalInfo(*nodeEntry.second);
        }
        nodeEntry.second->LocalSeen = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::ApplyIgnoreRulesToRemoteNodes(SyncNode& node)
{
    if (node.Remote.has_value() && !node.Local.has_value())
    {
        node.Ignored = m_IgnoreRules.IsIgnored(node.RelativePath, node.Remote->IsDirectory);
    }
    for (auto& childEntry : node.Children)
    {
        ApplyIgnoreRulesToRemoteNodes(*childEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncLocalScanner::MarkLocalPathChanged(const QString& relativePath)
{
    if (relativePath.isEmpty())
    {
        m_LocalChangedPaths.clear();
        m_LocalChangedPaths.insert(QString());
    }
    else if (!m_LocalChangedPaths.contains(QString()))
    {
        if (!IsTargetedLocalRefresh() || IsLocalRefreshPathInScope(relativePath))
        {
            m_LocalChangedPaths.insert(relativePath);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncLocalScanner::IsLocalRefreshPathInScope(const QString& relativePath) const
{
    bool result = !IsTargetedLocalRefresh();
    if (!result)
    {
        for (const QString& refreshPath : m_CurrentLocalRefreshPaths)
        {
            if (relativePath == refreshPath || relativePath.startsWith(refreshPath + "/"))
            {
                result = true;
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncLocalScanner::IsRootMetadataFile(const QString& relativePath) const
{
    return !relativePath.contains('/') && m_RootMetadataFileNames.contains(relativePath);
}
