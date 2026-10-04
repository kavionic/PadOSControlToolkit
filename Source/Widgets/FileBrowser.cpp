// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2020 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Widgets/FileBrowser.h"
#include "PadOSControl/Core/DeviceSession.h"
#include "PadOSControl/Core/HashCalculator.h"
#include "PadOSControl/Core/SerialHandler.h"

#include "SerialConsole/FilesystemMessages.h"

#include <random>

class FileTreeItem : public QTreeWidgetItem
{
public:
    FileTreeItem(const QString& name, const QDateTime& time, size_t size, bool isDirectory)
        : QTreeWidgetItem(QStringList({ name, time.toString("yy/MM/dd HH:mm:ss"), QString("%1").arg(size) }))
        , m_Name(name)
        , m_Time(time)
        , m_Size(size)
        , m_IsDirectory(isDirectory)
    {
        const QStyle* style = QApplication::style();
        setIcon(0, style->standardIcon(isDirectory ? QStyle::SP_DirClosedIcon : QStyle::SP_FileIcon));
    }

    virtual bool operator<(const QTreeWidgetItem& other) const override
    {
        const FileTreeItem& rhs = static_cast<const FileTreeItem&>(other);

        QTreeWidget* tree = treeWidget();

        const bool lhsIsDir = !m_IsDirectory;
        const bool rhsIsDir = !rhs.m_IsDirectory;
        switch (tree->sortColumn())
        {
            case 0: return std::tie(lhsIsDir, m_Name) < std::tie(rhsIsDir, rhs.m_Name);
            case 1: return std::tie(lhsIsDir, m_Time) < std::tie(rhsIsDir, rhs.m_Time);
            case 2: return std::tie(lhsIsDir, m_Size) < std::tie(rhsIsDir, rhs.m_Size);
            default:
                return std::tie(lhsIsDir, m_Name) < std::tie(rhsIsDir, rhs.m_Name);
        }
    }

    QString     m_Name;
    QDateTime   m_Time;
    size_t      m_Size;
    bool        m_IsDirectory;
};
///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

FileBrowser::FileBrowser(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    ui.m_LocalListView->setHeaderLabels({ "Name", "Data", "Size" });
    ui.m_LocalListView->setColumnWidth(0, 500);
    ui.m_LocalListView->setColumnWidth(1, 120);
    ui.m_LocalListView->setSortingEnabled(true);
    ui.m_LocalListView->sortByColumn(0, Qt::AscendingOrder);

    ui.m_RemoteListView->setHeaderLabels({ "Name", "Data", "Size" });
    ui.m_RemoteListView->setColumnWidth(0, 500);
    ui.m_RemoteListView->setColumnWidth(1, 120);
    ui.m_RemoteListView->setSortingEnabled(true);
    ui.m_RemoteListView->sortByColumn(0, Qt::AscendingOrder);

    const QStyle* style = QApplication::style();

//    ui.m_LocalFavouriteButton->setIcon(style->standardIcon(QStyle::SP_DialogApplyButton));
    ui.m_LocalUpButton->setIcon(style->standardIcon(QStyle::SP_FileDialogToParent));
    ui.m_LocalMkdirButton->setIcon(style->standardIcon(QStyle::SP_FileDialogNewFolder));
    ui.m_LocalDeleteButton->setIcon(style->standardIcon(QStyle::SP_DialogDiscardButton));
    ui.m_LocalRefreshButton->setIcon(style->standardIcon(QStyle::SP_BrowserReload));

//    ui.m_RemoteFavouriteButton->setIcon(style->standardIcon(QStyle::SP_DialogApplyButton));
    ui.m_RemoteUpButton->setIcon(style->standardIcon(QStyle::SP_FileDialogToParent));
    ui.m_RemoteMkdirButton->setIcon(style->standardIcon(QStyle::SP_FileDialogNewFolder));
    ui.m_RemoteDeleteButton->setIcon(style->standardIcon(QStyle::SP_DialogDiscardButton));
    ui.m_RemoteRefreshButton->setIcon(style->standardIcon(QStyle::SP_BrowserReload));

    ui.m_LocalPathCombo->setEditText("C:/");
    ui.m_RemotePathCombo->setEditText("/sdcard/");

    QSettings settings;

    int count = settings.beginReadArray("FileBrowser/LocalFolderList");
    for (int i = 0; i < count; ++i)
    {
        settings.setArrayIndex(i);
        ui.m_LocalPathCombo->addItem(settings.value("Path", "").toString());
//        QFileInfo fileInfo(settings.value("Path", "").toString());
//        if (!fileInfo.canonicalFilePath().isEmpty())
//        {
//            ui.m_LocalPathCombo->addItem(fileInfo.baseName(), fileInfo.canonicalFilePath());
//        }
    }
    settings.endArray();
    if (count > 0)
    {
        int index = std::max(0, std::min(count - 1, settings.value("FileBrowser/LocalFolderList/Selection").toInt()));

        ui.m_LocalPathCombo->setCurrentIndex(index);
//        SlotFileComboBoxSelectionChanged(index);
    }

    count = settings.beginReadArray("FileBrowser/RemoteFolderList");
    for (int i = 0; i < count; ++i)
    {
        settings.setArrayIndex(i);

        ui.m_RemotePathCombo->addItem(settings.value("Path", "").toString());
//        QFileInfo fileInfo(settings.value("Path", "").toString());
//        if (!fileInfo.canonicalFilePath().isEmpty())
//        {
//            ui.m_RemotePathCombo->addItem(fileInfo.baseName(), fileInfo.canonicalFilePath());
//        }
    }
    settings.endArray();
    if (count > 0)
    {
        int index = std::max(0, std::min(count - 1, settings.value("FileBrowser/RemoteFolderList/Selection").toInt()));

        ui.m_RemotePathCombo->setCurrentIndex(index);
        //        SlotFileComboBoxSelectionChanged(index);
    }

//    connect(ui.m_LocalPathCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FileBrowser::SlotLocalPathComboBoxSelectionChanged);

    UpdateLocalFavouriteButton();
    UpdateRemoteFavouriteButton();

    connect(ui.m_LocalFavouriteButton,  &QAbstractButton::clicked, this, &FileBrowser::SlotLocalFavouriteButtonClicked);
    connect(ui.m_LocalMkdirButton, &QAbstractButton::clicked, this, &FileBrowser::SlotLocalMkdirButtonClicked);
    connect(ui.m_LocalDeleteButton, &QAbstractButton::clicked, this, &FileBrowser::SlotLocalDeleteButtonClicked);
    connect(ui.m_LocalRefreshButton,    &QAbstractButton::clicked,  this, &FileBrowser::SlotLocalRefreshButtonClicked);
    connect(ui.m_LocalUpButton,         &QAbstractButton::clicked,  this, &FileBrowser::SlotLocalBackButtonClicked);

    connect(ui.m_RemoteFavouriteButton, &QAbstractButton::clicked, this, &FileBrowser::SlotRemoteFavouriteButtonClicked);
    connect(ui.m_RemoteMkdirButton, &QAbstractButton::clicked, this, &FileBrowser::SlotRemoteMkdirButtonClicked);
    connect(ui.m_RemoteDeleteButton, &QAbstractButton::clicked, this, &FileBrowser::SlotRemoteDeleteButtonClicked);
    connect(ui.m_RemoteRefreshButton,   &QAbstractButton::clicked,  this, &FileBrowser::SlotRemoteRefreshButtonClicked);
    connect(ui.m_RemoteUpButton,        &QAbstractButton::clicked,  this, &FileBrowser::SlotRemoteBackButtonClicked);

    connect(ui.m_LocalListView,         &QTreeWidget::itemDoubleClicked,    this, &FileBrowser::SlotLocalFileDoubleClicked);
    connect(ui.m_RemoteListView,        &QTreeWidget::itemDoubleClicked,    this, &FileBrowser::SlotRemoteFileDoubleClicked);

    connect(ui.m_UploadButton,      &QAbstractButton::clicked, this, &FileBrowser::SlotUploadButtonClicked);
    connect(ui.m_DownloadButton,    &QAbstractButton::clicked, this, &FileBrowser::SlotDownloadButtonClicked);

    connect(ui.m_LocalPathCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FileBrowser::SlotLocalPathComboSelectionChanged);
    connect(ui.m_RemotePathCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &FileBrowser::SlotRemotePathComboSelectionChanged);

    connect(ui.m_LocalPathCombo->lineEdit(),  &QLineEdit::editingFinished, this, &FileBrowser::SlotLocalPathComboTextChanged);
    connect(ui.m_RemotePathCombo->lineEdit(), &QLineEdit::editingFinished, this, &FileBrowser::SlotRemotePathComboTextChanged);

    SlotLocalPathComboTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SaveSettings()
{
    QSettings settings;

    settings.beginWriteArray("FileBrowser/LocalFolderList");
    for (int i = 0; i < ui.m_LocalPathCombo->count(); ++i)
    {
        settings.setArrayIndex(i);
        settings.setValue("Path", ui.m_LocalPathCombo->itemText(i));
    }
    settings.endArray();
    settings.setValue("FileBrowser/LocalFolderList/Selection", ui.m_LocalPathCombo->currentIndex());

    settings.beginWriteArray("FileBrowser/RemoteFolderList");
    for (int i = 0; i < ui.m_RemotePathCombo->count(); ++i)
    {
        settings.setArrayIndex(i);
        settings.setValue("Path", ui.m_RemotePathCombo->itemText(i));
    }
    settings.endArray();
    settings.setValue("FileBrowser/RemoteFolderList/Selection", ui.m_RemotePathCombo->currentIndex());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

FileBrowser::~FileBrowser()
{
    if (m_DeviceSession != nullptr) {
        m_DeviceSession->GetSerialHandler().UnregisterAllPacketHandlers(this);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SetDeviceSession(DeviceSession* deviceSession)
{
    Q_ASSERT(deviceSession != nullptr);
    Q_ASSERT(m_DeviceSession == nullptr);
    m_DeviceSession = deviceSession;

    m_ClientToken = std::random_device{}();

    SerialHandler& serialHandler = m_DeviceSession->GetSerialHandler();
    serialHandler.RegisterPacketHandler<SerialProtocol::OpenSessionReply>(this, &FileBrowser::HandleOpenSessionReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::FilesystemStatusReply>(this, &FileBrowser::HandleFilesystemStatusReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::GetDirectoryReply>(this, &FileBrowser::ProcessGetDirectoryReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::OpenFileReply>(this, &FileBrowser::HandleOpenFileReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::WriteFileReply>(this, &FileBrowser::HandleWriteFileReply);
    serialHandler.RegisterPacketHandler<SerialProtocol::ReadFileReply>(this, &FileBrowser::HandleReadFileReply);

    connect(m_DeviceSession, &DeviceSession::SignalMainStateChanged, this, &FileBrowser::SlotMainStateChanged);
    SlotRemotePathComboTextChanged();
    SlotMainStateChanged(m_DeviceSession->GetMainState());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SelectParentLocalFolder()
{
    QString path = m_LocalPath;
    int prevSlash = path.lastIndexOf('/');
    if (prevSlash <= 0) {
        return;
    }
    path.resize(prevSlash);
    SetLocalFolder(path);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SelectParentRemoteFolder()
{
    if (m_RemotePath.size() == 1) return;
    QString path = m_RemotePath;
    int prevSlash = path.lastIndexOf('/');
    if (prevSlash <= 0) {
        prevSlash = 1;
        //  return;
    }
    path.resize(prevSlash);
    SetRemoteFolder(path);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SetLocalFolder(const QString& path)
{
    if (path != m_LocalPath)
    {
        QDir directory(path);

        m_LocalPath = directory.canonicalPath();
        ui.m_LocalPathCombo->setCurrentText(m_LocalPath);
        UpdateLocalFavouriteButton();
        RefreshLocalFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::RefreshLocalFolder()
{
    QDir directory(m_LocalPath);

//    m_LocalPath = directory.canonicalPath();
//    ui.m_LocalPathCombo->setCurrentText(m_LocalPath);

    QFileInfoList fileList = directory.entryInfoList(QDir::NoFilter, QDir::SortFlag::Name | QDir::SortFlag::DirsFirst);

    const QStyle* style = QApplication::style();

    ui.m_LocalListView->clear();
    for (const QFileInfo& entry : fileList)
    {
        if (entry.fileName() == ".") continue;
        FileTreeItem* item = new FileTreeItem(entry.fileName(), entry.fileTime(QFileDevice::FileModificationTime), entry.size(), entry.isDir());
        ui.m_LocalListView->addTopLevelItem(item);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SetRemoteFolder(const QString& path)
{
    if (path != m_RemotePath)
    {
        m_RemotePath = path;
        ui.m_RemotePathCombo->setCurrentText(path);
        UpdateRemoteFavouriteButton();
        RefreshRemoteFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::RefreshRemoteFolder()
{
    if (m_DeviceSession->GetMainState() == MainState::ConnectedApplication && m_SessionID >= 0)
    {
        if (m_RemotePath.size() >= sizeof(SerialProtocol::GetDirectory::m_Path)) {
            return;
        }
        ui.m_RemoteListView->clear();
        QByteArray utf8Path = m_RemotePath.toUtf8();
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::GetDirectory>(m_SessionID, utf8Path.data(), utf8Path.size());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::HandleOpenSessionReply(const SerialProtocol::OpenSessionReply& packet)
{
    if (packet.m_ClientToken != m_ClientToken) {
        return;
    }
    m_SessionID = packet.m_SessionID;
    RefreshRemoteFolder();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::HandleFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    if (packet.m_Status == SerialProtocol::FilesystemError::UnknownSession)
    {
        // Session expired. Abort any ongoing transfer and re-open a session.
        m_SessionID = -1;
        m_State = State::Idle;
        m_FileOpsTimer.stop();
        m_CurrentLocalFile.close();
        m_CurrentRemoteFile = -1;
        if (m_DeviceSession->GetMainState() == MainState::ConnectedApplication)
        {
            m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::ProcessGetDirectoryReply(const SerialProtocol::GetDirectoryReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    const SerialProtocol::GetDirectoryReplyDirEnt* entries = reinterpret_cast<const SerialProtocol::GetDirectoryReplyDirEnt*>(&packet + 1);

    const QStyle* style = QApplication::style();

    for (int i = 0; i < packet.m_EntryCount; ++i)
    {
        const SerialProtocol::GetDirectoryReplyDirEnt& entry = entries[i];
        if (strcmp(entry.m_Name, ".") == 0) continue;

        const QDateTime modifiedTime = QDateTime::fromMSecsSinceEpoch(entry.m_ModificationTimeNanos / 1000000LL, QTimeZone(Qt::UTC)).toLocalTime();
        FileTreeItem* item = new FileTreeItem(entry.m_Name, modifiedTime, entry.m_Size, entry.m_IsDirectory);
        ui.m_RemoteListView->addTopLevelItem(item);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    if (m_State == State::CreatingFile || m_State == State::OpeningFile)
    {
        m_CurrentRemoteFile = packet.m_File;

        if (m_State == State::CreatingFile)
        {
            if (m_CurrentRemoteFile == -1)
            {
                m_CurrentLocalFile.close();
                m_State = State::Idle;
                return;
            }

            m_State = State::WritingFile;

            QByteArray data = m_CurrentLocalFile.read(sizeof(SerialProtocol::WriteFile::m_Buffer));
            if (data.size() > 0)
            {
                m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::WriteFile>(m_SessionID, m_CurrentRemoteFile, data.data(), 0, data.size());
                m_FileOpsTimer.start(10000);
            }
            else
            {
                m_FileOpsTimer.stop();
                m_CurrentLocalFile.close();
                m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::CloseFile>(m_SessionID, m_CurrentRemoteFile);
                m_State = State::Idle;
            }
        }
        else
        {
            if (m_CurrentLocalFile.open(QFile::WriteOnly | QFile::Truncate))
            {

                m_State = State::ReadingFile;
                m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::ReadFile>(m_SessionID, m_CurrentRemoteFile, 0, sizeof(SerialProtocol::ReadFileReply::m_Buffer));
                m_FileOpsTimer.start(10000);
            }
            else
            {
                m_FileOpsTimer.stop();
                m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::CloseFile>(m_SessionID, m_CurrentRemoteFile);
                m_State = State::Idle;
            }
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::HandleWriteFileReply(const SerialProtocol::WriteFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    if (m_State == State::WritingFile)
    {
        if (packet.m_BytesWritten != -1)
        {
            if (m_CurrentLocalFile.seek(packet.m_BytesWritten))
            {
                QByteArray data = m_CurrentLocalFile.read(sizeof(SerialProtocol::WriteFile::m_Buffer));
                if (data.size() > 0)
                {
                    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::WriteFile>(m_SessionID, m_CurrentRemoteFile, data.data(), packet.m_BytesWritten, data.size());
                    m_FileOpsTimer.start(10000);
                    return;
                }
            }
        }
    }
    m_FileOpsTimer.stop();
    m_CurrentLocalFile.close();
    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::CloseFile>(m_SessionID, m_CurrentRemoteFile);
    m_State = State::Idle;
    RefreshRemoteFolder();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::HandleReadFileReply(const SerialProtocol::ReadFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    if (m_State == State::ReadingFile)
    {
        if (packet.m_Size != -1)
        {
            if (m_CurrentLocalFile.seek(packet.m_StartPos))
            {
                qint64 bytesWritten = m_CurrentLocalFile.write(packet.m_Buffer, packet.m_Size);
                if (bytesWritten == packet.m_Size && bytesWritten == sizeof(SerialProtocol::ReadFileReply::m_Buffer))
                {
                    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::ReadFile>(m_SessionID, m_CurrentRemoteFile, packet.m_StartPos + packet.m_Size, sizeof(SerialProtocol::ReadFileReply::m_Buffer));
                    m_FileOpsTimer.start(10000);
                    return;
                }
            }
        }
    }
    m_FileOpsTimer.stop();
    m_CurrentLocalFile.close();
    m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::CloseFile>(m_SessionID, m_CurrentRemoteFile);
    m_State = State::Idle;

    if (!m_DownloadLocalPath.isEmpty())
    {
        QDesktopServices::openUrl(QUrl(m_DownloadLocalPath));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotMainStateChanged(MainState state)
{
    const bool active = state == MainState::ConnectedApplication;
    if (active)
    {
        // Open a new session; RefreshRemoteFolder is deferred until HandleOpenSessionReply.
        m_SessionID = -1;
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
    }
    else
    {
        m_SessionID = -1;
        ui.m_RemoteListView->clear();
    }
    ui.m_RemoteUpButton->setEnabled(active);
    ui.m_RemoteMkdirButton->setEnabled(active);
    ui.m_RemoteDeleteButton->setEnabled(active);
    ui.m_RemoteRefreshButton->setEnabled(active);

    ui.m_UploadButton->setEnabled(active);
    ui.m_DownloadButton->setEnabled(active);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalFavouriteButtonClicked()
{
    int index = ui.m_LocalPathCombo->findText(ui.m_LocalPathCombo->currentText());
    if (index == -1) {
        ui.m_LocalPathCombo->addItem(ui.m_LocalPathCombo->currentText());
    } else {
        QString currentText = ui.m_LocalPathCombo->currentText();
        ui.m_LocalPathCombo->removeItem(index);
        ui.m_LocalPathCombo->setEditText(currentText);
    }
    UpdateLocalFavouriteButton();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalMkdirButtonClicked()
{
    bool result;
    QString name = QInputDialog::getText(this, "Create local directory.", tr("Folder name:"), QLineEdit::Normal, "", &result);
    if (result)
    {
        QDir(m_LocalPath).mkdir(name);
        RefreshLocalFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalDeleteButtonClicked()
{
    FileTreeItem* fileItem = static_cast<FileTreeItem*>(ui.m_LocalListView->currentItem());
    if (fileItem == nullptr) {
        return;
    }
    QString path = m_LocalPath + "/" + fileItem->m_Name;
    if (QMessageBox::question(this, "Are you sure?", QString("Are you sure you want to delete '%1'").arg(path)) == QMessageBox::Yes) {
        QFile::moveToTrash(path);
        RefreshLocalFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalRefreshButtonClicked()
{
    RefreshLocalFolder();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalBackButtonClicked()
{
    SelectParentLocalFolder();
    if (m_SyncFolders) {
        SelectParentRemoteFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemoteFavouriteButtonClicked()
{
    int index = ui.m_RemotePathCombo->findText(ui.m_RemotePathCombo->currentText());
    if (index == -1) {
        ui.m_RemotePathCombo->addItem(ui.m_RemotePathCombo->currentText());
    }
    else {
        QString currentText = ui.m_RemotePathCombo->currentText();
        ui.m_RemotePathCombo->removeItem(index);
        ui.m_RemotePathCombo->setEditText(currentText);
    }
    UpdateRemoteFavouriteButton();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemoteMkdirButtonClicked()
{
    bool result;
    QString name = QInputDialog::getText(this, "Create local directory.", tr("Folder name:"), QLineEdit::Normal, "", &result);
    if (result)
    {
        QString path = m_RemotePath + "/" + name;
        QByteArray utf8Path = path.toUtf8();
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::CreateDirectory>(m_SessionID, utf8Path.data(), utf8Path.size());
        RefreshRemoteFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemoteDeleteButtonClicked()
{
    FileTreeItem* fileItem = static_cast<FileTreeItem*>(ui.m_RemoteListView->currentItem());
    if (fileItem == nullptr) {
        return;
    }
    QString path = m_RemotePath + "/" + fileItem->m_Name;
    if (QMessageBox::question(this, "Are you sure?", QString("Are you sure you want to delete:\n\n%1\n").arg(path)) == QMessageBox::Yes)
    {
        QByteArray utf8Path = path.toUtf8();
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::DeleteFile>(m_SessionID, utf8Path.data(), utf8Path.size());
        RefreshRemoteFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemoteRefreshButtonClicked()
{
    RefreshRemoteFolder();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemoteBackButtonClicked()
{
    SelectParentRemoteFolder();

    if (m_SyncFolders) {
        SelectParentLocalFolder();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalFileDoubleClicked(QTreeWidgetItem* item, int column)
{
    if (item->text(0) == "..")
    {
        SlotLocalBackButtonClicked();
    }
    else
    {
        if (m_SyncFolders) {
            SetRemoteFolder(m_RemotePath + "/" + item->text(0));
        }
        QString path = m_LocalPath + "/" + item->text(0);
        SetLocalFolder(path);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemoteFileDoubleClicked(QTreeWidgetItem* item, int column)
{
    FileTreeItem* fileItem = static_cast<FileTreeItem*>(item);

    if (fileItem->m_IsDirectory)
    {
        if (fileItem->m_Name != ".")
        {
            if (fileItem->m_Name == "..")
            {
                SlotRemoteBackButtonClicked();
            }
            else
            {
                if (m_SyncFolders) {
                    SetLocalFolder(m_LocalPath + "/" + fileItem->m_Name);
                }
                SetRemoteFolder(m_RemotePath + "/" + fileItem->m_Name);
            }
        }
    }
    else
    {
        const QString tempPath = QDir::tempPath();
        const QString& directoryName = m_DeviceSession->GetOptions().TemporaryDirectoryName;
        QDir(tempPath).mkdir(directoryName);

        m_DownloadLocalPath = QDir(tempPath).filePath(directoryName + "/" + fileItem->m_Name);
        StartDownload(m_DownloadLocalPath);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotUploadButtonClicked()
{
    if (m_State != State::Idle) {
        return;
    }
    QTreeWidgetItem* selection = ui.m_LocalListView->currentItem();
    if (selection != nullptr)
    {
        QString name = selection->text(0);

        QString localPath = m_LocalPath;
        localPath += "/";
        localPath += name;

        m_CurrentLocalFile.setFileName(localPath);
        if (m_CurrentLocalFile.open(QFile::ReadOnly))
        {
            QString remotePath = m_RemotePath;
            remotePath += "/";
            remotePath += name;

            QByteArray utf8Path = remotePath.toUtf8();
            m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::CreateFile>(m_SessionID, utf8Path.data(), utf8Path.size());
            m_FileOpsTimer.start(10000);
            m_State = State::CreatingFile;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::StartDownload(const QString& localPath)
{
    if (m_State != State::Idle) {
        return;
    }
    QTreeWidgetItem* selection = ui.m_RemoteListView->currentItem();
    if (selection != nullptr)
    {
        QString name = selection->text(0);

        m_CurrentLocalFile.setFileName(localPath);
        QString remotePath = m_RemotePath + "/" + name;
        QByteArray utf8Path = remotePath.toUtf8();

        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenFile>(
            m_SessionID,
            utf8Path.data(),
            utf8Path.size(),
            SerialProtocol::FilesystemOpenFlags::Read,
            SerialProtocol::FILESYSTEM_DEFAULT_FILE_PERMISSIONS);
        m_FileOpsTimer.start(10000);
        m_State = State::OpeningFile;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalPathComboSelectionChanged(int index)
{
    SlotLocalPathComboTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemotePathComboSelectionChanged(int index)
{
    SlotRemotePathComboTextChanged();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotLocalPathComboTextChanged()
{
    SetLocalFolder(ui.m_LocalPathCombo->currentText());
//    UpdateLocalFavouriteButton();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotRemotePathComboTextChanged()
{
    SetRemoteFolder(ui.m_RemotePathCombo->currentText());
//    UpdateRemoteFavouriteButton();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotFileOpsTimer()
{
    if (m_State != State::Idle)
    {
        m_CurrentLocalFile.close();
        m_CurrentRemoteFile = -1;
        m_State = State::Idle;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::SlotDownloadButtonClicked()
{
    if (m_State != State::Idle) {
        return;
    }
    QTreeWidgetItem* selection = ui.m_RemoteListView->currentItem();
    if (selection != nullptr)
    {
        QString name = selection->text(0);

        QString localPath = m_LocalPath + "/" + name;

        m_DownloadLocalPath.clear();
        StartDownload(localPath);
        //        m_CurrentLocalFile.setFileName(localPath);
        //        QString remotePath = m_RemotePath + "/" + name;
        //
        //        QByteArray utf8Path = remotePath.toUtf8();
        //        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenFile>(m_SessionID, utf8Path.data(), utf8Path.size());
        //        m_FileOpsTimer.start(10000);
        //        m_State = State::OpeningFile;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::UpdateLocalFavouriteButton()
{
    bool isFav = ui.m_LocalPathCombo->findText(ui.m_LocalPathCombo->currentText()) != -1;

    const QStyle* style = QApplication::style();
    ui.m_LocalFavouriteButton->setIcon(style->standardIcon((isFav) ? QStyle::SP_DialogApplyButton : QStyle::SP_DialogCancelButton));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void FileBrowser::UpdateRemoteFavouriteButton()
{
    bool isFav = ui.m_RemotePathCombo->findText(ui.m_RemotePathCombo->currentText()) != -1;

    const QStyle* style = QApplication::style();
    ui.m_RemoteFavouriteButton->setIcon(style->standardIcon((isFav) ? QStyle::SP_DialogApplyButton : QStyle::SP_DialogCancelButton));
}
