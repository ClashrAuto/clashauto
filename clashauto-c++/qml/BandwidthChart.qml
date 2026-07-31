import QtQuick
import Coast

// 实时带宽折线 —— 严格复刻 Widgets 版 TrafficChart（plugins/tubiao.js）：
//  · 固定 42 个点（可见 40 + 两端各 1 富余，滚动全程无缝、右侧不留缺口），初始填 1.0；
//  · 50ms 按「距上次入点的真实经过时间」推进滚动相位(0..1)，整条连续左滑；每秒 push 追加一点、相位归零；
//  · 四分网格 + 右侧速度刻度(max/¾/½/¼) + 左上标题；阶梯式自适应量程(128KB 基准 / 2MB 步进)；
//  · 只画折线(3px 圆角、线色 α0.70)，无面积填充；背景为线色极淡底(α0.03)。
Item {
    id: root
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
    property double lastPushMs: Date.now()
    property real phase: 0

    Component.onCompleted: {
        var a = [];
        for (var i = 0; i < maxPointer; ++i)
            a.push(1.0);
        pointers = a;
        canvas.requestPaint();
    }

    // 每秒一个样本 = 原 replay：推入新点、越界弹出最旧、相位归零。
    function push(value) {
        pointers.push(Math.max(0, value));
        while (pointers.length > maxPointer)
            pointers.shift();
        lastPushMs = Date.now();
        phase = 0;
        canvas.requestPaint();
    }

    function currentMax() {
        var m = kBase;
        for (var i = 0; i < pointers.length; ++i)
            if (pointers[i] > m)
                m = Math.ceil(pointers[i] / kRule) * kRule;
        return m;
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

    // 50ms：按真实经过时间推进滚动相位（定时器抖动不影响滚速），连续左滑。
    Timer {
        interval: 50
        repeat: true
        running: root.visible
        onTriggered: {
            root.phase = Math.min(1, (Date.now() - root.lastPushMs) / 1000);
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
            var n = root.pointers.length;
            if (n < 2 || W <= 0 || H <= 0)
                return;
            var lc = root.lineColor;
            var max = root.currentMax();

            if (!root.minimal) {
                // 背景（线色极淡）
                ctx.fillStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.03);
                ctx.fillRect(0, 0, W, H);

                // 四分网格
                ctx.strokeStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.10);
                ctx.lineWidth = 1;
                for (var gi = 0; gi <= 4; ++gi) {
                    var gy = Math.round(H / 4 * gi) + 0.5;
                    ctx.beginPath();
                    ctx.moveTo(0, gy);
                    ctx.lineTo(W, gy);
                    ctx.stroke();
                }

                // 右侧速度刻度（max / ¾ / ½ / ¼）——正文字体（Canvas 不继承应用默认字体，显式指定）
                ctx.fillStyle = "#969696";
                ctx.font = "10px '" + Theme.uiFont + "'";
                ctx.textAlign = "right";
                var labels = [max, max * 3 / 4, max / 2, max / 4];
                for (var li = 0; li < 4; ++li)
                    ctx.fillText(root.speedText(labels[li]), W - 6, H / 4 * li + 12);

                // 左上标题——正文字体（与 UI 一致）
                ctx.fillStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.70);
                ctx.font = "11px '" + Theme.uiFont + "'"; // 全 UI 不加粗
                ctx.textAlign = "left";
                ctx.fillText(root.title, 10, 18);
            }

            // 折线（带滚动偏移；两端富余点 → 无缝、右侧不留缺口）
            var lw = root.minimal ? 2.0 : 3.0;
            var spacing = W / (root.maxPointer - 2);
            var offset = -spacing * root.phase;
            var yOf = function (v) {
                var y = H - (v / max) * H * root.headroom - lw / 2;
                return y < lw / 2 ? lw / 2 : y;
            };

            // minimal 的刻度：**必须按 yOf 定位**，不能沿用非 minimal 那套「H/4 等分」——
            // 那套假设曲线铺满整高，而这里 headroom 只让曲线占下面一截，等分线会全部对不上
            // 曲线的实际高度，刻度就成了骗人的。画在填充之下、文字画在曲线之上（免得被盖住）。
            var ticks = [];
            if (root.minimal) {
                ctx.strokeStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.13);
                ctx.lineWidth = 1;
                const fr = [1.0, 0.75, 0.5, 0.25];
                for (var ti = 0; ti < fr.length; ++ti) {
                    var ty = Math.round(yOf(max * fr[ti])) + 0.5;
                    if (ty < 18) // 顶到卡片标题那一带了就不画这一档
                        continue;
                    ticks.push({ y: ty, v: max * fr[ti] });
                    ctx.beginPath();
                    ctx.moveTo(0, ty);
                    ctx.lineTo(W, ty);
                    ctx.stroke();
                }
            }

            // minimal：线下补一层淡填充。只有一条细线的话，在卡片底纹这个尺度上几乎看不见，
            // 填充才撑得起「这块区域是这张图」的感觉。
            if (root.minimal) {
                ctx.beginPath();
                ctx.moveTo(offset, H);
                for (var fi = 0; fi < n; ++fi)
                    ctx.lineTo(fi * spacing + offset, yOf(root.pointers[fi]));
                ctx.lineTo((n - 1) * spacing + offset, H);
                ctx.closePath();
                ctx.fillStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.16);
                ctx.fill();
            }

            ctx.beginPath();
            for (var i = 0; i < n; ++i) {
                var x = i * spacing + offset;
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

            // 刻度文字最后画：压在曲线之上才不会被填充盖掉。贴右边缘、坐在刻度线上方 3px。
            if (root.minimal && ticks.length > 0) {
                ctx.fillStyle = Theme.dark ? "#9a9a9a" : "#7a7a7a";
                ctx.font = "9px '" + Theme.uiFont + "'";
                ctx.textAlign = "right";
                for (var si = 0; si < ticks.length; ++si)
                    ctx.fillText(root.speedText(ticks[si].v), W - 6, ticks[si].y - 3);
            }
        }
    }
}
