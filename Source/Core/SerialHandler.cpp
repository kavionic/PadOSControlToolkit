// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2022 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Core/SerialHandler.h"

#include "PadOSControl/Core/HashCalculator.h"

#include <QDebug>
#include <QThread>


static constexpr uint32_t MESSAGE_TIMEOUT_MS = 10000;

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static const SerialProtocol::PacketHeader* GetPacketHeader(const QByteArray& packet)
{
    if (packet.size() < qsizetype(sizeof(SerialProtocol::PacketHeader)))
    {
        return nullptr;
    }
    return reinterpret_cast<const SerialProtocol::PacketHeader*>(packet.constData());
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static const SerialProtocol::PacketHeader* GetQueuedPacketHeader(const std::vector<QByteArray>& queue)
{
    if (queue.empty())
    {
        return nullptr;
    }
    return GetPacketHeader(queue[0]);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static bool IsFilteredDiagnosticCommand(SerialProtocol::Commands::Value command)
{
    return command == SerialProtocol::Commands::ProbeDevice ||
           command == SerialProtocol::Commands::ProbeDeviceReply ||
           command == SerialProtocol::Commands::LogMessage;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static bool ShouldScanPortInfo(const QSerialPortInfo& portInfo, const QString& forcedPortName)
{
    if (forcedPortName.isEmpty())
    {
        return portInfo.vendorIdentifier() == 0x0483 && !portInfo.isNull();
    }
    return portInfo.portName() == forcedPortName;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static bool ContainsScannablePortName(const QList<QSerialPortInfo>& serialPorts, const QString& portName, const QString& forcedPortName)
{
    for (const QSerialPortInfo& portInfo : serialPorts)
    {
        if (portInfo.portName() == portName && ShouldScanPortInfo(portInfo, forcedPortName))
        {
            return true;
        }
    }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

static void LogSerialWrite(const char* context, const SerialProtocol::PacketHeader* header, qint64 bytesWritten, qint64 packetSize, size_t queueSize, int resentCount)
{
    if (header != nullptr && IsFilteredDiagnosticCommand(header->Command))
    {
        return;
    }
    if (header != nullptr)
    {
        qDebug() << "SerialHandler::write"
                 << context
                 << "command" << uint32_t(header->Command)
                 << "flags" << header->Flags
                 << "length" << header->PackageLength
                 << "bytesWritten" << bytesWritten
                 << "packetSize" << packetSize
                 << "queueSize" << quint64(queueSize)
                 << "resentCount" << resentCount;
    }
    else
    {
        qDebug() << "SerialHandler::write"
                 << context
                 << "command" << "<invalid>"
                 << "bytesWritten" << bytesWritten
                 << "packetSize" << packetSize
                 << "queueSize" << quint64(queueSize)
                 << "resentCount" << resentCount;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialHandler::SerialHandler()
{
    QList<QSerialPortInfo> serialPorts = QSerialPortInfo::availablePorts();

    for (const QSerialPortInfo& port : serialPorts)
    {
        QList<qint32> baudrates = port.standardBaudRates();

        uint16_t vendorID = port.vendorIdentifier();
        QString desc = port.description();
        QString name = port.manufacturer();
    }

    m_PortScanTimer.setSingleShot(true);
    connect(&m_PortScanTimer, &QTimer::timeout, this, &SerialHandler::SlotPortScanTimeout);

    m_SendTimer.setSingleShot(true);
    connect(&m_SendTimer, &QTimer::timeout, this, &SerialHandler::SlotSendTimer);

}


///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SerialHandler::~SerialHandler()
{
    Stop();
    for (QThread* thread : m_PortOpenThreads) {
        thread->wait();
    }
    // Deliver only this object's queued port-open completions while it is still
    // alive. Their scan IDs have been invalidated by Stop().
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

void SerialHandler::Stop()
{
    ++m_PortScanID;
    m_State = State::Idle;
    m_PortScanTimer.stop();
    ResetConnection();
    for (QSerialPort* port : m_ScanningSerialPorts)
    {
        disconnect(port, nullptr, this, nullptr);
        port->close();
        port->deleteLater();
    }
    m_ScanningSerialPorts.clear();
    m_PendingScanningPortNames.clear();
    if (m_SerialPort != nullptr) {
        disconnect(m_SerialPort, nullptr, this, nullptr);
    }
    CloseSerial();
}

void SerialHandler::ScanSerialPorts()
{
    if (m_State != State::Scanning)
    {
        for (QSerialPort* port : m_ScanningSerialPorts)
        {
            port->close();
            port->deleteLater();
        }
        m_ScanningSerialPorts.clear();
        m_PendingScanningPortNames.clear();

        if (m_SerialPort != nullptr)
        {
            CloseSerial();
        }

        ++m_PortScanID;
        ResetPortScanDiagnostics();
        m_State = State::Scanning;
    }

    QThread* mainThread = QThread::currentThread();
    const QList<QSerialPortInfo> serialPorts = QSerialPortInfo::availablePorts();

    std::erase_if(m_ScanningSerialPorts, [this, &serialPorts](QSerialPort* port)
    {
        if (port == nullptr)
        {
            return true;
        }
        if (ContainsScannablePortName(serialPorts, port->portName(), m_ForcedPortName))
        {
            return false;
        }
        qDebug() << "SerialHandler::ScanSerialPorts removed" << "port" << port->portName();
        port->close();
        port->deleteLater();
        return true;
    });

    std::erase_if(m_PendingScanningPortNames, [this, &serialPorts](const QString& portName)
    {
        return !ContainsScannablePortName(serialPorts, portName, m_ForcedPortName);
    });

    size_t candidatePortCount = 0;
    size_t newPortCount = 0;
    for (qsizetype i = 0; i < serialPorts.size(); ++i)
    {
        const QSerialPortInfo& portInfo = serialPorts[i];

        if (ShouldScanPortInfo(portInfo, m_ForcedPortName))
        {
            ++candidatePortCount;
            const QString portName = portInfo.portName();
            if (IsScanningPort(portName) || IsPendingScanningPort(portName))
            {
                continue;
            }

            m_PendingScanningPortNames.push_back(portName);
            ++newPortCount;
            const uint32_t scanID = m_PortScanID;
            QThread* thread = QThread::create([this, portInfo, portName, scanID, mainThread]()
            {
                QSerialPort* port = new QSerialPort(portInfo);
                port->setDataBits(QSerialPort::Data8);
                port->setParity(QSerialPort::NoParity);
                if (port->open(QIODevice::ReadWrite))
                {
                    port->moveToThread(mainThread);
                    QMetaObject::invokeMethod(this, [this, port, scanID]()
                    {
                        SlotPortOpened(port, scanID);
                    }, Qt::QueuedConnection);
                }
                else
                {
                    const QSerialPort::SerialPortError error = port->error();
                    const QString errorString = port->errorString();
                    delete port;
                    QMetaObject::invokeMethod(this, [this, portName, scanID, error, errorString]()
                    {
                        if (scanID == m_PortScanID)
                        {
                            RemovePendingScanningPort(portName);
                            qDebug() << "SerialHandler::ScanSerialPorts open-failed"
                                     << "port" << portName
                                     << "error" << error
                                     << "errorString" << errorString;
                        }
                    }, Qt::QueuedConnection);
                }
            });
            m_PortOpenThreads.push_back(thread);
            connect(thread, &QThread::finished, this, [this, thread]() { std::erase(m_PortOpenThreads, thread); });
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
            thread->start();
        }
    }
    ReportPortScanState(candidatePortCount, m_ScanningSerialPorts.size(), m_PendingScanningPortNames.size(), newPortCount);
    m_PortScanTimer.start(PORT_SCAN_POLL_INTERVAL);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SlotPortOpened(QSerialPort* port, uint32_t scanID)
{
    if (port != nullptr)
    {
        RemovePendingScanningPort(port->portName());
    }
    if (scanID != m_PortScanID || m_State != State::Scanning)
    {
        port->close();
        port->deleteLater();
        return;
    }
    const QList<QSerialPortInfo> serialPorts = QSerialPortInfo::availablePorts();
    if (!ContainsScannablePortName(serialPorts, port->portName(), m_ForcedPortName) || IsScanningPort(port->portName()))
    {
        port->close();
        port->deleteLater();
        return;
    }
    qDebug() << "SerialHandler::SlotPortOpened" << "port" << port->portName() << "scanID" << scanID;
    m_ScanningSerialPorts.push_back(port);
    connect(port, &QSerialPort::readyRead, this, &SerialHandler::SlotSerialDataReady);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SetForcedPort(const QString& portName)
{
    m_ForcedPortName = portName;
    if (m_State != State::Idle) {
        ScanSerialPorts();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

QString SerialHandler::GetConnectedPortName() const
{
    if (m_SerialPort != nullptr) {
        return m_SerialPort->portName();
    }
    return QString();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::CloseSerial()
{
    if (m_SerialPort != nullptr)
    {
        if (m_SerialPort->isOpen()) {
            m_SerialPort->close();
        }
        m_SerialPort->deleteLater();
        m_SerialPort = nullptr;
        m_ConnectedPortName.clear();
        emit SignalTransmitError();
        emit SignalConnectedPortChanged(QString());
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::ResetConnection()
{
    m_SendTimer.stop();
    m_OutMessageQueue.clear();
    m_ResentCount = 0;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::UnregisterPacketHandler(SerialProtocol::Commands::Value commandID, const void* ownerObject)
{
    auto iter = m_CommandHandlerMap.find(commandID);
    if (iter != m_CommandHandlerMap.end())
    {
        std::vector<std::shared_ptr<const PacketHandlerBase>>& handlers = iter->second;

        std::erase_if(handlers, [ownerObject](const std::shared_ptr<const PacketHandlerBase>& handler) { return handler->m_OwnerObject == ownerObject; });
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::UnregisterAllPacketHandlers(const void* ownerObject)
{
    for (const auto& i : m_CommandHandlerMap)
    {
        UnregisterPacketHandler(i.first, ownerObject);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SendSerialPacket(SerialProtocol::PacketHeader* msg)
{
    if (m_SerialPort == nullptr) {
        return;
    }
    HashCalculator crcCalc(HashAlgorithm::CRC32);

    msg->Checksum = 0;
    crcCalc.AddData(msg, msg->PackageLength);
    msg->Checksum = crcCalc.Finalize();

    if ((msg->Flags & (SerialProtocol::PacketHeader::FLAG_NO_REPLY | SerialProtocol::PacketHeader::FLAG_REPLY_MESSAGE)) == 0)
    {

        auto buffer = m_OutMessageQueue.emplace(m_OutMessageQueue.end());
        buffer->append(reinterpret_cast<const char*>(msg), msg->PackageLength);

        if (m_OutMessageQueue.size() == 1)
        {
            const SerialProtocol::PacketHeader* queuedHeader = GetPacketHeader(m_OutMessageQueue[0]);
            const qint64 bytesWritten = m_SerialPort->write(m_OutMessageQueue[0]);
            LogSerialWrite("send-queued", queuedHeader, bytesWritten, m_OutMessageQueue[0].size(), m_OutMessageQueue.size(), m_ResentCount);
            m_SerialPort->flush();
            m_ResentCount = 0;
            m_SendTimer.start(MESSAGE_TIMEOUT_MS);
        }
    }
    else
    {
        const qint64 bytesWritten = m_SerialPort->write(reinterpret_cast<const char*>(msg), msg->PackageLength);
        LogSerialWrite("send-no-reply", msg, bytesWritten, msg->PackageLength, m_OutMessageQueue.size(), m_ResentCount);
        m_SerialPort->flush();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::AcknowledgeReceivedMessage()
{
    if (!m_OutMessageQueue.empty())
    {
        m_OutMessageQueue.erase(m_OutMessageQueue.begin());
        m_ResentCount = 0;
        if (!m_OutMessageQueue.empty() && m_SerialPort != nullptr)
        {
            const SerialProtocol::PacketHeader* queuedHeader = GetPacketHeader(m_OutMessageQueue[0]);
            const qint64 bytesWritten = m_SerialPort->write(m_OutMessageQueue[0]);
            LogSerialWrite("send-after-ack", queuedHeader, bytesWritten, m_OutMessageQueue[0].size(), m_OutMessageQueue.size(), m_ResentCount);
            m_SerialPort->flush();
            m_SendTimer.start(MESSAGE_TIMEOUT_MS);
        }
        else
        {
            m_SendTimer.stop();
        }
    }
    else
    {
        m_SendTimer.stop();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SlotSerialDataReady()
{
    QSerialPort* port = dynamic_cast<QSerialPort*>(sender());
    if (port == nullptr) {
        return;
    }
    if (!port->isOpen())
    {
        qDebug() << "SerialHandler::SlotSerialDataReady skipped"
                 << "port" << port->portName()
                 << "reason" << "serial port not open";
        return;
    }
    m_SerialInputBuffer += port->readAll();

    while (m_SerialInputBuffer.size() >= sizeof(SerialProtocol::PacketHeader))
    {
        while (m_SerialInputBuffer.size() > 0)
        {
            while (m_SerialInputBuffer.size() > 0 && m_SerialInputBuffer.data()[0] != SerialProtocol::PacketHeader::MAGIC1) {
                m_SerialInputBuffer.remove(0, 1);
            }
            if (m_SerialInputBuffer.size() > 1 && m_SerialInputBuffer.data()[1] == SerialProtocol::PacketHeader::MAGIC2) {
                break;
            }
            m_SerialInputBuffer.remove(0, 1);
        }
        if (m_SerialInputBuffer.size() < sizeof(SerialProtocol::PacketHeader)) {
            break;
        }
        SerialProtocol::PacketHeader* header = reinterpret_cast<SerialProtocol::PacketHeader*>(m_SerialInputBuffer.data());

        if (header->PackageLength > SerialProtocol::MAX_MESSAGE_SIZE) {
            m_SerialInputBuffer.remove(0, 1);
            continue;
        }

        if (m_SerialInputBuffer.size() >= header->PackageLength)
        {
            if (header->PackageLength == 0) {
                m_SerialInputBuffer.remove(0, 1);
                continue;
            }
            QByteArray packet;
            packet.swap(m_SerialInputBuffer);
            header = reinterpret_cast<SerialProtocol::PacketHeader*>(packet.data());
            if (packet.size() > header->PackageLength)
            {
                m_SerialInputBuffer.append(packet.data() + header->PackageLength, packet.size() - header->PackageLength);
                packet.resize(header->PackageLength);
                header = reinterpret_cast<SerialProtocol::PacketHeader*>(packet.data());
            }

            if (header->Checksum != 0)
            {
                HashCalculator crcCalc(HashAlgorithm::CRC32);

                uint32_t receivedCRC = header->Checksum;
                header->Checksum = 0;
                crcCalc.AddData(header, header->PackageLength);
                uint32_t calculatedCRC = crcCalc.Finalize();

                if (calculatedCRC != receivedCRC) {
                    if (!IsFilteredDiagnosticCommand(header->Command))
                    {
                        const SerialProtocol::PacketHeader* queuedHeader = GetQueuedPacketHeader(m_OutMessageQueue);
                        qDebug() << "SerialHandler::drop-crc"
                                 << "command" << uint32_t(header->Command)
                                 << "flags" << header->Flags
                                 << "length" << header->PackageLength
                                 << "receivedCRC" << receivedCRC
                                 << "calculatedCRC" << calculatedCRC
                                 << "queueSize" << quint64(m_OutMessageQueue.size())
                                 << "frontCommand" << ((queuedHeader != nullptr) ? uint32_t(queuedHeader->Command) : 0xffffffffu);
                    }
                    continue;
                }
            }
            if (m_State == State::Scanning)
            {
                ++m_PortScanID;
                m_PendingScanningPortNames.clear();

                m_SerialPort = port;

                for (size_t i = 0; i < m_ScanningSerialPorts.size(); ++i)
                {
                    if (m_ScanningSerialPorts[i] != port)
                    {
                        m_ScanningSerialPorts[i]->close();
                        m_ScanningSerialPorts[i]->deleteLater();
                    }
                }
                connect(port, &QSerialPort::readChannelFinished, this, &SerialHandler::SlotSerialPortClosed);
                connect(port, &QSerialPort::errorOccurred, this, &SerialHandler::SlotSerialPortError);
                m_ScanningSerialPorts.clear();
                m_PortScanTimer.stop();
                m_ConnectedPortName = port->portName();
                m_State = State::Connected;
                emit SignalConnectedPortChanged(m_ConnectedPortName);

                if (m_ForcedPortName.isEmpty())
                {
                    const QList<QSerialPortInfo> allPorts = QSerialPortInfo::availablePorts();
                    for (const QSerialPortInfo& portInfo : allPorts)
                    {
                        if (portInfo.vendorIdentifier() == 0x0483 && !portInfo.isNull() && portInfo.portName() != m_ConnectedPortName)
                        {
                            emit SignalShellPortIdentified(portInfo.portName());
                            break;
                        }
                    }
                }
            }
//            m_PortScanTimer.start(PROBE_INTERVAL_CONNECTED);
            ProcessPacket(header);
        }
        else
        {
            break;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::ProcessPacket(const SerialProtocol::PacketHeader* header)
{
    if (!IsFilteredDiagnosticCommand(header->Command))
    {
        const SerialProtocol::PacketHeader* queuedHeader = GetQueuedPacketHeader(m_OutMessageQueue);
        qDebug() << "SerialHandler::ProcessPacket"
                 << "command" << uint32_t(header->Command)
                 << "flags" << header->Flags
                 << "length" << header->PackageLength
                 << "queueSize" << quint64(m_OutMessageQueue.size())
                 << "frontCommand" << ((queuedHeader != nullptr) ? uint32_t(queuedHeader->Command) : 0xffffffffu)
                 << "frontFlags" << ((queuedHeader != nullptr) ? queuedHeader->Flags : 0xffffu);
    }

    if (header->Command == SerialProtocol::Commands::MessageReply)
    {
        AcknowledgeReceivedMessage();
    }
    else
    {
        const bool isReplyMessage = (header->Flags & SerialProtocol::PacketHeader::FLAG_REPLY_MESSAGE) != 0;

        if (isReplyMessage) {
            AcknowledgeReceivedMessage();
        }

        std::vector<std::shared_ptr<const PacketHandlerBase>> handlerSnapshot;
        const auto handlerIter = m_CommandHandlerMap.find(header->Command);
        if (handlerIter != m_CommandHandlerMap.end())
        {
            handlerSnapshot = handlerIter->second;
        }

        bool shouldAutoReply = true;
        if (!handlerSnapshot.empty())
        {
            shouldAutoReply = false;
            for (const std::shared_ptr<const PacketHandlerBase>& handler : handlerSnapshot)
            {
                if (handler->m_AutoReply)
                {
                    shouldAutoReply = true;
                    break;
                }
            }
        }

        const bool isReplyRequired = (header->Flags & SerialProtocol::PacketHeader::FLAG_NO_REPLY) == 0;
        if (!isReplyMessage && isReplyRequired && shouldAutoReply)
        {
            SendMessage<SerialProtocol::MessageReply>();
        }

        m_DidSendReplyMessage = false;
        for (const std::shared_ptr<const PacketHandlerBase>& handler : handlerSnapshot)
        {
            handler->HandleMessage(header);
        }
        if (isReplyRequired && !shouldAutoReply && !m_DidSendReplyMessage)
        {
            SendMessage<SerialProtocol::MessageReply>();
        }
        m_DidSendReplyMessage = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SlotSerialPortClosed()
{
    QSerialPort* port = qobject_cast<QSerialPort*>(sender());
    qDebug() << "SerialHandler::SlotSerialPortClosed"
             << "port" << ((port != nullptr) ? port->portName() : QString());
    ScanSerialPorts();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SlotSerialPortError(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::SerialPortError::NoError)
    {
        QSerialPort* port = qobject_cast<QSerialPort*>(sender());
        qDebug() << "SerialHandler::SlotSerialPortError"
                 << "port" << ((port != nullptr) ? port->portName() : QString())
                 << "error" << error
                 << "errorString" << ((port != nullptr) ? port->errorString() : QString());
        ScanSerialPorts();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SlotPortScanTimeout()
{
    ScanSerialPorts();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SerialHandler::IsScanningPort(const QString& portName) const
{
    return std::ranges::any_of(m_ScanningSerialPorts, [&portName](const QSerialPort* port)
    {
        return port != nullptr && port->portName() == portName;
    });
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SerialHandler::IsPendingScanningPort(const QString& portName) const
{
    return std::ranges::find(m_PendingScanningPortNames, portName) != m_PendingScanningPortNames.end();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::RemovePendingScanningPort(const QString& portName)
{
    std::erase(m_PendingScanningPortNames, portName);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::ResetPortScanDiagnostics()
{
    m_LastReportedScanCandidatePortCount = std::numeric_limits<size_t>::max();
    m_LastReportedScanOpenPortCount = std::numeric_limits<size_t>::max();
    m_LastReportedScanPendingPortCount = std::numeric_limits<size_t>::max();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::ReportPortScanState(size_t candidatePortCount, size_t openedPortCount, size_t pendingPortCount, size_t newPortCount)
{
    if (candidatePortCount != m_LastReportedScanCandidatePortCount ||
        openedPortCount != m_LastReportedScanOpenPortCount ||
        pendingPortCount != m_LastReportedScanPendingPortCount ||
        newPortCount != 0)
    {
        qDebug() << "SerialHandler::ScanSerialPorts"
                 << "scanID" << m_PortScanID
                 << "candidatePorts" << quint64(candidatePortCount)
                 << "openPorts" << quint64(openedPortCount)
                 << "pendingPorts" << quint64(pendingPortCount)
                 << "newPorts" << quint64(newPortCount)
                 << "pollMs" << PORT_SCAN_POLL_INTERVAL;

        m_LastReportedScanCandidatePortCount = candidatePortCount;
        m_LastReportedScanOpenPortCount = openedPortCount;
        m_LastReportedScanPendingPortCount = pendingPortCount;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SerialHandler::SlotSendTimer()
{
    if (m_SerialPort != nullptr && !m_OutMessageQueue.empty())
    {
        m_ResentCount++;
        if (m_ResentCount > 5)
        {
            m_ResentCount = 0;
            m_OutMessageQueue.clear();
            SignalTransmitError();
        }
        else if (!m_OutMessageQueue.empty())
        {
            const SerialProtocol::PacketHeader* queuedHeader = GetPacketHeader(m_OutMessageQueue[0]);
            const qint64 bytesWritten = m_SerialPort->write(m_OutMessageQueue[0]);
            LogSerialWrite("resend", queuedHeader, bytesWritten, m_OutMessageQueue[0].size(), m_OutMessageQueue.size(), m_ResentCount);
            m_SendTimer.start(MESSAGE_TIMEOUT_MS + (MESSAGE_TIMEOUT_MS / 4) * (1 << m_ResentCount));
        }
    }
}
