import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 更新窗 —— 独立窗口。改造后只「展示更新内容 + 一键更新」，不再让用户挑资源：
//   [程序] 当前版→最新版 + 更新说明 → 更新
//          （Windows：下 portable zip → 解压覆盖到项目目录 → 重启；mac/linux 走各自安装器）
//   [内核] 本地版→最新 mihomo + 说明 → 更新（settings.updateCore：下 zip/gz → 替换内核 → 重启核心）
//   [GeoIP] 说明 → 更新（settings.updateGeoip：下 country.mmdb 到用户目录 + 资源目录）
// 程序下载走 updater 的进度条（含 ✕ 取消 + 速度/已下载）；内核 / GeoIP 体积小，只显示状态文字。
ApplicationWindow {
    id: win
    // 独立顶层窗（去掉隐式 transientParent）：Win/Linux 任务栏显示独立图标，方便切换窗口。
    transientParent: null
    flags: Qt.Window
    width: 600
    height: 560
    minimumWidth: 460
    minimumHeight: 420
    title: qsTr("Clash Auto 更新")
    color: Theme.card

    property int currentTab: 0   // 0 程序 / 1 内核 / 2 GeoIP

    // 当前 tab 的「更新」是否进行中（下载 / 安装）。
    readonly property bool busy: currentTab === 0 ? updater.downloading
                                 : currentTab === 1 ? settings.coreUpdating
                                                    : settings.geoipUpdating

    // 打开即拉 release 列表 + 内核版本。
    onVisibleChanged: if (visible) updater.refresh()

    Connections {
        target: updater
        function onFailed(reason) { /* status 已在窗内展示，此处不额外弹窗 */ }
    }

    function doUpdate() {
        if (win.busy)
            return
        if (currentTab === 0) {
            var i = updater.recommendedIndex(0) // 正式版频道里自动选便携 zip / 安装器
            if (i >= 0)
                updater.oneClickUpdate(0, i, settings.mirror)
        } else if (currentTab === 1) {
            settings.updateCore()
        } else {
            settings.updateGeoip()
        }
    }

    // 竖排 tab 按钮（图标 + 文字）
    component TabButton_: Rectangle {
        property int idx: 0
        property string label: ""
        property string icon: ""
        Layout.preferredWidth: 78
        Layout.preferredHeight: 46
        radius: 5
        color: win.currentTab === idx ? Theme.accent : (Theme.dark ? "#252525" : "#eeeeee")
        RowLayout {
            anchors.centerIn: parent
            spacing: 6
            Text {
                text: icon
                font.family: Theme.riFont
                font.pixelSize: 16
                color: win.currentTab === idx ? "#ffffff" : Theme.textSecondary
            }
            Text {
                text: label
                font.pixelSize: 13
                color: win.currentTab === idx ? "#ffffff" : Theme.textSecondary
            }
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: win.currentTab = idx }
    }

    // 「更新说明」正文卡（只读、可滚动）
    component NotesCard: Rectangle {
        property alias text: notesInner.text
        Layout.fillWidth: true
        Layout.fillHeight: true
        radius: 4
        color: Theme.dark ? "#0d0d0d" : "#ffffff"
        border.width: 1
        border.color: Theme.divider
        Flickable {
            id: notesFlick
            anchors.fill: parent
            anchors.margins: 8
            clip: true
            contentWidth: width
            contentHeight: notesInner.implicitHeight
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            Text {
                id: notesInner
                width: notesFlick.width
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                lineHeight: 1.3
                color: Theme.textSecondary
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        // —— 标题 ——
        Text {
            text: qsTr("Clash Auto 更新")
            font.pixelSize: 16
            font.bold: true
            color: Theme.textPrimary
        }

        // —— 左 tab + 右内容 ——
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                spacing: 6
                TabButton_ { idx: 0; label: qsTr("程序"); icon: "" }
                TabButton_ { idx: 1; label: qsTr("内核"); icon: "" }
                TabButton_ { idx: 2; label: qsTr("GeoIP"); icon: "" }
                Item { Layout.fillHeight: true }
            }

            // 内容卡
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 5
                color: Theme.dark ? "#161616" : "#f6f6f6"
                border.width: 1
                border.color: Theme.divider

                // ——— 程序 ———
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    visible: win.currentTab === 0

                    Text {
                        text: qsTr("当前版本: ") + updater.currentVersion
                        font.pixelSize: 13
                        color: Theme.textSecondary
                    }
                    Text {
                        text: updater.releaseVersion
                        font.pixelSize: 15
                        font.bold: true
                        color: Theme.textPrimary
                    }
                    Text { text: qsTr("更新说明"); font.pixelSize: 12; color: Theme.textMuted }
                    NotesCard { text: updater.releaseNotes }
                }

                // ——— 内核 ———
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    visible: win.currentTab === 1

                    Text {
                        Layout.fillWidth: true
                        text: updater.coreVersion.length > 0 ? updater.coreVersion : qsTr("内核版本: 检测中...")
                        font.pixelSize: 15
                        font.bold: true
                        wrapMode: Text.WordWrap
                        color: Theme.textPrimary
                    }
                    Text { text: qsTr("更新说明"); font.pixelSize: 12; color: Theme.textMuted }
                    NotesCard { text: updater.coreNotes }
                }

                // ——— GeoIP ———
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    visible: win.currentTab === 2

                    Text {
                        text: qsTr("GeoIP 数据库")
                        font.pixelSize: 15
                        font.bold: true
                        color: Theme.textPrimary
                    }
                    Text { text: qsTr("说明"); font.pixelSize: 12; color: Theme.textMuted }
                    NotesCard {
                        text: qsTr("Country.mmdb 是「按 IP 归属地区分流」所用的地理数据库（来源 MetaCubeX/meta-rules-dat）。\n\n点击「更新」下载最新数据库到用户目录与资源目录，重启核心后生效。")
                    }
                }
            }
        }

        // —— 下载中（程序）：进度条 + 右侧 ✕ 取消 ——
        RowLayout {
            Layout.fillWidth: true
            visible: updater.downloading
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                radius: 4
                color: Theme.dark ? "#0d0d0d" : "#eeeeee"
                border.width: 1
                border.color: Theme.divider
                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 1
                    width: Math.max(0, (parent.width - 2) * updater.progress / 100)
                    radius: 4
                    color: Theme.accent
                }
                Text {
                    anchors.centerIn: parent
                    text: updater.progress + "%"
                    font.pixelSize: 11
                    color: Theme.textPrimary
                }
            }

            // ✕ 取消下载：abort → UpdateController 走「已取消」分支（删临时文件、不弹失败）。
            Rectangle {
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                radius: 12
                color: cancelHover.hovered ? "#f56c6c" : (Theme.dark ? "#252525" : "#eeeeee")
                border.width: 1
                border.color: cancelHover.hovered ? "#f56c6c" : Theme.divider
                Text {
                    anchors.centerIn: parent
                    text: "✕"
                    font.pixelSize: 12
                    color: cancelHover.hovered ? "#ffffff" : Theme.textSecondary
                }
                HoverHandler { id: cancelHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: updater.cancelDownload() }
            }
        }

        // —— 程序下载统计：速度 · 已下载 / 总量 ——
        Text {
            Layout.fillWidth: true
            visible: updater.downloading
            text: (updater.downloadSpeed.length > 0 ? updater.downloadSpeed + "   ·   " : "")
                  + updater.downloadedText + " / " + updater.totalText
            font.pixelSize: 11
            color: Theme.textSecondary
        }

        // —— 状态行：程序=updater.status；内核/GeoIP=各自进行中的状态 ——
        Text {
            Layout.fillWidth: true
            readonly property string s: win.currentTab === 1 ? (settings.coreUpdating ? settings.coreUpdateStatus : "")
                                        : win.currentTab === 2 ? (settings.geoipUpdating ? settings.geoipStatus : "")
                                                               : updater.status
            visible: s.length > 0
            text: s
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.textMuted
        }

        // —— 底部动作行 ——
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 国内代理下载（与设置页「国内加速」共用 settings.mirror；程序/内核下载都用它）
            RowLayout {
                spacing: 5
                Rectangle {
                    width: 16; height: 16; radius: 3
                    color: settings.mirror ? Theme.accent : "transparent"
                    border.width: 1
                    border.color: settings.mirror ? Theme.accent : Theme.divider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"; font.pixelSize: 11; color: "#ffffff"
                        visible: settings.mirror
                    }
                    TapHandler { onTapped: settings.setMirror(!settings.mirror) }
                }
                Text {
                    text: qsTr("国内代理下载")
                    font.pixelSize: 12
                    color: Theme.textSecondary
                    TapHandler { onTapped: settings.setMirror(!settings.mirror) }
                }
            }

            Item { Layout.fillWidth: true }

            // 关闭
            Rectangle {
                Layout.preferredWidth: 80
                Layout.preferredHeight: 30
                radius: 5
                color: closeHover.hovered ? Theme.hover : (Theme.dark ? "#252525" : "#eeeeee")
                border.width: 1
                border.color: Theme.divider
                Text {
                    anchors.centerIn: parent
                    text: qsTr("关闭")
                    font.pixelSize: 13
                    color: Theme.textSecondary
                }
                HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: win.close() }
            }

            // 更新（当前 tab 的动作）
            Rectangle {
                id: updateBtn
                Layout.preferredWidth: 100
                Layout.preferredHeight: 30
                radius: 5
                enabled: !win.busy
                opacity: win.busy ? 0.5 : 1.0
                color: updHover.hovered && !win.busy ? Theme.accentStrong : Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: win.busy ? qsTr("更新中…") : qsTr("更新")
                    font.pixelSize: 13
                    color: "#ffffff"
                }
                HoverHandler { id: updHover; cursorShape: win.busy ? Qt.ArrowCursor : Qt.PointingHandCursor }
                TapHandler {
                    enabled: !win.busy
                    onTapped: win.doUpdate()
                }
            }
        }
    }
}
