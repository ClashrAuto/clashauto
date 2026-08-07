import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 设备详情 —— 独立顶层窗口（原来是设备页右半边那一栏；页面现在只留列表）。
// 显示的永远是「当前选中的设备」(devices.selectedDevice)：窗口开着时点列表里另一台设备，
// 这里就跟着换，不用来回开关窗口。
//
// 内容自上而下：头部(头像/名字/代理开关) → 不可代理的原因 → 信息网格 → 备注名/类型 →
// 实时流量卡(含带宽图) → 近 7 天 → 策略 → 常用域名 → 该设备的实时连接列表。
//
// 注意：**别在使用处（DevicesPage）给这个组件写 onVisibleChanged 之外的东西**——
// 实例化处的信号处理器会**覆盖**组件内部声明的同名处理器。页面那边要用 onVisibleChanged
// 驱动 devices.setActive，所以本文件内部一律走 Connections，不写 onVisibleChanged。
ApplicationWindow {
    id: win
    // 独立顶层窗（去掉隐式 transientParent）：Win/Linux 任务栏显示独立图标，方便切换窗口。
    transientParent: null
    flags: Qt.Window
    width: 600
    height: 720
    minimumWidth: 420
    minimumHeight: 420
    title: qsTr("设备详情") + (win.dev.name ? "  ·  " + win.dev.name : "")
    // 统一用壳色（同 ConnectionsWindow，理由见那里）。
    color: Theme.shell

    readonly property bool isWin: Qt.platform.os === "windows"
    // Windows 系统标题栏染成本窗背景色（同 ConnectionsWindow，理由见那里）。
    // 这里尤其只能用 Connections —— 见本文件顶部那条「别在使用处写 onVisibleChanged」。
    Connections {
        target: win
        function onVisibleChanged() { win.paintTitleBar() }
    }
    Connections {
        target: Theme
        function onDarkChanged() { win.paintTitleBar() }
    }
    function paintTitleBar() {
        if (isWin && visible)
            bridge.applyWindowsTitleBar(win, win.color, Theme.dark)
    }

    // 单一数据源：所有子项都引用 win.dev.*。
    readonly property var dev: devices.selectedDevice

    // 列表行点进来：先切选中设备，再把窗口拉到前台（已经开着就只是换内容 + raise）。
    function openFor(mac) {
        devices.select(mac);
        win.show();
        win.raise();
        win.requestActivate();
    }

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
    // 近 7 天柱状图用：这 7 天里单日最大字节（归一化基准，至少 1 免得除零）、以及 7 天合计
    // （合计为 0 时整块不显示——没历史的设备画 7 根空柱子只是噪音）。
    function maxDayBytes(days) {
        var m = 1;
        for (var i = 0; days && i < days.length; ++i)
            m = Math.max(m, days[i].up + days[i].down);
        return m;
    }
    function daysTotal(days) {
        var t = 0;
        for (var i = 0; days && i < days.length; ++i)
            t += days[i].up + days[i].down;
        return t;
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

    // 选中设备没了（或从没选过）时给个空态，免得整窗空白。
    Text {
        anchors.centerIn: parent
        visible: devices.selectedMac === ""
        text: qsTr("未选择设备")
        font.pixelSize: 13
        color: Theme.textMuted
    }

    ScrollView {
        id: detailScroll
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        // **右边不留内距**（同设备页）：滚动区一直铺到窗口右缘，滚动条才贴着边，而不是悬在
        // 离边 12px 的空中。内容列自己收窄 12px 补回来，滚动条正好落在那条空隙上、不压内容。
        visible: devices.selectedMac !== ""
        clip: true
        contentWidth: availableWidth
        ScrollBar.vertical.policy: ScrollBar.AsNeeded

        ColumnLayout {
            id: detailCol
            width: detailScroll.availableWidth - 12
            // 高度就是内容的自然高度。以前这里取 max(内容高, 视口高)，是为了把多出来的高度
            // 分给唯一 fillHeight 的连接列表——但连接列表现在不再自带滚动、整份铺开，内容高
            // 本来就随连接数一起长，再跟视口高取大只会让各个 Layout 子项被拉伸变形。
            height: implicitHeight
            spacing: 12

            // —— 头部 ——
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: 48; Layout.preferredHeight: 48
                    radius: 10
                    color: Theme.deviceColor(win.dev.typeKey || "unknown")
                    Text { anchors.centerIn: parent
                           text: Theme.deviceGlyph(win.dev.typeKey || "unknown")
                           font.family: Theme.riFont // 图标字体（默认 MiSans 里这些码点是空的）
                           font.pixelSize: 26; color: "#ffffff" }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: win.dev.name || ""
                        font.pixelSize: 18
                        color: Theme.textPrimary
                        elide: Text.ElideRight; Layout.fillWidth: true
                    }
                    Text {
                        text: win.typeName(win.dev.typeKey || "unknown")
                              + "  ·  " + (win.dev.online ? qsTr("在线") : qsTr("离线"))
                        font.pixelSize: 12; color: Theme.textMuted
                    }
                }
                ColumnLayout {
                    id: proxyBox
                    // 已经开着的一律可见可关（离线/跨网段也得能撤销）；关着的只有
                    // 「可代理且在线」才点得动（离线设备拿不到 IP/ARP，劫持无从下手）。
                    visible: win.dev.proxyable === true || win.dev.proxyEnabled === true
                    spacing: 2
                    readonly property bool canToggle:
                        win.dev.proxyEnabled === true
                        || (win.dev.proxyable === true && win.dev.online === true)
                    Rectangle {
                        Layout.alignment: Qt.AlignRight
                        width: 52; height: 26; radius: 13
                        opacity: proxyBox.canToggle ? 1.0 : 0.4
                        color: win.dev.proxyEnabled ? Theme.accent : Theme.switchTrackOff
                        Behavior on color { ColorAnimation { duration: 120 } }
                        Rectangle {
                            width: 22; height: 22; radius: 11; y: 2
                            x: win.dev.proxyEnabled ? parent.width - width - 2 : 2
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
                                                              !win.dev.proxyEnabled)
                        }
                    }
                    Text { Layout.alignment: Qt.AlignRight; text: qsTr("代理网络")
                           font.pixelSize: 10; color: Theme.textMuted }
                }
            }

            // M0 诚实提示
            Rectangle {
                Layout.fillWidth: true
                visible: win.dev.proxyEnabled === true && !devices.gatewayReady
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

            // 代理生效中的「依赖说明」：这台设备的默认网关已经被指到本机，它的每个包都要本机
            // 转发——用户必须知道「本机不在 = 它上不了网」，以及各种退出方式的实际后果。
            Rectangle {
                Layout.fillWidth: true
                visible: win.dev.proxyEnabled === true && devices.gatewayReady
                radius: 5
                color: Theme.metricBg
                implicitHeight: dependTxt.implicitHeight + 14
                Text {
                    id: dependTxt
                    anchors.fill: parent
                    anchors.margins: 7
                    wrapMode: Text.WordWrap
                    text: qsTr("该设备的联网由本机转发。退出 / 关机 / 睡眠都会自动把它交还给路由器"
                               + "（约 1 秒内恢复）；但断电或强制结束进程时，它最多可能断网 30 秒左右。")
                          + (settings.autoStart ? "" : qsTr("\n建议在「设置」里打开「开机自启」，"
                                                            + "重启后才能自动接着代理。"))
                    font.pixelSize: 11; color: Theme.textMuted
                    lineHeight: 1.25
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
                        switch (win.dev.proxyBlockReason || "") {
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
                        { k: qsTr("IP"), v: win.dev.ip || "" },
                        { k: qsTr("MAC"), v: win.dev.mac || "" },
                        { k: qsTr("厂商"), v: win.dev.vendor || "" },
                        { k: qsTr("型号"), v: win.dev.model || "" },
                        { k: qsTr("首次发现"), v: win.dev.firstSeen || "" },
                        { k: qsTr("主机名"), v: win.dev.autoName || "" },
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
                    text: win.dev.alias || ""
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
                    model: win.typeKeys.map(function (k) { return win.typeOverrideName(k); })
                    currentIndex: Math.max(0, win.typeKeys.indexOf(win.dev.typeOverride || "unknown"))
                    onActivated: devices.setTypeOverride(devices.selectedMac, win.typeKeys[currentIndex])
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
                        Text { text: "↓ " + Theme.fmtRate(win.dev.rateDown)
                               font.pixelSize: 13; color: "#5bb44b" }
                        Text { text: "  ↑ " + Theme.fmtRate(win.dev.rateUp)
                               font.pixelSize: 13; color: "#b14a4a" }
                    }
                    BandwidthChart {
                        id: devChart
                        Layout.fillWidth: true
                        Layout.preferredHeight: 90
                        title: qsTr("下载")
                        lineColor: "#5bb44b"
                    }
                    // 会话/今日/累计三块用 Flow 而不是 RowLayout：这三块是定宽的（内容多宽就多宽），
                    // 放 RowLayout 里窗口一窄就整排溢出到卡片外；Flow 放不下会自动折行。
                    Flow {
                        Layout.fillWidth: true
                        spacing: 16
                        Column {
                            Text { text: qsTr("本次会话"); font.pixelSize: 10; color: Theme.textMuted }
                            Text { text: "↓" + Theme.fmtBytes(win.dev.sessionDown)
                                         + "  ↑" + Theme.fmtBytes(win.dev.sessionUp)
                                   font.pixelSize: 12; color: Theme.textSecondary }
                        }
                        Column {
                            Text { text: qsTr("今日"); font.pixelSize: 10; color: Theme.textMuted }
                            Text { text: "↓" + Theme.fmtBytes(win.dev.todayDown)
                                         + "  ↑" + Theme.fmtBytes(win.dev.todayUp)
                                   font.pixelSize: 12; color: Theme.textSecondary }
                        }
                        Column {
                            Text { text: qsTr("累计"); font.pixelSize: 10; color: Theme.textMuted }
                            Text { text: "↓" + Theme.fmtBytes(win.dev.totalDown)
                                         + "  ↑" + Theme.fmtBytes(win.dev.totalUp)
                                   font.pixelSize: 12; color: Theme.textSecondary }
                        }
                    }
                }
            }

            // —— 近 7 天用量（来自历史库，跨重启保留）——
            // 数据是「连接结束时落库」攒出来的，加上仍在途连接的实时字节，所以今天
            // 那根柱子也会跟着涨。没有任何记录时整块不显示（新装/没开过代理的设备）。
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: win.daysTotal(win.dev.recentDays) > 0
                Text { text: qsTr("近 7 天"); font.pixelSize: 14; color: Theme.textPrimary }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    spacing: 6
                    Repeater {
                        model: win.dev.recentDays || []
                        delegate: ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 3
                            Item {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Rectangle { // 柱子：按这 7 天里的最大值归一化
                                    anchors.bottom: parent.bottom
                                    width: parent.width
                                    radius: 2
                                    height: Math.max(2, parent.height
                                            * (modelData.up + modelData.down)
                                            / win.maxDayBytes(win.dev.recentDays))
                                    color: Theme.accent
                                    opacity: 0.75
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: String(modelData.day).slice(5) // MM-DD
                                font.pixelSize: 9
                                color: Theme.textMuted
                            }
                            Text {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: Theme.fmtBytes(modelData.up + modelData.down)
                                font.pixelSize: 9
                                color: Theme.textSecondary
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            // —— 策略 ——
            RowLayout {
                Layout.fillWidth: true
                visible: win.dev.proxyable === true
                spacing: 8
                Text { text: qsTr("策略"); font.pixelSize: 12; color: Theme.textMuted
                       Layout.preferredWidth: 64 }
                ThemedCombo {
                    id: modeCombo
                    Layout.preferredWidth: 130
                    model: win.modeKeys.map(function (k) { return win.modeName(k); })
                    currentIndex: win.modeKeys.indexOf(win.dev.policyMode || "follow")
                    onActivated: devices.setPolicy(devices.selectedMac,
                                                   win.modeKeys[currentIndex],
                                                   win.modeKeys[currentIndex] === "global"
                                                   ? targetCombo.currentText : "")
                }
                ThemedCombo {
                    id: targetCombo
                    visible: win.modeKeys[modeCombo.currentIndex] === "global"
                    Layout.fillWidth: true
                    model: bridge.groups
                    onActivated: devices.setPolicy(devices.selectedMac, "global", currentText)
                }
                Item { Layout.fillWidth: true
                       visible: win.modeKeys[modeCombo.currentIndex] !== "global" }
            }

            // —— 常用域名（Top 5，按累计字节）——
            Text {
                text: qsTr("常用域名")
                font.pixelSize: 14; color: Theme.textPrimary
                visible: (win.dev.topDomains || []).length > 0
            }
            Repeater {
                model: win.dev.topDomains || []
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
                id: connList
                Layout.fillWidth: true
                // **整份铺开，不自带滚动**：以前它是个 220px 高、自己带滚动条的小窗口，嵌在本来
                // 就能滚的详情页里——两层滚动叠在一起，滚轮落在谁身上全看指针位置，翻连接得先
                // 把指针挪进那个小框。现在整列铺开，整页只剩外层 ScrollView 一个滚动条。
                // 高度**按行数直接算**（行高 34 + 间距 2），不写 contentHeight：ListView 只为
                // 视口内的行建委托，拿 contentHeight 当自己的高度就成了「高度→建更多行→
                // contentHeight 变大→高度再变」的逐帧追赶。空列表时留 96px 放空态文字。
                readonly property int rows: devices.connModel.count
                Layout.preferredHeight: rows > 0 ? rows * 34 + (rows - 1) * spacing : 96
                interactive: false // 滚动交给外层 ScrollView（自己不再抢滚轮）
                clip: true         // 高度变化的那一帧，缓冲区里的委托别画到相邻区块上
                model: devices.connModel
                spacing: 2
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
                            text: "\uEB99" // close-fill（同 StatusPage：私用区码点写转义）
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

    // 选中设备的实时下载速率喂进详情带宽图 —— **固定 1s 一拍**，不再挂在 selectedChanged 上。
    // selectedChanged 的含义是「选中设备的数据变了」，一秒里能触发好几次（每秒的流量聚合、5s
    // 的在线态热更新、每轮扫描、台账的任何一次保存都会走到 rebuildSelected）。于是曲线一会儿
    // 连推三个点、一会儿一个不推，而 BandwidthChart 的滚动相位是按「距上次入点 1000ms」算的：
    // 入点节奏一乱，画面就一顿一顿。状态页那张图之所以顺，是因为它的数据源(/traffic 常开流)
    // 本来就是稳定的每秒一条。这里补一个自己的稳定节拍，两张图的观感就一致了。
    Timer {
        interval: 1000
        repeat: true
        running: win.visible && devices.selectedMac !== ""
        onTriggered: devChart.push(win.dev.rateDown || 0)
    }
}
