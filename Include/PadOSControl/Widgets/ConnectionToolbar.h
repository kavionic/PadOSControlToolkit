// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QToolBar>

class DeviceSession;
class QComboBox;
class QLabel;

// The session must outlive the toolbar.
class ConnectionToolbar : public QToolBar
{
    Q_OBJECT

public:
    explicit ConnectionToolbar(DeviceSession& deviceSession, QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void RefreshPorts();
    void SelectPort(const QString& portName);
    void Reconnect();
    void UpdateConnectionStatus();

    DeviceSession& m_DeviceSession;
    QComboBox* m_PortCombo = nullptr;
    QLabel* m_ConnectionStatus = nullptr;
};
