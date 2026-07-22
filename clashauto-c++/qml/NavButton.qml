import QtQuick
import ClashAuto

// 侧栏导航项：文字左对齐（距左 10px），hover / 选中高亮。无左侧强调竖条。
Item {
    id: root
    property string label: ""
    property bool current: false
    signal clicked()

    implicitWidth: 115
    implicitHeight: 40

    Rectangle {
        anchors.fill: parent
        anchors.rightMargin: -radius // 右侧超出 radius：右角落在内容卡内(同色无缝)、可见处为直角、紧贴内容
        radius: 5
        color: root.current ? Theme.card : (hover.hovered ? Theme.hover : "transparent")
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: 10 // 文字距左 10px
        anchors.right: parent.right
        anchors.rightMargin: 8 // 右缘留白：长译文（如某些语言的「订阅/设置」）到此省略号，不溢出侧栏
        text: root.label
        font.pixelSize: 14
        elide: Text.ElideRight
        color: root.current ? Theme.textSecondary : Theme.textPrimary
    }

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }
}
