import QtQuick
import ClashAuto

// 设备行的**背景**实时流量图：下行(绿) / 上行(红) 两条面积曲线叠在一起，共用同一量程。
// 数据是 DeviceListModel 里每设备一条的定长速率历史（模型侧采样，见 DeviceListModel::sampleHistory
// ——挂在委托上的话，ListView 一回收委托历史就没了）。
//
// 只画在「被代理」的设备行上：其余设备的流量根本不经 mihomo，画出来永远是一条贴底的 0 线。
// 速率为 0 时**照样画**（一条贴底的线 + 极淡的填充），这是「已接管、此刻没流量」的正常样子，
// 不是「没数据」——所以不做 visible 判空。
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

    onUpChanged: canvas.requestPaint()
    onDownChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()

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
                if (!arr || arr.length < 2)
                    return;
                var step = W / (arr.length - 1);
                var y = function (v) { return H - (Math.max(0, v) / max) * H * root.headroom; };
                ctx.beginPath();
                ctx.moveTo(0, H);
                for (var i = 0; i < arr.length; ++i)
                    ctx.lineTo(i * step, y(arr[i]));
                ctx.lineTo(W, H);
                ctx.closePath();
                ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, fillAlpha);
                ctx.fill();

                ctx.beginPath();
                for (var j = 0; j < arr.length; ++j) {
                    if (j === 0) ctx.moveTo(0, y(arr[j]));
                    else ctx.lineTo(j * step, y(arr[j]));
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
