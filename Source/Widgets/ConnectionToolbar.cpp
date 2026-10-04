// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Widgets/ConnectionToolbar.h"
#include "PadOSControl/Core/DeviceSession.h"

#include <QComboBox>
#include <QLabel>
#include <QSerialPortInfo>
#include <QSignalBlocker>

ConnectionToolbar::ConnectionToolbar(DeviceSession& deviceSession, QWidget* parent)
    : QToolBar(tr("Connection"), parent)
    , m_DeviceSession(deviceSession)
{
    addWidget(new QLabel(tr("Serial port: "), this));
    m_PortCombo = new QComboBox(this);
    addWidget(m_PortCombo);
    addAction(tr("Refresh ports"), this, &ConnectionToolbar::RefreshPorts);
    addAction(tr("Reconnect"), this, &ConnectionToolbar::Reconnect);
    m_ConnectionStatus = new QLabel(this);
    addSeparator();
    addWidget(m_ConnectionStatus);

    RefreshPorts();
    connect(m_PortCombo, &QComboBox::activated, this, &ConnectionToolbar::Reconnect);
    connect(&m_DeviceSession, &DeviceSession::SignalMainStateChanged, this, &ConnectionToolbar::UpdateConnectionStatus);
    connect(
        &m_DeviceSession.GetSerialHandler(),
        &SerialHandler::SignalConnectedPortChanged,
        this,
        &ConnectionToolbar::UpdateConnectionStatus
    );
    UpdateConnectionStatus();
}

void ConnectionToolbar::RefreshPorts()
{
    const QString selectedPort = m_PortCombo->currentData().toString();
    const QSignalBlocker blocker(m_PortCombo);
    m_PortCombo->clear();
    m_PortCombo->addItem(tr("Automatic"), QString());
    for (const QSerialPortInfo& port : QSerialPortInfo::availablePorts()) {
        m_PortCombo->addItem(port.portName() + " — " + port.description(), port.portName());
    }
    int selectedIndex = m_PortCombo->findData(selectedPort);
    if (selectedIndex < 0)
    {
        m_PortCombo->addItem(selectedPort, selectedPort);
        selectedIndex = m_PortCombo->count() - 1;
    }
    m_PortCombo->setCurrentIndex(selectedIndex);
}

void ConnectionToolbar::Reconnect()
{
    m_DeviceSession.GetSerialHandler().SetForcedPort(m_PortCombo->currentData().toString());
}

void ConnectionToolbar::UpdateConnectionStatus()
{
    QString stateText;
    switch (m_DeviceSession.GetMainState())
    {
        case MainState::Disconnected:
            stateText = tr("Disconnected");
            break;
        case MainState::ConnectedBootloader:
            stateText = tr("Bootloader");
            break;
        case MainState::ConnectedApplication:
            stateText = tr("Connected");
            break;
    }
    const QString portName = m_DeviceSession.GetSerialHandler().GetConnectedPortName();
    if (!portName.isEmpty()) {
        stateText += " — " + portName;
    }
    m_ConnectionStatus->setText(stateText);
}
