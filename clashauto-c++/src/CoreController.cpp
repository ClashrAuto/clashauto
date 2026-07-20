#include "CoreController.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTimer>

#if defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wininet.h> // InternetSetOption：原生设置系统(WinINET)代理，不再依赖捆绑的 sysproxy.exe
#endif

#if defined(Q_OS_MACOS)
// 系统代理改经 SystemConfiguration 框架（带 Authorization 授权）直接写入，替代逐服务多次
// 起 networksetup 子进程：既省去每次 fork/exec，也把授权收敛成整会话最多弹一次密码。
#include <Security/Authorization.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include "MacHelperClient.h" // 特权 helper：就绪时代理/核心/TUN 都走它（root，免密），否则回退本文件内实现
#endif

namespace {
#if defined(Q_OS_WIN)
constexpr unsigned kCreateNoWindow = 0x08000000; // CREATE_NO_WINDOW
#endif
// 同步执行子进程且不弹控制台窗口（GUI 子系统下 QProcess::execute 会给控制台子进程新开窗口）
int runHidden(const QString &program, const QStringList &args, int timeoutMs = 30000)
{
    QProcess p;
#if defined(Q_OS_WIN)
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= kCreateNoWindow;
    });
#endif
    p.start(program, args);
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        return -1;
    }
    return p.exitCode();
}

#if defined(Q_OS_WIN)
// Windows 原生系统代理：WinINET 每连接选项（即 IE/系统代理），与此前捆绑的 sysproxy.exe
// 所做的完全等价，但免掉外部二进制与子进程。设置后广播 SETTINGS_CHANGED/REFRESH 即时生效。
bool setWinSystemProxy(bool enable, const QString &server, const QString &bypass)
{
    std::wstring serverW = server.toStdWString();
    std::wstring bypassW = bypass.toStdWString();
    INTERNET_PER_CONN_OPTIONW options[3]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[0].Value.dwValue = enable ? (PROXY_TYPE_PROXY | PROXY_TYPE_DIRECT) : PROXY_TYPE_DIRECT;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[1].Value.pszValue = serverW.data();
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[2].Value.pszValue = bypassW.data();

    INTERNET_PER_CONN_OPTION_LISTW list{};
    list.dwSize = sizeof(list);
    list.pszConnection = nullptr; // 默认（LAN）连接
    list.dwOptionCount = enable ? 3 : 1; // 关闭时只写 FLAGS=DIRECT
    list.pOptions = options;
    const BOOL ok = InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof(list));
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    return ok == TRUE;
}
#endif

#if defined(Q_OS_MACOS)
// 往 CFMutableDictionary 写入一个整数值（键为 SC 框架导出的 CFStringRef 常量）
void cfDictSetInt(CFMutableDictionaryRef d, CFStringRef key, int v)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

// 往 CFMutableDictionary 写入一个字符串值
void cfDictSetStr(CFMutableDictionaryRef d, CFStringRef key, const QString &v)
{
    CFStringRef s = v.toCFString(); // 调用方持有，需 CFRelease
    CFDictionarySetValue(d, key, s);
    CFRelease(s);
}

// 取某网络服务现有的 Proxies 字典并做可变拷贝（保留 FTP/PAC 等我们不管的键，避免误清用户配置）；
// 不存在则新建空表。返回值归调用方所有，用完 CFRelease。
CFMutableDictionaryRef macCopyProxiesDict(SCPreferencesRef prefs, CFStringRef serviceID)
{
    CFStringRef path = CFStringCreateWithFormat(kCFAllocatorDefault, nullptr,
                                                CFSTR("/NetworkServices/%@/Proxies"), serviceID);
    CFDictionaryRef existing = (CFDictionaryRef)SCPreferencesPathGetValue(prefs, path); // Get 规则，不释放
    CFRelease(path);
    if (existing && CFGetTypeID(existing) == CFDictionaryGetTypeID()) {
        return CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, existing);
    }
    return CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

// 经带授权的 SCPreferences，在所有「已启用」网络服务上设置(enable)或清除(disable) HTTP/HTTPS/SOCKS
// 代理。一次 Lock→逐服务改 Proxies 字典→Commit→Apply，取代旧的逐服务四次 networksetup 子进程。
// disable 时只翻 *Enable 位、保留 host/port，方便下次秒开，也不动其它代理键。成功返回 true，
// 失败原因写入 *err。
bool macApplyProxies(AuthorizationRef auth, bool enable, const QString &host, int port,
                     const QStringList &bypass, QString *err)
{
    SCPreferencesRef prefs = SCPreferencesCreateWithAuthorization(
        kCFAllocatorDefault, CFSTR("ClashAuto"), nullptr, auth);
    if (!prefs) {
        if (err) *err = QStringLiteral("SCPreferencesCreateWithAuthorization 返回空");
        return false;
    }
    if (!SCPreferencesLock(prefs, true)) {
        if (err) *err = QStringLiteral("SCPreferencesLock 失败（无权限或配置被占用）");
        CFRelease(prefs);
        return false;
    }

    bool committed = false;
    CFArrayRef services = SCNetworkServiceCopyAll(prefs);
    if (services) {
        CFMutableArrayRef exceptions = nullptr;
        if (enable) {
            exceptions = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
            for (const QString &b : bypass) {
                CFStringRef s = b.toCFString();
                CFArrayAppendValue(exceptions, s);
                CFRelease(s);
            }
        }
        const CFIndex count = CFArrayGetCount(services);
        for (CFIndex i = 0; i < count; ++i) {
            SCNetworkServiceRef svc = (SCNetworkServiceRef)CFArrayGetValueAtIndex(services, i);
            if (!SCNetworkServiceGetEnabled(svc)) continue; // 跳过已停用的服务（等价旧代码跳过 * 前缀）
            CFStringRef sid = SCNetworkServiceGetServiceID(svc);
            if (!sid) continue;

            CFMutableDictionaryRef proxies = macCopyProxiesDict(prefs, sid);
            if (enable) {
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPEnable, 1);
                cfDictSetStr(proxies, kSCPropNetProxiesHTTPProxy, host);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPPort, port);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPSEnable, 1);
                cfDictSetStr(proxies, kSCPropNetProxiesHTTPSProxy, host);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPSPort, port);
                cfDictSetInt(proxies, kSCPropNetProxiesSOCKSEnable, 1);
                cfDictSetStr(proxies, kSCPropNetProxiesSOCKSProxy, host);
                cfDictSetInt(proxies, kSCPropNetProxiesSOCKSPort, port);
                if (exceptions) CFDictionarySetValue(proxies, kSCPropNetProxiesExceptionsList, exceptions);
            } else {
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPEnable, 0);
                cfDictSetInt(proxies, kSCPropNetProxiesHTTPSEnable, 0);
                cfDictSetInt(proxies, kSCPropNetProxiesSOCKSEnable, 0);
            }
            CFStringRef path = CFStringCreateWithFormat(kCFAllocatorDefault, nullptr,
                                                        CFSTR("/NetworkServices/%@/Proxies"), sid);
            SCPreferencesPathSetValue(prefs, path, proxies);
            CFRelease(path);
            CFRelease(proxies);
        }
        if (exceptions) CFRelease(exceptions);
        CFRelease(services);

        committed = SCPreferencesCommitChanges(prefs) && SCPreferencesApplyChanges(prefs);
        if (!committed && err) *err = QStringLiteral("SCPreferencesCommit/Apply 失败");
    } else if (err) {
        *err = QStringLiteral("SCNetworkServiceCopyAll 返回空");
    }

    SCPreferencesUnlock(prefs);
    CFRelease(prefs);
    return committed;
}
#endif
}

CoreController::CoreController(AppConfig config, QObject *parent)
    : QObject(parent),
      m_config(std::move(config)),
      m_configBuilder(m_config),
      m_proxyEnabled(m_config.webProxy),
      m_tunEnabled(m_config.tun)
{
#if defined(Q_OS_WIN)
    // 核心为控制台程序，GUI 子系统下启动会新开控制台窗口——用 CREATE_NO_WINDOW 隐藏
    m_core.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= kCreateNoWindow;
    });
#endif
    // mihomo 的 stdout/stderr 是 UTF-8：必须用 fromUtf8，否则中文 Windows 会按 GBK 解码成乱码
    connect(&m_core, &QProcess::readyReadStandardOutput, this, [this] {
        const QString output = QString::fromUtf8(m_core.readAllStandardOutput()).trimmed();
        if (!output.isEmpty()) {
            emit logUpdated(output.split(QRegularExpression("[\\r\\n]+")).last());
        }
    });
    connect(&m_core, &QProcess::readyReadStandardError, this, [this] {
        const QString output = QString::fromUtf8(m_core.readAllStandardError()).trimmed();
        if (!output.isEmpty()) {
            emit logUpdated(output.split(QRegularExpression("[\\r\\n]+")).last());
        }
    });
    connect(&m_core, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code) {
        stopProxy();
        emit logUpdated(QString("Clash 核心已退出，代码: %1").arg(code));
        emitStatus();
    });
}

CoreController::~CoreController()
{
#if defined(Q_OS_MACOS)
    if (m_macAuthRef) {
        // 仅释放本进程的授权引用，不 DestroyRights（进程退出即失效，无需全局吊销）
        AuthorizationFree(static_cast<AuthorizationRef>(m_macAuthRef), kAuthorizationFlagDefaults);
        m_macAuthRef = nullptr;
    }
#endif
}

#if defined(Q_OS_MACOS)
bool CoreController::ensureMacAuthorization()
{
    if (m_macAuthRef) {
        return true; // 本会话已授权，复用
    }
    AuthorizationRef authRef = nullptr;
    OSStatus st = AuthorizationCreate(nullptr, kAuthorizationEmptyEnvironment,
                                      kAuthorizationFlagDefaults, &authRef);
    if (st != errAuthorizationSuccess) {
        emit logUpdated(QString::fromUtf8("创建授权会话失败（AuthorizationCreate=%1）").arg(st));
        return false;
    }
    // 预授权修改网络配置的权限：弹一次密码框；成功后授权缓存在 authRef 上，
    // 后续 SCPreferencesCommit 在权限有效期内不再弹（这正是替代 networksetup 的目的）。
    AuthorizationItem item = { "system.services.systemconfiguration.network", 0, nullptr, 0 };
    AuthorizationRights rights = { 1, &item };
    AuthorizationFlags flags = kAuthorizationFlagDefaults
                             | kAuthorizationFlagInteractionAllowed
                             | kAuthorizationFlagPreAuthorize
                             | kAuthorizationFlagExtendRights;
    st = AuthorizationCopyRights(authRef, &rights, kAuthorizationEmptyEnvironment, flags, nullptr);
    if (st != errAuthorizationSuccess) {
        // 用户取消或权限不足：放弃，不再退回逐次弹窗的 networksetup
        AuthorizationFree(authRef, kAuthorizationFlagDefaults);
        emit logUpdated(QString::fromUtf8("未获得网络配置授权（用户取消或权限不足，AuthorizationCopyRights=%1）").arg(st));
        return false;
    }
    m_macAuthRef = authRef;
    return true;
}
#endif

bool CoreController::isRunning() const
{
#if defined(Q_OS_MACOS)
    if (m_helperCoreRunning) return true; // 核心由 helper 拥有时 m_core 处于 NotRunning
#endif
    return m_core.state() != QProcess::NotRunning;
}

bool CoreController::isHelperCore() const
{
#if defined(Q_OS_MACOS)
    return m_helperCoreRunning;
#else
    return false;
#endif
}

bool CoreController::isProxyEnabled() const
{
    return m_proxyEnabled;
}

bool CoreController::isTunEnabled() const
{
    return m_tunEnabled;
}

bool CoreController::isCoreInstalled() const
{
    return QFileInfo::exists(m_config.clashExecutable());
}

void CoreController::startCore()
{
    if (isRunning()) {
        return;
    }

    const QString exe = m_config.clashExecutable();
    m_fullConfigPath = m_configBuilder.ensureFullConfig(m_tunEnabled);
    const QString cfg = m_fullConfigPath.isEmpty() ? m_config.clashConfig() : m_fullConfigPath;
    if (!QFileInfo::exists(exe)) {
        // 不再预装内核：未找到时提示用户去「设置 → 系统」下载，而不是静默失败
        emit logUpdated(QString::fromUtf8("未检测到 mihomo 内核，请在「设置 → 系统」中下载: %1").arg(exe));
        emit coreMissing(exe);
        emitStatus();
        return;
    }
    if (!QFileInfo::exists(cfg)) {
        emit logUpdated(QString("找不到 Clash 配置: %1").arg(cfg));
        emitStatus();
        return;
    }

    // 固定核心家目录为 userDir（-d）：Country.mmdb/cache 等都从这里读，
    // 与设置页「更新 GeoIP」的落盘路径一致（否则老核心默认 ~/.config/clash）。
    const QString mmdb = QDir(m_config.userDir).filePath("Country.mmdb");
    const QString bundledMmdb = QDir(m_config.sourceRoot).filePath("config/Country.mmdb");
    if (!QFileInfo::exists(mmdb) && QFileInfo::exists(bundledMmdb)) {
        QFile::copy(bundledMmdb, mmdb);
        emit logUpdated(QString("Country.mmdb 已就位: %1").arg(mmdb));
    }

#if defined(Q_OS_WIN)
    // TUN 依赖 wintun.dll：从 bundle 按架构复制到核心 exe 同目录（DLL 搜索首选路径）
    const QString wintunTo = QDir(QFileInfo(exe).absolutePath()).filePath("wintun.dll");
    if (!QFileInfo::exists(wintunTo)) {
        const QString cpu = QSysInfo::currentCpuArchitecture();
        const QString archDir = cpu.contains("arm") ? (cpu.contains("64") ? "arm64" : "arm")
                                                    : (cpu.contains("64") ? "x64" : "x86");
        const QString wintunFrom = QDir(m_config.sourceRoot).filePath(QString("command/wintun/bin/%1/wintun.dll").arg(archDir));
        if (QFileInfo::exists(wintunFrom) && QFile::copy(wintunFrom, wintunTo)) {
            emit logUpdated(QString("wintun.dll 已部署: %1").arg(wintunTo));
        }
    }
#endif

#if defined(Q_OS_MACOS)
    // helper 已注册启用：以 root 起核心（这样 TUN/增强模式才能建 utun、改路由）。
    // 判定用 status()==Enabled——与 MainWindow 开启增强、状态栏显示的判定一致；不要再靠 isReady()
    // 里那次额外的 getVersion ping 做门槛：冷启动的 daemon 首个 XPC 偶发慢/超时，会让「helper 明明
    // 装了、增强也开了、核心却仍非 root」。这里直接尝试经 helper（真正的 startCore XPC，15s 超时，
    // 也会顺带把 daemon 拉起）；真失败才回退普通 QProcess，且把原因讲清楚（回退后 TUN 不生效）。
    if (MacHelper::status() == MacHelper::RegStatus::Enabled) {
        QString herr;
        if (MacHelper::startCore(exe, cfg, m_config.userDir, &herr)) {
            m_helperCoreRunning = true;
            startCoreLogTail();
            emit logUpdated(QString::fromUtf8("核心已由特权 helper 以 root 启动（支持 TUN）"));
            if (m_proxyEnabled) {
                startProxy();
            }
            emitStatus();
            return;
        }
        // 经 helper 起失败：不再静默退回——讲清楚原因，再以非 root QProcess 兜底（TUN 将不可用）。
        emit logUpdated(QString::fromUtf8("经特权 helper 以 root 启动核心失败：%1；回退为非 root 启动，TUN 将不生效").arg(herr));
    }
#endif

    m_core.setProgram(exe);
    // 仅传 -d/-f：stock mihomo 无 -token 参数（原 Clashr 定制核心才有），传了会「flag provided
    // but not defined: -token」→ 打印用法并以退出码 2 结束，导致核心起不来。
    m_core.setArguments({"-d", m_config.userDir, "-f", cfg});
    m_core.setWorkingDirectory(QFileInfo(exe).absolutePath());
    m_core.start();
    if (!m_core.waitForStarted(3000)) {
        emit logUpdated(QString("启动 Clash 核心失败: %1").arg(m_core.errorString()));
    } else {
        emit logUpdated("Start clash is OK!");
        if (m_proxyEnabled) {
            startProxy();
        }
    }
    emitStatus();
}

void CoreController::stopCore()
{
    stopProxy();
#if defined(Q_OS_MACOS)
    if (m_helperCoreRunning) {
        QString herr;
        MacHelper::stopCore(&herr);
        m_helperCoreRunning = false;
        stopCoreLogTail();
        emit logUpdated(QString::fromUtf8("核心已停止（helper）"));
        emitStatus();
        return;
    }
#endif
    if (isRunning()) {
#if defined(Q_OS_WIN)
        // mihomo 是无窗口控制台程序，terminate() 的 WM_CLOSE 对它无效（同 killCoreNow）——
        // 之前每次停核心/退出程序都必然白等满 2.5s 超时才 kill，这就是「退出要很久」的主因。
        // 直接硬杀：提权重启流程用 killCoreNow 硬杀多年无副作用（wintun 网卡随进程消失）。
        m_core.kill();
        m_core.waitForFinished(1500);
#else
        m_core.terminate(); // POSIX 下是 SIGTERM，mihomo 能优雅退出，留短暂宽限
        if (!m_core.waitForFinished(2500)) {
            m_core.kill();
        }
#endif
    }
    emitStatus();
}

void CoreController::setUiPort(int port)
{
    if (port <= 0 || port == m_config.uiPort) {
        return;
    }
    m_config.uiPort = port;
    // ConfigBuilder 持有 AppConfig 副本，用新端口重建它；下次 ensureFullConfig 会把
    // external-controller 写成 host:新端口。CoreController 自身的 /configs 调用也走新端口。
    m_configBuilder = ConfigBuilder(m_config);
}

void CoreController::setTunEnabled(bool enabled)
{
    // 仅置位，不重载：核心还没起时用它预置 TUN，随后 startCore() 会按此写入 full.yaml
    m_tunEnabled = enabled;
}

void CoreController::killCoreNow()
{
    // 硬杀：mihomo 是无窗口控制台程序，terminate() 的 WM_CLOSE 对它无效，只能 kill()。
    // 提权重启时用它立刻释放 9090 与系统代理，避免与新（提权）实例的核心抢端口。
    stopProxy();
#if defined(Q_OS_MACOS)
    if (m_helperCoreRunning) {
        QString herr;
        MacHelper::stopCore(&herr);
        m_helperCoreRunning = false;
        stopCoreLogTail();
        emitStatus();
        return;
    }
#endif
    if (isRunning()) {
        m_core.kill();
        m_core.waitForFinished(1500);
    }
    emitStatus();
}

void CoreController::toggleCore()
{
    isRunning() ? stopCore() : startCore();
}

void CoreController::toggleProxy()
{
    m_proxyEnabled = !m_proxyEnabled;
    if (m_proxyEnabled) {
        startProxy();
    } else {
        stopProxy();
    }
    emitStatus();
}

void CoreController::toggleTun()
{
    m_tunEnabled = !m_tunEnabled;
    if (m_fullConfigPath.isEmpty()) {
        m_fullConfigPath = m_configBuilder.ensureFullConfig(m_tunEnabled);
    } else {
        m_configBuilder.writeTunEnabled(m_fullConfigPath, m_tunEnabled);
    }
    emit logUpdated(m_tunEnabled ? "已开启增强模式，正在重载 TUN 配置" : "已关闭增强模式，正在重载 TUN 配置");
    reloadConfig();
    emitStatus();
}

void CoreController::rebuildConfig()
{
    m_fullConfigPath = m_configBuilder.ensureFullConfig(m_tunEnabled);
    emit logUpdated(QString("Config generated: %1").arg(m_fullConfigPath));
    reloadConfig();
}

void CoreController::startProxy()
{
#if defined(Q_OS_MACOS)
    const QStringList macBypass = {"localhost", "127.0.0.1", "10.0.0.0/8", "172.16.0.0/12",
                                   "192.168.0.0/16", "*.local"};
    // helper 就绪：以 root 设代理，全程免密（首选）。否则回退 Option B（进程内 SCPreferences + 一次性授权）。
    if (MacHelper::isReady()) {
        QString herr;
        if (!MacHelper::setSystemProxy(true, m_config.host, m_config.mixedPort, macBypass, &herr)) {
            emit logUpdated(QString::fromUtf8("设置系统代理失败（helper）：%1").arg(herr));
            return;
        }
        m_sysproxyActive = true;
        emit logUpdated("Start sysproxy ok!");
        return;
    }
    // 经 SCPreferences（带一次性授权）设代理：混合端口同时服务 HTTP/HTTPS/SOCKS，三种都指向它
    if (!ensureMacAuthorization()) {
        emit logUpdated(QString::fromUtf8("设置系统代理失败：未获得授权"));
        return;
    }
    QString err;
    if (!macApplyProxies(static_cast<AuthorizationRef>(m_macAuthRef), true,
                         m_config.host, m_config.mixedPort, macBypass, &err)) {
        emit logUpdated(QString::fromUtf8("设置系统代理失败：%1").arg(err));
        return;
    }
    m_sysproxyActive = true;
    emit logUpdated("Start sysproxy ok!");
#elif defined(Q_OS_WIN)
    // 进程内直调 WinINET，无子进程、无外部二进制
    const QString server = QString("%1:%2").arg(m_config.host).arg(m_config.mixedPort);
    if (!setWinSystemProxy(true, server, QStringLiteral("localhost;127.*;10.*;172.16.*;192.168.*;<local>"))) {
        emit logUpdated(QString::fromUtf8("设置系统代理失败（WinINET InternetSetOption）"));
        return;
    }
    m_sysproxyActive = true;
    emit logUpdated("Start sysproxy ok!");
#else
    // Linux：gsettings（GNOME/Cinnamon 系桌面）。混合端口同时服务 HTTP/HTTPS/SOCKS。
    // 非 GNOME 桌面（如 KDE）gsettings 不存在或 schema 缺失时报错并放弃，不再依赖捆绑二进制。
    if (runHidden("gsettings", {"set", "org.gnome.system.proxy", "mode", "manual"}, 10000) != 0) {
        emit logUpdated(QString::fromUtf8("设置系统代理失败：gsettings 不可用（非 GNOME 系桌面？）"));
        return;
    }
    const QString port = QString::number(m_config.mixedPort);
    for (const char *schema : {"org.gnome.system.proxy.http", "org.gnome.system.proxy.https", "org.gnome.system.proxy.socks"}) {
        runHidden("gsettings", {"set", QString::fromLatin1(schema), "host", m_config.host}, 10000);
        runHidden("gsettings", {"set", QString::fromLatin1(schema), "port", port}, 10000);
    }
    runHidden("gsettings", {"set", "org.gnome.system.proxy", "ignore-hosts",
                            "['localhost', '127.0.0.0/8', '::1', '10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16']"}, 10000);
    m_sysproxyActive = true;
    emit logUpdated("Start sysproxy ok!");
#endif
}

void CoreController::stopProxy()
{
    // 本会话没开过系统代理就直接跳过：退出时 stopCore 与核心 finished 信号会各调一次
    // stopProxy，之前每次都同步起 sysproxy 子进程（VM 里进程创建很慢），拖慢退出。
    if (!m_sysproxyActive) {
        return;
    }
#if defined(Q_OS_MACOS)
    // helper 就绪：以 root 清代理（免密）。否则回退 Option B（复用本会话已持有的授权）。
    if (MacHelper::isReady()) {
        QString herr;
        if (!MacHelper::setSystemProxy(false, m_config.host, m_config.mixedPort, {}, &herr)) {
            emit logUpdated(QString::fromUtf8("还原系统代理失败（helper）：%1").arg(herr));
        }
        m_sysproxyActive = false;
        emit logUpdated("Stop sysproxy ok!");
        return;
    }
    // 复用本会话已持有的授权（开代理时已弹过一次），关代理不再弹密码
    if (m_macAuthRef) {
        QString err;
        if (!macApplyProxies(static_cast<AuthorizationRef>(m_macAuthRef), false,
                             m_config.host, m_config.mixedPort, {}, &err)) {
            emit logUpdated(QString::fromUtf8("还原系统代理失败：%1").arg(err));
        }
    }
    m_sysproxyActive = false;
    emit logUpdated("Stop sysproxy ok!");
#elif defined(Q_OS_WIN)
    setWinSystemProxy(false, QString(), QString()); // FLAGS=DIRECT 即还原直连
    m_sysproxyActive = false;
    emit logUpdated("Stop sysproxy ok!");
#else
    runHidden("gsettings", {"set", "org.gnome.system.proxy", "mode", "none"}, 10000);
    m_sysproxyActive = false;
    emit logUpdated("Stop sysproxy ok!");
#endif
}

void CoreController::reloadConfig()
{
    if (!isRunning() || m_fullConfigPath.isEmpty()) {
        return;
    }

    // 安全加固：热重载前用核心自身的测试模式（mihomo -t）校验待加载配置。校验不过就不 PUT，
    // 保留当前正在运行的好配置，避免坏配置覆盖导致核心失效。核心 exe 缺失时跳过校验直接按原逻辑走
    // （别因缺核心反而不重载）。这是同步的短进程调用，reloadConfig 在 UI 线程被调用且频率低（设置/规则变更），可接受。
    const QString exe = m_config.clashExecutable();
    if (QFileInfo::exists(exe)) {
        const int rc = runHidden(exe, {"-t", "-d", m_config.userDir, "-f", m_fullConfigPath});
        if (rc != 0) {
            emit logUpdated(QString::fromUtf8("配置校验未通过，已跳过热重载（保留当前运行配置）"));
            return;
        }
    }

    QJsonObject payload;
    payload.insert("path", m_fullConfigPath);
    QNetworkRequest request(QUrl(QString("http://%1:%2/configs").arg(m_config.host).arg(m_config.uiPort)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // external-controller 设了 secret 后，本 PUT 也必须带 Bearer，否则热重载 401（与 ClashService 同）
    if (!m_config.secret.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + m_config.secret.toUtf8());
    }
    QNetworkReply *reply = m_network.put(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString error = reply->errorString();
        reply->deleteLater();
        emit logUpdated(ok ? "Clash 配置已重载" : QString("重载 Clash 配置失败: %1 %2").arg(error, QString::fromUtf8(body)));
    });
}

void CoreController::emitStatus()
{
    // 核心没在跑时，TUN/系统代理实际都不生效（stopCore 已还原系统代理、TUN 网卡随核心退出消失）。
    // 故灯以「核心在跑 且 该开关开启」为准（对齐旧项目：core close 时 this.tun/this.web 置 false），
    // 否则停核心后 增强/网页 灯仍亮着，与实际不符。m_tunEnabled/m_proxyEnabled 仍保留「意图」供开关判定。
    const bool running = isRunning();
    emit statusChanged(running && m_tunEnabled, running && m_proxyEnabled, running);
}

#if defined(Q_OS_MACOS)
void CoreController::startCoreLogTail()
{
    // helper 每次 startCore 会重建 core.log（截断到 0），故从头 tail
    m_coreLogPath = QDir(m_config.userDir).filePath("logs/core.log");
    m_coreLogPos = 0;
    if (!m_coreLogTimer) {
        m_coreLogTimer = new QTimer(this);
        m_coreLogTimer->setInterval(500);
        connect(m_coreLogTimer, &QTimer::timeout, this, &CoreController::pollCoreLog);
    }
    m_coreLogTimer->start();
}

void CoreController::stopCoreLogTail()
{
    if (m_coreLogTimer) {
        m_coreLogTimer->stop();
    }
    m_coreLogPath.clear();
    m_coreLogPos = 0;
}

void CoreController::pollCoreLog()
{
    if (m_coreLogPath.isEmpty()) {
        return;
    }
    QFile f(m_coreLogPath);
    if (!f.open(QIODevice::ReadOnly)) {
        return; // core.log 还没被 helper 建出来
    }
    const qint64 size = f.size();
    if (size < m_coreLogPos) {
        m_coreLogPos = 0; // 文件被重建（新一轮启动）
    }
    if (size == m_coreLogPos) {
        return;
    }
    f.seek(m_coreLogPos);
    const QByteArray chunk = f.readAll();
    m_coreLogPos = f.pos();
    // mihomo 输出为 UTF-8；按行发（顺带修掉进程内路径「每段只发最后一行」的截断）
    const QStringList lines =
        QString::fromUtf8(chunk).split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        emit logUpdated(line);
    }
}
#endif
