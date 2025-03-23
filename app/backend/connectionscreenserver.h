#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

class ConnectionScreenServer : public QObject
{
    Q_OBJECT

public:
    explicit ConnectionScreenServer(QObject *parent = nullptr);
    ~ConnectionScreenServer();

    bool startServer(quint16 port);
    void stopServer();
    bool isListening() const;
    quint16 serverPort() const;

signals:
    void ipAddressReceived(const QString &ipAddress);
    void serverStarted(quint16 port);
    void serverStopped();
    void serverError(const QString &errorMessage);

private slots:
    void handleNewConnection();
    void handleReadyRead();
    void handleDisconnected();
    void handleError(QAbstractSocket::SocketError socketError);

private:
    QTcpServer *m_server;
    QList<QTcpSocket*> m_clientConnections;
};