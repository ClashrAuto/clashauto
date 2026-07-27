import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 设备页：概览条 + 设备列表（发现/热更新）。**只有列表**——单台设备的详细信息（信息网格、
// 备注名/类型、历史用量、策略、常用域名、连接列表）在独立窗口 DeviceDetailWindow 里，点行打开。
// 页面显隐驱动 devices.setActive → 控制扫描/热更新/连接轮询的开停。
Item {
    id: page

    // 轮询开停的唯一决策点：页面**或**详情窗任一可见就得继续轮询——否则用户把详情窗拉出来、
    // 主窗切到别的页面，详情窗里的速率/连接就静止了。setActive 幂等（同值直接返回）。
    function syncActive() { devices.setActive(page.visible || detailWindow.visible); }
    onVisibleChanged: page.syncActive()

    // —— 概览条的自适应：窄了就按优先级把次要项收起来（收起顺序：今日 → 网关 → 提醒文字 → 导出）——
    // 断点**不写死像素**，拿各项自己的 implicitWidth 现算：同一句话在 12 种语言里宽度能差一倍
    // （德语 "Neues-Gerät-Hinweis" 比中文长一大截），写死的数字必然在某个语言上翻车——
    // 900px 窗口下英文的「Gateway 192.168.20.1」就正好被切掉，中文却看着好好的。
    // 读的都是**叶子 Text / 常显 Row** 的 implicitWidth：它们不随自己的 visible 变化，
    // 所以不会形成「宽度 → 显隐 → 宽度」的绑定环。
    readonly property real ovSpacing: 16
    readonly property real ovAvail: width - 20 // 减掉 ColumnLayout 的左右内距
    readonly property real ovBase: ovTitle.implicitWidth + ovOnline.implicitWidth
                                   + ovProxied.implicitWidth + 34 /* 提醒开关本体 */
                                   + ovSpacing * 3 + 5
    readonly property real ovNeedExport: ovBase + ovSpacing + ovExport.implicitWidth
    readonly property real ovNeedAlert: ovNeedExport + 5 + ovAlertTxt.implicitWidth
    readonly property real ovNeedGateway: ovNeedAlert + ovSpacing + ovGateway.implicitWidth
    readonly property real ovNeedToday: ovNeedGateway + ovSpacing + ovToday.implicitWidth
    readonly property bool showExport: ovAvail >= ovNeedExport
    readonly property bool showAlertLabel: ovAvail >= ovNeedAlert
    readonly property bool showGateway: ovAvail >= ovNeedGateway
    readonly property bool showToday: ovAvail >= ovNeedToday

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        // —————————————————— 概览条 ——————————————————
        // **必须显式 Layout.minimumWidth: 0**。RowLayout 里 fillWidth 为 false 的子项是「定宽」的，
        // 它们的宽度会原样计进这个 RowLayout 的最小宽度；而外层 ColumnLayout 布局子项时是按
        // max(自身宽度, 子项最小宽度) 给宽的——一旦概览条的最小宽超过页面宽度，整列（包括
        // fillWidth 的设备列表）就被撑到那个宽度，列表右半边连同代理开关一起被挤出窗口。
        // 给 0 之后：页面再窄，列表也只跟随内容区宽度；概览条自己按上面的断点收起次要项。
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 16
            Text { id: ovTitle; text: qsTr("设备"); font.pixelSize: 18; color: Theme.textPrimary }
            Row {
                id: ovOnline
                spacing: 4
                Text { text: qsTr("在线"); font.pixelSize: 12; color: Theme.textMuted }
                Text { text: devices.onlineCount + "/" + devices.deviceCount
                       font.pixelSize: 12; color: Theme.textSecondary }
            }
            Row {
                id: ovProxied
                spacing: 4
                Text { text: qsTr("代理中"); font.pixelSize: 12; color: Theme.textMuted }
                Text { text: devices.proxiedCount; font.pixelSize: 12; color: Theme.accent }
            }
            // 全部设备**今日**累计上/下行。以前这里放的是实时总速率，但那个数在状态页已经有一份
            // （而且更完整——它是核心的全局速率，不受「能不能归属到某台设备」影响）；这里更该回答
            // 的是「今天这个网络一共用了多少」。（单台设备的用量在详情窗里，行里不再显示。）
            Row {
                id: ovToday
                spacing: 8
                visible: page.showToday
                Text { text: qsTr("今日"); font.pixelSize: 12; color: Theme.textMuted
                       anchors.verticalCenter: parent.verticalCenter }
                Text { text: "↓ " + Theme.fmtBytes(devices.totalTodayDown)
                       font.pixelSize: 12; color: "#5bb44b" }
                Text { text: "↑ " + Theme.fmtBytes(devices.totalTodayUp)
                       font.pixelSize: 12; color: "#b14a4a" }
            }
            Item { Layout.fillWidth: true }
            // 新设备提醒（蹭网检测）开关
            Row {
                spacing: 5
                Text { id: ovAlertTxt
                       text: qsTr("新设备提醒"); font.pixelSize: 11; color: Theme.textMuted
                       visible: page.showAlertLabel
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
                id: ovExport
                text: qsTr("导出")
                visible: page.showExport
                font.pixelSize: 11; color: Theme.accent
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: devices.exportCsv() }
            }
            Text { id: ovGateway
                   text: qsTr("网关 ") + (devices.gatewayIp || "-")
                   visible: page.showGateway
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
            Layout.minimumWidth: 0 // 同概览条：定宽的「安装 Npcap」按钮不能反过来撑宽整页
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

        // —————————————————— 搜索 / 仅在线 / 重扫 ——————————————————
        RowLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0 // 同上：两个 28px 方钮不能把整页顶宽
            spacing: 6
            TextField {
                id: search
                Layout.fillWidth: true
                Layout.minimumWidth: 0
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
                    text: "" // refresh-line
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

        // —————————————————— 设备列表（整页宽）——————————————————
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
                lastHost: model.lastHost
                // 背景那张实时流量图只有被代理的设备画——没代理就别去取历史，
                // 省掉每拍为每一行把 40 个点转成 QVariantList 的开销。
                rateUpHist: model.proxied ? model.rateUpHist : []
                rateDownHist: model.proxied ? model.rateDownHist : []
                isSelf: model.isSelf
                isGateway: model.isGateway
                proxyable: model.proxyable
                selected: devices.selectedMac === model.mac
                onClicked: detailWindow.openFor(model.mac)
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

    // 导出结果 / 网关报错 → 右下角浮动提示。
    Connections {
        target: devices
        function onCsvExported(path) { noticeBar.show(qsTr("已导出到 ") + path); }
        function onGatewayError(msg) { noticeBar.show(msg); }
    }

    // 提示条上的「修复权限 / 安装 Npcap」跑完了要有回音 —— 那些流程的详细状态文字只在安装窗里
    // 显示，从提示条一键触发的用户是看不到的，这里把结论转到浮动提示上。
    Connections {
        target: npcap
        function onFinished(ok) { if (npcap.status.length > 0) noticeBar.show(npcap.status); }
    }

    // 右下角浮动提示（导出成功 / 出错），自动消失。
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

    // 设备详情窗（独立顶层窗口，默认隐藏；点列表行打开）。
    // onVisibleChanged 写在这里而不是组件内部：实例化处的处理器会覆盖组件里的同名处理器，
    // 两边都写只有这边生效——所以组件内部一律用 Connections，见 DeviceDetailWindow.qml 顶部注释。
    DeviceDetailWindow {
        id: detailWindow
        onVisibleChanged: page.syncActive()
    }

    // Npcap 安装窗（独立顶层窗口，默认隐藏；由上方提示条的「安装 Npcap」打开）。
    NpcapWindow { id: npcapWindow }
}
