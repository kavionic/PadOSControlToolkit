// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/AsyncSerialHandlers/SerialSetFileStat.h"

#include "PadOSControl/Core/SerialHandler.h"

#include <utility>

static constexpr int MAX_SERIAL_FILE_PATH_BYTES = 1024;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialSetFileStat::SerialSetFileStat(SerialHandler& serialHandler, int32_t sessionID)
    : m_SerialHandler(serialHandler)
    , m_SessionID(sessionID)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::OpenFileReply>(this, &SerialSetFileStat::HandleOpenFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::SetFileStatReply>(this, &SerialSetFileStat::HandleSetFileStatReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::CloseFileReply>(this, &SerialSetFileStat::HandleCloseFileReply);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialSetFileStat::~SerialSetFileStat()
{
    Cancel();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::SetFinishedHandler(FinishedHandler handler)
{
    m_FinishedHandler = std::move(handler);
    m_HasFinishedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialSetFileStat::Result SerialSetFileStat::Start(
    const QString& path,
    uint32_t mask,
    uint32_t mode,
    int64_t size,
    int64_t accessTimeNanos,
    int64_t modificationTimeNanos,
    int64_t creationTimeNanos
)
{
    Result result = Result::Busy;
    if (m_State == State::Idle)
    {
        const QByteArray utf8Path = path.toUtf8();
        result = Result::PathTooLong;
        if (utf8Path.size() < MAX_SERIAL_FILE_PATH_BYTES)
        {
            m_File = -1;
            m_Mask = mask;
            m_Mode = mode;
            m_Size = size;
            m_AccessTimeNanos = accessTimeNanos;
            m_ModificationTimeNanos = modificationTimeNanos;
            m_CreationTimeNanos = creationTimeNanos;
            m_Result = Result::OK;
            m_State = State::Opening;
            m_SerialHandler.SendMessage<SerialProtocol::OpenFile>(
                m_SessionID,
                utf8Path.data(),
                utf8Path.size(),
                SerialProtocol::FilesystemOpenFlags::Write,
                SerialProtocol::FILESYSTEM_DEFAULT_FILE_PERMISSIONS
            );
            StartTimeout();
            result = Result::OK;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::Cancel()
{
    if (m_File >= 0 && m_State != State::Closing && m_State != State::Finished)
    {
        const int32_t file = m_File;
        m_File = -1;
        m_SerialHandler.SendMessage<SerialProtocol::CloseFile>(m_SessionID, file);
    }
    m_State = State::Finished;
    m_File = -1;
    m_Result = Result::Canceled;
    StopTimeout();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SerialSetFileStat::IsActive() const
{
    return m_State != State::Idle && m_State != State::Finished;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Opening)
    {
        StopTimeout();
        if (packet.m_Status == SerialProtocol::FilesystemError::OK && packet.m_File >= 0)
        {
            m_File = packet.m_File;
            SendFileStat();
        }
        else
        {
            Finish(Result::OpenFailed);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::HandleSetFileStatReply(const SerialProtocol::SetFileStatReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::SettingFileStat)
    {
        StopTimeout();
        CloseFile((packet.m_Status == SerialProtocol::FilesystemError::OK) ? Result::OK : Result::SetFileStatFailed);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::HandleCloseFileReply(const SerialProtocol::CloseFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Closing)
    {
        StopTimeout();
        Finish((m_Result == Result::OK && packet.m_Status != SerialProtocol::FilesystemError::OK) ? Result::CloseFailed : m_Result);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::HandleTimeout()
{
    if (IsActive())
    {
        if (m_File >= 0 && m_State != State::Closing)
        {
            const int32_t file = m_File;
            m_File = -1;
            m_SerialHandler.SendMessage<SerialProtocol::CloseFile>(m_SessionID, file);
        }
        Finish(Result::Timeout);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::SendFileStat()
{
    if (m_File >= 0)
    {
        m_State = State::SettingFileStat;
        m_SerialHandler.SendMessage<SerialProtocol::SetFileStat>(
            m_SessionID,
            m_File,
            m_Mask,
            m_Mode,
            m_Size,
            m_AccessTimeNanos,
            m_ModificationTimeNanos,
            m_CreationTimeNanos
        );
        StartTimeout();
    }
    else
    {
        Finish(Result::OpenFailed);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::CloseFile(Result result)
{
    m_Result = result;
    if (m_File >= 0)
    {
        m_State = State::Closing;
        m_SerialHandler.SendMessage<SerialProtocol::CloseFile>(m_SessionID, m_File);
        StartTimeout();
    }
    else
    {
        Finish(result);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialSetFileStat::Finish(Result result)
{
    m_State = State::Finished;
    m_File = -1;
    StopTimeout();
    FinishedHandler finishedHandler = m_FinishedHandler;
    if (m_HasFinishedHandler)
    {
        finishedHandler(result);
    }
}
