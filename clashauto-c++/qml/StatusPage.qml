import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 状态页：左列 = 流量指标卡(2x2) + 实时带宽折线；右列 = 节点列表(搜索/测速/点击选择)。
Item {
    id: page

    // 节点搜索框是否展开（对齐旧项目 searchBox.show：默认收起，点放大镜展开）
    property bool searchShown: false

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10 // 状态页主内容区内距 10（StackLayout 为 0，各页自管）
        spacing: 10

        // —————————————————— 左列 ——————————————————
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            Layout.fillWidth: true
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 170 // 2 行 × 60px 卡 + 行距
                columns: 2
                rowSpacing: 6
                columnSpacing: 10

                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    Layout.fillHeight: false
                    glyph: "\uF24A"
                    title: qsTr("上传")
                    value: bridge.upText
                    accentColor: "#a84343"
                }
                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    Layout.fillHeight: false
                    glyph: "\uEC54"
                    title: qsTr("下载")
                    value: bridge.downText
                    accentColor: "#4da13e"
                }
                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    Layout.fillHeight: false
                    glyph: "\uEEB8"
                    title: qsTr("进程数")
                    value: bridge.connectionsCount
                    accentColor: "#466ea8"

                    // 右上角按钮组（Remix 图标）：eye 查看全部连接（开独立窗口）| delete-bin 清空全部连接
                    Rectangle {
                        anchors.top: parent.top
                        anchors.right: parent.right
                        width: 52
                        height: 22
                        radius: 4
                        clip: true
                        color: Qt.rgba(0, 0, 0, 0.32)

                        Row {
                            anchors.fill: parent
                            Item {
                                width: 26
                                height: parent.height
                                Text {
                                    anchors.centerIn: parent
                                    text: "" // eye-line
                                    font.family: Theme.riFont
                                    font.pixelSize: 14
                                    color: "#ffffff"
                                }
                                TapHandler {
                                    onTapped: {
                                        connWindow.show();
                                        connWindow.raise();
                                        connWindow.requestActivate();
                                    }
                                }
                            }
                            Rectangle { width: 1; height: parent.height; color: Qt.rgba(1, 1, 1, 0.15) }
                            Item {
                                width: 25
                                height: parent.height
                                Text {
                                    anchors.centerIn: parent
                                    text: "" // delete-bin-line
                                    font.family: Theme.riFont
                                    color: "#ff6b6b"
                                    font.pixelSize: 14
                                }
                                TapHandler { onTapped: bridge.clearConnections() }
                            }
                        }
                    }
                }
                MetricCard {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 80
                    Layout.fillHeight: false
                    glyph: "\uEC56"
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

            // 节点区顶栏（严格参考旧项目 status.vue：节点+数量 | 可展开搜索 | 测速 refresh/loading | 帮助 question→文档）
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 30 // 固定高度：搜索框(28)展开时不再撑高整行
                spacing: 6

                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("节点")
                    font.pixelSize: 18
                    color: Theme.textPrimary
                }
                Text {
                    text: "(" + nodeModel.count + ")"
                    font.pixelSize: 9
                    color: Theme.textMuted
                    Layout.alignment: Qt.AlignVCenter
                }

                // 搜索：默认只显示放大镜，点击展开输入框（右侧 ✕ 清空并收起）
                Text {
                    visible: !page.searchShown
                    text: "" // search-line
                    font.family: Theme.riFont
                    font.pixelSize: 16
                    color: Theme.textMuted
                    Layout.leftMargin: 4
                    Layout.alignment: Qt.AlignVCenter
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: { page.searchShown = true; search.forceActiveFocus() } }
                }
                TextField {
                    id: search
                    visible: page.searchShown
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: 28
                    rightPadding: 24
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
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 7
                        anchors.verticalCenter: parent.verticalCenter
                        text: "" // close-line
                        font.family: Theme.riFont
                        font.pixelSize: 14
                        color: Theme.textMuted
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                search.text = "";
                                bridge.setNodeFilter("");
                                page.searchShown = false;
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true; visible: !page.searchShown }

                // 测速：空闲 refresh-line、测速中 loader-4-line 旋转（对齐旧项目 refresh-right / loading）
                Text {
                    id: spdIcon
                    Layout.alignment: Qt.AlignVCenter
                    property real spin: 0
                    text: bridge.speedTesting ? "" : ""
                    font.family: Theme.riFont
                    font.pixelSize: 19
                    color: Theme.accent
                    rotation: bridge.speedTesting ? spin : 0
                    NumberAnimation on spin {
                        running: bridge.speedTesting
                        from: 0; to: 360; duration: 900; loops: Animation.Infinite
                    }
                    HoverHandler { id: spdHover; cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: spdHover.hovered
                    ToolTip.text: qsTr("测速")
                    TapHandler {
                        enabled: !bridge.speedTesting
                        onTapped: bridge.runSpeedTest()
                    }
                }

                // 帮助：打开在线文档（对齐旧项目 question → gitbook）
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: 5
                    text: "" // question-fill
                    font.family: Theme.riFont
                    font.pixelSize: 18
                    color: Theme.textMuted
                    HoverHandler { id: qHover; cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: qHover.hovered
                    ToolTip.text: qsTr("帮助")
                    TapHandler { onTapped: Qt.openUrlExternally("https://clashr-auto.gitbook.io/") }
                }
            }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: nodeModel
                spacing: 1 // 节点行间距紧凑为 1px
                rightMargin: 4 // QML 滚动条是悬浮式、不占宽；这里只留 4px 小边距，行更靠右
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: NodeRow {
                    width: ListView.view.width - ListView.view.rightMargin
                    display: model.display
                    badgeText: model.badgeText
                    badgeColor: model.badgeColor
                    active: model.active
                    // 切换加载态：目标行转圈、其余灰化禁用（对齐旧项目 createNodeRow 的 disableLoading）
                    switching: bridge.switching
                    isTarget: bridge.switching && model.rawName === bridge.switchTarget
                    spinnerText: bridge.spinnerGlyph
                    // 应用/禁用都先进入切换加载态，再调用后端（严格对齐旧项目 createNodeRow：beginNodeSwitch 在前）
                    onApply: {
                        bridge.beginNodeSwitch(model.rawName)
                        bridge.selectNode(model.rawName)
                    }
                    onDisable: {
                        bridge.beginNodeSwitch(model.rawName)
                        bridge.disableNode(model.rawName, model.rawNow)
                        subs.reload() // 刷新订阅页，反映刚禁用的节点（对齐旧项目 disableNodeByName→reloadSubscriptions）
                    }
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

    // 全部连接：独立窗口（对齐旧版 QDialog 的显示方式），由进程卡「查看」按钮打开。
    ConnectionsWindow { id: connWindow }
}
