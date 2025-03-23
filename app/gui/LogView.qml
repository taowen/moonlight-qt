import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2
import Qt.labs.platform 1.1

Flickable {
    id: logPage
    objectName: qsTr("日志")

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: logColumn.width
    contentHeight: logColumn.height

    ScrollBar.vertical: ScrollBar {
        anchors {
            left: parent.right
            leftMargin: -10
        }
    }

    property var logEntries: []
    property int maxLogEntries: 1000
    property string logFilePath: ""
    property bool autoRefresh: true
    property int refreshInterval: 2000 // 2秒刷新一次
    
    Timer {
        id: refreshTimer
        interval: refreshInterval
        running: autoRefresh
        repeat: true
        onTriggered: {
            readLogFile()
        }
    }
    
    function getLogDir() {
        // 获取日志目录路径，这应该与 Path::getLogDir() 返回的路径相匹配
        // 这里使用一个简化的实现，实际应用中可能需要从 C++ 导出此函数
        var logDir
        if (Qt.platform.os === "windows") {
            logDir = StandardPaths.writableLocation(StandardPaths.AppLocalDataLocation) + "/Moonlight Game Streaming Project/Moonlight"
        } else if (Qt.platform.os === "osx") {
            logDir = StandardPaths.writableLocation(StandardPaths.AppLocalDataLocation) + "/Logs"
        } else {
            // Linux 和其他平台
            logDir = StandardPaths.writableLocation(StandardPaths.AppLocalDataLocation) + "/logs"
        }
        return logDir
    }
    
    function findLatestLogFile() {
        // 这个函数需要在 C++ 中实现，因为 QML 没有直接的文件系统遍历功能
        // 这里我们假设日志文件路径已经通过某种方式设置好了
        // 实际实现中，你需要从 C++ 导出一个函数来获取最新的日志文件
        console.log("需要在 C++ 中实现查找最新日志文件的功能")
        return ""
    }
    
    function readLogFile() {
        // 这个函数需要在 C++ 中实现，因为 QML 对文件读取的支持有限
        // 实际实现中，你需要从 C++ 导出一个函数来读取日志文件内容
        console.log("需要在 C++ 中实现读取日志文件的功能")
        
        // 模拟从文件读取日志
        // 在实际实现中，这部分代码会被替换为真正的文件读取逻辑
        var currentTime = new Date().toLocaleTimeString()
        addLogEntry("从日志文件读取的示例日志 - " + currentTime)
    }

    function addLogEntry(message) {
        // 添加新日志条目到数组开头
        logEntries.unshift({
            text: message,
            timestamp: new Date().toLocaleTimeString()
        })
        
        // 限制日志条目数量
        if (logEntries.length > maxLogEntries) {
            logEntries.pop()
        }
        
        // 更新模型
        logModel.clear()
        for (var i = 0; i < logEntries.length; i++) {
            logModel.append(logEntries[i])
        }
    }

    function clearLogs() {
        logEntries = []
        logModel.clear()
    }

    Column {
        id: logColumn
        width: logPage.width
        padding: 10
        spacing: 10

        Rectangle {
            width: parent.width - parent.padding * 2
            height: 40
            color: "#333333"
            radius: 5

            RowLayout {
                anchors.fill: parent
                anchors.margins: 5

                Label {
                    text: qsTr("系统日志")
                    font.pointSize: 14
                    font.bold: true
                    color: "white"
                    Layout.fillWidth: true
                }

                Switch {
                    text: qsTr("自动刷新")
                    checked: autoRefresh
                    onCheckedChanged: {
                        autoRefresh = checked
                        if (autoRefresh) {
                            readLogFile()
                        }
                    }
                }

                Button {
                    text: qsTr("刷新")
                    onClicked: readLogFile()
                }
            }
        }

        ListView {
            id: logListView
            width: parent.width - parent.padding * 2
            height: Math.min(contentHeight, logPage.height - 60)
            clip: true
            model: ListModel {
                id: logModel
            }
            
            delegate: Rectangle {
                width: logListView.width
                height: logText.height + 20
                color: index % 2 === 0 ? "#f0f0f0" : "#e0e0e0"
                radius: 3
                
                RowLayout {
                    anchors {
                        fill: parent
                        margins: 10
                    }
                    spacing: 10
                    
                    Label {
                        text: timestamp
                        font.pointSize: 10
                        color: "#666666"
                        Layout.preferredWidth: 80
                    }
                    
                    Label {
                        id: logText
                        text: model.text
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        color: "#333333"
                    }
                }
            }

            // 当没有日志时显示的占位符
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                visible: logModel.count === 0
                
                Label {
                    anchors.centerIn: parent
                    text: qsTr("暂无日志记录")
                    font.pointSize: 12
                    color: "#999999"
                }
            }
        }
    }

    // 组件完成后初始化
    Component.onCompleted: {
        addLogEntry("应用程序启动")
        readLogFile()
    }
} 