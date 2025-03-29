#include "computermanager.h"
#include "connectscreenserver.h"
#include "qqmlcontext.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVersionNumber>
#include <cli/pair.h>

ConnectScreenServer::ConnectScreenServer(QQmlEngine* qmlEngine, StreamingPreferences* prefs)
    : qmlEngine(qmlEngine), prefs(prefs)
{
}

ConnectScreenServer::~ConnectScreenServer()
{
    stopServer();
}

void ConnectScreenServer::startServer(ComputerManager* computerManager) {
    if (this->computerManager != nullptr) {
        return;
    }
    this->computerManager = computerManager;
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &ConnectScreenServer::handleNewConnection);
    if (m_server->listen(QHostAddress::Any, prefs->connectPort)) {
        qInfo() << "ConnectScreenServer started on port" << prefs->connectPort;
    } else {
        QString errorMsg = m_server->errorString();
        qWarning() << "Failed to start ConnectScreenServer:" << errorMsg;
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
    }
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

    if (clientSocket->canReadLine()) {
        QByteArray line = clientSocket->readLine();
        QString request = QString::fromUtf8(line).trimmed();
        qInfo() << "Received ConnectScreen Request:" << request;

        // 解析JSON请求
        QJsonDocument jsonDoc = QJsonDocument::fromJson(request.toUtf8());
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            qWarning() << "Invalid json format";
            clientSocket->write("ERROR: Invalid JSON format\n");
            clientSocket->flush();
            return;
        }

        QJsonObject jsonObj = jsonDoc.object();
        QString action = jsonObj["action"].toString();
        QString ipAddress = jsonObj["ip"].toString();
        QString uuid = jsonObj["uuid"].toString();
        QString pin = jsonObj["pin"].toString();

        if (action.isEmpty() || action == "connect") {
            if (ipAddress.isEmpty()) {
                qWarning() << "Missing ip address in json request";
                clientSocket->write("ERROR: Missing IP address\n");
                clientSocket->flush();
                return;
            }

            NvComputer* paired = nullptr;
            for(auto* computer : computerManager->getComputers()) {
                qDebug() << "list known computer " << computer->name << ": " << computer->activeAddress.toString() << " uuid " << computer->uuid;
                if (computer->uuid == uuid && computer->pairState == NvComputer::PS_PAIRED) {
                    paired = computer;
                }
            }

            if (paired != nullptr) {
                if(paired->appList.empty()) {
                    clientSocket->write("ERROR\n");
                    clientSocket->flush();
                    return;
                }
                qInfo() << "Skip pairing";
                clientSocket->write("OK\n");
                clientSocket->flush();
                emit launchDesktop(new Session(paired, paired->appList[0], prefs));
            } else {
                QString pinToUse = pin.isEmpty() ? "1234" : pin;

                auto launcher = new CliPair::Launcher(ipAddress, pinToUse, qmlEngine);

                connect(launcher, &CliPair::Launcher::searchingComputer, this, [ipAddress]() {
                    qInfo() << "Searching computer:" << ipAddress;
                });

                connect(launcher, &CliPair::Launcher::pairing, this, [](QString computerName, QString pin) {
                    qInfo() << "paring:" << computerName << " with " << pin;
                });

                connect(launcher, &CliPair::Launcher::success, this, [this, clientSocket, ipAddress, uuid, launcher](NvComputer* computer) {
                    qInfo() << "Paring callback:" << ipAddress;

                    // 添加3秒定时器，等待appList更新
                    QTimer::singleShot(3000, this, [this, computer, ipAddress, clientSocket]() {
                        if(computer->appList.empty()) {
                            qWarning() << "应用列表为空";
                            if (clientSocket && clientSocket->isValid() && clientSocket->state() == QAbstractSocket::ConnectedState) {
                                clientSocket->write("ERROR: Empty app list\n");
                                clientSocket->flush();
                            }
                            return;
                        } else {
                            // 配对成功后回复"OK"给客户端
                            if (clientSocket && clientSocket->isValid() && clientSocket->state() == QAbstractSocket::ConnectedState) {
                                clientSocket->write("OK\n");
                                clientSocket->flush();
                            }
                        }

                        emit launchDesktop(new Session(computer, computer->appList[0], prefs));
                    });

                    // 清理launcher对象
                    launcher->deleteLater();
                });

                connect(launcher, &CliPair::Launcher::failed, this, [this, clientSocket, ipAddress, launcher](QString error) {
                    qWarning() << "配对失败:" << ipAddress << "错误:" << error;

                    if (clientSocket && clientSocket->isValid() && clientSocket->state() == QAbstractSocket::ConnectedState) {
                        clientSocket->write("ERROR: paring failed\n");
                        clientSocket->flush();
                    }

                    // 清理launcher对象
                    launcher->deleteLater();
                });

                launcher->execute(computerManager, uuid);
                qmlEngine->rootContext()->setContextProperty("launcher", launcher);
            }
        } else {
            // 未知操作
            qWarning() << "未知的操作类型:" << action << action;
            clientSocket->write("ERROR: Unknown action\n");
            clientSocket->flush();
        }
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
