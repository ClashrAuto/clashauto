#pragma once

#include "AppConfig.h"
#include "ConfigBuilder.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>

class CoreController final : public QObject
{
    Q_OBJECT

public:
    explicit CoreController(AppConfig config, QObject *parent = nullptr);

    bool isRunning() const;
    bool isProxyEnabled() const;
    bool isTunEnabled() const;

public slots:
    void startCore();
    void stopCore();
    void toggleCore();
    void toggleProxy();
    void toggleTun();
    void rebuildConfig();

signals:
    void statusChanged(bool tun, bool proxy, bool core);
    void logUpdated(QString message);

private:
    void startProxy();
    void stopProxy();
    bool ensureSysproxy();
    void reloadConfig();
    void emitStatus();

    AppConfig m_config;
    ConfigBuilder m_configBuilder;
    QNetworkAccessManager m_network;
    QProcess m_core;
    QString m_fullConfigPath;
    bool m_proxyEnabled = false;
    bool m_tunEnabled = false;
};
