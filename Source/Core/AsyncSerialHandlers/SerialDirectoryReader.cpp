// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/AsyncSerialHandlers/SerialDirectoryReader.h"

#include "PadOSControl/Core/SerialHandler.h"

#include <utility>

static constexpr int MAX_SERIAL_DIRECTORY_PATH_BYTES = 1024;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDirectoryReader::SerialDirectoryReader(SerialHandler& serialHandler, int32_t sessionID)
    : m_SerialHandler(serialHandler)
    , m_SessionID(sessionID)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::GetDirectoryReply>(this, &SerialDirectoryReader::HandleGetDirectoryReply);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDirectoryReader::~SerialDirectoryReader()
{
    Cancel();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDirectoryReader::SetEntriesReadyHandler(EntriesReadyHandler handler)
{
    m_EntriesReadyHandler = std::move(handler);
    m_HasEntriesReadyHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDirectoryReader::SetFinishedHandler(FinishedHandler handler)
{
    m_FinishedHandler = std::move(handler);
    m_HasFinishedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialDirectoryReader::Result SerialDirectoryReader::Start(const QString& path)
{
    Result result = Result::Busy;
    if (m_State == State::Idle)
    {
        const QByteArray utf8Path = path.toUtf8();
        result = Result::PathTooLong;
        if (utf8Path.size() < MAX_SERIAL_DIRECTORY_PATH_BYTES)
        {
            m_State = State::Reading;
            m_SerialHandler.SendMessage<SerialProtocol::GetDirectory>(m_SessionID, utf8Path.data(), utf8Path.size());
            StartTimeout();
            result = Result::OK;
        }
    }
    return result;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDirectoryReader::Cancel()
{
    m_State = State::Finished;
    StopTimeout();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SerialDirectoryReader::IsActive() const
{
    return m_State != State::Idle && m_State != State::Finished;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SerialDirectoryReader::GetResultText(Result result)
{
    switch (result)
    {
        case Result::OK:
            return "ok";
        case Result::Busy:
            return "reader is already busy";
        case Result::PathTooLong:
            return "device path is too long";
        case Result::EntryHandlerFailed:
            return "failed to process directory entries";
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

void SerialDirectoryReader::HandleGetDirectoryReply(const SerialProtocol::GetDirectoryReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Reading)
    {
        StopTimeout();
        if (packet.m_EntryCount > 0)
        {
            Result result = Result::OK;
            if (m_HasEntriesReadyHandler)
            {
                result = m_EntriesReadyHandler(packet);
            }
            if (result != Result::OK)
            {
                Finish(result);
            }
            else
            {
                StartTimeout();
            }
        }
        else
        {
            Finish(Result::OK);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDirectoryReader::HandleTimeout()
{
    if (IsActive())
    {
        Finish(Result::Timeout);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialDirectoryReader::Finish(Result result)
{
    m_State = State::Finished;
    StopTimeout();
    FinishedHandler finishedHandler = m_FinishedHandler;
    if (m_HasFinishedHandler)
    {
        finishedHandler(result);
    }
}
