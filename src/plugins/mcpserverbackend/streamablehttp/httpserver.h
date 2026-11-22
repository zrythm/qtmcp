// Copyright (C) 2025 Signal Slot Inc.
// SPDX-License-Identifier: LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include <QtCore/QJsonObject>
#include <QtCore/QStringList>
#include <QtMcpServer/qmcpabstracthttpserver.h>
#include <QtNetwork/QNetworkRequest>
#include <functional>

/*!
    \class HttpServer
    \internal
    \brief Implements the MCP Streamable HTTP transport on a single \c /mcp endpoint.

    The transport behaves differently depending on the revision a request
    declares through its \c MCP-Protocol-Version header:

    \list
    \li 2025-03-26 .. 2025-11-25: \c initialize mints a session that is returned
        in the \c Mcp-Session-Id response header and echoed back by the client
        on every subsequent request. \c GET opens a standalone SSE stream for
        that session, \c DELETE terminates it.
    \li 2026-07-28: sessions are gone from the wire. Requests run on one
        internal session per HTTP connection, which ends with the connection;
        \c subscriptions/listen gets a dedicated session that ends with its
        stream, because the core routes notifications per session.
        \c GET and \c DELETE are not allowed.
    \endlist
*/
class HttpServer : public QMcpAbstractHttpServer
{
    Q_OBJECT
public:
    explicit HttpServer(QObject *parent = nullptr);
    ~HttpServer() override;

    /*!
        Origins accepted in addition to the loopback ones. Validating \c Origin
        is what keeps a locally bound MCP server safe from DNS rebinding.
    */
    QStringList allowedOrigins() const;
    void setAllowedOrigins(const QStringList &origins);

    /*!
        Decides whether a request may be served, and as whom.

        The authenticator inspects the HTTP request (typically its
        \c Authorization header) and returns an identity string for an
        authenticated request, or an empty string to reject it with 401.
        The identity of the connection a session belongs to is reported
        through authenticated().
    */
    using Authenticator = std::function<QString(const QNetworkRequest &)>;
    void setAuthenticator(Authenticator authenticator);
    Authenticator authenticator() const;

    Q_INVOKABLE QByteArray postMcp(const QNetworkRequest &request, const QByteArray &body);
    Q_INVOKABLE QByteArray getMcp(const QNetworkRequest &request);
    Q_INVOKABLE QByteArray deleteMcp(const QNetworkRequest &request);

public slots:
    void send(const QUuid &session, const QJsonObject &object);

signals:
    void newSession(const QUuid &session);
    void received(const QUuid &session, const QJsonObject &object);
    void authenticated(const QUuid &session, const QString &identity);
    // The transport dropped a session: its connection closed, its dedicated
    // stream ended, or the client terminated it with DELETE. The core
    // destroys the matching QMcpServerSession.
    void sessionFinished(const QUuid &session);

private:
    QString authenticate(const QNetworkRequest &request, const QUuid &exchange);

    class Private;
    QScopedPointer<Private> d;
};

#endif // HTTPSERVER_H
