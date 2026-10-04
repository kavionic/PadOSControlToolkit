// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/AsyncSerialHandlers/SerialRenameFile.h"

#include "PadOSControl/Core/SerialHandler.h"

#include <utility>

static constexpr int MAX_SERIAL_FILE_PATH_BYTES = 1024;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialRenameFile::SerialRenameFile(SerialHandler& serialHandler, int32_t sessionID)
    : m_SerialHandler(serialHandler)
    , m_SessionID(sessionID)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::FilesystemStatusReply>(this, &SerialRenameFile::HandleFilesystemStatusReply);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialRenameFile::~SerialRenameFile()
{
    Cancel();
    m_SerialHandler.UnregisterAllPacketHandlers(this);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialRenameFile::SetFinishedHandler(FinishedHandler handler)
{
    m_FinishedHandler = std::move(handler);
    m_HasFinishedHandler = true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialRenameFile::Result SerialRenameFile::Start(const QString& oldPath, const QString& newPath)
{
    Result result = Result::Busy;
    if (m_State == State::Idle)
    {
        const QByteArray oldUtf8Path = oldPath.toUtf8();
        const QByteArray newUtf8Path = newPath.toUtf8();
        result = Result::PathTooLong;
        if (oldUtf8Path.size() < MAX_SERIAL_FILE_PATH_BYTES && newUtf8Path.size() < MAX_SERIAL_FILE_PATH_BYTES)
        {
            m_State = State::Renaming;
            m_SerialHandler.SendMessage<SerialProtocol::RenameFile>(
                m_SessionID,
                oldUtf8Path.data(), oldUtf8Path.size(),
                newUtf8Path.data(), newUtf8Path.size()
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

void SerialRenameFile::Cancel()
{
    m_State = State::Finished;
    StopTimeout();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SerialRenameFile::IsActive() const
{
    return m_State != State::Idle && m_State != State::Finished;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialRenameFile::HandleFilesystemStatusReply(const SerialProtocol::FilesystemStatusReply& packet)
{
    if (packet.m_SessionID == m_SessionID && m_State == State::Renaming)
    {
        StopTimeout();
        Finish((packet.m_Status == SerialProtocol::FilesystemError::OK) ? Result::OK : Result::RenameFailed);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialRenameFile::HandleTimeout()
{
    if (IsActive())
    {
        Finish(Result::Timeout);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialRenameFile::Finish(Result result)
{
    m_State = State::Finished;
    StopTimeout();
    FinishedHandler finishedHandler = m_FinishedHandler;
    if (m_HasFinishedHandler)
    {
        finishedHandler(result);
    }
}
