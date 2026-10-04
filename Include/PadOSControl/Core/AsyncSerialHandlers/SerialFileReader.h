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

class SerialFileReader : public AsyncSerialBase
{
public:
    enum class Result : int
    {
        OK,
        Busy,
        PathTooLong,
        OpenFailed,
        OpenedHandlerFailed,
        ReadFailed,
        UnexpectedEndOfFile,
        DataHandlerFailed,
        CloseFailed,
        Canceled,
        Timeout
    };

    enum class DataResult : int
    {
        Continue,
        Stop,
        Error
    };

    using OpenedHandler = std::function<Result()>;
    using DataReadyHandler = std::function<DataResult(int64_t startPos, const char* data, int32_t size)>;
    using FinishedHandler = std::function<void(Result result)>;

    SerialFileReader(SerialHandler& serialHandler, int32_t sessionID);
    ~SerialFileReader();

    void SetOpenedHandler(OpenedHandler handler);
    void SetDataReadyHandler(DataReadyHandler handler);
    void SetFinishedHandler(FinishedHandler handler);

    Result Start(const QString& path, int64_t sizeLimit = -1);
    void Cancel();
    bool IsActive() const;

private:
    enum class State : int
    {
        Idle,
        Opening,
        Reading,
        Closing,
        Finished
    };

    void HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet);
    void HandleReadFileReply(const SerialProtocol::ReadFileReply& packet);
    void HandleCloseFileReply(const SerialProtocol::CloseFileReply& packet);
    virtual void HandleTimeout() override;
    void SendNextRead();
    void CloseFile(Result result);
    void Finish(Result result);

    SerialHandler&    m_SerialHandler;
    OpenedHandler     m_OpenedHandler;
    DataReadyHandler  m_DataReadyHandler;
    FinishedHandler   m_FinishedHandler;
    int32_t           m_SessionID = -1;
    int32_t           m_File = -1;
    int64_t           m_Offset = 0;
    int64_t           m_SizeLimit = -1;
    State             m_State = State::Idle;
    Result            m_Result = Result::OK;
    bool              m_HasOpenedHandler = false;
    bool              m_HasDataReadyHandler = false;
    bool              m_HasFinishedHandler = false;
};
