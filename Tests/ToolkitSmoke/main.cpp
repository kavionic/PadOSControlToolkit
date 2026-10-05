// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include <PadOSControl/Widgets/ConnectionToolbar.h>
#include <PadOSControl/Widgets/ControlWindow.h>
#include <PadOSControl/Widgets/FileBrowser.h>
#include <PadOSControl/Widgets/FirmwareUpdater.h>
#include <PadOSControl/Widgets/LogView/LogView.h>
#include <PadOSControl/Widgets/LogView/LogViewModel.h>
#include <PadOSControl/Widgets/SDCardSync.h>
#include <SerialConsole/BootloaderMessages.h>

#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QTabBar>
#include <cstdio>

// Guard the existing wire contract while its ownership moves into PadOS.
static_assert(sizeof(SerialProtocol::PacketHeader) == 16);
static_assert(SerialProtocol::Commands::BeginFirmwareUpdate == 10010);
static_assert(SerialProtocol::Commands::BeginFirmwareUpdateReply == 10020);
static_assert(SerialProtocol::Commands::EndFirmwareUpdate == 10030);
static_assert(SerialProtocol::Commands::EraseFlashSection == 10040);
static_assert(SerialProtocol::Commands::EraseFlashSectionProgress == 10050);
static_assert(SerialProtocol::Commands::WriteFlashSection == 10060);
static_assert(SerialProtocol::Commands::WriteFlashSectionReply == 10070);
static_assert(SerialProtocol::Commands::GetFlashChecksum == 10080);
static_assert(SerialProtocol::Commands::GetFlashChecksumReply == 10090);
static_assert(SerialProtocol::Commands::InitiateFirmwareUpdate == 11009);
static_assert(sizeof(SerialProtocol::InitiateFirmwareUpdate) == 16);
static_assert(sizeof(SerialProtocol::BeginFirmwareUpdate) == 16);
static_assert(sizeof(SerialProtocol::BeginFirmwareUpdateReply) == 16);
static_assert(sizeof(SerialProtocol::EndFirmwareUpdate) == 16);
static_assert(sizeof(SerialProtocol::EraseFlashSection) == 24);
static_assert(sizeof(SerialProtocol::EraseFlashSectionProgress) == 20);
static_assert(sizeof(SerialProtocol::WriteFlashSection) == 16408);
static_assert(sizeof(SerialProtocol::WriteFlashSectionReply) == 24);
static_assert(sizeof(SerialProtocol::GetFlashChecksum) == 24);
static_assert(sizeof(SerialProtocol::GetFlashChecksumReply) == 20);

static bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

static void SendMouseEvent(
    QMainWindow& window,
    QEvent::Type type,
    const QPoint& position,
    Qt::MouseButton button,
    Qt::MouseButtons buttons)
{
    QMouseEvent event(type, position, window.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(&window, &event);
    QCoreApplication::processEvents();
}

static void DragVerticalSeparator(QMainWindow& window, QDockWidget& lowerDock, int distance)
{
    const int separatorExtent = window.style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent);
    const QPoint origin(window.width() / 2, lowerDock.y() - qMax(1, separatorExtent / 2));
    const QPoint destination = origin + QPoint(0, distance);
    SendMouseEvent(window, QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    SendMouseEvent(window, QEvent::MouseMove, destination, Qt::NoButton, Qt::LeftButton);
    SendMouseEvent(window, QEvent::MouseButtonRelease, destination, Qt::LeftButton, Qt::NoButton);
}

int main(int argc, char* argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setApplicationName("ToolkitSmoke");
    application.setApplicationDisplayName("PadOS Control");
    application.setOrganizationName("PadOSToolkitTests");
    application.setStyle(QStyleFactory::create("Fusion"));
    QTemporaryDir settingsDirectory;
    if (!Check(settingsDirectory.isValid(), "Temporary settings directory")) {
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());

    QSettings().setValue("SerialPort/selectedPort", "ToolkitSmokeMissingPort");

    bool succeeded = true;
    {
        ControlWindow window;
        const auto docks = window.findChildren<QDockWidget*>();
        succeeded &= Check(docks.size() == 4, "Four standard tool docks");
        ConnectionToolbar* toolbar = window.findChild<ConnectionToolbar*>();
        succeeded &= Check(toolbar != nullptr, "Connection toolbar");
        if (toolbar != nullptr)
        {
            succeeded &= Check(window.toolBarArea(toolbar) == Qt::TopToolBarArea, "Connection toolbar at top");
            QComboBox* portCombo = toolbar->findChild<QComboBox*>();
            succeeded &= Check(portCombo != nullptr, "Serial port selector");
            if (portCombo != nullptr)
            {
                succeeded &= Check(portCombo->currentData().toString() == "ToolkitSmokeMissingPort", "Restore saved port");
                portCombo->setCurrentIndex(0);
                emit portCombo->activated(0);
                succeeded &= Check(QSettings().value("SerialPort/selectedPort").toString().isEmpty(), "Save automatic selection");
            }
        }
        LogView* logView = window.findChild<LogView*>();
        succeeded &= Check(logView != nullptr, "Log panel");
        if (logView != nullptr)
        {
            LogViewModel* model = logView->findChild<LogViewModel*>();
            succeeded &= Check(model != nullptr, "Log model");
            window.GetDeviceSession().AddLogMessage(PLogSeverity::INFO_LOW_VOL, QString("Toolkit smoke test"));
            if (model != nullptr) {
                succeeded &= Check(model->rowCount() == 1, "Session log reaches the panel");
            }
        }

        QDockWidget* customDock = window.AddPanel("Test.Custom", "Custom tool", new QLabel("Device-specific panel"));
        QDockWidget* logDock = window.GetLogViewDock();
        window.splitDockWidget(customDock, logDock, Qt::Vertical);
        window.tabifyDockWidget(customDock, window.GetFileBrowserDock());
        window.tabifyDockWidget(customDock, window.GetFirmwareUpdaterDock());
        window.tabifyDockWidget(customDock, window.GetSDCardSyncDock());
        customDock->raise();
        const QByteArray layout = window.saveState(1);
        customDock->hide();
        succeeded &= Check(window.restoreState(layout, 1), "Restore layout with a custom tool");
        succeeded &= Check(window.findChildren<QDockWidget*>().size() == 5, "Hiding a dock keeps its panel alive");

        window.resize(window.sizeHint().expandedTo(QSize(1200, 1000)));
        window.show();
        application.processEvents();
        bool foundOrderedTabs = false;
        for (QTabBar* tabBar : window.findChildren<QTabBar*>())
        {
            QStringList titles;
            for (int index = 0; index < tabBar->count(); ++index) {
                titles.push_back(tabBar->tabText(index));
            }
            if (titles == QStringList{"Custom tool", "Files", "Firmware update", "SD-card synchronization"})
            {
                foundOrderedTabs = true;
                tabBar->setCurrentIndex(0);
            }
        }
        application.processEvents();
        succeeded &= Check(foundOrderedTabs, "Reorder standard docks alongside a custom panel");
        succeeded &= Check(customDock->geometry().bottom() < logDock->y(), "Log below the tabbed panels");

        int logHeight = logDock->height();
        int panelHeight = customDock->height();
        DragVerticalSeparator(window, *logDock, 40);
        succeeded &= Check(logDock->height() < logHeight, "Dragging the divider shrinks the log");
        succeeded &= Check(customDock->height() > panelHeight, "Dragging the divider grows the neighbouring panel");

        logHeight = logDock->height();
        logDock->setFloating(true);
        application.processEvents();
        logDock->setFloating(false);
        application.processEvents();
        succeeded &= Check(logDock->height() == logHeight, "Re-docking the log preserves its height");
        DragVerticalSeparator(window, *logDock, -40);
        succeeded &= Check(logDock->height() > logHeight, "Log divider remains draggable after re-docking");

        const QByteArray resizedLayout = window.saveState(1);
        logDock->hide();
        application.processEvents();
        succeeded &= Check(window.restoreState(resizedLayout, 1), "Restore the resized layout");
        application.processEvents();
        logHeight = logDock->height();
        DragVerticalSeparator(window, *logDock, 40);
        succeeded &= Check(logDock->height() < logHeight, "Log divider remains draggable after restoring the layout");
        if (argc > 1) {
            succeeded &= Check(window.grab().save(QString::fromLocal8Bit(argv[1])), "Render standard window");
        }
        window.close();
        application.processEvents();
        succeeded &= Check(QSettings().contains("MainWindow/windowState"), "Save layout on close");
    }

    // A consumer can also use the widgets without the standard window.
    {
        DeviceSession session;
        FileBrowser files(nullptr);
        FirmwareUpdater updater;
        LogView log;
        SDCardSync synchronization;
        files.SetDeviceSession(&session);
        updater.SetDeviceSession(&session);
        log.SetDeviceSession(&session);
        synchronization.SetDeviceSession(&session);
        session.Stop();
    }
    application.processEvents();
    std::fprintf(stdout, "%s\n", succeeded ? "Toolkit smoke test passed." : "Toolkit smoke test failed.");
    return succeeded ? 0 : 1;
}
