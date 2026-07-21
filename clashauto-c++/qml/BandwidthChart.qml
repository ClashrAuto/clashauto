import QtQuick
import ClashAuto

// 实时带宽折线 —— 严格复刻 Widgets 版 TrafficChart（plugins/tubiao.js）：
//  · 固定 42 个点（可见 40 + 两端各 1 富余，滚动全程无缝、右侧不留缺口），初始填 1.0；
//  · 50ms 按「距上次入点的真实经过时间」推进滚动相位(0..1)，整条连续左滑；每秒 push 追加一点、相位归零；
//  · 四分网格 + 右侧速度刻度(max/¾/½/¼) + 左上标题；阶梯式自适应量程(128KB 基准 / 2MB 步进)；
//  · 只画折线(3px 圆角、线色 α0.70)，无面积填充；背景为线色极淡底(α0.03)。
Item {
    id: root
    property string title: ""
    property color lineColor: Theme.accent
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
            ctx.font = "bold 11px '" + Theme.uiFont + "'";
            ctx.textAlign = "left";
            ctx.fillText(root.title, 10, 18);

            // 折线（带滚动偏移；两端富余点 → 无缝、右侧不留缺口；无面积填充）
            var lw = 3.0;
            var spacing = W / (root.maxPointer - 2);
            var offset = -spacing * root.phase;
            ctx.beginPath();
            for (var i = 0; i < n; ++i) {
                var y = H - (root.pointers[i] / max) * H - lw / 2;
                if (y < lw / 2)
                    y = lw / 2;
                var x = i * spacing + offset;
                if (i === 0)
                    ctx.moveTo(x, y);
                else
                    ctx.lineTo(x, y);
            }
            ctx.lineWidth = lw;
            ctx.lineCap = "round";
            ctx.lineJoin = "round";
            ctx.strokeStyle = Qt.rgba(lc.r, lc.g, lc.b, 0.70);
            ctx.stroke();
        }
    }
}
