// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/AsyncSerialHandlers/SerialFileReader.h"

#include "PadOSControl/Core/SerialHandler.h"

#include <algorithm>
#include <utility>

static constexpr int MAX_SERIAL_FILE_PATH_BYTES = 1024;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::SerialFileReader(SerialHandler& serialHandler, int32_t sessionID)
    : m_SerialHandler(serialHandler)
    , m_SessionID(sessionID)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::OpenFileReply>(this, &SerialFileReader::HandleOpenFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::ReadFileReply>(this, &SerialFileReader::HandleReadFileReply);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::CloseFileReply>(this, &SerialFileReader::HandleCloseFileReply);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::~SerialFileReader()
{
    Cancel();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileReader::SetOpenedHandler(OpenedHandler handler)
{
    m_OpenedHandler = std::move(handler);
    m_HasOpenedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileReader::SetDataReadyHandler(DataReadyHandler handler)
{
    m_DataReadyHandler = std::move(handler);
    m_HasDataReadyHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileReader::SetFinishedHandler(FinishedHandler handler)
{
    m_FinishedHandler = std::move(handler);
    m_HasFinishedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialFileReader::Result SerialFileReader::Start(const QString& path, int64_t sizeLimit)
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
            m_SizeLimit = sizeLimit;
            m_Result = Result::OK;
            m_State = State::Opening;
            m_SerialHandler.SendMessage<SerialProtocol::OpenFile>(
                m_SessionID,
                utf8Path.data(),
                utf8Path.size(),
                SerialProtocol::FilesystemOpenFlags::Read,
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

void SerialFileReader::Cancel()
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

bool SerialFileReader::IsActive() const
{
    return m_State != State::Idle && m_State != State::Finished;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileReader::HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Opening)
    {
        StopTimeout();
        if (packet.m_Status == SerialProtocol::FilesystemError::OK && packet.m_File >= 0)
        {
            m_File = packet.m_File;
            Result openedResult = Result::OK;
            if (m_HasOpenedHandler)
            {
                openedResult = m_OpenedHandler();
            }
            if (openedResult == Result::OK)
            {
                SendNextRead();
            }
            else
            {
                CloseFile((openedResult == Result::OK) ? Result::OpenedHandlerFailed : openedResult);
            }
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

void SerialFileReader::HandleReadFileReply(const SerialProtocol::ReadFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Reading && packet.m_File == m_File)
    {
        StopTimeout();
        Result result = Result::OK;
        bool shouldContinue = true;
        if (packet.m_Size > 0)
        {
            m_Offset = packet.m_StartPos + packet.m_Size;
            DataResult dataResult = DataResult::Continue;
            if (m_HasDataReadyHandler)
            {
                dataResult = m_DataReadyHandler(packet.m_StartPos, packet.m_Buffer, packet.m_Size);
            }
            if (dataResult == DataResult::Stop)
            {
                shouldContinue = false;
            }
            else if (dataResult == DataResult::Error)
            {
                result = Result::DataHandlerFailed;
                shouldContinue = false;
            }
        }
        else if (packet.m_Size < 0)
        {
            result = Result::ReadFailed;
            shouldContinue = false;
        }
        else
        {
            if (m_SizeLimit >= 0 && m_Offset < m_SizeLimit)
            {
                result = Result::UnexpectedEndOfFile;
            }
            shouldContinue = false;
        }

        if (result == Result::OK && shouldContinue && m_SizeLimit >= 0 && m_Offset >= m_SizeLimit)
        {
            shouldContinue = false;
        }
        if (result == Result::OK && shouldContinue && packet.m_Size == int32_t(sizeof(SerialProtocol::ReadFileReply::m_Buffer)))
        {
            SendNextRead();
        }
        else
        {
            CloseFile(result);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileReader::HandleCloseFileReply(const SerialProtocol::CloseFileReply& packet)
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

void SerialFileReader::HandleTimeout()
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

void SerialFileReader::SendNextRead()
{
    if (m_File >= 0)
    {
        int32_t readSize = int32_t(sizeof(SerialProtocol::ReadFileReply::m_Buffer));
        if (m_SizeLimit >= 0)
        {
            const int64_t remainingBytes = m_SizeLimit - m_Offset;
            if (remainingBytes <= 0)
            {
                CloseFile(Result::OK);
                return;
            }
            readSize = int32_t(std::min<int64_t>(readSize, remainingBytes));
        }
        m_State = State::Reading;
        m_SerialHandler.SendMessage<SerialProtocol::ReadFile>(m_SessionID, m_File, m_Offset, readSize);
        StartTimeout();
    }
    else
    {
        Finish(Result::ReadFailed);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialFileReader::CloseFile(Result result)
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

void SerialFileReader::Finish(Result result)
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
