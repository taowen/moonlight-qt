#include "computermanager.h"
#include "connectscreenserver.h"
#include "qqmlcontext.h"
#include <QDebug>
#include <cli/pair.h>

ConnectScreenServer::ConnectScreenServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &ConnectScreenServer::handleNewConnection);
}

ConnectScreenServer::~ConnectScreenServer()
{
    stopServer();
}

bool ConnectScreenServer::startServer(quint16 port)
{
    if (m_server->isListening()) {
        stopServer();
    }

    if (m_server->listen(QHostAddress::Any, port)) {
        qInfo() << "ConnectScreenServer started on port" << port;
        emit serverStarted(port);
        return true;
    } else {
        QString errorMsg = m_server->errorString();
        qWarning() << "Failed to start ConnectScreenServer:" << errorMsg;
        emit serverError(errorMsg);
        return false;
    }
}

void ConnectScreenServer::stopServer()
{
    if (m_server->isListening()) {
        m_server->close();
        
        // 断开所有客户端连接
        for (QTcpSocket* socket : m_clientConnections) {
            socket->disconnectFromHost();
        }
        
        qDeleteAll(m_clientConnections);
        m_clientConnections.clear();
        
        qInfo() << "ConnectScreenServer stopped";
        emit serverStopped();
    }
}

bool ConnectScreenServer::isListening() const
{
    return m_server->isListening();
}

quint16 ConnectScreenServer::serverPort() const
{
    return m_server->serverPort();
}

void ConnectScreenServer::setAppAndEngine(QGuiApplication* app, QQmlApplicationEngine* engine)
{
    m_app = app;
    m_engine = engine;
}

void ConnectScreenServer::handleNewConnection()
{
    QTcpSocket *clientSocket = m_server->nextPendingConnection();
    
    if (!clientSocket) {
        return;
    }
    
    m_clientConnections.append(clientSocket);
    
    connect(clientSocket, &QTcpSocket::readyRead, this, &ConnectScreenServer::handleReadyRead);
    connect(clientSocket, &QTcpSocket::disconnected, this, &ConnectScreenServer::handleDisconnected);
    connect(clientSocket, &QTcpSocket::errorOccurred, this, &ConnectScreenServer::handleError);
    
    qInfo() << "New client connected:" << clientSocket->peerAddress().toString();
}

void ConnectScreenServer::handleReadyRead()
{
    QTcpSocket *clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (!clientSocket) {
        return;
    }
    
    while (clientSocket->canReadLine()) {
        QByteArray line = clientSocket->readLine();
        QString ipAddress = QString::fromUtf8(line).trimmed();
        
        qInfo() << "Received IP address from client:" << ipAddress;
        emit ipAddressReceived(ipAddress);

        if (m_app && m_engine) {
            auto launcher = new CliPair::Launcher(ipAddress, "1234", m_app);
            launcher->execute(ComputerManager::getComputerManagerInstance());
            m_engine->rootContext()->setContextProperty("launcher", launcher);
        } else {
            qWarning() << "Cannot create launcher: app or engine not set";
        }
        
        // 回复"OK"给客户端
        clientSocket->write("OK\n");
        clientSocket->flush();
    }
}

void ConnectScreenServer::handleDisconnected()
{
    QTcpSocket *clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (!clientSocket) {
        return;
    }
    
    qInfo() << "Client disconnected:" << clientSocket->peerAddress().toString();
    
    m_clientConnections.removeOne(clientSocket);
    clientSocket->deleteLater();
}

void ConnectScreenServer::handleError(QAbstractSocket::SocketError socketError)
{
    QTcpSocket *clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (!clientSocket) {
        return;
    }
    
    qWarning() << "Socket error:" << socketError << clientSocket->errorString();
}
