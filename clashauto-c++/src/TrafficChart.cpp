#include "TrafficChart.h"

#include <QPainter>
#include <QPainterPath>

TrafficChart::TrafficChart(QString title, QColor lineColor, QWidget *parent)
    : QWidget(parent), m_title(std::move(title)), m_lineColor(std::move(lineColor))
{
    setMinimumHeight(110);
}

void TrafficChart::addSample(qint64 value)
{
    m_samples.push_back(qMax<qint64>(0, value));
    while (m_samples.size() > m_capacity) {
        m_samples.pop_front();
    }
    update();
}

void TrafficChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF r = rect().adjusted(8, 8, -8, -8);
    painter.fillRect(rect(), QColor(255, 255, 255, 6));

    painter.setPen(QPen(QColor(85, 85, 85, 70), 1));
    for (int i = 1; i < 4; ++i) {
        const qreal y = r.top() + r.height() * i / 4.0;
        painter.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }

    painter.setPen(QColor(150, 150, 150));
    painter.drawText(r.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignTop, m_title);

    if (m_samples.isEmpty()) {
        return;
    }

    qint64 maxValue = 1;
    for (qint64 v : m_samples) {
        maxValue = qMax(maxValue, v);
    }

    QPainterPath path;
    for (int i = 0; i < m_samples.size(); ++i) {
        const qreal x = r.left() + (m_samples.size() == 1 ? 0 : r.width() * i / (m_samples.size() - 1));
        const qreal y = r.bottom() - r.height() * static_cast<qreal>(m_samples[i]) / maxValue;
        if (i == 0) {
            path.moveTo(x, y);
        } else {
            path.lineTo(x, y);
        }
    }

    QColor fill = m_lineColor;
    fill.setAlpha(28);
    QPainterPath area = path;
    area.lineTo(r.right(), r.bottom());
    area.lineTo(r.left(), r.bottom());
    area.closeSubpath();
    painter.fillPath(area, fill);

    QColor line = m_lineColor;
    line.setAlpha(210);
    painter.setPen(QPen(line, 3));
    painter.drawPath(path);
}
