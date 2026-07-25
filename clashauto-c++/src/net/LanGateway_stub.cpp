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
QStringList LanGateway::activeDevices() const { return {}; }
