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
bool isReady();

// 经 helper 以 root 设/清系统代理。返回是否成功。
bool setSystemProxy(bool enable, const QString &host, int port, const QStringList &bypass, QString *err);

// 经 helper 以 root 起 mihomo（-d userDir -f config）。返回是否成功。
bool startCore(const QString &execPath, const QString &configPath, const QString &userDir, QString *err);

// 经 helper 停止其启动的核心。返回是否成功。
bool stopCore(QString *err);

} // namespace MacHelper
