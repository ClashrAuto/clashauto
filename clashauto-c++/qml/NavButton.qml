import QtQuick
import ClashAuto

// 侧栏导航项：左侧 3px 强调条（选中时点亮），文字左对齐，hover 高亮。
Item {
    id: root
    property string label: ""
    property bool current: false
    signal clicked()

    implicitWidth: 115
    implicitHeight: 40

    Rectangle {
        anchors.fill: parent
        radius: 5
        color: root.current ? Theme.card : (hover.hovered ? Theme.hover : "transparent")
    }
    Rectangle {
        width: 3
        height: parent.height
        anchors.left: parent.left
        color: root.current ? Theme.accent : "transparent"
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: root.current || hover.hovered ? 34 : 32
        text: root.label
        font.pixelSize: 14
        color: root.current ? Theme.textSecondary : Theme.textPrimary

        Behavior on anchors.leftMargin {
            NumberAnimation { duration: 90 }
        }
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }
}
