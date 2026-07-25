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

        // 名称 + 副标题
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            RowLayout {
                Layout.fillWidth: true
                spacing: 5
                Text {
                    Layout.fillWidth: true
                    text: root.name
                    elide: Text.ElideRight
                    font.pixelSize: 13
                    color: Theme.textPrimary
                }
                // 本机 / 网关 / 其它网络（跨网段，劫持不到）——不可代理的原因徽章
                Rectangle {
                    visible: root.isSelf || root.isGateway || !root.proxyable
                    radius: 3
                    color: Qt.rgba(0, 0, 0, 0.25)
                    implicitWidth: protTxt.implicitWidth + 8
                    implicitHeight: protTxt.implicitHeight + 3
                    Text {
                        id: protTxt
                        anchors.centerIn: parent
                        text: root.isSelf ? qsTr("本机")
                            : root.isGateway ? qsTr("网关") : qsTr("其它网络")
                        font.pixelSize: 9
                        color: Theme.textSecondary
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: root.ip + (root.vendor ? "  ·  " + root.vendor : "")
                elide: Text.ElideRight
                font.pixelSize: 10
                color: Theme.textMuted
            }
        }

        // 实时速率（仅在线且有流量时显示）
        ColumnLayout {
            visible: root.online && (root.rateDown > 0 || root.rateUp > 0)
            spacing: 0
            Layout.alignment: Qt.AlignVCenter
            Text {
                text: "↓ " + Theme.fmtRate(root.rateDown)
                font.pixelSize: 10
                color: "#5bb44b"
                horizontalAlignment: Text.AlignRight
                Layout.alignment: Qt.AlignRight
            }
            Text {
                text: "↑ " + Theme.fmtRate(root.rateUp)
                font.pixelSize: 10
                color: "#b14a4a"
                horizontalAlignment: Text.AlignRight
                Layout.alignment: Qt.AlignRight
            }
        }

        // 代理开关（本机/网关、以及跨网段设备不显示）——迷你滑动开关。
        // 离线设备：已开的可以关（撤销），没开的不能开（没 IP/ARP 无从劫持）→ 开关变灰不可点。
        Rectangle {
            id: proxySwitch
            // 已经开着的一律可见可关（哪怕设备离线/已跨到另一个网络，也得能撤销）；
            // 关着的只有「可代理且在线」才点得动。
            readonly property bool canToggle: root.proxied || (root.proxyable && root.online)
            visible: root.proxyable || root.proxied
            opacity: canToggle ? 1.0 : 0.4
            Layout.alignment: Qt.AlignVCenter
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
            // 同样用 MouseArea：它压在整行 MouseArea 之上，按下即独占 grab —— 既保证点开关不会
            // 连带选中整行，也保证在开关上按住拖动不会去拖窗口。
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: proxySwitch.canToggle ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                onClicked: if (proxySwitch.canToggle) root.toggleProxy()
            }
        }
    }
}
