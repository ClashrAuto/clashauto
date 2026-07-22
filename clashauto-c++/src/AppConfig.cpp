#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

QString AppConfig::clashExecutable() const
{
#if defined(Q_OS_WIN)
    return QDir(sourceRoot).filePath(QSysInfo::currentCpuArchitecture().contains("arm") ? "command/clash/clash-windows-arm64.exe" : "command/clash/clash-windows-amd64.exe");
#elif defined(Q_OS_MACOS)
    // core 必须放在「.app 包外、且用户可写」的位置：签名+公证后的 .app 是只读封存的，往包内
    // （Contents/Clashr-Auto/command/clash）下载 core 会破坏 app 的代码签名封存（codesign --verify
    // 报 "a sealed resource is missing or invalid / file added"）。而 SMAppService 特权 helper 启动时
    // 系统会校验其宿主 app 的签名——app 封存一破，daemon 即被 Launch Constraint Violation 用 SIGKILL 杀掉，
    // 核心永远无法以 root 运行（TUN/增强失效）。故 core 落到用户目录 userDir，与包内只读资源分离。
    return QDir(userDir).filePath(QSysInfo::currentCpuArchitecture().contains("arm") ? "command/clash/clash-darwin-arm64" : "command/clash/clash-darwin-amd64");
#else
    return QDir(sourceRoot).filePath(QSysInfo::currentCpuArchitecture().contains("arm") ? "command/clash/clash-linux-arm64" : "command/clash/clash-linux-amd64");
#endif
}

QString AppConfig::clashConfig() const
{
    const QString userFull = QDir(userDir).filePath("full.yaml");
    if (QFile::exists(userFull)) {
        return userFull;
    }
    return QDir(sourceRoot).filePath("config/full.yaml");
}

AppConfig AppConfigLoader::load()
{
    AppConfig config;
    QDir probe(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 8; ++i) {
        const QString candidate = probe.filePath("../Clashr-Auto");
        if (QFile::exists(QDir(candidate).filePath("config/config.yaml"))) {
            config.sourceRoot = QDir(candidate).canonicalPath();
            break;
        }
        if (!probe.cdUp()) {
            break;
        }
    }
    if (config.sourceRoot.isEmpty()) {
        config.sourceRoot = QDir(QCoreApplication::applicationDirPath()).filePath("../Clashr-Auto");
    }

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    config.userDir = QDir(appData).filePath("clash-auto");
    QDir().mkpath(config.userDir);

    const QString bundledConfig = QDir(config.sourceRoot).filePath("config/config.yaml");
    const QString userConfig = QDir(config.userDir).filePath("config.yaml");
    if (!QFile::exists(userConfig) && QFile::exists(bundledConfig)) {
        QFile::copy(bundledConfig, userConfig);
        // 首次落地用户配置时把「关闭到托盘」(mini) 归一到本 App 默认「关」。bundle 配置源自 Electron 版
        // 发布（固定写 mini: true），但 C++ 版默认关闭到托盘为关（新用户默认正常显示窗口 + ✕ 退出，
        // 与设置页开关的未勾选态一致）。仅首次 seed 时处理，用户之后在设置里的改动照常保存、不再被覆盖。
        QFile seed(userConfig);
        if (seed.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString c = QString::fromUtf8(seed.readAll());
            seed.close(); // 先关读句柄，避免与下面的写在 Windows 上争用
            static const QRegularExpression miniLine(QStringLiteral("(?m)^[ \\t]*mini[ \\t]*:.*$"));
            if (miniLine.match(c).hasMatch())
                c.replace(miniLine, QStringLiteral("mini: false"));
            else
                c += (c.isEmpty() || c.endsWith('\n') ? QString() : QStringLiteral("\n")) + QStringLiteral("mini: false\n");
            if (seed.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                seed.write(c.toUtf8());
                seed.close();
            }
        }
    }

    QFile file(QFile::exists(userConfig) ? userConfig : bundledConfig);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return config;
    }
    const QString yaml = QString::fromUtf8(file.readAll());
    config.host = valueFromYaml(yaml, "host", config.host);
    config.uiPort = intFromYaml(yaml, "ui", config.uiPort);
    config.mixedPort = intFromYaml(yaml, "port", config.mixedPort);
    config.webProxy = boolFromYaml(yaml, "web", config.webProxy);
    config.tun = boolFromYaml(yaml, "use", config.tun);
    config.nodeOnlyAvailable = boolFromYaml(yaml, "node", config.nodeOnlyAvailable);
    config.clearConnections = boolFromYaml(yaml, "clearConnections", config.clearConnections);
    config.increment = boolFromYaml(yaml, "increment", config.increment);
    config.closeToTray = boolFromYaml(yaml, "mini", config.closeToTray);
    config.autoStart = boolFromYaml(yaml, "sys", config.autoStart);
    config.nodeSwitchNote = boolFromYaml(yaml, "note", config.nodeSwitchNote);
    config.autoUpdateMinutes = intFromYaml(yaml, "autoUpdate", config.autoUpdateMinutes);
    config.allowRule = nestedValueFromYaml(yaml, "use_rule", "allow", config.allowRule);
    config.noAllowRule = nestedValueFromYaml(yaml, "use_rule", "noallow", config.noAllowRule);
    config.allowRuleEnabled = nestedBoolFromYaml(yaml, "use_rule", "allowUse", false);
    config.noAllowRuleEnabled = nestedBoolFromYaml(yaml, "use_rule", "noallowUse", false);
    config.theme = valueFromYaml(yaml, "theme", config.theme);
    config.autoTheme = boolFromYaml(yaml, "autoTheme", config.autoTheme);
    config.mirror = boolFromYaml(yaml, "mirror", config.mirror);
    config.language = valueFromYaml(yaml, "language", config.language);
    config.secret = valueFromYaml(yaml, "secret", QString());

    // 安全加固：external-controller 未设访问密钥时，首次加载随机生成 32 位十六进制 secret 并落盘，
    // 之后固定复用。仅写入「用户可写」的 userConfig（bundled 只读，不改）。
    if (config.secret.isEmpty()) {
        QString generated;
        generated.reserve(32);
        for (int i = 0; i < 16; ++i) {
            generated += QStringLiteral("%1").arg(QRandomGenerator::global()->bounded(256), 2, 16, QLatin1Char('0'));
        }
        config.secret = generated;
        if (QFile::exists(userConfig)) {
            file.close(); // 释放读句柄，避免与下面的追加写在 Windows 上争用
            QFile out(userConfig);
            if (out.open(QIODevice::Append)) {
                const QString line = (yaml.isEmpty() || yaml.endsWith('\n'))
                    ? QStringLiteral("secret: %1\n").arg(config.secret)
                    : QStringLiteral("\nsecret: %1\n").arg(config.secret);
                out.write(line.toUtf8());
                out.close();
            }
        }
    }
    return config;
}

QString AppConfigLoader::valueFromYaml(const QString &text, const QString &key, const QString &fallback)
{
    const QRegularExpression re(QStringLiteral("(?m)^%1:\\s*['\"]?([^'\"\\r\\n#]+)").arg(QRegularExpression::escape(key)));
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch()) {
        return fallback;
    }
    return match.captured(1).trimmed();
}

bool AppConfigLoader::boolFromYaml(const QString &text, const QString &key, bool fallback)
{
    const QString value = valueFromYaml(text, key, fallback ? "true" : "false").toLower();
    if (value == "true" || value == "yes" || value == "1") {
        return true;
    }
    if (value == "false" || value == "no" || value == "0") {
        return false;
    }
    return fallback;
}

QString AppConfigLoader::nestedValueFromYaml(const QString &text, const QString &section, const QString &key, const QString &fallback)
{
    const QRegularExpression sectionRe(QStringLiteral("(?m)^%1:\\n((?:  [^\\n]*\\n?)*)").arg(QRegularExpression::escape(section)));
    const QRegularExpressionMatch sectionMatch = sectionRe.match(text);
    if (!sectionMatch.hasMatch()) {
        return fallback;
    }
    const QString block = sectionMatch.captured(1);
    const QRegularExpression keyRe(QStringLiteral("(?m)^  %1:\\s*['\"]?([^'\"\\r\\n#]*)").arg(QRegularExpression::escape(key)));
    const QRegularExpressionMatch keyMatch = keyRe.match(block);
    if (!keyMatch.hasMatch()) {
        return fallback;
    }
    return keyMatch.captured(1).trimmed();
}

bool AppConfigLoader::nestedBoolFromYaml(const QString &text, const QString &section, const QString &key, bool fallback)
{
    const QString value = nestedValueFromYaml(text, section, key, fallback ? "true" : "false").toLower();
    if (value == "true" || value == "yes" || value == "1") {
        return true;
    }
    if (value == "false" || value == "no" || value == "0") {
        return false;
    }
    return fallback;
}

int AppConfigLoader::intFromYaml(const QString &text, const QString &key, int fallback)
{
    bool ok = false;
    const int value = valueFromYaml(text, key, QString::number(fallback)).toInt(&ok);
    return ok ? value : fallback;
}
