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
// mac 没有 sysproxy 二进制：系统代理用自带的 networksetup 设置。
// 这里列出所有「已启用」的网络服务（Wi-Fi、Ethernet 等），逐个设/清代理。
QStringList macNetworkServices()
{
    QProcess p;
    p.start("networksetup", {"-listallnetworkservices"});
    if (!p.waitForFinished(10000)) {
        p.kill();
        return {};
    }
    QStringList services;
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n');
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        // 首行是提示文字（An asterisk (*)...）；带 * 前缀的是已停用的服务，跳过
        if (line.isEmpty() || line.startsWith(QLatin1Char('*')) || line.contains(QLatin1String("asterisk"))) {
            continue;
        }
        services << line;
    }
    return services;
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

bool CoreController::isRunning() const
{
    return m_core.state() != QProcess::NotRunning;
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
    // mac 用系统自带 networksetup（运行用户需属 admin 组，个人 Mac 默认如此）：
    // 混合端口同时服务 HTTP/HTTPS/SOCKS，三种代理都指向它
    const QStringList services = macNetworkServices();
    if (services.isEmpty()) {
        emit logUpdated(QString::fromUtf8("设置系统代理失败：networksetup 未列出可用网络服务"));
        return;
    }
    const QString port = QString::number(m_config.mixedPort);
    for (const QString &svc : services) {
        runHidden("networksetup", {"-setwebproxy", svc, m_config.host, port}, 10000);
        runHidden("networksetup", {"-setsecurewebproxy", svc, m_config.host, port}, 10000);
        runHidden("networksetup", {"-setsocksfirewallproxy", svc, m_config.host, port}, 10000);
        runHidden("networksetup", {"-setproxybypassdomains", svc,
                                   "localhost", "127.0.0.1", "10.0.0.0/8", "172.16.0.0/12",
                                   "192.168.0.0/16", "*.local"}, 10000);
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
    for (const QString &svc : macNetworkServices()) {
        runHidden("networksetup", {"-setwebproxystate", svc, "off"}, 10000);
        runHidden("networksetup", {"-setsecurewebproxystate", svc, "off"}, 10000);
        runHidden("networksetup", {"-setsocksfirewallproxystate", svc, "off"}, 10000);
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

    QJsonObject payload;
    payload.insert("path", m_fullConfigPath);
    QNetworkRequest request(QUrl(QString("http://%1:%2/configs").arg(m_config.host).arg(m_config.uiPort)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
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
