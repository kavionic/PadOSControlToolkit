// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Widgets/ControlWindow.h"
#include "PadOSControl/Widgets/ConnectionToolbar.h"
#include "PadOSControl/Widgets/FileBrowser.h"
#include "PadOSControl/Widgets/FirmwareUpdater.h"
#include "PadOSControl/Widgets/LogView/LogView.h"
#include "PadOSControl/Widgets/SDCardSync.h"

#include <QCloseEvent>
#include <QDockWidget>
#include <QGridLayout>

static constexpr int CONTROL_WINDOW_LAYOUT_VERSION = 2;

ControlWindow::ControlWindow(QWidget* parent)
    : ControlWindow(DeviceSessionOptions{}, parent)
{
}

ControlWindow::ControlWindow(const DeviceSessionOptions& options, QWidget* parent)
    : QMainWindow(parent)
    , m_DeviceSession(options)
{
    setWindowTitle(QApplication::applicationDisplayName());
    resize(1200, 800);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("E&xit"), this, &QWidget::close);
    m_ViewMenu = menuBar()->addMenu(tr("&View"));
    m_ViewMenu->addAction(tr("Reset layout"), this, &ControlWindow::ResetLayout);
    m_ViewMenu->addSeparator();

    CreateStandardPanels();
    CreateConnectionToolbar();
    QLabel* mountStatus = new QLabel(this);
    statusBar()->addPermanentWidget(mountStatus);
    connect(&m_DeviceSession, &DeviceSession::SignalMountStatusChanged, mountStatus, &QLabel::setText);
}

ControlWindow::~ControlWindow()
{
    m_DeviceSession.Stop();
    delete m_ConnectionToolbar;
    delete takeCentralWidget();
    for (QDockWidget* dock : m_Docks) {
        delete dock;
    }
}

QDockWidget* ControlWindow::AddPanel(const QString& identifier, const QString& title, QWidget* panel, PanelSizingFlags sizing)
{
    QDockWidget* dock = new QDockWidget(title, this);
    dock->setObjectName(identifier);
    dock->setWidget(sizing ? CreatePanelContainer(panel, dock, sizing) : panel);
    // Keep the dock-only layout in one area so its splitters resize neighbouring panels.
    dock->setAllowedAreas(Qt::LeftDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    m_ViewMenu->addAction(dock->toggleViewAction());
    m_Docks.push_back(dock);
    return dock;
}

void ControlWindow::Start()
{
    m_DefaultLayout = saveState(CONTROL_WINDOW_LAYOUT_VERSION);
    QSettings settings;
    restoreGeometry(settings.value("MainWindow/geometry").toByteArray());
    restoreState(settings.value("MainWindow/windowState").toByteArray(), CONTROL_WINDOW_LAYOUT_VERSION);
    m_DeviceSession.Start();
}

void ControlWindow::closeEvent(QCloseEvent* event)
{
    m_FileBrowser->SaveSettings();
    m_SDCardSync->SaveSettings();
    m_LogView->SaveSettings();

    QSettings settings;
    settings.setValue("MainWindow/geometry", saveGeometry());
    settings.setValue("MainWindow/windowState", saveState(CONTROL_WINDOW_LAYOUT_VERSION));
    m_DeviceSession.Stop();
    QMainWindow::closeEvent(event);
}

QWidget* ControlWindow::CreatePanelContainer(QWidget* panel, QDockWidget* dock, PanelSizingFlags sizing)
{
    panel->setSizePolicy(GetPanelSizePolicy(panel, sizing, QSizePolicy::Maximum));

    Qt::Alignment alignment;
    if (sizing.testFlag(PanelSizing::CompactWidth)) {
        alignment |= Qt::AlignLeft;
    }
    if (sizing.testFlag(PanelSizing::CompactHeight)) {
        alignment |= Qt::AlignTop;
    }

    QWidget* container = new QWidget(dock);
    QGridLayout* layout = new QGridLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(panel, 0, 0, alignment);
    container->setSizePolicy(GetPanelSizePolicy(panel, sizing, QSizePolicy::Preferred));

    connect(dock, &QDockWidget::topLevelChanged, container, [container, sizing](bool floating)
    {
        const QSizePolicy::Policy policy = floating ? QSizePolicy::Maximum : QSizePolicy::Preferred;
        container->setSizePolicy(GetPanelSizePolicy(container, sizing, policy));
    });
    return container;
}

QSizePolicy ControlWindow::GetPanelSizePolicy(QWidget* panel, PanelSizingFlags sizing, QSizePolicy::Policy compactPolicy)
{
    QSizePolicy policy = panel->sizePolicy();
    if (sizing.testFlag(PanelSizing::CompactWidth)) {
        policy.setHorizontalPolicy(compactPolicy);
    }
    if (sizing.testFlag(PanelSizing::CompactHeight)) {
        policy.setVerticalPolicy(compactPolicy);
    }
    return policy;
}

void ControlWindow::CreateStandardPanels()
{
    m_LogView = new LogView(this);
    m_FileBrowser = new FileBrowser(this);
    m_FirmwareUpdater = new FirmwareUpdater(this);
    m_SDCardSync = new SDCardSync(this);

    m_LogView->SetDeviceSession(&m_DeviceSession);
    m_FileBrowser->SetDeviceSession(&m_DeviceSession);
    m_FirmwareUpdater->SetDeviceSession(&m_DeviceSession);
    m_SDCardSync->SetDeviceSession(&m_DeviceSession);

    m_FileBrowserDock = AddPanel("PadOS.Files", tr("Files"), m_FileBrowser);
    m_SDCardSyncDock = AddPanel("PadOS.SDCardSync", tr("SD-card synchronization"), m_SDCardSync);
    m_FirmwareUpdaterDock = AddPanel("PadOS.Firmware", tr("Firmware update"), m_FirmwareUpdater, PanelSizing::CompactHeight);

    m_LogViewDock = AddPanel("PadOS.Log", tr("Log"), m_LogView);

    splitDockWidget(m_FileBrowserDock, m_LogViewDock, Qt::Vertical);
    tabifyDockWidget(m_FileBrowserDock, m_SDCardSyncDock);
    tabifyDockWidget(m_FileBrowserDock, m_FirmwareUpdaterDock);

    m_FileBrowserDock->raise();
}

void ControlWindow::CreateConnectionToolbar()
{
    m_ConnectionToolbar = new ConnectionToolbar(m_DeviceSession, this);
    m_ConnectionToolbar->setObjectName("PadOS.Connection");
    addToolBar(Qt::TopToolBarArea, m_ConnectionToolbar);
    m_ViewMenu->addAction(m_ConnectionToolbar->toggleViewAction());
}

void ControlWindow::ResetLayout()
{
    restoreState(m_DefaultLayout, CONTROL_WINDOW_LAYOUT_VERSION);
}
