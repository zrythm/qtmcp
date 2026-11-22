// Copyright (C) 2025 Signal Slot Inc.
// SPDX-License-Identifier: LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimer>
#include <QtMcpCommon/qtmcpnamespace.h>
#include <QtMcpServer/QMcpServer>
#include <QtMcpServer/QMcpServerSession>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QTcpServer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <memory>

#ifdef QTMCP_EMBEDDED_BACKEND
#include <qmcpserverstreamablehttp.h>
#endif

using namespace Qt::Literals::StringLiterals;

namespace {

constexpr int Timeout = 5000;

// Waits for a reply to finish and returns its body. SSE replies never finish,
// so the tests that need one drive it through readyRead instead.
QByteArray waitForBody(QNetworkReply *reply, int *statusCode)
{
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(Timeout, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    if (statusCode)
        *statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return reply->readAll();
}

QJsonObject jsonRpcNotification(const QString &method, const QJsonObject &params = {})
{
    QJsonObject object;
    object.insert("jsonrpc"_L1, "2.0"_L1);
    object.insert("method"_L1, method);
    object.insert("params"_L1, params);
    return object;
}

// A request is a notification plus the id it is answered under.
QJsonObject jsonRpc(const QString &method, const QJsonValue &id, const QJsonObject &params = {})
{
    auto object = jsonRpcNotification(method, params);
    object.insert("id"_L1, id);
    return object;
}

// Adds the params._meta every 2026-07-28 request has to carry.
QJsonObject withStatelessMeta(QJsonObject message, const QString &version)
{
    auto params = message.value("params"_L1).toObject();
    auto meta = params.value("_meta"_L1).toObject();
    meta.insert("io.modelcontextprotocol/protocolVersion"_L1, version);
    params.insert("_meta"_L1, meta);
    message.insert("params"_L1, params);
    return message;
}

int errorCodeOf(const QByteArray &body)
{
    return QJsonDocument::fromJson(body).object()
            .value("error"_L1).toObject().value("code"_L1).toInt();
}

} // namespace

// Builds the server under test. The upstream build loads the backend as a
// plugin; a plain build links it in directly.
QMcpServer *createServer(QObject *parent)
{
#ifdef QTMCP_EMBEDDED_BACKEND
    return new QMcpServer(new QMcpServerStreamableHttp(), parent);
#else
    return new QMcpServer("streamablehttp"_L1, parent);
#endif
}

class tst_StreamableHttp : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void backendIsAvailable();
    void startFailureIsReported();
    void invalidPortIsRejected_data();
    void invalidPortIsRejected();
    void statefulLifecycle();
    void unknownSessionIsNotFound();
    void missingSessionHeaderIsBadRequest();
    void standaloneStreamAndDelete();
    void statelessRequest();
    void statelessHeaderMismatchIsRejected();
    void statelessRejectsGetAndDelete();
    void statelessSessionsEndWithTheirConnection();
    void listenSessionEndsWithItsStream();
    void statefulSessionSurvivesDisconnect();
    void listenAcknowledgementCarriesTheListenRequestId_data();
    void listenAcknowledgementCarriesTheListenRequestId();
    void forbiddenOrigin();

private:
    QNetworkRequest endpoint(const QString &protocolVersion = {}) const;
    // Runs the initialize / notifications/initialized handshake and returns the
    // session id the server minted.
    QByteArray openSession(const QString &protocolVersion,
                           QNetworkAccessManager *manager = nullptr);

    QMcpServer *m_server = nullptr;
    QNetworkAccessManager m_networkAccessManager;
    quint16 m_port = 0;
};

void tst_StreamableHttp::initTestCase()
{
#ifndef QTMCP_EMBEDDED_BACKEND
    if (!QMcpServer::backends().contains("streamablehttp"_L1))
        QSKIP("the streamablehttp backend is not available");
#endif

    // Let the OS pick a free port, then hand that port to the server.
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    m_port = probe.serverPort();
    probe.close();

    m_server = createServer(this);

    QEventLoop loop;
    connect(m_server, &QMcpServer::started, &loop, &QEventLoop::quit);
    QTimer::singleShot(Timeout, &loop, &QEventLoop::quit);
    m_server->start(u"127.0.0.1:%1"_s.arg(m_port));
    loop.exec();
}

void tst_StreamableHttp::cleanupTestCase()
{
    delete m_server;
    m_server = nullptr;
}

QNetworkRequest tst_StreamableHttp::endpoint(const QString &protocolVersion) const
{
    QNetworkRequest request(QUrl(u"http://127.0.0.1:%1/mcp"_s.arg(m_port)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"_ba);
    request.setRawHeader("Accept"_ba, "application/json, text/event-stream"_ba);
    if (!protocolVersion.isEmpty())
        request.setRawHeader("MCP-Protocol-Version"_ba, protocolVersion.toLatin1());
    return request;
}

QByteArray tst_StreamableHttp::openSession(const QString &protocolVersion,
                                           QNetworkAccessManager *manager)
{
    if (!manager)
        manager = &m_networkAccessManager;

    QJsonObject params;
    params.insert("protocolVersion"_L1, protocolVersion);
    params.insert("capabilities"_L1, QJsonObject());
    QJsonObject clientInfo;
    clientInfo.insert("name"_L1, "tst_streamablehttp"_L1);
    clientInfo.insert("version"_L1, "1.0"_L1);
    params.insert("clientInfo"_L1, clientInfo);

    const auto body = QJsonDocument(jsonRpc("initialize"_L1, 1, params)).toJson(QJsonDocument::Compact);
    auto *reply = manager->post(endpoint(protocolVersion), body);
    int statusCode = 0;
    waitForBody(reply, &statusCode);
    reply->deleteLater();
    if (statusCode != 200)
        return {};

    const auto sessionId = reply->rawHeader("Mcp-Session-Id"_ba);
    if (sessionId.isEmpty())
        return {};

    // The session only becomes usable once the client confirms initialization.
    auto request = endpoint(protocolVersion);
    request.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    const auto initialized = QJsonDocument(jsonRpcNotification("notifications/initialized"_L1))
                                     .toJson(QJsonDocument::Compact);
    auto *notifyReply = manager->post(request, initialized);
    waitForBody(notifyReply, nullptr);
    notifyReply->deleteLater();

    return sessionId;
}

void tst_StreamableHttp::backendIsAvailable()
{
#ifdef QTMCP_EMBEDDED_BACKEND
    // This build links the backend directly; the constructed server is the
    // availability proof.
    QVERIFY(m_server);
#else
    QVERIFY(QMcpServer::backends().contains("streamablehttp"_L1));
#endif
}

void tst_StreamableHttp::startFailureIsReported()
{
    // m_server already listens on m_port, so a second server cannot.
    std::unique_ptr<QMcpServer> server(createServer(nullptr));
    QSignalSpy startedSpy(server.get(), &QMcpServer::started);
    QSignalSpy errorSpy(server.get(), &QMcpServer::errorOccurred);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"^Failed to listen on "_s));
    server->start(u"127.0.0.1:%1"_s.arg(m_port));
    // The error arrives from the event loop, not from within start().
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(errorSpy.wait(Timeout));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(!errorSpy.first().first().toString().isEmpty());
    QCOMPARE(startedSpy.count(), 0);
}

void tst_StreamableHttp::invalidPortIsRejected_data()
{
    QTest::addColumn<QString>("address");
    QTest::newRow("not a number") << u"127.0.0.1:abc"_s;
    QTest::newRow("out of range") << u"127.0.0.1:70000"_s;
    QTest::newRow("negative") << u"127.0.0.1:-1"_s;
    QTest::newRow("empty") << u"127.0.0.1:"_s;
}

void tst_StreamableHttp::invalidPortIsRejected()
{
    QFETCH(QString, address);
    std::unique_ptr<QMcpServer> server(createServer(nullptr));
    QSignalSpy startedSpy(server.get(), &QMcpServer::started);
    QSignalSpy errorSpy(server.get(), &QMcpServer::errorOccurred);
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(u"^Invalid port in "_s));
    server->start(address);
    QVERIFY(errorSpy.wait(Timeout));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(startedSpy.count(), 0);
}

void tst_StreamableHttp::statefulLifecycle()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2025_11_25);

    // initialize mints a session and reports it in the response header.
    QJsonObject params;
    params.insert("protocolVersion"_L1, version);
    params.insert("capabilities"_L1, QJsonObject());
    QJsonObject clientInfo;
    clientInfo.insert("name"_L1, "tst_streamablehttp"_L1);
    clientInfo.insert("version"_L1, "1.0"_L1);
    params.insert("clientInfo"_L1, clientInfo);

    const auto body = QJsonDocument(jsonRpc("initialize"_L1, 1, params)).toJson(QJsonDocument::Compact);
    auto *reply = m_networkAccessManager.post(endpoint(version), body);
    int statusCode = 0;
    const auto responseBody = waitForBody(reply, &statusCode);
    reply->deleteLater();

    QCOMPARE(statusCode, 200);
    QCOMPARE(reply->header(QNetworkRequest::ContentTypeHeader).toString(), "application/json"_L1);
    const auto sessionId = reply->rawHeader("Mcp-Session-Id"_ba);
    QVERIFY(!sessionId.isEmpty());
    QVERIFY(!QUuid::fromString(QString::fromLatin1(sessionId)).isNull());

    const auto response = QJsonDocument::fromJson(responseBody).object();
    // The id the client chose comes back unchanged, even though the transport
    // rewrites it on the way in.
    QCOMPARE(response.value("id"_L1).toInt(), 1);
    QCOMPARE(response.value("result"_L1).toObject().value("protocolVersion"_L1).toString(), version);

    // notifications/initialized is a notification, so it is only acknowledged.
    auto notifyRequest = endpoint(version);
    notifyRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    const auto initialized = QJsonDocument(jsonRpcNotification("notifications/initialized"_L1))
                                     .toJson(QJsonDocument::Compact);
    auto *notifyReply = m_networkAccessManager.post(notifyRequest, initialized);
    const auto notifyBody = waitForBody(notifyReply, &statusCode);
    notifyReply->deleteLater();
    QCOMPARE(statusCode, 202);
    QVERIFY(notifyBody.isEmpty());

    // A request on the established session is answered with a JSON body.
    auto listRequest = endpoint(version);
    listRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    const auto listBody = QJsonDocument(jsonRpc("tools/list"_L1, 7)).toJson(QJsonDocument::Compact);
    auto *listReply = m_networkAccessManager.post(listRequest, listBody);
    const auto listResponseBody = waitForBody(listReply, &statusCode);
    listReply->deleteLater();

    QCOMPARE(statusCode, 200);
    const auto listResponse = QJsonDocument::fromJson(listResponseBody).object();
    QCOMPARE(listResponse.value("id"_L1).toInt(), 7);
    QVERIFY(listResponse.contains("result"_L1));
    QVERIFY(listResponse.value("result"_L1).toObject().value("tools"_L1).isArray());
    // Before 2026-07-28 a result carries no resultType discriminator.
    QVERIFY(!listResponse.value("result"_L1).toObject().contains("resultType"_L1));
}

void tst_StreamableHttp::unknownSessionIsNotFound()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2025_11_25);
    auto request = endpoint(version);
    request.setRawHeader("Mcp-Session-Id"_ba,
                         QUuid::createUuid().toByteArray(QUuid::WithoutBraces));

    const auto body = QJsonDocument(jsonRpc("tools/list"_L1, 1)).toJson(QJsonDocument::Compact);
    auto *reply = m_networkAccessManager.post(request, body);
    int statusCode = 0;
    waitForBody(reply, &statusCode);
    reply->deleteLater();

    QCOMPARE(statusCode, 404);
}

void tst_StreamableHttp::missingSessionHeaderIsBadRequest()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2025_11_25);
    const auto body = QJsonDocument(jsonRpc("tools/list"_L1, 1)).toJson(QJsonDocument::Compact);
    auto *reply = m_networkAccessManager.post(endpoint(version), body);
    int statusCode = 0;
    waitForBody(reply, &statusCode);
    reply->deleteLater();

    QCOMPARE(statusCode, 400);
}

void tst_StreamableHttp::standaloneStreamAndDelete()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2025_11_25);
    const auto sessionId = openSession(version);
    QVERIFY(!sessionId.isEmpty());

    // A GET that does not accept text/event-stream is not a stream request.
    auto plainRequest = endpoint(version);
    plainRequest.setRawHeader("Accept"_ba, "application/json"_ba);
    plainRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    auto *plainReply = m_networkAccessManager.get(plainRequest);
    int statusCode = 0;
    waitForBody(plainReply, &statusCode);
    plainReply->deleteLater();
    QCOMPARE(statusCode, 405);

    // The stream stays open, so wait for the response headers rather than for
    // finished().
    auto streamRequest = endpoint(version);
    streamRequest.setRawHeader("Accept"_ba, "text/event-stream"_ba);
    streamRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    auto *streamReply = m_networkAccessManager.get(streamRequest);

    QByteArray streamed;
    connect(streamReply, &QNetworkReply::readyRead, streamReply, [&streamed, streamReply]() {
        streamed.append(streamReply->readAll());
    });

    QEventLoop loop;
    connect(streamReply, &QNetworkReply::metaDataChanged, &loop, &QEventLoop::quit);
    connect(streamReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(Timeout, &loop, &QEventLoop::quit);
    loop.exec();

    QCOMPARE(streamReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QCOMPARE(streamReply->header(QNetworkRequest::ContentTypeHeader).toString(),
             "text/event-stream"_L1);
    QVERIFY(!streamReply->isFinished());

    // A notification for that session travels down the standalone stream.
    const auto sessions = m_server->findChildren<QMcpServerSession *>();
    QVERIFY(!sessions.isEmpty());
    for (auto *session : sessions)
        emit session->toolListChanged();

    QTRY_VERIFY_WITH_TIMEOUT(streamed.contains("notifications/tools/list_changed"), Timeout);
    QVERIFY(streamed.startsWith("data: "));

    streamReply->abort();
    streamReply->deleteLater();

    // DELETE ends the session; a request on it afterwards is a 404.
    auto deleteRequest = endpoint(version);
    deleteRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    auto *deleteReply = m_networkAccessManager.deleteResource(deleteRequest);
    waitForBody(deleteReply, &statusCode);
    deleteReply->deleteLater();
    QCOMPARE(statusCode, 200);

    auto afterRequest = endpoint(version);
    afterRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    const auto body = QJsonDocument(jsonRpc("tools/list"_L1, 1)).toJson(QJsonDocument::Compact);
    auto *afterReply = m_networkAccessManager.post(afterRequest, body);
    waitForBody(afterReply, &statusCode);
    afterReply->deleteLater();
    QCOMPARE(statusCode, 404);
}

void tst_StreamableHttp::statelessRequest()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2026_07_28);

    auto request = endpoint(version);
    request.setRawHeader("Mcp-Method"_ba, "tools/list"_ba);
    // 2026-07-28 has no sessions, so any Mcp-Session-Id must simply be ignored.
    request.setRawHeader("Mcp-Session-Id"_ba,
                         QUuid::createUuid().toByteArray(QUuid::WithoutBraces));

    const auto message = withStatelessMeta(jsonRpc("tools/list"_L1, "abc"_L1), version);
    auto *reply = m_networkAccessManager.post(request,
                                              QJsonDocument(message).toJson(QJsonDocument::Compact));
    int statusCode = 0;
    const auto body = waitForBody(reply, &statusCode);
    reply->deleteLater();

    QCOMPARE(statusCode, 200);
    // No session is minted, so no session header is handed out either.
    QVERIFY(reply->rawHeader("Mcp-Session-Id"_ba).isEmpty());

    const auto response = QJsonDocument::fromJson(body).object();
    // A string id round trips just as a numeric one does.
    QCOMPARE(response.value("id"_L1).toString(), "abc"_L1);
    const auto result = response.value("result"_L1).toObject();
    QVERIFY(result.value("tools"_L1).isArray());
    // Since 2026-07-28 every result carries the MRTR discriminator, which is
    // "complete" for a result that is not an interim one.
    QCOMPARE(result.value("resultType"_L1).toString(), "complete"_L1);
    QVERIFY(result.value("_meta"_L1).toObject().contains("io.modelcontextprotocol/serverInfo"_L1));
}

void tst_StreamableHttp::statelessHeaderMismatchIsRejected()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2026_07_28);
    const auto message = withStatelessMeta(jsonRpc("tools/list"_L1, 1), version);
    const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    int statusCode = 0;

    // Mcp-Method is mandatory since 2026-07-28.
    auto *missing = m_networkAccessManager.post(endpoint(version), body);
    auto responseBody = waitForBody(missing, &statusCode);
    missing->deleteLater();
    QCOMPARE(statusCode, 400);
    QCOMPARE(errorCodeOf(responseBody), -32020);

    // ... and it has to agree with the body.
    auto mismatched = endpoint(version);
    mismatched.setRawHeader("Mcp-Method"_ba, "prompts/list"_ba);
    auto *wrongMethod = m_networkAccessManager.post(mismatched, body);
    responseBody = waitForBody(wrongMethod, &statusCode);
    wrongMethod->deleteLater();
    QCOMPARE(statusCode, 400);
    QCOMPARE(errorCodeOf(responseBody), -32020);

    // So does the protocol version in params._meta.
    auto versionMismatch = endpoint(version);
    versionMismatch.setRawHeader("Mcp-Method"_ba, "tools/list"_ba);
    const auto staleMeta = withStatelessMeta(jsonRpc("tools/list"_L1, 1), "2025-11-25"_L1);
    auto *wrongVersion = m_networkAccessManager.post(
            versionMismatch, QJsonDocument(staleMeta).toJson(QJsonDocument::Compact));
    responseBody = waitForBody(wrongVersion, &statusCode);
    wrongVersion->deleteLater();
    QCOMPARE(statusCode, 400);
    QCOMPARE(errorCodeOf(responseBody), -32020);

    // tools/call additionally has to name its target in Mcp-Name.
    QJsonObject callParams;
    callParams.insert("name"_L1, QString::fromUtf8("計算"));
    callParams.insert("arguments"_L1, QJsonObject());
    const auto callMessage = withStatelessMeta(jsonRpc("tools/call"_L1, 2, callParams), version);
    const auto callBody = QJsonDocument(callMessage).toJson(QJsonDocument::Compact);

    auto missingName = endpoint(version);
    missingName.setRawHeader("Mcp-Method"_ba, "tools/call"_ba);
    auto *noName = m_networkAccessManager.post(missingName, callBody);
    responseBody = waitForBody(noName, &statusCode);
    noName->deleteLater();
    QCOMPARE(statusCode, 400);
    QCOMPARE(errorCodeOf(responseBody), -32020);

    // A non ASCII name arrives base64 wrapped in the sentinel and has to be
    // decoded before it can be compared with the body.
    auto encodedName = endpoint(version);
    encodedName.setRawHeader("Mcp-Method"_ba, "tools/call"_ba);
    encodedName.setRawHeader("Mcp-Name"_ba,
                             "=?base64?"_ba + QString::fromUtf8("計算").toUtf8().toBase64() + "?="_ba);
    auto *withName = m_networkAccessManager.post(encodedName, callBody);
    responseBody = waitForBody(withName, &statusCode);
    withName->deleteLater();
    // The header check passes, so the message reaches the core and is answered
    // on its merits rather than rejected as a header mismatch.
    QCOMPARE(statusCode, 200);
    QVERIFY(errorCodeOf(responseBody) != -32020);
    QCOMPARE(QJsonDocument::fromJson(responseBody).object().value("id"_L1).toInt(), 2);
}

void tst_StreamableHttp::statelessRejectsGetAndDelete()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2026_07_28);
    int statusCode = 0;

    auto getRequest = endpoint(version);
    getRequest.setRawHeader("Accept"_ba, "text/event-stream"_ba);
    auto *getReply = m_networkAccessManager.get(getRequest);
    waitForBody(getReply, &statusCode);
    getReply->deleteLater();
    QCOMPARE(statusCode, 405);

    auto *deleteReply = m_networkAccessManager.deleteResource(endpoint(version));
    waitForBody(deleteReply, &statusCode);
    deleteReply->deleteLater();
    QCOMPARE(statusCode, 405);
}

/*
 * Scenario: a long running server serves stateless clients that reconnect
 *           for every request
 * GIVEN a 2026-07-28 client that sends its requests over its own connection
 * WHEN  the connection closes
 * THEN  the session the requests ran on is destroyed, and a later connection
 *       runs on a session of its own
 */
void tst_StreamableHttp::statelessSessionsEndWithTheirConnection()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2026_07_28);

    // Earlier tests keep their sessions on the shared connection pool, so all
    // counts are relative to the sessions that exist now.
    QList<QUuid> idsBefore;
    for (auto *session : m_server->sessions())
        idsBefore.append(session->sessionId());
    const int baseline = idsBefore.size();

    QList<QUuid> destroyedIds;
    // Scoped to this function: the connection must not outlive the captured
    // list, because a session may be destroyed while a later test runs.
    QObject scope;
    connect(m_server, &QMcpServer::sessionDestroyed, &scope,
            [&destroyedIds](QMcpServerSession *session) {
                // The reported session is still readable.
                destroyedIds.append(session->sessionId());
            });

    for (int i = 0; i < 2; ++i) {
        // A manager of its own, so that releasing it closes the connection.
        auto *manager = new QNetworkAccessManager(this);
        auto request = endpoint(version);
        request.setRawHeader("Mcp-Method"_ba, "tools/list"_ba);
        const auto message = withStatelessMeta(jsonRpc("tools/list"_L1, i), version);
        auto *reply = manager->post(request, QJsonDocument(message).toJson(QJsonDocument::Compact));
        int statusCode = 0;
        waitForBody(reply, &statusCode);
        reply->deleteLater();
        QCOMPARE(statusCode, 200);

        const auto sessions = m_server->sessions();
        QCOMPARE(sessions.size(), baseline + 1);
        QUuid liveId;
        for (auto *session : sessions)
            if (!idsBefore.contains(session->sessionId()))
                liveId = session->sessionId();
        QVERIFY(!liveId.isNull());

        manager->deleteLater();
        QTRY_COMPARE_WITH_TIMEOUT(destroyedIds.size(), i + 1, Timeout);
        QCOMPARE(destroyedIds.last(), liveId);
        QCOMPARE(m_server->sessions().size(), baseline);
    }

    // Every connection ran on its own session.
    QVERIFY(destroyedIds.at(0) != destroyedIds.at(1));
}

/*
 * Scenario: a subscription stream owns its session
 * GIVEN a 2026-07-28 client with an open subscriptions/listen stream
 * WHEN  the stream connection closes
 * THEN  the dedicated session the subscription was routed on is destroyed
 */
void tst_StreamableHttp::listenSessionEndsWithItsStream()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2026_07_28);

    QUuid listenSessionId;
    bool destroyed = false;
    // Scoped to this function, so that a session destroyed while a later test
    // runs cannot write to this stack frame.
    QObject scope;
    connect(m_server, &QMcpServer::sessionDestroyed, &scope,
            [&destroyed, &listenSessionId](QMcpServerSession *session) {
                if (session->sessionId() == listenSessionId)
                    destroyed = true;
            });

    const int baseline = m_server->sessions().size();

    // Open the subscription stream on a connection of its own, so that no
    // other session shares its lifetime.
    QJsonObject optIns;
    optIns.insert("toolsListChanged"_L1, true);
    QJsonObject params;
    params.insert("notifications"_L1, optIns);
    const auto message = withStatelessMeta(jsonRpc("subscriptions/listen"_L1, "listen:1"_L1, params),
                                           version);
    const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    request += "Accept: application/json, text/event-stream\r\n";
    request += "MCP-Protocol-Version: " + version.toLatin1() + "\r\n";
    request += "Mcp-Method: subscriptions/listen\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
    request += body;

    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, m_port);
    QTRY_VERIFY_WITH_TIMEOUT(socket.state() == QAbstractSocket::ConnectedState, Timeout);
    socket.write(request);
    QByteArray received;
    connect(&socket, &QTcpSocket::readyRead, &socket, [&received, &socket]() {
        received.append(socket.readAll());
    });
    QTRY_VERIFY_WITH_TIMEOUT(
            received.contains("notifications/subscriptions/acknowledged"), Timeout);
    QVERIFY(received.startsWith("HTTP/1.1 200"));
    QVERIFY(received.contains("text/event-stream"));

    for (auto *session : m_server->sessions()) {
        if (session->hasListenSubscriptions())
            listenSessionId = session->sessionId();
    }
    // The subscription runs on a dedicated session of its own.
    QVERIFY(!listenSessionId.isNull());
    QCOMPARE(m_server->sessions().size(), baseline + 1);
    QVERIFY(!destroyed);

    // Closing the stream ends the subscription, and with it its session.
    socket.disconnectFromHost();
    QTRY_VERIFY_WITH_TIMEOUT(destroyed, Timeout);
    for (auto *session : m_server->sessions())
        QVERIFY(session->sessionId() != listenSessionId);
}

/*
 * Scenario: a pre-2026-07-28 session is bound to its Mcp-Session-Id, not to a
 *           connection
 * GIVEN a 2025-11-25 client whose initialize connection closed
 * WHEN  no DELETE arrives
 * THEN  the session survives the closed connection
 * WHEN  the client sends DELETE
 * THEN  the session is destroyed
 */
void tst_StreamableHttp::statefulSessionSurvivesDisconnect()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2025_11_25);

    QUuid targetId;
    bool destroyed = false;
    // Scoped to this function, so that a session destroyed while a later test
    // runs cannot write to this stack frame.
    QObject scope;
    connect(m_server, &QMcpServer::sessionDestroyed, &scope,
            [&destroyed, &targetId](QMcpServerSession *session) {
                if (session->sessionId() == targetId)
                    destroyed = true;
            });

    auto *manager = new QNetworkAccessManager(this);
    const auto sessionId = openSession(version, manager);
    QVERIFY(!sessionId.isEmpty());
    targetId = QUuid::fromString(QString::fromLatin1(sessionId));
    QVERIFY(!targetId.isNull());

    manager->deleteLater();
    // A connection teardown arriving in that window must not end the session.
    QTest::qWait(300);
    bool stillListed = false;
    for (auto *session : m_server->sessions()) {
        if (session->sessionId() == targetId)
            stillListed = true;
    }
    QVERIFY(stillListed);
    QVERIFY(!destroyed);

    auto deleteRequest = endpoint(version);
    deleteRequest.setRawHeader("Mcp-Session-Id"_ba, sessionId);
    auto *deleteReply = m_networkAccessManager.deleteResource(deleteRequest);
    int statusCode = 0;
    waitForBody(deleteReply, &statusCode);
    deleteReply->deleteLater();
    QCOMPARE(statusCode, 200);
    QTRY_VERIFY_WITH_TIMEOUT(destroyed, Timeout);
    for (auto *session : m_server->sessions())
        QVERIFY(session->sessionId() != targetId);
}

void tst_StreamableHttp::listenAcknowledgementCarriesTheListenRequestId_data()
{
    QTest::addColumn<QJsonValue>("listenId");

    QTest::newRow("string id") << QJsonValue("listen:0"_L1);
    QTest::newRow("numeric id") << QJsonValue(42);
}

/*
 * Scenario: a client correlates the subscription acknowledgement with its
 *           subscriptions/listen request
 * GIVEN a 2026-07-28 client that opens a subscription with a listen request
 * WHEN  the server acknowledges the subscription on the stream
 * THEN  the acknowledgement is tagged with the id of that listen request, as
 *       required for io.modelcontextprotocol/subscriptionId
 */
void tst_StreamableHttp::listenAcknowledgementCarriesTheListenRequestId()
{
    QFETCH(QJsonValue, listenId);
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2026_07_28);

    // GIVEN: A listen request that opts in to tool list changes.
    QJsonObject optIns;
    optIns.insert("toolsListChanged"_L1, true);
    QJsonObject params;
    params.insert("notifications"_L1, optIns);
    const auto message = withStatelessMeta(jsonRpc("subscriptions/listen"_L1, listenId, params),
                                           version);
    auto request = endpoint(version);
    request.setRawHeader("Mcp-Method"_ba, "subscriptions/listen"_ba);

    // WHEN: The client opens the subscription stream.
    auto *streamReply = m_networkAccessManager.post(
            request, QJsonDocument(message).toJson(QJsonDocument::Compact));
    QByteArray streamed;
    connect(streamReply, &QNetworkReply::readyRead, streamReply, [&streamed, streamReply]() {
        streamed.append(streamReply->readAll());
    });
    QTRY_VERIFY_WITH_TIMEOUT(streamed.contains("notifications/subscriptions/acknowledged"), Timeout);

    // THEN: The acknowledgement names the listen request as its subscription.
    QVERIFY(streamed.startsWith("data: "));
    const auto acknowledged = QJsonDocument::fromJson(streamed.mid(6).trimmed()).object();
    const auto subscriptionId = acknowledged.value("params"_L1).toObject()
            .value("_meta"_L1).toObject()
            .value("io.modelcontextprotocol/subscriptionId"_L1);
    QCOMPARE(subscriptionId, listenId);

    streamReply->abort();
    streamReply->deleteLater();
}

void tst_StreamableHttp::forbiddenOrigin()
{
    const auto version = QtMcp::protocolVersionToString(QtMcp::ProtocolVersion::v2025_11_25);
    const auto body = QJsonDocument(jsonRpc("tools/list"_L1, 1)).toJson(QJsonDocument::Compact);
    int statusCode = 0;

    auto request = endpoint(version);
    request.setRawHeader("Origin"_ba, "http://evil.example.com"_ba);
    auto *reply = m_networkAccessManager.post(request, body);
    waitForBody(reply, &statusCode);
    reply->deleteLater();
    QCOMPARE(statusCode, 403);

    // A loopback origin is always accepted; this one only fails later, on the
    // missing session header.
    auto localRequest = endpoint(version);
    localRequest.setRawHeader("Origin"_ba, "http://localhost:3000"_ba);
    auto *localReply = m_networkAccessManager.post(localRequest, body);
    waitForBody(localReply, &statusCode);
    localReply->deleteLater();
    QCOMPARE(statusCode, 400);
}

QTEST_MAIN(tst_StreamableHttp)
#include "tst_streamablehttp.moc"
