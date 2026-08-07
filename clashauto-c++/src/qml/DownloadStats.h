#pragma once

// 一次下载的三个**显示量**（速度 / 已下载 / 总量）+ 算速度所需的采样状态。
//
// 三处下载各自抄过一份同样的 `fmtBytes` 与「每 500ms 采一次样」的算法
//（程序更新 UpdateController、Npcap 安装器 NpcapInstaller、内核/GeoIP 更新
// SettingsController）。抄第三份的时候抽出来 —— 显示口径必须一致：同一个界面上
// 「1.5 MB」和「1536 KB」并排出现，看着像两个不同的东西。
//
// 纯内联、无 Q_OBJECT：它只是一个值对象，谁需要谁 `#include`，不进 CMake 源表。

#include <QElapsedTimer>
#include <QString>

struct DownloadStats {
    QString speedText;      // "1.5 MB/s"；第一次采样出来之前为空
    QString downloadedText; // "5.0 MB"
    QString totalText;      // "12.0 MB"；总量未知时为 "?"

    /// 人类可读字节数：1023 → "1023 B"，1536 → "1.5 KB"。
    static QString fmtBytes(qint64 v)
    {
        double n = v < 0 ? 0 : static_cast<double>(v);
        static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
        int i = 0;
        while (n >= 1024.0 && i < 4) {
            n /= 1024.0;
            ++i;
        }
        return QString::number(n, 'f', i == 0 ? 0 : 1) + QLatin1Char(' ') + QLatin1String(u[i]);
    }

    /// 开始一次新下载前调用：清空文本并重开计时。
    void reset()
    {
        speedText.clear();
        downloadedText.clear();
        totalText.clear();
        m_timer.start();
        m_lastMs = 0;
        m_lastBytes = 0;
    }

    /// 接 `QNetworkReply::downloadProgress`。速度**每 ≥500ms 才采一次**：
    /// 每个进度回调都算一次的话，两次回调间隔可以小到几毫秒，算出来的瞬时速率
    /// 在 0 和几十 MB/s 之间乱跳，读数没法看。
    void update(qint64 received, qint64 total)
    {
        downloadedText = fmtBytes(received);
        totalText = total > 0 ? fmtBytes(total) : QStringLiteral("?");
        if (!m_timer.isValid()) {
            return;
        }
        const qint64 nowMs = m_timer.elapsed();
        const qint64 dt = nowMs - m_lastMs;
        if (dt >= 500) {
            const double bps = (received - m_lastBytes) * 1000.0 / static_cast<double>(dt);
            speedText = fmtBytes(static_cast<qint64>(bps)) + QStringLiteral("/s");
            m_lastMs = nowMs;
            m_lastBytes = received;
        }
    }

private:
    QElapsedTimer m_timer;
    qint64 m_lastMs = 0;
    qint64 m_lastBytes = 0;
};
