#pragma once

#include <QColor>
#include <QQueue>
#include <QString>
#include <QVector>
#include <QWidget>

class QTimer;

// 复刻旧项目 plugins/tubiao.js：平滑左滚折线 + 四分网格 + 右侧速度刻度 + 左上标题 + 阶梯式自适应最大值
class TrafficChart final : public QWidget
{
    Q_OBJECT

public:
    explicit TrafficChart(QString title, QColor lineColor, QWidget *parent = nullptr);

    void addSample(qint64 value);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void tick();    // 50ms：滚动偏移
    void replay();  // 1s：推入一个新点
    double currentMax() const;

    QString m_title;
    QColor m_lineColor;
    QVector<double> m_pointers;   // 可见数据点
    QQueue<double> m_pending;     // 待入队样本
    double m_offset = 0.0;
    int m_maxPointer = 42;        // length + 2
    QTimer *m_scrollTimer = nullptr;
    QTimer *m_stepTimer = nullptr;

    static constexpr double kBase = 131072.0;   // 128KB 基准
    static constexpr double kRule = 2097152.0;  // 2MB 步进
};
