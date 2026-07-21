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

    // 图标 + 上下两行（标题 / 数值）。
    Row {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 12

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.glyph
            font.family: Theme.iconFont
            font.pixelSize: 26
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
                font.family: Theme.monoFont // 实时数值等宽：每秒刷新不抖动
                font.pixelSize: 17
                font.bold: true
                color: root.accentColor
            }
        }
    }
}
