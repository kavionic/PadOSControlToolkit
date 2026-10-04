// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"

#include <QStringList>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::map<QString, std::unique_ptr<SDCardSyncModel::SyncNode>>& SDCardSyncModel::GetRootNodes()
{
    return m_RootNodes;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

const std::map<QString, std::unique_ptr<SDCardSyncModel::SyncNode>>& SDCardSyncModel::GetRootNodes() const
{
    return m_RootNodes;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncModel::Clear()
{
    m_RootNodes.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncModel::ClearLocalInfo(SyncNode& node)
{
    node.Local.reset();
    node.LocalSeen = false;
    node.Ignored = false;
    for (auto& childEntry : node.Children)
    {
        ClearLocalInfo(*childEntry.second);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncModel::RemoveEmptyNodes()
{
    RemoveEmptyNodes(m_RootNodes);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncModel::SyncNode& SDCardSyncModel::FindOrCreateNode(const QString& relativePath)
{
    std::map<QString, std::unique_ptr<SyncNode>>* siblings = &m_RootNodes;
    SyncNode* node = nullptr;
    QString accumulatedPath;
    const QStringList pathParts = relativePath.split('/', Qt::SkipEmptyParts);

    for (const QString& pathPart : pathParts)
    {
        accumulatedPath = accumulatedPath.isEmpty() ? pathPart : accumulatedPath + "/" + pathPart;
        std::unique_ptr<SyncNode>& nodePtr = (*siblings)[pathPart];
        if (nodePtr.get() == nullptr)
        {
            nodePtr = std::make_unique<SyncNode>();
            nodePtr->Name = pathPart;
            nodePtr->RelativePath = accumulatedPath;
        }
        node = nodePtr.get();
        siblings = &node->Children;
    }

    Q_ASSERT(node != nullptr);
    return *node;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDCardSyncModel::SyncNode* SDCardSyncModel::FindNode(const QString& relativePath)
{
    std::map<QString, std::unique_ptr<SyncNode>>* siblings = &m_RootNodes;
    SyncNode* node = nullptr;
    const QStringList pathParts = relativePath.split('/', Qt::SkipEmptyParts);

    for (const QString& pathPart : pathParts)
    {
        auto nodeIterator = siblings->find(pathPart);
        if (nodeIterator == siblings->end())
        {
            return nullptr;
        }
        node = nodeIterator->second.get();
        if (node == nullptr)
        {
            return nullptr;
        }
        siblings = &node->Children;
    }
    return node;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

const SDCardSyncModel::SyncNode* SDCardSyncModel::FindNode(const QString& relativePath) const
{
    const std::map<QString, std::unique_ptr<SyncNode>>* siblings = &m_RootNodes;
    const SyncNode* node = nullptr;
    const QStringList pathParts = relativePath.split('/', Qt::SkipEmptyParts);

    for (const QString& pathPart : pathParts)
    {
        auto nodeIterator = siblings->find(pathPart);
        if (nodeIterator == siblings->end())
        {
            return nullptr;
        }
        node = nodeIterator->second.get();
        if (node == nullptr)
        {
            return nullptr;
        }
        siblings = &node->Children;
    }
    return node;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncModel::FileInfoMatches(const FileInfo& lhs, const FileInfo& rhs)
{
    bool result = lhs.Name == rhs.Name && lhs.IsDirectory == rhs.IsDirectory;
    if (result && !lhs.IsDirectory)
    {
        result = lhs.Size == rhs.Size && lhs.ModificationTimeNanos == rhs.ModificationTimeNanos;
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncModel::IsDirectoryNode(const SyncNode& node)
{
    return (node.Local.has_value() && node.Local->IsDirectory) || (node.Remote.has_value() && node.Remote->IsDirectory) || !node.Children.empty();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDCardSyncModel::CompareNodeDisplayOrder(const SyncNode* lhs, const SyncNode* rhs)
{
    Q_ASSERT(lhs != nullptr);
    Q_ASSERT(rhs != nullptr);

    const bool lhsDirectory = IsDirectoryNode(*lhs);
    const bool rhsDirectory = IsDirectoryNode(*rhs);
    if (lhsDirectory != rhsDirectory)
    {
        return lhsDirectory;
    }

    const int caseInsensitiveResult = QString::compare(lhs->Name, rhs->Name, Qt::CaseInsensitive);
    if (caseInsensitiveResult != 0)
    {
        return caseInsensitiveResult < 0;
    }
    return lhs->Name < rhs->Name;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDCardSyncModel::RemoveEmptyNodes(std::map<QString, std::unique_ptr<SyncNode>>& nodes)
{
    for (auto nodeIterator = nodes.begin(); nodeIterator != nodes.end();)
    {
        RemoveEmptyNodes(nodeIterator->second->Children);
        if (!nodeIterator->second->Local.has_value() && !nodeIterator->second->Remote.has_value() && nodeIterator->second->Children.empty())
        {
            nodeIterator = nodes.erase(nodeIterator);
        }
        else
        {
            ++nodeIterator;
        }
    }
}
