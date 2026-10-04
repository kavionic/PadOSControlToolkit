// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/AsyncSerialHandlers/SerialDeleteFile.h"

#include "PadOSControl/Core/SerialHandler.h"

#include <utility>

static constexpr int MAX_SERIAL_FILE_PATH_BYTES = 1024;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDeleteFile::SerialDeleteFile(SerialHandler& serialHandler, int32_t sessionID)
    : m_SerialHandler(serialHandler)
    , m_SessionID(sessionID)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::DeleteFileReply>(this, &SerialDeleteFile::HandleDeleteFileReply);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDeleteFile::~SerialDeleteFile()
{
    Cancel();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDeleteFile::SetFinishedHandler(FinishedHandler handler)
{
    m_FinishedHandler = std::move(handler);
    m_HasFinishedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDeleteFile::Result SerialDeleteFile::Start(const QString& path)
{
    Result result = Result::Busy;
    if (m_State == State::Idle)
    {
        const QByteArray utf8Path = path.toUtf8();
        result = Result::PathTooLong;
        if (utf8Path.size() < MAX_SERIAL_FILE_PATH_BYTES)
        {
            m_State = State::Deleting;
            m_SerialHandler.SendMessage<SerialProtocol::DeleteFile>(m_SessionID, utf8Path.data(), utf8Path.size());
            StartTimeout();
            result = Result::OK;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDeleteFile::Cancel()
{
    m_State = State::Finished;
    StopTimeout();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SerialDeleteFile::IsActive() const
{
    return m_State != State::Idle && m_State != State::Finished;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDeleteFile::HandleDeleteFileReply(const SerialProtocol::DeleteFileReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Deleting)
    {
        StopTimeout();
        Finish((packet.m_Status == SerialProtocol::FilesystemError::OK) ? Result::OK : Result::DeleteFailed);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDeleteFile::HandleTimeout()
{
    if (IsActive())
    {
        Finish(Result::Timeout);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDeleteFile::Finish(Result result)
{
    m_State = State::Finished;
    StopTimeout();
    FinishedHandler finishedHandler = m_FinishedHandler;
    if (m_HasFinishedHandler)
    {
        finishedHandler(result);
    }
}
