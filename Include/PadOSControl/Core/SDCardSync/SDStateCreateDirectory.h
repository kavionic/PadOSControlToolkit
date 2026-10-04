// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "PadOSControl/Core/AsyncSerialHandlers/SerialCreateDirectory.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncModel.h"
#include "PadOSControl/Core/SDCardSync/SDCardSyncState.h"

class QTreeWidget;
class QWidget;

class SDStateCreateDirectory : public SDCardSyncState
{
public:
    using FileInfo = SDCardSyncModel::FileInfo;
    using SyncNode = SDCardSyncModel::SyncNode;

    struct Result
    {
        QString RelativePath;
        std::optional<FileInfo> LocalInfo;
        std::optional<FileInfo> RemoteInfo;
        QString ErrorText;
        bool LocalRequested = false;
        bool LocalCreated = false;
        bool RemoteRequested = false;
        bool RemoteCreated = false;
    };

    using ResultCompletedDelegate = std::function<void(const Result& result)>;

    SDStateCreateDirectory(
        Context context,
        QWidget* parent,
        const SyncNode& clickedNode,
        bool clickedLocalSide,
        const SDCardSyncModel& model,
        const QString& localRootPath,
        const QString& remoteRootPath,
        bool connected,
        ResultCompletedDelegate resultCompleted
    );

    virtual void Start() override;
    virtual void Cancel() override;
    virtual QString GetStatusText() const override;

private:
    enum class Target : int
    {
        Local,
        Remote,
        Both
    };

    bool ShowCreateDirectoryDialog(QString& parentRelativePath, QString& name, Target& target);
    bool ValidateCreateDirectoryTarget(const QString& parentRelativePath, const QString& name, Target target, QString& newRelativePath, QString& errorText) const;
    bool CreateLocalDirectory(QString& errorText);
    void StartRemoteCreateDirectory();
    void FinishRemoteCreateDirectory(bool success);
    void NotifyResultCompleted();
    QString GetCreateDirectoryParentRelativePath() const;
    QString GetLocalPath() const;
    QString GetRemotePath() const;
    QString GetLocalPath(const QString& relativePath) const;
    QString GetRemotePath(const QString& relativePath) const;
    static QString FormatProgressItemText(const QString& relativePath);

    QWidget* m_Parent = nullptr;
    const SyncNode* m_ClickedNode = nullptr;
    const SDCardSyncModel* m_Model = nullptr;
    QString m_RelativePath;
    QString m_LocalRootPath;
    QString m_RemoteRootPath;
    bool    m_ClickedLocalSide = false;
    bool    m_Connected = false;
    bool    m_CreateLocal = false;
    bool    m_CreateRemote = false;
    ResultCompletedDelegate m_ResultCompleted;
    std::unique_ptr<SerialCreateDirectory> m_CurrentCreateDirectory;
    Result  m_Result;
    bool    m_ResultCompletedNotified = false;
};
