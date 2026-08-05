import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 更新窗 —— 独立窗口。与 Swift 端**同一套版式**（那边是 `UpdateView.swift`）：
//
//   顶部：当前版本 → 最新版本 + 三段切换（程序 / 内核 / GeoIP）
//   中间：只放更新内容（滚动）
//   底部：一条状态机 —— 默认「获取更新」→ 查出有新版换成「更新」→ 点了之后换成
//         「进度条 + ✕」，速度与「下载量/总量」在**进度条正下方居中**；
//         状态/错误信息在**左侧**，与右侧那组**底对齐**。
//
// ★ 顶部按平台分三种，这是刻意的 —— 三端的「标题栏」根本不是一回事：
//   · **macOS**：标题栏被做成透明 + 内容铺满整窗（`configureMacTitleBar`），所以版本号与
//     三段组**就摆在标题栏那条带子里**；左边留出红绿灯的位置（`kMacTrafficLights + 20`），
//     不留的话文字直接压在三颗灯上。
//   · **Windows**：三段以**系统原生标题栏菜单**的样子出现 —— 平铺文字、悬停高亮、贴着
//     客户区左上角，就是 Windows 应用菜单栏的长相；窗口标题仍旧由系统标题栏显示。
//   · **Linux**：窗口标题交给系统标题栏（各家 WM 自己画），三段组**悬浮在标题栏下面、居左**。
//
// 「国内加速」不在这一页 —— 它是设置页里的**同一个** `settings.mirror`，
// 两处各放一份只会让人以为是两个开关。
ApplicationWindow {
    id: win
    // 独立顶层窗（去掉隐式 transientParent）：Win/Linux 任务栏显示独立图标，方便切换窗口。
    transientParent: null
    flags: Qt.Window
    width: 600
    height: 560
    minimumWidth: 460
    minimumHeight: 420
    title: qsTr("Coast 更新")
    // mac 透明底露玻璃（applyMacGlass 会在窗后插一层 NSVisualEffectView），
    // 其余平台用壳色 —— 与 Main.qml 同一处置。
    color: isMac ? "transparent" : Theme.shell

    readonly property bool isMac: Qt.platform.os === "osx"
    readonly property bool isWin: Qt.platform.os === "windows"

    /// 顶栏高度。mac 上就是标题栏那条带子的高度（红绿灯由系统在里面垂直居中）。
    readonly property int bandHeight: 50
    /// mac 红绿灯占掉的宽度。版本号从它右边**再让 20** 起排 —— 不让的话直接压在三颗灯上。
    readonly property int macTrafficLights: 78

    property int currentTab: 0   // 0 程序 / 1 内核 / 2 GeoIP

    // 程序更新的**频道**：0=正式版 / 1=测试版(prerelease)。与 currentTab 无关，别混。
    readonly property int channel: settings.receiveBeta ? 1 : 0

    // 当前 tab 的「更新」是否进行中（下载 / 安装）。
    readonly property bool busy: currentTab === 0 ? updater.downloading
                                 : currentTab === 1 ? settings.coreUpdating
                                                    : settings.geoipUpdating

    // 打开即拉 release 列表 + 内核版本。
    // mac 上还要把标题栏做成透明 + 内容铺满整窗 —— 版本号与三段组就摆在那条带子里
    // （与主窗、连接窗同一处理；不调的话系统标题栏还在，顶栏会掉到它下面）。
    onVisibleChanged: {
        if (!visible)
            return
        fetchCurrentTab()
        if (win.isMac) {
            bridge.applyMacGlass(win, Theme.dark)
            win.macInset = bridge.macTitleBarInset(win)
        }
    }

    /// 切到某一页时才取那一页的数据。开着窗切页签也要取 —— 否则内核页显示的是上次打开
    /// 时的旧值（或空），而用户并不知道自己看的是陈的。
    onCurrentTabChanged: if (visible) fetchCurrentTab()

    /// 只取**当前页签**要用的那一份。
    function fetchCurrentTab() {
        if (currentTab === 0)
            updater.refreshApp()
        else if (currentTab === 1)
            updater.refreshCore()
    }

    /// mac 标题栏占掉的高度，由 AppKit 实取（见 `MacWindow::macTitleBarInset`）。
    /// 内容用**负的这个数**当上边距，才能真的摆进标题栏那条带子里。
    property int macInset: 0

    // 主题切换后毛玻璃深浅要重设（与 Main.qml 同）。
    Connections {
        target: Theme
        function onDarkChanged() { if (win.isMac && win.visible) bridge.applyMacGlass(win, Theme.dark) }
    }

    // —— 每个页签的三样：当前版本 / 最新版本 / 更新内容 ——
    readonly property string localVersion:
        currentTab === 0 ? updater.currentVersion
        : currentTab === 1 ? (updater.coreVersion.length > 0 ? updater.coreVersion : "—")
                           : "—"
    /// `updater.releaseVersion` 是「VERSION: 1.2.3」这种老展示串，这一版的版式里
    /// 前缀由「最新 」承担了，去掉免得读成「最新 VERSION: 1.2.3」。
    function stripVersionPrefix(v) {
        return (v || "").replace(/^VERSION:\s*/, "")
    }
    readonly property string remoteVersion:
        currentTab === 0 ? (stripVersionPrefix(channel === 1 ? updater.betaVersion
                                                             : updater.releaseVersion) || "—")
        // 内核那条 updater 只给「更新说明」，最新版本号本身在 about 那边（角标也用它）。
        : currentTab === 1 ? (about.coreUpdateAvailable ? qsTr("有新版") : qsTr("已是最新"))
                           : (updater.checking ? "…" : "latest")
    readonly property string notesText:
        currentTab === 0 ? (channel === 1 ? updater.betaNotes : updater.releaseNotes)
        : currentTab === 1 ? updater.coreNotes
                           : qsTr("Country.mmdb 是「按 IP 归属地区分流」所用的地理数据库（来源 MetaCubeX/meta-rules-dat）。\n\n点击「更新」下载最新数据库到用户目录与资源目录，重启核心后生效。")

    /// 这个页签有没有可装的更新。程序页拿版本号比；内核页交给后端的判断；
    /// GeoIP 没有版本可比（上游天天重建同一个 tag），查过一轮就当「可以更新」——
    /// 说「已是最新」才是撒谎。
    /// 「有没有可装的更新」用的是 `about` 那份判断 —— 侧栏那两颗角标（new / core）同一来源，
    /// 两处结论必须一致（各判各的迟早会出现「角标说有、这页说没有」）。
    readonly property bool hasUpdate:
        currentTab === 0 ? about.updateAvailable
        : currentTab === 1 ? about.coreUpdateAvailable
                           : checkedGeoip

    property bool checkedGeoip: false

    /// 当前页签是不是正在检查。**按页签取**：程序页看 about.checking，内核页看
    /// about.coreChecking —— 混着看的话，在内核页按下「获取更新」会立刻变回「获取更新」
    /// （程序那份没在跑），看着像没反应。
    readonly property bool checkingNow:
        currentTab === 0 ? (updater.checking || about.checking)
        : currentTab === 1 ? about.coreChecking
                           : false

    /// 底部左侧那行状态/错误。
    readonly property string statusText:
        currentTab === 1 ? (settings.coreUpdating ? settings.coreUpdateStatus : "")
        : currentTab === 2 ? (settings.geoipUpdating ? settings.geoipStatus : "")
                           : updater.status

    function doUpdate() {
        if (win.busy)
            return
        if (currentTab === 0) {
            var i = updater.recommendedIndex(win.channel) // 该频道里自动选安装器/便携包
            if (i >= 0)
                updater.oneClickUpdate(win.channel, i, settings.mirror)
        } else if (currentTab === 1) {
            settings.updateCore()
        } else {
            settings.updateGeoip()
        }
    }

    /// 「获取更新」= 刷新**当前页签**那一份数据 + 重做**当前页签**那一份版本比对。
    ///
    /// ★ 以前这里是 `updater.refresh() + about.check()`，两个都不分页签。后果不是"多取了点
    ///   东西"，而是**内核页永远出不来「更新」按钮**：`about.check()` 只查程序，
    ///   `coreUpdateAvailable` 一直由每小时的后台 checkAll 决定，手动检查根本碰不到它。
    ///   在内核页按下去，转圈的是程序那份，内核这边什么都没变。
    function doCheck() {
        if (currentTab === 0) {
            updater.refreshApp()
            about.check()          // → about.updateAvailable
        } else if (currentTab === 1) {
            updater.refreshCore()  // → coreVersion / coreNotes（本页的「更新内容」）
            about.checkCore()      // → about.coreUpdateAvailable（本页的「更新」按钮）
        } else {
            // GeoIP 上游天天重建同一个 tag，没有版本可比 —— 查过一轮就当可以更新，
            // 说「已是最新」才是撒谎。
            win.checkedGeoip = true
        }
    }

    // ———————————————————————— 顶部 ————————————————————————

    // mac：版本号 + 三段组**摆进标题栏那条带子**。
    component MacBand: Item {
        implicitHeight: win.bandHeight
        RowLayout {
            anchors.fill: parent
            // 让开红绿灯：三颗灯占掉约 78，再让 20。不让的话版本号直接压在灯上。
            anchors.leftMargin: win.macTrafficLights + 20
            anchors.rightMargin: 20
            spacing: 12
            VersionLine { Layout.fillWidth: true }
            Item { Layout.fillWidth: true }
            SegmentedTabs {}
        }
    }

    // Windows：三段做成**系统原生标题栏菜单**的样子（平铺文字 + 悬停高亮，贴左上角）；
    // 窗口标题仍旧由系统标题栏显示。版本号排在同一条的右端。
    component WinMenuBar: Rectangle {
        implicitHeight: 28
        color: Theme.dark ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(0, 0, 0, 0.04)
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 4
            anchors.rightMargin: 12
            spacing: 0
            Repeater {
                model: [qsTr("程序"), qsTr("内核"), "GeoIP"]
                delegate: Rectangle {
                    required property int index
                    required property string modelData
                    Layout.preferredWidth: menuText.implicitWidth + 24
                    Layout.preferredHeight: 28
                    // 菜单栏那种「悬停/选中才有底」的平铺样式，不画圆角、不画边框。
                    color: win.currentTab === index ? Theme.accent
                           : (menuHover.hovered ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.10)
                                                              : Qt.rgba(0, 0, 0, 0.08))
                                                : "transparent")
                    Text {
                        id: menuText
                        anchors.centerIn: parent
                        text: parent.modelData
                        font.pixelSize: 12
                        color: win.currentTab === parent.index ? "#ffffff" : Theme.textPrimary
                    }
                    HoverHandler { id: menuHover }
                    TapHandler { onTapped: win.currentTab = parent.index }
                }
            }
            Item { Layout.preferredWidth: 12 }
            VersionLine { Layout.fillWidth: true }
        }
    }

    // Linux：标题归系统标题栏，三段组**悬浮在标题栏下面、居左**。
    component LinuxBar: Item {
        implicitHeight: 44
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 12
            SegmentedTabs {}
            Item { Layout.preferredWidth: 12 }
            VersionLine { Layout.fillWidth: true }
        }
    }

    // 当前版本 → 最新版本。查不到的那一头显示「—」，不编。
    /// ★ 两段都要 `Layout.fillWidth` + 省略号：内核页那条「当前」本身就是一句话
    ///   （「内核版本: 未安装 → 正式版 v1.10.4392（可切换）」），不给它上限就会把右边的
    ///   三段组一路顶出窗外（实测 GeoIP 那段整个跑到屏幕外面去了）。
    component VersionLine: RowLayout {
        spacing: 8
        Text {
            Layout.fillWidth: true
            text: qsTr("当前 ") + win.localVersion
            font.pixelSize: 13
            color: Theme.textPrimary
            elide: Text.ElideRight
        }
        Text { text: "→"; font.pixelSize: 12; color: Theme.textMuted }
        Text {
            Layout.fillWidth: true
            text: qsTr("最新 ") + win.remoteVersion
            font.pixelSize: 13
            color: win.hasUpdate ? Theme.accent : Theme.textSecondary
            elide: Text.ElideRight
        }
    }

    // 三段：**一层**胶囊底，选中段的底压在里面 —— 与主窗页脚那颗「规则」同一画法，
    // 压在里面才不会给整组带来额外圆角（看着是一颗胶囊被分成三段，而不是三颗独立按钮）。
    component SegmentedTabs: Rectangle {
        // ★ 宽度**只能由内容决定**：`implicitWidth ← Row.implicitWidth` 的同时又让 Row
        //   `anchors.fill: parent`，就是一条循环绑定 —— 宽度每轮都往上长，实测能撑出屏幕。
        //   Row 不再 fill，自己按内容定宽，外面这层只跟着它。
        implicitWidth: segRow.width
        implicitHeight: 28
        radius: height / 2
        color: Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
        Row {
            id: segRow
            height: parent.height
            Repeater {
                model: [qsTr("程序"), qsTr("内核"), "GeoIP"]
                delegate: Rectangle {
                    required property int index
                    required property string modelData
                    width: 66
                    height: parent.height
                    radius: height / 2
                    color: win.currentTab === index ? Qt.rgba(Theme.accent.r, Theme.accent.g,
                                                              Theme.accent.b, 0.45)
                                                    : "transparent"
                    Text {
                        anchors.centerIn: parent
                        text: parent.modelData
                        font.pixelSize: 12
                        color: win.currentTab === parent.index ? Theme.textPrimary : Theme.textMuted
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: win.currentTab = parent.index }
                }
            }
        }
    }

    // ———————————————————————— 底部动作 ————————————————————————

    // 胶囊按钮（获取更新 / 更新）。mac 上系统会给玻璃感，其余平台是同形状的实心胶囊。
    component PillButton: Rectangle {
        property string label: ""
        property bool disabled: false
        signal clicked
        implicitWidth: pillText.implicitWidth + 32
        implicitHeight: 30
        radius: height / 2
        opacity: disabled ? 0.5 : 1.0
        color: pillHover.hovered && !disabled
               ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.12))
               : (Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(0, 0, 0, 0.08))
        Text {
            id: pillText
            anchors.centerIn: parent
            text: parent.label
            font.pixelSize: 13
            color: Theme.textPrimary
        }
        HoverHandler { id: pillHover; cursorShape: parent.disabled ? Qt.ArrowCursor
                                                                   : Qt.PointingHandCursor }
        TapHandler { enabled: !parent.disabled; onTapped: parent.clicked() }
    }

    // ———————————————————————— 版面 ————————————————————————

    ColumnLayout {
        anchors.fill: parent
        // ★ mac：**负的上边距**把内容顶进标题栏那条带子里。
        //   探针量过：`configureMacTitleBar` 已经开了 fullSizeContentView，NSView 确实铺满整窗
        //   （contentView 600×560），但 AppKit 的 `contentLayoutRect` 只有 600×528 ——
        //   Qt 按后者起排，于是场景整体下移 32，顶栏就掉到红绿灯下面去了。
        //   顶回去之后版本号与三段组才和三颗灯在同一行（与 Swift 端一致）。
        anchors.topMargin: win.isMac ? -win.macInset : 0
        spacing: 0

        // 顶部：三端各一种（见文件头的说明）
        Loader {
            Layout.fillWidth: true
            sourceComponent: win.isMac ? macBand : (win.isWin ? winMenuBar : linuxBar)
        }
        Component { id: macBand; MacBand {} }
        Component { id: winMenuBar; WinMenuBar {} }
        Component { id: linuxBar; LinuxBar {} }

        // 中间：只放更新内容
        Flickable {
            id: notesFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 20
            // ★ 右边**不留内距**：滚动条要贴在窗口右缘上（系统列表都是这么排的）。
            //   正文自己让出 20（见下面 Text 的 width），所以文字与左边一样是 20 内距，
            //   只有滚动条压在最右边。
            Layout.topMargin: 12
            clip: true
            contentWidth: width
            contentHeight: notesInner.implicitHeight
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Text {
                id: notesInner
                width: notesFlick.width - 20   // 让出右侧 20：滚动条贴边，正文不贴
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                lineHeight: 1.35
                color: Theme.textSecondary
                text: win.notesText.length > 0 ? win.notesText : qsTr("暂无更新说明")
            }
        }

        // 底栏：**左边状态/错误，右边动作，两边底对齐**。
        // 状态那行常常很长（限流那条一句话就两行），塞在按钮上下会把底栏顶得忽高忽低；
        // 底对齐是因为右侧在下载态是「进度条 + 一行文字」两行高，顶对齐的话左边那句会飘在半空。
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.topMargin: 10
            spacing: 16

            Text {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignBottom
                text: win.statusText
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.textMuted
            }

            // —— 下载中：进度条 + ✕（结束下载），速度在进度条正下方居中 ——
            ColumnLayout {
                Layout.alignment: Qt.AlignBottom
                visible: updater.downloading
                spacing: 5

                RowLayout {
                    spacing: 10
                    ColumnLayout {
                        spacing: 5
                        // 细进度条：高 5、**全圆角**、不写百分比（下面那行已经有量了，
                        // 5 高的条也塞不下字）。
                        Rectangle {
                            Layout.preferredWidth: 220
                            Layout.preferredHeight: 5
                            radius: height / 2
                            color: Theme.dark ? "#0d0d0d" : "#e4e4e4"
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Math.max(0, parent.width * updater.progress / 100)
                                radius: height / 2
                                color: Theme.accent
                            }
                        }
                        Text {
                            Layout.preferredWidth: 220
                            horizontalAlignment: Text.AlignHCenter
                            text: (updater.downloadSpeed.length > 0 ? updater.downloadSpeed : "0 B/s")
                                  + " (" + updater.downloadedText + " / " + updater.totalText + ")"
                            font.pixelSize: 11
                            color: Theme.textMuted
                        }
                    }
                    // ✕ = **结束这次下载**，不是关窗。窗留着：取消完底栏自己回到
                    // 「更新 / 获取更新」，用户可以直接再来一次。
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        radius: 13
                        color: cancelHover.hovered
                               ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.12))
                               : (Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(0, 0, 0, 0.08))
                        Text {
                            anchors.centerIn: parent
                            text: "✕"
                            font.pixelSize: 11
                            color: Theme.textPrimary
                        }
                        HoverHandler { id: cancelHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: updater.cancelDownload() }
                    }
                }
            }

            // —— 有更新：更新 ——
            PillButton {
                Layout.alignment: Qt.AlignBottom
                visible: !updater.downloading && win.hasUpdate
                label: win.busy ? qsTr("更新中…") : qsTr("更新")
                disabled: win.busy
                onClicked: win.doUpdate()
            }

            // —— 默认：获取更新 ——
            PillButton {
                Layout.alignment: Qt.AlignBottom
                visible: !updater.downloading && !win.hasUpdate
                label: win.checkingNow ? qsTr("检查中…") : qsTr("获取更新")
                disabled: win.checkingNow || win.busy
                onClicked: win.doCheck()
            }
        }
    }
}
