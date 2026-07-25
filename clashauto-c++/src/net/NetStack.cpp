#include "NetStack.h"
#include "IL2Endpoint.h"
#include "Socks5Client.h"

#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QTimer>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

extern "C" {
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
}

// lwIP(NO_SYS) 需要单调毫秒时钟。
extern "C" u32_t sys_now(void)
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return static_cast<u32_t>(duration_cast<milliseconds>(steady_clock::now() - t0).count());
}

namespace {

struct DeviceInfo {
    QByteArray mac6;
    QString socksUser;
};

// 一条被终结的 TCP 连接：lwIP pcb ↔ 到 mihomo 的 Socks5Tcp。
struct TcpConn {
    NetStack::Impl *impl = nullptr;
    struct tcp_pcb *pcb = nullptr;
    Socks5Tcp *socks = nullptr;
    QByteArray toLwip;    // socks→设备 方向待写入 lwIP 的字节（受 tcp_sndbuf 限流）
    bool established = false;
    bool lwipClosed = false;
};

} // namespace

// 一张网卡对应的 netif 上下文。指针存进 netif->state，C 回调据此拿到「该从哪个二层端点发出去」
// 和「本机在这张卡上的 MAC」——多网卡就是靠它区分的。
struct NetStack::Nic {
    struct netif nif;          // 必须是稳定地址：Nic 一律堆分配，不放进会搬家的容器里
    IL2Endpoint *ep = nullptr;
    QByteArray localMac6;
    QString localIp, netmask;
};

namespace {

// 一台设备的 UDP 会话（含 DNS）：一个 Socks5Udp + 简易 NAT（记住每个目标对应的设备源端口）。
struct UdpSess {
    Socks5Udp *socks = nullptr;
    QByteArray mac6;
    QString victimIp;
    NetStack::Nic *nic = nullptr;           // 该设备所在网卡：回程包从这张卡、用这张卡的 MAC 发
    bool ready = false;
    QList<QByteArray> pending;              // ready 前暂存的 (dstIp,dport,payload) 打包
    QHash<QString, quint16> nat;            // "dstIp:dport" → 设备源端口(vport)
};

} // namespace

struct NetStack::Impl {
    quint16 socksPort = 0;
    NetStack *owner = nullptr;
    struct tcp_pcb *listener = nullptr;
    QHash<IL2Endpoint *, Nic *> nics;        // 二层端点 → 该网卡的 netif 上下文
    QHash<QString, DeviceInfo> devices;      // 设备 IP → {mac,user}
    QHash<QString, UdpSess *> udp;           // 设备 IP → UDP 会话
    QTimer *timer = nullptr;
    bool inited = false;

    QString userForIp(const QString &ip) const { return devices.value(ip).socksUser; }
    QByteArray macForIp(const QString &ip) const { return devices.value(ip).mac6; }
};

// ———————————————————————————— 前置：C 回调 ————————————————————————————
namespace {

// lwIP 本身只能有一个实例（lwip_init 全局、ARP 表全局、PCB 链全局），所以「栈」确实是单例：
// g_impl 只用来给 TCP accept 这类**与网卡无关**的全局回调取上下文（监听 pcb、设备表、socks 端口）。
// 与网卡相关的东西（二层端点、本机 MAC）一律从 netif->state 取，不走这里——多网卡的关键。
NetStack::Impl *g_impl = nullptr;
bool g_debug = false;             // COAST_GATEWAY_DEBUG=1 时打诊断日志（自测/联调用）

NetStack::Nic *nicOf(struct netif *netif)
{
    return netif ? static_cast<NetStack::Nic *>(netif->state) : nullptr;
}

// pbuf 链 → QByteArray
QByteArray pbufToBytes(struct pbuf *p)
{
    QByteArray out;
    out.reserve(p->tot_len);
    for (struct pbuf *q = p; q != nullptr; q = q->next) {
        out.append(static_cast<const char *>(q->payload), q->len);
        if (q->tot_len == q->len)
            break;
    }
    return out;
}

// netif linkoutput：lwIP 要发一帧 → 序列化 → 从**该 netif 自己的**二层端点发出
//（dst MAC 已由静态 ARP 填好）。多网卡就靠这里分流：绝不能回到某个全局端点上。
err_t lwipLinkOutput(struct netif *netif, struct pbuf *p)
{
    NetStack::Nic *nic = nicOf(netif);
    if (nic && nic->ep) {
        const QByteArray f = pbufToBytes(p);
        if (g_debug)
            std::fprintf(stderr, "NETSTACK OUT nic=%s len=%d\n",
                         nic->localIp.toLatin1().constData(), int(f.size()));
        nic->ep->send(f);
    }
    return ERR_OK;
}

err_t lwipNetifInit(struct netif *netif)
{
    netif->name[0] = 'c';
    netif->name[1] = 't';
    netif->output = etharp_output;      // IP 出口经 ARP（静态表已填，不会真发 ARP 请求）
    netif->linkoutput = lwipLinkOutput; // 二层出口
    netif->mtu = 1500;
    netif->hwaddr_len = 6;
    if (NetStack::Nic *nic = nicOf(netif))
        std::memcpy(netif->hwaddr, nic->localMac6.constData(), 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET
                   | NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
    return ERR_OK;
}

void closeConn(TcpConn *c, bool abort);

// 把 socks→设备方向的待写字节尽量写进 lwIP（受发送窗口限流），tcp_sent 回调后再续。
void pumpToLwip(TcpConn *c)
{
    if (!c || !c->pcb || c->lwipClosed)
        return;
    while (!c->toLwip.isEmpty()) {
        const u16_t space = tcp_sndbuf(c->pcb);
        if (space == 0)
            break;
        const int n = qMin<int>(space, c->toLwip.size());
        const err_t e = tcp_write(c->pcb, c->toLwip.constData(), n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM)
            break;        // 缓冲不足，等 tcp_sent 再来
        if (e != ERR_OK) {
            closeConn(c, true);
            return;
        }
        c->toLwip.remove(0, n);
    }
    tcp_output(c->pcb);
}

void closeConn(TcpConn *c, bool abort)
{
    if (!c)
        return;
    if (c->pcb && !c->lwipClosed) {
        tcp_arg(c->pcb, nullptr);
        tcp_recv(c->pcb, nullptr);
        tcp_sent(c->pcb, nullptr);
        tcp_err(c->pcb, nullptr);
        if (abort)
            tcp_abort(c->pcb);
        else if (tcp_close(c->pcb) != ERR_OK)
            tcp_abort(c->pcb);
        c->lwipClosed = true;
    }
    c->pcb = nullptr;
    if (c->socks) {
        c->socks->closeTunnel();
        c->socks->deleteLater();
        c->socks = nullptr;
    }
    delete c;
}

// 设备→服务器 方向（lwIP 收到设备数据）→ 写给 socks。
err_t lwipTcpRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    auto *c = static_cast<TcpConn *>(arg);
    if (!c)
        return ERR_OK;
    if (err != ERR_OK) {
        if (p)
            pbuf_free(p);
        closeConn(c, true);
        return ERR_OK;
    }
    if (p == nullptr) {           // 设备侧关闭 → 关 socks 上行
        if (c->socks)
            c->socks->closeTunnel();
        return ERR_OK;
    }
    const QByteArray data = pbufToBytes(p);
    const u16_t len = p->tot_len;
    if (c->socks)
        c->socks->write(data);    // Socks5Tcp 内部会缓冲直到 established
    tcp_recved(pcb, len);
    pbuf_free(p);
    return ERR_OK;
}

err_t lwipTcpSent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    Q_UNUSED(pcb);
    Q_UNUSED(len);
    pumpToLwip(static_cast<TcpConn *>(arg));
    return ERR_OK;
}

void lwipTcpErr(void *arg, err_t err)
{
    Q_UNUSED(err);
    auto *c = static_cast<TcpConn *>(arg);
    if (!c)
        return;
    c->pcb = nullptr;          // lwIP 已释放 pcb
    c->lwipClosed = true;
    if (c->socks) {
        c->socks->closeTunnel();
        c->socks->deleteLater();
        c->socks = nullptr;
    }
    delete c;
}

// 新 SYN 被 catch-all 监听接受：local_ip/port = 设备想访问的服务器；remote = 设备。
err_t lwipTcpAccept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    Q_UNUSED(arg);
    if (err != ERR_OK || newpcb == nullptr)
        return ERR_VAL;
    if (g_impl && g_impl->listener)
        tcp_accepted(g_impl->listener);

    const QString serverIp = QString::fromLatin1(ipaddr_ntoa(&newpcb->local_ip));
    const quint16 serverPort = newpcb->local_port;
    const QString victimIp = QString::fromLatin1(ipaddr_ntoa(&newpcb->remote_ip));
    const QString user = g_impl ? g_impl->userForIp(victimIp) : QString();
    if (g_debug)
        std::fprintf(stderr, "NETSTACK ACCEPT server=%s:%u victim=%s user=%s\n",
                     serverIp.toLatin1().constData(), serverPort,
                     victimIp.toLatin1().constData(), user.toLatin1().constData());

    auto *c = new TcpConn;
    c->impl = g_impl;
    c->pcb = newpcb;
    c->socks = new Socks5Tcp(g_impl->owner);

    tcp_arg(newpcb, c);
    tcp_recv(newpcb, lwipTcpRecv);
    tcp_sent(newpcb, lwipTcpSent);
    tcp_err(newpcb, lwipTcpErr);
    tcp_nagle_disable(newpcb);

    QObject::connect(c->socks, &Socks5Tcp::established, g_impl->owner, [c]() {
        c->established = true;
    });
    QObject::connect(c->socks, &Socks5Tcp::dataReceived, g_impl->owner, [c](const QByteArray &d) {
        c->toLwip.append(d);
        pumpToLwip(c);
    });
    QObject::connect(c->socks, &Socks5Tcp::failed, g_impl->owner, [c](const QString &) {
        closeConn(c, true);
    });
    QObject::connect(c->socks, &Socks5Tcp::closed, g_impl->owner, [c]() {
        // socks 关闭：把剩余下行写完后优雅关闭 lwIP 侧。
        if (c->pcb && !c->lwipClosed && c->toLwip.isEmpty())
            closeConn(c, false);
    });

    c->socks->connectTo(g_impl->socksPort, serverIp, serverPort, user);
    return ERR_OK;
}

// —— UDP 手工封包辅助 ——
quint16 ipChecksum(const uchar *data, int len)
{
    quint32 sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += (quint32(data[i]) << 8) | data[i + 1];
    if (len & 1)
        sum += quint32(data[len - 1]) << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<quint16>(~sum);
}

} // namespace

// ———————————————————————————— NetStack ————————————————————————————
NetStack::NetStack(quint16 socksPort, QObject *parent)
    : QObject(parent), d(new Impl)
{
    d->socksPort = socksPort;
    d->owner = this;
}

NetStack::~NetStack()
{
    for (UdpSess *s : d->udp) {
        if (s->socks)
            s->socks->closeSession();
        delete s;
    }
    for (Nic *n : d->nics) {
        if (d->inited)
            netif_remove(&n->nif);
        delete n;
    }
    if (g_impl == d)
        g_impl = nullptr;
    delete d;
}

bool NetStack::init(QString *err)
{
    if (d->inited)
        return true;
    if (g_impl && g_impl != d) {
        if (err)
            *err = QStringLiteral("已有一个网关协议栈实例在运行");
        return false;
    }
    g_impl = d;
    g_debug = qEnvironmentVariableIsSet("COAST_GATEWAY_DEBUG");

    lwip_init();

    // catch-all TCP 监听：绑任意 IP + 端口 0（配合 tcp_in.c 补丁通配任意目的端口）。
    // 监听是全局的、与网卡无关：哪张卡进来的 SYN 都命中它，accept 里再按设备 IP 查身份。
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        g_impl = nullptr;
        if (err)
            *err = QStringLiteral("tcp_new 失败");
        return false;
    }
    tcp_bind(pcb, IP_ADDR_ANY, 0); // 绑任意 IP + 端口 0（配合 tcp_in.c 补丁通配任意目的端口）
    d->listener = tcp_listen_with_backlog(pcb, TCP_DEFAULT_LISTEN_BACKLOG);
    if (!d->listener) {
        tcp_close(pcb);
        g_impl = nullptr;
        if (err)
            *err = QStringLiteral("tcp_listen 失败");
        return false;
    }
    d->listener->local_port = 0; // 通配任意目的端口
    tcp_accept(d->listener, lwipTcpAccept);

    // lwIP 定时器泵（TCP 重传/超时/ARP 老化）。
    d->timer = new QTimer(this);
    d->timer->setInterval(200);
    connect(d->timer, &QTimer::timeout, this, [] { sys_check_timeouts(); });
    d->timer->start();

    d->inited = true;
    return true;
}

bool NetStack::addNic(IL2Endpoint *ep, const QByteArray &localMac6, const QString &localIp,
                      const QString &netmask, QString *err)
{
    if (!d->inited) {
        if (err)
            *err = QStringLiteral("协议栈未初始化");
        return false;
    }
    if (!ep || localMac6.size() != 6) {
        if (err)
            *err = QStringLiteral("网卡端点或本机 MAC 非法");
        return false;
    }
    if (d->nics.contains(ep))
        return true;

    // netif 的 IP/掩码用**本机在这张卡上的真实地址**，不能再用早期的 0.0.0.1 // 0.0.0.0 占位：
    //  · ip4_route 会跳过 IP 为 0.0.0.0 的 netif（回包找不到出口而被丢），所以 IP 必须非零；
    //  · 掩码若给 0.0.0.0，该 netif「匹配一切」，多网卡时 ip4_route 永远返回链表第一张 →
    //    B 网段设备的回包会从 A 网卡发出去。用真实掩码，出方向按子网各归各的。
    ip4_addr_t nip, nmask, ngw;
    if (!ip4addr_aton(localIp.toLatin1().constData(), &nip) || ip4_addr_isany_val(nip)) {
        if (err)
            *err = QStringLiteral("本机 IP 非法: ") + localIp;
        return false;
    }
    if (!ip4addr_aton(netmask.toLatin1().constData(), &nmask) || ip4_addr_isany_val(nmask)) {
        if (err)
            *err = QStringLiteral("子网掩码非法: ") + netmask;
        return false;
    }
    ip4_addr_set_zero(&ngw);

    auto *nic = new Nic;
    nic->ep = ep;
    nic->localMac6 = localMac6;
    nic->localIp = localIp;
    nic->netmask = netmask;
    // state 必须在 netif_add 之前就绪：lwipNetifInit 里要用它填 hwaddr。
    if (netif_add(&nic->nif, &nip, &nmask, &ngw, nic, lwipNetifInit, ethernet_input) == nullptr) {
        delete nic;
        if (err)
            *err = QStringLiteral("netif_add 失败");
        return false;
    }
    if (d->nics.isEmpty())
        netif_set_default(&nic->nif); // ip4_route 无匹配时的兜底出口
    netif_set_up(&nic->nif);
    netif_set_link_up(&nic->nif);
    d->nics.insert(ep, nic);
    return true;
}

void NetStack::removeNic(IL2Endpoint *ep)
{
    Nic *nic = d->nics.take(ep);
    if (!nic)
        return;
    if (d->inited)
        netif_remove(&nic->nif);
    // 该卡上的 UDP 会话失去出口，一并收掉（设备重新发包会重建）。
    const QStringList victims = d->udp.keys();
    for (const QString &ip : victims) {
        UdpSess *s = d->udp.value(ip);
        if (s && s->nic == nic) {
            if (s->socks) {
                s->socks->closeSession();
                s->socks->deleteLater();
            }
            d->udp.remove(ip);
            delete s;
        }
    }
    delete nic;
}

bool NetStack::hasNic(IL2Endpoint *ep) const
{
    return d->nics.contains(ep);
}

void NetStack::addDevice(const QString &ip, const QByteArray &mac6, const QString &socksUser)
{
    if (ip.isEmpty() || mac6.size() != 6)
        return;
    d->devices.insert(ip, DeviceInfo{mac6, socksUser});
    if (d->inited) {
        // 预置静态 ARP：lwIP 回包给设备时直接用其 MAC，不发 ARP 请求。
        ip4_addr_t a;
        if (ip4addr_aton(ip.toLatin1().constData(), &a)) {
            struct eth_addr e;
            std::memcpy(e.addr, mac6.constData(), 6);
            etharp_add_static_entry(&a, &e);
        }
    }
}

void NetStack::removeDevice(const QString &ip)
{
    d->devices.remove(ip);
    if (auto *s = d->udp.take(ip)) {
        if (s->socks) {
            s->socks->closeSession();
            s->socks->deleteLater();
        }
        delete s;
    }
    if (d->inited) {
        ip4_addr_t a;
        if (ip4addr_aton(ip.toLatin1().constData(), &a))
            etharp_remove_static_entry(&a);
    }
}

void NetStack::inputFrame(IL2Endpoint *from, const QByteArray &frame)
{
    if (!d->inited || frame.size() < 14)
        return;
    Nic *nic = d->nics.value(from);
    if (!nic)
        return; // 帧来自一张没挂上来的卡（正在重配/已摘除）
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    const quint16 ethType = (quint16(f[12]) << 8) | f[13];
    if (g_debug) {
        const int proto = (frame.size() >= 24) ? f[14 + 9] : -1;
        std::fprintf(stderr, "NETSTACK IN nic=%s len=%d eth=%04x proto=%d\n",
                     nic->localIp.toLatin1().constData(), int(frame.size()), ethType, proto);
    }

    // 仅处理 IPv4；ARP 等不喂 lwIP（ARP 投毒由 ArpSpoofer 负责，避免 lwIP 误答）。
    if (ethType != 0x0800)
        return;
    if (frame.size() < 14 + 20)
        return;
    const uchar *ip = f + 14;
    const int ihl = (ip[0] & 0x0F) * 4;
    const quint8 proto = ip[9];

    if (proto == 17) { // UDP：手工拦截转发（含 DNS）
        handleUdpFrame(nic, frame, ihl);
        return;
    }
    // TCP/ICMP 等交给 lwIP —— 注入**收到它的那个 netif**（accept-all 补丁会把无主单播收到 inp 上）。
    struct pbuf *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(frame.size()), PBUF_POOL);
    if (!p)
        return;
    pbuf_take(p, frame.constData(), static_cast<u16_t>(frame.size()));
    if (nic->nif.input(p, &nic->nif) != ERR_OK)
        pbuf_free(p);
}

// ———————————————————————————— UDP 拦截 ————————————————————————————
void NetStack::handleUdpFrame(Nic *nic, const QByteArray &frame, int ihl)
{
    const uchar *f = reinterpret_cast<const uchar *>(frame.constData());
    const uchar *ip = f + 14;
    if (14 + ihl + 8 > frame.size())
        return;
    const QString srcIp = QString("%1.%2.%3.%4").arg(ip[12]).arg(ip[13]).arg(ip[14]).arg(ip[15]);
    const QString dstIp = QString("%1.%2.%3.%4").arg(ip[16]).arg(ip[17]).arg(ip[18]).arg(ip[19]);
    const uchar *udp = ip + ihl;
    const quint16 sport = (quint16(udp[0]) << 8) | udp[1];
    const quint16 dport = (quint16(udp[2]) << 8) | udp[3];
    const int ulen = (quint16(udp[4]) << 8) | udp[5];
    if (ulen < 8 || 14 + ihl + ulen > frame.size())
        return;
    const QByteArray payload = frame.mid(14 + ihl + 8, ulen - 8);

    const DeviceInfo dev = d->devices.value(srcIp);
    if (dev.mac6.size() != 6)
        return;

    UdpSess *s = d->udp.value(srcIp);
    if (!s) {
        s = new UdpSess;
        s->mac6 = dev.mac6;
        s->victimIp = srcIp;
        s->nic = nic; // 回程包要从这张卡发、用这张卡的本机 MAC 当源
        s->socks = new Socks5Udp(this);
        d->udp.insert(srcIp, s);
        connect(s->socks, &Socks5Udp::ready, this, [s]() {
            s->ready = true;
            for (const QByteArray &pk : std::as_const(s->pending)) {
                // pending 打包格式：[dstIp\0 dport(2) payload]
                const int z = pk.indexOf('\0');
                const QString di = QString::fromLatin1(pk.left(z));
                const quint16 dp = (quint16(uchar(pk[z + 1])) << 8) | uchar(pk[z + 2]);
                s->socks->sendTo(QHostAddress(di), dp, pk.mid(z + 3));
            }
            s->pending.clear();
        });
        connect(s->socks, &Socks5Udp::datagramReceived, this,
                [this, srcIp](const QHostAddress &fromIp, quint16 fromPort, const QByteArray &data) {
                    onUdpResponse(srcIp, fromIp, fromPort, data);
                });
        s->socks->associate(d->socksPort, dev.socksUser);
    }

    s->nat.insert(dstIp + ':' + QString::number(dport), sport); // 记住回程该发到哪个设备端口
    if (s->ready) {
        s->socks->sendTo(QHostAddress(dstIp), dport, payload);
    } else {
        QByteArray pk = dstIp.toLatin1();
        pk.append('\0');
        pk.append(char((dport >> 8) & 0xFF));
        pk.append(char(dport & 0xFF));
        pk.append(payload);
        s->pending.append(pk);
    }
}

void NetStack::onUdpResponse(const QString &victimIp, const QHostAddress &fromIp, quint16 fromPort,
                             const QByteArray &payload)
{
    UdpSess *s = d->udp.value(victimIp);
    if (!s || !s->nic)
        return;
    const quint16 vport = s->nat.value(fromIp.toString() + ':' + QString::number(fromPort), 0);
    if (vport == 0)
        return;
    // 手工封 UDP/IP/以太 回程包发给设备。
    const quint32 srcIp = fromIp.toIPv4Address();     // 服务器
    QHostAddress va(victimIp);
    const quint32 dstIp = va.toIPv4Address();          // 设备
    const int udpLen = 8 + payload.size();
    const int ipLen = 20 + udpLen;

    QByteArray frame(14 + ipLen, char(0));
    uchar *b = reinterpret_cast<uchar *>(frame.data());
    // 以太头
    std::memcpy(b, s->mac6.constData(), 6);                 // dst = 设备
    std::memcpy(b + 6, s->nic->localMac6.constData(), 6);   // src = 本机在该设备那张卡上的 MAC
    b[12] = 0x08; b[13] = 0x00;
    // IP 头
    uchar *ip = b + 14;
    ip[0] = 0x45; ip[1] = 0x00;
    ip[2] = (ipLen >> 8) & 0xFF; ip[3] = ipLen & 0xFF;
    ip[4] = 0; ip[5] = 0;
    ip[6] = 0x40; ip[7] = 0; // DF
    ip[8] = 64;  ip[9] = 17; // ttl, proto=UDP
    ip[10] = 0;  ip[11] = 0; // checksum 占位
    ip[12] = (srcIp >> 24) & 0xFF; ip[13] = (srcIp >> 16) & 0xFF;
    ip[14] = (srcIp >> 8) & 0xFF;  ip[15] = srcIp & 0xFF;
    ip[16] = (dstIp >> 24) & 0xFF; ip[17] = (dstIp >> 16) & 0xFF;
    ip[18] = (dstIp >> 8) & 0xFF;  ip[19] = dstIp & 0xFF;
    const quint16 ipck = ipChecksum(ip, 20);
    ip[10] = (ipck >> 8) & 0xFF; ip[11] = ipck & 0xFF;
    // UDP 头
    uchar *u = ip + 20;
    u[0] = (fromPort >> 8) & 0xFF; u[1] = fromPort & 0xFF;   // sport = 服务器端口
    u[2] = (vport >> 8) & 0xFF;    u[3] = vport & 0xFF;      // dport = 设备源端口
    u[4] = (udpLen >> 8) & 0xFF;   u[5] = udpLen & 0xFF;
    u[6] = 0; u[7] = 0; // checksum
    std::memcpy(u + 8, payload.constData(), payload.size());
    // UDP 校验和（含伪首部）
    QByteArray pseudo;
    pseudo.append(char((srcIp >> 24) & 0xFF)); pseudo.append(char((srcIp >> 16) & 0xFF));
    pseudo.append(char((srcIp >> 8) & 0xFF));  pseudo.append(char(srcIp & 0xFF));
    pseudo.append(char((dstIp >> 24) & 0xFF)); pseudo.append(char((dstIp >> 16) & 0xFF));
    pseudo.append(char((dstIp >> 8) & 0xFF));  pseudo.append(char(dstIp & 0xFF));
    pseudo.append(char(0)); pseudo.append(char(17));
    pseudo.append(char((udpLen >> 8) & 0xFF)); pseudo.append(char(udpLen & 0xFF));
    pseudo.append(reinterpret_cast<const char *>(u), udpLen);
    quint16 uck = ipChecksum(reinterpret_cast<const uchar *>(pseudo.constData()), pseudo.size());
    if (uck == 0)
        uck = 0xFFFF;
    u[6] = (uck >> 8) & 0xFF; u[7] = uck & 0xFF;

    if (s->nic->ep)
        s->nic->ep->send(frame);
}
