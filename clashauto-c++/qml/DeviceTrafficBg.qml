import QtQuick
import ClashAuto

// 设备行的**背景**实时流量图：下行(绿) / 上行(红) 两条面积曲线叠在一起，共用同一量程。
// 数据是 DeviceListModel 里每设备一条的定长速率历史（模型侧采样，见 DeviceListModel::sampleHistory
// ——挂在委托上的话，ListView 一回收委托历史就没了）。
//
// 只画在「被代理」的设备行上：其余设备的流量根本不经 mihomo，画出来永远是一条贴底的 0 线。
// 速率为 0 时**照样画**（一条贴底的线 + 极淡的填充），这是「已接管、此刻没流量」的正常样子，
// 不是「没数据」——所以不做 visible 判空。
//
// ★★ 分层是**性能约束**，不是风格偏好。动它之前先读完这段。★★
//
// 从前这里是「50ms 定时器 → 整张 Canvas 重画」：圆角裁剪 + 两条序列各一次填充一次描边，每秒 20 遍
// ——而这中间唯一变的只有曲线的水平位置，且只是整体平移。实测每个可见的被代理行常驻 **4.8% 的
// 一个核**（0/1/3/6 行分别是 0.16%/5.0%/14.2%/27.2%，线性），6 台设备被代理就是四分之一个核。
// 成本拆解：圆角裁剪 ~0.2%（几乎免费），填充占 28%，第二条序列占 40%——钱全在路径光栅化上。
// 所以和状态页的 BandwidthChart 用同一套办法，把重绘从 20Hz 降到「一拍一次」：
//
//   1) 平移层（下面的 canvas）——只画两条序列，坐标里**不含滚动偏移**；滚动靠给整个 Canvas 项
//      赋 x 来做，场景图直接搬已有纹理，不重跑 JS、不重描路径。重绘只发生在新样本 / 改尺寸时。
//      驱动是 NumberAnimation 而不是 JS 定时器：渲染线程按 vsync 插值，观感 60fps，每帧 JS 开销为 0。
//   2) 静态遮罩层（cornerMask）——平移层没法自带圆角裁剪（裁剪会跟着一起平移），所以改成矩形
//      裁剪(root.clip) + 一层「画一次就不动」的角落遮罩，用行背后的底色把溢出圆角的四个角盖回去。
//
// 一句话：**滚动不许触发重绘。**
Item {
    id: root
    // 平移层比可见区宽一格，必须裁掉溢出的那一格。
    clip: true
    property var up: []
    property var down: []
    property color upColor: "#b14a4a"
    property color downColor: "#5bb44b"
    property real corner: 5            // 与行的 radius 一致
    property real headroom: 0.75       // 曲线最高只占行高的这个比例，给文字留出上方空间
    // 行的圆角之外露出来的是**设备列表背后的底色**，也就是页面内容卡（Main.qml 里 StackLayout
    // 外面那层 Card）。角落遮罩用它把平移层溢出到圆角外的部分盖回去，视觉等价于原来的圆角裁剪。
    // 换页面/换容器时这个默认值会失真，那时由调用方显式传入。
    property color backdrop: Theme.card
    // 量程下限 128KB/s：闲着时几百字节的抖动不会被放大成满屏山峰。
    readonly property real floorScale: 131072

    // 两条序列共用同一个点数与间距：模型侧 up/down 是等长定长（kHistPoints=40）一起采的，
    // 各算各的 step 只会让两条曲线在平移时错开。
    readonly property int histLen: Math.max((root.down || []).length, (root.up || []).length)
    // 相邻两点的水平间距，也就是一拍滑过的距离。**必须取整**：平移层是一张已光栅化的纹理，
    // 落在小数位置就会被重采样。ceil 而不是 round：round 会在 width/(histLen-2) 向下取整时
    // 让内容盖不满可见宽度，右侧漏出空隙。
    readonly property real step: histLen > 2 ? Math.max(1, Math.ceil(width / (histLen - 2))) : 0
    // 滚动位移的连续量；真正赋给 Canvas 的 x 是它取整后的值。
    property real slideX: 0
    // 当前量程。显式属性而不是每帧现算：一拍才可能变一次。
    property real maxScale: floorScale

    // —— 滚动周期 ——
    // periodMs 不写死 1000：这张图的采样节拍是模型侧节流出来的（kHistMinIntervalMs=800 + 连接
    // 轮询 1s），实际间隔会飘。用**上一拍实测的间隔**当滚动周期，滑到位的时刻才对得上下一拍。
    property double lastPushMs: Date.now()
    property real periodMs: 1000

    // up/down 是同一拍里分两次赋过来的（两个 role 各触发一次），200ms 内的第二次不算新的一拍——
    // 否则周期会被算成 0ms，曲线瞬间滑到底、看着还是在跳。
    function onSample() {
        var now = Date.now();
        var dt = now - lastPushMs;
        if (dt < 200)
            return;
        periodMs = Math.max(500, Math.min(2000, dt));
        lastPushMs = now;
        recomputeMax();
        canvas.requestPaint();
        // 不可见时不起跑：非代理行、切走的页面、被 ListView 回收的委托照样会被喂数据。
        if (root.visible)
            slide.restart();
    }

    function recomputeMax() {
        var d = root.down || [], u = root.up || [];
        var m = floorScale;
        for (var i = 0; i < histLen; ++i) {
            if (i < d.length && d[i] > m) m = d[i];
            if (i < u.length && u[i] > m) m = u[i];
        }
        maxScale = m;
    }

    onUpChanged: root.onSample()
    onDownChanged: root.onSample()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onVisibleChanged: {
        if (!visible) {
            slide.stop();
            slideX = 0;
        }
    }

    // ——— 平移驱动 ———
    NumberAnimation {
        id: slide
        target: root
        property: "slideX"
        from: 0
        to: -root.step
        duration: root.periodMs
    }

    // ——— 平移层：只画会跟着滚动走的东西 ———
    Canvas {
        id: canvas
        // histLen-1 格宽（histLen 个点、整数间距）。
        width: root.step * Math.max(0, root.histLen - 1)
        height: root.height
        // **右对齐**摆放：一拍走完（slideX = -step）时最新那个点正好落在可见区右边缘上。
        // 整体取整，理由见 step 的注释。
        x: Math.round(root.width - width + root.step + root.slideX)
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var W = width, H = height;
            if (W <= 0 || H <= 0 || root.histLen < 2)
                return;
            var d = root.down || [], u = root.up || [];
            var max = root.maxScale;
            var step = root.step;

            // 圆角由外面的静态遮罩层负责，这里不做裁剪（裁剪会跟着平移一起跑）。
            function series(arr, c, fillAlpha, lineAlpha) {
                if (!arr || arr.length < 3)
                    return;
                var x = function (i) { return i * step; };
                var y = function (v) { return H - (Math.max(0, v) / max) * H * root.headroom; };
                ctx.beginPath();
                ctx.moveTo(x(0), H);
                for (var i = 0; i < arr.length; ++i)
                    ctx.lineTo(x(i), y(arr[i]));
                ctx.lineTo(x(arr.length - 1), H);
                ctx.closePath();
                ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, fillAlpha);
                ctx.fill();

                ctx.beginPath();
                for (var j = 0; j < arr.length; ++j) {
                    if (j === 0) ctx.moveTo(x(j), y(arr[j]));
                    else ctx.lineTo(x(j), y(arr[j]));
                }
                ctx.lineWidth = 1.5;
                ctx.lineJoin = "round";
                ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, lineAlpha);
                ctx.stroke();
            }

            // 下行在下、上行在上：上行通常小得多，压在后面画才不会被下行的填充盖住。
            series(d, root.downColor, 0.17, 0.50);
            series(u, root.upColor, 0.14, 0.45);
        }
    }

    // ——— 静态遮罩层：把溢出到行圆角之外的四个角盖回底色 ———
    // 只在圆角/底色变化时重画，滚动时一帧都不动。
    //
    // 四个 corner×corner 的小画布，**不是一张铺满整行的大画布**：后者即使不重绘，每帧也要把
    // 一整层 580×60 的透明像素合成进去，白白吃掉这次优化省下来的量。四个 5×5 可以忽略不计。
    Repeater {
        model: root.corner > 0 ? 4 : 0
        Canvas {
            id: wedge
            required property int index
            readonly property real r: root.corner
            // 0=左上 1=右上 2=右下 3=左下
            width: r
            height: r
            x: (index === 1 || index === 2) ? root.width - r : 0
            y: (index === 2 || index === 3) ? root.height - r : 0
            onRChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d");
                ctx.reset();
                if (r <= 0)
                    return;
                // 方角 C、两条边上距 C 为 r 的 A/B，再用 arcTo 沿圆弧从 A 回到 B —— 围出的就是
                // 「方角减去圆弧」的那片月牙。坐标是本画布的局部坐标。
                var pts = [
                    [0, 0, r, 0, 0, r], // 左上
                    [r, 0, 0, 0, r, r], // 右上
                    [r, r, r, 0, 0, r], // 右下
                    [0, r, 0, 0, r, r]  // 左下
                ][index];
                ctx.fillStyle = root.backdrop;
                ctx.beginPath();
                ctx.moveTo(pts[0], pts[1]);
                ctx.lineTo(pts[2], pts[3]);
                ctx.arcTo(pts[0], pts[1], pts[4], pts[5], r);
                ctx.closePath();
                ctx.fill();
            }
            Connections {
                target: root
                function onBackdropChanged() { wedge.requestPaint(); }
            }
        }
    }
}
