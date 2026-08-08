import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import ClashAuto

// 节点页：顶栏（节点+数量 | 可展开搜索 | 测速 | 帮助）+ 节点列表（点击选择 / 右键禁用）。
// 原本是状态页右半边那一栏，独立成页后整页宽都归它——节点名普遍很长（地区+倍率+机场名），
// 以前挤在半栏里大量 elide 成省略号。状态页那边只留流量卡与带宽图。
Item {
    id: page

    // 节点搜索框是否展开（对齐旧项目 searchBox.show：默认收起，点放大镜展开）
    property bool searchShown: false

    // 页面显隐驱动 /proxies 的轮询频率（节点页 1s，其他页 5s）——
    // 与 StatusPage 里 setStatusActive 同一模式。理由见 ClashService::setNodesVisible。
    onVisibleChanged: bridge.setNodesActive(page.visible)
    Component.onCompleted: bridge.setNodesActive(page.visible)

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10 // 与状态页同内距（StackLayout 为 0，各页自管）
        spacing: 8

        // 顶栏（严格参考旧项目 status.vue：节点+数量 | 可展开搜索 | 测速 refresh/loading | 帮助 question→文档）
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30 // 固定高度：搜索框(28)展开时不再撑高整行
            spacing: 6

            Text {
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("节点")
                font.pixelSize: 18
                color: Theme.textPrimary
            }
            Text {
                text: "(" + nodeModel.count + ")"
                font.pixelSize: 9
                color: Theme.textMuted
                Layout.alignment: Qt.AlignVCenter
            }

            // 搜索：默认只显示放大镜，点击展开输入框（右侧 ✕ 清空并收起）
            Text {
                visible: !page.searchShown
                text: "\uF0D1" // search-line
                font.family: Theme.riFont
                font.pixelSize: 16
                color: Theme.textMuted
                Layout.leftMargin: 4
                Layout.alignment: Qt.AlignVCenter
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: { page.searchShown = true; search.forceActiveFocus() } }
            }
            TextField {
                id: search
                visible: page.searchShown
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: 28
                rightPadding: 24
                placeholderText: qsTr("搜索节点")
                color: Theme.textPrimary
                placeholderTextColor: Theme.textMuted
                font.pixelSize: 12
                background: Rectangle {
                    radius: 3
                    color: Theme.inputBg
                    border.width: 1
                    border.color: search.activeFocus ? Theme.accent : Theme.inputBorder
                }
                onTextChanged: bridge.setNodeFilter(text)
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: "\uEB99" // close-line
                    font.family: Theme.riFont
                    font.pixelSize: 14
                    color: Theme.textMuted
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        onTapped: {
                            search.text = "";
                            bridge.setNodeFilter("");
                            page.searchShown = false;
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true; visible: !page.searchShown }

            // 策略组切换。★ 只在**真有得选**（≥2 个）时出现：订阅只给一个选择组时，一个
            //   永远只有一项的下拉纯属噪音。清单由 ClashService 过滤成只含 Selector 组 ——
            //   见那里的说明：自动选择/故障转移这类组核心不接受手动指定。
            //   在此之前主组完全是启发式自动挑的（名字里带「节点/选择/代理/Proxy」），
            //   订阅里的流媒体分流、广告拦截等策略组用户根本碰不到。
            ThemedCombo {
                id: groupCombo
                visible: bridge.groups.length > 1
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Math.min(200, Math.max(120, implicitContentWidth + 32))
                implicitHeight: 28
                font.pixelSize: 12
                model: bridge.groups
                // 跟着后端走，而不是自己记一份：切模式/换订阅时主组会被后端重挑，
                // 本地状态那份会跟真值悄悄分叉。indexOf 返回 -1 时 ComboBox 显示空，正确。
                currentIndex: bridge.groups.indexOf(bridge.selectedGroup)
                // 用 onActivated（**只有用户操作才触发**）而不是 onCurrentIndexChanged ——
                // 后者在上面那条绑定刷新时也会触发，等于把后端刚下发的值再回写一遍，
                // 轮询一次抖一次。
                onActivated: bridge.selectGroup(bridge.groups[currentIndex])
                ToolTip.visible: hovered && !popup.visible
                ToolTip.text: qsTr("策略组")
            }

            // 测速：空闲 refresh-line、测速中 loader-4-line 旋转（对齐旧项目 refresh-right / loading）
            Text {
                id: spdIcon
                Layout.alignment: Qt.AlignVCenter
                property real spin: 0
                text: bridge.speedTesting ? "\uEEC6" : "\uF064" // loader-4-line / refresh-line
                font.family: Theme.riFont
                font.pixelSize: 19
                color: Theme.accent
                rotation: bridge.speedTesting ? spin : 0
                NumberAnimation on spin {
                    running: bridge.speedTesting
                    from: 0; to: 360; duration: 900; loops: Animation.Infinite
                }
                HoverHandler { id: spdHover; cursorShape: Qt.PointingHandCursor }
                ToolTip.visible: spdHover.hovered
                ToolTip.text: qsTr("测速")
                TapHandler {
                    enabled: !bridge.speedTesting
                    onTapped: bridge.runSpeedTest()
                }
            }

            // 帮助：打开在线文档（对齐旧项目 question → gitbook）
            Text {
                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: 5
                text: "\uF044" // question-fill
                font.family: Theme.riFont
                font.pixelSize: 18
                color: Theme.textMuted
                HoverHandler { id: qHover; cursorShape: Qt.PointingHandCursor }
                ToolTip.visible: qHover.hovered
                ToolTip.text: qsTr("帮助")
                TapHandler { onTapped: Qt.openUrlExternally("https://clashr-auto.gitbook.io/") }
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: nodeModel
            spacing: 1 // 节点行间距紧凑为 1px；右边不留边距，行占满列表宽（滚动条悬浮不占宽）
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: NodeRow {
                width: ListView.view.width
                display: model.display
                badgeText: model.badgeText
                badgeColor: model.badgeColor
                active: model.active
                // 切换加载态：目标行转圈、其余灰化禁用（对齐旧项目 createNodeRow 的 disableLoading）
                switching: bridge.switching
                isTarget: bridge.switching && model.rawName === bridge.switchTarget
                spinnerText: bridge.spinnerGlyph
                // 应用/禁用都先进入切换加载态，再调用后端（严格对齐旧项目 createNodeRow：beginNodeSwitch 在前）
                onApply: {
                    bridge.beginNodeSwitch(model.rawName)
                    bridge.selectNode(model.rawName)
                }
                onDisable: {
                    bridge.beginNodeSwitch(model.rawName)
                    bridge.disableNode(model.rawName, model.rawNow)
                    subs.reload() // 刷新订阅页，反映刚禁用的节点（对齐旧项目 disableNodeByName→reloadSubscriptions）
                }
            }
        }
    }
}
