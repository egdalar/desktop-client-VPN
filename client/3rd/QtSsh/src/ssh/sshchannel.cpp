/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2012 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: http://www.qt-project.org/
**
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** Other Usage
**
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**************************************************************************/

#include "sshchannel_p.h"

#include "sshincomingpacket_p.h"
#include "sshsendfacility_p.h"
#include "sshlogging_p.h"

#include <botan_all.h>

#include <QTimer>

namespace QSsh {
namespace Internal {

const quint32 NoChannel = 0xffffffffu;

AbstractSshChannel::AbstractSshChannel(quint32 channelId,
    SshSendFacility &sendFacility)
    : m_sendFacility(sendFacility),
      m_localChannel(channelId), m_remoteChannel(NoChannel),
      m_localWindowSize(initialWindowSize()), m_remoteWindowSize(0),
      m_state(Inactive)
{
    m_timeoutTimer.setTimerType(Qt::VeryCoarseTimer);
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &AbstractSshChannel::timeout);
}

AbstractSshChannel::~AbstractSshChannel()
{

}

void AbstractSshChannel::setChannelState(ChannelState state)
{
    m_state = state;
    if (state == Closed)
        closeHook();
}

void AbstractSshChannel::requestSessionStart()
{
    // Note: We are just being paranoid here about the Botan exceptions,
    // which are extremely unlikely to happen, because if there was a problem
    // with our cryptography stuff, it would have hit us before, on
    // establishing the connection.
    try {
        m_sendFacility.sendSessionPacket(m_localChannel, initialWindowSize(), maxPacketSize());
        setChannelState(SessionRequested);
        m_timeoutTimer.start(ReplyTimeout);
    }  catch (const std::exception &e) {
        qCWarning(sshLog, "Botan error: %s", e.what());
        closeChannel();
    }
}

void AbstractSshChannel::sendData(const QByteArray &data)
{
    try {
        m_sendBuffer += data;
        flushSendBuffer();
    }  catch (const std::exception &e) {
        qCWarning(sshLog, "Botan error: %s", e.what());
        closeChannel();
    }
}

quint32 AbstractSshChannel::initialWindowSize()
{
    return maxPacketSize();
}

quint32 AbstractSshChannel::maxPacketSize()
{
    return 16 * 1024 * 1024;
}

void AbstractSshChannel::handleWindowAdjust(quint64 bytesToAdd)
{
    checkChannelActive();

    const quint64 newValue = m_remoteWindowSize + bytesToAdd;
    if (newValue > 0xffffffffu) {
        throw SSH_SERVER_EXCEPTION(SSH_DISCONNECT_PROTOCOL_ERROR,
            "Illegal window size requested.");
    }

    m_remoteWindowSize = newValue;
    flushSendBuffer();
}

void AbstractSshChannel::flushSendBuffer()
{
    while (true) {
        const quint32 bytesToSend = qMin(m_remoteMaxPacketSize,
                qMin<quint32>(m_remoteWindowSize, m_sendBuffer.size()));
        if (bytesToSend == 0)
            break;
        const QByteArray &data = m_sendBuffer.left(bytesToSend);
        m_sendFacility.sendChannelDataPacket(m_remoteChannel, data);
        m_sendBuffer.remove(0, bytesToSend);
        m_remoteWindowSize -= bytesToSend;
    }
}

void AbstractSshChannel::handleOpenSuccess(quint32 remoteChannelId,
    quint32 remoteWindowSize, quint32 remoteMaxPacketSize)
{
    const ChannelState oldState = m_state;
    switch (oldState) {
    case CloseRequested:   // closeChannel() was called while we were in SessionRequested state
    case SessionRequested:
        break; // Ok, continue.
    default:
        throw SSH_SERVER_EXCEPTION(SSH_DISCONNECT_PROTOCOL_ERROR,
            "Unexpected SSH_MSG_CHANNEL_OPEN_CONFIRMATION packet.");
    }

    m_timeoutTimer.stop();

   qCDebug(sshLog, "Channel opened. remote channel id: %u, remote window size: %u, "
       "remote max packet size: %u",
       remoteChannelId, remoteWindowSize, remoteMaxPacketSize);
   m_remoteChannel = remoteChannelId;
   m_remoteWindowSize = remoteWindowSize;
   m_remoteMaxPacketSize = remoteMaxPacketSize;
   setChannelState(SessionEstablished);
   if (oldState == CloseRequested)
       closeChannel();
   else
       handleOpenSuccessInternal();
}

void AbstractSshChannel::handleOpenFailure(const QString &reason)
{
    switch (m_state) {
    case SessionRequested:
        break; // Ok, continue.
    case CloseRequested:
        return; // Late server reply; we requested a channel close in the meantime.
    default:
        throw SSH_SERVER_EXCEPTION(SSH_DISCONNECT_PROTOCOL_ERROR,
            "Unexpected SSH_MSG_CHANNEL_OPEN_FAILURE packet.");
    }

    m_timeoutTimer.stop();

   qCDebug(sshLog, "Channel open request failed for channel %u", m_localChannel);
   handleOpenFailureInternal(reason);
}

void AbstractSshChannel::handleChannelEof()
{
    if (m_state == Inactive || m_state == Closed) {
        throw SSH_SERVER_EXCEPTION(SSH_DISCONNECT_PROTOCOL_ERROR,
            "Unexpected SSH_MSG_CHANNEL_EOF message.");
    }
    m_localWindowSize = 0;
    emit eof();
}

void AbstractSshChannel::handleChannelClose()
{
    qCDebug(sshLog, "Receiving CLOSE for channel %u", m_localChannel);
    if (channelState() == Inactive || channelState() == Closed) {
        throw SSH_SERVER_EXCEPTION(SSH_DISCONNECT_PROTOCOL_ERROR,
            "Unexpected SSH_MSG_CHANNEL_CLOSE message.");
    }
    closeChannel();
    setChannelState(Closed);
}

void AbstractSshChannel::handleChannelData(const QByteArray &data)
{
    const int bytesToDeliver = handleChannelOrExtendedChannelData(data);
    handleChannelDataInternal(bytesToDeliver == data.size()
        ? data : data.left(bytesToDeliver));
}

void AbstractSshChannel::handleChannelExtendedData(quint32 type, const QByteArray &data)
{
    const int bytesToDeliver = handleChannelOrExtendedChannelData(data);
    handleChannelExtendedDataInternal(type, bytesToDeliver == data.size()
        ? data : data.left(bytesToDeliver));
}

void AbstractSshChannel::handleChannelRequest(const SshIncomingPacket &packet)
{
    checkChannelActive();
    const QByteArray &requestType = packet.extractChannelRequestType();
    if (requestType == SshIncomingPacket::ExitStatusType)
        handleExitStatus(packet.extractChannelExitStatus());
    else if (requestType == SshIncomingPacket::ExitSignalType)
        handleExitSignal(packet.extractChannelExitSignal());
    else if (requestType != "eow@openssh.com") // Suppress warning for this one, as it's sent all the time.
        qCWarning(sshLog, "Ignoring unknown request type '%s'", requestType.data());
}

int AbstractSshChannel::handleChannelOrExtendedChannelData(const QByteArray &data)
{
    checkChannelActive();

    const int bytesToDeliver = qMin<quint32>(data.size(), maxDataSize());
    if (bytesToDeliver != data.size())
        qCWarning(sshLog, "Misbehaving server does not respect local window, clipping.");

    m_localWindowSize -= bytesToDeliver;
    if (m_localWindowSize < maxPacketSize()) {
        m_localWindowSize += maxPacketSize();
        m_sendFacility.sendWindowAdjustPacket(m_remoteChannel, maxPacketSize());
    }
    return bytesToDeliver;
}

void AbstractSshChannel::closeChannel()
{
    if (m_state == CloseRequested) {
        m_timeoutTimer.stop();
    } else if (m_state != Closed) {
        if (m_state == Inactive) {
            setChannelState(Closed);
        } else {
            const ChannelState oldState = m_state;
            setChannelState(CloseRequested);
            if (m_remoteChannel != NoChannel) {
                m_sendFacility.sendChannelEofPacket(m_remoteChannel);
                m_sendFacility.sendChannelClosePacket(m_remoteChannel);
            } else {
                QSSH_ASSERT(oldState == SessionRequested);
            }
        }
    }
}

void AbstractSshChannel::checkChannelActive() const
{
    if (channelState() == Inactive || channelState() == Closed)
        throw SSH_SERVER_EXCEPTION(SSH_DISCONNECT_PROTOCOL_ERROR,
            "Channel not open.");
}

quint32 AbstractSshChannel::maxDataSize() const
{
    return qMin(m_localWindowSize, maxPacketSize());
}

} // namespace Internal
} // namespace QSsh
