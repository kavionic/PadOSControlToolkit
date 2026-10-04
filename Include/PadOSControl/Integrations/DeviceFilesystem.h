// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <condition_variable>

#ifdef HAVE_WINFSP

#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <random>
#include <chrono>
#include <optional>
#include <vector>
#include <QObject>
#include <QString>

#include "SerialConsole/FilesystemMessages.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winfsp/winfsp.h>

// Windows headers define these as macros (CreateFileW, etc.) which mangle
// SerialProtocol and SerialHandler names that share the same identifiers.
#ifdef CreateFile
#undef CreateFile
#endif
#ifdef DeleteFile
#undef DeleteFile
#endif
#ifdef CreateDirectory
#undef CreateDirectory
#endif
#ifdef SendMessage
#undef SendMessage
#endif

class SerialHandler;
class DeviceSession;

enum class MainState : int;

class DeviceFilesystem : public QObject
{
    Q_OBJECT
public:
    explicit DeviceFilesystem(SerialHandler& serialHandler, DeviceSession* deviceSession);
    ~DeviceFilesystem();

    bool Mount(wchar_t driveLetter);
    void Unmount();
    QString GetMountRootPath() const;

Q_SIGNALS:
    void SignalMountStatusChanged(const QString& text);

private slots:
    void SlotMainStateChanged(MainState state);
    void SlotOpenSessionReply(const SerialProtocol::OpenSessionReply& packet);
    void SlotGetDirectoryReply(const SerialProtocol::GetDirectoryReply& packet);
    void SlotOpenFileReply(const SerialProtocol::OpenFileReply& packet);
    void SlotReadFileReply(const SerialProtocol::ReadFileReply& packet);
    void SlotWriteFileReply(const SerialProtocol::WriteFileReply& packet);
    void SlotSetFileStatReply(const SerialProtocol::SetFileStatReply& packet);
    void SlotCloseFileReply(const SerialProtocol::CloseFileReply& packet);
    void SlotCreateDirectoryReply(const SerialProtocol::CreateDirectoryReply& packet);
    void SlotDeleteFileReply(const SerialProtocol::DeleteFileReply& packet);
    void SlotFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet);
    void SlotGetVolumeInfoReply(const SerialProtocol::GetVolumeInfoReply& packet);

private:
    // Directory cache entry
    struct DirEntry
    {
        int64_t  m_Size;
        int64_t  m_CreationTimeNanos;
        int64_t  m_AccessTimeNanos;
        int64_t  m_ModificationTimeNanos;
        uint32_t m_Attributes;
        bool     m_IsDirectory;
        std::string m_Name;
    };

    struct CachedDir
    {
        std::vector<DirEntry>                    m_Entries;
        std::chrono::steady_clock::time_point    m_FetchTime;
    };

    // Per-open file context stored as WinFsp file node user context
    struct FileContext
    {
        std::string m_Path;
        bool        m_IsDirectory;
        int32_t     m_DeviceHandle; // -1 for directories
        UINT64      m_FileSize = 0;
        UINT32      m_FileAttributes = FILE_ATTRIBUTE_NORMAL;
        int64_t     m_CreationTimeNanos = 0;
        int64_t     m_AccessTimeNanos = 0;
        int64_t     m_ModificationTimeNanos = 0;
    };

    // Blocking serial operation result holder
    struct PendingResult
    {
        std::mutex              m_Mutex;
        std::condition_variable m_Condition;
        bool                    m_Ready = false;

        // Fields populated by reply handlers:
        SerialProtocol::FilesystemError m_Status     = SerialProtocol::FilesystemError::OK;
        int32_t                         m_Handle      = -1;
        int32_t                         m_BytesResult = -1;
        int64_t                         m_TotalBytes  = 0;
        int64_t                         m_FreeBytes   = 0;
        std::vector<char>               m_Data;
        std::vector<DirEntry>           m_DirEntries;
        bool                            m_DirDone     = false;
    };

    // Ensures a session is open; called while m_SerialOpMutex is already held.
    void EnsureSession_Locked();

    // Serial operations (called from WinFsp worker threads, block until reply)
    int32_t          SendOpenFile(const std::string& path, int32_t openFlags, int32_t permissions, SerialProtocol::FilesystemError& outStatus);
    int32_t          SendCreateFile(const std::string& path);
    SerialProtocol::FilesystemError SendCloseFile(int32_t handle);
    SerialProtocol::FilesystemError SendReadFile(int32_t handle, int64_t offset, int32_t size, std::vector<char>& outData);
    SerialProtocol::FilesystemError SendWriteFile(int32_t handle, int64_t offset, const void* data, int32_t size, int64_t& outBytesWritten);
    SerialProtocol::FilesystemError SendSetFileStat(int32_t handle, uint32_t mask, uint32_t mode, int64_t size, int64_t accessTimeNanos, int64_t modificationTimeNanos, int64_t creationTimeNanos);
    SerialProtocol::FilesystemError SendCreateDirectory(const std::string& path);
    SerialProtocol::FilesystemError SendDeleteFile(const std::string& path);
    SerialProtocol::FilesystemError SendRenameFile(const std::string& oldPath, const std::string& newPath);
    bool                            SendGetDirectory(const std::string& path, std::vector<DirEntry>& outEntries);
    bool                            SendGetVolumeInfo(int64_t& outTotal, int64_t& outFree);

    // Directory cache
    const CachedDir* GetCachedDir(const std::string& dirPath);
    std::optional<DirEntry>  FindCachedEntry(const std::string& path);
    void             InvalidateDirCache(const std::string& dirPath);
    std::string      ParentPath(const std::string& path) const;
    bool             RefreshContextFromDirectory(FileContext& context);

    // WinFsp static dispatch functions (C callbacks calling into this instance)
    static NTSTATUS WinFspGetVolumeInfo(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* volumeInfo);
    static NTSTATUS WinFspGetSecurityByName(FSP_FILE_SYSTEM* fs, PWSTR fileName, PUINT32 pFileAttributes, PSECURITY_DESCRIPTOR securityDescriptor, SIZE_T* pSecurityDescriptorSize);
    static NTSTATUS WinFspCreate(FSP_FILE_SYSTEM* fs, PWSTR fileName, UINT32 createOptions, UINT32 grantedAccess, UINT32 fileAttributes, PSECURITY_DESCRIPTOR securityDescriptor, UINT64 allocationSize, PVOID* pFileContext, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspOpen(FSP_FILE_SYSTEM* fs, PWSTR fileName, UINT32 createOptions, UINT32 grantedAccess, PVOID* pFileContext, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspOverwrite(FSP_FILE_SYSTEM* fs, PVOID fileContext, UINT32 fileAttributes, BOOLEAN replaceFileAttributes, UINT64 allocationSize, FSP_FSCTL_FILE_INFO* fileInfo);
    static VOID     WinFspCleanup(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR fileName, ULONG flags);
    static VOID     WinFspClose(FSP_FILE_SYSTEM* fs, PVOID fileContext);
    static NTSTATUS WinFspRead(FSP_FILE_SYSTEM* fs, PVOID fileContext, PVOID buffer, UINT64 offset, ULONG length, PULONG pBytesTransferred);
    static NTSTATUS WinFspWrite(FSP_FILE_SYSTEM* fs, PVOID fileContext, PVOID buffer, UINT64 offset, ULONG length, BOOLEAN writeToEndOfFile, BOOLEAN constrainedIo, PULONG pBytesTransferred, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspFlush(FSP_FILE_SYSTEM* fs, PVOID fileContext, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspGetFileInfo(FSP_FILE_SYSTEM* fs, PVOID fileContext, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspSetBasicInfo(FSP_FILE_SYSTEM* fs, PVOID fileContext, UINT32 fileAttributes, UINT64 creationTime, UINT64 lastAccessTime, UINT64 lastWriteTime, UINT64 changeTime, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspSetFileSize(FSP_FILE_SYSTEM* fs, PVOID fileContext, UINT64 newSize, BOOLEAN setAllocationSize, FSP_FSCTL_FILE_INFO* fileInfo);
    static NTSTATUS WinFspCanDelete(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR fileName);
    static NTSTATUS WinFspRename(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR fileName, PWSTR newFileName, BOOLEAN replaceIfExists);
    static NTSTATUS WinFspGetSecurity(FSP_FILE_SYSTEM* fs, PVOID fileContext, PSECURITY_DESCRIPTOR securityDescriptor, SIZE_T* pSecurityDescriptorSize);
    static NTSTATUS WinFspReadDirectory(FSP_FILE_SYSTEM* fs, PVOID fileContext, PWSTR pattern, PWSTR marker, PVOID buffer, ULONG length, PULONG pBytesTransferred);

    // Helpers
    static UINT32        PosixModeToFileAttributes(uint32_t mode, bool isDirectory);
    static uint32_t      FileAttributesToPosixMode(UINT32 fileAttributes, bool isDirectory);
    static UINT64        UnixNanosToWinTime(int64_t unixNanos);
    static int64_t       WinTimeToUnixNanos(UINT64 winTime);
    static int64_t       CurrentUnixTimeNanos();
    static void          ApplyDirEntryToContext(FileContext& context, const DirEntry& entry);
    static void          FillFileInfo(FSP_FSCTL_FILE_INFO* fileInfo, const DirEntry& entry);
    static void          FillFileInfo(FSP_FSCTL_FILE_INFO* fileInfo, const FileContext& context);
    static std::string   WideToUtf8(PWSTR wide);
    static std::wstring  Utf8ToWide(const std::string& utf8);
    static std::string   WinPathToDevice(PWSTR winPath);
    static void          BuildSecurityDescriptor();
    static DeviceFilesystem* Self(FSP_FILE_SYSTEM* fs);

    SerialHandler&   m_SerialHandler;
    DeviceSession*  m_DeviceSession = nullptr;
    FSP_FILE_SYSTEM* m_FileSystem = nullptr;
    QString          m_MountRootPath;

    std::atomic<int32_t> m_SessionID{-1};
    uint32_t             m_ClientToken = 0;

    // Pending operation — serialized by two mutexes:
    // m_SerialOpMutex: held for the entire Send* call; prevents concurrent serial ops.
    // m_PendingMutex:  guards m_Pending; used with condition_variable in slot handlers.
    std::mutex                   m_SerialOpMutex;
    std::mutex                   m_PendingMutex;
    std::atomic<bool>            m_Unmounting{false};
    std::shared_ptr<PendingResult> m_Pending;

    // Directory cache (protected by m_CacheMutex)
    std::mutex                        m_CacheMutex;
    std::map<std::string, CachedDir>  m_DirCache;

    // Static minimal security descriptor returned for all files
    static BYTE s_SecurityDescriptor[];
    static SIZE_T s_SecurityDescriptorSize;
};

#endif // HAVE_WINFSP
