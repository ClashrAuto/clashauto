import QtQuick
import ClashAuto

// 流量指标卡：左侧 iconfont 图标 + 右侧标题/数值。accentColor 对齐 Widgets 版各卡配色。
// 角标（如进程卡的「清空连接」）由调用方在外层叠加，保持本组件简单。
//
// bgGlyph：可选的**大号背景水印**（上传/下载卡用）。它压在内容之下、只有一成多的不透明度，
// 是卡片的底纹而不是第二个图标——所以尺寸跟着卡高走、颜色跟着 accentColor 走。
// clip 必须开：水印按卡高定尺寸，不裁的话会溢出到相邻卡片上。
Rectangle {
    id: root
    property string glyph: ""       // remixicon 码点（见 Theme.riFont）
    property string bgGlyph: ""     // 背景水印码点（空 = 不画）
    property string title: ""
    property string value: "0.00 B"
    property color accentColor: Theme.textSecondary

    radius: 4
    color: Theme.metricBg
    clip: true

    // 背景水印：贴右侧、竖直居中，尺寸按卡高走。
    Text {
        visible: root.bgGlyph !== ""
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: root.bgGlyph
        font.family: Theme.riFont
        font.pixelSize: Math.round(root.height * 0.82)
        color: root.accentColor
        opacity: 0.16
    }

    // 图标 + 上下两行（标题 / 数值）。
    Row {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        spacing: 12

        Text {
            id: metricIcon
            anchors.verticalCenter: parent.verticalCenter
            text: root.glyph
            font.family: Theme.riFont
            font.pixelSize: 28
            color: Theme.dark ? "#aaaaaa" : "#888888"
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            // 占满图标右侧剩余宽，供标题/数值省略号（长译文如「总下载/进程数」不溢出卡片）
            width: parent.width - metricIcon.width - parent.spacing
            spacing: 3
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: root.title
                font.pixelSize: 13
                color: root.accentColor
            }
            Text {
                width: parent.width
                elide: Text.ElideRight
                text: root.value
                font.pixelSize: 24
                color: root.accentColor
            }
        }
    }
}
