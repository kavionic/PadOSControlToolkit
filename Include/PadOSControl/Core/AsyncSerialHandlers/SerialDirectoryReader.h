// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <functional>

#include <QString>

#include "PadOSControl/Core/AsyncSerialHandlers/AsyncSerialBase.h"
#include "SerialConsole/FilesystemMessages.h"

class SerialHandler;

class SerialDirectoryReader : public AsyncSerialBase
{
public:
    enum class Result : int
    {
        OK,
        Busy,
        PathTooLong,
        EntryHandlerFailed,
        Canceled,
        Timeout
    };

    using EntriesReadyHandler = std::function<Result(const SerialProtocol::GetDirectoryReply& packet)>;
    using FinishedHandler = std::function<void(Result result)>;

    SerialDirectoryReader(SerialHandler& serialHandler, int32_t sessionID);
    ~SerialDirectoryReader();

    void SetEntriesReadyHandler(EntriesReadyHandler handler);
    void SetFinishedHandler(FinishedHandler handler);

    Result Start(const QString& path);
    void Cancel();
    bool IsActive() const;
    static QString GetResultText(Result result);

private:
    enum class State : int
    {
        Idle,
        Reading,
        Finished
    };

    void HandleGetDirectoryReply(const SerialProtocol::GetDirectoryReply& packet);
    virtual void HandleTimeout() override;
    void Finish(Result result);

    SerialHandler&       m_SerialHandler;
    EntriesReadyHandler  m_EntriesReadyHandler;
    FinishedHandler      m_FinishedHandler;
    int32_t              m_SessionID = -1;
    State                m_State = State::Idle;
    bool                 m_HasEntriesReadyHandler = false;
    bool                 m_HasFinishedHandler = false;
};
