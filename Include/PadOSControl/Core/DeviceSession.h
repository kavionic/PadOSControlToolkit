// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <format>
#include <memory>
#include <utility>

#include <QObject>
#include <QString>
#include <QTimer>
// Windows headers also define ERROR; PadOS uses that name for a log severity.
#ifdef ERROR
#undef ERROR
#endif
#include <Utils/LogSeverity.h>

#include "PadOSControl/Core/DeviceSessionOptions.h"
#include "PadOSControl/Core/SerialHandler.h"

class DeviceFilesystem;
class ShellPortMux;
class SshServer;

enum class MainState : int
{
    Disconnected,
    ConnectedBootloader,
    ConnectedApplication
};

// Owns one device connection. Create panels before Start(), and destroy them
// before the session. Hiding a panel does not stop its operations.
class DeviceSession : public QObject
{
    Q_OBJECT

public:
    explicit DeviceSession(QObject* parent = nullptr);
    explicit DeviceSession(const DeviceSessionOptions& options, QObject* parent = nullptr);
    ~DeviceSession() override;

    void Start();
    void Stop();

    const DeviceSessionOptions& GetOptions() const { return m_Options; }
    SerialHandler& GetSerialHandler() { return m_SerialHandler; }
    MainState GetMainState() const { return m_MainState; }
    void SetExpectedDeviceMode(SerialProtocol::ProbeDeviceType mode);
    void Disconnect();

    QString GetDeviceFilesystemMountRootPath() const;
    const QString& GetMountStatus() const { return m_MountStatus; }

    void AddLogMessage(PLogSeverity severity, const QString& text);

    template<typename... Args>
    void AddLogMessage(PLogSeverity severity, std::format_string<Args...> format, Args&&... args)
    {
        AddLogMessage(severity, QString::fromStdString(std::format(format, std::forward<Args>(args)...)));
    }

Q_SIGNALS:
    void SignalMainStateChanged(MainState state);
    void SignalLogMessage(PLogSeverity severity, const QString& text);
    void SignalMountStatusChanged(const QString& text);

private:
    void SetMainState(MainState state);
    void StartShellBridge();
    void StartDeviceFilesystem();
    void SetMountStatus(const QString& text);
    void SendSystemTime(const SerialProtocol::RequestSystemTime& packet);
    void HandleProbeDeviceReply(const SerialProtocol::ProbeDeviceReply& packet);

    const DeviceSessionOptions m_Options;
    SerialHandler m_SerialHandler;
    QTimer m_ProbeDeviceTimer;
    MainState m_MainState = MainState::Disconnected;
    SerialProtocol::ProbeDeviceType m_ExpectedDeviceMode = SerialProtocol::ProbeDeviceType::Application;
    bool m_Started = false;
    QString m_MountStatus;

    std::unique_ptr<ShellPortMux> m_ShellPortMux;
#ifdef HAVE_LIBSSH
    std::unique_ptr<SshServer> m_SshServer;
#endif
#ifdef HAVE_WINFSP
    std::unique_ptr<DeviceFilesystem> m_DeviceFilesystem;
#endif
};
