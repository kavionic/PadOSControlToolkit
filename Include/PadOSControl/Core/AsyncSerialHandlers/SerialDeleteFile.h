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

class SerialDeleteFile : public AsyncSerialBase
{
public:
    enum class Result : int
    {
        OK,
        Busy,
        PathTooLong,
        DeleteFailed,
        Canceled,
        Timeout
    };

    using FinishedHandler = std::function<void(Result result)>;

    SerialDeleteFile(SerialHandler& serialHandler, int32_t sessionID);
    ~SerialDeleteFile();

    void SetFinishedHandler(FinishedHandler handler);

    Result Start(const QString& path);
    void Cancel();
    bool IsActive() const;

private:
    enum class State : int
    {
        Idle,
        Deleting,
        Finished
    };

    void HandleDeleteFileReply(const SerialProtocol::DeleteFileReply& packet);
    virtual void HandleTimeout() override;
    void Finish(Result result);

    SerialHandler&   m_SerialHandler;
    FinishedHandler  m_FinishedHandler;
    int32_t          m_SessionID = -1;
    State            m_State = State::Idle;
    bool             m_HasFinishedHandler = false;
};
