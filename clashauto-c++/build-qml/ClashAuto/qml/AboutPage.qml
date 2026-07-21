import QtQuick
import QtQuick.Layouts
import ClashAuto

// 关于页：居中 logo + 「Clash Auto」标题 + 版本行(点击查更新，发现新版本转红) + 简介 + 链接行。
// 版本来自 bridge.version；检查更新走 about 适配器（src/qml/AboutController）。
Item {
    id: page

    // 链接数据（label + url）；点击用系统浏览器打开。
    readonly property var links: [
        { label: qsTr("官网/文档"), url: "https://clashr-auto.gitbook.io" },
        { label: qsTr("项目地址"), url: "https://github.com/clashrauto/clashr-auto-desktop" },
        { label: qsTr("用户讨论"), url: "https://t.me/clashr_auto" }
    ]

    // 一次性的检查结果提示（已最新 / 失败），淡入淡出。
    property string hint: ""
    property color hintColor: Theme.textMuted

    Card {
        anchors.fill: parent
        anchors.margins: 10

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 420)
            spacing: 14

            // —— logo（与侧栏同一 iconfont 字形，随主题/品牌色）——
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: ""
                font.family: Theme.iconFont
                font.pixelSize: 84
                color: Theme.accent
            }

            // —— 标题 ——
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "Clash Auto"
                font.pixelSize: 30
                font.bold: true
                color: Theme.textPrimary
            }

            // —— 版本行：平时绿色「点击检查更新」；发现新版本转红「有新版本」点击打开发布页 ——
            Text {
                id: versionLine
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 14
                font.bold: about.updateAvailable
                color: about.checking ? Theme.textMuted
                     : about.updateAvailable ? Theme.danger
                     : "#4da13e"
                text: about.checking
                          ? qsTr("正在检查更新…")
                          : about.updateAvailable
                              ? ("v" + bridge.version + "  →  " + about.latestVersion + qsTr("  有新版本"))
                              : (qsTr("版本 ") + bridge.version + qsTr("  ·  点击检查更新"))

                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    enabled: !about.checking
                    onTapped: {
                        if (about.updateAvailable)
                            Qt.openUrlExternally(about.releaseUrl)
                        else
                            about.check()
                    }
                }
            }

            // —— 一次性提示（已最新 / 检查失败）——
            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: page.hintColor
                text: page.hint
                visible: page.hint.length > 0
                opacity: visible ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 180 } }
            }

            // —— 简介 ——
            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 13
                lineHeight: 1.35
                color: Theme.textSecondary
                text: qsTr("基于 Clash / mihomo 内核的跨平台代理客户端，自动订阅、节点测速、规则分流，简单好用。")
            }

            Item { Layout.preferredHeight: 6 }

            // —— 链接行（药丸按钮，系统浏览器打开）——
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                Repeater {
                    model: page.links
                    delegate: Rectangle {
                        required property var modelData
                        implicitWidth: linkText.implicitWidth + 26
                        implicitHeight: 30
                        radius: 15
                        color: linkHover.hovered ? Theme.hover : Theme.metricBg
                        border.width: 1
                        border.color: linkHover.hovered ? Theme.accent : Theme.divider

                        Text {
                            id: linkText
                            anchors.centerIn: parent
                            text: modelData.label
                            font.pixelSize: 12
                            color: linkHover.hovered ? Theme.accentStrong : Theme.textSecondary
                        }
                        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: Qt.openUrlExternally(modelData.url) }
                    }
                }
            }
        }
    }

    // 检查结果 → 一次性提示文案
    Connections {
        target: about
        function onUpToDate() {
            page.hintColor = Theme.textMuted
            page.hint = qsTr("已是最新版本")
        }
        function onCheckFailed(reason) {
            page.hintColor = Theme.danger
            page.hint = qsTr("检查失败：") + reason
        }
        function onUpdateAvailableChanged() {
            if (about.updateAvailable)
                page.hint = ""
        }
        function onCheckingChanged() {
            if (about.checking)
                page.hint = ""
        }
    }
}
