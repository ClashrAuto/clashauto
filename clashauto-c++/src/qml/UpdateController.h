#pragma once

// UpdateController —— 更新窗专用适配器（QObject）。
// 契约（见 qml/ARCHITECTURE.md）：不改后端类、也不改 QmlBridge；把 Widgets 版
// MainWindow::showUpdateDialog() / checkForUpdate() / launchSilentUpdateAndRestart*()
// 这一整套「检查更新 → 拉 release 列表 + 内核版本 → 一键更新（下载→校验→安装重启）」
// 逻辑原样搬进来，通过 context property `update` 暴露给 qml/UpdateWindow.qml。
//
// 端点与 Widgets 版完全一致：
//   - https://api.github.com/repos/ClashrAuto/clashauto/releases        （正式/测试版列表）
//   - https://api.github.com/repos/ClashrAuto/clash/releases/tags/Prerelease-master （内核，滚动 prerelease）
//   - 下载资源 browser_download_url
//   - 校验用同名 <资源名>.sha256 边车
#include "AppConfig.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class CoreController;

class UpdateController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // 下载统计（进度条下方显示：速度 + 已下载/总量）
    Q_PROPERTY(QString downloadSpeed READ downloadSpeed NOTIFY downloadStatsChanged)
    Q_PROPERTY(QString downloadedText READ downloadedText NOTIFY downloadStatsChanged)
    Q_PROPERTY(QString totalText READ totalText NOTIFY downloadStatsChanged)
    // 正式版 tab
    Q_PROPERTY(QString releaseVersion READ releaseVersion NOTIFY releaseChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY releaseChanged)
    Q_PROPERTY(QStringList releaseAssets READ releaseAssets NOTIFY releaseChanged)
    // 测试版 tab
    Q_PROPERTY(QString betaVersion READ betaVersion NOTIFY betaChanged)
    Q_PROPERTY(QString betaNotes READ betaNotes NOTIFY betaChanged)
    Q_PROPERTY(QStringList betaAssets READ betaAssets NOTIFY betaChanged)
    // 内核 tab
    Q_PROPERTY(QString coreVersion READ coreVersion NOTIFY coreChanged)
    Q_PROPERTY(QString coreNotes READ coreNotes NOTIFY coreChanged)

public:
    explicit UpdateController(AppConfig config, CoreController *core, QObject *parent = nullptr);

    QString currentVersion() const;
    bool checking() const { return m_checking; }
    bool downloading() const { return m_downloading; }
    int progress() const { return m_progress; }
    QString status() const { return m_status; }
    QString downloadSpeed() const { return m_speedText; }
    QString downloadedText() const { return m_downloadedText; }
    QString totalText() const { return m_totalText; }

    QString releaseVersion() const { return m_releaseVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    QStringList releaseAssets() const { return m_releaseAssetNames; }
    QString betaVersion() const { return m_betaVersion; }
    QString betaNotes() const { return m_betaNotes; }
    QStringList betaAssets() const { return m_betaAssetNames; }
    QString coreVersion() const { return m_coreVersion; }
    QString coreNotes() const { return m_coreNotes; }

    // 打开更新窗即调用：拉 release 列表 + 内核版本（对齐 showUpdateDialog 打开即发起的请求）。
    Q_INVOKABLE void refresh();
    // ★ 按页签分开取，别一次全取。更新页顶上的三段（程序/内核/GeoIP）各看各的数据，
    //   「获取更新」按下时只该刷新**当前那一段** —— 混在一起取的代价不是多一次请求，
    //   而是按钮的「检查中/已完成」状态跟着另一段走，用户在内核页按下去看到的是程序那份
    //   在转圈，内核这边什么都没变。
    Q_INVOKABLE void refreshApp();   // 程序：release 列表（正式 + 测试）
    Q_INVOKABLE void refreshCore();  // 内核：本地 `-v` + 上游最新版与说明
    // 推荐资源下标（优先含 setup 的安装包）；tab: 0=正式 1=测试。无合适资源返回 -1。
    // 非 const：macOS 上「这个 release 里没有本产品线的包」要写进 status 说给用户看，
    // 静默返回 -1 的话点「更新」是毫无反应，用户只会以为程序坏了。
    Q_INVOKABLE int recommendedIndex(int tab);
    // 「一键更新」：下载所选资源 → 校验 sha256 → 静默安装并自动重启。
    // tab: 0=正式 1=测试；index=资源下标。
    Q_INVOKABLE void oneClickUpdate(int tab, int index);
    // 记住「不再提示」当前正式版版本号（对齐 update/skipTag）。
    Q_INVOKABLE void skipCurrentRelease();
    // 取消当前下载：abort reply，finished 里按 m_cancelled 走「已取消」而非「失败」。
    Q_INVOKABLE void cancelDownload();

signals:
    void checkingChanged();
    void downloadingChanged();
    void progressChanged();
    void statusChanged();
    void downloadStatsChanged();
    void releaseChanged();
    void betaChanged();
    void coreChanged();
    void failed(const QString &reason); // 供 QML 弹失败提示

private:
    struct Asset {
        QString name;
        QString url;       // browser_download_url
        QString sha256Url; // 同名 <资源名>.sha256 边车直连 url（缺失为空）
    };

    void applyDownloadProxy(QNetworkAccessManager *nam) const;
    void fetchReleases();
    void fetchCore();
    bool verifySha256(const QString &filePath, const QString &expectedHexLower) const;
    void doExecute(const QString &savePath, const QString &name);
    void setChecking(bool v);
    void setDownloading(bool v);
    void setProgress(int v);
    void setStatus(const QString &s);
#if defined(Q_OS_WIN)
    void launchSilentUpdateAndRestart(const QString &installerPath);
    // 便携版 zip 更新：等本进程退出 → 解压覆盖到项目目录（保留已下载内核）→ 重启。
    void launchZipUpdateAndRestart(const QString &zipPath);
#elif defined(Q_OS_MACOS)
    void launchSilentUpdateAndRestartMac(const QString &dmgPath);
#endif

    AppConfig m_config;
    CoreController *m_core = nullptr;
    QNetworkAccessManager *m_nam = nullptr;

    bool m_checking = false;
    bool m_downloading = false;
    int m_progress = 0;
    QString m_status;
    // 下载统计 + 取消
    QString m_speedText;
    QString m_downloadedText;
    QString m_totalText;
    QNetworkReply *m_dlReply = nullptr; // 当前下载 reply（用于取消）
    QElapsedTimer m_dlTimer;            // 计速用
    qint64 m_lastBytes = 0;
    qint64 m_lastMs = 0;
    bool m_cancelled = false; // 用户主动取消标记（finished 里据此不弹失败）

    QString m_releaseVersion = QStringLiteral("-");
    QString m_releaseNotes;
    QVector<Asset> m_releaseAssets;
    QStringList m_releaseAssetNames;

    QString m_betaVersion = QStringLiteral("-");
    QString m_betaNotes;
    QVector<Asset> m_betaAssets;
    QStringList m_betaAssetNames;

    QString m_coreVersion;
    QString m_coreNotes;
};
