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

    // 本机进程内入站（MixedInbound）的**实际监听端口**；0 = 没在跑。
    //
    // ★ 由 DevicesController 在 listen() **成功之后**才调用，停的时候传 0。
    //   为什么不直接读 config 的 coastcore_inbound：那只是「意图」。若端口被占、或 coastcore 总开关
    //   关着（入站根本不会起），照配置去设系统代理就会把本机流量指到一个没人监听的端口上——
    //   **用户本机直接断网**。以「确实 listen 成功」为唯一依据，这个脚枪就不存在。
    //   系统代理正开着时调用会**就地重设**到新端口（切换无需重启）。
    void setLocalInboundPort(int port);

    // 「全部切换到进程内」的端口换位（由 DevicesController 编排）：进程内入站占走 mixedPort 时，
    // 核心的 mixed-port 挪到 port（= mixedPort+1，只作回退出口）；0 = 还原 mixedPort。
    // 只改**下一次** ensureFullConfig 的产物（含 startCore 冷启动那次）——调用方负责在核心
    // 正在跑时随后 rebuildConfig()（热重载换绑）或重启核心。系统代理不受影响：它指向的
    // proxyPort()（mixedPort）在换位前后不变。
    void setCoreMixedPort(int port);
    int coreMixedPort() const { return m_coreMixedPort > 0 ? m_coreMixedPort : m_config.mixedPort; }

    // 预生成 full.yaml **并预跑一次 `mihomo -t` 校验**，但不重载。
    // ★ 存在的唯一理由是压缩端口换手窗口：切回时 7890 从「我们放手」到「核心接手」之间无人监听，
    //   而 reloadConfig 里那次同步的校验子进程（几百 ms）恰好落在这个窗口里。先在入站**还在
    //   服务**的时候把生成+校验做完，紧随其后的 rebuildConfig 就只剩一个 PUT。
    void prewarmConfig();

    // 系统代理机制自检（COAST_SYSPROXY_SELFTEST=1，见 main_qml.cpp）。**不建 GUI、不起核心。**
    //
    // ★ 为什么单独做一个：把系统代理改指进程内入站这条路，只有在**真实桌面会话**里才验得了
    //   （Windows 走 WinINET、Linux 走 gsettings），CI 和无头机都跑不了。而它一旦出错的后果是
    //   「本机上不了网」，让用户拿 GUI 去试代价太大。这个钩子读当前值 → 设成测试值 → 读回核对
    //   → **还原原值**，几秒钟就能知道机制在这台机器上到底通不通。
    static bool systemProxySelfTest();
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

    // force=true → PUT /configs?force=true。mihomo 默认**不重载 general 段**（端口、tun 等都在里面），
    // 所以只有 force 才换得动 mixed-port —— 端口换位必须用它，否则核心一直听在旧端口上
    // （真机实测：非 force 重载后新端口起不来，只能重启核心兜底，代价是断掉全部在途连接）。
    void reloadConfig(bool force = false);
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
    int m_localInboundPort = 0;    // 进程内入站实际在听的端口（0=没跑），见 setLocalInboundPort
    int m_coreMixedPort = 0;       // >0 = 核心 mixed-port 的覆盖值（端口换位时 = mixedPort+1），见 setCoreMixedPort
    bool m_forceNextReload = false; // 下一次 rebuildConfig 用 force 重载（端口刚变过，见 setCoreMixedPort）
    QString m_prevalidatedPath;     // prewarmConfig 已校验通过的 full.yaml 路径（reloadConfig 据此跳过重复校验）
    // 系统代理该指向哪个端口：进程内入站在跑就用它（本机流量走进程内引擎），否则用 mihomo 的混合端口。
    int proxyPort() const { return m_localInboundPort > 0 ? m_localInboundPort : m_config.mixedPort; }
#if defined(Q_OS_MACOS)
    const void *m_macAuthRef = nullptr; // 实为 AuthorizationRef(=const AuthorizationOpaqueRef*)；const void* 避免引 Security 头且不丢 const
    bool m_helperCoreRunning = false;  // 核心是否由特权 helper（root）启动（TUN 依赖此）
    QTimer *m_coreLogTimer = nullptr;  // tail core.log 的定时器（helper 拥有核心时的日志来源）
    QString m_coreLogPath;
    qint64 m_coreLogPos = 0;
#endif
};
