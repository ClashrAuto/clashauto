#pragma once

#include <QWidget>
#include <QVector>

class TrafficChart final : public QWidget
{
    Q_OBJECT

public:
    explicit TrafficChart(QString title, QColor lineColor, QWidget *parent = nullptr);

    void addSample(qint64 value);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_title;
    QColor m_lineColor;
    QVector<qint64> m_samples;
    int m_capacity = 40;
};
