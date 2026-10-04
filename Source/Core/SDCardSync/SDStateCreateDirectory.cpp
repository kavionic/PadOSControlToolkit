// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/SDCardSync/SDStateCreateDirectory.h"

#include "PadOSControl/Core/SDCardSync/SDCardSyncFileUtils.h"

#include <utility>

#include <QButtonGroup>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SDStateCreateDirectory::SDStateCreateDirectory(
    Context context,
    QWidget* parent,
    const SyncNode& clickedNode,
    bool clickedLocalSide,
    const SDCardSyncModel& model,
    const QString& localRootPath,
    const QString& remoteRootPath,
    bool connected,
    ResultCompletedDelegate resultCompleted
)
    : SDCardSyncState(std::move(context))
    , m_Parent(parent)
    , m_ClickedNode(&clickedNode)
    , m_Model(&model)
    , m_LocalRootPath(localRootPath)
    , m_RemoteRootPath(remoteRootPath)
    , m_ClickedLocalSide(clickedLocalSide)
    , m_Connected(connected)
    , m_ResultCompleted(std::move(resultCompleted))
{
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCreateDirectory::Start()
{
    QString parentRelativePath;
    QString name;
    Target target = Target::Local;
    if (!ShowCreateDirectoryDialog(parentRelativePath, name, target))
    {
        FinishCanceled();
        return;
    }

    QString errorText;
    if (!ValidateCreateDirectoryTarget(parentRelativePath, name, target, m_RelativePath, errorText))
    {
        QMessageBox::warning(m_Parent, "Create folder", errorText);
        FinishCanceled();
        return;
    }

    m_CreateLocal = target == Target::Local || target == Target::Both;
    m_CreateRemote = target == Target::Remote || target == Target::Both;
    if (m_CreateRemote && !m_Connected)
    {
        QMessageBox::warning(m_Parent, "Create folder", "The device is not connected.");
        FinishFailed("Device is not connected");
        return;
    }

    m_CurrentCreateDirectory.reset();
    m_Result = Result();
    m_Result.RelativePath = m_RelativePath;
    m_Result.LocalRequested = m_CreateLocal;
    m_Result.RemoteRequested = m_CreateRemote;
    m_ResultCompletedNotified = false;
    SetProgress(std::nullopt, std::nullopt, m_RelativePath);

    if (m_CreateLocal)
    {
        QString errorText;
        if (!CreateLocalDirectory(errorText))
        {
            m_Result.ErrorText = errorText;
            NotifyResultCompleted();
            FinishFailed(errorText);
            return;
        }
    }

    if (m_CreateRemote)
    {
        StartRemoteCreateDirectory();
    }
    else
    {
        NotifyResultCompleted();
        FinishSucceeded();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCreateDirectory::Cancel()
{
    m_CurrentCreateDirectory.reset();
    SDCardSyncState::Cancel();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCreateDirectory::GetStatusText() const
{
    return "Creating folder...";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateCreateDirectory::ShowCreateDirectoryDialog(QString& parentRelativePath, QString& name, Target& target)
{
    QDialog dialog(m_Parent);
    dialog.setWindowTitle("Create folder");

    parentRelativePath = GetCreateDirectoryParentRelativePath();

    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QString("Create in: %1").arg(FormatProgressItemText(parentRelativePath)), &dialog));

    QHBoxLayout* nameLayout = new QHBoxLayout();
    QLabel* nameLabel = new QLabel("Folder name:", &dialog);
    QLineEdit* nameEdit = new QLineEdit(&dialog);
    nameLayout->addWidget(nameLabel);
    nameLayout->addWidget(nameEdit);
    layout->addLayout(nameLayout);

    QLabel* sideLabel = new QLabel("Create on:", &dialog);
    layout->addWidget(sideLabel);

    QButtonGroup* buttonGroup = new QButtonGroup(&dialog);
    QRadioButton* localButton = new QRadioButton("Local", &dialog);
    QRadioButton* deviceButton = new QRadioButton("Device", &dialog);
    QRadioButton* bothButton = new QRadioButton("Both", &dialog);
    buttonGroup->addButton(localButton);
    buttonGroup->addButton(deviceButton);
    buttonGroup->addButton(bothButton);
    layout->addWidget(localButton);
    layout->addWidget(deviceButton);
    layout->addWidget(bothButton);

    if (m_ClickedLocalSide)
    {
        localButton->setChecked(true);
    }
    else
    {
        deviceButton->setChecked(true);
    }

    QDialogButtonBox* buttonBox = new QDialogButtonBox(&dialog);
    QPushButton* okButton = buttonBox->addButton("Ok", QDialogButtonBox::AcceptRole);
    buttonBox->addButton("Cancel", QDialogButtonBox::RejectRole);
    okButton->setEnabled(false);
    layout->addWidget(buttonBox);

    QObject::connect(nameEdit, &QLineEdit::textChanged, &dialog, [okButton](const QString& text)
    {
        okButton->setEnabled(!text.trimmed().isEmpty());
    });
    QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    nameEdit->setFocus();
    if (dialog.exec() != QDialog::Accepted)
    {
        return false;
    }

    target = Target::Local;
    if (deviceButton->isChecked())
    {
        target = Target::Remote;
    }
    else if (bothButton->isChecked())
    {
        target = Target::Both;
    }
    name = nameEdit->text();
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateCreateDirectory::ValidateCreateDirectoryTarget(const QString& parentRelativePath, const QString& name, Target target, QString& newRelativePath, QString& errorText) const
{
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty())
    {
        errorText = "Folder name cannot be empty.";
        return false;
    }
    if (trimmedName != name)
    {
        errorText = "Folder name cannot begin or end with whitespace.";
        return false;
    }
    if (name == "." || name == ".." || name.contains('/') || name.contains('\\'))
    {
        errorText = "Folder name must be a single folder name.";
        return false;
    }

    newRelativePath = SDCardSyncFileUtils::JoinRelativePath(parentRelativePath, name);

    const bool createLocal = target == Target::Local || target == Target::Both;
    const bool createRemote = target == Target::Remote || target == Target::Both;
    if (createLocal && m_LocalRootPath.isEmpty())
    {
        errorText = "Select a local root before creating a local folder.";
        return false;
    }
    if (createRemote && GetRemotePath(newRelativePath).toUtf8().size() >= SDCardSyncFileUtils::MaxFilesystemPathBytes)
    {
        errorText = "Device path is too long.";
        return false;
    }

    const SyncNode* targetNode = m_Model->FindNode(newRelativePath);
    if (targetNode != nullptr)
    {
        if (createLocal && targetNode->Local.has_value())
        {
            errorText = "A local file or folder with that name already exists.";
            return false;
        }
        if (createRemote && targetNode->Remote.has_value())
        {
            errorText = "A device file or folder with that name already exists.";
            return false;
        }
    }
    if (createLocal && QFileInfo(GetLocalPath(newRelativePath)).exists())
    {
        errorText = "A local file or folder with that name already exists.";
        return false;
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SDStateCreateDirectory::CreateLocalDirectory(QString& errorText)
{
    const QString localPath = GetLocalPath();
    const QFileInfo localPathInfo(localPath);
    QDir parentDirectory(localPathInfo.absolutePath());
    if (!parentDirectory.mkdir(localPathInfo.fileName()))
    {
        errorText = QString("Failed to create local folder '%1'.").arg(localPath);
        return false;
    }

    m_Result.LocalInfo = SDCardSyncFileUtils::BuildLocalFileInfo(QFileInfo(localPath));
    m_Result.LocalCreated = true;
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCreateDirectory::StartRemoteCreateDirectory()
{
    m_CurrentCreateDirectory = std::make_unique<SerialCreateDirectory>(GetSerialHandler(), GetSessionID());
    m_CurrentCreateDirectory->SetFinishedHandler([this](SerialCreateDirectory::Result result)
    {
        m_CurrentCreateDirectory.reset();
        FinishRemoteCreateDirectory(result == SerialCreateDirectory::Result::OK);
    });

    const SerialCreateDirectory::Result startResult = m_CurrentCreateDirectory->Start(GetRemotePath());
    if (startResult != SerialCreateDirectory::Result::OK)
    {
        m_CurrentCreateDirectory.reset();
        FinishRemoteCreateDirectory(false);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCreateDirectory::FinishRemoteCreateDirectory(bool success)
{
    if (success)
    {
        FileInfo createdRemoteInfo;
        createdRemoteInfo.Name = QFileInfo(m_RelativePath).fileName();
        createdRemoteInfo.IsDirectory = true;
        createdRemoteInfo.ModificationTimeNanos = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch() * SDCardSyncFileUtils::NanosecondsPerMillisecond;
        m_Result.RemoteInfo = createdRemoteInfo;
        m_Result.RemoteCreated = true;
        NotifyResultCompleted();
        FinishSucceeded();
    }
    else
    {
        m_Result.ErrorText = m_Result.LocalCreated ? "Created local folder, but failed to create device folder" : "Failed to create device folder";
        NotifyResultCompleted();
        FinishFailed(m_Result.ErrorText);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SDStateCreateDirectory::NotifyResultCompleted()
{
    if (!m_ResultCompletedNotified)
    {
        m_ResultCompletedNotified = true;
        if (m_ResultCompleted)
        {
            m_ResultCompleted(m_Result);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCreateDirectory::GetCreateDirectoryParentRelativePath() const
{
    bool clickedSideIsDirectory = false;
    if (m_ClickedLocalSide && m_ClickedNode->Local.has_value())
    {
        clickedSideIsDirectory = m_ClickedNode->Local->IsDirectory;
    }
    else if (!m_ClickedLocalSide && m_ClickedNode->Remote.has_value())
    {
        clickedSideIsDirectory = m_ClickedNode->Remote->IsDirectory;
    }
    else
    {
        clickedSideIsDirectory = SDCardSyncModel::IsDirectoryNode(*m_ClickedNode);
    }

    if (clickedSideIsDirectory)
    {
        return m_ClickedNode->RelativePath;
    }

    const qsizetype separatorIndex = m_ClickedNode->RelativePath.lastIndexOf('/');
    return (separatorIndex >= 0) ? m_ClickedNode->RelativePath.left(separatorIndex) : QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCreateDirectory::GetLocalPath() const
{
    return GetLocalPath(m_RelativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCreateDirectory::GetRemotePath() const
{
    return GetRemotePath(m_RelativePath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCreateDirectory::GetLocalPath(const QString& relativePath) const
{
    if (relativePath.isEmpty())
    {
        return m_LocalRootPath;
    }
    return m_LocalRootPath + "/" + relativePath;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SDStateCreateDirectory::GetRemotePath(const QString& relativePath) const
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

QString SDStateCreateDirectory::FormatProgressItemText(const QString& relativePath)
{
    if (relativePath.isEmpty())
    {
        return "/";
    }
    return relativePath;
}
