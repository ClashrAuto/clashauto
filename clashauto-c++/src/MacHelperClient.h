// macOS 特权 helper 的应用侧封装：SMAppService 注册/注销 + XPC 客户端。
// 纯 C++ 接口，实现在 MacHelperClient.mm（Obj-C++）。仅在 macOS 编译/链接。
#pragma once

#include <QString>
#include <QStringList>

namespace MacHelper {

// 对应 SMAppServiceStatus，外加连接失败/不适用
enum class RegStatus {
    NotRegistered,    // 未注册
    Enabled,          // 已注册并启用（可用）
    RequiresApproval, // 已注册但待用户在「系统设置」批准
    NotFound,         // plist 找不到（bundle 布局异常）
    Unknown,
};

// 当前 SMAppService 注册状态（不触发注册）
RegStatus status();

// 注册 daemon（首次会引导系统弹出批准）。err 写入失败原因。返回注册后的状态。
RegStatus registerDaemon(QString *err);

// 注销 daemon。返回是否成功。
bool unregisterDaemon(QString *err);

// 打开「系统设置 → 登录项」，引导用户批准本应用的后台项（daemon 注册后需一次批准）。
void openLoginItemsSettings();

// 经 XPC 向 helper 请求版本号；成功返回版本串，失败返回空并把原因写入 err。
QString pingVersion(QString *err);

// 便捷判断：helper 是否已装好且能应答（status==Enabled 且 ping 成功）
// helper 能不能用。`attempts` 是 ping 的次数：默认 3 次是给**刚注册**的 daemon 留冷启动时间
// （launchd 按需拉起，首个 XPC 连接可能比单次 5s 超时更慢）。
// ★ **退出/关代理这类路径要传 1**：那时 daemon 早就该是热的，而「注册着但不应答」时每次 ping
//   都要等满 5 秒 —— 3 次就是 15 秒的界面冻结，卡在用户点了退出之后。这个组合并不罕见：
//   自更新替换 .app 之后 helper 的代码签名要求对不上，status 仍是 Enabled，XPC 却没人应答。
bool isReady(int attempts = 3);

// 经 helper 以 root 设/清系统代理。返回是否成功。
bool setSystemProxy(bool enable, const QString &host, int port, const QStringList &bypass, QString *err);

// 经 helper 以 root 起 mihomo（-d userDir -f config）。返回是否成功。
bool startCore(const QString &execPath, const QString &configPath, const QString &userDir, QString *err);

// 经 helper 停止其启动的核心。返回是否成功。
bool stopCore(QString *err);

// 经 helper 以 root 打开并配置绑到 ifname 的 BPF 设备，返回**本进程可用的 fd**（已 dup，调用方负责 close）。
// 失败返回 -1 并把原因写入 err。透明网关(L2Endpoint_mac)非 root 运行时用它拿二层收发能力。
int openBpf(const QString &ifname, QString *err);

// ── 透明网关的 pf 数据面。三件事（pfctl / 写 forwarding / 读 /dev/pf）**都要 root**，
//    而 GUI 应用是普通用户 uid，一件都做不了 —— 所以整条生命周期交给 helper。
//    规则文本由 helper 自己拼，这里只给端口/网卡名/IP 列表（见 HelperProtocol.h 的说明）。
bool pfInstall(int redirPort, int dnsPort, const QStringList &ifnames, QString *err);
bool pfSyncProxied(const QStringList &ipv4, QString *err);
bool pfRemove(QString *err);

} // namespace MacHelper
