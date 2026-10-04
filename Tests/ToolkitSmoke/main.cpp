// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include <PadOSControl/Widgets/ControlWindow.h>
#include <PadOSControl/Widgets/FileBrowser.h>
#include <PadOSControl/Widgets/FirmwareUpdater.h>
#include <PadOSControl/Widgets/LogView/LogView.h>
#include <PadOSControl/Widgets/LogView/LogViewModel.h>
#include <PadOSControl/Widgets/SDCardSync.h>
#include <SerialConsole/BootloaderMessages.h>

#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QSettings>
#include <QStyleFactory>
#include <QTemporaryDir>
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

    bool succeeded = true;
    {
        ControlWindow window;
        const auto docks = window.findChildren<QDockWidget*>();
        succeeded &= Check(docks.size() == 4, "Four standard tool docks");
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

        QDockWidget* customDock = window.AddPanel(
            "Test.Custom",
            "Custom tool",
            new QLabel("Device-specific panel"),
            Qt::LeftDockWidgetArea
        );
        const QByteArray layout = window.saveState(1);
        customDock->hide();
        succeeded &= Check(window.restoreState(layout, 1), "Restore layout with a custom tool");
        succeeded &= Check(window.findChildren<QDockWidget*>().size() == 5, "Hiding a dock keeps its panel alive");

        window.show();
        application.processEvents();
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
