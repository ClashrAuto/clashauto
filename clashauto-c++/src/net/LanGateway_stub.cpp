// LanGateway 的非 Linux 桩实现（CMake 在非 Linux 平台编入本文件）。
// M1 只做 Linux 透明网关；Windows(Npcap)/macOS(BPF) 留待 M3/M4。此桩让 DevicesController 等
// 跨平台代码正常编译链接：isAvailable() 恒 false，enableDevice() 直接失败。
#include "LanGateway.h"

struct LanGateway::Impl {};

LanGateway::LanGateway(QObject *parent) : QObject(parent), d(new Impl) {}
LanGateway::~LanGateway() { delete d; }

void LanGateway::configure(const QVector<NicSpec> &, quint16) {}
bool LanGateway::isAvailable() const { return false; }
bool LanGateway::canProxy(const QString &) const { return false; }

bool LanGateway::enableDevice(const QString &, const QString &, const QString &, QString *err)
{
    if (err)
        *err = QStringLiteral("当前平台暂不支持透明网关（仅 Linux）");
    return false;
}
void LanGateway::disableDevice(const QString &) {}
void LanGateway::disableAll() {}
void LanGateway::recoverFromCrash() {}
// 非 Linux 无网关数据面 → CoastCore 进程内出站无处可接，空实现（shared_ptr 按值析构，无需完整类型）。
void LanGateway::setCoastCore(bool, bool, std::shared_ptr<ProxyConfigStore>,
                              std::shared_ptr<RuleEngine>, std::shared_ptr<DnsResolver>) {}
// 非 Linux/mac/Win 无数据面 → 没有协议栈可借。进程内 TUN 拿到 nullptr 后自建（它那条路本来就
// 只在能建 TUN 的平台上走得通）。
NetStack *LanGateway::acquireStack(QString *err)
{
    if (err)
        *err = QStringLiteral("当前平台无网关数据面，无共享协议栈");
    return nullptr;
}
QStringList LanGateway::activeDevices() const { return {}; }
