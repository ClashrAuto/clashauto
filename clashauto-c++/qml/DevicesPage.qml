import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 设备页：概览条 + 左设备列表(发现/热更新) + 右详情(信息/实时流量/策略/连接)。
// 页面显隐驱动 devices.setActive → 控制扫描/热更新/连接轮询的开停。
Item {
    id: page
    onVisibleChanged: devices.setActive(visible)

    // 本地化：类型 key → 名称、策略模式 key → 名称。
    function typeName(k) {
        switch (k) {
        case "phone": return qsTr("手机");
        case "tablet": return qsTr("平板");
        case "computer": return qsTr("电脑");
        case "router": return qsTr("路由器");
        case "tvbox": return qsTr("电视/盒子");
        case "speaker": return qsTr("音箱");
        case "printer": return qsTr("打印机");
        case "camera": return qsTr("摄像头");
        case "game": return qsTr("游戏机");
        case "nas": return qsTr("存储/NAS");
        case "iot": return qsTr("智能设备");
        default: return qsTr("未知设备");
        }
    }
    readonly property var modeKeys: ["follow", "rule", "global", "direct", "reject"]
    function modeName(k) {
        switch (k) {
        case "rule": return qsTr("规则分流");
        case "global": return qsTr("指定节点");
        case "direct": return qsTr("强制直连");
        case "reject": return qsTr("禁止上网");
        default: return qsTr("跟随全局");
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        // —————————————————— 概览条 ——————————————————
        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Text { text: qsTr("设备"); font.pixelSize: 18; color: Theme.textPrimary }
            Row {
                spacing: 4
                Text { text: qsTr("在线"); font.pixelSize: 12; color: Theme.textMuted }
                Text { text: devices.onlineCount + "/" + devices.deviceCount
                       font.pixelSize: 12; color: Theme.textSecondary }
            }
            Row {
                spacing: 4
                Text { text: qsTr("代理中"); font.pixelSize: 12; color: Theme.textMuted }
                Text { text: devices.proxiedCount; font.pixelSize: 12; color: Theme.accent }
            }
            Row {
                spacing: 8
                Text { text: "↓ " + Theme.fmtRate(devices.totalRateDown)
                       font.pixelSize: 12; color: "#5bb44b" }
                Text { text: "↑ " + Theme.fmtRate(devices.totalRateUp)
                       font.pixelSize: 12; color: "#b14a4a" }
            }
            Item { Layout.fillWidth: true }
            Text { text: qsTr("网关 ") + (devices.gatewayIp || "-")
                   font.pixelSize: 11; color: Theme.textMuted }
        }

        // —————————————————— 主体：左列表 + 右详情 ——————————————————
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // —————————— 左：设备列表 ——————————
            ColumnLayout {
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: search
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        placeholderText: qsTr("搜索设备 / IP / 厂商")
                        color: Theme.textPrimary
                        placeholderTextColor: Theme.textMuted
                        font.pixelSize: 12
                        background: Rectangle {
                            radius: 3; color: Theme.inputBg
                            border.width: 1
                            border.color: search.activeFocus ? Theme.accent : Theme.inputBorder
                        }
                        onTextChanged: devices.model.setFilter(text, onlineOnly.checked)
                    }
                    Rectangle {
                        id: onlineOnly
                        property bool checked: false
                        width: 28; height: 28; radius: 3
                        color: checked ? Theme.accent : Theme.inputBg
                        border.width: 1
                        border.color: checked ? Theme.accent : Theme.inputBorder
                        Text { anchors.centerIn: parent; text: "●"; font.pixelSize: 12
                               color: onlineOnly.checked ? "#ffffff" : Theme.textMuted }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                onlineOnly.checked = !onlineOnly.checked;
                                devices.model.setFilter(search.text, onlineOnly.checked);
                            }
                        }
                    }
                    Rectangle {
                        width: 28; height: 28; radius: 3
                        color: scanHover.hovered ? Theme.hover : Theme.inputBg
                        border.width: 1; border.color: Theme.inputBorder
                        Text {
                            anchors.centerIn: parent
                            text: "" // refresh-line
                            font.family: Theme.riFont
                            font.pixelSize: 15
                            color: Theme.accent
                            NumberAnimation on rotation {
                                running: devices.scanning
                                from: 0; to: 360; duration: 900; loops: Animation.Infinite
                            }
                        }
                        HoverHandler { id: scanHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { enabled: !devices.scanning; onTapped: devices.scan() }
                    }
                }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: devices.model
                    spacing: 4
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    delegate: DeviceRow {
                        width: ListView.view.width
                        mac: model.mac
                        ip: model.ip
                        name: model.name
                        typeKey: model.typeKey
                        vendor: model.vendor
                        online: model.online
                        proxied: model.proxied
                        rateUp: model.rateUp
                        rateDown: model.rateDown
                        isSelf: model.isSelf
                        isGateway: model.isGateway
                        selected: devices.selectedMac === model.mac
                        onClicked: devices.select(model.mac)
                        onToggleProxy: devices.setProxyEnabled(model.mac, !model.proxied)
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: devices.model.count === 0
                        text: devices.scanning ? qsTr("正在扫描局域网…") : qsTr("未发现设备")
                        font.pixelSize: 12; color: Theme.textMuted
                    }
                }
            }

            Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.divider }

            // —————————— 右：详情 ——————————
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Text {
                    anchors.centerIn: parent
                    visible: devices.selectedMac === ""
                    text: qsTr("从左侧选择设备查看详情")
                    font.pixelSize: 13; color: Theme.textMuted
                }

                ScrollView {
                    anchors.fill: parent
                    visible: devices.selectedMac !== ""
                    clip: true
                    contentWidth: availableWidth
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                    ColumnLayout {
                        id: detailCol
                        width: parent.width
                        spacing: 12
                        // 单一数据源：所有子项都引用 detailCol.dev.*（document-scoped id，稳当不受 Layout 重父影响）。
                        readonly property var dev: devices.selectedDevice

                        // —— 头部 ——
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                Layout.preferredWidth: 48; Layout.preferredHeight: 48
                                radius: 10
                                color: Theme.deviceColor(detailCol.dev.typeKey || "unknown")
                                Text { anchors.centerIn: parent
                                       text: Theme.deviceGlyph(detailCol.dev.typeKey || "unknown")
                                       font.pixelSize: 22; color: "#ffffff" }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: detailCol.dev.name || ""
                                    font.pixelSize: 18; font.bold: true
                                    color: Theme.textPrimary
                                    elide: Text.ElideRight; Layout.fillWidth: true
                                }
                                Text {
                                    text: page.typeName(detailCol.dev.typeKey || "unknown")
                                          + "  ·  " + (detailCol.dev.online ? qsTr("在线") : qsTr("离线"))
                                    font.pixelSize: 12; color: Theme.textMuted
                                }
                            }
                            ColumnLayout {
                                visible: detailCol.dev.protectable === true
                                spacing: 2
                                Rectangle {
                                    Layout.alignment: Qt.AlignRight
                                    width: 52; height: 26; radius: 13
                                    color: detailCol.dev.proxyEnabled ? Theme.accent : Theme.switchTrackOff
                                    Behavior on color { ColorAnimation { duration: 120 } }
                                    Rectangle {
                                        width: 22; height: 22; radius: 11; y: 2
                                        x: detailCol.dev.proxyEnabled ? parent.width - width - 2 : 2
                                        color: "#ffffff"
                                        Behavior on x { NumberAnimation { duration: 120 } }
                                    }
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler {
                                        onTapped: devices.setProxyEnabled(devices.selectedMac,
                                                                          !detailCol.dev.proxyEnabled)
                                    }
                                }
                                Text { Layout.alignment: Qt.AlignRight; text: qsTr("代理网络")
                                       font.pixelSize: 10; color: Theme.textMuted }
                            }
                        }

                        // M0 诚实提示
                        Rectangle {
                            Layout.fillWidth: true
                            visible: detailCol.dev.proxyEnabled === true && !devices.gatewayReady
                            radius: 5
                            color: Qt.rgba(200 / 255, 154 / 255, 84 / 255, 0.15)
                            implicitHeight: hintTxt.implicitHeight + 14
                            Text {
                                id: hintTxt
                                anchors.fill: parent
                                anchors.margins: 7
                                wrapMode: Text.WordWrap
                                text: qsTr("已标记代理此设备；透明网关模块启用后将自动接管其流量。")
                                font.pixelSize: 11; color: "#c69a54"
                            }
                        }

                        // —— 信息网格 ——（用 Repeater 铺 2 列，避免嵌套内联组件的限制）
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 24
                            rowSpacing: 6
                            Repeater {
                                model: [
                                    { k: qsTr("IP"), v: detailCol.dev.ip || "" },
                                    { k: qsTr("MAC"), v: detailCol.dev.mac || "" },
                                    { k: qsTr("厂商"), v: detailCol.dev.vendor || "" },
                                    { k: qsTr("型号"), v: detailCol.dev.model || "" },
                                    { k: qsTr("首次发现"), v: detailCol.dev.firstSeen || "" },
                                    { k: qsTr("主机名"), v: detailCol.dev.autoName || "" },
                                ]
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text { text: modelData.k; font.pixelSize: 12; color: Theme.textMuted
                                           Layout.preferredWidth: 64 }
                                    Text { text: modelData.v || "-"; font.pixelSize: 12
                                           color: Theme.textSecondary
                                           elide: Text.ElideRight; Layout.fillWidth: true }
                                }
                            }
                        }

                        // 备注名编辑
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Text { text: qsTr("备注名"); font.pixelSize: 12; color: Theme.textMuted
                                   Layout.preferredWidth: 64 }
                            TextField {
                                id: aliasField
                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                text: detailCol.dev.alias || ""
                                placeholderText: qsTr("为该设备起个名字")
                                color: Theme.textPrimary
                                placeholderTextColor: Theme.textMuted
                                font.pixelSize: 12
                                background: Rectangle {
                                    radius: 3; color: Theme.inputBg
                                    border.width: 1
                                    border.color: aliasField.activeFocus ? Theme.accent : Theme.inputBorder
                                }
                                onEditingFinished: devices.setAlias(devices.selectedMac, text)
                            }
                        }

                        // —— 实时流量卡 ——
                        Rectangle {
                            Layout.fillWidth: true
                            radius: 6
                            color: Theme.metricBg
                            implicitHeight: trafCol.implicitHeight + 20
                            ColumnLayout {
                                id: trafCol
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: qsTr("实时流量"); font.pixelSize: 14
                                           color: Theme.textPrimary; Layout.fillWidth: true }
                                    Text { text: "↓ " + Theme.fmtRate(detailCol.dev.rateDown)
                                           font.pixelSize: 13; color: "#5bb44b" }
                                    Text { text: "  ↑ " + Theme.fmtRate(detailCol.dev.rateUp)
                                           font.pixelSize: 13; color: "#b14a4a" }
                                }
                                BandwidthChart {
                                    id: devChart
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 90
                                    title: qsTr("下载")
                                    lineColor: "#5bb44b"
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16
                                    Column {
                                        Text { text: qsTr("本次会话"); font.pixelSize: 10; color: Theme.textMuted }
                                        Text { text: "↓" + Theme.fmtBytes(detailCol.dev.sessionDown)
                                                     + "  ↑" + Theme.fmtBytes(detailCol.dev.sessionUp)
                                               font.pixelSize: 12; color: Theme.textSecondary }
                                    }
                                    Column {
                                        Text { text: qsTr("今日"); font.pixelSize: 10; color: Theme.textMuted }
                                        Text { text: "↓" + Theme.fmtBytes(detailCol.dev.todayDown)
                                                     + "  ↑" + Theme.fmtBytes(detailCol.dev.todayUp)
                                               font.pixelSize: 12; color: Theme.textSecondary }
                                    }
                                    Column {
                                        Text { text: qsTr("累计"); font.pixelSize: 10; color: Theme.textMuted }
                                        Text { text: "↓" + Theme.fmtBytes(detailCol.dev.totalDown)
                                                     + "  ↑" + Theme.fmtBytes(detailCol.dev.totalUp)
                                               font.pixelSize: 12; color: Theme.textSecondary }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }

                        // —— 策略 ——
                        RowLayout {
                            Layout.fillWidth: true
                            visible: detailCol.dev.protectable === true
                            spacing: 8
                            Text { text: qsTr("策略"); font.pixelSize: 12; color: Theme.textMuted
                                   Layout.preferredWidth: 64 }
                            ComboBox {
                                id: modeCombo
                                Layout.preferredWidth: 130
                                Layout.preferredHeight: 28
                                font.pixelSize: 12
                                model: page.modeKeys.map(function (k) { return page.modeName(k); })
                                currentIndex: page.modeKeys.indexOf(detailCol.dev.policyMode || "follow")
                                onActivated: devices.setPolicy(devices.selectedMac,
                                                               page.modeKeys[currentIndex],
                                                               page.modeKeys[currentIndex] === "global"
                                                               ? targetCombo.currentText : "")
                            }
                            ComboBox {
                                id: targetCombo
                                visible: page.modeKeys[modeCombo.currentIndex] === "global"
                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                font.pixelSize: 12
                                model: bridge.groups
                                onActivated: devices.setPolicy(devices.selectedMac, "global", currentText)
                            }
                            Item { Layout.fillWidth: true
                                   visible: page.modeKeys[modeCombo.currentIndex] !== "global" }
                        }

                        // —— 该设备连接列表（实时热更新）——
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: qsTr("连接"); font.pixelSize: 14; color: Theme.textPrimary }
                            Text { text: "(" + devices.connModel.count + ")"; font.pixelSize: 10
                                   color: Theme.textMuted }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: qsTr("全部断开")
                                font.pixelSize: 11; color: "#ff6b6b"
                                visible: devices.connModel.count > 0
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: devices.closeDeviceConnections(devices.selectedMac) }
                            }
                        }
                        ListView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Math.min(contentHeight, 220)
                            clip: true
                            interactive: contentHeight > height
                            model: devices.connModel
                            spacing: 2
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            delegate: Rectangle {
                                width: ListView.view.width
                                height: 34
                                radius: 4
                                color: Theme.nodeRowBg
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 8
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Text { text: model.host; font.pixelSize: 11
                                               color: Theme.textSecondary; elide: Text.ElideRight
                                               Layout.fillWidth: true }
                                        Text { text: model.type + "  ·  " + model.chain
                                               font.pixelSize: 9; color: Theme.textMuted
                                               elide: Text.ElideRight; Layout.fillWidth: true }
                                    }
                                    Text { text: "↓" + Theme.fmtBytes(model.download)
                                           font.pixelSize: 10; color: "#5bb44b" }
                                    Text { text: "↑" + Theme.fmtBytes(model.upload)
                                           font.pixelSize: 10; color: "#b14a4a" }
                                    Text {
                                        text: "" // close-fill
                                        font.family: Theme.riFont
                                        font.pixelSize: 12; color: Theme.textMuted
                                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                                        TapHandler { onTapped: bridge.closeConnectionById(model.connId) }
                                    }
                                }
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: devices.connModel.count === 0
                                text: qsTr("暂无经代理的连接")
                                font.pixelSize: 11; color: Theme.textMuted
                            }
                        }

                        Item { Layout.preferredHeight: 4 }
                    }
                }
            }
        }
    }

    // 选中设备的实时下载速率喂进详情带宽图（selectedChanged 每拍触发）。
    Connections {
        target: devices
        function onSelectedChanged() {
            if (devices.selectedMac !== "")
                devChart.push(devices.selectedDevice.rateDown || 0);
        }
    }
}
