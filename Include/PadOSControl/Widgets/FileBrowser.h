// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2020 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <QFile>
#include <QTimer>

#include <QWidget>

#include "ui_FileBrowser.h"

class DeviceSession;
enum class MainState : int;

namespace SerialProtocol
{
struct OpenSessionReply;
struct FilesystemStatusReply;
struct GetDirectoryReply;
struct OpenFileReply;
struct WriteFileReply;
struct ReadFileReply;
}

class FileBrowser : public QWidget
{
    Q_OBJECT

    enum class State
    {
        Idle,
        ReadingFolder,
        OpeningFile,
        CreatingFile,
        ReadingFile,
        WritingFile
    };
public:
    Ui::FileBrowser ui;

    FileBrowser(QWidget* parent);
    ~FileBrowser();

    void SaveSettings();

    void SetDeviceSession(DeviceSession* deviceSession);

    void HandleOpenSessionReply(const SerialProtocol::OpenSessionReply& packet);
    void HandleFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet);
    void ProcessGetDirectoryReply(const SerialProtocol::GetDirectoryReply& packet);
    void HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet);
    void HandleWriteFileReply(const SerialProtocol::WriteFileReply& packet);
    void HandleReadFileReply(const SerialProtocol::ReadFileReply& packet);

private slots:
    void SlotMainStateChanged(MainState state);
    void SlotLocalFavouriteButtonClicked();
    void SlotLocalMkdirButtonClicked();
    void SlotLocalDeleteButtonClicked();
    void SlotLocalRefreshButtonClicked();
    void SlotLocalBackButtonClicked();

    void SlotRemoteFavouriteButtonClicked();
    void SlotRemoteMkdirButtonClicked();
    void SlotRemoteDeleteButtonClicked();
    void SlotRemoteRefreshButtonClicked();
    void SlotRemoteBackButtonClicked();

    void SlotLocalFileDoubleClicked(QTreeWidgetItem* item, int column);
    void SlotRemoteFileDoubleClicked(QTreeWidgetItem* item, int column);

    void SlotUploadButtonClicked();
    void SlotDownloadButtonClicked();

    void SlotLocalPathComboSelectionChanged(int index);
    void SlotRemotePathComboSelectionChanged(int index);

    void SlotLocalPathComboTextChanged();
    void SlotRemotePathComboTextChanged();

    void SlotFileOpsTimer();

private:
    void SelectParentLocalFolder();
    void SelectParentRemoteFolder();

    void SetLocalFolder(const QString& path);
    void RefreshLocalFolder();
    void SetRemoteFolder(const QString& path);
    void RefreshRemoteFolder();

    void StartDownload(const QString& localPath);

    void UpdateLocalFavouriteButton();
    void UpdateRemoteFavouriteButton();

    DeviceSession* m_DeviceSession = nullptr;

    int32_t  m_SessionID    = -1;
    uint32_t m_ClientToken  = 0;

    State m_State = State::Idle;
    QString m_DownloadLocalPath;

    QString m_LocalPath;
    QString m_RemotePath;

    QTimer  m_FileOpsTimer;
    QFile   m_CurrentLocalFile;
    int32_t m_CurrentRemoteFile = -1;

    bool    m_SyncFolders = true;
};
