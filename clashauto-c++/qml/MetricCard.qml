import QtQuick
import Coast

// 流量指标卡：左侧 iconfont 图标 + 右侧标题/数值。accentColor 对齐 Widgets 版各卡配色。
// 角标（如进程卡的「清空连接」）由调用方在外层叠加，保持本组件简单。
//
// showChart：把**实时带宽折线画成这张卡的背景**（上传/下载卡用）。曲线以前是页面下方两张
// 独立的图，现在各自并进对应的卡里——同一个数字的「此刻」和「最近 40 秒」挨在一起看，
// 也省下了整整一屏的竖向空间。图用 minimal 模式（无网格/刻度/标题）+ headroom 压在下半部，
// 免得曲线冲到标题和数值上。喂数据：调用方每拍调 pushChart(bytesPerSecond)。
// clip 必须开：Canvas 铺满卡片，不裁的话会盖到相邻卡片上。
Rectangle {
    id: root
    property string glyph: ""       // remixicon 码点（见 Theme.riFont）
    property string title: ""
    property string value: "0.00 B"
    property color accentColor: Theme.textSecondary
    property bool showChart: false  // 背景是否画实时带宽折线
    // 曲线颜色默认跟卡片主色走（上传红 / 下载绿），需要时可单独指定。
    property color chartColor: accentColor

    radius: 4
    color: Theme.metricBg
    clip: true

    // 每拍喂一个速率样本给背景折线（不显示图时是空操作，调用方不必判断）。
    function pushChart(bytesPerSecond) {
        if (root.showChart)
            bgChart.push(bytesPerSecond);
    }

    // 背景折线：声明在内容之前 → 压在标题/数值之下。
    BandwidthChart {
        id: bgChart
        anchors.fill: parent
        visible: root.showChart
        minimal: true
        headroom: 0.62 // 曲线最高只到卡片下 62%，上面留给标题和数值
        lineColor: root.chartColor
    }

    // 图标 + 上下两行（标题 / 数值）。
    // 带背景折线时**只占卡片顶部一条 64px 的带子**（内容在带子里居中）：卡片会随窗口长高，
    // 若还按整卡居中，图和字就会在卡片中央撞在一起，且窗口越高字越往下漂。
    Row {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        height: root.showChart ? Math.min(parent.height, 64) : parent.height
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
