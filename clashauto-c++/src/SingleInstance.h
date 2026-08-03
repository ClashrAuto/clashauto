#pragma once

// 单实例守卫：保证同一用户同一时刻**只有一个 Coast 在跑**，并让第二次启动去把已有窗口叫到前台。
//
// ── 为什么这不是「锦上添花的体验优化」，而是**必须的正确性保护** ──────────────
// Coast 的透明网关把状态写进**内核全局命名空间**，而不是自己的进程里：
//   Linux：nftables 表 `coast_tproxy` + fwmark 策略路由 + route_localnet 等 sysctl；
//   macOS：pf anchor `coast` + net.inet.ip.forwarding。
// 这些名字是**固定**的（刻意如此：被 kill -9 打死后要靠固定名字清理残留，见
// TproxyRules::removeStale / PfRules::removeStale），因而清理**分不清是谁装的**。
// 于是两个实例并存时，后启动的那个：
//   · 启动时 removeStale() 先把前一个装的规则删干净；
//   · 退出时 remove() 再删一遍。
// 结果是 —— **第一个实例还活着、界面一切正常，它的整个数据面却被第二个实例的退出清空了。**
//
// 真机复现（树莓派网关，2026-08-03，两个实例同一数据目录）：
//     起第二个实例前：nft 表 1、劫持 9 台、ip rule fwmark 1 条
//     第二个实例退出后：nft 表 **0**、劫持 **0** 台、fwmark **0** 条、route_localnet 被还原成 0
//     而生产实例 pid 仍在，日志无任何异常，UI 照常显示"网关已开启"。
// 被代理设备就此**静默退回直连**：能上网，所以用户不会察觉，但代理与每设备策略已全部失效
// （对以"给孩子设备限流/分流"为卖点的功能来说，静默失效比断网更糟）。
// 用户侧的触发条件极其普通：**再双击一次图标**、或托盘还在时又从开始菜单点了一下。
//
// ── 实现选择 ────────────────────────────────────────────────────────────────
// 用 QLocalServer 而不是 QLockFile：QLockFile 能可靠判断"有没有别人在"，但没有通路把
// **"请把窗口显示出来"** 送过去。Coast 关窗口只是 hide（见 Main.qml onClosing），第二次
// 启动若只是静默退出，用户会觉得"点了没反应"——那会逼着他去杀进程重开，反而更危险。
// 连得上 = 有活实例 → 发一个字节请求前置 + 自己退出；连不上 = 没人 → 本进程当主实例监听。
// 进程被 kill -9 时 Unix 下会留下陈旧的 socket 文件，但 connect 会失败，随后 removeServer()
// 覆盖掉它，不需要额外的存活判定。
//
// ★ socket 名字**带用户标识**：多用户 Linux 上，用户 A 的实例不该挡住用户 B 启动 GUI。
//   这不削弱上面的保护——装 nft/pf 需要 root，两个**不同的**非 root 用户本来就动不了对方的
//   数据面（removeStale 在非 root 下会失败），真正会互相清规则的是同一个 root，而它们 uid
//   相同、必然共用同一个名字，照样被拦住。
//
// ★ `--tun-elevated`（Windows 开增强时以管理员身份重启自身）**必须豁免**：那条路上旧的
//   非提权实例还没退干净，提权实例若走探测就会连上它、把自己当成二次启动直接退出——增强
//   永远开不起来。这一点与旧 Widgets 版 main.cpp 的处理一致。

#include <QString>

#include <functional>
#include <memory>

class QLocalServer;

class SingleInstance
{
public:
    /// 探测是否已有本用户的实例在跑。true = 有（调用方应立刻退出，窗口已请对方前置）。
    /// `--tun-elevated` 等豁免场景由调用方判断后不调用本函数。
    static bool notifyExistingAndQuit();

    SingleInstance();
    ~SingleInstance();
    SingleInstance(const SingleInstance &) = delete;
    SingleInstance &operator=(const SingleInstance &) = delete;

    /// 本进程当主实例：开始监听，后续实例连上来时触发 onRaise（用于 show/raise 主窗）。
    /// listen 失败不算致命（顶多丢掉"前置窗口"这一个能力），只记一条日志。
    void listen(std::function<void()> onRaise);

private:
    std::unique_ptr<QLocalServer> m_server;
};
