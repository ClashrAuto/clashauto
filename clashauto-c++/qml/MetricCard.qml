import QtQuick
import ClashAuto

// 流量指标卡：左侧 iconfont 图标 + 右侧标题/数值。accentColor 对齐 Widgets 版各卡配色。
// 角标（如进程卡的「清空连接」）由调用方在外层叠加，保持本组件简单。
Rectangle {
    id: root
    property string glyph: ""       // iconfont 码点
    property string title: ""
    property string value: "0.00 B"
    property color accentColor: Theme.textSecondary

    radius: 4
    color: Theme.metricBg

    Row {
        anchors.fill: parent
        anchors.margins: 8
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 16

        Text {
            width: 50
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: Text.AlignHCenter
            text: root.glyph
            font.family: Theme.iconFont
            font.pixelSize: 30
            color: Theme.dark ? "#aaaaaa" : "#888888"
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: root.title
                font.pixelSize: 12
                color: root.accentColor
            }
            Text {
                text: root.value
                font.pixelSize: 18
                color: root.accentColor
            }
        }
    }
}
