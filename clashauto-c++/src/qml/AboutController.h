#pragma once

// AboutController —— 关于页专用的小适配器（QObject）。
// 契约（见 qml/ARCHITECTURE.md）：不改后端类、也不改 QmlBridge；这里只把「检查更新」这一
// 关于页独有的动作/状态封装出来，通过 context property `about` 暴露给 QML。
// 逻辑镜像 Widgets 版 MainWindow::checkForUpdate()：GET GitHub releases/latest，比较 tag 与本地版本。
#include <QObject>
#include <QString>

class QNetworkAccessManager;

class AboutController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateAvailableChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)
    Q_PROPERTY(QString releaseUrl READ releaseUrl CONSTANT)

public:
    explicit AboutController(QObject *parent = nullptr);

    QString currentVersion() const;
    QString latestVersion() const { return m_latestVersion; }
    bool updateAvailable() const { return m_updateAvailable; }
    bool checking() const { return m_checking; }
    QString releaseUrl() const { return m_releaseUrl; }

    // 手动检查更新。检查中重复调用会被忽略。
    Q_INVOKABLE void check();

signals:
    void latestVersionChanged();
    void updateAvailableChanged();
    void checkingChanged();
    void checkFailed(const QString &reason); // 供 QML 提示失败原因
    void upToDate();                         // 已是最新（供 QML 弹「已最新」提示）

private:
    void setChecking(bool v);

    QNetworkAccessManager *m_nam = nullptr;
    QString m_latestVersion;
    bool m_updateAvailable = false;
    bool m_checking = false;
    const QString m_releaseUrl =
        QStringLiteral("https://github.com/ClashrAuto/clashauto/releases/latest");
};
