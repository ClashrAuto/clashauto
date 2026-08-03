#include "CoreController.h"

#include "MmdbFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QDateTime>
#include <QThread>
#include <QTimer>

#if defined(Q_OS_UNIX)
#include <csignal>
#include <sys/types.h>
#endif

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

// 取一批输出里的**最后一行**。原先写的是 `output.split(QRegularExpression("[\\r\\n]+")).last()`：
// 每次读都现场编译一个正则、再把整批切成 QStringList，只为取最后一个元素后全部丢掉。
// 核心在 log-level=info 且网关满负载时每秒有几百行，这条路径是热的。改成从尾部找换行，
// 不分配列表、不碰正则。行为不变（同样跳过尾部空行 —— 调用方已 trimmed）。
QString lastLineOf(const QString &output)
{
    for (int i = output.size() - 1; i >= 0; --i) {
        const QChar c = output.at(i);
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r'))
            return output.mid(i + 1);
    }
    return output;
}

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
        kCFAllocatorDefault, CFSTR("Coast"), nullptr, auth);
    if (!prefs) {
        if (err) *err = QObject::tr("SCPreferencesCreateWithAuthorization 返回空");
        return false;
    }
    if (!SCPreferencesLock(prefs, true)) {
        if (err) *err = QObject::tr("SCPreferencesLock 失败（无权限或配置被占用）");
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
        if (!committed && err) *err = QObject::tr("SCPreferencesCommit/Apply 失败");
    } else if (err) {
        *err = QObject::tr("SCNetworkServiceCopyAll 返回空");
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
      m_tunEnabled(m_config.tun),
      m_ipv6Enabled(m_config.ipv6)
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
        if (!output.isEmpty())
            emit logUpdated(lastLineOf(output));
    });
    connect(&m_core, &QProcess::readyReadStandardError, this, [this] {
        const QString output = QString::fromUtf8(m_core.readAllStandardError()).trimmed();
        if (!output.isEmpty())
            emit logUpdated(lastLineOf(output));
    });
    connect(&m_core, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code) {
        stopProxy();
        emit logUpdated(tr("Clash 核心已退出，代码: %1").arg(code));
        emitStatus();

        // ★ 核心**意外**退出时有界自愈地拉起来。
        //
        //   为什么必须做：被代理设备没有"直连"这条路——本机就是它的默认网关，核心一没，
        //   DevicesController 会立刻撤掉劫持让设备掉回真网关（这一步是对的，fail-safe，
        //   真机验过 ARP 秒回真路由器）。但撤完之后**没有任何人再把核心拉起来**：
        //   coast 还活着、每秒刷「内核未运行，本轮不上劫持」，于是网关就此**永久停摆**，
        //   直到有人去界面上手动点一次。而设备主人根本看不到这个界面 —— 他只会静默地
        //   失去代理，且不知道发生过什么。真机实测（负载中 pkill 核心）复现的就是这个状态。
        //
        //   **有界**是关键，别做成崩溃循环：只连续重试 kMaxCoreRestarts 次；只要有一次
        //   活过 kCoreStableMs 就认为稳住了、把计数清零（这样"跑了一天后崩一次"不会因为
        //   历史计数而不救）。超出预算就彻底停手并明确记一条 —— 那时多半是配置或内核本身
        //   坏了，无脑重启只会刷屏并反复扰动设备。
        //
        //   `m_stopRequested` 区分「用户/程序主动停」与「崩了」：主动停一律不重启。
        constexpr int kMaxCoreRestarts = 3;
        constexpr qint64 kCoreStableMs = 60000;  // 活过 1 分钟就算稳住
        constexpr int kCoreRestartDelayMs = 2000; // 别贴着崩溃点立刻重来，给端口/文件句柄让位

        if (m_stopRequested) {
            m_stopRequested = false;
            m_coreRestarts = 0;
            return;
        }
        const qint64 aliveMs = m_coreStartedMs > 0
                                   ? QDateTime::currentMSecsSinceEpoch() - m_coreStartedMs
                                   : 0;
        if (aliveMs >= kCoreStableMs) {
            m_coreRestarts = 0; // 上一次是长期稳定运行后才崩的，预算重新开始
            stopSlowRetry();    // 稳住了就撤掉兜底
        }
        if (m_coreRestarts >= kMaxCoreRestarts) {
            // ★ 快速预算用尽 —— 但**不能就此彻底停手**。
            //
            //   真机实测过停手之后是什么样（连杀核心到预算耗尽，再静置 90 秒）：核心进程 0、
            //   9191 与网关 socks 7899 都不再监听、ARP 劫持被撤回（被代理设备的网关 MAC 秒回
            //   真路由器），而 coast 进程还活着、台账里 9 台设备仍标着「已开代理」。
            //   也就是说：**网关静默死亡，每设备策略（含「禁网」）全部失效，界面上一切照旧。**
            //   本产品的典型形态就是一台没人盯着的网关盒子，指望「用户去界面点一下」等于不修。
            //
            //   所以改成两级：快的那级维持原样（2s × 3，防崩溃循环刷屏、防反复扰动设备），
            //   之后降级为 5 分钟一次的慢速兜底，**无限期**重试。端口被别的进程短暂占用、
            //   内存瞬时不足、订阅拉坏了下次更新又好了 —— 这些都是会自己恢复的故障，慢速
            //   重试能自动救回来；真正坏死的配置也只是每 5 分钟失败一次，既不刷屏也不扰动。
            startSlowRetry();
            return;
        }
        ++m_coreRestarts;
        emit logUpdated(tr("核心异常退出，%1 秒后自动重启（第 %2/%3 次）")
                            .arg(kCoreRestartDelayMs / 1000)
                            .arg(m_coreRestarts)
                            .arg(kMaxCoreRestarts));
        QTimer::singleShot(kCoreRestartDelayMs, this, [this] {
            if (!isRunning())
                startCore(); // 起来之后 DevicesController 的 statusChanged 会把劫持重新上回去
        });
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
        emit logUpdated(tr("创建授权会话失败（AuthorizationCreate=%1）").arg(st));
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
        emit logUpdated(tr("未获得网络配置授权（用户取消或权限不足，AuthorizationCopyRights=%1）").arg(st));
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

// 打包时集成的内核（Windows/Linux 在可执行文件同目录的 core[.exe]，macOS 在 bundle 的
// Contents/Resources/core）首次运行落位到 userDir/command。只做「缺了才补」：**绝不覆盖**
// 已装内核 —— 用户手动升级过（或切了测试版通道）的内核被一次启动静默回滚是最难查的
// 那种坑。集成只是让全新安装开箱即用，之后的版本管理仍归「设置 → 系统」的下载/更新。
// 开发构建旁边没有这个文件，安静跳过，行为与从前一致。
void CoreController::seedBundledCore()
{
    if (QFileInfo::exists(m_config.clashExecutable())) {
        return;
    }
#if defined(Q_OS_WIN)
    const QString coreName = QStringLiteral("core.exe");
#else
    const QString coreName = QStringLiteral("core");
#endif
    QStringList candidates{QDir(QCoreApplication::applicationDirPath()).filePath(coreName)};
#if defined(Q_OS_MACOS)
    candidates << QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("../Resources/core"));
#endif
    QString bundled;
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            bundled = candidate;
            break;
        }
    }
    if (bundled.isEmpty()) {
        return;
    }
    const QString target = QDir(m_config.userDir).filePath(QStringLiteral("command/") + coreName);
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!QFile::copy(bundled, target)) {
        emit logUpdated(tr("集成内核落位失败: %1 -> %2").arg(bundled, target));
        return;
    }
    // /opt/coast 下的源文件是 root 属主、对用户只读；副本必须补回属主写（应用内更新要
    // 覆盖它，否则以后每次更新都静默失败）+ 执行位（缺了核心起不来，报错只有「启动失败」）。
    QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner | QFileDevice::ReadUser
                                      | QFileDevice::WriteUser | QFileDevice::ExeUser
                                      | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                      | QFileDevice::ReadOther | QFileDevice::ExeOther);
    emit logUpdated(tr("已启用打包集成的内核: %1").arg(target));
}

void CoreController::startCore()
{
    if (isRunning()) {
        return;
    }
    // 记下这次拉起的时刻：QProcess::finished 里据此判断「这次是不是稳定跑了一阵才崩的」，
    // 是的话就把自动重启的预算清零（见那段注释）。
    m_coreStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_stopRequested = false;
    m_userStopped = false; // 有人（用户或兜底）要它跑起来，解除「保持停着」的意愿

    // 集成内核先落位再取 clashExecutable()：它的「扁平优先、回退旧路径」判定依赖文件是否存在
    seedBundledCore();

    const QString exe = m_config.clashExecutable();
    m_fullConfigPath = m_configBuilder.ensureFullConfig(m_tunEnabled, m_ipv6Enabled);
    const QString cfg = m_fullConfigPath.isEmpty() ? m_config.clashConfig() : m_fullConfigPath;
    if (!QFileInfo::exists(exe)) {
        // 包里未集成内核（开发构建/--no-core）且用户也没下载过：提示去「设置 → 系统」下载，而不是静默失败
        emit logUpdated(tr("未检测到 mihomo 内核，请在「设置 → 系统」中下载: %1").arg(exe));
        emit coreMissing(exe);
        emitStatus();
        return;
    }
    if (!QFileInfo::exists(cfg)) {
        emit logUpdated(tr("找不到 Clash 配置: %1").arg(cfg));
        emitStatus();
        return;
    }

    // 固定核心家目录为 userDir（-d）：Country.mmdb/cache 等都从这里读，
    // 与设置页「更新 GeoIP」的落盘路径一致（否则老核心默认 ~/.config/clash）。
    const QString mmdb = QDir(m_config.userDir).filePath("Country.mmdb");
    const QString bundledMmdb = QStringLiteral(":/assets/bundle/config/Country.mmdb"); // 内嵌种子
    if (!QFileInfo::exists(mmdb) && QFileInfo::exists(bundledMmdb)) {
        QFile::copy(bundledMmdb, mmdb);
        AppConfig::makeWritable(mmdb); // qrc 种子只读；「更新 GeoIP」要覆盖它
        emit logUpdated(tr("Country.mmdb 已就位: %1").arg(mmdb));
    }
    // 下载来的新 GeoIP 库只被暂存成 Country.mmdb.new（下载侧绝不原地覆盖，见 MmdbFile.h）。
    // **这里是唯一能安全换上去的时机**：核心还没起来，文件也就还没被它 mmap 住。
    // 换之前 applyStaged 会再校验一遍，坏的直接丢弃 —— 宁可继续用旧库，也不让核心带着一份
    // 「打得开但查什么都查不到」的 GeoIP 跑（那会让 GEOIP,CN 静默失配、国内流量集体出海）。
    if (MmdbFile::applyStaged(mmdb)) {
        AppConfig::makeWritable(mmdb);
        emit logUpdated(tr("已启用新下载的 GeoIP 数据库: %1").arg(mmdb));
    }
    // 线上库自愈。上面那两步只保证「以后不会再写坏」，救不了**已经**坏在用户目录里的那份：
    // 旧的下载路径原地覆盖时写坏过一次，而且当场把 geoip/lastPublished 记成了最新版本，于是
    // AboutController::checkGeoip() 之后每次都在 `last == stamp && haveLocal` 处直接返回 ——
    // 不重下、不报错，坏库会一直用到上游发下一个 release。这类坏文件核心能正常 Load、不 fatal，
    // 只是查什么都返回空：GEOIP,CN 是 MATCH 前最后一条规则，它静默失配就等于所有嗅探不出域名的
    // 裸 IP 目的地集体出海。所以每次起核心前给线上那份做一次体检（几 MB、一次启动一次）。
    QString mmdbWhy;
    if (QFileInfo::exists(mmdb) && !MmdbFile::validateFile(mmdb, &mmdbWhy)) {
        emit logUpdated(tr("GeoIP 数据库不可用，已退回内置种子 — %1").arg(mmdbWhy));
        QFile::remove(mmdb);
        if (QFileInfo::exists(bundledMmdb) && QFile::copy(bundledMmdb, mmdb)) {
            AppConfig::makeWritable(mmdb);
        }
        // 清掉版本戳：否则 checkGeoip() 认为本地已是最新，永远不会去下真正的新库，
        // 用户就被钉死在内置种子上了。清掉之后下次启动会重新下载 + 走 stage/applyStaged。
        QSettings().remove(QStringLiteral("geoip/lastPublished"));
    }

#if defined(Q_OS_WIN)
    // TUN 依赖 wintun.dll：从 bundle 按架构复制到核心 exe 同目录（DLL 搜索首选路径）
    const QString wintunTo = QDir(QFileInfo(exe).absolutePath()).filePath("wintun.dll");
    if (!QFileInfo::exists(wintunTo)) {
        const QString cpu = QSysInfo::currentCpuArchitecture();
        const QString archDir = cpu.contains("arm") ? (cpu.contains("64") ? "arm64" : "arm")
                                                    : (cpu.contains("64") ? "x64" : "x86");
        const QString wintunFrom = QStringLiteral(":/assets/bundle/wintun/%1/wintun.dll").arg(archDir);
        if (QFileInfo::exists(wintunFrom) && QFile::copy(wintunFrom, wintunTo)) {
            emit logUpdated(tr("wintun.dll 已部署: %1").arg(wintunTo));
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
            emit logUpdated(tr("核心已由特权 helper 以 root 启动（支持 TUN）"));
            if (m_proxyEnabled) {
                startProxy();
            }
            emitStatus();
            return;
        }
        // 经 helper 起失败：不再静默退回——讲清楚原因，再以非 root QProcess 兜底（TUN 将不可用）。
        emit logUpdated(tr("经特权 helper 以 root 启动核心失败：%1；回退为非 root 启动，TUN 将不生效").arg(herr));
        if (m_config.gatewayPf) {
            // ★ pf 数据面同样要 root，而它失效时**没有任何外部症状可循**，必须显式说明。
            //   真机验证过这个失效形态：/dev/pf 是 crw------- root:wheel，核心的 darwin redir
            //   实现要打开它做 DIOCNATLOOK 才能还原原始目的地；非 root 时它**不报错、不打日志**，
            //   只是 accept 之后把连接丢掉 —— 被接管设备表现为「TCP 握手成功、随后 http=000」，
            //   核心日志一行都没有，而 pf 侧规则计数照常在涨、状态表也正常。三边看着都对却不通。
            emit logUpdated(tr("网关(pf) 需要 root 才能读 /dev/pf 还原原始目的地——"
                               "核心非 root 时被接管设备将连不上网且无任何报错。"
                               "请在「设置 → 系统」安装并批准免密助手。"));
        }
    } else if (m_tunEnabled || m_config.gatewayPf) {
        // TUN 或 pf 网关开着、但免密 helper 还没启用（未安装/待批准）：核心只能非 root 冷启动。
        // 以前这里是**静默**回退（用户只看到「增强灯亮着却不全局」，无从查起）——明确记一条，
        // 把「去设置装并批准免密助手」这条出路讲清楚。
        // 两种失效的**症状完全不同**，所以分开措辞，别让用户拿着 TUN 的说明去查网关问题：
        //   · TUN：建不了 utun，表现为「增强灯亮着却不全局」；
        //   · pf 网关：读不了 /dev/pf，核心 accept 后静默丢连接，被接管设备「握手成功但 http=000」，
        //     且**核心日志一行都没有**、pf 规则计数还照常在涨（真机踩过，极难定位）。
        if (m_tunEnabled) {
            emit logUpdated(tr("增强(TUN) 已开启，但免密 helper 未启用——核心将以非 root 启动、TUN 不会生效。"
                               "请在「设置 → 系统」安装并批准免密助手后重开增强。"));
        }
        if (m_config.gatewayPf) {
            emit logUpdated(tr("网关(pf) 已启用，但免密 helper 未启用——核心将以非 root 启动，"
                               "读不了 /dev/pf 还原原始目的地，被接管设备会连不上网且无任何报错。"
                               "请在「设置 → 系统」安装并批准免密助手。"));
        }
    }
#endif

    m_core.setProgram(exe);
    // 仅传 -d/-f：stock mihomo 无 -token 参数（原 Clashr 定制核心才有），传了会「flag provided
    // but not defined: -token」→ 打印用法并以退出码 2 结束，导致核心起不来。
    m_core.setArguments({"-d", m_config.userDir, "-f", cfg});
    m_core.setWorkingDirectory(QFileInfo(exe).absolutePath());
    reapOrphanCore(); // ★ 先收掉上一次崩溃遗留的核心，否则新核心绑不上端口（见该函数说明）
    m_core.start();
    if (!m_core.waitForStarted(3000)) {
        emit logUpdated(tr("启动 Clash 核心失败: %1").arg(m_core.errorString()));
    } else {
        writeCorePid(qint64(m_core.processId()));
        emit logUpdated("Start clash is OK!");
        if (m_proxyEnabled) {
            startProxy();
        }
    }
    emitStatus();
}

// 记下我们拉起的核心 pid。崩溃后下一次启动靠它把遗留进程收掉（见 reapOrphanCore）。
void CoreController::writeCorePid(qint64 pid) const
{
    QFile f(QDir(m_config.userDir).filePath("core.pid"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray::number(pid));
}

// ★ 收掉上一次遗留的核心 —— 不做这件事，**app 崩过一次之后被代理设备会彻底断网**。
//
// 链条是这样的（真机一步步验出来的，不是推测）：
//   1. app 被 SIGKILL（崩溃/强杀）时来不及停核心，而核心是它的子进程 → 变成孤儿被 init 收养，
//      **继续活着并占着 9191/7890/7899/1053**（Linux 上实测 pid 存活、`ss` 里端口仍归它）；
//   2. 用户重新打开 app，我们起一个新核心；mihomo 绑不上端口时**并不退出** ——
//      它每个 listener 记一条 "address already in use" 然后照常运行；
//   3. 于是 `QProcess` 状态是 Running、`isRunning()` 为真、劫持照常上，
//      而这个新核心**一个端口都没监听**：设备的流量进了 lwIP，出站拨 SOCKS 无人应答。
//   实测就是 **100% 失败**（31911 条全是 read 错误：连接被接下、随即断），
//   而且孤儿死掉之后新核心也不会回头重试绑定，坏到重启为止。
//
// 判据必须严：只收「pid 文件里记着的那个」且「确实是我们这份核心可执行文件」的进程 ——
// pid 会被系统复用，光按 pid 杀可能误伤无辜进程。Linux/macOS 用 /proc 或 ps 核对可执行路径。
void CoreController::reapOrphanCore()
{
    QFile f(QDir(m_config.userDir).filePath("core.pid"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const qint64 pid = f.readAll().trimmed().toLongLong();
    f.close();
    if (pid <= 0)
        return;

#if defined(Q_OS_UNIX)
    // 核对可执行路径：pid 复用时不能误杀。/proc 没有（macOS）就退回 `ps -p <pid> -o comm=`。
    const QString exe = QFileInfo(m_config.clashExecutable()).canonicalFilePath();
    QString actual = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).canonicalFilePath();
    if (actual.isEmpty()) {
        QProcess ps;
        ps.start("ps", {"-p", QString::number(pid), "-o", "comm="});
        ps.waitForFinished(1000);
        const QString comm = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
        if (comm.isEmpty() || !exe.endsWith(comm))
            return; // 进程不在，或不是我们的核心
    } else if (!exe.isEmpty() && actual != exe) {
        return; // pid 被别人复用了
    }
    emit logUpdated(tr("发现上次遗留的内核进程 %1，先收掉再启动").arg(pid));
    ::kill(pid_t(pid), SIGKILL);
    // 等它真的消失再往下走，否则新核心照样绑不上端口。
    for (int i = 0; i < 20 && ::kill(pid_t(pid), 0) == 0; ++i)
        QThread::msleep(50);
#elif defined(Q_OS_WIN)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, DWORD(pid));
    if (!h)
        return; // 进程已经不在
    wchar_t buf[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    const bool gotPath = QueryFullProcessImageNameW(h, 0, buf, &n) != 0;
    const QString actual = gotPath ? QFileInfo(QString::fromWCharArray(buf, int(n))).canonicalFilePath()
                                   : QString();
    const QString exe = QFileInfo(m_config.clashExecutable()).canonicalFilePath();
    if (!actual.isEmpty() && !exe.isEmpty() && actual != exe) {
        CloseHandle(h); // pid 被复用，不是我们的核心
        return;
    }
    emit logUpdated(tr("发现上次遗留的内核进程 %1，先收掉再启动").arg(pid));
    TerminateProcess(h, 1);
    WaitForSingleObject(h, 1000);
    CloseHandle(h);
#endif
}

// 兜底重试：5 分钟一次，不设次数上限。只在「核心不在跑」且「不是用户主动停的」时才拉。
void CoreController::startSlowRetry()
{
    if (!m_slowRetryTimer) {
        m_slowRetryTimer = new QTimer(this);
        m_slowRetryTimer->setInterval(5 * 60 * 1000);
        connect(m_slowRetryTimer, &QTimer::timeout, this, [this] {
            // 已经在跑（自己救回来了，或用户手动起了）→ 兜底功成身退，别每 5 分钟空转一次。
            // 之后若再崩，快速预算会重新走一遍，需要时会重新 startSlowRetry()。
            if (isRunning()) {
                stopSlowRetry();
                return;
            }
            if (m_userStopped)
                return;
            // 不打日志刷屏：真坏死的配置会每 5 分钟走一次这里，进入 startCore 后失败自会记一条。
            startCore();
        });
    }
    if (!m_slowRetryTimer->isActive()) {
        m_slowRetryTimer->start();
        emit logUpdated(tr("核心连续 %1 次异常退出，快速重启已停止；转为每 5 分钟重试一次")
                            .arg(m_coreRestarts));
    }
}

void CoreController::stopSlowRetry()
{
    if (m_slowRetryTimer && m_slowRetryTimer->isActive())
        m_slowRetryTimer->stop();
}

void CoreController::stopCore()
{
    // 主动停：finished 槽据此**不**触发自动重启（否则用户点"停止"会被我们又拉起来）。
    // ★ m_stopRequested 在 finished 槽里会被立刻清掉（它只表达「这一次退出是主动的」），
    //   所以另用 m_userStopped 记住「用户希望它保持停着」——否则慢速兜底会在 5 分钟后
    //   把用户刚停掉的核心又拉起来。
    m_stopRequested = true;
    m_userStopped = true;
    stopSlowRetry();
    m_coreRestarts = 0;
    stopProxy();
#if defined(Q_OS_MACOS)
    if (m_helperCoreRunning) {
        QString herr;
        MacHelper::stopCore(&herr);
        m_helperCoreRunning = false;
        stopCoreLogTail();
        emit logUpdated(tr("核心已停止（helper）"));
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

void CoreController::setIpv6Enabled(bool enabled)
{
    m_ipv6Enabled = enabled;
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
        m_fullConfigPath = m_configBuilder.ensureFullConfig(m_tunEnabled, m_ipv6Enabled);
    } else {
        m_configBuilder.writeTunEnabled(m_fullConfigPath, m_tunEnabled);
    }
#if defined(Q_OS_MACOS)
    // macOS 不能靠热重载翻 TUN——两条原因叠加，缺一不可：
    //   ① 建/拆 utun + 改默认路由必须 **root**，而 root 只在核心由特权 helper **冷启动**时才有；
    //   ② mihomo 的 PUT /configs 默认**不重载 general/tun 段**（要 ?force=true 才会），所以热重载
    //      改 tun.enable 核心根本不理会。
    // 于是运行中翻开关既没权限建 utun、核心也不重读 tun → 「开了 TUN 却不全局」。这里改为把核心
    // **重启**：停掉后 startCore() 经 helper 以 root 冷启动，读取刚写入的 tun.enable，utun 与全局路由
    // 才真正建立（与 Windows 开 TUN 提权冷重启同理）。helper 没启用时 startCore 会回退非 root 并明确
    // 记「TUN 将不生效」，把真正原因暴露出来而非静默失败。
    emit logUpdated(m_tunEnabled ? tr("已开启增强模式，正在以 root 重启核心以应用 TUN")
                                 : tr("已关闭增强模式，正在重启核心"));
    if (isRunning()) {
        stopCore();
        startCore();
    }
    emitStatus();
    return;
#else
    emit logUpdated(m_tunEnabled ? tr("已开启增强模式，正在重载 TUN 配置") : tr("已关闭增强模式，正在重载 TUN 配置"));
    reloadConfig();
    emitStatus();
#endif
}

void CoreController::rebuildConfig()
{
    m_fullConfigPath = m_configBuilder.ensureFullConfig(m_tunEnabled, m_ipv6Enabled);
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
            emit logUpdated(tr("设置系统代理失败（helper）：%1").arg(herr));
            return;
        }
        m_sysproxyActive = true;
        emit logUpdated("Start sysproxy ok!");
        return;
    }
    // 经 SCPreferences（带一次性授权）设代理：混合端口同时服务 HTTP/HTTPS/SOCKS，三种都指向它
    if (!ensureMacAuthorization()) {
        emit logUpdated(tr("设置系统代理失败：未获得授权"));
        return;
    }
    QString err;
    if (!macApplyProxies(static_cast<AuthorizationRef>(m_macAuthRef), true,
                         m_config.host, m_config.mixedPort, macBypass, &err)) {
        emit logUpdated(tr("设置系统代理失败：%1").arg(err));
        return;
    }
    m_sysproxyActive = true;
    emit logUpdated("Start sysproxy ok!");
#elif defined(Q_OS_WIN)
    // 进程内直调 WinINET，无子进程、无外部二进制
    const QString server = QString("%1:%2").arg(m_config.host).arg(m_config.mixedPort);
    if (!setWinSystemProxy(true, server, QStringLiteral("localhost;127.*;10.*;172.16.*;192.168.*;<local>"))) {
        emit logUpdated(tr("设置系统代理失败（WinINET InternetSetOption）"));
        return;
    }
    m_sysproxyActive = true;
    emit logUpdated("Start sysproxy ok!");
#else
    // Linux：gsettings（GNOME/Cinnamon 系桌面）。混合端口同时服务 HTTP/HTTPS/SOCKS。
    // 非 GNOME 桌面（如 KDE）gsettings 不存在或 schema 缺失时报错并放弃，不再依赖捆绑二进制。
    if (runHidden("gsettings", {"set", "org.gnome.system.proxy", "mode", "manual"}, 10000) != 0) {
        emit logUpdated(tr("设置系统代理失败：gsettings 不可用（非 GNOME 系桌面？）"));
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
    // **只 ping 一次**（见 isReady 的说明）：这条路会在退出时跑，而「注册着但不应答」的 helper
    // 会让默认的 3 次重试把退出卡住 15 秒。开代理那侧仍用默认重试，冷启动要留时间。
    if (MacHelper::isReady(1)) {
        QString herr;
        if (!MacHelper::setSystemProxy(false, m_config.host, m_config.mixedPort, {}, &herr)) {
            emit logUpdated(tr("还原系统代理失败（helper）：%1").arg(herr));
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
            emit logUpdated(tr("还原系统代理失败：%1").arg(err));
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
    // 保留当前正在运行的好配置，避免坏配置覆盖导致核心失效。核心 exe 缺失时跳过校验直接 PUT
    // （别因缺核心反而不重载）。
    //
    // ★ 这一步**必须异步**，别改回 waitForFinished。真机实测（树莓派网关，1528 行 full.yaml）：
    //     mihomo -t 校验 ≈ 970ms（5 次测量 968/969/971/970/969，非常稳定）
    //     真正的 PUT /configs 只要 52ms
    //   校验占了整个热重载的 95%。它原先同步跑在 UI 线程上，而 rebuildConfig() 有 10 处调用点
    //   （设备开关代理、改每设备策略、改设置、订阅变更…都是高频交互），于是**每次操作界面冻结
    //   近 1 秒**。异步化之后 UI 立刻返回，校验在后台跑完再决定要不要 PUT。
    const QString exe = m_config.clashExecutable();
    if (!QFileInfo::exists(exe)) {
        putConfigs(); // 没有核心二进制可用来校验：保持原语义，直接重载
        return;
    }
    if (m_configTest && m_configTest->state() != QProcess::NotRunning) {
        // 上一次校验还在跑。**不要**再起一个：只记下"还欠一次"，等它回来补跑。
        // 合并的是"重载意图"而非配置内容 —— full.yaml 已经被 rebuildConfig 重写过了，
        // 补跑那次读到的就是最新的，所以合并不会丢更新。
        m_reloadPending = true;
        return;
    }
    if (!m_configTest) {
        m_configTest = new QProcess(this);
        connect(m_configTest, &QProcess::finished, this,
                [this](int code, QProcess::ExitStatus) {
                    if (code == 0) {
                        putConfigs();
                    } else {
                        emit logUpdated(tr("配置校验未通过，已跳过热重载（保留当前运行配置）"));
                    }
                    if (m_reloadPending) { // 校验期间又有新的重载请求，补跑一次
                        m_reloadPending = false;
                        reloadConfig();
                    }
                });
    }
    m_configTest->start(exe, {"-t", "-d", m_config.userDir, "-f", m_fullConfigPath});
}

void CoreController::putConfigs()
{
    if (!isRunning() || m_fullConfigPath.isEmpty())
        return;
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
        emit logUpdated(ok ? tr("Clash 配置已重载") : tr("重载 Clash 配置失败: %1 %2").arg(error, QString::fromUtf8(body)));
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
