// This file is part of PadOSControlToolkit.
//
// Copyright (c) 2026 Kurt Skauen
//
// SPDX-License-Identifier: Apache-2.0
///////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "PadOSControl/Integrations/SshServer.h"
#include "PadOSControl/Core/ShellPortMux.h"

#include <QFile>
#include <QMetaObject>

// Hardcoded users: username → password
static bool CheckCredentials(const QString& username, const QString& password)
{
    if (username == "root"    && password == "root")    { return true; }
    if (username == "rainbow" && password == "rainbow") { return true; }
    return false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SshServer::SshServer(ShellPortMux& mux, QObject* parent)
    : QObject(parent)
    , m_Mux(mux)
{
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

SshServer::~SshServer()
{
    StopSynchronously();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

bool SshServer::Start(uint16_t port, const QString& privateKeyPath)
{
    m_Port           = port;
    m_PrivateKeyPath = privateKeyPath;

    if (m_StopInProgress)
    {
        m_RestartAfterStop = true;
        return true;
    }
    if (m_Bind != nullptr) {
        return true;
    }
    m_StopRequested.store(false);
    m_Bind = ssh_bind_new();
    if (m_Bind == nullptr) {
        return false;
    }

    const QByteArray keyPathBytes = privateKeyPath.toUtf8();
    ssh_bind_options_set(m_Bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(m_Bind, SSH_BIND_OPTIONS_RSAKEY, keyPathBytes.constData());

    // Generate host key if it does not exist yet
    if (!QFile::exists(privateKeyPath))
    {
        ssh_key hostKey = nullptr;
        if (ssh_pki_generate(SSH_KEYTYPE_RSA, 2048, &hostKey) == SSH_OK)
        {
            if (ssh_pki_export_privkey_file(hostKey, nullptr, nullptr, nullptr, keyPathBytes.constData()) != SSH_OK) {
                qWarning() << "SshServer: failed to write host key to" << privateKeyPath;
            }
            ssh_key_free(hostKey);
        }
        else
        {
            qWarning() << "SshServer: failed to generate host key";
        }
    }

    if (ssh_bind_listen(m_Bind) != SSH_OK)
    {
        qWarning() << "SshServer: ssh_bind_listen failed:" << ssh_get_error(m_Bind);
        ssh_bind_free(m_Bind);
        m_Bind = nullptr;
        return false;
    }

    m_AcceptThread = QThread::create([this]() { AcceptLoop(); });
    QThread* acceptThread = m_AcceptThread;
    connect(acceptThread, &QThread::finished, this, [this, acceptThread]()
    {
        if (m_AcceptThread == acceptThread) {
            m_AcceptThread = nullptr;
        }
        acceptThread->deleteLater();
        FinishStopIfReady();
    });
    m_AcceptThread->start();
    return true;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::Restart()
{
    RequestStop(true);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::Stop()
{
    RequestStop(false);
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::RequestStop(bool restartAfterStop)
{
    m_StopRequested.store(true);
    m_StopInProgress = true;
    m_RestartAfterStop = m_RestartAfterStop || restartAfterStop;

    CloseBindSocket();

    QMutexLocker lock(&m_SessionsMutex);
    for (Session* session : m_Sessions)
    {
        if (session != nullptr) {
            session->CloseRequested.store(true);
        }
    }
    for (Session* session : m_PendingSessions)
    {
        if (session != nullptr) {
            session->CloseRequested.store(true);
        }
    }
    lock.unlock();

    FinishStopIfReady();
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::CloseBindSocket()
{
    if (m_Bind != nullptr)
    {
        const socket_t bindSocket = ssh_bind_get_fd(m_Bind);
        if (bindSocket != SSH_INVALID_SOCKET)
        {
            shutdown(bindSocket, SD_BOTH);
            closesocket(bindSocket);
            ssh_bind_set_fd(m_Bind, SSH_INVALID_SOCKET);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::StopSynchronously()
{
    m_StopRequested.store(true);
    m_RestartAfterStop = false;

    CloseBindSocket();

    QMutexLocker lock(&m_SessionsMutex);
    for (Session* session : m_Sessions)
    {
        if (session != nullptr) {
            session->CloseRequested.store(true);
        }
    }
    for (Session* session : m_PendingSessions)
    {
        if (session != nullptr) {
            session->CloseRequested.store(true);
        }
    }
    lock.unlock();

    QThread* acceptThread = m_AcceptThread;
    m_AcceptThread = nullptr;
    if (acceptThread != nullptr && acceptThread != QThread::currentThread())
    {
        disconnect(acceptThread, nullptr, this, nullptr);
        acceptThread->wait();
        delete acceptThread;
    }

    while (true)
    {
        QThread* sessionThread = nullptr;
        {
            QMutexLocker threadLock(&m_SessionThreadsMutex);
            if (m_SessionThreads.isEmpty()) {
                break;
            }
            sessionThread = *m_SessionThreads.begin();
            m_SessionThreads.remove(sessionThread);
        }

        if (sessionThread != nullptr && sessionThread != QThread::currentThread())
        {
            disconnect(sessionThread, nullptr, this, nullptr);
            sessionThread->wait();
            delete sessionThread;
        }
    }

    if (m_Bind != nullptr)
    {
        ssh_bind_free(m_Bind);
        m_Bind = nullptr;
    }

    m_StopInProgress = false;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::FinishStopIfReady()
{
    if (!m_StopInProgress) {
        return;
    }
    if (m_AcceptThread != nullptr) {
        return;
    }
    {
        QMutexLocker threadLock(&m_SessionThreadsMutex);
        if (!m_SessionThreads.isEmpty()) {
            return;
        }
    }

    if (m_Bind != nullptr)
    {
        ssh_bind_free(m_Bind);
        m_Bind = nullptr;
    }

    const bool restartAfterStop = m_RestartAfterStop;
    m_StopInProgress = false;
    m_RestartAfterStop = false;

    if (restartAfterStop && !m_PrivateKeyPath.isEmpty())
    {
        Start(m_Port, m_PrivateKeyPath);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::AcceptLoop()
{
    while (!m_StopRequested.load() && m_Bind != nullptr)
    {
        ssh_session session = ssh_new();
        if (session == nullptr) {
            break;
        }
        if (ssh_bind_accept(m_Bind, session) != SSH_OK)
        {
            ssh_free(session);
            break;
        }
        if (m_StopRequested.load())
        {
            ssh_free(session);
            break;
        }
        QThread* sessionThread = QThread::create([this, session]() { HandleSession(session); });
        {
            QMutexLocker threadLock(&m_SessionThreadsMutex);
            m_SessionThreads.insert(sessionThread);
        }
        connect(sessionThread, &QThread::finished, this, [this, sessionThread]()
        {
            QMutexLocker threadLock(&m_SessionThreadsMutex);
            m_SessionThreads.remove(sessionThread);
            threadLock.unlock();
            sessionThread->deleteLater();
            FinishStopIfReady();
        });
        sessionThread->start();
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::HandleSession(ssh_session session)
{
    if (ssh_handle_key_exchange(session) != SSH_OK)
    {
        ssh_free(session);
        return;
    }

    ssh_message message = nullptr;
    bool authenticated = false;

    while (!authenticated)
    {
        message = ssh_message_get(session);
        if (message == nullptr) {
            break;
        }
        if (ssh_message_type(message) == SSH_REQUEST_AUTH && ssh_message_subtype(message) == SSH_AUTH_METHOD_PASSWORD)
        {
            const QString username = QString::fromUtf8(ssh_message_auth_user(message));
            const QString password = QString::fromUtf8(ssh_message_auth_password(message));
            if (CheckCredentials(username, password))
            {
                ssh_message_auth_reply_success(message, 0);
                authenticated = true;
            }
            else
            {
                ssh_message_reply_default(message);
            }
        }
        else
        {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }

    if (!authenticated)
    {
        ssh_free(session);
        return;
    }

    ssh_channel channel = nullptr;

    while (channel == nullptr)
    {
        message = ssh_message_get(session);
        if (message == nullptr) {
            break;
        }
        if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN && ssh_message_subtype(message) == SSH_CHANNEL_SESSION)
        {
            channel = ssh_message_channel_request_open_reply_accept(message);
        }
        else
        {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }

    if (channel == nullptr)
    {
        ssh_free(session);
        return;
    }

    // Wait for PTY and shell requests
    uint16_t initialWidth       = 80;
    uint16_t initialHeight      = 24;
    uint16_t initialPixelWidth  = 0;
    uint16_t initialPixelHeight = 0;
    bool shellStarted = false;
    while (!shellStarted)
    {
        message = ssh_message_get(session);
        if (message == nullptr) {
            break;
        }
        if (ssh_message_type(message) == SSH_REQUEST_CHANNEL)
        {
            const int subtype = ssh_message_subtype(message);
            if (subtype == SSH_CHANNEL_REQUEST_PTY || subtype == SSH_CHANNEL_REQUEST_SHELL || subtype == SSH_CHANNEL_REQUEST_EXEC)
            {
                if (subtype == SSH_CHANNEL_REQUEST_PTY) {
                    initialWidth       = static_cast<uint16_t>(ssh_message_channel_request_pty_width(message));
                    initialHeight      = static_cast<uint16_t>(ssh_message_channel_request_pty_height(message));
                    initialPixelWidth  = static_cast<uint16_t>(ssh_message_channel_request_pty_pxwidth(message));
                    initialPixelHeight = static_cast<uint16_t>(ssh_message_channel_request_pty_pxheight(message));
                }
                ssh_message_channel_request_reply_success(message);
                if (subtype == SSH_CHANNEL_REQUEST_SHELL || subtype == SSH_CHANNEL_REQUEST_EXEC) {
                    shellStarted = true;
                }
            }
            else
            {
                ssh_message_reply_default(message);
            }
        }
        else
        {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    }

    if (!shellStarted)
    {
        ssh_channel_close(channel);
        ssh_free(session);
        return;
    }

    // Register session as pending, request a channel from the device
    Session* sessionState = new Session();
    sessionState->SshSession = session;
    sessionState->SshChannel = channel;
    sessionState->Thread     = QThread::currentThread();
    sessionState->Server     = this;

    {
        QMutexLocker lock(&m_SessionsMutex);
        m_PendingSessions.append(sessionState);
    }

    // Request channel from device (crosses to main thread via queued connection)
    QMetaObject::invokeMethod(this, [this]() { m_Mux.RequestOpenChannel(); }, Qt::QueuedConnection);

    // Spin-wait for the channel ID to be assigned (OnChannelOpened is called from main thread)
    while (!sessionState->ChannelIDAssigned.load() && !sessionState->CloseRequested.load())
    {
        QThread::msleep(10);
        if (ssh_channel_is_closed(channel) || sessionState->CloseRequested.load())
        {
            QMutexLocker lock(&m_SessionsMutex);
            m_PendingSessions.removeOne(sessionState);
            if (m_Sessions.value(sessionState->ChannelID, nullptr) == sessionState) {
                m_Sessions.remove(sessionState->ChannelID);
            }
            delete sessionState;
            ssh_channel_close(channel);
            ssh_free(session);
            return;
        }
    }

    // server_callbacks must be non-NULL so that libssh dispatches channel request
    // callbacks (e.g. window-change) instead of routing them to the message queue.
    ssh_callbacks_init(&sessionState->ServerCbs);
    ssh_set_server_callbacks(session, &sessionState->ServerCbs);

    ssh_callbacks_init(&sessionState->WindowChangeCbs);
    sessionState->WindowChangeCbs.userdata = sessionState;
    sessionState->WindowChangeCbs.channel_pty_window_change_function = &SshServer::OnWindowChange;
    ssh_set_channel_callbacks(channel, &sessionState->WindowChangeCbs);

    const uint16_t channelID = sessionState->ChannelID;

    QMetaObject::invokeMethod(this, [this, channelID, initialWidth, initialHeight, initialPixelWidth, initialPixelHeight]()
    {
        m_Mux.SendWindowSizeChange(channelID, initialWidth, initialHeight, initialPixelWidth, initialPixelHeight);
    }, Qt::QueuedConnection);

    // Bridge loop: all libssh operations stay on this thread to avoid data races.
    // OnDataReceived enqueues into OutQueue; OnChannelClosed sets CloseRequested.
    char buffer[4096];
    while (!sessionState->CloseRequested.load() && ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel))
    {
        // Flush device→SSH queue
        {
            QMutexLocker outLock(&sessionState->OutMutex);
            while (!sessionState->OutQueue.isEmpty())
            {
                const QByteArray outData = sessionState->OutQueue.takeFirst();
                outLock.unlock();
                ssh_channel_write(channel, outData.constData(), static_cast<uint32_t>(outData.size()));
                outLock.relock();
            }
        }

        // Read SSH→device; packet processing inside here triggers the window-change callback
        const int bytesRead = ssh_channel_read_timeout(channel, buffer, sizeof(buffer), 0, 50);
        if (bytesRead > 0)
        {
            qDebug() << "SshServer: read" << bytesRead << "bytes from SSH client";
            const QByteArray data(buffer, bytesRead);
            QMetaObject::invokeMethod(this, [this, channelID, data]()
            {
                m_Mux.SendData(channelID, data);
            }, Qt::QueuedConnection);
        }
        else if (bytesRead == SSH_ERROR)
        {
            qDebug() << "SshServer: bridge loop exiting: SSH_ERROR:" << ssh_get_error(session);
            break;
        }
    }

    const bool deviceInitiatedClose = sessionState->CloseRequested.load();
    if (deviceInitiatedClose) {
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
    }

    if (!deviceInitiatedClose && !m_StopRequested.load())
    {
        QMetaObject::invokeMethod(this, [this, channelID]()
        {
            m_Mux.RequestCloseChannel(channelID);
        }, Qt::QueuedConnection);
    }

    ssh_free(session);
    {
        QMutexLocker lock(&m_SessionsMutex);
        if (m_Sessions.value(channelID, nullptr) == sessionState) {
            m_Sessions.remove(channelID);
        }
    }
    delete sessionState;
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::OnChannelOpened(uint16_t channelID)
{
    QMutexLocker lock(&m_SessionsMutex);
    if (!m_PendingSessions.isEmpty())
    {
        Session* session    = m_PendingSessions.takeFirst();
        session->ChannelID  = channelID;
        session->ChannelIDAssigned = true;
        m_Sessions[channelID] = session;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::OnChannelClosed(uint16_t channelID)
{
    QMutexLocker lock(&m_SessionsMutex);
    Session* session = m_Sessions.value(channelID, nullptr);
    if (session != nullptr) {
        session->CloseRequested.store(true);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

void SshServer::OnDataReceived(uint16_t channelID, QByteArray data)
{
    QMutexLocker lock(&m_SessionsMutex);
    Session* session = m_Sessions.value(channelID, nullptr);
    if (session != nullptr) {
        QMutexLocker outLock(&session->OutMutex);
        session->OutQueue.append(data);
    }
}

///////////////////////////////////////////////////////////////////////////////
/// \author Kurt Skauen
///////////////////////////////////////////////////////////////////////////////

int SshServer::OnWindowChange(ssh_session, ssh_channel, int width, int height,
                               int pxwidth, int pheight, void* userdata)
{
    Session* s = static_cast<Session*>(userdata);
    const uint16_t channelID = s->ChannelID;
    QMetaObject::invokeMethod(s->Server, [server = s->Server, channelID, width, height, pxwidth, pheight]()
    {
        server->m_Mux.SendWindowSizeChange(channelID,
                                           static_cast<uint16_t>(width),
                                           static_cast<uint16_t>(height),
                                           static_cast<uint16_t>(pxwidth),
                                           static_cast<uint16_t>(pheight));
    }, Qt::QueuedConnection);
    return 0;
}
