// Windows 二层端点（Npcap）—— 透明网关在 Windows 上的 IL2Endpoint 实现。
// 用 Npcap(WinPcap 兼容 API)打开网卡、读/写完整以太帧；用 QWinEventNotifier 挂 pcap 事件句柄
// 融入 Qt 事件循环。
//
// 运行前提：目标机需安装 **Npcap 驱动**（含 WinPcap 兼容模式）。未装/无权限 → open 失败 →
// LanGateway.isAvailable()=false（优雅降级）。编译需 **Npcap SDK**（CI 里下载，见 release.yml）。
//
// 头文件次序：winsock2.h 必须在 windows.h/Qt 之前，否则与 winsock.h 冲突。故本文件用编译器内建
// 宏 _WIN32 作外层守卫（不依赖 Qt 的 Q_OS_WIN），并把 winsock2 放最前。
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <pcap.h>

#include "IL2Endpoint.h"

#include <QByteArray>
#include <QString>
#include <QWinEventNotifier>

#include <vector>

namespace {

// 由接口 GUID（QNetworkInterface::name()，形如 "{XXXX-...}"）取 6 字节 MAC。
QByteArray macForGuid(const QString &guid)
{
    QByteArray mac;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size)
        != ERROR_BUFFER_OVERFLOW)
        return mac;
    std::vector<char> buf(size);
    auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addrs, &size)
        != NO_ERROR)
        return mac;
    const QByteArray want = guid.toLatin1();
    for (auto *a = addrs; a; a = a->Next) {
        if (a->PhysicalAddressLength == 6
            && want.compare(a->AdapterName, Qt::CaseInsensitive) == 0) {
            mac = QByteArray(reinterpret_cast<const char *>(a->PhysicalAddress), 6);
            break;
        }
    }
    return mac;
}

class WinL2Endpoint final : public IL2Endpoint
{
public:
    explicit WinL2Endpoint(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~WinL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        // Npcap 的 wpcap.dll/Packet.dll 装在 %SystemRoot%\System32\Npcap\，且本程序对它们**延迟加载**
        // (/DELAYLOAD)，故普通用户没装 Npcap 时 app 照常运行、仅网关不可用。这里先把 Npcap 目录加入
        // DLL 搜索路径并试加载 wpcap.dll：加载不到 = 没装 Npcap → 返回 false（gatewayReady=false）。
        char sysdir[MAX_PATH] = {0};
        if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
            QByteArray npcapDir = QByteArray(sysdir) + "\\Npcap";
            SetDllDirectoryA(npcapDir.constData());
        }
        if (!LoadLibraryA("wpcap.dll")) {
            if (err) *err = QStringLiteral("未检测到 Npcap（请安装 Npcap 驱动后重试）");
            return false;
        }

        // ifname = 接口 GUID（QNetworkInterface::name()）；Npcap 设备名为 \Device\NPF_{GUID}。
        const QByteArray dev = QByteArray("\\Device\\NPF_") + ifname.toLatin1();
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        m_pcap = pcap_open_live(dev.constData(), 65536, 1 /*promisc*/, 1 /*ms*/, errbuf);
        if (!m_pcap) {
            if (err) *err = QStringLiteral("pcap_open_live 失败(需装 Npcap): ")
                            + QString::fromLatin1(errbuf);
            return false;
        }
        pcap_setmintocopy(m_pcap, 1); // 有 1 字节就触发事件，降低延迟
        m_localMac = macForGuid(ifname);

        HANDLE h = pcap_getevent(m_pcap);
        if (!h) {
            if (err) *err = QStringLiteral("pcap_getevent 返回空句柄");
            close();
            return false;
        }
        m_notifier = new QWinEventNotifier(h, this);
        QObject::connect(m_notifier, &QWinEventNotifier::activated, this,
                         [this](HANDLE) { drain(); });
        m_notifier->setEnabled(true);
        return true;
    }

    void close() override
    {
        if (m_notifier) {
            m_notifier->setEnabled(false);
            delete m_notifier;
            m_notifier = nullptr;
        }
        if (m_pcap) {
            pcap_close(m_pcap);
            m_pcap = nullptr;
        }
    }
    bool isOpen() const override { return m_pcap != nullptr; }

    bool send(const QByteArray &frame) override
    {
        return m_pcap
               && pcap_sendpacket(m_pcap, reinterpret_cast<const u_char *>(frame.constData()),
                                  frame.size())
                      == 0;
    }
    QByteArray localMac() const override { return m_localMac; }
    int ifIndex() const override { return 0; }
    int mtu() const override { return 1500; }

    // 内核态源 MAC 过滤：pcap_compile 一条 "ether src A or ether src B ..." 的 BPF，pcap_setfilter
    // 下发给 Npcap 驱动。过滤只影响捕获（收），pcap_sendpacket 不受影响。契约见 IL2Endpoint.h。
    bool setSourceMacFilter(const QVector<QByteArray> &macs) override
    {
        if (!m_pcap)
            return false;

        QByteArray expr;
        if (macs.isEmpty()) {
            // 没有被劫持设备：装一个「恒不命中」的过滤把整段流量挡在内核里。源 MAC 不可能是广播地址，
            // 拿它当恒假条件——比 "len < 0" 之类更稳的合法 pcap 语法。
            expr = "ether src ff:ff:ff:ff:ff:ff";
        } else {
            for (const QByteArray &m : macs) {
                if (m.size() != 6)
                    continue;
                if (!expr.isEmpty())
                    expr += " or ";
                expr += "ether src " + macToText(m);
            }
            if (expr.isEmpty())
                return false; // 一个合法 MAC 都没有：不装，交用户态兜底
        }

        struct bpf_program prog;
        // optimize=1；netmask 用 PCAP_NETMASK_UNKNOWN（表达式里不含 ip broadcast，用不到掩码）。
        if (pcap_compile(m_pcap, &prog, expr.constData(), 1, PCAP_NETMASK_UNKNOWN) < 0)
            return false; // 编译失败：不装内核过滤，用户态照旧兜底
        const int rc = pcap_setfilter(m_pcap, &prog);
        pcap_freecode(&prog);
        return rc == 0;
    }

private:
    // 6 字节 MAC → "aa:bb:cc:dd:ee:ff"（拼进 pcap 过滤表达式）。
    static QByteArray macToText(const QByteArray &m)
    {
        const auto *b = reinterpret_cast<const unsigned char *>(m.constData());
        return QString::asprintf("%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5])
            .toLatin1();
    }

    void drain()
    {
        struct pcap_pkthdr *hdr = nullptr;
        const u_char *data = nullptr;
        int r;
        // 1=拿到一帧；0=本轮无更多(超时)；<0=出错。循环取尽本次事件的所有帧。
        while (m_pcap && (r = pcap_next_ex(m_pcap, &hdr, &data)) == 1) {
            if (data && hdr)
                emit frameReceived(QByteArray(reinterpret_cast<const char *>(data), hdr->caplen));
        }
    }

    pcap_t *m_pcap = nullptr;
    QWinEventNotifier *m_notifier = nullptr;
    QByteArray m_localMac;
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new WinL2Endpoint(parent);
}

#endif // _WIN32
