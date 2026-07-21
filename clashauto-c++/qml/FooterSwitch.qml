import QtQuick
import ClashAuto

// 页脚开关（增强/网页/核心）：左侧呼吸圆点 + 文字。on 决定圆点点亮。
Rectangle {
    id: root
    property string label: ""
    property bool on: false
    signal clicked()

    implicitWidth: 66
    implicitHeight: 28
    radius: 3
    color: hover.hovered ? Qt.lighter(Theme.card, Theme.dark ? 2.2 : 0.97) : Theme.card

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Rectangle {
            id: dot
            width: 12
            height: 12
            radius: 6
            anchors.verticalCenter: parent.verticalCenter
            color: root.on ? Theme.accent : Theme.switchTrackOff
            border.width: 3
            border.color: root.on ? Qt.rgba(72 / 255, 152 / 255, 248 / 255, 0.30)
                                   : Qt.rgba(0.4, 0.4, 0.4, 0.15)
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.label
            font.pixelSize: 12
            color: Theme.textPrimary
        }
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }
}
