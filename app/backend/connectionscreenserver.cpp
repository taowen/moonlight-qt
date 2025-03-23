#include "connectionscreenserver.h"
#include <QDebug>

ConnectionScreenServer::ConnectionScreenServer(QObject *parent)
    : QObject(parent), m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &ConnectionScreenServer::handleNewConnection);
}

ConnectionScreenServer::~ConnectionScreenServer()
{
    stopServer();
}

bool ConnectionScreenServer::startServer(quint16 port)
{
    if (m_server->isListening()) {
        stopServer();
    }

    if (m_server->listen(QHostAddress::Any, port)) {
        qInfo() << "ConnectionScreenServer started on port" << port;
        emit serverStarted(port);
        return true;
    } else {
        QString errorMsg = m_server->errorString();
        qWarning() << "Failed to start ConnectionScreenServer:" << errorMsg;
        emit serverError(errorMsg);
        return false;
    }
}

void ConnectionScreenServer::stopServer()
{
    if (m_server->isListening()) {
        m_server->close();
        
        // 断开所有客户端连接
        for (QTcpSocket* socket : m_clientConnections) {
            socket->disconnectFromHost();
        }
        
        qDeleteAll(m_clientConnections);
        m_clientConnections.clear();
        
        qInfo() << "ConnectionScreenServer stopped";
        emit serverStopped();
    }
}

bool ConnectionScreenServer::isListening() const
{
    return m_server->isListening();
}

quint16 ConnectionScreenServer::serverPort() const
{
    return m_server->serverPort();
}

void ConnectionScreenServer::handleNewConnection()
{
    QTcpSocket *clientSocket = m_server->nextPendingConnection();
    
    if (!clientSocket) {
        return;
    }
    
    m_clientConnections.append(clientSocket);
    
    connect(clientSocket, &QTcpSocket::readyRead, this, &ConnectionScreenServer::handleReadyRead);
    connect(clientSocket, &QTcpSocket::disconnected, this, &ConnectionScreenServer::handleDisconnected);
    connect(clientSocket, &QTcpSocket::errorOccurred, this, &ConnectionScreenServer::handleError);
    
    qInfo() << "New client connected:" << clientSocket->peerAddress().toString();
}

void ConnectionScreenServer::handleReadyRead()
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
        
        // 回复"OK"给客户端
        clientSocket->write("OK\n");
        clientSocket->flush();
    }
}

void ConnectionScreenServer::handleDisconnected()
{
    QTcpSocket *clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (!clientSocket) {
        return;
    }
    
    qInfo() << "Client disconnected:" << clientSocket->peerAddress().toString();
    
    m_clientConnections.removeOne(clientSocket);
    clientSocket->deleteLater();
}

void ConnectionScreenServer::handleError(QAbstractSocket::SocketError socketError)
{
    QTcpSocket *clientSocket = qobject_cast<QTcpSocket*>(sender());
    if (!clientSocket) {
        return;
    }
    
    qWarning() << "Socket error:" << socketError << clientSocket->errorString();
}