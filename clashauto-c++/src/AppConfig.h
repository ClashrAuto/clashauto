#pragma once

#include <QRect>
#include <QString>
#include <QStringList>

struct AppConfig {
    QString sourceRoot;
    QString userDir;
    QString host = "127.0.0.1";
    int uiPort = 9191; // REST API 端口（对齐 default.yaml external-controller）；用 9191 避开原版 9090
    int mixedPort = 7890;
    int autoUpdateMinutes = 0;
    bool webProxy = true;
    bool tun = false;
    bool nodeOnlyAvailable = true;
    bool clearConnections = true;
    bool increment = false;
    bool closeToTray = true;
    bool autoStart = false;
    bool nodeSwitchNote = true;
    bool allowRuleEnabled = false;
    bool noAllowRuleEnabled = false;
    QString allowRule;
    QString noAllowRule;
    QString theme = "black";
    bool autoTheme = false;
    bool mirror = false;               // 「国内加速 / 国内代理下载」共用：下载走国内镜像(ghfast.top)
    QString language = "zh-CN";

    QString clashExecutable() const;
    QString clashConfig() const;
};

class AppConfigLoader
{
public:
    static AppConfig load();

private:
    static QString valueFromYaml(const QString &text, const QString &key, const QString &fallback);
    static QString nestedValueFromYaml(const QString &text, const QString &section, const QString &key, const QString &fallback);
    static bool boolFromYaml(const QString &text, const QString &key, bool fallback);
    static bool nestedBoolFromYaml(const QString &text, const QString &section, const QString &key, bool fallback);
    static int intFromYaml(const QString &text, const QString &key, int fallback);
};
