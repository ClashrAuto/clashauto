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
    // 类型覆盖下拉：unknown = 自动识别，其余为手动指定。
    readonly property var typeKeys: ["unknown", "phone", "tablet", "computer", "router",
                                     "tvbox", "speaker", "printer", "camera", "game", "nas", "iot"]
    function typeOverrideName(k) { return k === "unknown" ? qsTr("自动识别") : typeName(k); }
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
            // 新设备提醒（蹭网检测）开关
            Row {
                spacing: 5
                Text { text: qsTr("新设备提醒"); font.pixelSize: 11; color: Theme.textMuted
                       anchors.verticalCenter: parent.verticalCenter }
                Rectangle {
                    width: 34; height: 18; radius: 9
                    anchors.verticalCenter: parent.verticalCenter
                    color: devices.newDeviceAlert ? Theme.accent : Theme.switchTrackOff
                    Behavior on color { ColorAnimation { duration: 120 } }
                    Rectangle {
                        width: 14; height: 14; radius: 7; y: 2
                        x: devices.newDeviceAlert ? parent.width - width - 2 : 2
                        color: "#ffffff"
                        Behavior on x { NumberAnimation { duration: 120 } }
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: devices.newDeviceAlert = !devices.newDeviceAlert }
                }
            }
            // 导出 CSV
            Text {
                text: qsTr("导出")
                font.pixelSize: 11; color: Theme.accent
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: devices.exportCsv() }
            }
            Text { text: qsTr("网关 ") + (devices.gatewayIp || "-")
                   font.pixelSize: 11; color: Theme.textMuted }
        }

        // ———————— Npcap 提示条（仅 Windows；未装 **或** 装了却被锁成仅管理员时显示）————————
        // 没有它透明网关根本起不来，而程序对 wpcap.dll 是延迟加载 —— 不提示的话用户只会看到
        // 「开关打开了但没生效」。两种病因两种解法：
        //   · 没装 → 打开安装窗（下载 + 验签 + 静默安装，全自动）；
        //   · 装了但驱动被限制为「仅管理员可访问」（Npcap 向导默认勾选，这是「打开网卡失败：
        //     拒绝访问」的实际原因）→ 一键提权修复，不用重装也不用每次以管理员身份启动。
        Rectangle {
            Layout.fillWidth: true
            visible: npcap.supported && (!npcap.installed || npcap.restricted)
            radius: 5
            color: Qt.rgba(198 / 255, 154 / 255, 84 / 255, 0.15)
            border.width: 1
            border.color: Qt.rgba(198 / 255, 154 / 255, 84 / 255, 0.35)
            implicitHeight: npcapRow.implicitHeight + 16

            RowLayout {
                id: npcapRow
                anchors.fill: parent
                anchors.margins: 8
                spacing: 10

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: "#c69a54"
                    text: npcap.restricted
                          ? qsTr("Npcap 驱动被限制为「仅管理员可访问」—— 「代理网络」会一直报"
                                 + "「打开网卡失败：拒绝访问」。点右侧一键解除（弹一次 UAC）。")
                          : qsTr("未安装 Npcap 驱动 —— 「代理网络」开关不会真正接管设备流量。")
                }
                Rectangle {
                    // 随文本伸缩：「安装 Npcap」/「修复权限」的英德俄译文比中文长不少，
                    // 定宽会把字撑到按钮外（自绘 Rectangle，没有 Button 的 elide 兜底）。
                    Layout.preferredWidth: Math.max(96, npcapBtnTxt.implicitWidth + 20)
                    Layout.preferredHeight: 26
                    radius: 4
                    enabled: !npcap.busy
                    opacity: enabled ? 1.0 : 0.5
                    color: npcapBtnHover.hovered && enabled ? Theme.accentStrong : Theme.accent
                    Text {
                        id: npcapBtnTxt
                        anchors.centerIn: parent
                        text: npcap.busy ? qsTr("处理中…")
                                         : npcap.restricted ? qsTr("修复权限") : qsTr("安装 Npcap")
                        font.pixelSize: 11
                        color: "#ffffff"
                    }
                    HoverHandler {
                        id: npcapBtnHover
                        cursorShape: parent.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                    TapHandler {
                        enabled: !npcap.busy
                        onTapped: npcap.restricted ? npcap.fixPermissions() : npcapWindow.show()
                    }
                }
            }
        }

        // —————————————————— 主体：左列表 + 右详情 ——————————————————
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // —————————— 左：设备列表 ——————————
            // 左右各占一半：两侧必须给同样的 fillWidth + preferredWidth 权重。
            // （曾经左 preferredWidth:320 / 右不给 —— Layouts 在未设 stretchFactor 时按
            //  preferredWidth 的比例分配剩余空间，右侧权重 0 → 永远 0 宽，怎么拉窗口都看不见第二栏。）
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.minimumWidth: 200
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
                        proxyable: model.proxyable
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
                Layout.preferredWidth: 1 // 与左列表同权重 → 平分宽度
                Layout.minimumWidth: 200
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
                                       font.family: Theme.riFont // 图标字体（默认 MiSans 里这些码点是空的）
                                       font.pixelSize: 26; color: "#ffffff" }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    text: detailCol.dev.name || ""
                                    font.pixelSize: 18
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
                                id: proxyBox
                                // 已经开着的一律可见可关（离线/跨网段也得能撤销）；关着的只有
                                // 「可代理且在线」才点得动（离线设备拿不到 IP/ARP，劫持无从下手）。
                                visible: detailCol.dev.proxyable === true
                                         || detailCol.dev.proxyEnabled === true
                                spacing: 2
                                readonly property bool canToggle:
                                    detailCol.dev.proxyEnabled === true
                                    || (detailCol.dev.proxyable === true
                                        && detailCol.dev.online === true)
                                Rectangle {
                                    Layout.alignment: Qt.AlignRight
                                    width: 52; height: 26; radius: 13
                                    opacity: proxyBox.canToggle ? 1.0 : 0.4
                                    color: detailCol.dev.proxyEnabled ? Theme.accent : Theme.switchTrackOff
                                    Behavior on color { ColorAnimation { duration: 120 } }
                                    Rectangle {
                                        width: 22; height: 22; radius: 11; y: 2
                                        x: detailCol.dev.proxyEnabled ? parent.width - width - 2 : 2
                                        color: "#ffffff"
                                        Behavior on x { NumberAnimation { duration: 120 } }
                                    }
                                    HoverHandler {
                                        cursorShape: proxyBox.canToggle ? Qt.PointingHandCursor
                                                                        : Qt.ForbiddenCursor
                                    }
                                    TapHandler {
                                        enabled: proxyBox.canToggle
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

                        // 「为什么不能开代理」——本机/网关/跨网段/离线各给一句人话。
                        Rectangle {
                            Layout.fillWidth: true
                            visible: blockTxt.text !== ""
                            radius: 5
                            color: Qt.rgba(1, 1, 1, 0.06)
                            implicitHeight: blockTxt.implicitHeight + 14
                            Text {
                                id: blockTxt
                                anchors.fill: parent
                                anchors.margins: 7
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11; color: Theme.textMuted
                                text: {
                                    switch (detailCol.dev.proxyBlockReason || "") {
                                    case "self":
                                        return qsTr("这是本机——它的流量已经由 Coast 自己接管，无需（也不能）代理。");
                                    case "gateway":
                                        return qsTr("这是当前网络的路由器（网关），劫持它会把整个网络打瘫，因此不可代理。");
                                    case "foreign":
                                        return qsTr("该设备不在本机任何一张网卡的网段内，透明网关只能劫持"
                                                    + "与本机同网段的设备，因此不可代理。（有线和 Wi-Fi 各接"
                                                    + "一个路由时，两边的网段都算「同网段」。）");
                                    case "offline":
                                        return qsTr("设备当前离线：拿不到它的 IP/ARP，无法开启代理；等它上线后再开。"
                                                    + "（已经开着的代理随时可以关掉。）");
                                    default:
                                        return "";
                                    }
                                }
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

                        // 手动改类型（覆盖自动识别）
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Text { text: qsTr("类型"); font.pixelSize: 12; color: Theme.textMuted
                                   Layout.preferredWidth: 64 }
                            ThemedCombo { // 与设置页/规则编辑器同一个下拉组件（别退回裸 ComboBox）
                                id: typeCombo
                                Layout.preferredWidth: 150
                                model: page.typeKeys.map(function (k) { return page.typeOverrideName(k); })
                                currentIndex: Math.max(0, page.typeKeys.indexOf(detailCol.dev.typeOverride || "unknown"))
                                onActivated: devices.setTypeOverride(devices.selectedMac, page.typeKeys[currentIndex])
                            }
                            Item { Layout.fillWidth: true }
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
                            visible: detailCol.dev.proxyable === true
                            spacing: 8
                            Text { text: qsTr("策略"); font.pixelSize: 12; color: Theme.textMuted
                                   Layout.preferredWidth: 64 }
                            ThemedCombo {
                                id: modeCombo
                                Layout.preferredWidth: 130
                                model: page.modeKeys.map(function (k) { return page.modeName(k); })
                                currentIndex: page.modeKeys.indexOf(detailCol.dev.policyMode || "follow")
                                onActivated: devices.setPolicy(devices.selectedMac,
                                                               page.modeKeys[currentIndex],
                                                               page.modeKeys[currentIndex] === "global"
                                                               ? targetCombo.currentText : "")
                            }
                            ThemedCombo {
                                id: targetCombo
                                visible: page.modeKeys[modeCombo.currentIndex] === "global"
                                Layout.fillWidth: true
                                model: bridge.groups
                                onActivated: devices.setPolicy(devices.selectedMac, "global", currentText)
                            }
                            Item { Layout.fillWidth: true
                                   visible: page.modeKeys[modeCombo.currentIndex] !== "global" }
                        }

                        // —— 常用域名（Top 5，按累计字节）——
                        Text {
                            text: qsTr("常用域名")
                            font.pixelSize: 14; color: Theme.textPrimary
                            visible: (detailCol.dev.topDomains || []).length > 0
                        }
                        Repeater {
                            model: detailCol.dev.topDomains || []
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8
                                Text { text: modelData.host; font.pixelSize: 11
                                       color: Theme.textSecondary; elide: Text.ElideRight
                                       Layout.fillWidth: true }
                                Text { text: Theme.fmtBytes(modelData.bytes); font.pixelSize: 11
                                       color: Theme.textMuted }
                            }
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
                                // 同 DeviceRow：按下即抢独占 grab，否则在这个列表上按住拖动会把
                                // 整个窗口拖走（Main.qml 的背景 DragHandler 会接管）。
                                MouseArea { anchors.fill: parent }
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
                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: bridge.closeConnectionById(model.connId)
                                        }
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
        function onCsvExported(path) { noticeBar.show(qsTr("已导出到 ") + path); }
        function onGatewayError(msg) { noticeBar.show(msg); }
    }

    // 提示条上的「修复权限 / 安装 Npcap」跑完了要有回音 —— 那些流程的详细状态文字只在安装窗里
    // 显示，从提示条一键触发的用户是看不到的，这里把结论转到浮动提示上。
    Connections {
        target: npcap
        function onFinished(ok) { if (npcap.status.length > 0) noticeBar.show(npcap.status); }
    }

    // 右下角浮动提示（导出成功 / 出错），3.5s 自动消失。
    // 网关的报错可以很长（例如 Npcap 权限那条），所以固定最大宽度 + 换行：单行 Text 会顶着
    // anchors.right 一路撑到窗口左边界外，长文案直接看不全。
    Rectangle {
        id: noticeBar
        property string msg: ""
        visible: msg !== ""
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        radius: 5
        color: Qt.rgba(0, 0, 0, 0.78)
        implicitWidth: noticeTxt.width + 20
        implicitHeight: noticeTxt.implicitHeight + 12
        Text {
            id: noticeTxt
            anchors.centerIn: parent
            width: Math.min(implicitWidth, Math.max(160, page.width - 48))
            text: noticeBar.msg
            wrapMode: Text.WordWrap
            color: "#ffffff"
            font.pixelSize: 12
        }
        // 长文案（网关报错常常两三行）3.5s 读不完，给到 6s。
        Timer { id: noticeTimer; interval: 6000; onTriggered: noticeBar.msg = "" }
        function show(m) { msg = m; noticeTimer.restart(); }
    }

    // Npcap 安装窗（独立顶层窗口，默认隐藏；由上方提示条的「安装 Npcap」打开）。
    NpcapWindow { id: npcapWindow }
}
