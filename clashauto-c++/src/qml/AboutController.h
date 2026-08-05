#pragma once

// AboutController —— 关于页专用的小适配器（QObject）。
// 契约（见 qml/ARCHITECTURE.md）：不改后端类、也不改 QmlBridge；这里只把「检查更新」这一
// 关于页独有的动作/状态封装出来，通过 context property `about` 暴露给 QML。
// 逻辑镜像 Widgets 版 MainWindow::checkForUpdate()：GET GitHub releases/latest，比较 tag 与本地版本。
#include "AppConfig.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QTimer;

class AboutController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateAvailableChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    // 内核有新版可更新（主窗版本右上角 "core" 角标据此显示）。
    Q_PROPERTY(bool coreUpdateAvailable READ coreUpdateAvailable NOTIFY coreUpdateAvailableChanged)
    Q_PROPERTY(bool coreChecking READ coreChecking NOTIFY coreCheckingChanged)
    Q_PROPERTY(QString releaseUrl READ releaseUrl CONSTANT)

public:
    explicit AboutController(AppConfig config, QObject *parent = nullptr);

    QString currentVersion() const;
    QString latestVersion() const { return m_latestVersion; }
    bool updateAvailable() const { return m_updateAvailable; }
    bool checking() const { return m_checking; }
    bool coreUpdateAvailable() const { return m_coreUpdateAvailable; }
    bool coreChecking() const { return m_coreChecking; }
    QString releaseUrl() const { return m_releaseUrl; }

    // 手动检查更新（程序）。检查中重复调用会被忽略。
    Q_INVOKABLE void check();
    // 手动检查内核更新。**必须与 check() 分开暴露**：更新页的「获取更新」按页签分发，
    // 在内核页按下时要刷新的是这一份判断（coreUpdateAvailable），而不是程序那份。
    // 以前这里是私有的，QML 只能调到 check()，于是内核页怎么按都不会出现「更新」按钮 ——
    // coreUpdateAvailable 只由每小时的后台 checkAll 改，手动检查根本碰不到它。
    Q_INVOKABLE void checkCore();
    // 启动后台自动检查：立即查一次程序/内核/GeoIP，之后每小时一次；
    // GeoIP 有新发布则静默下载，程序/内核有新版则置角标。多次调用只启一个定时器。
    Q_INVOKABLE void startAutoCheck();

signals:
    void latestVersionChanged();
    void updateAvailableChanged();
    void checkingChanged();
    void coreUpdateAvailableChanged();
    void coreCheckingChanged();
    void checkFailed(const QString &reason); // 供 QML 提示失败原因
    void upToDate();                         // 已是最新（供 QML 弹「已最新」提示）

private:
    void setChecking(bool v);
    void checkAll();    // 程序 + 内核 + GeoIP
    void checkGeoip();  // 按 meta-rules-dat 最新发布时间判断是否有新 mmdb，有则静默下载
    void setCoreUpdateAvailable(bool v);
    void setCoreChecking(bool v);

    AppConfig m_config;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_autoTimer = nullptr;
    QString m_latestVersion;
    bool m_updateAvailable = false;
    bool m_checking = false;
    bool m_coreUpdateAvailable = false;
    bool m_coreChecking = false;
    const QString m_releaseUrl =
        QStringLiteral("https://github.com/ClashrAuto/clashauto/releases/latest");
};
