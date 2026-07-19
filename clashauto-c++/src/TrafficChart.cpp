#include "TrafficChart.h"

#include <QCoreApplication>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QTimer>
#include <cmath>

namespace {
QString speedText(double v)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    return QString::asprintf("%.2f %s/s", v, units[i]);
}

// 所有图表共用同一定时器：timeout 在同一事件循环轮次派发给两张图，各自 update() 的脏区
// 由 Qt 合并成一次 backing-store 刷新。此前每图各建 50ms 定时器、相位随机错开，
// 每秒最多 40 次互不合并的刷新——无 GPU 的虚拟机上刷新是纯 CPU 拷贝，正是卡顿大头。
QTimer *sharedTimer(int intervalMs)
{
    static QHash<int, QTimer *> timers;
    QTimer *&timer = timers[intervalMs];
    if (!timer) {
        timer = new QTimer(QCoreApplication::instance());
        timer->start(intervalMs);
    }
    return timer;
}
} // namespace

TrafficChart::TrafficChart(QString title, QColor lineColor, QWidget *parent)
    : QWidget(parent), m_title(std::move(title)), m_lineColor(std::move(lineColor))
{
    setMinimumHeight(60); // 低下限：避免与固定 200px 流量卡区争高导致卡片被挤（真机验证）
    m_pointers.fill(1.0, m_maxPointer);
    m_sinceReplay.start();

    connect(sharedTimer(50), &QTimer::timeout, this, &TrafficChart::tick);
    connect(sharedTimer(1000), &QTimer::timeout, this, &TrafficChart::replay);
}

void TrafficChart::addSample(qint64 value)
{
    m_pending.enqueue(static_cast<double>(qMax<qint64>(0, value)));
}

void TrafficChart::tick()
{
    // 滚动相位按「距上次入点的真实时间」算，而不是每 tick 固定挪一格：Windows 粗粒度
    // 定时器 50ms 实际在 ~47/62ms 间抖动，固定步进会滚得忽快忽慢；事件循环被别的活
    // 卡住错过几拍时，这里也会直接跳到正确相位，而不是越落越远。
    const double spacing = width() / static_cast<double>(m_maxPointer - 2);
    const double phase = qMin(1.0, m_sinceReplay.elapsed() / 1000.0);
    m_offset = -spacing * phase;
    scheduleRepaint();
}

void TrafficChart::replay()
{
    const double point = m_pending.isEmpty() ? (m_pointers.isEmpty() ? 1.0 : m_pointers.last()) : m_pending.dequeue();
    m_pointers.push_back(point);
    while (m_pointers.size() > m_maxPointer) {
        m_pointers.pop_front();
    }
    m_offset = 0.0;
    m_sinceReplay.restart();
    scheduleRepaint();
}

void TrafficChart::scheduleRepaint()
{
    // 不可见（切到其它页/隐藏到托盘）不画；窗口最小化时 isVisible() 仍为 true，
    // 需单独拦，否则最小化在任务栏里还在 20FPS 白绘。
    if (!isVisible()) {
        return;
    }
    const QWidget *top = window();
    if (top && top->isMinimized()) {
        return;
    }
    update();
}

double TrafficChart::currentMax() const
{
    double max = kBase;
    for (double p : m_pointers) {
        if (p > max) {
            max = std::ceil(p / kRule) * kRule;
        }
    }
    return max;
}

void TrafficChart::ensureStaticLayer(double max)
{
    // 背景、网格、右侧速度刻度、标题只依赖尺寸/量程/缩放，缓存成位图逐帧直贴。
    // 此前每帧 5 次 drawText + 网格线全部重新光栅化，软件渲染下文本是最贵的一项。
    const qreal dpr = devicePixelRatioF();
    if (!m_staticLayer.isNull() && m_staticSize == size() && m_staticMax == max && m_staticDpr == dpr) {
        return;
    }
    m_staticSize = size();
    m_staticMax = max;
    m_staticDpr = dpr;

    const double W = width();
    const double H = height();
    const int r = m_lineColor.red();
    const int g = m_lineColor.green();
    const int b = m_lineColor.blue();

    m_staticLayer = QPixmap(size() * dpr);
    m_staticLayer.setDevicePixelRatio(dpr);
    m_staticLayer.fill(Qt::transparent);
    QPainter painter(&m_staticLayer);
    painter.setRenderHint(QPainter::Antialiasing);

    // 背景填充（tubiao：RGBA 0.03）
    painter.fillRect(QRectF(0, 0, W, H), QColor(r, g, b, 8));

    // 四分网格线（顶 + 四条）
    painter.setPen(QPen(QColor(r, g, b, 26), 1));
    for (int i = 0; i <= 4; ++i) {
        const double y = H / 4.0 * i;
        painter.drawLine(QPointF(0, y), QPointF(W, y));
    }

    // 右侧速度刻度（右对齐）
    painter.setPen(QColor(150, 150, 150));
    QFont scaleFont = painter.font();
    scaleFont.setPointSize(8);
    painter.setFont(scaleFont);
    const double labels[4] = {max, max * 3.0 / 4.0, max * 2.0 / 4.0, max / 4.0};
    for (int i = 0; i < 4; ++i) {
        const double y = H / 4.0 * i + 4.0;
        painter.drawText(QRectF(0, y, W - 6, 16), Qt::AlignRight | Qt::AlignTop, speedText(labels[i]));
    }

    // 左上标题
    painter.setPen(QColor(r, g, b, 180));
    QFont titleFont = painter.font();
    titleFont.setBold(true);
    titleFont.setPointSize(11);
    painter.setFont(titleFont);
    painter.drawText(QRectF(10, 8, W - 20, 22), Qt::AlignLeft | Qt::AlignTop, m_title);
}

void TrafficChart::paintEvent(QPaintEvent *)
{
    if (width() <= 0 || height() <= 0) {
        return;
    }
    const double max = currentMax();
    ensureStaticLayer(max);

    QPainter painter(this);
    painter.drawPixmap(0, 0, m_staticLayer);

    // 折线（带滚动偏移）——唯一逐帧变化的内容，只有它走抗锯齿矢量绘制
    const double W = width();
    const double H = height();
    const double lineWidth = 3.0;
    const double spacing = W / static_cast<double>(m_maxPointer - 2);
    m_linePoints.resize(m_pointers.size());
    for (int i = 0; i < m_pointers.size(); ++i) {
        double y = H - (m_pointers[i] / max) * H - lineWidth / 2.0;
        if (y < lineWidth / 2.0) {
            y = lineWidth / 2.0;
        }
        m_linePoints[i] = QPointF(i * spacing + m_offset, y);
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    const int r = m_lineColor.red();
    const int g = m_lineColor.green();
    const int b = m_lineColor.blue();
    painter.setPen(QPen(QColor(r, g, b, 180), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(m_linePoints.constData(), static_cast<int>(m_linePoints.size()));
}
