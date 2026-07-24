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
    // 核心按需下载到「用户可写目录」userDir/command：优先 command/core[.exe]，回退旧
    // command/clash/clash-<os>-<arch>（兼容早期下载到该路径的用户）。app 自包含后不再依赖 Clashr-Auto。
    // mac 尤其必须放 .app 外：签名+公证的 .app 只读封存，往包内写核心会破坏封存，
    // 使 SMAppService 特权 helper 被 Launch Constraint Violation SIGKILL（TUN/增强失效）。
    const bool arm = QSysInfo::currentCpuArchitecture().contains("arm");
#if defined(Q_OS_WIN)
    const QString flat = QDir(userDir).filePath("command/core.exe");
    if (QFile::exists(flat)) return flat;
    return QDir(userDir).filePath(arm ? "command/clash/clash-windows-arm64.exe" : "command/clash/clash-windows-amd64.exe");
#elif defined(Q_OS_MACOS)
    const QString flat = QDir(userDir).filePath("command/core");
    if (QFile::exists(flat)) return flat;
    return QDir(userDir).filePath(arm ? "command/clash/clash-darwin-arm64" : "command/clash/clash-darwin-amd64");
#else
    const QString flat = QDir(userDir).filePath("command/core");
    if (QFile::exists(flat)) return flat;
    return QDir(userDir).filePath(arm ? "command/clash/clash-linux-arm64" : "command/clash/clash-linux-amd64");
#endif
}

QString AppConfig::clashConfig() const
{
    // full.yaml 由 ConfigBuilder 生成到 configDir；核心启动前必先 ensureFullConfig，故总在此。
    return QDir(configDir).filePath("full.yaml");
}

void AppConfig::makeWritable(const QString &path)
{
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ReadUser | QFileDevice::WriteUser
                                    | QFileDevice::ReadGroup | QFileDevice::ReadOther);
}

AppConfig AppConfigLoader::load()
{
    AppConfig config;
    // 资源已内嵌(qrc :/assets/bundle/*)：config 种子从 qrc 落地，核心/wintun/mmdb 运行时写到 userDir。
    // 不再探测同级 Clashr-Auto 目录 —— app 完全自包含。
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    // 数据根 userDir = <AppData 根>/Coast（Win: %AppData%\Coast）：核心家目录(-d)，放 logs\、Country.mmdb、cache。
    // AppDataLocation 因 org==app=="Coast" 得 <根>/Coast/Coast，上跳一级回到品牌根 <根>/Coast。
    // 配置 yaml（config.yaml/full.yaml 等）单独放 configDir = userDir/config；cleanPath 为纯词法运算。
    config.userDir = QDir::cleanPath(appData + QStringLiteral("/.."));
    config.configDir = QDir(config.userDir).filePath(QStringLiteral("config"));
    QDir().mkpath(config.configDir); // 一并创建父级 userDir

    const QString bundledConfig = QStringLiteral(":/assets/bundle/config/config.yaml"); // 内嵌种子，不再依赖 Clashr-Auto
    const QString userConfig = QDir(config.configDir).filePath("config.yaml");
    if (!QFile::exists(userConfig) && QFile::exists(bundledConfig)) {
        QFile::copy(bundledConfig, userConfig);
        AppConfig::makeWritable(userConfig); // qrc 种子默认只读，先补写权限（下方 mini 归一化需写回）
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
    config.autoLanguage = boolFromYaml(yaml, "autoLanguage", config.autoLanguage);
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
