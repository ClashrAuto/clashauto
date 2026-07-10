#include "TrafficChart.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
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
}

TrafficChart::TrafficChart(QString title, QColor lineColor, QWidget *parent)
    : QWidget(parent), m_title(std::move(title)), m_lineColor(std::move(lineColor))
{
    setMinimumHeight(110);
    m_pointers.fill(1.0, m_maxPointer);

    m_scrollTimer = new QTimer(this);
    connect(m_scrollTimer, &QTimer::timeout, this, &TrafficChart::tick);
    m_scrollTimer->start(50);

    m_stepTimer = new QTimer(this);
    connect(m_stepTimer, &QTimer::timeout, this, &TrafficChart::replay);
    m_stepTimer->start(1000);
}

void TrafficChart::addSample(qint64 value)
{
    m_pending.enqueue(static_cast<double>(qMax<qint64>(0, value)));
}

void TrafficChart::tick()
{
    // offset -= width/(maxPointer-2)/(loop/speed)，loop/speed = 1000/50 = 20
    const double spacing = width() / static_cast<double>(m_maxPointer - 2);
    m_offset -= spacing / 20.0;
    if (isVisible()) {
        update();
    }
}

void TrafficChart::replay()
{
    const double point = m_pending.isEmpty() ? (m_pointers.isEmpty() ? 1.0 : m_pointers.last()) : m_pending.dequeue();
    m_pointers.push_back(point);
    while (m_pointers.size() > m_maxPointer) {
        m_pointers.pop_front();
    }
    m_offset = 0.0;
    if (isVisible()) {
        update();
    }
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

void TrafficChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const double W = width();
    const double H = height();
    const int r = m_lineColor.red();
    const int g = m_lineColor.green();
    const int b = m_lineColor.blue();

    // 背景填充（tubiao：RGBA 0.03）
    painter.fillRect(rect(), QColor(r, g, b, 8));

    // 四分网格线（顶 + 四条）
    painter.setPen(QPen(QColor(r, g, b, 26), 1));
    for (int i = 0; i <= 4; ++i) {
        const double y = H / 4.0 * i;
        painter.drawLine(QPointF(0, y), QPointF(W, y));
    }

    const double max = currentMax();

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

    // 折线（带滚动偏移）
    const double lineWidth = 3.0;
    const double spacing = W / static_cast<double>(m_maxPointer - 2);
    QPainterPath path;
    for (int i = 0; i < m_pointers.size(); ++i) {
        double y = H - (m_pointers[i] / max) * H - lineWidth / 2.0;
        if (y < lineWidth / 2.0) {
            y = lineWidth / 2.0;
        }
        const double x = i * spacing + m_offset;
        if (i == 0) {
            path.moveTo(x, y);
        } else {
            path.lineTo(x, y);
        }
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(r, g, b, 180), lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);
}
