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

    // 紧凑单行布局：图标 + 「标题 数值」并排，适配减到 ~1/3 的卡高。
    Row {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.glyph
            font.family: Theme.iconFont
            font.pixelSize: 18
            color: Theme.dark ? "#aaaaaa" : "#888888"
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            font.pixelSize: 12
            color: root.accentColor
        }
        Item { width: 2; height: 1 }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.value
            font.pixelSize: 15
            font.bold: true
            color: root.accentColor
        }
    }
}
