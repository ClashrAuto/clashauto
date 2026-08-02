import QtQuick
import ClashAuto

// 实时带宽折线 —— 严格复刻 Widgets 版 TrafficChart（plugins/tubiao.js）：
//  · 固定 42 个点（可见 40 + 两端各 1 富余，滚动全程无缝、右侧不留缺口），初始填 1.0；
//  · 每秒 push 追加一点，整条曲线在这一秒里连续左滑一格；
//  · 四分网格 + 右侧速度刻度(max/¾/½/¼) + 左上标题；阶梯式自适应量程(128KB 基准 / 2MB 步进)；
//  · 只画折线(3px 圆角、线色 α0.70)，无面积填充；背景为线色极淡底(α0.03)。
//
// ★★ 这个文件的分层是**性能约束**，不是风格偏好。动它之前先读完这段。★★
//
// 从前这里是「50ms 定时器 → 整张 Canvas 重画」：背景、网格、刻度线、文字、42 点折线，每秒 20 遍
// 全部重描一次——而这中间**唯一变的只有折线的水平位置，且只是整体平移**。树莓派上实测主线程
// 因此常驻 14% CPU。现在拆成三层，各自只在真正需要时才动：
//
//   1) 静态层（Rectangle）——背景、四分网格、minimal 的刻度线。纯 QML 矩形，只在尺寸/量程变化
//      时重新布局，滚动时一帧都不重绘。
//   2) 平移层（Canvas）——只画折线本身（和 minimal 的线下填充），坐标里**不含滚动偏移**；滚动
//      靠给整个 Canvas 项赋 x 来做，场景图直接搬已有纹理，不重跑 JS、不重描路径。Canvas 比可见
//      区宽出一格(spacing)，正好覆盖平移露出的边，两端不留缺口。重绘只发生在 push / 改尺寸时。
//      驱动是 NumberAnimation 而不是 JS 定时器：由渲染线程按 vsync 插值，观感 60fps，每帧 JS 开销为 0。
//   3) 文字层（Text）——刻度读数与标题。**绝对不要改回 ctx.fillText**：Canvas 的 fillText 每次都走
//      QPainterPath::addText → QFontEngineFT::addGlyphsToPath → cubicTo，把每个字形现场转成贝塞尔
//      轮廓重描一遍，完全绕开字形缓存；而 `ctx.font = "..."` 每次赋值都要过一遍
//      QFontDatabase::hasFamily()（Linux 上直落 fontconfig 的别名解析）。挂在 20Hz 上时，光这一项
//      就占了主线程绘制开销的约 40%，而文字一秒才可能变一次。
//
// 一句话：**滚动不许触发重绘。** 任何让 onPaint 跟着滚动跑的改法都会把上面这些全部退回去。
Item {
    id: root
    // 平移层比可见区宽一格，必须裁掉溢出的那一格（MetricCard 自己已 clip，独立成图时没人裁）。
    clip: true
    property string title: ""
    property color lineColor: Theme.accent
    // —— 背景模式（minimal）——
    // 这张图现在还有第二种用法：当上传/下载卡的**底纹**（见 MetricCard.bgChart）。那时候网格、
    // 右侧刻度、左上标题全是噪音，压在卡片的数字底下只会打架，所以 minimal 时一概不画，
    // 改成「一条线 + 线下的淡填充」——底纹要的是趋势的形状，不是能读数的图表。
    property bool minimal: false
    // 曲线最高只占本控件高度的这个比例。minimal 下留出上方给卡片的标题/数值，曲线再高也不会
    // 顶到字上；默认 1.0 = 独立成图时铺满，与原行为一致。
    property real headroom: 1.0
    readonly property int maxPointer: 42        // length(40) + 2
    readonly property real kBase: 131072.0      // 128KB 基准
    readonly property real kRule: 2097152.0     // 2MB 步进
    property var pointers: []
    // 当前量程。**显式属性而不是每帧现算的函数**：一来刻度文字/网格要绑它（pointers 是原地
    // push/shift 的，var 属性不会发变更信号，绑 pointers 绑不动），二来量程一秒才可能变一次，
    // 没道理跟着滚动重算 42 次比较。
    property real maxScale: kBase
    // 相邻两点的水平间距，也就是一秒滑过的距离。
    // **必须取整**：平移层是一张已光栅化的纹理，落在小数位置就会被重采样，整条线变软
    // （实测线条峰值亮度 165 → 150）。取整之后间距、画布宽、每拍位移全部落在整像素上，
    // 一拍走完正好 -spacing，push 时归零，接缝为零。ceil 而不是 round：round 会在
    // width/40 向下取整时让 40*spacing < width，右侧漏出一条没画到的空隙。
    readonly property real spacing: Math.max(1, Math.ceil(width / (maxPointer - 2)))
    // 滚动位移的连续量；真正赋给 Canvas 的 x 是它取整后的值（见下）。
    property real slideX: 0

    Component.onCompleted: {
        var a = [];
        for (var i = 0; i < maxPointer; ++i)
            a.push(1.0);
        pointers = a;
        recomputeMax();
        canvas.requestPaint();
    }

    // 每秒一个样本 = 原 replay：推入新点、越界弹出最旧、滚动重新起跑。
    function push(value) {
        pointers.push(Math.max(0, value));
        while (pointers.length > maxPointer)
            pointers.shift();
        recomputeMax();
        canvas.requestPaint();
        // 从 x=0 重新滑到 -spacing：来早了就重新起跑，来晚了动画早已停在 -spacing。
        // 不可见时不起跑（对应旧版 Timer 的 running: root.visible）——外面每拍照喂数据，
        // 没这层判断的话切走的页面里还有一堆图在逐帧插值。
        if (root.visible)
            slide.restart();
    }

    onVisibleChanged: {
        if (!visible) {
            slide.stop();
            slideX = 0;
        }
    }

    function recomputeMax() {
        var m = kBase;
        for (var i = 0; i < pointers.length; ++i)
            if (pointers[i] > m)
                m = Math.ceil(pointers[i] / kRule) * kRule;
        maxScale = m;
    }

    function speedText(v) {
        var units = ["B", "KB", "MB", "GB", "TB", "PB"];
        var i = 0;
        while (v >= 1024.0 && i < 5) {
            v /= 1024.0;
            ++i;
        }
        return v.toFixed(2) + " " + units[i] + "/s";
    }

    // ——— 1) 静态层：背景 + 四分网格（非 minimal）/ 刻度线（minimal）———
    // 滚动时这一层一帧都不重绘；只在尺寸、量程、配色变化时重新布局。

    Rectangle {
        anchors.fill: parent
        visible: !root.minimal
        color: Qt.rgba(root.lineColor.r, root.lineColor.g, root.lineColor.b, 0.03)
    }

    Repeater { // 四分网格：5 条横线
        model: root.minimal ? 0 : 5
        Rectangle {
            required property int index
            width: root.width
            height: 1
            y: Math.round(root.height / 4 * index)
            color: Qt.rgba(root.lineColor.r, root.lineColor.g, root.lineColor.b, 0.10)
        }
    }

    Repeater { // minimal 的刻度线。y 与下面刻度文字、以及 yOf() 是同一套公式，改一处要改三处。
        model: root.minimal ? 4 : 0
        Rectangle {
            required property int index
            readonly property real frac: [1.0, 0.75, 0.5, 0.25][index]
            visible: y >= 18 // 顶到卡片标题那一带了就不画这一档
            width: root.width
            height: 1
            y: Math.round(Math.max(1.0, root.height - frac * root.height * root.headroom - 1.0))
            color: Qt.rgba(root.lineColor.r, root.lineColor.g, root.lineColor.b, 0.13)
        }
    }

    // ——— 2) 平移层 ———
    // 一秒滑过一格。**只改 x，不重绘**：场景图搬现成的纹理，JS 完全不参与。
    NumberAnimation {
        id: slide
        target: root
        property: "slideX"
        from: 0
        to: -root.spacing
        duration: 1000
    }

    Canvas {
        id: canvas
        // 41 格宽（42 个点、整数间距）。
        width: root.spacing * (root.maxPointer - 1)
        height: root.height
        // **右对齐**摆放，不是从 0 起：这样一拍走完（slideX = -spacing）时，最新那个点正好落在
        // 可见区右边缘上——和改造前的语义一致。若从 0 起摆，整数间距下 40*spacing ≥ width，
        // 最新点会被永远挤到右边缘之外，图看着就慢了一拍。
        // 整体再取整：小数位置会让这张已光栅化的纹理被重采样。软件后端看不出来，但 Windows(D3D11)
        // 和 macOS(Metal) 上小数偏移的纹理四边形是实打实要走双线性采样的。
        x: Math.round(root.width - width + root.spacing + root.slideX)
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var W = width, H = height;
            var n = root.pointers.length;
            if (n < 2 || W <= 0 || H <= 0)
                return;
            var lc = root.lineColor;
            var max = root.maxScale;

            // 背景、网格、minimal 的刻度线都在上面的静态层（Rectangle）里；文字在下面的文字层。
            // 这里**只画会随滚动平移的东西**——折线，以及 minimal 的线下填充。
            //
            // 坐标里没有滚动偏移：第 i 点固定落在 i*spacing。滚动是给整个 Canvas 项赋 x 做的
            // （见上面的 slide），所以这份内容画一次能用一整拍。
            var lw = root.minimal ? 2.0 : 3.0;
            var spacing = root.spacing;
            var yOf = function (v) {
                var y = H - (v / max) * H * root.headroom - lw / 2;
                return y < lw / 2 ? lw / 2 : y;
            };

            // minimal：线下补一层淡填充。只有一条细线的话，在卡片底纹这个尺度上几乎看不见，
            // 填充才撑得起「这块区域是这张图」的感觉。压在刻度线之上（刻度**文字**在曲线之上）。
            if (root.minimal) {
                ctx.beginPath();
                ctx.moveTo(0, H);
                for (var fi = 0; fi < n; ++fi)
                    ctx.lineTo(fi * spacing, yOf(root.pointers[fi]));
                ctx.lineTo((n - 1) * spacing, H);
                ctx.closePath();
                ctx.fillStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.16);
                ctx.fill();
            }

            ctx.beginPath();
            for (var i = 0; i < n; ++i) {
                var x = i * spacing;
                if (i === 0)
                    ctx.moveTo(x, yOf(root.pointers[i]));
                else
                    ctx.lineTo(x, yOf(root.pointers[i]));
            }
            ctx.lineWidth = lw;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.strokeStyle = Qt.rgba(lc.r, lc.g, lc.b, root.minimal ? 0.55 : 0.70);
            ctx.stroke();
        }
    }

    // ——— 文字层 ———
    // 声明在 Canvas 之后 = 压在曲线之上，刻度读数不会被填充/折线盖掉（minimal 原本就是这个层序；
    // 非 minimal 原本是折线压在刻度上，现在反过来——那几个读数贴在右边缘，可读性只增不减）。
    // 基线对齐用 FontMetrics.ascent 换算：Canvas 的 fillText 给的 y 是基线，Text 的 y 是顶边。

    FontMetrics { id: fmScale; font.pixelSize: 10 }
    FontMetrics { id: fmTitle; font.pixelSize: 11 }
    FontMetrics { id: fmTick;  font.pixelSize: 9 }

    // 非 minimal：右侧四档速度刻度（max / ¾ / ½ / ¼），基线 H/4*i + 12、右端贴 W-6。
    Repeater {
        model: root.minimal ? 0 : 4
        Text {
            required property int index
            width: root.width - 6
            horizontalAlignment: Text.AlignRight
            y: root.height / 4 * index + 12 - fmScale.ascent
            font.pixelSize: 10
            color: "#969696"
            text: root.speedText(root.maxScale * (4 - index) / 4)
        }
    }

    // 非 minimal：左上标题，基线 18、左端 10。
    Text {
        visible: !root.minimal && root.title !== ""
        x: 10
        y: 18 - fmTitle.ascent
        font.pixelSize: 11 // 全 UI 不加粗
        color: Qt.rgba(root.lineColor.r, root.lineColor.g, root.lineColor.b, 0.70)
        text: root.title
    }

    // minimal：贴右边缘、坐在刻度线上方 3px。y 必须与上面静态层里那条刻度线用同一套公式
    // （lw = 2.0，yOf(max*frac) = H - frac*H*headroom - lw/2，下限 lw/2）。
    Repeater {
        model: root.minimal ? 4 : 0
        Text {
            required property int index
            readonly property real frac: [1.0, 0.75, 0.5, 0.25][index]
            readonly property real tickY:
                Math.round(Math.max(1.0, root.height - frac * root.height * root.headroom - 1.0)) + 0.5
            visible: tickY >= 18 // 顶到卡片标题那一带了就不画这一档
            width: root.width - 6
            horizontalAlignment: Text.AlignRight
            y: tickY - 3 - fmTick.ascent
            font.pixelSize: 9
            color: Theme.dark ? "#9a9a9a" : "#7a7a7a"
            text: root.speedText(root.maxScale * frac)
        }
    }
}
