import QtQuick
import QtQuick.Layouts
import ClashAuto

// 节点列表行：名称（省略号）+ 延迟/速度药丸 + 应用按钮。活动行整行高亮 + 左强调条。
Rectangle {
    id: root
    property string display: ""
    property string badgeText: "-"
    property color badgeColor: "#33ffffff"
    property bool active: false
    signal apply()

    height: 40
    radius: 4
    color: active ? Theme.nodeRowActive : Theme.nodeRowBg

    Rectangle {
        width: 3
        height: parent.height
        color: root.active ? Theme.accentStrong : "transparent"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        spacing: 8

        Text {
            Layout.fillWidth: true
            text: root.display
            elide: Text.ElideRight
            font.pixelSize: 12
            color: Theme.textSecondary
        }

        Rectangle {
            radius: 5
            color: root.badgeColor
            implicitWidth: badge.implicitWidth + 10
            implicitHeight: badge.implicitHeight + 6
            Layout.alignment: Qt.AlignVCenter
            Text {
                id: badge
                anchors.centerIn: parent
                text: root.badgeText
                font.pixelSize: 12
                color: "#222222"
            }
        }

        Rectangle {
            Layout.preferredWidth: 82
            Layout.fillHeight: true
            color: applyHover.hovered && !root.active ? Theme.hover : "transparent"
            Text {
                anchors.centerIn: parent
                text: root.active ? qsTr("使用中") : qsTr("应用")
                font.pixelSize: 12
                color: root.active ? Theme.textSecondary : Theme.textSecondary
            }
            HoverHandler { id: applyHover }
            TapHandler {
                enabled: !root.active
                onTapped: root.apply()
            }
        }
    }
}
