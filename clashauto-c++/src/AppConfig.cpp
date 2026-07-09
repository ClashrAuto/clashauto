#include "AppConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextStream>

QString AppConfig::clashExecutable() const
{
#if defined(Q_OS_WIN)
    return QDir(sourceRoot).filePath("command/clash/clash-windows-amd64.exe");
#elif defined(Q_OS_MACOS)
    return QDir(sourceRoot).filePath(QSysInfo::currentCpuArchitecture().contains("arm") ? "command/clash/clash-darwin-arm64" : "command/clash/clash-darwin-amd64");
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

QString AppConfig::sysproxyExecutable() const
{
#if defined(Q_OS_WIN)
    return QDir(sourceRoot).filePath("command/sysproxy/sysproxy-windows-amd64.exe");
#elif defined(Q_OS_MACOS)
    return QDir(sourceRoot).filePath("command/sysproxy/sysproxy-darwin-amd64");
#else
    return QDir(sourceRoot).filePath("command/sysproxy/sysproxy-linux-amd64");
#endif
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
    config.allowRule = nestedValueFromYaml(yaml, "use_rule", "allow", config.allowRule);
    config.noAllowRule = nestedValueFromYaml(yaml, "use_rule", "noallow", config.noAllowRule);
    config.allowRuleEnabled = nestedBoolFromYaml(yaml, "use_rule", "allowUse", false);
    config.noAllowRuleEnabled = nestedBoolFromYaml(yaml, "use_rule", "noallowUse", false);
    config.theme = valueFromYaml(yaml, "theme", config.theme);
    config.language = valueFromYaml(yaml, "language", config.language);
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
