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
    bool isCoreInstalled() const; // 内核二进制是否已就位（不再预装内核，需用户从设置下载）

    // 设置 TUN 标志但不重载（用于核心尚未启动时预置状态，例如提权重启后带 TUN 冷启动）
    void setTunEnabled(bool enabled);
    // 立即硬杀核心并还原系统代理（提权重启时用，避免旧核心占用 9090 与新实例冲突）
    void killCoreNow();
    // 杀掉系统里残留/孤儿的同名核心进程（崩溃、强杀、更新内核重启会遗留）。
    // startCore 会自动调用；更新内核替换二进制前也需调用，否则孤儿核心占着 exe 文件句柄导致替换失败。
    void killOrphanCores();

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
    void coreMissing(QString path); // 启动时未找到内核二进制 → UI 引导用户去设置下载

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
