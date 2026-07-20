#pragma once

#include "AppConfig.h"
#include "ConfigBuilder.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>

class QTimer;

class CoreController final : public QObject
{
    Q_OBJECT

public:
    explicit CoreController(AppConfig config, QObject *parent = nullptr);
    ~CoreController() override;

    bool isRunning() const;
    bool isProxyEnabled() const;
    bool isTunEnabled() const;
    bool isCoreInstalled() const; // 内核二进制是否已就位（不再预装内核，需用户从设置下载）
    bool isHelperCore() const;    // macOS：当前核心是否由特权 helper（root）启动（决定 TUN 是否可用）

    // 设置 TUN 标志但不重载（用于核心尚未启动时预置状态，例如提权重启后带 TUN 冷启动）
    void setTunEnabled(bool enabled);
    // 立即硬杀核心并还原系统代理（提权重启时用，避免旧核心占用 9090 与新实例冲突）
    void killCoreNow();
    // 修改 REST API 端口（设置页「应用」时）：更新配置并让下次 full.yaml 用新端口写 external-controller。
    // 端口变更需重启核心才能重新 bind（热重载改不了），调用方负责随后 stop/startCore。
    void setUiPort(int port);
    int uiPort() const { return m_config.uiPort; }

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
    void reloadConfig();
    void emitStatus();

#if defined(Q_OS_MACOS)
    // 懒创建并预授权一个 system.services.systemconfiguration.network 权限的 AuthorizationRef，
    // 整个进程生命周期复用：设/清系统代理经 SCPreferences 提交时最多首次弹一次密码。
    // （仅作 helper 不可用时的 fallback；helper 就绪时设代理走 root helper，全程免密。）
    bool ensureMacAuthorization();
    void startCoreLogTail(); // helper 起核心后 tail userDir/logs/core.log，转成 logUpdated
    void stopCoreLogTail();
    void pollCoreLog();
#endif

    AppConfig m_config;
    ConfigBuilder m_configBuilder;
    QNetworkAccessManager m_network;
    QProcess m_core;
    QString m_fullConfigPath;
    bool m_proxyEnabled = false;
    bool m_tunEnabled = false;
    bool m_sysproxyActive = false; // 本会话是否真的应用过系统代理：stopProxy 据此跳过无谓的还原动作
#if defined(Q_OS_MACOS)
    void *m_macAuthRef = nullptr;      // 实为 AuthorizationRef；用 void* 避免在头文件引入 Security 头
    bool m_helperCoreRunning = false;  // 核心是否由特权 helper（root）启动（TUN 依赖此）
    QTimer *m_coreLogTimer = nullptr;  // tail core.log 的定时器（helper 拥有核心时的日志来源）
    QString m_coreLogPath;
    qint64 m_coreLogPos = 0;
#endif
};
