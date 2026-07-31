import QtQuick
import Coast

// 设备行的**背景**实时流量图：下行(绿) / 上行(红) 两条面积曲线叠在一起，共用同一量程。
// 数据是 DeviceListModel 里每设备一条的定长速率历史（模型侧采样，见 DeviceListModel::sampleHistory
// ——挂在委托上的话，ListView 一回收委托历史就没了）。
//
// 只画在「被代理」的设备行上：其余设备的流量根本不经 mihomo，画出来永远是一条贴底的 0 线。
// 速率为 0 时**照样画**（一条贴底的线 + 极淡的填充），这是「已接管、此刻没流量」的正常样子，
// 不是「没数据」——所以不做 visible 判空。
//
// **滚动方式和状态页的 BandwidthChart 一致**：曲线连续左滑，而不是「每来一个样本整条跳一格」。
// 数据本身仍是每秒一拍（模型侧节流采样），所以光靠 onUpChanged 重画就是一秒一跳；这里跟
// BandwidthChart 用同一套办法——50ms 定时按**距上次入点的真实经过时间**推进滚动相位(0..1)，
// 画的时候整条曲线左移 spacing*phase。新样本从右边缘外进入、匀速滑到位，相位归零时正好接上。
Item {
    id: root
    property var up: []
    property var down: []
    property color upColor: "#b14a4a"
    property color downColor: "#5bb44b"
    property real corner: 5            // 与行的 radius 一致：Canvas 自己裁圆角（clip 是直角裁剪，盖不住圆角）
    property real headroom: 0.75       // 曲线最高只占行高的这个比例，给文字留出上方空间
    // 量程下限 128KB/s：闲着时几百字节的抖动不会被放大成满屏山峰。
    readonly property real floorScale: 131072

    // —— 滚动相位 ——
    // periodMs 不写死 1000：这张图的采样节拍是模型侧节流出来的（kHistMinIntervalMs=800 + 连接
    // 轮询 1s），实际间隔会飘。用**上一拍实测的间隔**当滚动周期，滑到位的时刻才对得上下一拍。
    property double lastPushMs: Date.now()
    property real periodMs: 1000
    property real phase: 0

    // up/down 是同一拍里分两次赋过来的（两个 role 各触发一次），200ms 内的第二次不算新的一拍——
    // 否则周期会被算成 0ms，曲线瞬间滑到底、看着还是在跳。
    function onSample() {
        var now = Date.now();
        var dt = now - lastPushMs;
        if (dt < 200)
            return;
        periodMs = Math.max(500, Math.min(2000, dt));
        lastPushMs = now;
        phase = 0;
        canvas.requestPaint();
    }

    onUpChanged: root.onSample()
    onDownChanged: root.onSample()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

    // 50ms = 20fps，与 BandwidthChart 相同。只在行真正可见时跑：非代理行的这张图 visible:false，
    // 页面切走 / 委托被 ListView 回收时也一并停掉，不会有一堆看不见的 Canvas 在后台重画。
    Timer {
        interval: 50
        repeat: true
        running: root.visible
        onTriggered: {
            root.phase = Math.min(1, (Date.now() - root.lastPushMs) / root.periodMs);
            canvas.requestPaint();
        }
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var W = width, H = height;
            if (W <= 0 || H <= 0)
                return;
            var d = root.down || [], u = root.up || [];
            var n = Math.max(d.length, u.length);
            if (n < 2)
                return;

            var max = root.floorScale;
            for (var i = 0; i < n; ++i) {
                if (i < d.length && d[i] > max) max = d[i];
                if (i < u.length && u[i] > max) max = u[i];
            }

            // 圆角裁剪：行是圆角矩形，直接铺满会在四角露出方角。
            ctx.beginPath();
            ctx.roundedRect(0, 0, W, H, root.corner, root.corner);
            ctx.clip();

            function series(arr, c, fillAlpha, lineAlpha) {
                if (!arr || arr.length < 3)
                    return;
                // 横向留一格富余：**可见的是 length-2 段**，最新那点在相位 0 时还在右边缘之外，
                // 滑到相位 1 时才刚好落在右边缘上（下一拍接着入点、相位归零，接得上、不留缺口）。
                var step = W / (arr.length - 2);
                var offset = -step * root.phase;
                var x = function (i) { return i * step + offset; };
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
}
