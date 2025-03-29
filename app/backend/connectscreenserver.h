#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <streaming/session.h>

class ConnectScreenServer : public QObject
{
    Q_OBJECT

public:
    explicit ConnectScreenServer(QQmlEngine* qmlEngine, StreamingPreferences* prefs);
    ~ConnectScreenServer();

    Q_INVOKABLE void startServer(ComputerManager* computerManager);
    void stopServer();

signals:
    void launchDesktop(Session* session);

private slots:
    void handleNewConnection();
    void handleReadyRead();
    void handleDisconnected();
    void handleError(QAbstractSocket::SocketError socketError);

private:
    QTcpServer *m_server;
    QList<QTcpSocket*> m_clientConnections;
    QQmlEngine* qmlEngine;
    ComputerManager* computerManager = nullptr;
    StreamingPreferences* prefs = nullptr;
};
