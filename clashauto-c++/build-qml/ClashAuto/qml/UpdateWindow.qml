import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 更新窗 —— 独立窗口，复刻 Widgets 版 MainWindow::showUpdateDialog()：
// 标题「Clash Auto 更新」600×560；左侧竖排 tab（正式版/测试版/内核）；每个 tab：VERSION 行 +
// 「更新说明」正文（只读）+「下载资源」列表（可选）；底部：进度条 + 不再提示 / 国内代理下载 /
// 关闭 / 一键更新。逻辑全部走 `update`（src/qml/UpdateController）。
ApplicationWindow {
    id: win
    width: 600
    height: 560
    minimumWidth: 460
    minimumHeight: 420
    title: qsTr("Clash Auto 更新")
    color: Theme.card

    property int currentTab: 0        // 0 正式版 / 1 测试版 / 2 内核
    property int selectedRelease: -1  // 正式版选中资源下标
    property int selectedBeta: -1     // 测试版选中资源下标

    // 打开即拉 release 列表 + 内核版本；关闭停留（下次打开再刷新）。
    onVisibleChanged: if (visible) updater.refresh()

    // 数据到达后自动选中推荐资源（优先 setup 安装包），一开窗「一键更新」即可点。
    Connections {
        target: updater
        function onReleaseChanged() { win.selectedRelease = updater.recommendedIndex(0) }
        function onBetaChanged() { win.selectedBeta = updater.recommendedIndex(1) }
        function onFailed(reason) { /* status 已展示；此处不额外弹窗，保持窗内提示 */ }
    }

    // 竖排 tab 按钮
    component TabButton_: Rectangle {
        property int idx: 0
        property string label: ""
        Layout.preferredWidth: 64
        Layout.preferredHeight: 44
        radius: 5
        color: win.currentTab === idx ? Theme.accent : (Theme.dark ? "#252525" : "#eeeeee")
        Text {
            anchors.centerIn: parent
            text: label
            font.pixelSize: 13
            color: win.currentTab === idx ? "#ffffff" : Theme.textSecondary
        }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: win.currentTab = idx }
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
                TabButton_ { idx: 0; label: qsTr("正式版") }
                TabButton_ { idx: 1; label: qsTr("测试版") }
                TabButton_ { idx: 2; label: qsTr("内核") }
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

                // ——— 正式版 / 测试版 ———
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4
                    visible: win.currentTab === 0 || win.currentTab === 1

                    Text {
                        text: win.currentTab === 1 ? updater.betaVersion : updater.releaseVersion
                        font.pixelSize: 15
                        font.bold: true
                        color: Theme.textPrimary
                    }
                    Text {
                        text: qsTr("更新说明")
                        font.pixelSize: 12
                        color: Theme.textMuted
                    }
                    // 更新说明正文（只读、可滚动）
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: 180
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
                            contentHeight: notesText.implicitHeight
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            Text {
                                id: notesText
                                width: notesFlick.width
                                text: win.currentTab === 1 ? updater.betaNotes : updater.releaseNotes
                                wrapMode: Text.WordWrap
                                font.pixelSize: 12
                                lineHeight: 1.3
                                color: Theme.textSecondary
                            }
                        }
                    }
                    Text {
                        text: qsTr("下载资源")
                        font.pixelSize: 12
                        color: Theme.textMuted
                    }
                    // 资源列表（单选）
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 120
                        radius: 4
                        color: Theme.dark ? "#0d0d0d" : "#ffffff"
                        border.width: 1
                        border.color: Theme.divider
                        ListView {
                            id: assetList
                            anchors.fill: parent
                            anchors.margins: 4
                            clip: true
                            model: win.currentTab === 1 ? updater.betaAssets : updater.releaseAssets
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            delegate: Rectangle {
                                required property int index
                                required property string modelData
                                width: ListView.view.width
                                height: 28
                                radius: 4
                                readonly property bool sel: (win.currentTab === 1 ? win.selectedBeta : win.selectedRelease) === index
                                color: sel ? Theme.accent : (rowHover.hovered ? Theme.hover : "transparent")
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    text: modelData
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 12
                                    color: sel ? "#ffffff" : Theme.textSecondary
                                }
                                HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: {
                                        if (win.currentTab === 1) win.selectedBeta = index
                                        else win.selectedRelease = index
                                    }
                                }
                            }
                        }
                    }
                }

                // ——— 内核 ———
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6
                    visible: win.currentTab === 2

                    Text {
                        Layout.fillWidth: true
                        text: updater.coreVersion.length > 0 ? updater.coreVersion : qsTr("内核版本: 检测中...")
                        font.pixelSize: 15
                        font.bold: true
                        wrapMode: Text.WordWrap
                        color: Theme.textPrimary
                    }
                    Text {
                        text: qsTr("更新说明")
                        font.pixelSize: 12
                        color: Theme.textMuted
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 4
                        color: Theme.dark ? "#0d0d0d" : "#ffffff"
                        border.width: 1
                        border.color: Theme.divider
                        Flickable {
                            id: coreFlick
                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true
                            contentWidth: width
                            contentHeight: coreNotesText.implicitHeight
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            Text {
                                id: coreNotesText
                                width: coreFlick.width
                                text: updater.coreNotes
                                wrapMode: Text.WordWrap
                                font.pixelSize: 12
                                lineHeight: 1.3
                                color: Theme.textSecondary
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        // 「更新内核」：与设置页同一流程（SettingsController::updateCore，用当前镜像偏好）。
                        Rectangle {
                            Layout.preferredWidth: 110
                            Layout.preferredHeight: 30
                            radius: 5
                            color: coreBtnHover.hovered ? Theme.accentStrong : Theme.accent
                            Text {
                                anchors.centerIn: parent
                                text: qsTr("更新内核")
                                font.pixelSize: 13
                                color: "#ffffff"
                            }
                            HoverHandler { id: coreBtnHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: settings.updateCore() }
                        }
                    }
                }
            }
        }

        // —— 进度条 ——
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            radius: 4
            visible: updater.downloading
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

        // —— 状态行（下载 / 校验 / 失败提示）——
        Text {
            Layout.fillWidth: true
            visible: updater.status.length > 0
            text: updater.status
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.textMuted
        }

        // —— 底部动作行 ——
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // 不再提示（勾选并关闭后记住跳过当前正式版）
            RowLayout {
                spacing: 5
                Rectangle {
                    id: noTipBox
                    property bool checked: false
                    width: 16; height: 16; radius: 3
                    color: checked ? Theme.accent : "transparent"
                    border.width: 1
                    border.color: checked ? Theme.accent : Theme.divider
                    Text {
                        anchors.centerIn: parent
                        text: "✓"; font.pixelSize: 11; color: "#ffffff"
                        visible: noTipBox.checked
                    }
                    TapHandler { onTapped: noTipBox.checked = !noTipBox.checked }
                }
                Text {
                    text: qsTr("不再提示")
                    font.pixelSize: 12
                    color: Theme.textSecondary
                    TapHandler { onTapped: noTipBox.checked = !noTipBox.checked }
                }
            }

            Item { Layout.fillWidth: true }

            // 国内代理下载（与设置页「国内加速」共用 settings.mirror）
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
                TapHandler {
                    onTapped: {
                        if (noTipBox.checked) updater.skipCurrentRelease()
                        win.close()
                    }
                }
            }

            // 一键更新
            Rectangle {
                id: updateBtn
                Layout.preferredWidth: 100
                Layout.preferredHeight: 30
                radius: 5
                // 内核 tab 用页内「更新内核」，底部一键更新置灰；正式/测试需已选资源且非下载中。
                readonly property int sel: win.currentTab === 1 ? win.selectedBeta : win.selectedRelease
                readonly property bool ready: win.currentTab !== 2 && sel >= 0 && !updater.downloading
                enabled: ready
                opacity: ready ? 1.0 : 0.5
                color: updHover.hovered && ready ? Theme.accentStrong : Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: updater.downloading ? qsTr("下载中…") : qsTr("一键更新")
                    font.pixelSize: 13
                    color: "#ffffff"
                }
                HoverHandler { id: updHover; cursorShape: updateBtn.ready ? Qt.PointingHandCursor : Qt.ArrowCursor }
                TapHandler {
                    enabled: updateBtn.ready
                    onTapped: updater.oneClickUpdate(win.currentTab, updateBtn.sel, settings.mirror)
                }
            }
        }
    }
}
