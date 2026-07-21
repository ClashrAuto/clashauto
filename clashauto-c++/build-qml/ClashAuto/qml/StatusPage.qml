import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 状态页：左列 = 流量指标卡(2x2) + 实时带宽折线；右列 = 节点列表(搜索/测速/点击选择)。
Item {
    id: page

    RowLayout {
        anchors.fill: parent
        anchors.margins: 0 // 卡片内边距已由 StackLayout 提供 5px，这里不再叠加
        spacing: 10

        // —————————————————— 左列 ——————————————————
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.fillWidth: true
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 68 // 原 200 的约 1/3（卡片高度减 2/3）
                columns: 2
                rowSpacing: 6
                columnSpacing: 10

                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Layout.fillHeight: false
                    glyph: ""
                    title: qsTr("上传")
                    value: bridge.upText
                    accentColor: "#a84343"
                }
                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Layout.fillHeight: false
                    glyph: ""
                    title: qsTr("下载")
                    value: bridge.downText
                    accentColor: "#4da13e"
                }
                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Layout.fillHeight: false
                    glyph: ""
                    title: qsTr("进程数")
                    value: bridge.connectionsCount
                    accentColor: "#466ea8"

                    // 右上角：清空全部连接
                    Rectangle {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        width: 26
                        height: 22
                        radius: 4
                        color: Qt.rgba(1, 0, 0, 0.30)
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            color: "white"
                            font.pixelSize: 12
                        }
                        TapHandler { onTapped: bridge.clearConnections() }
                    }
                }
                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Layout.fillHeight: false
                    glyph: ""
                    title: qsTr("总下载")
                    value: bridge.totalDownText
                    accentColor: "#48a5a7"
                }
            }

            Text {
                text: qsTr("实时带宽")
                font.pixelSize: 18
                color: Theme.textPrimary
            }

            BandwidthChart {
                id: upChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: qsTr("上传")
                lineColor: "#b14a4a"
            }
            BandwidthChart {
                id: downChart
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: qsTr("下载")
                lineColor: "#5bb44b"
            }
        }

        // —————————————————— 右列 ——————————————————
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.fillWidth: true
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Text {
                    text: qsTr("节点")
                    font.pixelSize: 18
                    color: Theme.textPrimary
                }
                Text {
                    text: "(" + nodeModel.count + ")"
                    font.pixelSize: 10
                    color: Theme.textMuted
                    Layout.alignment: Qt.AlignBottom
                    bottomPadding: 3
                }
                Item { Layout.fillWidth: true }

                TextField {
                    id: search
                    Layout.preferredWidth: 160
                    placeholderText: qsTr("搜索节点")
                    color: Theme.textPrimary
                    placeholderTextColor: Theme.textMuted
                    font.pixelSize: 12
                    background: Rectangle {
                        radius: 3
                        color: Theme.inputBg
                        border.width: 1
                        border.color: search.activeFocus ? Theme.accent : Theme.inputBorder
                    }
                    onTextChanged: bridge.setNodeFilter(text)
                }

                Text {
                    text: bridge.speedTesting ? "⏳" : "↻"
                    font.pixelSize: 18
                    color: Theme.accent
                    ToolTip.text: qsTr("测速")
                    TapHandler {
                        enabled: !bridge.speedTesting
                        onTapped: bridge.runSpeedTest()
                    }
                }
            }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: nodeModel
                spacing: 5
                rightMargin: 10 // 行右缘离卡片右边 10px（滚动条悬浮）
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: NodeRow {
                    width: ListView.view.width - ListView.view.rightMargin
                    display: model.display
                    badgeText: model.badgeText
                    badgeColor: model.badgeColor
                    active: model.active
                    onApply: bridge.selectNode(model.rawName)
                }
            }
        }
    }

    // 把 bridge 的实时流量喂进两个带宽图
    Connections {
        target: bridge
        function onTrafficChanged() {
            upChart.push(bridge.upBytes);
            downChart.push(bridge.downBytes);
        }
    }
}
