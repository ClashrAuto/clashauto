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
        m_core.terminate();
        if (!m_core.waitForFinished(2500)) {
            m_core.kill();
        }
    }
    emitStatus();
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
    if (!ensureSysproxy()) {
        return;
    }

    const QString sysproxy = m_config.sysproxyExecutable();
    runHidden(sysproxy, {"global", QString("%1:%2").arg(m_config.host).arg(m_config.mixedPort), "localhost;127.*;10.*;172.16.*;192.168.*;<local>"});
    emit logUpdated("Start sysproxy ok!");
}

void CoreController::stopProxy()
{
    const QString sysproxy = m_config.sysproxyExecutable();
    if (QFileInfo::exists(sysproxy)) {
        runHidden(sysproxy, {"set", "1"});
        emit logUpdated("Stop sysproxy ok!");
    }
}

bool CoreController::ensureSysproxy()
{
    const QString sysproxy = m_config.sysproxyExecutable();
    if (QFileInfo::exists(sysproxy)) {
        return true;
    }

    const QString archive = QDir(m_config.sourceRoot).filePath("command/sysproxy/sysproxy.zip");
    const QString outputDir = QDir(m_config.sourceRoot).filePath("command/sysproxy");
    if (!QFileInfo::exists(archive)) {
        emit logUpdated(QString("sysproxy 不存在，且找不到压缩包: %1").arg(archive));
        return false;
    }

#if defined(Q_OS_WIN)
    QString escapedArchive = archive;
    QString escapedOutputDir = outputDir;
    escapedArchive.replace("'", "''");
    escapedOutputDir.replace("'", "''");
    const int code = runHidden("powershell", {
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-Command",
        QString("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
            .arg(escapedArchive, escapedOutputDir)
    });
#else
    const int code = runHidden("unzip", {"-o", archive, "-d", outputDir});
#endif

    if (code != 0 || !QFileInfo::exists(sysproxy)) {
        emit logUpdated(QString("解压 sysproxy 失败: %1").arg(archive));
        return false;
    }

    emit logUpdated(QString("sysproxy 已解压: %1").arg(sysproxy));
    return true;
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
    emit statusChanged(m_tunEnabled, m_proxyEnabled, isRunning());
}
