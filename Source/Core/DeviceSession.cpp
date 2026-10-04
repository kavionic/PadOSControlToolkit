// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"

#include "PadOSControl/Core/DeviceSession.h"
#include "PadOSControl/Core/ShellPortMux.h"
#ifdef HAVE_LIBSSH
#include "PadOSControl/Integrations/SshServer.h"
#endif
#ifdef HAVE_WINFSP
#include "PadOSControl/Integrations/DeviceFilesystem.h"
#endif

static constexpr int PROBE_DEVICE_RETRY_INTERVAL_MILLISECONDS = 100;

DeviceSession::DeviceSession(QObject* parent)
    : DeviceSession(DeviceSessionOptions{}, parent)
{
}

DeviceSession::DeviceSession(const DeviceSessionOptions& options, QObject* parent)
    : QObject(parent)
    , m_Options(options)
{
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::RequestSystemTime>(this, &DeviceSession::SendSystemTime, false);
    m_SerialHandler.RegisterPacketHandler<SerialProtocol::ProbeDeviceReply>(this, &DeviceSession::HandleProbeDeviceReply);
    connect(&m_SerialHandler, &SerialHandler::SignalTransmitError, this, &DeviceSession::Disconnect);

    m_ProbeDeviceTimer.setSingleShot(true);
    connect(
        &m_ProbeDeviceTimer,
        &QTimer::timeout,
        this,
        [this]() { m_SerialHandler.SendMessage<SerialProtocol::ProbeDevice>(m_ExpectedDeviceMode); }
    );
}

DeviceSession::~DeviceSession()
{
    Stop();
}

void DeviceSession::Start()
{
    if (!m_Started)
    {
        m_Started = true;
        StartShellBridge();
        StartDeviceFilesystem();
        m_SerialHandler.ScanSerialPorts();
    }
}

void DeviceSession::Stop()
{
    if (m_Started)
    {
        m_Started = false;
        m_SerialHandler.Stop();
        SetMainState(MainState::Disconnected);
        m_ProbeDeviceTimer.stop();
#ifdef HAVE_WINFSP
        m_DeviceFilesystem.reset();
#endif
#ifdef HAVE_LIBSSH
        m_SshServer.reset();
#endif
        m_ShellPortMux.reset();
    }
}

void DeviceSession::SetExpectedDeviceMode(SerialProtocol::ProbeDeviceType mode)
{
    if (mode != m_ExpectedDeviceMode)
    {
        m_ProbeDeviceTimer.stop();
        m_ExpectedDeviceMode = mode;
        SetMainState(MainState::Disconnected);
        m_SerialHandler.ResetConnection();
    }
}

void DeviceSession::Disconnect()
{
    SetMainState(MainState::Disconnected);
}

QString DeviceSession::GetDeviceFilesystemMountRootPath() const
{
#ifdef HAVE_WINFSP
    if (m_DeviceFilesystem != nullptr) {
        return m_DeviceFilesystem->GetMountRootPath();
    }
#endif
    return QString();
}

void DeviceSession::AddLogMessage(PLogSeverity severity, const QString& text)
{
    emit SignalLogMessage(severity, text);
}

void DeviceSession::SetMainState(MainState state)
{
    if (state != m_MainState)
    {
        m_MainState = state;
        m_SerialHandler.ResetConnection();
        m_ProbeDeviceTimer.stop();
        if (state != MainState::Disconnected)
        {
            m_SerialHandler.SendMessage<SerialProtocol::SetSystemTime>(QDateTime::currentMSecsSinceEpoch() * 1000LL);
            m_SerialHandler.SendMessage<SerialProtocol::ProbeDevice>(m_ExpectedDeviceMode);
        }
        emit SignalMainStateChanged(state);
    }
}

void DeviceSession::StartShellBridge()
{
    m_ShellPortMux = std::make_unique<ShellPortMux>();
    connect(&m_SerialHandler, &SerialHandler::SignalShellPortIdentified, m_ShellPortMux.get(), &ShellPortMux::Open);
#ifdef HAVE_LIBSSH
    m_SshServer = std::make_unique<SshServer>(*m_ShellPortMux);
    connect(&m_SerialHandler, &SerialHandler::SignalShellPortIdentified, m_SshServer.get(), &SshServer::Restart);
    connect(m_ShellPortMux.get(), &ShellPortMux::ChannelOpened, m_SshServer.get(), &SshServer::OnChannelOpened);
    connect(m_ShellPortMux.get(), &ShellPortMux::ChannelClosed, m_SshServer.get(), &SshServer::OnChannelClosed);
    connect(m_ShellPortMux.get(), &ShellPortMux::DataReceived, m_SshServer.get(), &SshServer::OnDataReceived);
    connect(m_ShellPortMux.get(), &ShellPortMux::PortLost, m_SshServer.get(), &SshServer::Stop);

    const QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(appDataPath);
    m_SshServer->Start(2222, appDataPath + "/ssh_host_key");
#endif
}

void DeviceSession::StartDeviceFilesystem()
{
#ifdef HAVE_WINFSP
    HMODULE winFspModule = nullptr;
    HKEY registryKey;
    const LSTATUS registryStatus = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\WinFsp",
        0,
        KEY_READ | KEY_WOW64_32KEY,
        &registryKey
    );
    if (registryStatus == ERROR_SUCCESS)
    {
        WCHAR installDirectory[MAX_PATH];
        DWORD size = sizeof(installDirectory);
        DWORD type = REG_SZ;
        const LSTATUS valueStatus = RegQueryValueExW(
            registryKey,
            L"InstallDir",
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(installDirectory),
            &size
        );
        if (valueStatus == ERROR_SUCCESS && type == REG_SZ)
        {
            WCHAR dllPath[MAX_PATH];
            wcscpy_s(dllPath, installDirectory);
            wcscat_s(dllPath, L"bin\\winfsp-x64.dll");
            winFspModule = LoadLibraryW(dllPath);
        }
        RegCloseKey(registryKey);
    }
    if (winFspModule != nullptr)
    {
        m_DeviceFilesystem = std::make_unique<DeviceFilesystem>(m_SerialHandler, this);
        connect(
            m_DeviceFilesystem.get(),
            &DeviceFilesystem::SignalMountStatusChanged,
            this,
            &DeviceSession::SetMountStatus
        );
        SetMountStatus("Drive: ready");
    }
    else
    {
        SetMountStatus("Drive: WinFsp not installed");
    }
#else
    SetMountStatus("Drive: mounting disabled");
#endif
}

void DeviceSession::SetMountStatus(const QString& text)
{
    m_MountStatus = text;
    emit SignalMountStatusChanged(text);
}

void DeviceSession::SendSystemTime(const SerialProtocol::RequestSystemTime& packet)
{
    m_SerialHandler.SendReplyMessage<SerialProtocol::SetSystemTime>(QDateTime::currentMSecsSinceEpoch() * 1000LL);
}

void DeviceSession::HandleProbeDeviceReply(const SerialProtocol::ProbeDeviceReply& packet)
{
    if (packet.DeviceType == m_ExpectedDeviceMode)
    {
        m_ProbeDeviceTimer.stop();
        switch (packet.DeviceType)
        {
            case SerialProtocol::ProbeDeviceType::Bootloader:
                SetMainState(MainState::ConnectedBootloader);
                break;
            case SerialProtocol::ProbeDeviceType::Application:
                SetMainState(MainState::ConnectedApplication);
                break;
            case SerialProtocol::ProbeDeviceType::None:
                break;
        }
    }
    else if (!m_ProbeDeviceTimer.isActive())
    {
        m_ProbeDeviceTimer.start(PROBE_DEVICE_RETRY_INTERVAL_MILLISECONDS);
    }
}
