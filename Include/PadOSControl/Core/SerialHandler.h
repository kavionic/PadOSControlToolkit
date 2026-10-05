// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2022 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <utility>
#include <vector>
#include <QTimer>
#include <QThread>

#include <algorithm>
#include <limits>
#include <memory>

#include <QSerialPort>
#include <QSerialPortInfo>

#include <SerialConsole/SerialProtocol.h>
#include "SerialConsole/FilesystemMessages.h"

static constexpr uint32_t PORT_SCAN_POLL_INTERVAL = SerialProtocol::PING_PERIOD_MS_PC;
static constexpr uint32_t PROBE_INTERVAL_CONNECTED = 5000;

class PacketHandlerBase
{
public:
    PacketHandlerBase(const void* ownerObject, bool autoReply) : m_OwnerObject(ownerObject), m_AutoReply(autoReply) {}
    virtual void HandleMessage(const SerialProtocol::PacketHeader* packet) const = 0;
    const void* m_OwnerObject = nullptr;
    bool        m_AutoReply = false;
};

template<typename PacketType, typename CallbackType>
class PacketHandler : public PacketHandlerBase
{
public:
    PacketHandler(const void* ownerObject, CallbackType&& callback, bool autoReply)
        : PacketHandlerBase(ownerObject, autoReply)
        , m_Callback(std::move(callback)
    ) {}

    virtual void HandleMessage(const SerialProtocol::PacketHeader* packet) const override { m_Callback(*static_cast<const PacketType*>(packet)); }

private:
    CallbackType m_Callback;
};

class SerialHandler : public QObject
{
    Q_OBJECT
public:
    SerialHandler();
    ~SerialHandler() override;
    void Stop();

    void ScanSerialPorts();
    void CloseSerial();
    void ResetConnection();

    // Selecting a port while stopped does not start discovery.
    void SetForcedPort(const QString& portName);
    QString GetConnectedPortName() const;

    template<typename MSG_TYPE, typename... ARGS>
    void SendMessage(ARGS&&... args)
    {
            std::unique_ptr<MSG_TYPE> msg = std::make_unique_for_overwrite<MSG_TYPE>();
            memset(msg.get(), 0xcd, sizeof(MSG_TYPE));
            MSG_TYPE::InitMsg(*msg, std::forward<ARGS>(args)...);
            SendSerialPacket(msg.get());
    }

    template<typename MSG_TYPE, typename... ARGS>
    void SendReplyMessage(ARGS&&... args)
    {
            std::unique_ptr<MSG_TYPE> msg = std::make_unique_for_overwrite<MSG_TYPE>();
            memset(msg.get(), 0xcd, sizeof(MSG_TYPE));
            MSG_TYPE::InitMsg(*msg, std::forward<ARGS>(args)...);
            if (!m_DidSendReplyMessage)
            {
                msg->Flags |= SerialProtocol::PacketHeader::FLAG_REPLY_MESSAGE | SerialProtocol::PacketHeader::FLAG_NO_REPLY;
                m_DidSendReplyMessage = true;
            }
            SendSerialPacket(msg.get());
    }

    template<typename PacketType, typename CallbackType>
    void RegisterPacketHandler(SerialProtocol::Commands::Value commandID, CallbackType&& callback, bool autoReply, const void* ownerObject)
    {
        std::shared_ptr<PacketHandler<PacketType, CallbackType>> handler = std::make_shared<PacketHandler<PacketType, CallbackType>>(ownerObject, std::move(callback), autoReply);
        m_CommandHandlerMap[commandID].push_back(handler);

        m_LargestCommandPacket = std::max(m_LargestCommandPacket, sizeof(PacketType));
    }
    template<typename PacketType, typename CallbackType>
    void RegisterPacketHandler(CallbackType&& callback, bool autoReply, const void* ownerObject)
    {
        RegisterPacketHandler<PacketType>(PacketType::COMMAND, std::move(callback), autoReply, ownerObject);
    }

    template<typename PacketType, typename ObjectType, typename CallbackType>
    void RegisterPacketHandler(ObjectType* object, CallbackType callback, bool autoReply = true)
    {
        RegisterPacketHandler<PacketType>(std::bind(callback, object, std::placeholders::_1), autoReply, object);
    }

    template<typename PacketType, typename ObjectType, typename CallbackType>
    void RegisterPacketHandler(SerialProtocol::Commands::Value commandID, ObjectType* object, CallbackType callback, bool autoReply = true)
    {
        RegisterPacketHandler<PacketType>(commandID, std::bind(callback, object, std::placeholders::_1), autoReply, object);
    }

    void UnregisterPacketHandler(SerialProtocol::Commands::Value commandID, const void* ownerObject);
    void UnregisterAllPacketHandlers(const void* ownerObject);

Q_SIGNALS:
    void SignalTransmitError();
    void SignalConnectedPortChanged(const QString& portName);
    void SignalShellPortIdentified(const QString& portName);

private slots:
    void SlotPortOpened(QSerialPort* port, uint32_t scanID);
    void SlotSerialDataReady();
    void SlotSerialPortClosed();
    void SlotSerialPortError(QSerialPort::SerialPortError error);
    void SlotPortScanTimeout();
    void SlotSendTimer();

private:
    void AcknowledgeReceivedMessage();
    void ProcessPacket(const SerialProtocol::PacketHeader* header);
    void SendSerialPacket(SerialProtocol::PacketHeader* msg);
    bool IsScanningPort(const QString& portName) const;
    bool IsPendingScanningPort(const QString& portName) const;
    void RemovePendingScanningPort(const QString& portName);
    void ResetPortScanDiagnostics();
    void ReportPortScanState(size_t candidatePortCount, size_t openedPortCount, size_t pendingPortCount, size_t newPortCount);

    enum class State : int
    {
        Idle,
        Scanning,
        Connected
    };

    std::vector<QThread*> m_PortOpenThreads;

    QString                     m_ConnectedPortName;

    uint32_t                    m_PortScanID = 0;
    QString                     m_ForcedPortName;
    std::vector<QSerialPort*>   m_ScanningSerialPorts;
    std::vector<QString>        m_PendingScanningPortNames;
    size_t                      m_LastReportedScanCandidatePortCount = std::numeric_limits<size_t>::max();
    size_t                      m_LastReportedScanOpenPortCount = std::numeric_limits<size_t>::max();
    size_t                      m_LastReportedScanPendingPortCount = std::numeric_limits<size_t>::max();
    QSerialPort*                m_SerialPort = nullptr;
    QByteArray                  m_SerialInputBuffer;

    State m_State = State::Idle;

    std::map<SerialProtocol::Commands::Value, std::vector<std::shared_ptr<const PacketHandlerBase>>> m_CommandHandlerMap;
    size_t                                                       m_LargestCommandPacket = 0;

    QTimer	m_PortScanTimer;

    QTimer	m_SendTimer;
    int	    m_ResentCount = 0;
    std::vector<QByteArray>  m_OutMessageQueue;
    bool    m_DidSendReplyMessage = false;
};
