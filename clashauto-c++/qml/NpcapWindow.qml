import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// Npcap 安装窗 —— 独立顶层窗口，从设备页的「未安装 Npcap」提示条打开。
// 流程全自动：下载（进度条 + 速度 + ✕ 取消）→ 验数字签名 → 提权静默安装 → 复检。
// 窗内诚实写明两件事：装驱动必弹一次 UAC；免费版可能拒绝静默安装，届时自动改走可见向导。
ApplicationWindow {
    id: win
    transientParent: null
    flags: Qt.Window
    // 高度按「下载中」那一刻算：说明卡 + 进度条 + 速度行 + 状态行 + 按钮行都在时也不挤。
    // （给小了会看到进度条压在说明卡上——ColumnLayout 挤扁卡片，卡片里 anchors.fill 的文字并不会跟着缩。）
    width: 520
    height: 560
    minimumWidth: 420
    minimumHeight: 480
    title: qsTr("安装 Npcap")
    color: Theme.card

    // 打开即复检 + 查最新版本号。
    onVisibleChanged: if (visible) npcap.refresh()

    // 装好了就自动关窗（状态文字已经由设备页的提示条消失来体现）。
    Connections {
        target: npcap
        function onFinished(ok) { if (ok) closeTimer.start() }
    }
    Timer { id: closeTimer; interval: 1200; onTriggered: win.close() }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 10

        Text {
            text: qsTr("安装 Npcap 驱动")
            font.pixelSize: 16
            color: Theme.textPrimary
        }

        // —— 说明卡 ——
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 5
            clip: true // 窗口被拉到很矮时，卡内文字裁在卡里，不外溢压到进度条
            color: Theme.dark ? "#161616" : "#f6f6f6"
            border.width: 1
            border.color: Theme.divider

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    lineHeight: 1.35
                    color: Theme.textSecondary
                    text: qsTr("设备页的「代理网络」靠透明网关接管局域网设备的流量。"
                               + "在 Windows 上它需要 Npcap 驱动来收发二层数据帧 —— 没装的话，"
                               + "开关能打开，但流量不会真正被接管。")
                }

                // 当前状态 / 版本
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 5

                    Text { text: qsTr("本机状态"); font.pixelSize: 12; color: Theme.textMuted }
                    Text {
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        color: npcap.installed ? "#5bb44b" : "#c69a54"
                        text: npcap.installed
                              ? (qsTr("已安装") + (npcap.installedVersion ? " " + npcap.installedVersion : ""))
                              : qsTr("未安装")
                    }

                    Text { text: qsTr("最新版本"); font.pixelSize: 12; color: Theme.textMuted }
                    Text {
                        Layout.fillWidth: true
                        font.pixelSize: 12
                        color: Theme.textSecondary
                        text: npcap.latestVersion ? npcap.latestVersion : qsTr("查询中…")
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    lineHeight: 1.3
                    color: Theme.textMuted
                    text: qsTr("点「立即安装」后：自动下载官方安装包 → 校验数字签名 → 静默安装。\n"
                               + "· 安装驱动需要管理员权限，过程中会弹一次 UAC，请点「是」。\n"
                               + "· Npcap 免费版仅 OEM 授权允许静默安装；若静默被拒，会自动打开"
                               + "安装向导，按提示点 Install 即可。")
                }

                Item { Layout.fillHeight: true }
            }
        }

        // —— 下载进度条 + ✕ 取消 ——
        RowLayout {
            Layout.fillWidth: true
            visible: npcap.downloading
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
                    width: Math.max(0, (parent.width - 2) * npcap.progress / 100)
                    radius: 4
                    color: Theme.accent
                }
                Text {
                    anchors.centerIn: parent
                    text: npcap.progress + "%"
                    font.pixelSize: 11
                    color: Theme.textPrimary
                }
            }

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
                TapHandler { onTapped: npcap.cancel() }
            }
        }

        // 速度 · 已下载 / 总量
        Text {
            Layout.fillWidth: true
            visible: npcap.downloading
            text: (npcap.downloadSpeed.length > 0 ? npcap.downloadSpeed + "   ·   " : "")
                  + npcap.downloadedText + " / " + npcap.totalText
            font.pixelSize: 11
            color: Theme.textSecondary
        }

        // 状态行
        Text {
            Layout.fillWidth: true
            visible: npcap.status.length > 0
            text: npcap.status
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.textMuted
        }

        // —— 底部动作行 ——
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 这里没有「国内代理下载」勾选框：ghfast.top 只代理 GitHub，而安装包来自 npcap.com。
            // 核心在跑时下载会自动走本地混合端口，这是唯一（也够用）的绕行路径。
            Text {
                text: qsTr("下载源：npcap.com（官方）")
                font.pixelSize: 11
                color: Theme.textMuted
            }

            Item { Layout.fillWidth: true }

            // 官网下载（自动流程失败时的兜底）
            Text {
                text: qsTr("官网下载")
                font.pixelSize: 12
                color: Theme.accent
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: npcap.openHomepage() }
            }

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

            Rectangle {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 30
                radius: 5
                enabled: !npcap.busy && !npcap.installed
                opacity: enabled ? 1.0 : 0.5
                color: instHover.hovered && enabled ? Theme.accentStrong : Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: npcap.installed ? qsTr("已安装")
                                          : npcap.busy ? qsTr("处理中…") : qsTr("立即安装")
                    font.pixelSize: 13
                    color: "#ffffff"
                }
                HoverHandler {
                    id: instHover
                    cursorShape: parent.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                }
                TapHandler {
                    enabled: !npcap.busy && !npcap.installed
                    onTapped: npcap.install()
                }
            }
        }
    }
}
