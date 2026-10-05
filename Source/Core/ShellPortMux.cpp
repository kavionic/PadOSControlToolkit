// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Core/ShellPortMux.h"

static constexpr uint8_t MAGIC_BYTE0 = static_cast<uint8_t>(ShellMuxHeader::MAGIC & 0xFF);
static constexpr uint8_t MAGIC_BYTE1 = static_cast<uint8_t>(ShellMuxHeader::MAGIC >> 8);

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

ShellPortMux::ShellPortMux(QObject* parent)
    : QObject(parent)
{
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::Open(const QString& portName)
{
    Close();

    m_SerialPort = new QSerialPort(portName, this);
    m_SerialPort->setDataBits(QSerialPort::Data8);
    m_SerialPort->setParity(QSerialPort::NoParity);

    if (m_SerialPort->open(QIODevice::ReadWrite))
    {
        connect(m_SerialPort, &QSerialPort::readyRead, this, &ShellPortMux::SlotDataReady);
        connect(m_SerialPort, &QSerialPort::errorOccurred, this, &ShellPortMux::SlotPortError);
    }
    else
    {
        m_SerialPort->deleteLater();
        m_SerialPort = nullptr;
        emit PortLost();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::Close()
{
    if (m_SerialPort != nullptr)
    {
        disconnect(m_SerialPort, nullptr, this, nullptr);
        m_SerialPort->close();
        m_SerialPort->deleteLater();
        m_SerialPort = nullptr;
    }
    m_ParseState = ParseState::SyncByte0;
    m_PayloadBuf.clear();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::RequestOpenChannel()
{
    ShellMuxControlPayload payload;
    payload.Command   = ShellMuxCommand::OpenChannel;
    payload.ChannelID = 0;
    SendFrame(SHELL_MUX_CONTROL_CHANNEL, QByteArray(reinterpret_cast<const char*>(&payload), sizeof(payload)));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::RequestCloseChannel(uint16_t channelID)
{
    ShellMuxControlPayload payload;
    payload.Command   = ShellMuxCommand::CloseChannel;
    payload.ChannelID = channelID;
    SendFrame(SHELL_MUX_CONTROL_CHANNEL, QByteArray(reinterpret_cast<const char*>(&payload), sizeof(payload)));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::SendData(uint16_t channelID, const QByteArray& data)
{
    SendFrame(channelID, data);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::SendWindowSizeChange(uint16_t channelID, uint16_t width, uint16_t height, uint16_t pixelWidth, uint16_t pixelHeight)
{
    ShellMuxWindowSizePayload payload;
    payload.Command     = ShellMuxCommand::WindowSizeChange;
    payload.ChannelID   = channelID;
    payload.Width       = width;
    payload.Height      = height;
    payload.PixelWidth  = pixelWidth;
    payload.PixelHeight = pixelHeight;
    SendFrame(SHELL_MUX_CONTROL_CHANNEL, QByteArray(reinterpret_cast<const char*>(&payload), sizeof(payload)));
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::SlotDataReady()
{
    const QByteArray data = m_SerialPort->readAll();
    for (const char byte : data) {
        ProcessByte(static_cast<uint8_t>(byte));
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::SlotPortError(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::SerialPortError::NoError)
    {
        Close();
        emit PortLost();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::ProcessByte(uint8_t byte)
{
    switch (m_ParseState)
    {
        case ParseState::SyncByte0:
            if (byte == MAGIC_BYTE0) {
                m_ParseState = ParseState::SyncByte1;
            }
            break;

        case ParseState::SyncByte1:
            if (byte == MAGIC_BYTE1) {
                m_HeaderBytesReceived = 0;
                m_ParseState = ParseState::Header;
            } else if (byte == MAGIC_BYTE0) {
                // Stay — consecutive 0x5A bytes
            } else {
                m_ParseState = ParseState::SyncByte0;
            }
            break;

        case ParseState::Header:
            m_HeaderBytes[m_HeaderBytesReceived++] = byte;
            if (m_HeaderBytesReceived == 4)
            {
                m_CurrentHeader.Magic     = ShellMuxHeader::MAGIC;
                m_CurrentHeader.ChannelID = uint16_t(m_HeaderBytes[0]) | (uint16_t(m_HeaderBytes[1]) << 8);
                m_CurrentHeader.Length    = uint16_t(m_HeaderBytes[2]) | (uint16_t(m_HeaderBytes[3]) << 8);

                if (m_CurrentHeader.Length == 0)
                {
                    // Zero-length frames are valid (control messages can be empty)
                    m_ParseState = ParseState::SyncByte0;
                }
                else if (m_CurrentHeader.Length > SHELL_MUX_MAX_PAYLOAD)
                {
                    m_ParseState = ParseState::SyncByte0;
                }
                else
                {
                    m_PayloadBuf.clear();
                    m_PayloadBuf.reserve(m_CurrentHeader.Length);
                    m_ParseState = ParseState::Payload;
                }
            }
            break;

        case ParseState::Payload:
            m_PayloadBuf.append(static_cast<char>(byte));
            if (static_cast<size_t>(m_PayloadBuf.size()) == m_CurrentHeader.Length)
            {
                const uint16_t channelID = m_CurrentHeader.ChannelID;

                if (channelID == SHELL_MUX_CONTROL_CHANNEL)
                {
                    if (static_cast<size_t>(m_PayloadBuf.size()) >= sizeof(ShellMuxControlPayload))
                    {
                        ShellMuxControlPayload payload;
                        memcpy(&payload, m_PayloadBuf.constData(), sizeof(payload));

                        if (payload.Command == ShellMuxCommand::OpenChannelAck) {
                            emit ChannelOpened(payload.ChannelID);
                        } else if (payload.Command == ShellMuxCommand::CloseChannel) {
                            emit ChannelClosed(payload.ChannelID);
                        }
                    }
                }
                else
                {
                    emit DataReceived(channelID, m_PayloadBuf);
                }
                m_ParseState = ParseState::SyncByte0;
            }
            break;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void ShellPortMux::SendFrame(uint16_t channelID, const QByteArray& payload)
{
    if (m_SerialPort == nullptr || !m_SerialPort->isOpen()) {
        return;
    }
    ShellMuxHeader header;
    header.Magic     = ShellMuxHeader::MAGIC;
    header.ChannelID = channelID;
    header.Length    = uint16_t(payload.size());

    m_SerialPort->write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!payload.isEmpty()) {
        m_SerialPort->write(payload);
    }
}
