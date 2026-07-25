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
    property bool selected: false
    signal clicked()
    signal toggleProxy()

    height: 52
    radius: 5
    color: selected ? Qt.rgba(72 / 255, 151 / 255, 248 / 255, 0.22)
                    : (rowHover.hovered ? Theme.hover : Theme.nodeRowBg)
    opacity: online ? 1.0 : 0.5

    HoverHandler { id: rowHover }
    TapHandler { onTapped: root.clicked() }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8

        // 类型头像（纯色圆角 + 单字）+ 在线小圆点
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
                    font.pixelSize: 16
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
                    font.bold: root.selected
                    color: Theme.textPrimary
                }
                // 本机 / 网关 保护徽章
                Rectangle {
                    visible: root.isSelf || root.isGateway
                    radius: 3
                    color: Qt.rgba(0, 0, 0, 0.25)
                    implicitWidth: protTxt.implicitWidth + 8
                    implicitHeight: protTxt.implicitHeight + 3
                    Text {
                        id: protTxt
                        anchors.centerIn: parent
                        text: root.isSelf ? qsTr("本机") : qsTr("网关")
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

        // 代理开关（本机/网关不显示）——迷你滑动开关
        Rectangle {
            visible: !(root.isSelf || root.isGateway)
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
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: root.toggleProxy() }
        }
    }
}
