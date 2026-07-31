#pragma once

#include <QRect>
#include <QString>
#include <QStringList>

struct AppConfig {
    QString userDir;
    QString configDir; // userDir/config：放 config.yaml/full.yaml 等配置 yaml；logs\、Country.mmdb、cache 仍在 userDir 根
    QString host = "127.0.0.1";
    QString secret; // external-controller 访问密钥：为空时首次加载随机生成并落盘到用户 config.yaml
    int uiPort = 9191; // REST API 端口（对齐 default.yaml external-controller）；用 9191 避开原版 9090
    int mixedPort = 7890;
    int autoUpdateMinutes = 0;
    bool webProxy = true;
    bool tun = false;
    bool nodeOnlyAvailable = true;
    bool clearConnections = true;
    bool increment = false;
    bool closeToTray = false; // 关闭到托盘（config.mini）默认关：正常显示窗口 + ✕ 退出。见 AppConfigLoader::load 首次落地归一化
    bool autoStart = false;
    bool nodeSwitchNote = true;
    bool allowRuleEnabled = false;
    bool noAllowRuleEnabled = false;
    QString allowRule;
    QString noAllowRule;
    QString theme = "black";
    bool autoTheme = false;
    bool autoLanguage = true; // 跟随系统语言（默认开）：按系统区域自动选 zh-CN/en-US，忽略 language 手选；关则用 language
    bool mirror = false;               // 「国内加速 / 国内代理下载」共用：下载走国内镜像(ghfast.top)
    // 接收测试版：更新检查与「一键更新」改用 prerelease 频道。CI 对**非主分支**的每次推送都会发
    // 一个 prerelease（tag 带 -beta.<sha>），主分支才发正式版。默认关 —— 普通用户不该被卷进测试流。
    bool receiveBeta = false;
    // 把网关这条数据面搬进本进程（CoastCore）。默认关 = 零行为变化，网关全走 mihomo。
    // 开启后 **DNS + TCP + UDP 整条都不经核心**：直连/全局/规则三种模式都在进程内分流，
    // 设备的 :53 由我们自己答（fake-ip 当场合成，见 NetStack::answerDnsLocally）。
    // 判不了的情形（协议没编进来、规则要先解析 IP、fake-ip 没反查到）仍**回退** mihomo，
    // 回退按原因记账，见 GatewayDiag 的 cc=… 那几栏。（config.yaml 键 `coastcore`）
    // 默认 **true**：数据面已经过真机验证（0.70 ms / 2883 conn/s / 0% 失败，且杀掉 mihomo
    // 后网关照常服务）。这里改默认值而不是只改种子 config.yaml —— 只改种子的话，**已经装过
    // 的用户**其 config.yaml 里没有这个键，会一直停在旧默认上，等于只覆盖了一半人。
    // 出问题时用户仍可在 config.yaml 里写 `coastcore: false` 全部回落 mihomo。
    bool coastcore = true;
    // 严格模式：上面那些「判不了」的情形**拒绝回退**，直接让该连接失败。默认关。
    // 用途是把「离完全替换 mihomo 还差多少」暴露出来 —— 静默回退会把差距藏起来。
    // 代价是这些连接会断，所以单独一个开关。（config.yaml 键 `coastcore_strict`）
    bool coastcoreStrict = false;

    // 本机 HTTP/SOCKS 混合入站的监听端口（回环）。**0 = 关闭**（默认，零行为变化）。
    // 打开后本机流量可以直接指向它，走**进程内引擎**而不是 mihomo —— 这是 CoastCore 的第二个
    // 入口（第一个是局域网网关）。在此之前进程内出站只服务被代理的设备，本机自己 100% 走 mihomo。
    // 与 mihomo 的混合端口（`port`，默认 7890）**并存**，便于同机 A/B 对比两个引擎。
    // 只有 coastcore 开着时才有意义（关着时没有可用的配置快照）。（config.yaml 键 `coastcore_inbound`）
    int coastcoreInboundPort = 0;
    QString language = "zh-CN";

    QString clashExecutable() const;
    QString clashConfig() const;

    // qrc(:/…) 复制出来的文件默认只读，补上属主读写权限，供 config.yaml/default.yaml 等种子后续编辑保存。
    static void makeWritable(const QString &path);
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
