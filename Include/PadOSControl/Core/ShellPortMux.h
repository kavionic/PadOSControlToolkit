// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QObject>
#include <QSerialPort>
#include <QByteArray>

#include "SerialConsole/ShellMuxProtocol.h"

class ShellPortMux : public QObject
{
    Q_OBJECT
public:
    explicit ShellPortMux(QObject* parent = nullptr);

    void Open(const QString& portName);
    void Close();

    void RequestOpenChannel();
    void RequestCloseChannel(uint16_t channelID);
    void SendData(uint16_t channelID, const QByteArray& data);
    void SendWindowSizeChange(uint16_t channelID, uint16_t width, uint16_t height, uint16_t pixelWidth, uint16_t pixelHeight);

Q_SIGNALS:
    void ChannelOpened(uint16_t channelID);
    void ChannelClosed(uint16_t channelID);
    void DataReceived(uint16_t channelID, QByteArray data);
    void PortLost();

private slots:
    void SlotDataReady();
    void SlotPortError(QSerialPort::SerialPortError error);

private:
    void ProcessByte(uint8_t byte);
    void SendFrame(uint16_t channelID, const QByteArray& payload);

    QSerialPort* m_SerialPort = nullptr;

    enum class ParseState { SyncByte0, SyncByte1, Header, Payload };
    ParseState     m_ParseState = ParseState::SyncByte0;
    uint8_t        m_HeaderBytes[4];
    size_t         m_HeaderBytesReceived = 0;
    ShellMuxHeader m_CurrentHeader;
    QByteArray     m_PayloadBuf;
};
