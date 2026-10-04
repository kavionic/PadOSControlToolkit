// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/AsyncSerialHandlers/SerialFileWriter.h"

#include "PadOSControl/Core/SerialHandler.h"

#include <algorithm>
#include <utility>

static constexpr int MAX_SERIAL_FILE_PATH_BYTES = 1024;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileWriter::SerialFileWriter(SerialHandler& serialHandler, int32_t sessionID)
    : m_SerialHandler(serialHandler)
    , m_SessionID(sessionID)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::OpenFileReply>(this, &SerialFileWriter::HandleOpenFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::WriteFileReply>(this, &SerialFileWriter::HandleWriteFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::SetFileStatReply>(this, &SerialFileWriter::HandleSetFileStatReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::CloseFileReply>(this, &SerialFileWriter::HandleCloseFileReply);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileWriter::~SerialFileWriter()
{
    Cancel();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::SetDataNeededHandler(DataNeededHandler handler)
{
    m_DataNeededHandler = std::move(handler);
    m_HasDataNeededHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::SetFinishedHandler(FinishedHandler handler)
{
    m_FinishedHandler = std::move(handler);
    m_HasFinishedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileWriter::Result SerialFileWriter::Start(const QString& path, int64_t size, std::optional<int64_t> modificationTimeNanos)
{
    Result result = Result::Busy;
    if (m_State == State::Idle)
    {
        const QByteArray utf8Path = path.toUtf8();
        result = Result::PathTooLong;
        if (utf8Path.size() < MAX_SERIAL_FILE_PATH_BYTES)
        {
            m_File = -1;
            m_Offset = 0;
            m_Size = size;
            m_ModificationTimeNanos = modificationTimeNanos;
            m_Result = Result::OK;
            m_State = State::Creating;
            m_SerialHandler.SendMessage<SerialProtocol::CreateFile>(m_SessionID, utf8Path.data(), utf8Path.size());
            StartTimeout();
            result = Result::OK;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::Cancel()
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

bool SerialFileWriter::IsActive() const
{
    return m_State != State::Idle && m_State != State::Finished;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SerialFileWriter::GetResultText(Result result)
{
    switch (result)
    {
        case Result::OK:
            return "ok";
        case Result::Busy:
            return "writer is already busy";
        case Result::PathTooLong:
            return "device path is too long";
        case Result::CreateFailed:
            return "failed to create device file";
        case Result::DataProviderFailed:
            return "failed to read upload data";
        case Result::WriteFailed:
            return "failed to write device file";
        case Result::SetFileStatFailed:
            return "failed to update device timestamp";
        case Result::CloseFailed:
            return "failed to close device file";
        case Result::Canceled:
            return "operation canceled";
        case Result::Timeout:
            return "device operation timed out";
    }
    return "unknown error";
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Creating)
    {
        StopTimeout();
        if (packet.m_Status == SerialProtocol::FilesystemError::OK && packet.m_File >= 0)
        {
            m_File = packet.m_File;
            SendNextWrite();
        }
        else
        {
            Finish(Result::CreateFailed);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::HandleWriteFileReply(const SerialProtocol::WriteFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Writing && packet.m_File == m_File)
    {
        StopTimeout();
        if (packet.m_Status == SerialProtocol::FilesystemError::OK && packet.m_BytesWritten > m_Offset)
        {
            m_Offset = packet.m_BytesWritten;
            SendNextWrite();
        }
        else
        {
            CloseFile(Result::WriteFailed);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::HandleSetFileStatReply(const SerialProtocol::SetFileStatReply& packet)
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

void SerialFileWriter::HandleCloseFileReply(const SerialProtocol::CloseFileReply& packet)
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

void SerialFileWriter::HandleTimeout()
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

void SerialFileWriter::SendNextWrite()
{
    if (m_File < 0)
    {
        Finish(Result::CreateFailed);
        return;
    }
    if (m_Offset >= m_Size)
    {
        SendFileStat();
        return;
    }

    QByteArray data;
    const int64_t remainingBytes = m_Size - m_Offset;
    const int32_t maxSize = int32_t(std::min<int64_t>(sizeof(SerialProtocol::WriteFile::m_Buffer), remainingBytes));
    Result dataResult = Result::DataProviderFailed;
    if (m_HasDataNeededHandler)
    {
        dataResult = m_DataNeededHandler(m_Offset, maxSize, data);
    }
    if (dataResult != Result::OK)
    {
        CloseFile(dataResult);
        return;
    }
    if (data.size() <= 0 || data.size() > maxSize)
    {
        CloseFile(Result::DataProviderFailed);
        return;
    }

    m_State = State::Writing;
    m_SerialHandler.SendMessage<SerialProtocol::WriteFile>(m_SessionID, m_File, data.constData(), m_Offset, data.size());
    StartTimeout();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::SendFileStat()
{
    if (m_ModificationTimeNanos.has_value())
    {
        m_State = State::SettingFileStat;
        m_SerialHandler.SendMessage<SerialProtocol::SetFileStat>(
            m_SessionID,
            m_File,
            SerialProtocol::FilesystemStatMask::ModificationTime,
            0,
            0,
            0,
            *m_ModificationTimeNanos,
            0
        );
        StartTimeout();
    }
    else
    {
        CloseFile(Result::OK);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileWriter::CloseFile(Result result)
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

void SerialFileWriter::Finish(Result result)
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
