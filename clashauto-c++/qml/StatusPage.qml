import QtQuick
import QtQuick.Layouts
import ClashAuto

// 状态页：四张放大的指标卡（2×2），整页只有这一块。
// 节点列表已独立成「节点」页（NodesPage），这里整页宽都归流量看板，卡片因此能画得下更多东西：
//   · 上传 / 下载：**实时带宽折线直接画在卡的背景上**（原来是页面下方两张独立的图，
//     占掉整整一屏高度还和上面的数字隔着老远——同一个指标的「此刻」和「最近 40 秒」该挨在一起）；
//   · 连接：数字下面直接列**最近建立的 5 条连接**（含发起它的设备），不用再开「全部连接」窗才知道在连谁；
//   · 总流量：直连 / 代理两段一根进度条对比，下面列**本会话用量最大的 5 个目标**。
// 后两张卡的数据来自 bridge.recentConnections / topConnections / directBytes / proxyBytes，
// 都是 QmlBridge::observeConnections 蹭历史库那次 /connections（2s 一发）算出来的，不额外发包。
//
// 私用区图标一律写 \uXXXX 转义、不写字面字符（见 DevicesPage.qml 顶部那条教训）。
Item {
    id: page

    // 两张列表卡共用的行样式：左边 host（吃掉剩余宽度、超长省略）+ 设备名，右边字节数。
    // 行内的 id 而不是 parent.xxx：Layout 会给子项换 parent，靠 parent 取属性是个随时会断的绑定。
    component ConnLine: RowLayout {
        id: line
        property string host: ""
        property string device: ""
        property string trailing: ""
        property bool direct: false
        Layout.fillWidth: true
        spacing: 8
        // 直连/代理的色点：一眼看出这条走没走代理，比再加一列文字省地方
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            width: 6; height: 6; radius: 3
            color: line.direct ? "#48a5a7" : Theme.accent
        }
        Text {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            text: line.host
            elide: Text.ElideRight
            font.pixelSize: 11
            color: Theme.textSecondary
        }
        Text {
            // 设备名定宽：不给的话长设备名会把右边的字节数挤走，几行之间对不齐
            Layout.preferredWidth: 96
            Layout.minimumWidth: 96
            horizontalAlignment: Text.AlignRight
            text: line.device
            elide: Text.ElideRight
            font.pixelSize: 10
            color: Theme.textMuted
        }
        Text {
            Layout.preferredWidth: 72
            Layout.minimumWidth: 72
            horizontalAlignment: Text.AlignRight
            text: line.trailing
            font.pixelSize: 10
            color: Theme.textMuted
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10 // 状态页主内容区内距 10（StackLayout 为 0，各页自管）
        spacing: 10

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 2
            rowSpacing: 10
            columnSpacing: 10

            // —— 上传 / 下载：实时带宽折线就画在卡的背景上 ——
            // 这两张卡吃掉窗口变高时多出来的高度（曲线越高越好读）；下面两张列表卡是定高的。
            MetricCard {
                id: upCard
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 92
                Layout.preferredHeight: 190
                glyph: "\uF24A" // upload-2-line
                title: qsTr("上传")
                value: bridge.upText
                accentColor: "#a84343"
                showChart: true
                chartColor: "#b14a4a" // 与原「实时带宽·上传」那条线同色
            }
            MetricCard {
                id: downCard
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 92
                Layout.preferredHeight: 190
                glyph: "\uEC54" // download-2-line
                title: qsTr("下载")
                value: bridge.downText
                accentColor: "#4da13e"
                showChart: true
                chartColor: "#5bb44b"
            }

            // —— 连接：数字 + 最近建立的 5 条 ——
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 208
                Layout.maximumHeight: 260
                radius: 4
                color: Theme.metricBg
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Text {
                            text: "\uEEB8" // pulse / 进程数（沿用原卡图标）
                            font.family: Theme.riFont
                            font.pixelSize: 28
                            color: Theme.dark ? "#aaaaaa" : "#888888"
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 3
                            Text { text: qsTr("连接"); font.pixelSize: 13; color: "#466ea8" }
                            Text { text: bridge.connectionsCount; font.pixelSize: 24; color: "#466ea8" }
                        }
                        // 右上角按钮组：eye 查看全部连接（开独立窗口）| delete-bin 清空全部连接
                        Rectangle {
                            Layout.alignment: Qt.AlignTop
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
                                        text: "\uECB5" // eye-line
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
                                        text: "\uEC2A" // delete-bin-line
                                        font.family: Theme.riFont
                                        color: "#ff6b6b"
                                        font.pixelSize: 14
                                    }
                                    TapHandler { onTapped: bridge.clearConnections() }
                                }
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                    Text {
                        text: qsTr("最近连接")
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }
                    // 最近建立的 5 条（含发起设备）。**行数固定 5**：条数随流量增减的话，卡片
                    // 高度会跟着一秒一变，整页跟着抖；不足 5 条就留空行。
                    Repeater {
                        model: 5
                        delegate: ConnLine {
                            required property int index
                            readonly property var e: bridge.recentConnections[index]
                            visible: e !== undefined
                            host: e ? (e.host || "-") : ""
                            device: e ? e.device : ""
                            direct: e ? e.direct : false
                            trailing: e ? Theme.fmtBytes(e.bytes) : ""
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        visible: bridge.recentConnections.length === 0
                        text: qsTr("暂无连接")
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }
                }
            }

            // —— 总流量：直连 / 代理对比条 + 用量最大的 5 个目标 ——
            Rectangle {
                id: totalCard
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 208
                Layout.maximumHeight: 260
                radius: 4
                color: Theme.metricBg
                clip: true

                readonly property color directColor: "#48a5a7"
                readonly property real total: bridge.directBytes + bridge.proxyBytes
                // 两段各占多少：一个字节都没跑的时候画一根空槽，别让 0/0 变成 NaN 宽度
                readonly property real directRatio: total > 0 ? bridge.directBytes / total : 0

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        Text {
                            text: "\uECAD" // exchange-line
                            font.family: Theme.riFont
                            font.pixelSize: 28
                            color: Theme.dark ? "#aaaaaa" : "#888888"
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            spacing: 3
                            Text { text: qsTr("总流量"); font.pixelSize: 13; color: "#48a5a7" }
                            Text { text: bridge.totalText; font.pixelSize: 24; color: "#48a5a7"
                                   elide: Text.ElideRight; Layout.fillWidth: true }
                        }
                    }

                    // 直连 / 代理对比条：一根槽里两段，宽度即占比。
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 8
                        radius: 4
                        color: Theme.dark ? "#1c1c1c" : "#dddddd"
                        Rectangle {
                            id: directSeg
                            height: parent.height
                            width: parent.width * totalCard.directRatio
                            radius: 4
                            color: totalCard.directColor
                            Behavior on width { NumberAnimation { duration: 200 } }
                        }
                        Rectangle {
                            anchors.left: directSeg.right
                            anchors.right: parent.right
                            height: parent.height
                            radius: 4
                            color: Theme.accent
                            visible: totalCard.total > 0
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Rectangle { width: 6; height: 6; radius: 3; color: totalCard.directColor
                                    Layout.alignment: Qt.AlignVCenter }
                        // 分隔的空格写在拼接里、不写进 qsTr——带尾空格的源串是个单独的翻译键，
                        // 12 份表里全都要为它单独补一条，纯属自找麻烦。
                        Text { text: qsTr("直连") + " " + bridge.directText; font.pixelSize: 10
                               color: Theme.textMuted }
                        Item { Layout.fillWidth: true }
                        Rectangle { width: 6; height: 6; radius: 3; color: Theme.accent
                                    Layout.alignment: Qt.AlignVCenter }
                        Text { text: qsTr("代理") + " " + bridge.proxyText; font.pixelSize: 10
                               color: Theme.textMuted }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                    Text {
                        text: qsTr("用量最多")
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }
                    Repeater {
                        model: 5
                        delegate: ConnLine {
                            required property int index
                            readonly property var e: bridge.topConnections[index]
                            visible: e !== undefined
                            host: e ? (e.host || "-") : ""
                            device: e ? e.device : ""
                            direct: e ? e.direct : false
                            trailing: e ? Theme.fmtBytes(e.bytes) : ""
                        }
                    }
                    Item { Layout.fillHeight: true }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        visible: bridge.topConnections.length === 0
                        text: qsTr("暂无流量")
                        font.pixelSize: 11
                        color: Theme.textMuted
                    }
                }
            }
        }

    }

    // 把 bridge 的实时流量喂进两张卡的背景折线
    Connections {
        target: bridge
        function onTrafficChanged() {
            upCard.pushChart(bridge.upBytes);
            downCard.pushChart(bridge.downBytes);
        }
    }

    // 全部连接：独立窗口（对齐旧版 QDialog 的显示方式），由连接卡「查看」按钮打开。
    ConnectionsWindow { id: connWindow }
}
