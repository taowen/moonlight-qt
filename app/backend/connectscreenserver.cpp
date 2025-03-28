#include "computermanager.h"
#include "connectscreenserver.h"
#include "qqmlcontext.h"
#include <QDebug>
#include <cli/commandlineparser.h>
#include <cli/pair.h>
#include <cli/startstream.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QVersionNumber>

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

    // m_MdnsServer.reset(new QMdnsEngine::Server());
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

void ConnectScreenServer::setComputerManager(ComputerManager* instance) {
    computerManager = instance;
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
    if (!m_app || !m_engine) {
        clientSocket->write("ERROR: missing m_app or m_engine\n");
        clientSocket->flush();
        return;
    }
    
    if (clientSocket->canReadLine()) {
        QByteArray line = clientSocket->readLine();
        QString request = QString::fromUtf8(line).trimmed();
        qInfo() << "收到 ConnectScreen 请求:" << request;
        
        // 解析JSON请求
        QJsonDocument jsonDoc = QJsonDocument::fromJson(request.toUtf8());
        if (jsonDoc.isNull() || !jsonDoc.isObject()) {
            qWarning() << "无效的JSON请求格式";
            clientSocket->write("ERROR: Invalid JSON format\n");
            clientSocket->flush();
            return;
        }
        
        QJsonObject jsonObj = jsonDoc.object();
        QString action = jsonObj["action"].toString();
        QString ipAddress = jsonObj["ip"].toString();
        QString uuid = jsonObj["uuid"].toString();
        QString pin = jsonObj["pin"].toString();
        
        // 处理 connect 操作或默认操作（向后兼容）
        if (action.isEmpty() || action == "connect") {
            if (ipAddress.isEmpty()) {
                qWarning() << "JSON请求缺少IP地址";
                clientSocket->write("ERROR: Missing IP address\n");
                clientSocket->flush();
                return;
            }
            
            NvComputer* paired = nullptr;
            for(auto* computer : computerManager->getComputers()) {
                qDebug() << "列出已配对计算机 " << computer->name << ": " << computer->activeAddress.toString() << " uuid " << computer->uuid;
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
                qInfo() << "跳过配对";
                clientSocket->write("OK\n");
                clientSocket->flush();
                
                StreamingPreferences* preferences = StreamingPreferences::get();
                auto launcher   = new CliStartStream::Launcher(ipAddress, "Desktop", preferences, m_app);
                m_engine->rootContext()->setContextProperty("launcher", launcher);

                emit launchDesktop(new Session(paired, paired->appList[0], preferences));
            } else {
                // 使用从JSON中获取的PIN码，如果为空则使用默认值"1234"
                QString pinToUse = pin.isEmpty() ? "1234" : pin;

                auto launcher = new CliPair::Launcher(ipAddress, pinToUse, m_app);

                // 连接信号以处理配对过程和结果
                connect(launcher, &CliPair::Launcher::searchingComputer, this, [ipAddress]() {
                    qInfo() << "正在搜索计算机:" << ipAddress;
                });

                connect(launcher, &CliPair::Launcher::pairing, this, [](QString computerName, QString pin) {
                    qInfo() << "paring:" << computerName << " with " << pin;
                });

                connect(launcher, &CliPair::Launcher::success, this, [this, clientSocket, ipAddress, uuid, launcher](NvComputer* computer) {
                    qInfo() << "配对回调:" << ipAddress;
                    
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
                        
                        StreamingPreferences* preferences = StreamingPreferences::get();
                        auto launcher = new CliStartStream::Launcher(ipAddress, "Desktop", preferences, m_app);
                        m_engine->rootContext()->setContextProperty("launcher", launcher);
                        
                        emit launchDesktop(new Session(computer, computer->appList[0], preferences));
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
                m_engine->rootContext()->setContextProperty("launcher", launcher);
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
