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

#include "GatewayDiag.h" // 数据面收发帧计数（与 linux/mac 端点对称；将来诊断版发布后 win 也有数据）
#include "IL2Endpoint.h"
#include "NpcapStatus.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QString>
#include <QWinEventNotifier>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace {
// COAST_GATEWAY_DEBUG=1 时把二层收发的关键节点打到 stderr。GUI 程序看不到 stderr，但
// `Coast.exe 2> gw.txt` 能把它重定向到文件——排查「设备流量为 0」用它定位断点。
bool dbgOn()
{
    static const bool on = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");
    return on;
}
void dbg(const char *fmt, ...)
{
    if (!dbgOn())
        return;
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[WINL2] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    va_end(ap);
}
} // namespace

namespace {

// —— wpcap.dll 运行时绑定 ——
// 本文件**不与 wpcap 导入库链接**，全部 pcap_* 都在首次用到时 GetProcAddress 拿。
// 理由：Npcap 是可选依赖，没装的机器上 Coast 必须照常启动（只是网关不可用）。原先靠 MSVC 的
// /DELAYLOAD 实现这一点，但那段是 if(MSVC) 守卫的——MinGW 构建会把 wpcap.dll 写进导入表，
// 没装 Npcap 的机器上进程直接 0xC0000135 起不来（本地开发构建长期如此）。运行时绑定对两种
// 工具链都成立，也省掉 delayimp 依赖。
// 函数指针类型一律用 decltype(&pcap_xxx) 从 pcap.h 推出来，签名不可能写歪。
struct PcapApi
{
    decltype(&pcap_open_live) open_live = nullptr;
    decltype(&pcap_close) close = nullptr;
    decltype(&pcap_setmintocopy) setmintocopy = nullptr;
    decltype(&pcap_getevent) getevent = nullptr;
    decltype(&pcap_sendpacket) sendpacket = nullptr;
    decltype(&pcap_next_ex) next_ex = nullptr;
    decltype(&pcap_compile) compile = nullptr;
    decltype(&pcap_setfilter) setfilter = nullptr;
    decltype(&pcap_freecode) freecode = nullptr;
    decltype(&pcap_setbuff) setbuff = nullptr; // 可选：加大驱动内核收缓冲（缺了不降级）
};

// 驱动内核收缓冲字节数。pcap_open_live 用的是 WinPcap/Npcap 的默认值（历史上 1 MiB 级），
// 十几台设备一起下载时**收方**方向很容易在内核里溢出——溢出即静默丢设备发来的帧，lwIP 那条
// TCP 只能等对端重传，表现就是「时好时坏 / 极慢」。加大到 4 MiB，与 lwipopts.h 里 PBUF_POOL
// （入站帧缓冲）同量级。代价是驱动占的非分页内存，家用几十台设备的规模可接受。
// 注意这是**收**缓冲；发方(pcap_sendpacket)是同步写驱动 TX 缓冲，没有对应的用户态旋钮，也没有
// linux/mac 那种 EAGAIN+可写事件的流控模型，故 win 侧不做「发方积压队列」（做不了也不需要）。
constexpr int kRxKernelBufBytes = 4 * 1024 * 1024;
// 发方丢帧告警节流（与 linux/mac 端点一致）。
constexpr qint64 kDropReportMinIntervalMs = 30000;

// 加载并解析 wpcap.dll。返回 nullptr = 没装 Npcap（或版本太老、缺符号）。
// 只做一次；DLL 句柄故意不释放（进程生命周期内一直要用）。
const PcapApi *pcapApi()
{
    static bool tried = false;
    static PcapApi api;
    static bool ok = false;
    if (tried)
        return ok ? &api : nullptr;
    tried = true;

    // Npcap 的 DLL 装在 %SystemRoot%\System32\Npcap\，不在默认搜索路径上，先把它加进去。
    // 用 AddDllDirectory 语义的 LOAD_LIBRARY_SEARCH_* 更干净，但那要求 LoadLibraryEx 带 flag
    // 且系统支持；这里沿用 SetDllDirectory + LoadLibrary，行为与旧代码一致。
    char sysdir[MAX_PATH] = {0};
    if (GetSystemDirectoryA(sysdir, MAX_PATH)) {
        const QByteArray npcapDir = QByteArray(sysdir) + "\\Npcap";
        SetDllDirectoryA(npcapDir.constData());
    }
    HMODULE h = LoadLibraryA("wpcap.dll");
    SetDllDirectoryA(nullptr); // 还原进程级 DLL 搜索路径，别留副作用给别的模块
    if (!h)
        return nullptr;

    auto sym = [h](const char *name) { return GetProcAddress(h, name); };
    api.open_live = reinterpret_cast<decltype(&pcap_open_live)>(sym("pcap_open_live"));
    api.close = reinterpret_cast<decltype(&pcap_close)>(sym("pcap_close"));
    api.setmintocopy = reinterpret_cast<decltype(&pcap_setmintocopy)>(sym("pcap_setmintocopy"));
    api.getevent = reinterpret_cast<decltype(&pcap_getevent)>(sym("pcap_getevent"));
    api.sendpacket = reinterpret_cast<decltype(&pcap_sendpacket)>(sym("pcap_sendpacket"));
    api.next_ex = reinterpret_cast<decltype(&pcap_next_ex)>(sym("pcap_next_ex"));
    api.compile = reinterpret_cast<decltype(&pcap_compile)>(sym("pcap_compile"));
    api.setfilter = reinterpret_cast<decltype(&pcap_setfilter)>(sym("pcap_setfilter"));
    api.freecode = reinterpret_cast<decltype(&pcap_freecode)>(sym("pcap_freecode"));
    api.setbuff = reinterpret_cast<decltype(&pcap_setbuff)>(sym("pcap_setbuff"));

    // 缺任何一个都当没装：宁可优雅降级，也不要半残地跑到一半崩。
    // setbuff **不在**必需清单里——它纯属收方优化，老 wpcap 缺它也应照常工作。
    ok = api.open_live && api.close && api.setmintocopy && api.getevent && api.sendpacket
            && api.next_ex && api.compile && api.setfilter && api.freecode;
    return ok ? &api : nullptr;
}

// 由 QNetworkInterface::name() 反查这张网卡的 Npcap 设备名与 MAC。
//
// ★ 这里有个足以让整个网关在 Windows 上永远打不开的坑：**Qt 6 的 QNetworkInterface::name()
//   在 Windows 上返回的不是适配器 GUID**，而是 LUID 名（ConvertInterfaceLuidToNameW 的结果，
//   形如 "ethernet_32775"）；只有取不到 LUID 名时才退回 IP_ADAPTER_ADDRESSES::AdapterName
//   （那个才是 "{XXXXXXXX-....}"）。而 Npcap 的设备名固定是 \Device\NPF_{GUID}。
//   直接把 name() 拼进设备名 ⇒ "\Device\NPF_ethernet_32775" ⇒ 必然打不开。
//   （这个错误此前一直被 Npcap 的 AdminOnly「拒绝访问」挡在前面，看不出来。）
// 所以按三种口径逐个比对，命中后一律用该适配器**真正的 AdapterName(GUID)** 拼设备名。
struct AdapterInfo
{
    QByteArray npfName; // "\Device\NPF_{GUID}"
    QByteArray mac;     // 6 字节；取不到则为空
    bool ok = false;
};

AdapterInfo adapterFor(const QString &ifname)
{
    AdapterInfo out;
    if (ifname.isEmpty())
        return out;

    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size)
        != ERROR_BUFFER_OVERFLOW)
        return out;
    std::vector<char> buf(size);
    auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, addrs, &size)
        != NO_ERROR)
        return out;

    for (auto *a = addrs; a; a = a->Next) {
        const QString guid = QString::fromLatin1(a->AdapterName);

        wchar_t luidBuf[IF_MAX_STRING_SIZE + 1] = {0};
        QString luidName;
        if (ConvertInterfaceLuidToNameW(&a->Luid, luidBuf, IF_MAX_STRING_SIZE + 1) == NO_ERROR)
            luidName = QString::fromWCharArray(luidBuf);

        const QString friendly =
                a->FriendlyName ? QString::fromWCharArray(a->FriendlyName) : QString();

        const bool hit = ifname.compare(guid, Qt::CaseInsensitive) == 0
                || (!luidName.isEmpty() && ifname.compare(luidName, Qt::CaseInsensitive) == 0)
                || (!friendly.isEmpty() && ifname.compare(friendly, Qt::CaseInsensitive) == 0);
        if (!hit)
            continue;

        out.npfName = QByteArray("\\Device\\NPF_") + guid.toLatin1();
        if (a->PhysicalAddressLength == 6)
            out.mac = QByteArray(reinterpret_cast<const char *>(a->PhysicalAddress), 6);
        out.ok = !guid.isEmpty();
        break;
    }
    return out;
}

class WinL2Endpoint final : public IL2Endpoint
{
public:
    explicit WinL2Endpoint(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~WinL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        // wpcap.dll 运行时绑定（见上面的 pcapApi）：解析不到 = 没装 Npcap，优雅降级
        // （gatewayReady=false），程序其余部分照常。
        m_api = pcapApi();
        if (!m_api) {
            if (err) *err = QStringLiteral("未检测到 Npcap（请安装 Npcap 驱动后重试）");
            return false;
        }

        // ifname 是 QNetworkInterface::name()，在 Win 上可能是 LUID 名而非 GUID —— 必须反查，
        // 详见 adapterFor 的注释。
        const AdapterInfo adapter = adapterFor(ifname);
        if (!adapter.ok) {
            if (err)
                *err = QStringLiteral("系统里找不到网卡 %1（已按 LUID 名/友好名/GUID 三种口径查过）")
                               .arg(ifname);
            return false;
        }

        // ★ 非混杂模式打开（promisc=0）。这是「开代理后本机+设备一起断网」的根因修复：
        //   混杂模式下网卡把**所有**帧上交，Windows 自己的网络栈也会看到我们**单播发给
        //   设备/网关的 ARP 欺骗应答**（sha=本机MAC, spa=网关IP），于是把本机 ARP 缓存也更新成
        //   「网关 → 本机MAC」——本机自投毒，出网流量指回自己 → 本机断网；mihomo 替设备转发
        //   也走同一条已中毒的默认路由 → 被代理设备一起断。两台物理机确定性复现，正是这个。
        //   而这套「先投毒、再抓设备发来的帧」的透明网关**根本不需要混杂**：设备被投毒后，
        //   它发往「网关」的帧目的 MAC 就是本机 MAC，本机网卡与 Npcap 都会正常收到；广播的
        //   ARP 请求也照收。开混杂只是白抓无用帧、还顺手坑了本机。
        //   应急开关：COAST_GATEWAY_PROMISC=1 可强制回到混杂模式（仅调试/排查用）。
        const int promisc = qEnvironmentVariableIsSet("COAST_GATEWAY_PROMISC") ? 1 : 0;
        dbg("open dev=%s promisc=%d", adapter.npfName.constData(), promisc);
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        m_pcap = m_api->open_live(adapter.npfName.constData(), 65536, promisc, 1 /*ms*/, errbuf);
        if (!m_pcap) {
            // errbuf 里是 Windows 本地化的系统错误文案（zh-CN 下是 GBK），fromLatin1 会拧成乱码。
            const QString raw = QString::fromLocal8Bit(errbuf);
            // 走到这里 wpcap.dll 已经加载成功 = Npcap 装着的，所以旧文案那句「需装 Npcap」必然是
            // 误导。这里按「驱动是否被锁 × 本进程是否提权」分三种情况给出真正能照着做的下一步，
            // 并且**永远**把这两个事实附在末尾——这一带的问题隔着截图很难猜，state 直接写出来
            // 就不用再来回问「你是管理员账户还是右键以管理员身份运行的」。
            const bool locked = npcapstatus::adminOnly();
            const bool elevated = npcapstatus::processElevated();
            const QString state =
                    QStringLiteral("[AdminOnly=%1, 本进程%2提权]")
                            .arg(locked ? QStringLiteral("1") : QStringLiteral("0"),
                                 elevated ? QStringLiteral("已") : QStringLiteral("未"));
            if (err) {
                if (locked && !elevated) {
                    // 绝大多数「总是打开网卡失败」都落在这里：账户是管理员 ≠ 进程被提权，
                    // UAC 开着时双击启动拿到的是受限令牌，照样被 Npcap 挡在门外。
                    *err = QStringLiteral("Npcap 驱动被限制为「仅管理员可访问」（安装时勾选了 "
                                          "Restrict Npcap driver's access to Administrators only）"
                                          "——请在设备页点「修复权限」一键解除。原始错误: ")
                            + raw + QLatin1Char(' ') + state;
                } else if (locked) {
                    *err = QStringLiteral("已提权但仍打不开二层设备——若刚改过 Npcap 权限，"
                                          "需重启 npcap 驱动或重启电脑才生效。原始错误: ")
                            + raw + QLatin1Char(' ') + state;
                } else {
                    *err = QStringLiteral("打不开二层设备: ") + raw + QLatin1Char(' ') + state;
                }
            }
            return false;
        }
        // 先加大驱动内核收缓冲，再武装通知器——必须在开始收包前设好（见 kRxKernelBufBytes）。
        // pcap_setbuff 会清空当前缓冲，此刻还没开始收，无副作用；失败不致命（只是少了这层余量）。
        if (m_api->setbuff && m_api->setbuff(m_pcap, kRxKernelBufBytes) != 0)
            dbg("pcap_setbuff(%d) 失败，沿用驱动默认收缓冲", kRxKernelBufBytes);
        m_api->setmintocopy(m_pcap, 1); // 有 1 字节就触发事件，降低延迟
        m_localMac = adapter.mac;

        HANDLE h = m_api->getevent(m_pcap);
        if (!h) {
            if (err) *err = QStringLiteral("pcap_getevent 返回空句柄");
            close();
            return false;
        }
        m_notifier = new QWinEventNotifier(h, this);
        QObject::connect(m_notifier, &QWinEventNotifier::activated, this,
                         [this](HANDLE) { drain(); });
        m_notifier->setEnabled(true);
        dbg("open ok, notifier armed, localMac=%s", m_localMac.toHex(':').constData());
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
            m_api->close(m_pcap); // m_pcap 非空 ⇒ open 成功过 ⇒ m_api 必然有效
            m_pcap = nullptr;
        }
    }
    bool isOpen() const override { return m_pcap != nullptr; }

    // 发一帧。pcap_sendpacket 是**同步**的：把帧写进驱动 TX 缓冲，成功返 0、失败返 -1。
    // 没有 EAGAIN/可写事件那套语义，所以这里不做「满了排队等可写」——做不了（无可写通知），
    // 也不必（驱动 TX 满时 WriteFile 会阻塞而非丢）。失败基本都是持久性错误（网卡拔了/句柄失效），
    // 排队重发无意义，直接计数丢弃、交由 lwIP 重传兜底。计数与 linux/mac 端点对称，接进 GatewayDiag。
    bool send(const QByteArray &frame) override
    {
        if (!m_pcap)
            return false;
        if (m_api->sendpacket(m_pcap, reinterpret_cast<const u_char *>(frame.constData()),
                              frame.size())
            == 0) {
            ++GatewayDiag::c.txFrames;
            GatewayDiag::c.txBytes += frame.size();
            return true;
        }
        ++GatewayDiag::c.txDropped;
        reportTxDropThrottled();
        return false;
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
            QByteArray macClause;
            for (const QByteArray &m : macs) {
                if (m.size() != 6)
                    continue;
                if (!macClause.isEmpty())
                    macClause += " or ";
                macClause += "ether src " + macToText(m);
            }
            if (macClause.isEmpty())
                return false; // 一个合法 MAC 都没有：不装，交用户态兜底
            // 末尾「或 arp」:必须捕获 ARP——尤其真网关广播的 who-has(携带「网关在真 MAC」会把设备
            // 解毒),LanGateway 收到后立刻反制重投。ARP 低频,全收几乎零成本。契约见 IL2Endpoint.h。
            expr = "(" + macClause + ") or arp";
        }

        struct bpf_program prog;
        // optimize=1；netmask 用 PCAP_NETMASK_UNKNOWN（表达式里不含 ip broadcast，用不到掩码）。
        if (m_api->compile(m_pcap, &prog, expr.constData(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
            dbg("filter compile FAILED expr=%s (回退用户态兜底)", expr.constData());
            return false; // 编译失败：不装内核过滤，用户态照旧兜底
        }
        const int rc = m_api->setfilter(m_pcap, &prog);
        m_api->freecode(&prog);
        dbg("filter set rc=%d expr=%s", rc, expr.constData());
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

    void reportTxDropThrottled()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastTxDropReportMs < kDropReportMinIntervalMs)
            return;
        m_lastTxDropReportMs = now;
        qWarning().noquote()
            << "L2Endpoint(Npcap): 发帧失败累计" << GatewayDiag::c.txDropped
            << "帧（pcap_sendpacket 返回错误——网卡异常/句柄失效）。本机→设备方向丢帧会让被代理"
               "设备「时好时坏 / 极慢」。";
    }

    void drain()
    {
        ++GatewayDiag::c.rxWakes; // 与 rxFrames 之比 = 每次唤醒取回多少帧（批处理效率）
        struct pcap_pkthdr *hdr = nullptr;
        const u_char *data = nullptr;
        int r;
        // 1=拿到一帧；0=本轮无更多(超时)；<0=出错。循环取尽本次事件的所有帧。
        while (m_pcap && (r = m_api->next_ex(m_pcap, &hdr, &data)) == 1) {
            if (data && hdr) {
                ++GatewayDiag::c.rxFrames;
                GatewayDiag::c.rxBytes += hdr->caplen;
                // 诊断：过滤器已把「非被劫持设备」的帧挡在内核里，所以这里出现的每一帧都
                // 应当来自某台被代理设备。前 20 帧逐帧打印源/目的 MAC + ethertype，之后每
                // 200 帧报一次计数——用来判定「设备的帧到底有没有进到这一层」。
                if (dbgOn() && hdr->caplen >= 14) {
                    const u_char *f = data;
                    if (m_rxLogged < 20) {
                        ++m_rxLogged;
                        dbg("rx#%lld len=%u dst=%02x:%02x:%02x:%02x:%02x:%02x "
                            "src=%02x:%02x:%02x:%02x:%02x:%02x eth=%02x%02x",
                            m_rxCount + 1, hdr->caplen, f[0], f[1], f[2], f[3], f[4], f[5],
                            f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
                    } else if ((m_rxCount % 200) == 0) {
                        dbg("rx total=%lld", m_rxCount + 1);
                    }
                }
                ++m_rxCount;
                emit frameReceived(QByteArray(reinterpret_cast<const char *>(data), hdr->caplen));
            }
        }
    }

    const PcapApi *m_api = nullptr; // open() 里解析；进程级单例，不属本对象所有
    pcap_t *m_pcap = nullptr;
    QWinEventNotifier *m_notifier = nullptr;
    QByteArray m_localMac;
    long long m_rxCount = 0; // 收到的帧总数（诊断用）
    int m_rxLogged = 0;      // 已逐帧打印的条数（封顶 20）
    qint64 m_lastTxDropReportMs = -kDropReportMinIntervalMs; // 发帧失败告警节流
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new WinL2Endpoint(parent);
}

#endif // _WIN32
