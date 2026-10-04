// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <QObject>
#include <QMutex>
#include <QThread>
#include <QMap>
#include <QList>
#include <QSet>
#include <atomic>

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>

class ShellPortMux;

class SshServer : public QObject
{
    Q_OBJECT
public:
    explicit SshServer(ShellPortMux& mux, QObject* parent = nullptr);
    ~SshServer();

    bool Start(uint16_t port, const QString& privateKeyPath);
    void Stop();

public slots:
    void Restart();
    void OnChannelOpened(uint16_t channelID);
    void OnChannelClosed(uint16_t channelID);
    void OnDataReceived(uint16_t channelID, QByteArray data);

private:
    struct Session
    {
        ssh_session       SshSession        = nullptr;
        ssh_channel       SshChannel        = nullptr;
        QThread*          Thread            = nullptr;
        SshServer*        Server            = nullptr;
        uint16_t          ChannelID         = 0;
        std::atomic<bool> ChannelIDAssigned{false};
        std::atomic<bool> CloseRequested{false};
        QMutex            OutMutex;
        QList<QByteArray> OutQueue;
        ssh_server_callbacks_struct  ServerCbs{};
        ssh_channel_callbacks_struct WindowChangeCbs{};
    };

    void CloseBindSocket();
    void RequestStop(bool restartAfterStop);
    void StopSynchronously();
    void FinishStopIfReady();
    void AcceptLoop();
    void HandleSession(ssh_session session);
    bool AuthenticatePassword(const QString& username, const QString& password);
    static int OnWindowChange(ssh_session session, ssh_channel channel,
                              int width, int height, int pxwidth, int pheight,
                              void* userdata);

    ShellPortMux& m_Mux;
    ssh_bind      m_Bind = nullptr;
    QThread*      m_AcceptThread = nullptr;
    std::atomic<bool> m_StopRequested{false};
    bool          m_StopInProgress = false;
    bool          m_RestartAfterStop = false;
    uint16_t      m_Port = 0;
    QString       m_PrivateKeyPath;
    QMutex        m_SessionsMutex;
    QMap<uint16_t, Session*> m_Sessions;
    QList<Session*>          m_PendingSessions;
    QMutex        m_SessionThreadsMutex;
    QSet<QThread*> m_SessionThreads;
};
