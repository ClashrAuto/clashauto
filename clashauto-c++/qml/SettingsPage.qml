import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ClashAuto

// 设置页：TabBar(系统/过滤/区域/规则) + 表单，复刻 Widgets buildSettingsPage()。
// 后端经 context 属性 `settings`（SettingsController）。系统/过滤 tab 由右上「应用」按钮整表落盘；
// 区域/规则 tab 是可编辑表（增/改/删），改动即时重建配置。样式全部走 Theme 令牌（深/浅）。
Item {
    id: page

    // ————————————————————————— 复用样式组件（inline）—————————————————————————

    component GroupTitle: Label {
        color: Theme.accentStrong
        font.pixelSize: 13
        font.bold: true
        topPadding: 6
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        height: 1
        color: Theme.divider
    }

    component RowLabel: Label {
        Layout.preferredWidth: 150
        color: Theme.textSecondary
        font.pixelSize: 13
    }

    // 主题化开关
    component ThemedSwitch: Switch {
        id: sw
        implicitWidth: 46
        implicitHeight: 24
        padding: 0
        indicator: Rectangle {
            width: 40
            height: 20
            radius: 10
            x: 0
            anchors.verticalCenter: parent.verticalCenter
            color: sw.checked ? Theme.accent : Theme.switchTrackOff
            border.width: 1
            border.color: sw.checked ? Theme.accent : Theme.inputBorder
            Behavior on color { ColorAnimation { duration: 120 } }
            Rectangle {
                width: 16
                height: 16
                radius: 8
                y: 2
                x: sw.checked ? parent.width - width - 2 : 2
                color: "white"
                Behavior on x { NumberAnimation { duration: 120 } }
            }
        }
        contentItem: Item {}
    }

    // 主题化单行输入框
    component ThemedField: TextField {
        id: tf
        implicitWidth: 200
        color: Theme.textPrimary
        placeholderTextColor: Theme.textMuted
        font.pixelSize: 13
        selectByMouse: true
        background: Rectangle {
            radius: 3
            color: Theme.inputBg
            border.width: 1
            border.color: tf.activeFocus ? Theme.accent : Theme.inputBorder
        }
    }

    // 主题化下拉（不可编辑）
    component ThemedCombo: ComboBox {
        id: cb
        implicitWidth: 200
        implicitHeight: 30
        font.pixelSize: 13
        background: Rectangle {
            radius: 3
            color: Theme.inputBg
            border.width: 1
            border.color: cb.activeFocus ? Theme.accent : Theme.inputBorder
        }
        contentItem: Text {
            leftPadding: 8
            rightPadding: cb.indicator.width + 4
            text: cb.displayText
            color: Theme.textPrimary
            font: cb.font
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Text {
            x: cb.width - width - 8
            y: (cb.height - height) / 2
            text: "▾"
            color: Theme.textMuted
            font.pixelSize: 12
        }
        delegate: ItemDelegate {
            width: cb.width
            height: 28
            contentItem: Text {
                text: modelData
                color: Theme.textPrimary
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
                leftPadding: 6
                elide: Text.ElideRight
            }
            background: Rectangle {
                color: highlighted ? Theme.hover : "transparent"
            }
            highlighted: cb.highlightedIndex === index
        }
        popup: Popup {
            y: cb.height
            width: cb.width
            implicitHeight: Math.min(contentItem.implicitHeight + 2, 240)
            padding: 1
            background: Rectangle {
                radius: 3
                color: Theme.card
                border.width: 1
                border.color: Theme.inputBorder
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: cb.delegateModel
                currentIndex: cb.highlightedIndex
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }

    // 主题化可编辑下拉（Host / 允许规则 / 排除规则）——带预置项且允许手输
    component ThemedEditCombo: ComboBox {
        id: ec
        editable: true
        implicitWidth: 300
        implicitHeight: 30
        font.pixelSize: 13
        selectTextByMouse: true
        background: Rectangle {
            radius: 3
            color: Theme.inputBg
            border.width: 1
            border.color: ec.activeFocus ? Theme.accent : Theme.inputBorder
        }
        contentItem: TextField {
            text: ec.editText
            onTextEdited: ec.editText = text
            color: Theme.textPrimary
            font: ec.font
            selectByMouse: true
            leftPadding: 8
            rightPadding: ec.indicator.width + 4
            verticalAlignment: Text.AlignVCenter
            background: null
        }
        indicator: Text {
            x: ec.width - width - 8
            y: (ec.height - height) / 2
            text: "▾"
            color: Theme.textMuted
            font.pixelSize: 12
        }
        delegate: ItemDelegate {
            width: ec.width
            height: 28
            contentItem: Text {
                text: modelData
                color: Theme.textPrimary
                font.pixelSize: 13
                verticalAlignment: Text.AlignVCenter
                leftPadding: 6
                elide: Text.ElideRight
            }
            background: Rectangle { color: highlighted ? Theme.hover : "transparent" }
            highlighted: ec.highlightedIndex === index
        }
        popup: Popup {
            y: ec.height
            width: ec.width
            implicitHeight: Math.min(contentItem.implicitHeight + 2, 240)
            padding: 1
            background: Rectangle {
                radius: 3
                color: Theme.card
                border.width: 1
                border.color: Theme.inputBorder
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: ec.delegateModel
                currentIndex: ec.highlightedIndex
                ScrollBar.vertical: ScrollBar {}
            }
        }
    }

    // 主题化数字框
    component ThemedSpin: SpinBox {
        id: sp
        implicitWidth: 200
        implicitHeight: 30
        from: 0
        to: 1440
        editable: true
        font.pixelSize: 13
        contentItem: TextInput {
            text: sp.textFromValue(sp.value, sp.locale)
            color: Theme.textPrimary
            font: sp.font
            selectByMouse: true
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !sp.editable
            validator: sp.validator
            onTextChanged: {
                var v = parseInt(text)
                if (!isNaN(v)) sp.value = Math.max(sp.from, Math.min(sp.to, v))
            }
        }
        background: Rectangle {
            radius: 3
            color: Theme.inputBg
            border.width: 1
            border.color: sp.activeFocus ? Theme.accent : Theme.inputBorder
        }
        up.indicator: Rectangle {
            x: sp.width - width
            height: parent.height / 2
            implicitWidth: 24
            color: sp.up.pressed ? Theme.hover : "transparent"
            Text { anchors.centerIn: parent; text: "+"; color: Theme.textSecondary; font.pixelSize: 13 }
        }
        down.indicator: Rectangle {
            x: sp.width - width
            y: parent.height / 2
            height: parent.height / 2
            implicitWidth: 24
            color: sp.down.pressed ? Theme.hover : "transparent"
            Text { anchors.centerIn: parent; text: "−"; color: Theme.textSecondary; font.pixelSize: 13 }
        }
    }

    // 主题化按钮
    component PillButton: Button {
        id: btn
        property bool primary: false
        implicitHeight: 30
        padding: 8
        font.pixelSize: 13
        contentItem: Text {
            text: btn.text
            color: btn.primary ? "white" : Theme.textPrimary
            font: btn.font
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight // 定宽按钮（如「应用」）长译文省略号，不溢出（自适应宽按钮不受影响）
            opacity: btn.enabled ? 1 : 0.5
        }
        background: Rectangle {
            radius: 4
            color: btn.primary
                   ? (btn.down ? Theme.accentStrong : Theme.accent)
                   : (btn.down ? Theme.hover : Theme.inputBg)
            border.width: btn.primary ? 0 : 1
            border.color: Theme.inputBorder
            opacity: btn.enabled ? 1 : 0.6
        }
    }

    component SettingTab: TabButton {
        id: tb
        implicitWidth: 84
        implicitHeight: 32
        contentItem: Text {
            text: tb.text
            font.pixelSize: 14
            color: tb.checked ? Theme.accent : Theme.textSecondary
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight // 长译文（如某些语言的「区域/规则」）在标签内省略号，不溢出到相邻标签
        }
        background: Rectangle {
            color: "transparent"
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 2
                color: tb.checked ? Theme.accent : "transparent"
            }
        }
    }

    // ————————————————————————— 数据/状态 —————————————————————————

    // 当前编辑对话框的目标索引（-1 = 新增）
    property int editRuleIndex: -1
    property int editAreaIndex: -1

    Component.onCompleted: settings.refreshMacHelperStatus()

    // ————————————————————————— 布局 —————————————————————————

    Card {
        anchors.fill: parent
        anchors.margins: 10

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            // —— 顶栏：Tab + 应用 ——
            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    background: Rectangle { color: "transparent" }

                    SettingTab { text: qsTr("系统") }
                    SettingTab { text: qsTr("过滤") }
                    SettingTab { text: qsTr("区域") }
                    SettingTab { text: qsTr("规则") }
                }

                PillButton {
                    text: qsTr("应用")
                    primary: true
                    implicitWidth: 82
                    visible: tabs.currentIndex === 0 || tabs.currentIndex === 1
                    onClicked: {
                        settings.apply(
                            hostCombo.editText,
                            parseInt(uiField.text) || 0,
                            parseInt(mixedField.text) || 0,
                            webSwitch.checked,
                            nodeSwitch.checked,
                            clearSwitch.checked,
                            incSwitch.checked,
                            traySwitch.checked,
                            autoStartSwitch.checked,
                            noteSwitch.checked,
                            mirrorCheck.checked,
                            autoUpdateSpin.value,
                            themeCombo.currentIndex === 1,   // themeLight
                            autoThemeSwitch.checked,
                            langCombo.langCodes[langCombo.currentIndex],
                            autoLangSwitch.checked,
                            allowCombo.editText,
                            allowSwitch.checked,
                            blockCombo.editText,
                            blockSwitch.checked)
                        // 「跟随系统深浅色」实时生效：更新 bridge 的跟随态；开启则立刻按当前系统外观切主题。
                        bridge.setAutoTheme(autoThemeSwitch.checked)
                        // 「关闭到托盘」：更新 bridge。✕ 关闭行为即时生效（Main.qml onClosing）；
                        // 「启动是否静默到托盘」下次启动生效（Main.qml Component.onCompleted）。
                        bridge.setCloseToTray(traySwitch.checked)
                        // 「切换通知」实时生效：更新 bridge（决定切换节点是否发系统通知）。
                        // 关→开会顺带重注册系统通知（重显托盘图标），尝试恢复此前失效的通知。
                        bridge.setNodeSwitchNote(noteSwitch.checked)
                    }
                }
            }

            // 反馈提示
            Text {
                Layout.fillWidth: true
                text: settings.lastMessage
                color: Theme.textMuted
                font.pixelSize: 11
                elide: Text.ElideRight
                visible: text.length > 0
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabs.currentIndex

                // ============================ TAB 0：系统 ============================
                ScrollView {
                    clip: true
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded

                    ColumnLayout {
                        width: page.width - 60
                        spacing: 8

                        GroupTitle { text: qsTr("系统") }
                        RowLayout { RowLabel { text: qsTr("开机自启") }
                            ThemedSwitch { id: autoStartSwitch; checked: settings.autoStart } }
                        RowLayout { RowLabel { text: qsTr("关闭到托盘") }
                            ThemedSwitch { id: traySwitch; checked: settings.closeToTray } }

                        Divider {}
                        GroupTitle { text: qsTr("节点") }
                        RowLayout { RowLabel { text: qsTr("仅可用节点") }
                            ThemedSwitch { id: nodeSwitch; checked: settings.nodeOnlyAvailable
                                onToggled: settings.setNodeOnly(checked) } }
                        RowLayout { RowLabel { text: qsTr("增量更新") }
                            ThemedSwitch { id: incSwitch; checked: settings.increment } }

                        Divider {}
                        GroupTitle { text: qsTr("通知") }
                        RowLayout { RowLabel { text: qsTr("切换通知") }
                            ThemedSwitch { id: noteSwitch; checked: settings.nodeSwitchNote } }

                        Divider {}
                        GroupTitle { text: qsTr("增强") }
                        RowLayout { RowLabel { text: qsTr("系统代理") }
                            ThemedSwitch { id: webSwitch; checked: settings.webProxy } }
                        RowLayout {
                            spacing: 10
                            RowLabel { text: qsTr("GeoIP 数据") }
                            PillButton {
                                text: settings.geoipUpdating ? settings.geoipStatus : qsTr("更新 GeoIP")
                                enabled: !settings.geoipUpdating
                                implicitWidth: 120
                                onClicked: settings.updateGeoip()
                            }
                        }
                        RowLayout {
                            spacing: 10
                            RowLabel { text: qsTr("mihomo 内核") }
                            CheckBox {
                                id: mirrorCheck
                                text: qsTr("国内加速")
                                checked: settings.mirror
                                font.pixelSize: 13
                                onToggled: settings.setMirror(checked)
                                indicator: Rectangle {
                                    implicitWidth: 16; implicitHeight: 16; radius: 3
                                    x: 0; anchors.verticalCenter: parent.verticalCenter
                                    color: mirrorCheck.checked ? Theme.accent : Theme.inputBg
                                    border.width: 1
                                    border.color: mirrorCheck.checked ? Theme.accent : Theme.inputBorder
                                    Text { anchors.centerIn: parent; text: "✓"; color: "white"
                                        font.pixelSize: 11; visible: mirrorCheck.checked }
                                }
                                contentItem: Text {
                                    text: mirrorCheck.text; color: Theme.textSecondary
                                    font: mirrorCheck.font; leftPadding: 22
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                            PillButton {
                                text: settings.coreUpdating ? settings.coreUpdateStatus : qsTr("更新内核")
                                enabled: !settings.coreUpdating
                                implicitWidth: 120
                                onClicked: settings.updateCore()
                            }
                        }

                        // macOS 免密助手
                        RowLayout {
                            spacing: 10
                            visible: settings.macHelperSupported
                            RowLabel { text: qsTr("免密助手") }
                            Text {
                                text: settings.macHelperStatus
                                color: Theme.textMuted
                                font.pixelSize: 12
                                Layout.preferredWidth: 220
                                elide: Text.ElideRight
                            }
                            PillButton { text: qsTr("安装/启用"); onClicked: settings.installMacHelper() }
                            PillButton { text: qsTr("卸载"); onClicked: settings.uninstallMacHelper() }
                        }

                        Divider {}
                        GroupTitle { text: qsTr("界面") }
                        RowLayout {
                            RowLabel { text: qsTr("主题") }
                            ThemedCombo {
                                id: themeCombo
                                model: [qsTr("黑色"), qsTr("白色")]
                                currentIndex: Theme.dark ? 0 : 1
                                onActivated: Theme.dark = (currentIndex === 0)
                            }
                        }
                        RowLayout { RowLabel { text: qsTr("跟随系统深浅色") }
                            ThemedSwitch { id: autoThemeSwitch; checked: settings.autoTheme } }

                        Divider {}
                        GroupTitle { text: qsTr("内核") }
                        RowLayout {
                            RowLabel { text: qsTr("Host") }
                            ThemedEditCombo {
                                id: hostCombo
                                model: ["127.0.0.1", "localhost"]
                                editText: settings.host
                                implicitWidth: 200
                            }
                        }
                        RowLayout { RowLabel { text: qsTr("端口") }
                            ThemedField { id: mixedField; text: String(settings.mixedPort)
                                inputMethodHints: Qt.ImhDigitsOnly } }
                        RowLayout {
                            RowLabel { text: qsTr("API 端口") }
                            ThemedField { id: uiField; text: String(settings.uiPort)
                                inputMethodHints: Qt.ImhDigitsOnly
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("mihomo REST API 端口；默认 9191，改后会重启核心") }
                        }
                        RowLayout { RowLabel { text: qsTr("切换时清理连接") }
                            ThemedSwitch { id: clearSwitch; checked: settings.clearConnections } }

                        Divider {}
                        GroupTitle { text: qsTr("其他") }
                        RowLayout { RowLabel { text: qsTr("跟随系统语言") }
                            ThemedSwitch { id: autoLangSwitch; checked: settings.autoLanguage } }
                        RowLayout {
                            RowLabel { text: qsTr("语言") }
                            ThemedCombo {
                                id: langCombo
                                enabled: !autoLangSwitch.checked // 跟随系统时手选无效，置灰
                                // 显示各语言本名（不翻译），值存语言码；apply 传 langCodes[currentIndex]、
                                // 初值按 settings.language 反查。model 与 langCodes 一一对应、顺序一致。
                                readonly property var langCodes: ["zh-CN", "en-US", "zh-TW", "ja", "ko", "ru", "es", "fr", "de", "pt-BR", "it", "tr", "vi"]
                                model: ["简体中文", "English", "繁體中文", "日本語", "한국어", "Русский", "Español", "Français", "Deutsch", "Português", "Italiano", "Türkçe", "Tiếng Việt"]
                                currentIndex: Math.max(0, langCodes.indexOf(settings.language))
                            }
                        }
                        RowLayout {
                            RowLabel { text: qsTr("订阅自动更新周期") }
                            ThemedSpin { id: autoUpdateSpin; value: settings.autoUpdateMinutes }
                            Label { text: qsTr("分钟"); color: Theme.textMuted; font.pixelSize: 12 }
                        }
                        Item { Layout.preferredHeight: 6 }
                    }
                }

                // ============================ TAB 1：过滤 ============================
                ColumnLayout {
                    spacing: 10

                    RowLayout {
                        RowLabel { text: qsTr("启用允许规则") }
                        ThemedSwitch { id: allowSwitch; checked: settings.allowRuleEnabled }
                    }
                    RowLayout {
                        RowLabel { text: qsTr("允许规则(正则)") }
                        ThemedEditCombo {
                            id: allowCombo
                            model: settings.allowRulePresets
                            editText: settings.allowRule
                        }
                    }
                    RowLayout {
                        RowLabel { text: qsTr("启用排除规则") }
                        ThemedSwitch { id: blockSwitch; checked: settings.noAllowRuleEnabled }
                    }
                    RowLayout {
                        RowLabel { text: qsTr("排除规则(正则)") }
                        ThemedEditCombo {
                            id: blockCombo
                            model: settings.noAllowRulePresets
                            editText: settings.noAllowRule
                        }
                    }
                    Item { Layout.fillHeight: true }
                }

                // ============================ TAB 2：区域 ============================
                ColumnLayout {
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        PillButton {
                            text: qsTr("＋ 添加")
                            primary: true
                            implicitWidth: 96
                            onClicked: { page.editAreaIndex = -1; areaEditor.openWith({}) }
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: qsTr("共 %1 组").arg(settings.areaTotal)
                            color: Theme.textMuted
                            font.pixelSize: 12
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: settings.areaRows
                        spacing: 4
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 36
                            radius: 3
                            color: Theme.nodeRowBg
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 6
                                spacing: 8
                                Label { text: modelData.name; color: Theme.textPrimary
                                    font.pixelSize: 13; Layout.preferredWidth: 150; elide: Text.ElideRight }
                                Label { text: modelData.type; color: Theme.textMuted
                                    font.pixelSize: 12; Layout.preferredWidth: 90 }
                                Label { text: modelData.rule; color: Theme.textSecondary
                                    font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                                PillButton {
                                    text: "✎"; implicitWidth: 30; implicitHeight: 24
                                    onClicked: {
                                        page.editAreaIndex = modelData.index
                                        areaEditor.openWith(settings.areaAt(modelData.index))
                                    }
                                }
                            }
                        }
                    }
                }

                // ============================ TAB 3：规则 ============================
                ColumnLayout {
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        PillButton {
                            text: qsTr("＋ 添加")
                            primary: true
                            implicitWidth: 96
                            onClicked: { page.editRuleIndex = -1; ruleEditor.openWith({}) }
                        }
                        ThemedField {
                            id: ruleSearch
                            implicitWidth: 220
                            placeholderText: qsTr("搜索规则（类型/节点/值）")
                            onTextChanged: settings.setRuleFilter(text)
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: settings.ruleShown < settings.ruleTotal
                                  ? qsTr("共 %1 条，显示前 %2 条").arg(settings.ruleTotal).arg(settings.ruleShown)
                                  : qsTr("共 %1 条").arg(settings.ruleTotal)
                            color: Theme.textMuted
                            font.pixelSize: 12
                        }
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: settings.ruleRows
                        spacing: 4
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 36
                            radius: 3
                            color: Theme.nodeRowBg
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 6
                                spacing: 8
                                Label { text: modelData.type; color: Theme.textPrimary
                                    font.pixelSize: 13; Layout.preferredWidth: 150; elide: Text.ElideRight }
                                Label { text: modelData.node; color: Theme.accentStrong
                                    font.pixelSize: 12; Layout.preferredWidth: 130; elide: Text.ElideRight }
                                Label { text: modelData.value; color: Theme.textSecondary
                                    font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                                PillButton {
                                    text: "✎"; implicitWidth: 30; implicitHeight: 24
                                    onClicked: {
                                        page.editRuleIndex = modelData.index
                                        ruleEditor.openWith(settings.ruleAt(modelData.index))
                                    }
                                }
                                PillButton {
                                    text: "✕"; implicitWidth: 30; implicitHeight: 24
                                    onClicked: settings.deleteRule(modelData.index)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ————————————————————————— 规则编辑器 —————————————————————————
    Popup {
        id: ruleEditor
        anchors.centerIn: Overlay.overlay
        width: 420
        modal: true
        padding: 16
        property var processNames: []
        property var processPaths: []
        background: Rectangle {
            radius: 6
            color: Theme.card
            border.width: 1
            border.color: Theme.inputBorder
        }

        function openWith(obj) {
            rTypeCombo.editText = obj.type ? obj.type : "DOMAIN-SUFFIX"
            rValueCombo.editText = obj.value ? obj.value : ""
            rNodeCombo.editText = obj.node ? obj.node : ""
            rNodeCombo.model = settings.proxyGroupNames()
            open()
        }

        function refreshValueChoices(type) {
            var t = String(type).toUpperCase()
            if (t === "PROCESS-NAME") {
                if (processNames.length === 0) processNames = settings.processChoices(false)
                rValueCombo.model = processNames
            } else if (t === "PROCESS-PATH") {
                if (processPaths.length === 0) processPaths = settings.processChoices(true)
                rValueCombo.model = processPaths
            } else {
                rValueCombo.model = []
            }
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label { text: page.editRuleIndex >= 0 ? qsTr("编辑规则") : qsTr("新增规则")
                color: Theme.textPrimary; font.pixelSize: 16; font.bold: true }

            Label { text: qsTr("类型"); color: Theme.textSecondary; font.pixelSize: 12 }
            ThemedEditCombo {
                id: rTypeCombo
                Layout.fillWidth: true
                model: ["DOMAIN-SUFFIX", "DOMAIN", "DOMAIN-KEYWORD", "DOMAIN-REGEX",
                        "IP-CIDR", "IP-CIDR6", "GEOIP", "GEOSITE", "IP-ASN",
                        "SRC-IP-CIDR", "SRC-PORT", "DST-PORT",
                        "PROCESS-NAME", "PROCESS-PATH", "RULE-SET", "MATCH"]
                onEditTextChanged: ruleEditor.refreshValueChoices(editText)
            }

            Label { text: qsTr("值 (域名/IP段；进程规则从下拉选；MATCH 留空)")
                color: Theme.textSecondary; font.pixelSize: 12 }
            ThemedEditCombo { id: rValueCombo; Layout.fillWidth: true }

            Label { text: qsTr("节点/策略组"); color: Theme.textSecondary; font.pixelSize: 12 }
            ThemedEditCombo { id: rNodeCombo; Layout.fillWidth: true }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                PillButton { text: qsTr("取消"); onClicked: ruleEditor.close() }
                PillButton {
                    text: qsTr("确定"); primary: true
                    onClicked: {
                        settings.saveRule(page.editRuleIndex, rTypeCombo.editText,
                                          rValueCombo.editText, rNodeCombo.editText)
                        ruleEditor.close()
                    }
                }
            }
        }
    }

    // ————————————————————————— 区域编辑器 —————————————————————————
    Popup {
        id: areaEditor
        anchors.centerIn: Overlay.overlay
        width: 440
        modal: true
        padding: 16
        background: Rectangle {
            radius: 6
            color: Theme.card
            border.width: 1
            border.color: Theme.inputBorder
        }

        function openWith(obj) {
            aNameField.text = obj.name ? obj.name : ""
            aNameField.readOnly = page.editAreaIndex >= 0   // 编辑时锁名（改名破坏引用）
            var idx = aTypeCombo.model.indexOf(obj.type ? obj.type : "select")
            aTypeCombo.currentIndex = idx >= 0 ? idx : 0
            aProxiesArea.text = obj.proxiesText ? obj.proxiesText : ""
            aUrlField.text = obj.url ? obj.url : ""
            aIntervalField.text = obj.interval ? obj.interval : ""
            open()
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label { text: page.editAreaIndex >= 0 ? qsTr("编辑区域分组") : qsTr("新增区域分组")
                color: Theme.textPrimary; font.pixelSize: 16; font.bold: true }

            Label { text: qsTr("名称"); color: Theme.textSecondary; font.pixelSize: 12 }
            ThemedField { id: aNameField; Layout.fillWidth: true }

            Label { text: qsTr("类型"); color: Theme.textSecondary; font.pixelSize: 12 }
            ThemedCombo {
                id: aTypeCombo
                Layout.fillWidth: true
                model: ["select", "url-test", "fallback", "load-balance", "relay"]
            }

            Label { text: qsTr("成员（每行一个：DIRECT / REJECT / 组名 / 节点名）")
                color: Theme.textSecondary; font.pixelSize: 12 }
            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: 120
                TextArea {
                    id: aProxiesArea
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    selectByMouse: true
                    wrapMode: TextArea.NoWrap
                    background: Rectangle {
                        radius: 3
                        color: Theme.inputBg
                        border.width: 1
                        border.color: aProxiesArea.activeFocus ? Theme.accent : Theme.inputBorder
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ThemedField {
                    id: aUrlField
                    Layout.fillWidth: true
                    placeholderText: qsTr("url（url-test/fallback，可留空用默认）")
                }
                ThemedField {
                    id: aIntervalField
                    implicitWidth: 90
                    placeholderText: qsTr("间隔秒")
                    inputMethodHints: Qt.ImhDigitsOnly
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                PillButton { text: qsTr("取消"); onClicked: areaEditor.close() }
                PillButton {
                    text: qsTr("确定"); primary: true
                    onClicked: {
                        settings.saveArea(page.editAreaIndex, aNameField.text,
                                          aTypeCombo.currentText, aProxiesArea.text,
                                          aUrlField.text, aIntervalField.text)
                        areaEditor.close()
                    }
                }
            }
        }
    }
}
