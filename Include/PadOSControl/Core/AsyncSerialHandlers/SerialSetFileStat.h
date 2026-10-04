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

class SerialSetFileStat : public AsyncSerialBase
{
public:
    enum class Result : int
    {
        OK,
        Busy,
        PathTooLong,
        OpenFailed,
        SetFileStatFailed,
        CloseFailed,
        Canceled,
        Timeout
    };

    using FinishedHandler = std::function<void(Result result)>;

    SerialSetFileStat(SerialHandler& serialHandler, int32_t sessionID);
    ~SerialSetFileStat();

    void SetFinishedHandler(FinishedHandler handler);

    Result Start(
        const QString& path,
        uint32_t mask,
        uint32_t mode,
        int64_t size,
        int64_t accessTimeNanos,
        int64_t modificationTimeNanos,
        int64_t creationTimeNanos
    );
    void Cancel();
    bool IsActive() const;

private:
    enum class State : int
    {
        Idle,
        Opening,
        SettingFileStat,
        Closing,
        Finished
    };

    void HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet);
    void HandleSetFileStatReply(const SerialProtocol::SetFileStatReply& packet);
    void HandleCloseFileReply(const SerialProtocol::CloseFileReply& packet);
    virtual void HandleTimeout() override;
    void SendFileStat();
    void CloseFile(Result result);
    void Finish(Result result);

    SerialHandler&   m_SerialHandler;
    FinishedHandler  m_FinishedHandler;
    int32_t          m_SessionID = -1;
    int32_t          m_File = -1;
    uint32_t         m_Mask = 0;
    uint32_t         m_Mode = 0;
    int64_t          m_Size = 0;
    int64_t          m_AccessTimeNanos = 0;
    int64_t          m_ModificationTimeNanos = 0;
    int64_t          m_CreationTimeNanos = 0;
    State            m_State = State::Idle;
    Result           m_Result = Result::OK;
    bool             m_HasFinishedHandler = false;
};
