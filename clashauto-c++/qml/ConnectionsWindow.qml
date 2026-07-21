import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 全部连接 —— 独立窗口（对齐旧版 QDialog 的显示方式）。进程卡「👁 查看」按钮 show() 打开。
// 可见时拉取并每 2s 刷新；每条 [type] host + 代理链/下载/上传 + 断开按钮；离线连接置灰。
ApplicationWindow {
    id: win
    width: 600
    height: 500
    minimumWidth: 420
    minimumHeight: 320
    title: qsTr("全部连接")
    color: Theme.card

    onVisibleChanged: {
        if (visible) {
            bridge.refreshConnections();
            refresh.start();
        } else {
            refresh.stop();
        }
    }

    Timer {
        id: refresh
        interval: 2000
        repeat: true
        onTriggered: bridge.refreshConnections()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("全部连接") + " (" + bridge.connections.length + ")"
                font.pixelSize: 16
                color: Theme.textPrimary
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("清空全部")
                onClicked: {
                    bridge.clearConnections();
                    bridge.refreshConnections();
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                anchors.fill: parent
                clip: true
                model: bridge.connections
                spacing: 5
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 50
                    radius: 4
                    color: modelData.offline ? Qt.rgba(0.5, 0.5, 0.5, 0.15) : Theme.nodeRowBg

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 8
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: "[" + modelData.type + "] " + modelData.host
                                color: modelData.offline ? Theme.textMuted : Theme.textPrimary
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.chain + "    ↓ " + modelData.download + "    ↑ " + modelData.upload
                                color: Theme.textMuted
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                        Rectangle {
                            visible: !modelData.offline
                            width: 48
                            height: 26
                            radius: 3
                            color: Qt.rgba(1, 0, 0, 0.25)
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("断开")
                                color: "white"
                                font.pixelSize: 11
                            }
                            TapHandler { onTapped: bridge.closeConnectionById(modelData.id) }
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: bridge.connections.length === 0
                text: qsTr("暂无连接")
                color: Theme.textMuted
                font.pixelSize: 13
            }
        }
    }
}
