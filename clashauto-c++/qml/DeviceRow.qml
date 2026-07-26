import QtQuick
import QtQuick.Layouts
import ClashAuto

// 设备列表行：类型头像 + 名称/副标题（IP·厂商）+ 实时速率 + 代理开关。
// 选中态整行高亮；离线行整体淡化。本机/网关不显示代理开关（改显「保护」徽章）。
Rectangle {
    id: root
    property string mac: ""
    property string ip: ""
    property string name: ""
    property string typeKey: "unknown"
    property string vendor: ""
    property bool online: false
    property bool proxied: false
    property real rateUp: 0
    property real rateDown: 0
    property real todayUp: 0    // 今日累计上行字节
    property real todayDown: 0  // 今日累计下行字节
    property bool isSelf: false
    property bool isGateway: false
    property bool proxyable: false // 能否开代理（非本机/网关 + 在主网卡网段内）
    property bool selected: false
    signal clicked()
    signal toggleProxy()

    height: 52
    radius: 5
    color: selected ? Qt.rgba(72 / 255, 151 / 255, 248 / 255, 0.22)
                    : (rowMouse.containsMouse ? Theme.hover : Theme.nodeRowBg)
    opacity: online ? 1.0 : 0.5

    // 整行用 MouseArea 而不是 TapHandler：TapHandler 只拿「被动 grab」，按下的一瞬间 Main.qml
    // 那个铺满窗口的背景 DragHandler 就能接管 → 按住列表往下拖会把**整个窗口**拖走（实测：拖一次
    // 窗口位移 58px、列表只滚了 2px）。MouseArea 在按下时拿独占 grab，而那个 DragHandler 的
    // grabPermissions 只有 Approves* 位、没有 CanTakeOverFrom*，抢不走 → 窗口不动；
    // ListView(Flickable) 仍可从 MouseArea 手里抢 grab（preventStealing 默认 false）→ 列表照常滚。
    MouseArea {
        id: rowMouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8

        // 类型头像（纯色圆角 + 类型图标）+ 在线小圆点
        Item {
            Layout.preferredWidth: 34
            Layout.minimumWidth: 34 // 同右侧各列：不给最小宽，长副标题的行会把头像也压扁
            Layout.preferredHeight: 34
            Layout.alignment: Qt.AlignVCenter
            Rectangle {
                anchors.fill: parent
                radius: 8
                color: Theme.deviceColor(root.typeKey)
                Text {
                    anchors.centerIn: parent
                    text: Theme.deviceGlyph(root.typeKey)
                    font.family: Theme.riFont // 图标字体（不继承默认的 MiSans，否则是空白方块）
                    font.pixelSize: 18
                    color: "#ffffff"
                }
            }
            Rectangle { // 在线状态角标
                width: 11; height: 11; radius: 6
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: -2
                anchors.bottomMargin: -2
                color: root.online ? "#4da13e" : "#888888"
                border.width: 2
                border.color: Theme.card
            }
        }

        // 名称 + 副标题：**吃掉右侧信息区剩下的全部宽度**，放不下就 elide。
        // minimumWidth 显式给 0：RowLayout 空间不够时是按各项可压缩余量一起压的，右侧那几列都
        // 写了 minimumWidth = 自身宽度（刚性），这里给 0 就保证「该被挤的是名字」，而不是把右侧
        // 挤窄——那会让不同行的右侧内容落在不同的 x 上。
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            spacing: 1
            Text {
                Layout.fillWidth: true
                text: root.name
                elide: Text.ElideRight
                font.pixelSize: 13
                color: Theme.textPrimary
            }
            Text {
                Layout.fillWidth: true
                text: root.ip + (root.vendor ? "  ·  " + root.vendor : "")
                elide: Text.ElideRight
                font.pixelSize: 10
                color: Theme.textMuted
            }
        }

        // ———————————————— 右侧信息区：徽章 + 今日 + 实时速率 + 代理开关 ————————————————
        // **紧贴行的右边缘**，宽度只取自身内容：名称列 fillWidth 会把剩下的宽度全吃掉，所以这块
        // 自然被顶到最右。用不上的东西直接 visible:false 收起来（不占位），把宽度让给名字。
        //
        // 两条必须守住的规矩：
        //  1) 每个子项都写死宽度并给 minimumWidth —— RowLayout 空间不够时是按各项可压缩余量
        //     **一起**压的，不给最小宽的话，副标题长的行会把右侧几列压窄一截，同一列在不同行
        //     落在不同的 x 上（实测「网关」行比「本机」行早 39px）。给了之后被挤的只会是名字。
        //  2) 流量两列的显隐条件都挂在「今日有没有流量」上，而不是「此刻有没有速率」——
        //     后者每一拍都在变，列一出一收就是整行左右抽搐。
        RowLayout {
            id: infoRow
            // 今天传过东西的设备才显示流量两列；其余设备（绝大多数没开代理，流量根本不经 mihomo）
            // 一整天都是 0，与其显示两行「0 B」噪音，不如把宽度让给名字。
            readonly property bool hasTraffic: (root.todayDown + root.todayUp) > 0
            Layout.alignment: Qt.AlignVCenter
            spacing: 8

            // 本机 / 网关 / 其它网络（跨网段，劫持不到）——不可代理的原因徽章。
            Rectangle {
                visible: root.isSelf || root.isGateway || !root.proxyable
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Math.min(64, protTxt.implicitWidth + 8)
                Layout.minimumWidth: Layout.preferredWidth
                Layout.preferredHeight: protTxt.implicitHeight + 3
                radius: 3
                color: Qt.rgba(0, 0, 0, 0.25)
                Text {
                    id: protTxt
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 8, implicitWidth)
                    elide: Text.ElideRight
                    text: root.isSelf ? qsTr("本机")
                        : root.isGateway ? qsTr("网关") : qsTr("其它网络")
                    font.pixelSize: 9
                    color: Theme.textSecondary
                }
            }

            // 今日累计上/下行（也是列表的排序主键，见 DeviceListModel::buildTarget）。
            // 用「今日」而不是「累计」：跨会话的历史总量对「谁在占带宽」没有参考价值。
            ColumnLayout {
                visible: infoRow.hasTraffic
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 82
                Layout.minimumWidth: 82
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: "↓ " + Theme.fmtBytes(root.todayDown)
                    font.pixelSize: 10
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
                Text {
                    Layout.fillWidth: true
                    text: "↑ " + Theme.fmtBytes(root.todayUp)
                    font.pixelSize: 10
                    color: Theme.textMuted
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
            }

            // 实时速率：跟着「今日有流量」占位，闲下来只是淡出（opacity），**位置不动**——
            // 速率文字每一拍都在变宽变窄（"↓ 9.77 KB/s" ↔ "↓ 1.20 MB/s"），一收一放整行就在抖。
            ColumnLayout {
                visible: infoRow.hasTraffic
                opacity: root.online && (root.rateDown > 0 || root.rateUp > 0) ? 1.0 : 0.0
                Behavior on opacity { NumberAnimation { duration: 120 } }
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 76
                Layout.minimumWidth: 76
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: "↓ " + Theme.fmtRate(root.rateDown)
                    font.pixelSize: 10
                    color: "#5bb44b"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
                Text {
                    Layout.fillWidth: true
                    text: "↑ " + Theme.fmtRate(root.rateUp)
                    font.pixelSize: 10
                    color: "#b14a4a"
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignRight
                }
            }

            // 代理开关。本机/网关/跨网段的行没有这个概念 → 直接收起来，让徽章顶到最右。
            Rectangle {
                id: proxySwitch
                readonly property bool canToggle: root.proxied || (root.proxyable && root.online)
                visible: root.proxyable || root.proxied
                opacity: canToggle ? 1.0 : 0.4
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 38
                Layout.minimumWidth: 38
                Layout.preferredHeight: 20
                width: 38; height: 20; radius: 10
                color: root.proxied ? Theme.accent : Theme.switchTrackOff
                Behavior on color { ColorAnimation { duration: 120 } }
                Rectangle {
                    width: 16; height: 16; radius: 8
                    y: 2
                    x: root.proxied ? parent.width - width - 2 : 2
                    color: "#ffffff"
                    Behavior on x { NumberAnimation { duration: 120 } }
                }
                // 用 MouseArea 而非 TapHandler：它压在整行 MouseArea 之上，按下即独占 grab ——
                // 既保证点开关不会连带选中整行，也保证在开关上按住拖动不会去拖窗口。
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: proxySwitch.canToggle ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: if (proxySwitch.canToggle) root.toggleProxy()
                }
            }
        }
    }
}
