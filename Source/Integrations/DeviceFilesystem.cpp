// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#ifdef HAVE_WINFSP

#include "PadOSControl/Integrations/DeviceFilesystem.h"
#include "PadOSControl/Core/SerialHandler.h"

#include <sddl.h>
#include <condition_variable>
#include <iterator>
#include <limits>

#include "PadOSControl/Core/DeviceSession.h"

//#define LOG_FS_OPERATIONS 1


// Directory cache TTL
static constexpr auto CACHE_TTL = std::chrono::milliseconds(10*1000);

static constexpr int64_t WINDOWS_TICKS_PER_SECOND       = 10000000LL;
static constexpr int64_t NANOS_PER_SECOND               = 1000000000LL;
static constexpr int64_t UNIX_TO_WINDOWS_EPOCH_SECONDS  = 11644473600LL;
static constexpr UINT64  NANOS_PER_WINDOWS_TICK         = 100ULL;
static constexpr UINT64  UNIX_TO_WINDOWS_EPOCH_TICKS    = UNIX_TO_WINDOWS_EPOCH_SECONDS * WINDOWS_TICKS_PER_SECOND;

namespace
{

///////////////////////////////////////////////////////////////////////////////

NTSTATUS FilesystemErrorToNtStatus(SerialProtocol::FilesystemError error)
{
    switch (error)
    {
        case SerialProtocol::FilesystemError::OK:
            return STATUS_SUCCESS;

        case SerialProtocol::FilesystemError::NotFound:
            return STATUS_OBJECT_NAME_NOT_FOUND;

        case SerialProtocol::FilesystemError::PermissionDenied:
            return STATUS_ACCESS_DENIED;

        case SerialProtocol::FilesystemError::NoSpace:
            return STATUS_DISK_FULL;

        case SerialProtocol::FilesystemError::AlreadyExists:
            return STATUS_OBJECT_NAME_COLLISION;

        case SerialProtocol::FilesystemError::InvalidArgument:
            return STATUS_INVALID_PARAMETER;

        case SerialProtocol::FilesystemError::UnknownSession:
        case SerialProtocol::FilesystemError::IOError:
        default:
            return STATUS_IO_DEVICE_ERROR;
    }
}

///////////////////////////////////////////////////////////////////////////////

int32_t WinFspOpenFlagsFromGrantedAccess(UINT32 grantedAccess)
{
    int32_t openFlags = 0;

    if ((grantedAccess & FILE_READ_DATA) != 0) {
        openFlags |= SerialProtocol::FilesystemOpenFlags::Read;
    }
    if ((grantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0) {
        openFlags |= SerialProtocol::FilesystemOpenFlags::Write;
    }
    if (openFlags == 0) {
        openFlags = SerialProtocol::FilesystemOpenFlags::Read;
    }
    if ((grantedAccess & FILE_APPEND_DATA) != 0 && (grantedAccess & FILE_WRITE_DATA) == 0) {
        openFlags |= SerialProtocol::FilesystemOpenFlags::Append;
    }
    return openFlags;
}

} // namespace

// Minimal "Everyone: full control" security descriptor for all exposed files.
// Built once at startup via ConvertStringSecurityDescriptorToSecurityDescriptor.
BYTE DeviceFilesystem::s_SecurityDescriptor[256] = {};
SIZE_T DeviceFilesystem::s_SecurityDescriptorSize = 0;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::BuildSecurityDescriptor()
{
    PSECURITY_DESCRIPTOR psd = nullptr;
    ULONG size = 0;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;WD)", SDDL_REVISION_1, &psd, &size))
    {
        if (size <= sizeof(DeviceFilesystem::s_SecurityDescriptor))
        {
            memcpy(DeviceFilesystem::s_SecurityDescriptor, psd, size);
            DeviceFilesystem::s_SecurityDescriptorSize = size;
        }
        LocalFree(psd);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

DeviceFilesystem::DeviceFilesystem(SerialHandler& serialHandler, DeviceSession* deviceSession)
    : QObject(deviceSession)
    , m_SerialHandler(serialHandler)
    , m_DeviceSession(deviceSession)
{
    if (s_SecurityDescriptorSize == 0) {
        BuildSecurityDescriptor();
    }

    m_ClientToken = std::random_device{}();

    m_SerialHandler.RegisterPacketHandler<SerialProtocol::OpenSessionReply>(this, &DeviceFilesystem::SlotOpenSessionReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::GetDirectoryReply>(this, &DeviceFilesystem::SlotGetDirectoryReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::OpenFileReply>(this, &DeviceFilesystem::SlotOpenFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::ReadFileReply>(this, &DeviceFilesystem::SlotReadFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::WriteFileReply>(this, &DeviceFilesystem::SlotWriteFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::SetFileStatReply>(this, &DeviceFilesystem::SlotSetFileStatReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::CloseFileReply>(this, &DeviceFilesystem::SlotCloseFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::CreateDirectoryReply>(this, &DeviceFilesystem::SlotCreateDirectoryReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::DeleteFileReply>(this, &DeviceFilesystem::SlotDeleteFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::FilesystemStatusReply>(this, &DeviceFilesystem::SlotFilesystemStatusReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::GetVolumeInfoReply>(this, &DeviceFilesystem::SlotGetVolumeInfoReply);

    connect(m_DeviceSession, &DeviceSession::SignalMainStateChanged, this, &DeviceFilesystem::SlotMainStateChanged);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

DeviceFilesystem::~DeviceFilesystem()
{
    Unmount();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool DeviceFilesystem::Mount(wchar_t driveLetter)
{
    if (m_FileSystem != nullptr) {
        return true;
    }
    static FSP_FILE_SYSTEM_INTERFACE iface = {};

    iface.GetVolumeInfo      = WinFspGetVolumeInfo;
    iface.GetSecurityByName  = WinFspGetSecurityByName;
    iface.Create             = WinFspCreate;
    iface.Open               = WinFspOpen;
    iface.Overwrite          = WinFspOverwrite;
    iface.Cleanup            = WinFspCleanup;
    iface.Close              = WinFspClose;
    iface.Read               = WinFspRead;
    iface.Write              = WinFspWrite;
    iface.Flush              = WinFspFlush;
    iface.GetFileInfo        = WinFspGetFileInfo;
    iface.SetBasicInfo       = WinFspSetBasicInfo;
    iface.SetFileSize        = WinFspSetFileSize;
    iface.CanDelete          = WinFspCanDelete;
    iface.Rename             = WinFspRename;
    iface.GetSecurity        = WinFspGetSecurity;
    iface.ReadDirectory      = WinFspReadDirectory;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    FSP_FSCTL_VOLUME_PARAMS params = {};

    params.SectorSize                   = 512;
    params.SectorsPerAllocationUnit     = 1;
    params.MaxComponentLength           = 255;
    params.VolumeCreationTime           = (static_cast<UINT64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    params.VolumeSerialNumber           = 0x52424433; // 'RBD3'
    params.FileInfoTimeout              = 10*1000;
    params.CaseSensitiveSearch          = 1;
    params.CasePreservedNames           = 1;
    params.UnicodeOnDisk                = 1;
    params.PersistentAcls               = 0;
    params.PostCleanupWhenModifiedOnly  = 1;
    params.SupportsPosixUnlinkRename    = 1;
    params.UmFileContextIsUserContext2  = 1;

    const DeviceSessionOptions& options = m_DeviceSession->GetOptions();
    const std::wstring filesystemPrefix = options.FilesystemPrefix.toStdWString();
    const std::wstring filesystemName = options.FilesystemName.toStdWString();
    if (filesystemPrefix.size() >= std::size(params.Prefix) || filesystemName.size() >= std::size(params.FileSystemName)) {
        return false;
    }
    wcscpy_s(params.Prefix, filesystemPrefix.c_str());
    wcscpy_s(params.FileSystemName, filesystemName.c_str());

    NTSTATUS status = FspFileSystemCreate(
        const_cast<PWSTR>(L"" FSP_FSCTL_NET_DEVICE_NAME),
        &params, &iface, &m_FileSystem
    );

    if (!NT_SUCCESS(status)) {
        return false;
    }
    m_FileSystem->UserContext = this;

    // Start dispatcher first so WinFsp worker threads are running before
    // the mount triggers any early volume queries from Windows.
    status = FspFileSystemStartDispatcher(m_FileSystem, 0);
    if (!NT_SUCCESS(status))
    {
        FspFileSystemDelete(m_FileSystem);
        m_FileSystem = nullptr;
        return false;
    }

    wchar_t mountPoint[3] = { driveLetter, L':', L'\0' };
    status = FspFileSystemSetMountPoint(m_FileSystem, mountPoint);
    if (!NT_SUCCESS(status))
    {
        FspFileSystemStopDispatcher(m_FileSystem);
        FspFileSystemDelete(m_FileSystem);
        m_FileSystem = nullptr;
        return false;
    }
    m_MountRootPath = QString::fromWCharArray(&driveLetter, 1) + ":/";
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::Unmount()
{
    if (m_FileSystem != nullptr)
    {
        m_Unmounting = true;
        {
            std::lock_guard<std::mutex> pendLk(m_PendingMutex);
            if (m_Pending != nullptr)
            {
                m_Pending->m_Status = SerialProtocol::FilesystemError::IOError;
                m_Pending->m_Ready  = true;
                m_Pending->m_Condition.notify_all();
            }
        }

        FspFileSystemStopDispatcher(m_FileSystem);
        FspFileSystemDelete(m_FileSystem);
        m_FileSystem = nullptr;
        m_MountRootPath.clear();
        m_Unmounting = false;

        emit SignalMountStatusChanged("Drive: disconnected");
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString DeviceFilesystem::GetMountRootPath() const
{
    return m_MountRootPath;
}

////////// Session management //////////


///////////////////////////////////////////////////////////////////////////////
/// Called while m_SerialOpMutex is already held.
/// Sends OpenSession to the device if no session is established and waits for the reply.
///
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::EnsureSession_Locked()
{
    if (m_SessionID >= 0) {
        return;
    }
    if (m_Unmounting) {
        return;
    }
    auto pending = std::make_shared<PendingResult>();

    {
        std::lock_guard<std::mutex> pendLk(m_PendingMutex);
        m_Pending = pending;
    }

    const uint32_t clientToken = m_ClientToken;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, clientToken]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::OpenSession>(clientToken);
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(10), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    // m_SessionID has been updated by SlotOpenSessionReply if the reply arrived in time.
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotMainStateChanged(MainState state)
{
    m_SessionID = -1;

    Unmount();

    if (state == MainState::ConnectedApplication) {
        m_DeviceSession->GetSerialHandler().SendMessage<SerialProtocol::OpenSession>(m_ClientToken);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotOpenSessionReply(const SerialProtocol::OpenSessionReply& packet)
{
    if (packet.m_ClientToken != m_ClientToken) {
        return;
    }
    m_SessionID = packet.m_SessionID;

    if (packet.m_SessionID >= 0)
    {
        if (Mount(L'R')) {
            emit SignalMountStatusChanged("Drive R: mounted");
        } else {
            emit SignalMountStatusChanged("Drive: mount failed");
        }
    }
    if (m_Pending != nullptr)
    {
        std::lock_guard<std::mutex> lock(m_PendingMutex);

        m_Pending->m_Status = (packet.m_SessionID >= 0) ? SerialProtocol::FilesystemError::OK
                                                        : SerialProtocol::FilesystemError::IOError;

        m_Pending->m_Ready = true;
        m_Pending->m_Condition.notify_all();
    }
}

////////// Reply slots — called on Qt main thread when device sends a reply packet. //////////
////////// Each slot delivers the result to the pending operation and signals it.   //////////


///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotGetDirectoryReply(const SerialProtocol::GetDirectoryReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    const SerialProtocol::GetDirectoryReplyDirEnt* entries =
        reinterpret_cast<const SerialProtocol::GetDirectoryReplyDirEnt*>(&packet + 1);

    for (int32_t i = 0; i < packet.m_EntryCount; ++i)
    {
        const SerialProtocol::GetDirectoryReplyDirEnt& src = entries[i];
        DirEntry& dst = m_Pending->m_DirEntries.emplace_back();
        dst.m_Size                  = src.m_Size;
        dst.m_CreationTimeNanos     = src.m_CreationTimeNanos;
        dst.m_AccessTimeNanos       = src.m_AccessTimeNanos;
        dst.m_ModificationTimeNanos = src.m_ModificationTimeNanos;
        dst.m_Attributes            = src.m_Attributes;
        dst.m_IsDirectory           = src.m_IsDirectory;
        dst.m_Name                  = src.m_Name;
    }
    if (packet.m_EntryCount == 0)
    {
        // Final (terminating) packet received.
        m_Pending->m_DirDone = true;
        m_Pending->m_Ready   = true;
        m_Pending->m_Condition.notify_all();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotOpenFileReply(const SerialProtocol::OpenFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Handle = packet.m_File;
    m_Pending->m_Status = packet.m_Status;
    m_Pending->m_Ready = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotReadFileReply(const SerialProtocol::ReadFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
#ifdef LOG_FS_OPERATIONS
    m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SlotReadFileReply(): file: {}, offset: {}, size: {}\n", packet.m_File, packet.m_StartPos, packet.m_Size);
#endif
    if (packet.m_Size > 0)
    {
        m_Pending->m_Data.assign(packet.m_Buffer, packet.m_Buffer + packet.m_Size);
        m_Pending->m_BytesResult = packet.m_Size;
        m_Pending->m_Status      = SerialProtocol::FilesystemError::OK;
    }
    else
    {
        m_Pending->m_BytesResult = (packet.m_Size < 0) ? -1 : 0;
        m_Pending->m_Status      = (packet.m_Size < 0) ? SerialProtocol::FilesystemError::IOError
                                                       : SerialProtocol::FilesystemError::OK;
    }
    m_Pending->m_Ready = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotWriteFileReply(const SerialProtocol::WriteFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_BytesResult = packet.m_BytesWritten;
    m_Pending->m_Status      = packet.m_Status;
    m_Pending->m_Ready = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotSetFileStatReply(const SerialProtocol::SetFileStatReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Status = packet.m_Status;
    m_Pending->m_Ready = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotCloseFileReply(const SerialProtocol::CloseFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
#ifdef LOG_FS_OPERATIONS
    m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SlotCloseFileReply(): ses: {}, status: {}\n", m_SessionID, int(packet.m_Status));
#endif
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Status = packet.m_Status;
    m_Pending->m_Ready  = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotCreateDirectoryReply(const SerialProtocol::CreateDirectoryReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Status = packet.m_Status;
    m_Pending->m_Ready  = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotDeleteFileReply(const SerialProtocol::DeleteFileReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Status = packet.m_Status;
    m_Pending->m_Ready  = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    if (packet.m_Status == SerialProtocol::FilesystemError::UnknownSession) {
        // Session expired (device rebooted). Mark it invalid so EnsureSession_Locked will
        // re-open a new session on the next Send* call.
        m_SessionID = -1;

        Unmount();
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Status = packet.m_Status;
    m_Pending->m_Ready  = true;
    m_Pending->m_Condition.notify_all();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::SlotGetVolumeInfoReply(const SerialProtocol::GetVolumeInfoReply& packet)
{
    if (packet.m_SessionID != m_SessionID) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_PendingMutex);
    if (m_Pending == nullptr) {
        return;
    }
    m_Pending->m_Status     = packet.m_Status;
    m_Pending->m_TotalBytes = packet.m_TotalBytes;
    m_Pending->m_FreeBytes  = packet.m_FreeBytes;
    m_Pending->m_Ready      = true;
    m_Pending->m_Condition.notify_all();
}


/// Serial operation helpers — each serializes via m_SerialOpMutex (one op at a
/// time across all WinFsp worker threads), then posts the send to the Qt main
/// thread via QueuedConnection and blocks on m_PendingMutex / condition_variable
/// until the reply slot signals completion.



///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int32_t DeviceFilesystem::SendOpenFile(const std::string& path, int32_t openFlags, int32_t permissions, SerialProtocol::FilesystemError& outStatus)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting)
    {
        outStatus = SerialProtocol::FilesystemError::IOError;
        return -1;
    }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, path, openFlags, permissions]()
        {
#ifdef LOG_FS_OPERATIONS
            m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SendOpenFile(): ses: {}, path: '{}', flags: {}, permissions: {}\n", sessionID, path, openFlags, permissions);
#endif
            m_SerialHandler.SendMessage<SerialProtocol::OpenFile>(
                sessionID, path.c_str(), int(path.size()), openFlags, permissions);
        }, Qt::QueuedConnection
    );

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    if (!pending->m_Ready || (pending->m_Handle < 0 && pending->m_Status == SerialProtocol::FilesystemError::OK))
    {
        pending->m_Status = SerialProtocol::FilesystemError::IOError;
    }
    outStatus = pending->m_Status;
    return pending->m_Handle;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int32_t DeviceFilesystem::SendCreateFile(const std::string& path)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return -1; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, path]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::CreateFile>(
            sessionID, path.c_str(), int(path.size()));
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    return pending->m_Handle;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendCloseFile(int32_t handle)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return SerialProtocol::FilesystemError::IOError; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, handle]()
        {
#ifdef LOG_FS_OPERATIONS
            m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SendCloseFile(): ses: {}, file: {}\n", sessionID, handle);
#endif
            m_SerialHandler.SendMessage<SerialProtocol::CloseFile>(
                sessionID, handle);
        }, Qt::QueuedConnection
    );

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    if (!pending->m_Ready) {
        pending->m_Status = SerialProtocol::FilesystemError::IOError;
    }
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendReadFile(int32_t handle, int64_t offset, int32_t size, std::vector<char>& outData)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return SerialProtocol::FilesystemError::IOError; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, handle, offset, size]()
    {
#ifdef LOG_FS_OPERATIONS
            m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SendReadFile(): ses: {}, file: {}, offset: {}, size: {}\n", sessionID, handle, offset, size);
#endif
            m_SerialHandler.SendMessage<SerialProtocol::ReadFile>(
                sessionID, handle, offset, size);
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    outData = std::move(pending->m_Data);
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendWriteFile(int32_t handle, int64_t offset, const void* data, int32_t size, int64_t& outBytesWritten)
{
    // data pointer stays valid for the duration of this call: WinFsp guarantees the
    // buffer lives until the callback returns, and we block here until the reply arrives.
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return SerialProtocol::FilesystemError::IOError; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, handle, offset, data, size]()
    {
#ifdef LOG_FS_OPERATIONS
            m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SendWriteFile(): ses: {}, file: {}, offset: {}, size: {}\n", sessionID, handle, offset, size);
#endif
            m_SerialHandler.SendMessage<SerialProtocol::WriteFile>(
                sessionID, handle, data, offset, size);
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    outBytesWritten = pending->m_BytesResult;
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendSetFileStat(int32_t handle, uint32_t mask, uint32_t mode, int64_t size, int64_t accessTimeNanos, int64_t modificationTimeNanos, int64_t creationTimeNanos)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) {
        return SerialProtocol::FilesystemError::IOError;
    }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    {
        std::lock_guard<std::mutex> pendLk(m_PendingMutex);
        m_Pending = pending;
    }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, handle, mask, mode, size, accessTimeNanos, modificationTimeNanos, creationTimeNanos]()
    {
#ifdef LOG_FS_OPERATIONS
            m_DeviceSession->AddLogMessage(PLogSeverity::INFO_HIGH_VOL, "SendSetFileStat(): ses: {}, file: {}, mask: {}, size: {}\n", sessionID, handle, mask, size);
#endif
            m_SerialHandler.SendMessage<SerialProtocol::SetFileStat>(
                sessionID,
                handle,
                mask,
                mode,
                size,
                accessTimeNanos,
                modificationTimeNanos,
                creationTimeNanos);
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendCreateDirectory(const std::string& path)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return SerialProtocol::FilesystemError::IOError; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, path]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::CreateDirectory>(
            sessionID, path.c_str(), int(path.size()));
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendDeleteFile(const std::string& path)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return SerialProtocol::FilesystemError::IOError; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, path]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::DeleteFile>(
            sessionID, path.c_str(), int(path.size()));
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialProtocol::FilesystemError DeviceFilesystem::SendRenameFile(const std::string& oldPath, const std::string& newPath)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return SerialProtocol::FilesystemError::IOError; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, oldPath, newPath]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::RenameFile>(
            sessionID,
            oldPath.c_str(), int(oldPath.size()),
            newPath.c_str(), int(newPath.size()));
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    return pending->m_Status;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool DeviceFilesystem::SendGetDirectory(const std::string& path, std::vector<DirEntry>& outEntries)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return false; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    pending->m_DirDone = false;

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID, path]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::GetDirectory>(
            sessionID, path.c_str(), int(path.size()));
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    outEntries = std::move(pending->m_DirEntries);
    return pending->m_DirDone;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool DeviceFilesystem::SendGetVolumeInfo(int64_t& outTotal, int64_t& outFree)
{
    std::lock_guard<std::mutex> opLock(m_SerialOpMutex);
    if (m_Unmounting) { return false; }
    EnsureSession_Locked();

    auto pending = std::make_shared<PendingResult>();

    { std::lock_guard<std::mutex> pendLk(m_PendingMutex); m_Pending = pending; }

    const int32_t sessionID = m_SessionID;
    QMetaObject::invokeMethod(&m_SerialHandler, [this, sessionID]()
    {
        m_SerialHandler.SendMessage<SerialProtocol::GetVolumeInfo>(sessionID);
    }, Qt::QueuedConnection);

    {
        std::unique_lock<std::mutex> pendLk(m_PendingMutex);
        pending->m_Condition.wait_for(pendLk, std::chrono::seconds(100), [&] { return pending->m_Ready; });
        m_Pending.reset();
    }
    outTotal = pending->m_TotalBytes;
    outFree  = pending->m_FreeBytes;
    return pending->m_Status == SerialProtocol::FilesystemError::OK;
}


////////// Directory cache //////////


///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

const DeviceFilesystem::CachedDir* DeviceFilesystem::GetCachedDir(const std::string& dirPath)
{
    std::lock_guard<std::mutex> lock(m_CacheMutex);

    auto it = m_DirCache.find(dirPath);

    if (it == m_DirCache.end()) {
        return nullptr;
    }
    const auto age = std::chrono::steady_clock::now() - it->second.m_FetchTime;
    if (age > CACHE_TTL)
    {
        m_DirCache.erase(it);
        return nullptr;
    }
    return &it->second;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::optional<DeviceFilesystem::DirEntry> DeviceFilesystem::FindCachedEntry(const std::string& path)
{
    std::string dir  = ParentPath(path);
    std::string name = path.substr(dir.size() + (dir.back() == '/' ? 0 : 1));

    // Search the cache under lock and return a copy to avoid a dangling pointer if
    // another thread invalidates the cache entry after we release the lock.
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);

        auto it = m_DirCache.find(dir);

        if (it != m_DirCache.end())
        {
            const auto age = std::chrono::steady_clock::now() - it->second.m_FetchTime;
            if (age <= CACHE_TTL)
            {
                for (const DirEntry& entry : it->second.m_Entries)
                {
                    if (entry.m_Name == name) {
                        return entry; // copy
                    }
                }
                return std::nullopt;
            }
            m_DirCache.erase(it);
        }
    }

    // Cache miss — fetch the parent directory from the device.
    std::vector<DirEntry> entries;
    SendGetDirectory(dir, entries);

    std::optional<DirEntry> result;
    {
        std::lock_guard<std::mutex> lock(m_CacheMutex);
        CachedDir& cd  = m_DirCache[dir];
        cd.m_Entries   = std::move(entries);
        cd.m_FetchTime = std::chrono::steady_clock::now();
        for (const DirEntry& entry : cd.m_Entries)
        {
            if (entry.m_Name == name)
            {
                result = entry; // copy
                break;
            }
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::InvalidateDirCache(const std::string& dirPath)
{
    std::lock_guard<std::mutex> lock(m_CacheMutex);
    m_DirCache.erase(dirPath);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::string DeviceFilesystem::ParentPath(const std::string& path) const
{
    size_t pos = path.rfind('/');
    if (pos == std::string::npos || pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool DeviceFilesystem::RefreshContextFromDirectory(FileContext& context)
{
    std::optional<DirEntry> entry = FindCachedEntry(context.m_Path);
    if (entry.has_value())
    {
        ApplyDirEntryToContext(context, *entry);
        return true;
    }
    return false;
}


////////// Helpers //////////


///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

UINT32 DeviceFilesystem::PosixModeToFileAttributes(uint32_t mode, bool isDirectory)
{
    UINT32 attrs = 0;
    if (isDirectory) {
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        attrs |= FILE_ATTRIBUTE_NORMAL;
    }
    if ((mode & 0222) == 0) { // no write bits
        attrs |= FILE_ATTRIBUTE_READONLY;
    }
    return attrs;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

uint32_t DeviceFilesystem::FileAttributesToPosixMode(UINT32 fileAttributes, bool isDirectory)
{
    uint32_t mode = isDirectory ? 0777 : 0666;
    if ((fileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        mode &= ~0222;
    }
    return mode;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

UINT64 DeviceFilesystem::UnixNanosToWinTime(int64_t unixNanos)
{
    int64_t unixSeconds = unixNanos / NANOS_PER_SECOND;
    int64_t nanosRemainder = unixNanos % NANOS_PER_SECOND;
    if (nanosRemainder < 0)
    {
        nanosRemainder += NANOS_PER_SECOND;
        --unixSeconds;
    }
    if (unixSeconds < -UNIX_TO_WINDOWS_EPOCH_SECONDS) {
        return 0;
    }
    return UINT64(unixSeconds + UNIX_TO_WINDOWS_EPOCH_SECONDS) * UINT64(WINDOWS_TICKS_PER_SECOND) + UINT64(nanosRemainder / 100);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int64_t DeviceFilesystem::WinTimeToUnixNanos(UINT64 winTime)
{
    if (winTime < UNIX_TO_WINDOWS_EPOCH_TICKS) {
        return 0;
    }
    return int64_t((winTime - UNIX_TO_WINDOWS_EPOCH_TICKS) * NANOS_PER_WINDOWS_TICK);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int64_t DeviceFilesystem::CurrentUnixTimeNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::ApplyDirEntryToContext(FileContext& context, const DirEntry& entry)
{
    context.m_IsDirectory = entry.m_IsDirectory;
    context.m_FileSize = entry.m_IsDirectory ? 0 : UINT64(entry.m_Size);
    context.m_FileAttributes = PosixModeToFileAttributes(entry.m_Attributes, entry.m_IsDirectory);
    context.m_CreationTimeNanos = entry.m_CreationTimeNanos;
    context.m_AccessTimeNanos = entry.m_AccessTimeNanos;
    context.m_ModificationTimeNanos = entry.m_ModificationTimeNanos;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::FillFileInfo(FSP_FSCTL_FILE_INFO* fileInfo, const DirEntry& entry)
{
    fileInfo->FileAttributes      = PosixModeToFileAttributes(entry.m_Attributes, entry.m_IsDirectory);
    fileInfo->ReparseTag          = 0;
    fileInfo->FileSize            = entry.m_IsDirectory ? 0 : UINT64(entry.m_Size);
    fileInfo->AllocationSize      = (fileInfo->FileSize + 511) & ~UINT64(511);
    fileInfo->CreationTime        = UnixNanosToWinTime(entry.m_CreationTimeNanos);
    fileInfo->LastAccessTime      = UnixNanosToWinTime(entry.m_AccessTimeNanos);
    fileInfo->LastWriteTime       = UnixNanosToWinTime(entry.m_ModificationTimeNanos);
    fileInfo->ChangeTime          = fileInfo->CreationTime;
    fileInfo->HardLinks           = 0;
    fileInfo->IndexNumber         = 0;
    fileInfo->EaSize              = 0;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void DeviceFilesystem::FillFileInfo(FSP_FSCTL_FILE_INFO* fileInfo, const FileContext& context)
{
    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->FileAttributes = context.m_FileAttributes;
    fileInfo->FileSize       = context.m_FileSize;
    fileInfo->AllocationSize = (fileInfo->FileSize + 511) & ~UINT64(511);
    fileInfo->CreationTime   = UnixNanosToWinTime(context.m_CreationTimeNanos);
    fileInfo->LastAccessTime = UnixNanosToWinTime(context.m_AccessTimeNanos);
    fileInfo->LastWriteTime  = UnixNanosToWinTime(context.m_ModificationTimeNanos);
    fileInfo->ChangeTime     = fileInfo->CreationTime;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::string DeviceFilesystem::WideToUtf8(PWSTR wide)
{
    if (wide == nullptr) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::wstring DeviceFilesystem::Utf8ToWide(const std::string& utf8)
{
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), size);
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

DeviceFilesystem* DeviceFilesystem::Self(FSP_FILE_SYSTEM* fs)
{
    return static_cast<DeviceFilesystem*>(fs->UserContext);
}

///////////////////////////////////////////////////////////////////////////////
/// WinFsp paths arrive as Win32-style wide strings starting with '\'.
/// Convert to POSIX-style UTF-8 with forward slashes rooted at "/sdcard".
///
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

std::string DeviceFilesystem::WinPathToDevice(PWSTR winPath)
{
    std::string utf8 = DeviceFilesystem::WideToUtf8(winPath);
    for (char& ch : utf8) {
        if (ch == '\\') { ch = '/'; }
    }
    // Strip a trailing slash unless it's the root.
    if (utf8.size() > 1 && utf8.back() == '/') {
        utf8.pop_back();
    }
    return "/sdcard" + utf8;
}


////////// WinFsp callback implementations //////////


///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspGetVolumeInfo(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* volumeInfo)
{
    DeviceFilesystem* self = Self(fs);
    int64_t totalBytes = 0;
    int64_t freeBytes  = 0;

    self->SendGetVolumeInfo(totalBytes, freeBytes);

    volumeInfo->TotalSize    = UINT64(totalBytes);
    volumeInfo->FreeSize     = UINT64(freeBytes);
    const std::wstring volumeLabel = self->m_DeviceSession->GetOptions().FilesystemName.toStdWString();
    const size_t volumeLabelLength = std::min(volumeLabel.size(), std::size(volumeInfo->VolumeLabel));
    volumeInfo->VolumeLabelLength = static_cast<UINT16>(volumeLabelLength * sizeof(WCHAR));
    wmemcpy(volumeInfo->VolumeLabel, volumeLabel.data(), volumeLabelLength);

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspGetSecurityByName(FSP_FILE_SYSTEM* fs, PWSTR fileName, PUINT32 pFileAttributes, PSECURITY_DESCRIPTOR securityDescriptor, SIZE_T* pSecurityDescriptorSize)
{
    DeviceFilesystem* self = Self(fs);

    UINT32 attrs = FILE_ATTRIBUTE_NORMAL;

    if (wcscmp(fileName, L"\\") == 0)
    {
        attrs = FILE_ATTRIBUTE_DIRECTORY;
    }
    else
    {
        std::string path = WinPathToDevice(fileName);
        std::optional<DirEntry> entry = self->FindCachedEntry(path);
        if (!entry.has_value()) {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        attrs = PosixModeToFileAttributes(entry->m_Attributes, entry->m_IsDirectory);
    }
    if (pFileAttributes != nullptr) {
        *pFileAttributes = attrs;
    }
    if (pSecurityDescriptorSize != nullptr)
    {
        if (*pSecurityDescriptorSize < s_SecurityDescriptorSize)
        {
            *pSecurityDescriptorSize = s_SecurityDescriptorSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        *pSecurityDescriptorSize = s_SecurityDescriptorSize;
        if (securityDescriptor != nullptr) {
            memcpy(securityDescriptor, s_SecurityDescriptor, s_SecurityDescriptorSize);
        }
    }
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspOpen(FSP_FILE_SYSTEM* fs, PWSTR fileName, UINT32 createOptions, UINT32 grantedAccess, PVOID* pFileContext, FSP_FSCTL_FILE_INFO* fileInfo)
{
    *pFileContext = nullptr;

    DeviceFilesystem* self = Self(fs);
    std::unique_ptr<FileContext> context(new FileContext());

    if (wcscmp(fileName, L"\\") == 0)
    {
        context->m_Path           = "/sdcard";
        context->m_IsDirectory    = true;
        context->m_DeviceHandle   = -1;
        context->m_FileSize       = 0;
        context->m_FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        const int64_t currentTime = CurrentUnixTimeNanos();
        context->m_CreationTimeNanos = currentTime;
        context->m_AccessTimeNanos = currentTime;
        context->m_ModificationTimeNanos = currentTime;

        FillFileInfo(fileInfo, *context);
    }
    else
    {
        std::string path = WinPathToDevice(fileName);
        std::optional<DirEntry> entry = self->FindCachedEntry(path);
        if (!entry.has_value())
        {
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        context->m_Path           = path;
        ApplyDirEntryToContext(*context, *entry);
        FillFileInfo(fileInfo, *context);

        if (entry->m_IsDirectory)
        {
            context->m_DeviceHandle = -1;
        }
        else
        {
            SerialProtocol::FilesystemError err = SerialProtocol::FilesystemError::OK;
            const int32_t openFlags = WinFspOpenFlagsFromGrantedAccess(grantedAccess);
            int32_t handle = self->SendOpenFile(path, openFlags, SerialProtocol::FILESYSTEM_DEFAULT_FILE_PERMISSIONS, err);
            if (handle < 0)
            {
                return FilesystemErrorToNtStatus(err);
            }
            context->m_DeviceHandle = handle;
        }
    }
    *pFileContext = context.release();

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspCreate(FSP_FILE_SYSTEM* fs, PWSTR fileName, UINT32 createOptions, UINT32 grantedAccess, UINT32 fileAttributes, PSECURITY_DESCRIPTOR /*securityDescriptor*/, UINT64 /*allocationSize*/, PVOID* pFileContext, FSP_FSCTL_FILE_INFO* fileInfo)
{
    *pFileContext = nullptr;

    DeviceFilesystem* self = Self(fs);
    std::string path = WinPathToDevice(fileName);
    std::unique_ptr<FileContext> context(new FileContext());
    context->m_Path     = path;
    context->m_FileSize = 0;
    const int64_t currentTime = CurrentUnixTimeNanos();
    context->m_CreationTimeNanos = currentTime;
    context->m_AccessTimeNanos = currentTime;
    context->m_ModificationTimeNanos = currentTime;

    const bool createDir = (createOptions & FILE_DIRECTORY_FILE) != 0;
    context->m_IsDirectory = createDir;
    context->m_FileAttributes = createDir ? FILE_ATTRIBUTE_DIRECTORY
                                          : ((fileAttributes != 0) ? fileAttributes : FILE_ATTRIBUTE_NORMAL);

    if (createDir)
    {
        SerialProtocol::FilesystemError err = self->SendCreateDirectory(path);
        context->m_DeviceHandle = -1;
        if (err != SerialProtocol::FilesystemError::OK)
        {
            return STATUS_ACCESS_DENIED;
        }
    }
    else
    {
        SerialProtocol::FilesystemError err = SerialProtocol::FilesystemError::OK;
        const int32_t openFlags = WinFspOpenFlagsFromGrantedAccess(grantedAccess) |
            SerialProtocol::FilesystemOpenFlags::Create |
            SerialProtocol::FilesystemOpenFlags::Truncate;
        int32_t handle = self->SendOpenFile(path, openFlags, SerialProtocol::FILESYSTEM_DEFAULT_FILE_PERMISSIONS, err);
        if (handle < 0)
        {
            return FilesystemErrorToNtStatus(err);
        }
        context->m_DeviceHandle = handle;
    }
    self->InvalidateDirCache(self->ParentPath(path));
    self->RefreshContextFromDirectory(*context);

    FillFileInfo(fileInfo, *context);

    *pFileContext = context.release();

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspOverwrite(FSP_FILE_SYSTEM* fs, PVOID fileContext, UINT32 /*fileAttributes*/, BOOLEAN /*replaceFileAttributes*/, UINT64 /*allocationSize*/, FSP_FSCTL_FILE_INFO* fileInfo)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);

    if (context->m_DeviceHandle >= 0) {
        self->SendCloseFile(context->m_DeviceHandle);
    }
    SerialProtocol::FilesystemError err = SerialProtocol::FilesystemError::OK;
    const int32_t openFlags = SerialProtocol::FilesystemOpenFlags::Write |
        SerialProtocol::FilesystemOpenFlags::Create |
        SerialProtocol::FilesystemOpenFlags::Truncate;
    int32_t handle = self->SendOpenFile(context->m_Path, openFlags, SerialProtocol::FILESYSTEM_DEFAULT_FILE_PERMISSIONS, err);
    if (handle < 0)
    {
        context->m_DeviceHandle = -1;
        return FilesystemErrorToNtStatus(err);
    }
    context->m_DeviceHandle = handle;
    context->m_FileSize = 0;
    context->m_FileAttributes = FILE_ATTRIBUTE_NORMAL;
    const int64_t currentTime = CurrentUnixTimeNanos();
    context->m_AccessTimeNanos = currentTime;
    context->m_ModificationTimeNanos = currentTime;
    self->InvalidateDirCache(self->ParentPath(context->m_Path));
    self->RefreshContextFromDirectory(*context);

    FillFileInfo(fileInfo, *context);
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

VOID DeviceFilesystem::WinFspCleanup(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR fileName, ULONG flags)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);
    if (context == nullptr) {
        return;
    }

    std::string path = context->m_Path;
    if (path.empty() && fileName != nullptr) {
        path = WinPathToDevice(fileName);
    }
    if (path.empty()) {
        return;
    }

    const bool shouldDelete = (flags & FspCleanupDelete) != 0;
    const bool metadataChanged = (flags & (
        FspCleanupSetAllocationSize |
        FspCleanupSetArchiveBit |
        FspCleanupSetLastAccessTime |
        FspCleanupSetLastWriteTime |
        FspCleanupSetChangeTime)) != 0;

    if (shouldDelete)
    {
        if (context->m_DeviceHandle >= 0)
        {
            self->SendCloseFile(context->m_DeviceHandle);
            context->m_DeviceHandle = -1;
        }
        self->SendDeleteFile(path);
        self->InvalidateDirCache(path);
        self->InvalidateDirCache(self->ParentPath(path));
        return;
    }
    if (metadataChanged) {
        self->InvalidateDirCache(self->ParentPath(path));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

VOID DeviceFilesystem::WinFspClose(FSP_FILE_SYSTEM* fs, PVOID fileContext)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);
    if (context->m_DeviceHandle >= 0) {
        self->SendCloseFile(context->m_DeviceHandle);
    }
    delete context;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspRead(FSP_FILE_SYSTEM* fs, PVOID fileContext, PVOID buffer, UINT64 offset, ULONG length, PULONG pBytesTransferred)
{
    DeviceFilesystem* self = Self(fs);
    const FileContext* context = static_cast<FileContext*>(fileContext);
    if (context->m_DeviceHandle < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    ULONG totalRead = 0;
    ULONG remaining = length;
    UINT64 pos      = offset;

    while (remaining > 0)
    {
        int32_t chunkSize = int32_t(std::min<ULONG>(remaining, SerialProtocol::FILESYSTEM_IOBUFFER_SIZE));
        std::vector<char> data;
        SerialProtocol::FilesystemError err = self->SendReadFile(context->m_DeviceHandle, pos, chunkSize, data);
        if (err != SerialProtocol::FilesystemError::OK || data.empty()) {
            break;
        }
        memcpy(static_cast<BYTE*>(buffer) + totalRead, data.data(), data.size());
        totalRead += ULONG(data.size());
        pos       += data.size();
        remaining -= ULONG(data.size());
        if (int32_t(data.size()) < chunkSize) {
            break; // EOF
        }
    }
    *pBytesTransferred = totalRead;
    return (totalRead > 0) ? STATUS_SUCCESS : STATUS_END_OF_FILE;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspWrite(FSP_FILE_SYSTEM* fs, PVOID fileContext, PVOID buffer, UINT64 offset, ULONG length, BOOLEAN writeToEndOfFile, BOOLEAN constrainedIo, PULONG pBytesTransferred, FSP_FSCTL_FILE_INFO* fileInfo)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);
    if (context->m_DeviceHandle < 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (writeToEndOfFile) {
        offset = context->m_FileSize;
    }
    if (constrainedIo)
    {
        if (offset >= context->m_FileSize)
        {
            *pBytesTransferred = 0;
            FillFileInfo(fileInfo, *context);
            return STATUS_SUCCESS;
        }
        length = ULONG(std::min<UINT64>(length, context->m_FileSize - offset));
    }
    if (length == 0)
    {
        *pBytesTransferred = 0;
        FillFileInfo(fileInfo, *context);
        return STATUS_SUCCESS;
    }

    ULONG totalWritten = 0;
    ULONG remaining    = length;
    UINT64 pos         = offset;
    SerialProtocol::FilesystemError lastError = SerialProtocol::FilesystemError::OK;

    while (remaining > 0)
    {
        int32_t chunkSize = int32_t(std::min<ULONG>(remaining, SerialProtocol::FILESYSTEM_IOBUFFER_SIZE));
        int64_t bytesWritten = 0;

        SerialProtocol::FilesystemError err = self->SendWriteFile(
            context->m_DeviceHandle, pos,
            static_cast<const BYTE*>(buffer) + totalWritten,
            chunkSize, bytesWritten
        );

        if (err != SerialProtocol::FilesystemError::OK || bytesWritten < 0)
        {
            lastError = (err != SerialProtocol::FilesystemError::OK) ? err : SerialProtocol::FilesystemError::IOError;
            break;
        }
        const int64_t written = bytesWritten - static_cast<int64_t>(pos); // firmware returns new file position
        if (written <= 0)
        {
            lastError = SerialProtocol::FilesystemError::IOError;
            break;
        }
        totalWritten += ULONG(written);
        pos          += written;
        remaining    -= ULONG(written);
    }
    *pBytesTransferred = totalWritten;

    if (pos > context->m_FileSize) {
        context->m_FileSize = pos;
    }

    if (totalWritten > 0)
    {
        self->InvalidateDirCache(self->ParentPath(context->m_Path));
        self->RefreshContextFromDirectory(*context);
        FillFileInfo(fileInfo, *context);
        return STATUS_SUCCESS;
    }

    FillFileInfo(fileInfo, *context);

    return FilesystemErrorToNtStatus(lastError);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspFlush(FSP_FILE_SYSTEM* /*fs*/, PVOID fileContext, FSP_FSCTL_FILE_INFO* fileInfo)
{
    if (fileInfo != nullptr && fileContext != nullptr)
    {
        const FileContext* context = static_cast<const FileContext*>(fileContext);
        FillFileInfo(fileInfo, *context);
    }
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspGetFileInfo(FSP_FILE_SYSTEM* fs, PVOID fileContext, FSP_FSCTL_FILE_INFO* fileInfo)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);

    if (context->m_Path == "/sdcard")
    {
        FillFileInfo(fileInfo, *context);
        return STATUS_SUCCESS;
    }

    std::optional<DirEntry> entry = self->FindCachedEntry(context->m_Path);

    if (!entry.has_value()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    ApplyDirEntryToContext(*context, *entry);
    FillFileInfo(fileInfo, *context);

    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspSetBasicInfo(FSP_FILE_SYSTEM* fs, PVOID fileContext, UINT32 fileAttributes, UINT64 creationTime, UINT64 lastAccessTime, UINT64 lastWriteTime, UINT64 /*changeTime*/, FSP_FSCTL_FILE_INFO* fileInfo)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);

    uint32_t mask = 0;
    uint32_t mode = 0;
    UINT32 newFileAttributes = context->m_FileAttributes;
    int64_t accessTimeNanos = context->m_AccessTimeNanos;
    int64_t modificationTimeNanos = context->m_ModificationTimeNanos;
    int64_t creationTimeNanos = context->m_CreationTimeNanos;

    if (fileAttributes != INVALID_FILE_ATTRIBUTES)
    {
        newFileAttributes = context->m_IsDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        newFileAttributes |= fileAttributes & FILE_ATTRIBUTE_READONLY;
        mode = FileAttributesToPosixMode(newFileAttributes, context->m_IsDirectory);
        mask |= SerialProtocol::FilesystemStatMask::Mode;
    }
    if (lastAccessTime != 0)
    {
        accessTimeNanos = WinTimeToUnixNanos(lastAccessTime);
        mask |= SerialProtocol::FilesystemStatMask::AccessTime;
    }
    if (lastWriteTime != 0)
    {
        modificationTimeNanos = WinTimeToUnixNanos(lastWriteTime);
        mask |= SerialProtocol::FilesystemStatMask::ModificationTime;
    }
    if (creationTime != 0)
    {
        creationTimeNanos = WinTimeToUnixNanos(creationTime);
        mask |= SerialProtocol::FilesystemStatMask::CreationTime;
    }
    if (mask != 0)
    {
        int32_t handle = context->m_DeviceHandle;
        bool closeHandle = false;
        if (handle < 0)
        {
            SerialProtocol::FilesystemError openStatus = SerialProtocol::FilesystemError::OK;
            handle = self->SendOpenFile(context->m_Path, SerialProtocol::FilesystemOpenFlags::Read, SerialProtocol::FILESYSTEM_DEFAULT_FILE_PERMISSIONS, openStatus);
            if (handle < 0)
            {
                return FilesystemErrorToNtStatus(openStatus);
            }
            closeHandle = true;
        }

        const SerialProtocol::FilesystemError status = self->SendSetFileStat(
            handle,
            mask,
            mode,
            int64_t(context->m_FileSize),
            accessTimeNanos,
            modificationTimeNanos,
            creationTimeNanos);
        if (closeHandle) {
            self->SendCloseFile(handle);
        }
        if (status != SerialProtocol::FilesystemError::OK) {
            return FilesystemErrorToNtStatus(status);
        }

        context->m_FileAttributes = newFileAttributes;
        context->m_AccessTimeNanos = accessTimeNanos;
        context->m_ModificationTimeNanos = modificationTimeNanos;
        context->m_CreationTimeNanos = creationTimeNanos;
        self->InvalidateDirCache(self->ParentPath(context->m_Path));
        self->RefreshContextFromDirectory(*context);
    }

    FillFileInfo(fileInfo, *context);
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspSetFileSize(FSP_FILE_SYSTEM* fs, PVOID fileContext, UINT64 newSize, BOOLEAN setAllocationSize, FSP_FSCTL_FILE_INFO* fileInfo)
{
    DeviceFilesystem* self = Self(fs);
    FileContext* context = static_cast<FileContext*>(fileContext);
    if (context->m_DeviceHandle < 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (newSize > UINT64(std::numeric_limits<int64_t>::max())) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!setAllocationSize)
    {
        SerialProtocol::FilesystemError err = self->SendSetFileStat(
            context->m_DeviceHandle,
            SerialProtocol::FilesystemStatMask::Size,
            0,
            static_cast<int64_t>(newSize),
            0,
            0,
            0);
        if (err != SerialProtocol::FilesystemError::OK) {
            return FilesystemErrorToNtStatus(err);
        }
        context->m_FileSize = newSize;
        self->InvalidateDirCache(self->ParentPath(context->m_Path));
        self->RefreshContextFromDirectory(*context);
    }

    FillFileInfo(fileInfo, *context);
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspCanDelete(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR /*fileName*/)
{
    DeviceFilesystem* self = Self(fs);
    const FileContext* context = static_cast<const FileContext*>(fileContext);

    std::optional<DirEntry> entry = self->FindCachedEntry(context->m_Path);
    if (!entry.has_value()) {
        return STATUS_SUCCESS; // Not cached; allow and let Delete handle any error.
    }
    if ((entry->m_Attributes & 0222) == 0) { // no write bits == read-only
        return STATUS_ACCESS_DENIED;
    }
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspRename(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR fileName, PWSTR newFileName, BOOLEAN /*replaceIfExists*/)
{
    DeviceFilesystem* self = Self(fs);
    std::string oldPath = WinPathToDevice(fileName);
    std::string newPath = WinPathToDevice(newFileName);

    SerialProtocol::FilesystemError err = self->SendRenameFile(oldPath, newPath);
    if (err != SerialProtocol::FilesystemError::OK) {
        return STATUS_ACCESS_DENIED;
    }
    self->InvalidateDirCache(self->ParentPath(oldPath));
    self->InvalidateDirCache(self->ParentPath(newPath));

    FileContext* context = static_cast<FileContext*>(fileContext);
    context->m_Path = newPath;
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspGetSecurity(FSP_FILE_SYSTEM* /*fs*/, PVOID /*fileContext*/, PSECURITY_DESCRIPTOR securityDescriptor, SIZE_T* pSecurityDescriptorSize)
{
    if (pSecurityDescriptorSize != nullptr)
    {
        if (*pSecurityDescriptorSize < s_SecurityDescriptorSize) {
            *pSecurityDescriptorSize = s_SecurityDescriptorSize;
            return STATUS_BUFFER_OVERFLOW;
        }
        *pSecurityDescriptorSize = s_SecurityDescriptorSize;
        if (securityDescriptor != nullptr) {
            memcpy(securityDescriptor, s_SecurityDescriptor, s_SecurityDescriptorSize);
        }
    }
    return STATUS_SUCCESS;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

NTSTATUS DeviceFilesystem::WinFspReadDirectory(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR /*pattern*/, PWSTR marker, PVOID buffer, ULONG length, PULONG pBytesTransferred)
{
    DeviceFilesystem* self = Self(fs);
    const FileContext* context = static_cast<const FileContext*>(fileContext);

    // Always fetch fresh from device; cache is only for attribute lookups.
    std::vector<DirEntry> entries;
    self->SendGetDirectory(context->m_Path, entries);

    // Also update cache with freshly fetched data.
    {
        std::lock_guard<std::mutex> lock(self->m_CacheMutex);
        CachedDir& cd = self->m_DirCache[context->m_Path];
        cd.m_Entries   = entries;
        cd.m_FetchTime = std::chrono::steady_clock::now();
    }

    const UINT64 now = UnixNanosToWinTime(CurrentUnixTimeNanos());
    std::wstring markerStr = marker ? std::wstring(marker) : std::wstring();
    bool pastMarker = markerStr.empty();

    auto addDirEntry = [&](const wchar_t* name, const FSP_FSCTL_FILE_INFO& fi) -> bool
    {
        if (!pastMarker)
        {
            if (markerStr == name) { pastMarker = true; }
            return true;
        }
        union
        {
            UINT8 buf[sizeof(FSP_FSCTL_DIR_INFO) + 256 * sizeof(WCHAR)];
            FSP_FSCTL_DIR_INFO dirInfo;
        };
        memset(&dirInfo, 0, sizeof(FSP_FSCTL_DIR_INFO));
        dirInfo.FileInfo = fi;
        size_t nameLen   = wcslen(name);
        dirInfo.Size     = UINT16(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + nameLen * sizeof(WCHAR));
        wmemcpy(dirInfo.FileNameBuf, name, nameLen);
        return FspFileSystemAddDirInfo(&dirInfo, buffer, length, pBytesTransferred);
    };

    FSP_FSCTL_FILE_INFO dotInfo = {};
    dotInfo.FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    dotInfo.CreationTime = dotInfo.LastAccessTime = dotInfo.LastWriteTime = dotInfo.ChangeTime = now;
    addDirEntry(L".", dotInfo);
    addDirEntry(L"..", dotInfo);

    for (const DirEntry& entry : entries)
    {
        if (entry.m_Name == "." || entry.m_Name == "..") {
            continue;
        }
        FSP_FSCTL_FILE_INFO fi = {};
        FillFileInfo(&fi, entry);
        std::wstring wideName = Utf8ToWide(entry.m_Name);
        if (!addDirEntry(wideName.c_str(), fi)) {
            break;
        }
    }
    FspFileSystemAddDirInfo(nullptr, buffer, length, pBytesTransferred);
    return STATUS_SUCCESS;
}

#endif // HAVE_WINFSP
