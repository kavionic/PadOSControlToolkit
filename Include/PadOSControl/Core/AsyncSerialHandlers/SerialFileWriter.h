// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <QByteArray>
#include <QString>

#include "PadOSControl/Core/AsyncSerialHandlers/AsyncSerialBase.h"
#include "SerialConsole/FilesystemMessages.h"

class SerialHandler;

class SerialFileWriter : public AsyncSerialBase
{
public:
    enum class Result : int
    {
        OK,
        Busy,
        PathTooLong,
        CreateFailed,
        DataProviderFailed,
        WriteFailed,
        SetFileStatFailed,
        CloseFailed,
        Canceled,
        Timeout
    };

    using DataNeededHandler = std::function<Result(int64_t offset, int32_t maxSize, QByteArray& data)>;
    using FinishedHandler = std::function<void(Result result)>;

    SerialFileWriter(SerialHandler& serialHandler, int32_t sessionID);
    ~SerialFileWriter();

    void SetDataNeededHandler(DataNeededHandler handler);
    void SetFinishedHandler(FinishedHandler handler);

    Result Start(const QString& path, int64_t size, std::optional<int64_t> modificationTimeNanos = std::nullopt);
    void Cancel();
    bool IsActive() const;
    static QString GetResultText(Result result);

private:
    enum class State : int
    {
        Idle,
        Creating,
        Writing,
        SettingFileStat,
        Closing,
        Finished
    };

    void HandleOpenFileReply(const SerialProtocol::OpenFileReply& packet);
    void HandleWriteFileReply(const SerialProtocol::WriteFileReply& packet);
    void HandleSetFileStatReply(const SerialProtocol::SetFileStatReply& packet);
    void HandleCloseFileReply(const SerialProtocol::CloseFileReply& packet);
    virtual void HandleTimeout() override;
    void SendNextWrite();
    void SendFileStat();
    void CloseFile(Result result);
    void Finish(Result result);

    SerialHandler&               m_SerialHandler;
    DataNeededHandler            m_DataNeededHandler;
    FinishedHandler              m_FinishedHandler;
    std::optional<int64_t>       m_ModificationTimeNanos;
    int32_t                      m_SessionID = -1;
    int32_t                      m_File = -1;
    int64_t                      m_Offset = 0;
    int64_t                      m_Size = 0;
    State                        m_State = State::Idle;
    Result                       m_Result = Result::OK;
    bool                         m_HasDataNeededHandler = false;
    bool                         m_HasFinishedHandler = false;
};
