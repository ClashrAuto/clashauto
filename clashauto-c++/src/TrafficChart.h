#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPixmap>
#include <QPointF>
#include <QQueue>
#include <QString>
#include <QVector>
#include <QWidget>

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
    void tick();    // 50ms：按真实流逝时间推进滚动相位
    void replay();  // 1s：推入一个新点并归零相位
    double currentMax() const;
    void ensureStaticLayer(double max); // 背景/网格/刻度/标题缓存进位图，尺寸或量程变了才重画
    void scheduleRepaint();             // 可见且窗口未最小化才 update()

    QString m_title;
    QColor m_lineColor;
    QVector<double> m_pointers;    // 可见数据点
    QQueue<double> m_pending;      // 待入队样本
    QVector<QPointF> m_linePoints; // 折线顶点复用缓冲，避免每帧重新分配
    QPixmap m_staticLayer;         // 静态层缓存：背景+网格+速度刻度+标题（文本光栅化最贵，绝不逐帧画）
    double m_staticMax = -1.0;     // 静态层对应的量程/逻辑尺寸/缩放，任一变化即重建
    QSize m_staticSize;
    qreal m_staticDpr = 0.0;
    QElapsedTimer m_sinceReplay;  // 距上次入点的真实时间：滚动相位由它算出，定时器抖动不影响速度
    double m_offset = 0.0;
    int m_maxPointer = 42;        // length + 2

    static constexpr double kBase = 131072.0;   // 128KB 基准
    static constexpr double kRule = 2097152.0;  // 2MB 步进
};
