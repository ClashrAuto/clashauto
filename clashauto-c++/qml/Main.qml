import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 应用主窗（shell）：左侧栏(logo/导航/版本) + 中间页面区(StackLayout) + 底部页脚(开关/模式)。
// macOS：保留系统窗（透明标题栏 + 整窗毛玻璃，见 bridge.applyMacGlass），内容为浮在玻璃上的
// 不透明圆角卡片（离窗顶/右 5px）；其它平台：无边框 + 背景拖动。
ApplicationWindow {
    id: window
    visible: true
    width: 900
    height: 510
    minimumWidth: 640
    minimumHeight: 430
    title: "Clash Auto"

    readonly property bool isMac: Qt.platform.os === "osx"
    readonly property bool isWin: Qt.platform.os === "windows"
    property int currentPage: 0

    // 所有平台都用系统原生窗（带标题栏）。mac 由 applyMacGlass 把标题栏变透明+整窗毛玻璃；
    // Windows 由 applyWindowsTitleBar 用 DWM 把标题栏背景染成窗口壳色；Linux 保持系统原生标题栏
    // （颜色由 WM 决定，应用无法强制）。不再对 Win/Linux 用无边框。
    flags: Qt.Window
    // mac 透明底露玻璃；其它平台用壳色（标题栏在 Windows 上也会被染成此色）。
    color: isMac ? "transparent" : Theme.shell

    // 平台窗口装饰：mac 毛玻璃 / Windows 标题栏染色 / Linux 无操作。
    function applyChrome() {
        if (isMac)
            bridge.applyMacGlass(window, Theme.dark);
        else if (isWin)
            bridge.applyWindowsTitleBar(window, Theme.shell, Theme.dark);
    }

    Component.onCompleted: {
        Theme.dark = bridge.initialDark;
        applyChrome();
    }
    onVisibleChanged: if (visible) applyChrome()
    // 主题切换后毛玻璃深浅 / 标题栏颜色需重设
    Connections {
        target: Theme
        function onDarkChanged() { window.applyChrome(); }
    }

    // 背景拖动：按住窗口任意「非交互」空白/文字/卡片背景即可拖动整窗。铺满窗口并置于所有
    // 内容之后(z:-1)。列表/ComboBox(select) 等控件在其上层先占用按下事件；本 handler 用
    // 「不夺取」的 grabPermissions（无 CanTakeOver 位），故在这些控件上按住拖动不会移动窗口，
    // 只有空白/文字处才触发。mac 由原生透明标题栏负责移动，仅非 mac 调 startSystemMove。
    Item {
        anchors.fill: parent
        z: -1
        DragHandler {
            target: null
            grabPermissions: PointerHandler.ApprovesTakeOverByItems
                             | PointerHandler.ApprovesTakeOverByHandlersOfDifferentType
                             | PointerHandler.ApprovesTakeOverByHandlersOfSameType
            onActiveChanged: if (active && !window.isMac) window.startSystemMove()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ———————————————————— 侧栏 ————————————————————
        Item {
            Layout.preferredWidth: Theme.sidebarWidth
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 16
                anchors.bottomMargin: 5
                spacing: 0

                // logo（网络图标，随状态变色）——窗口拖动由上方窗口级背景 handler 统一负责。
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 118
                    Text {
                        anchors.centerIn: parent
                        text: ""
                        font.family: Theme.iconFont
                        font.pixelSize: 70
                        color: bridge.tunEnabled ? "#ff0000"
                             : bridge.coreRunning ? "#ffff00"
                             : (Theme.dark ? "#ffffff" : "#333333")
                    }
                }

                // 导航（与 StackLayout 索引 1:1 对齐）
                Repeater {
                    model: [qsTr("状态"), qsTr("订阅"), qsTr("设置"), qsTr("日志"), qsTr("关于")]
                    delegate: NavButton {
                        Layout.fillWidth: true // 铺满侧栏宽、右缘紧贴内容卡
                        Layout.leftMargin: 20  // 按钮左侧留 20px 间距
                        Layout.topMargin: index === 0 ? 0 : 5
                        label: modelData
                        current: window.currentPage === index
                        onClicked: window.currentPage = index
                    }
                }

                Item { Layout.fillHeight: true }

                // 版本行：点击打开更新窗（复刻 Widgets 版侧栏「Ver:」——平时灰色，
                // 发现新版本转红并加 ⬆；点击都走更新流程）。
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: about.updateAvailable ? ("Ver: " + bridge.version + " ⬆")
                                                : ("Ver: " + bridge.version)
                    font.pixelSize: 12
                    font.bold: about.updateAvailable
                    color: about.updateAvailable ? Theme.danger : Theme.versionColor

                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: updateWindow.show() }
                }
            }
        }

        // ———————————————————— 右列（内容 + 页脚）————————————————————
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: window.isMac ? Theme.inset : 0
            Layout.rightMargin: window.isMac ? Theme.inset : 0
            spacing: 0

            // 内容卡（不透明，浮在玻璃上）
            Card {
                Layout.fillWidth: true
                Layout.fillHeight: true

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: 5 // 主内容区(卡片内)统一 padding 5px
                    currentIndex: window.currentPage

                    StatusPage {}
                    SubscriptionsPage {}
                    SettingsPage {}
                    LogsPage {}
                    AboutPage {}
                }
            }

            // 页脚（透明，露壳色/玻璃）
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.footerHeight

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    spacing: 5

                    Text {
                        text: ""
                        font.family: Theme.iconFont
                        font.pixelSize: 14
                        color: Theme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: bridge.lastLog
                        elide: Text.ElideRight
                        font.pixelSize: 12
                        color: Theme.textSecondary
                    }

                    FooterSwitch {
                        label: qsTr("增强")
                        on: bridge.tunEnabled
                        onClicked: bridge.toggleTun()
                    }
                    FooterSwitch {
                        label: qsTr("网页")
                        on: bridge.proxyEnabled
                        onClicked: bridge.toggleProxy()
                    }
                    FooterSwitch {
                        label: qsTr("核心")
                        on: bridge.coreRunning
                        onClicked: bridge.toggleCore()
                    }

                    ComboBox {
                        id: modeCombo
                        Layout.preferredWidth: 120
                        Layout.preferredHeight: 28
                        model: [qsTr("规则"), qsTr("全局"), qsTr("直连")]
                        currentIndex: Math.max(0, model.indexOf(bridge.mode))
                        font.pixelSize: 12
                        onActivated: bridge.setMode(currentText)

                        // 纯黑(暗)/纯白(亮) + 圆角 3（同「核心」按钮 FooterSwitch）
                        background: Rectangle {
                            radius: 3
                            color: Theme.dark ? "#000000" : "#ffffff"
                        }
                        contentItem: Text {
                            leftPadding: 10
                            text: modeCombo.displayText
                            font: modeCombo.font
                            color: Theme.textPrimary
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        indicator: Text {
                            x: modeCombo.width - width - 9
                            y: (modeCombo.height - height) / 2
                            text: "▾"
                            font.pixelSize: 10
                            color: Theme.textMuted
                        }
                        popup: Popup {
                            y: modeCombo.height + 2
                            width: modeCombo.width
                            padding: 4
                            background: Rectangle {
                                radius: 3
                                color: Theme.dark ? "#000000" : "#ffffff"
                                border.width: 1
                                border.color: Theme.divider
                            }
                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: modeCombo.popup.visible ? modeCombo.delegateModel : null
                                currentIndex: modeCombo.highlightedIndex
                            }
                        }
                        delegate: ItemDelegate {
                            width: modeCombo.width - 8
                            height: 28
                            highlighted: modeCombo.highlightedIndex === index
                            contentItem: Text {
                                text: modelData
                                leftPadding: 6
                                font.pixelSize: 12
                                color: Theme.textPrimary
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 3
                                color: highlighted ? Theme.hover : "transparent"
                            }
                        }
                    }
                }
            }
        }
    }

    // 更新窗（独立顶层窗口，默认隐藏；点击侧栏「Ver:」→ updateWindow.show()）。
    UpdateWindow { id: updateWindow }

    // 启动 3 秒后静默检查一次（对齐 Widgets 版 checkForUpdate(true)）：有新版本时
    // about.updateAvailable 置真，侧栏「Ver:」转红加 ⬆。
    Timer {
        interval: 3000
        running: true
        repeat: false
        onTriggered: about.check()
    }
}
