import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

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
    property int maxLogEntries: 100

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

                Button {
                    text: qsTr("清除")
                    onClicked: clearLogs()
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

    // 示例：添加一些初始日志
    Component.onCompleted: {
        addLogEntry("应用程序启动")
    }
} 