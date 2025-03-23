#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QGuiApplication>
#include <QQmlApplicationEngine>

class ConnectScreenServer : public QObject
{
    Q_OBJECT

public:
    explicit ConnectScreenServer(QObject *parent = nullptr);
    ~ConnectScreenServer();

    bool startServer(quint16 port);
    void stopServer();
    bool isListening() const;
    quint16 serverPort() const;

    // 添加设置 app 和 engine 的方法
    void setAppAndEngine(QGuiApplication* app, QQmlApplicationEngine* engine);

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

    // 添加成员变量
    QGuiApplication* m_app = nullptr;
    QQmlApplicationEngine* m_engine = nullptr;
};