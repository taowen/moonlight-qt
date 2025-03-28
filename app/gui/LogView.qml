import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2
import Qt.labs.platform 1.1
import LogManager 1.0

Flickable {
    id: logPage
    objectName: qsTr("日志")

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: logColumn.width
    contentHeight: logColumn.height

    property var logEntries: []
    property int maxLogEntries: 1000
    property string logFilePath: LogManager.latestLogPath
    
    // 监听 LogManager 的文件变化信号
    Connections {
        target: LogManager
        function onLogFileChanged() {
            readLogFile()
        }
        
        function onLatestLogPathChanged() {
            logFilePath = LogManager.latestLogPath
            readLogFile()
        }
        
        function onLogContentChanged() {
            readLogFile()
        }
    }
    
    function readLogFile() {
        if (logFilePath === "") {
            return
        }
        
        // 使用 LogManager 读取日志文件
        var logLines = LogManager.readLogFile(logFilePath, maxLogEntries)
        
        // 清除现有日志
        clearLogs()
        
        for (var i = 0; i < logLines.length; i++) {
            addLogEntry(logLines[i])
        }
    }

    function addLogEntry(message) {
        // 解析日志行，提取时间戳（如果有）
        var timestamp = ""
        var text = message
        
        // 尝试匹配时间戳格式，例如 "12:34:56 - ..."
        var match = message.match(/^(\d{2}:\d{2}:\d{2})(.*)/)
        if (match) {
            timestamp = match[1]
            text = match[2]
        }
        
        // 添加新日志条目到数组（添加到开头）
        logEntries.unshift({
            text: text,
            timestamp: timestamp,
            fullText: message
        })
        
        // 限制日志条目数量
        if (logEntries.length > maxLogEntries) {
            logEntries.pop()
        }
        
        // 更新模型（添加到开头）
        logModel.insert(0, {
            text: text,
            timestamp: timestamp,
            fullText: message
        })
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
            }
        }

        // 添加"请连接"文本
        Rectangle {
            width: parent.width - parent.padding * 2
            height: 30
            color: "#f0f0f0"
            radius: 3
            border.color: "#dddddd"
            border.width: 1
            
            TextEdit {
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: 8
                }
                text: qsTr("屏易连请连接：") + " " + LogManager.getLocalIpAddresses().join(" | ")
                font.pointSize: 10
                color: "#666666"
                readOnly: true
                selectByMouse: true
                selectedTextColor: "white"
                selectionColor: "#007acc"
            }
        }

        ListView {
            id: logListView
            width: parent.width - parent.padding * 2
            height: logPage.height - 150
            clip: true
            model: ListModel {
                id: logModel
            }
            
            // 添加滚动条
            ScrollBar.vertical: ScrollBar {
                active: true
                policy: ScrollBar.AsNeeded
                width: 8
                anchors.right: parent.right
                anchors.rightMargin: 1
                contentItem: Rectangle {
                    implicitWidth: 8
                    radius: width / 2
                    color: "#007acc"
                }
                background: Rectangle {
                    implicitWidth: 8
                    radius: width / 2
                    color: "#dddddd"
                }
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
                    
                    TextEdit {
                        text: timestamp
                        font.pointSize: 10
                        color: "#666666"
                        Layout.preferredWidth: 80
                        readOnly: true
                        selectByMouse: true
                        selectedTextColor: "white"
                        selectionColor: "#007acc"
                    }
                    
                    TextEdit {
                        id: logText
                        text: model.text
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        color: "#333333"
                        readOnly: true
                        selectByMouse: true
                        selectedTextColor: "white"
                        selectionColor: "#007acc"
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

        
        // 添加显示当前日志文件路径的标签
        Rectangle {
            width: parent.width - parent.padding * 2
            height: 30
            color: "#f5f5f5"
            radius: 3
            border.color: "#dddddd"
            border.width: 1
            
            TextEdit {
                anchors {
                    left: parent.left
                    right: parent.right
                    verticalCenter: parent.verticalCenter
                    margins: 8
                }
                text: qsTr("日志路径: ") + logFilePath
                font.pointSize: 9
                wrapMode: Text.Wrap
                color: "#666666"
                readOnly: true
                selectByMouse: true
                selectedTextColor: "white"
                selectionColor: "#007acc"
            }
        }
    }

    // 组件完成后初始化
    Component.onCompleted: {
        readLogFile()
    }
} 