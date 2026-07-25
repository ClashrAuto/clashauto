// Windows 二层端点工厂。M4（Npcap）待实现——现返回 nullptr（LanGateway 在 Windows 用桩，
// isAvailable()=false，与 mac 之前一致）。实现 Npcap 端点后替换此处。
#include "IL2Endpoint.h"

#if defined(Q_OS_WIN)

IL2Endpoint *createL2Endpoint(QObject * /*parent*/)
{
    return nullptr;
}

#endif // Q_OS_WIN
