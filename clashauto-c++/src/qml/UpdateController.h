#pragma once

// UpdateController —— 更新窗专用适配器（QObject）。
// 契约（见 qml/ARCHITECTURE.md）：不改后端类、也不改 QmlBridge；把 Widgets 版
// MainWindow::showUpdateDialog() / checkForUpdate() / launchSilentUpdateAndRestart*()
// 这一整套「检查更新 → 拉 release 列表 + 内核版本 → 一键更新（下载→校验→安装重启）」
// 逻辑原样搬进来，通过 context property `update` 暴露给 qml/UpdateWindow.qml。
//
// 端点与 Widgets 版完全一致：
//   - https://api.github.com/repos/ClashrAuto/clashauto/releases        （正式/测试版列表）
//   - https://api.github.com/repos/MetaCubeX/mihomo/releases/latest      （内核最新版）
//   - 下载资源 browser_download_url（勾国内代理时前缀 https://ghfast.top/）
//   - 校验用同名 <资源名>.sha256 边车（永远官方直连，不加镜像）
#include "AppConfig.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QNetworkAccessManager;
class CoreController;

class UpdateController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY downloadingChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
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
    // 推荐资源下标（优先含 setup 的安装包）；tab: 0=正式 1=测试。无资源返回 -1。
    Q_INVOKABLE int recommendedIndex(int tab) const;
    // 「一键更新」：下载所选资源 → 校验 sha256 → 静默安装并自动重启。
    // tab: 0=正式 1=测试；index=资源下标；useMirror=国内代理下载（ghfast.top 直连）。
    Q_INVOKABLE void oneClickUpdate(int tab, int index, bool useMirror);
    // 记住「不再提示」当前正式版版本号（对齐 update/skipTag）。
    Q_INVOKABLE void skipCurrentRelease();

signals:
    void checkingChanged();
    void downloadingChanged();
    void progressChanged();
    void statusChanged();
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
