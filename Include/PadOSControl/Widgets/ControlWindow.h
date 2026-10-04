// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <vector>
#include <QMainWindow>
#include "PadOSControl/Core/DeviceSession.h"

class ConnectionToolbar;
class FileBrowser;
class FirmwareUpdater;
class LogView;
class SDCardSync;
class QDockWidget;
class QMenu;

class ControlWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit ControlWindow(QWidget* parent = nullptr);
    explicit ControlWindow(const DeviceSessionOptions& options, QWidget* parent = nullptr);
    ~ControlWindow() override;

    DeviceSession& GetDeviceSession() { return m_DeviceSession; }
    QDockWidget* AddPanel(const QString& identifier, const QString& title, QWidget* panel, Qt::DockWidgetArea area);
    void Start();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void CreateStandardPanels();
    void CreateConnectionToolbar();
    void ResetLayout();

    DeviceSession m_DeviceSession;
    QMenu* m_ViewMenu = nullptr;
    ConnectionToolbar* m_ConnectionToolbar = nullptr;
    FileBrowser* m_FileBrowser = nullptr;
    FirmwareUpdater* m_FirmwareUpdater = nullptr;
    LogView* m_LogView = nullptr;
    SDCardSync* m_SDCardSync = nullptr;
    std::vector<QDockWidget*> m_Docks;
    QByteArray m_DefaultLayout;
};
