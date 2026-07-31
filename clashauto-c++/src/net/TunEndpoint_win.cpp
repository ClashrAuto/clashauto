// TUN 端点（Windows / Wintun）—— 把「本机全量流量」接进已有的用户态栈。
//
// 设计与 Linux 版同源：**伪装成 IL2Endpoint**，读时给裸 IP 包前置 14 字节以太头、写时剥掉，
// 上层 NetStack/lwIP 一行不改。合成 MAC、以太头长度都取自共享的 TunEndpoint.h（放一处，
// 免得两个平台各持一份迟早漂移）。总纲见那个头文件。
//
// —— Windows 侧特有的三件事 ——
//
// ① **wintun.dll 运行期动态加载，不静态链接**。仓库里没有 wintun 的 .lib/.h（只内嵌了按架构的
//    DLL，见 resources_win.qrc），而且我们要在**没有 mihomo 的情况下**也能用 TUN —— 这正是
//    「替换内核」的要点。所以这里自己找 DLL：先看已部署的两个位置，都没有就**从 qrc 按当前架构
//    释放一份**。不能指望 CoreController::deploy() 跑过（那条路只有下载过内核才会走）。
//
// ② **句柄类型自己声明**。函数签名照 wintun 0.14 的 ABI 抄，全是 WINAPI(stdcall)。签名写错在
//    x64 上不会立刻炸（寄存器传参），会在运行时以诡异方式坏掉 —— 改这里务必对着官方 wintun.h。
//
// ③ **收帧是 level-triggered 的**：WintunGetReadWaitEvent 的事件在环里有包时置位、排空后由驱动
//    复位。所以 drain() 里加迭代上限（别把事件循环饿死）是安全的 —— 没排完事件仍然是置位状态，
//    QWinEventNotifier 会立刻再来一发。
//
// 权限：建网卡需要 **Administrator**（wintun 要装/启网络适配器）。拿不到就 open() 失败并说明，
// 由上层决定回退 mihomo 的 TUN。
#include "TunEndpoint.h"

#include "IL2Endpoint.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QStandardPaths>
#include <QSysInfo>
#include <QWinEventNotifier>

#include <winsock2.h>
// clang-format off
#include <windows.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on

namespace {

// —— wintun 0.14 ABI ——（对着官方 wintun.h；改动前请复核）
struct WINTUN_ADAPTER;
struct WINTUN_SESSION;
using WINTUN_ADAPTER_HANDLE = WINTUN_ADAPTER *;
using WINTUN_SESSION_HANDLE = WINTUN_SESSION *;

using FnCreateAdapter = WINTUN_ADAPTER_HANDLE(WINAPI *)(LPCWSTR, LPCWSTR, const GUID *);
using FnCloseAdapter = void(WINAPI *)(WINTUN_ADAPTER_HANDLE);
using FnGetAdapterLUID = void(WINAPI *)(WINTUN_ADAPTER_HANDLE, NET_LUID *);
using FnStartSession = WINTUN_SESSION_HANDLE(WINAPI *)(WINTUN_ADAPTER_HANDLE, DWORD);
using FnEndSession = void(WINAPI *)(WINTUN_SESSION_HANDLE);
using FnGetReadWaitEvent = HANDLE(WINAPI *)(WINTUN_SESSION_HANDLE);
using FnReceivePacket = BYTE *(WINAPI *)(WINTUN_SESSION_HANDLE, DWORD *);
using FnReleaseReceivePacket = void(WINAPI *)(WINTUN_SESSION_HANDLE, const BYTE *);
using FnAllocateSendPacket = BYTE *(WINAPI *)(WINTUN_SESSION_HANDLE, DWORD);
using FnSendPacket = void(WINAPI *)(WINTUN_SESSION_HANDLE, const BYTE *);
using FnGetRunningDriverVersion = DWORD(WINAPI *)(void);

struct WintunApi
{
    FnCreateAdapter createAdapter = nullptr;
    FnCloseAdapter closeAdapter = nullptr;
    FnGetAdapterLUID getAdapterLuid = nullptr;
    FnStartSession startSession = nullptr;
    FnEndSession endSession = nullptr;
    FnGetReadWaitEvent getReadWaitEvent = nullptr;
    FnReceivePacket receivePacket = nullptr;
    FnReleaseReceivePacket releaseReceivePacket = nullptr;
    FnAllocateSendPacket allocateSendPacket = nullptr;
    FnSendPacket sendPacket = nullptr;
    FnGetRunningDriverVersion runningDriverVersion = nullptr;
    bool ok = false;
};

// qrc 里 wintun.dll 的架构子目录名（resources_win.qrc 里那四个）。
QString wintunArchDir()
{
    const QString a = QSysInfo::currentCpuArchitecture();
    if (a == QLatin1String("arm64"))
        return QStringLiteral("arm64");
    if (a.startsWith(QLatin1String("arm")))
        return QStringLiteral("arm");
    if (a == QLatin1String("i386"))
        return QStringLiteral("x86");
    return QStringLiteral("x64"); // x86_64 及未知一律按 x64
}

// 找 wintun.dll：① 自己 exe 旁 ② 内核旁（CoreController 部署的位置）③ 从 qrc 释放到 AppData。
// 返回绝对路径；空 = 连释放都失败。
QString locateWintun(QString *err)
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QStringList candidates = {
        QDir(exeDir).filePath(QStringLiteral("wintun.dll")),
        QDir(appData).filePath(QStringLiteral("command/wintun.dll")),
        QDir(appData).filePath(QStringLiteral("wintun.dll")),
    };
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c))
            return c;
    }

    // 都没有 —— 从内嵌资源按架构释放一份。**这条路是关键**：内核可能从未下载过，
    // 而「进程内 TUN」恰恰要在没有 mihomo 的情况下工作。
    const QString from = QStringLiteral(":/assets/bundle/wintun/%1/wintun.dll").arg(wintunArchDir());
    if (!QFileInfo::exists(from)) {
        if (err)
            *err = QStringLiteral("内嵌资源里没有本架构(%1)的 wintun.dll").arg(wintunArchDir());
        return {};
    }
    const QString to = QDir(appData).filePath(QStringLiteral("wintun.dll"));
    QDir().mkpath(appData);
    if (!QFile::copy(from, to)) {
        if (err)
            *err = QStringLiteral("释放 wintun.dll 到 %1 失败").arg(to);
        return {};
    }
    QFile(to).setPermissions(QFile::permissions(to) | QFileDevice::WriteOwner); // qrc 出来是只读
    return to;
}

// 进程内只加载一次。
const WintunApi *wintun(QString *err)
{
    static WintunApi api;
    static bool tried = false;
    static QString loadErr;
    if (tried) {
        if (!api.ok && err)
            *err = loadErr;
        return api.ok ? &api : nullptr;
    }
    tried = true;

    const QString path = locateWintun(&loadErr);
    if (path.isEmpty()) {
        if (err)
            *err = loadErr;
        return nullptr;
    }
    static QLibrary lib(path);
    if (!lib.load()) {
        loadErr = QStringLiteral("加载 %1 失败：%2").arg(path, lib.errorString());
        if (err)
            *err = loadErr;
        return nullptr;
    }
    auto sym = [](QLibrary &l, const char *n) { return l.resolve(n); };
    api.createAdapter = reinterpret_cast<FnCreateAdapter>(sym(lib, "WintunCreateAdapter"));
    api.closeAdapter = reinterpret_cast<FnCloseAdapter>(sym(lib, "WintunCloseAdapter"));
    api.getAdapterLuid = reinterpret_cast<FnGetAdapterLUID>(sym(lib, "WintunGetAdapterLUID"));
    api.startSession = reinterpret_cast<FnStartSession>(sym(lib, "WintunStartSession"));
    api.endSession = reinterpret_cast<FnEndSession>(sym(lib, "WintunEndSession"));
    api.getReadWaitEvent = reinterpret_cast<FnGetReadWaitEvent>(sym(lib, "WintunGetReadWaitEvent"));
    api.receivePacket = reinterpret_cast<FnReceivePacket>(sym(lib, "WintunReceivePacket"));
    api.releaseReceivePacket =
            reinterpret_cast<FnReleaseReceivePacket>(sym(lib, "WintunReleaseReceivePacket"));
    api.allocateSendPacket =
            reinterpret_cast<FnAllocateSendPacket>(sym(lib, "WintunAllocateSendPacket"));
    api.sendPacket = reinterpret_cast<FnSendPacket>(sym(lib, "WintunSendPacket"));
    api.runningDriverVersion =
            reinterpret_cast<FnGetRunningDriverVersion>(sym(lib, "WintunGetRunningDriverVersion"));

    api.ok = api.createAdapter && api.closeAdapter && api.getAdapterLuid && api.startSession
            && api.endSession && api.getReadWaitEvent && api.receivePacket
            && api.releaseReceivePacket && api.allocateSendPacket && api.sendPacket;
    if (!api.ok) {
        loadErr = QStringLiteral("%1 里缺少 wintun 导出符号（版本不匹配？）").arg(path);
        if (err)
            *err = loadErr;
        return nullptr;
    }
    return &api;
}

QString lastErrorText(DWORD e)
{
    wchar_t *buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                           | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, e, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
    QString s = n ? QString::fromWCharArray(buf, int(n)).trimmed() : QString::number(e);
    if (buf)
        LocalFree(buf);
    return s;
}

// 环容量：必须是 2 的幂、[128 KiB, 64 MiB]。4 MiB 与 mihomo 的 wintun 用量同量级。
constexpr DWORD kRingCapacity = 0x400000;

} // namespace

class TunEndpointWin final : public IL2Endpoint
{
public:
    explicit TunEndpointWin(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~TunEndpointWin() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        m_api = wintun(err);
        if (!m_api)
            return false;

        const QString name = ifname.isEmpty() ? QStringLiteral("Coast") : ifname;
        m_adapter = m_api->createAdapter(reinterpret_cast<LPCWSTR>(name.utf16()), L"Coast", nullptr);
        if (!m_adapter) {
            const DWORD e = GetLastError();
            if (err)
                *err = QStringLiteral("创建 wintun 网卡「%1」失败：%2（%3）")
                               .arg(name, lastErrorText(e),
                                    e == ERROR_ACCESS_DENIED
                                            ? QStringLiteral("需要以管理员身份运行")
                                            : QStringLiteral("错误码 %1").arg(e));
            return false;
        }

        m_session = m_api->startSession(m_adapter, kRingCapacity);
        if (!m_session) {
            if (err)
                *err = QStringLiteral("wintun 会话建立失败：%1").arg(lastErrorText(GetLastError()));
            m_api->closeAdapter(m_adapter);
            m_adapter = nullptr;
            return false;
        }

        NET_LUID luid{};
        m_api->getAdapterLuid(m_adapter, &luid);
        NET_IFINDEX idx = 0;
        if (ConvertInterfaceLuidToIndex(&luid, &idx) == NO_ERROR)
            m_ifIndex = int(idx);
        m_ifname = name;

        HANDLE ev = m_api->getReadWaitEvent(m_session);
        m_notifier = new QWinEventNotifier(ev, this);
        connect(m_notifier, &QWinEventNotifier::activated, this, [this](HANDLE) { drain(); });
        m_notifier->setEnabled(true);
        drain(); // 装通知器前可能已经有包了
        return true;
    }

    void close() override
    {
        if (m_notifier) {
            m_notifier->setEnabled(false);
            delete m_notifier;
            m_notifier = nullptr;
        }
        if (m_session) {
            m_api->endSession(m_session);
            m_session = nullptr;
        }
        if (m_adapter) {
            m_api->closeAdapter(m_adapter);
            m_adapter = nullptr;
        }
    }
    bool isOpen() const override { return m_session != nullptr; }

    // 上层给的是完整以太帧：剥掉 14 字节头，只把 IP 包塞进 wintun 的发送环。
    bool send(const QByteArray &frame) override
    {
        if (!m_session || frame.size() <= coastcore::kTunEthHdr)
            return false;
        const DWORD n = DWORD(frame.size() - coastcore::kTunEthHdr);
        BYTE *p = m_api->allocateSendPacket(m_session, n);
        if (!p)
            return false; // 环满(ERROR_BUFFER_OVERFLOW) → 丢；由 TCP 重传兜底，契约允许
        memcpy(p, frame.constData() + coastcore::kTunEthHdr, n);
        m_api->sendPacket(m_session, p);
        return true;
    }

    QByteArray localMac() const override { return coastcore::tunLocalMac(); }
    int ifIndex() const override { return m_ifIndex; }
    int mtu() const override { return 1500; }
    void drainNow() override { drain(); }
    QString ifname() const { return m_ifname; }

private:
    // 排空接收环：每个裸 IP 包套上以太头交给上层。
    void drain()
    {
        if (!m_session)
            return;
        for (int i = 0; i < 256; ++i) { // 上限只为不饿死事件循环；事件是 level-triggered，还有包会立刻再来
            DWORD n = 0;
            BYTE *p = m_api->receivePacket(m_session, &n);
            if (!p)
                break; // ERROR_NO_MORE_ITEMS = 空了
            if (n >= 1) {
                const unsigned char ver = p[0] >> 4;
                if (ver == 4 || ver == 6) {
                    QByteArray frame;
                    frame.reserve(int(n) + coastcore::kTunEthHdr);
                    frame.append(reinterpret_cast<const char *>(coastcore::kTunLocalMac), 6);
                    frame.append(reinterpret_cast<const char *>(coastcore::kTunPeerMac), 6);
                    const unsigned char et4[2] = {0x08, 0x00};
                    const unsigned char et6[2] = {0x86, 0xDD};
                    frame.append(reinterpret_cast<const char *>(ver == 4 ? et4 : et6), 2);
                    frame.append(reinterpret_cast<const char *>(p), int(n));
                    emit frameReceived(frame);
                }
            }
            m_api->releaseReceivePacket(m_session, p);
        }
    }

    const WintunApi *m_api = nullptr;
    WINTUN_ADAPTER_HANDLE m_adapter = nullptr;
    WINTUN_SESSION_HANDLE m_session = nullptr;
    QWinEventNotifier *m_notifier = nullptr;
    QString m_ifname;
    int m_ifIndex = 0;
};

IL2Endpoint *createTunEndpoint(QObject *parent)
{
    return new TunEndpointWin(parent);
}
