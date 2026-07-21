import QtQuick
import ClashAuto

// 轻量实时带宽折线（自绘 Canvas，无外部模块依赖）。调用 push(value) 追加一个样本，
// 保留最近 N 个点、自适应量程绘制。对齐 Widgets 版 TrafficChart 的观感（简化版）。
Item {
    id: root
    property string title: ""
    property color lineColor: Theme.accent
    property int capacity: 60
    property var samples: []

    function push(value) {
        samples.push(Math.max(0, value));
        if (samples.length > capacity)
            samples.shift();
        canvas.requestPaint();
    }

    Rectangle {
        anchors.fill: parent
        radius: 4
        color: Theme.dark ? "#111111" : "#f5f5f5"
    }

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 6
        text: root.title
        font.pixelSize: 11
        color: Theme.textMuted
        z: 2
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.margins: 4
        onPaint: {
            var ctx = getContext("2d");
            ctx.reset();
            var n = root.samples.length;
            if (n < 2)
                return;
            var maxV = 1;
            for (var i = 0; i < n; ++i)
                maxV = Math.max(maxV, root.samples[i]);
            var w = width, h = height;
            var stepX = w / (root.capacity - 1);
            var toY = function (v) { return h - (v / maxV) * (h - 8) - 2; };

            // 填充
            ctx.beginPath();
            ctx.moveTo(w - (n - 1) * stepX, h);
            for (var j = 0; j < n; ++j)
                ctx.lineTo(w - (n - 1 - j) * stepX, toY(root.samples[j]));
            ctx.lineTo(w, h);
            ctx.closePath();
            ctx.fillStyle = Qt.rgba(root.lineColor.r, root.lineColor.g, root.lineColor.b, 0.18);
            ctx.fill();

            // 折线
            ctx.beginPath();
            for (var k = 0; k < n; ++k) {
                var x = w - (n - 1 - k) * stepX;
                var y = toY(root.samples[k]);
                if (k === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
            }
            ctx.lineWidth = 1.5;
            ctx.strokeStyle = root.lineColor;
            ctx.stroke();
        }
    }
}
