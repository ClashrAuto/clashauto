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

    /// 打包集成的内核落位（缺了才补，绝不覆盖已装的）。
    ///
    /// ★ 必须在 isCoreInstalled() 之前调用一次，否则全新安装**开不了机**：
    /// autoStartCore() 一上来就用 isCoreInstalled() 挡门，而它查的是
    /// userDir/command/core —— 全新安装那里是空的，集成内核躺在可执行文件同目录。
    /// 于是早退 → startCore() 不执行 → 原先只写在 startCore() 里的落位也不执行 →
    /// 内核永远不落位。用户装完打开，界面起来了，什么也没发生，
    /// 而一个好端端的内核就在旁边。2026-08-07 在树莓派上用正式发布包实测到：
    /// 全新安装启动 30s 后既没有 command/ 目录、也没有 full.yaml、更没有核心进程。
    void seedBundledCore(); // 幂等；重复调用无副作用

    /// 集成内核落位自测（`COAST_BUNDLEDCORE_SELFTEST=1`，见 main_qml.cpp）。
    static bool runBundledCoreSelfTest();
    bool isHelperCore() const;    // macOS：当前核心是否由特权 helper（root）启动（决定 TUN 是否可用）

    // 设置 TUN 标志但不重载（用于核心尚未启动时预置状态，例如提权重启后带 TUN 冷启动）
    void setTunEnabled(bool enabled);
    // 立即硬杀核心并还原系统代理（提权重启时用，避免旧核心占用 9090 与新实例冲突）
    void killCoreNow();
    // 修改 REST API 端口（设置页「应用」时）：更新配置并让下次 full.yaml 用新端口写 external-controller。
    // 端口变更需重启核心才能重新 bind（热重载改不了），调用方负责随后 stop/startCore。
    void setUiPort(int port);
    int uiPort() const { return m_config.uiPort; }
    /// 网关数据面是否走 TPROXY（见 AppConfig::gatewayTproxy）。DevicesController 据此配置 LanGateway。
    bool gatewayTproxy() const { return m_config.gatewayTproxy; }
    /// macOS 的 pf 数据面是否启用（见 AppConfig::gatewayPf）。
    bool gatewayPf() const { return m_config.gatewayPf; }

    /// 每网卡出口表（见 ConfigBuilder::NicEgress）。DevicesController 每轮扫描后推进来。
    /// **只存不重建** —— 扫描每 ~5s 一轮，每轮都热重载一次是不可接受的；调用方发现集合真的
    /// 变了才调 rebuildConfig()（与 v6 前缀那条同一个套路）。
    void setGatewayNics(const QVector<ConfigBuilder::NicEgress> &nics);

    // 设置 IPv6 开关。只改内存里的运行态，不自己触发重建 —— 调用方（SettingsController）
    // 落完 config.yaml 后统一调 rebuildConfig()，避免一次开关引发两次热重载。
    void setIpv6Enabled(bool enabled);
    bool isIpv6Enabled() const { return m_ipv6Enabled; }

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
    /// 启动自愈：上一次会话被强杀/崩溃时来不及还原系统代理，会把整机流量指向一个
    /// **已经没人监听**的本机端口 —— 用户表现为"什么都打不开"，且完全无从得知原因。
    /// 与 TproxyRules::removeStale() 同一模式：进程起来后先把上一世的残留擦干净。
    void clearStaleSystemProxy();
    void reloadConfig();
    void putConfigs(); // reloadConfig 的第二段：校验通过后真正 PUT /configs（异步回调里调）

    // 热重载前的 `mihomo -t` 预校验进程。**必须异步**：真机实测这一步在树莓派网关上稳定
    // 耗时 ~970ms，而整个热重载里真正的 PUT /configs 只要 52ms —— 校验占了 95%，且原先是
    // waitForFinished 同步跑在 UI 线程上，于是每次改设备策略/规则/设置都冻结界面近 1 秒。
    QProcess *m_configTest = nullptr;
    // 校验期间又来了新的重载请求 → 只置位，等这次校验回来再补跑一次，避免并发起多个校验进程
    // （rebuildConfig 有 10 处调用点，且 resumeProxies 的设备循环体内也会调，天然会连发）。
    bool m_reloadPending = false;
    void emitStatus();
    void writeCorePid(qint64 pid) const; // 记下自己拉起的核心 pid，供下次启动收孤儿
    void reapOrphanCore();               // 收掉上次崩溃遗留的核心（否则新核心绑不上端口）

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
    // ★ 单独存一份：m_configBuilder 在改端口时会被**整个重建**（见 setUiPort），
    //   只调 setEgressNics 的话那次重建会把网卡表清空，随后生成的 full.yaml 悄悄退回单出口。
    QVector<ConfigBuilder::NicEgress> m_gatewayNics;
    QNetworkAccessManager m_network;
    QProcess m_core;
    QString m_fullConfigPath;
    bool m_proxyEnabled = false;
    bool m_tunEnabled = false;
    bool m_ipv6Enabled = false;
    bool m_sysproxyActive = false; // 本会话是否真的应用过系统代理：stopProxy 据此跳过无谓的还原动作
    // —— 核心意外退出后的有界自愈（见 .cpp 里 QProcess::finished 那段）——
    bool m_stopRequested = false;  // 是不是我们主动停的：主动停不重启，崩溃才重启
    bool m_userStopped = false;    // 用户希望核心**保持**停着（m_stopRequested 只管一次退出）
    int m_coreRestarts = 0;        // 本轮连续自动重启次数，起来一阵子就清零
    qint64 m_coreStartedMs = 0;    // 上次拉起核心的时刻，用来判断「这次算不算稳住了」
    // 快速重试预算用尽后的**慢速兜底重试**。见 .cpp 里那段：快速预算耗尽就彻底停手的话，
    // 这台机器（多半是没人盯着的网关盒子）会静默停摆到有人去点界面为止，而设备主人只会
    // 莫名其妙地失去代理与每设备策略。慢速重试让「端口被占/ 内存瞬时不足 / 订阅暂时坏了」
    // 这类**会自己好**的故障能自动恢复，代价是每 5 分钟一次失败的拉起。
    QTimer *m_slowRetryTimer = nullptr;
    void startSlowRetry();  // 快速预算耗尽后转入 5 分钟一次的无限期兜底重试
    void stopSlowRetry();   // 核心稳住 / 用户主动停 时撤掉兜底
#if defined(Q_OS_MACOS)
    const void *m_macAuthRef = nullptr; // 实为 AuthorizationRef(=const AuthorizationOpaqueRef*)；const void* 避免引 Security 头且不丢 const
    bool m_helperCoreRunning = false;  // 核心是否由特权 helper（root）启动（TUN 依赖此）
    QTimer *m_coreLogTimer = nullptr;  // tail core.log 的定时器（helper 拥有核心时的日志来源）
    QString m_coreLogPath;
    qint64 m_coreLogPos = 0;
#endif
};
