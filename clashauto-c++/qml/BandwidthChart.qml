import QtQuick
import ClashAuto

// 实时带宽折线（自绘 Canvas）。平滑观感对齐原 Widgets 版 TrafficChart：
// 50ms tick 按「距上次入点的真实经过时间」算滚动相位(0..1)，整条连续左滑；每秒 push() 追加
// 一个样本、相位归零 —— 而不是每秒把整条线整跳一格。
Item {
    id: root
    property string title: ""
    property color lineColor: Theme.accent
    property int capacity: 60
    property var samples: []
    property double lastPushMs: Date.now()
    property real phase: 0 // 0..1：两次入点之间已滚动的比例

    function push(value) {
        samples.push(Math.max(0, value));
        if (samples.length > capacity)
            samples.shift();
        lastPushMs = Date.now();
        phase = 0;
        canvas.requestPaint();
    }

    // 20FPS：按真实经过时间推进滚动相位并重绘（连续左滑）。不可见时停表省电。
    Timer {
        interval: 50
        repeat: true
        running: root.visible
        onTriggered: {
            root.phase = Math.min(1, (Date.now() - root.lastPushMs) / 1000);
            canvas.requestPaint();
        }
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
            var shift = root.phase * stepX;                 // 左滑量 0..一格
            var toY = function (v) { return h - (v / maxV) * (h - 8) - 2; };
            var xAt = function (idx) { return w - (n - 1 - idx) * stepX - shift; };
            var yLast = toY(root.samples[n - 1]);

            // 填充区域（末端补到右边缘，避免滚动时右侧留缺口）
            ctx.beginPath();
            ctx.moveTo(xAt(0), h);
            for (var j = 0; j < n; ++j)
                ctx.lineTo(xAt(j), toY(root.samples[j]));
            ctx.lineTo(w, yLast);
            ctx.lineTo(w, h);
            ctx.closePath();
            ctx.fillStyle = Qt.rgba(root.lineColor.r, root.lineColor.g, root.lineColor.b, 0.18);
            ctx.fill();

            // 折线
            ctx.beginPath();
            for (var k = 0; k < n; ++k) {
                var x = xAt(k), y = toY(root.samples[k]);
                if (k === 0)
                    ctx.moveTo(x, y);
                else
                    ctx.lineTo(x, y);
            }
            ctx.lineTo(w, yLast); // 补到右边缘
            ctx.lineWidth = 1.5;
            ctx.strokeStyle = root.lineColor;
            ctx.stroke();
        }
    }
}
