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
    // 启用 IPv6。默认关 —— 打开后 ConfigBuilder 才会往 full.yaml 写 ipv6/dns.ipv6/
    // dns.fake-ip-range6 三件套。注意三者缺一不可：只开 dns.ipv6 而没有 v6 fake-ip 池时，
    // fake-ip 模式下核心对 AAAA 照样回空答案（见 mihomo dns/middleware.go 的 withFakeIP）。
    bool ipv6 = false;
    // 透明网关的**数据面**走哪条路。默认 false = 现有的 lwIP 用户态栈。
    //
    // 打开后改走「内核转发 + nftables TPROXY」：ARP 劫持照旧（那是「设备什么都不用改」的来源），
    // 但劫持来的帧不再喂进 lwIP，而是由内核转发、TPROXY 投给核心。树莓派 5 实测同机同靶：
    //   lwIP 0.89 Gbps @ 82%（单线程,换网卡也过不去）  vs  TPROXY 10.6 Gbps @ 0.73 核。
    // 仅 Linux 有效（需要 nftables + iproute2 + root）；其它平台恒为 lwIP。
    //
    // ★ 默认 false 是刻意的：这条路一旦装了规则却没人接管，被覆盖的设备会**完全断网**，
    //   所以在它跑够久之前，默认必须是可回退的那一条。环境变量 COAST_GATEWAY_DATAPATH=tproxy
    //   可临时覆盖（供测试；见 AppConfigLoader::load）。
    bool gatewayTproxy = false;
    // 无可用代理节点时的兜底行为。默认 false = 回落直连（clash 既定语义：普通代理设备跟随
    // 规则、规则最终回落 DIRECT）。true = fail-closed：无订阅节点时把"该走代理"的流量导向
    // REJECT,避免被劫持设备在机场跑路/订阅过期时**静默裸奔**(真实 IP 泄露、翻墙失效)。
    // ★ 默认 false 是用户 2026-08-03 明确拍板的:能上网优先于防泄露,且真实用户几乎总配了节点,
    //   无节点只是配置未完成的中间态;fail-closed 会因节点抖动误伤断网。隐私优先的用户可开。
    // 配置键 noNodeReject;环境变量 COAST_NO_NODE_REJECT=1/0 覆盖(供测试/应急)。
    // 已知范围:只覆盖"订阅完全无节点"这个静态可判场景(最常见);"有节点但运行时全部测速失败
    //   回落 DIRECT"是运行时状态,本项不拦(见 ConfigBuilder 注入处的说明)。
    bool noNodeReject = false;
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
