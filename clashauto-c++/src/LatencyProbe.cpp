#include "LatencyProbe.h"

#include "ClashService.h"

#include <QDnsLookup>
#include <QElapsedTimer>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

namespace {
// 直连探测目标：公共 DNS 的 53 口。选它是因为**不需要先解析域名**（拿域名当目标的话，测到的
// 是「DNS + 握手」，DNS 那一项就重复计了），而且它在国内外都通，端口常年开着。
constexpr const char *kDirectHost = "223.5.5.5"; // AliDNS
constexpr quint16 kDirectPort = 53;

// DNS 探测轮换这几个域名：同一个名字连着解析会被系统缓存直接答掉，测出来永远是 0ms。
// 换着来至少能让一部分请求真的出网。
const char *const kDnsNames[] = {"www.qq.com", "www.taobao.com", "www.bilibili.com",
                                 "www.jd.com", "www.zhihu.com"};
constexpr int kDnsNameCount = 5;
} // namespace

LatencyProbe::LatencyProbe(ClashService *clash, QObject *parent) : QObject(parent), m_clash(clash)
{
    if (m_clash) {
        // 「到代理的延迟」直接吃核心的测速结果：找到当前选中的那条，取它的 delay。
        connect(m_clash, &ClashService::nodesUpdated, this,
                [this](const QVector<NodeInfo> &nodes, const QString &selected) {
                    const int before = m_proxy;
                    const QString beforeName = m_proxyName;
                    m_proxyName = selected;
                    m_proxy = -1;
                    for (const NodeInfo &n : nodes) {
                        if (n.name == selected) {
                            // delay<=0 = 核心还没测出来/测失败，按未知处理，别显示成 0ms
                            m_proxy = n.delay > 0 ? n.delay : -1;
                            break;
                        }
                    }
                    if (m_proxy != before || m_proxyName != beforeName)
                        emit changed();
                });
    }

    m_timer = new QTimer(this);
    m_timer->setInterval(kIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &LatencyProbe::probe);
}

void LatencyProbe::setGatewayIp(const QString &ip)
{
    if (ip == m_gatewayIp)
        return;
    m_gatewayIp = ip;
    if (m_gatewayIp.isEmpty()) {
        m_router = -1;
        emit changed();
    }
}

void LatencyProbe::setActive(bool active)
{
    if (active == m_timer->isActive())
        return;
    if (active) {
        probe(); // 切到状态页立刻给一轮，别让用户对着上次的旧数
        m_timer->start();
    } else {
        m_timer->stop();
    }
}

void LatencyProbe::probe()
{
    if (m_pending > 0)
        return; // 上一轮还没回全：跳过这一拍，免得 socket 越堆越多

    probeTcp(QString::fromLatin1(kDirectHost), kDirectPort, &m_direct);
    if (!m_gatewayIp.isEmpty())
        probeTcp(m_gatewayIp, 80, &m_router); // 路由器的 Web 口；关着也能测（见头文件）
    probeDns();
    emit changed(); // probing 转真
}

// TCP 握手 RTT。connected 与 ConnectionRefused 都算测到了（RST 和 SYN-ACK 走同一条路）；
// 超时/不可达算失败。socket 用 deleteLater 自杀，调用方不持有。
void LatencyProbe::probeTcp(const QString &host, quint16 port, int *slot)
{
    auto *sock = new QTcpSocket(this);
    sock->setProxy(QNetworkProxy::NoProxy); // 必须：否则开着系统代理时测的是到代理的延迟
    // ★ clock/done 必须是 shared_ptr，不能裸 new/delete。settle 里的 abort() 会**同步**再发一
    //   次信号（连接还在途时 QAbstractSocket::abort 立刻发 errorOccurred；QDnsLookup::abort 同理
    //   发 finished）→ 另一份 settle 副本被**重入**。老写法在 abort() 之前就 delete 了 done/clock，
    //   重入那次读的是已释放内存：读到非 0 侥幸早退，读到 0 就当作「还没结算过」一路走到底，再
    //   delete 一遍 → glibc "free(): double free detected" → 整个进程 SIGABRT。
    //   真机实证（树莓派）：网络不通/DNS 超时时必崩，而那正是**开机那几十秒**的常态；Coast 一崩，
    //   被代理设备的 ARP 就没人还原、也没人再转发 → 设备彻底断网。交给引用计数后谁都不用删。
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto done = std::make_shared<bool>(false);

    auto settle = [this, sock, clock, done, slot](int ms) {
        if (*done)
            return;
        *done = true;
        *slot = ms;
        sock->abort();
        sock->deleteLater();
        finishOne();
    };

    connect(sock, &QTcpSocket::connected, this,
            [settle, clock] { settle(int(clock->elapsed())); });
    connect(sock, &QTcpSocket::errorOccurred, this,
            [settle, clock](QAbstractSocket::SocketError e) {
                // 端口关着 = 对端立刻回 RST，这依然是一次有效的往返计时。
                settle(e == QAbstractSocket::ConnectionRefusedError ? int(clock->elapsed()) : -1);
            });
    QTimer::singleShot(kTimeoutMs, sock, [settle] { settle(-1); });

    ++m_pending;
    sock->connectToHost(host, port);
}

void LatencyProbe::probeDns()
{
    auto *lookup = new QDnsLookup(QDnsLookup::A,
                                  QString::fromLatin1(kDnsNames[m_dnsNameIndex]), this);
    m_dnsNameIndex = (m_dnsNameIndex + 1) % kDnsNameCount;
    // 同 probeTcp：abort() 会同步再发一次 finished → 重入 settle，裸指针必然二次 delete。
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto done = std::make_shared<bool>(false);

    auto settle = [this, lookup, clock, done](int ms) {
        if (*done)
            return;
        *done = true;
        m_dns = ms;
        lookup->abort();
        lookup->deleteLater();
        finishOne();
    };

    connect(lookup, &QDnsLookup::finished, this, [settle, clock, lookup] {
        // NXDOMAIN/SERVFAIL 也是一次完整往返，照样计时；只有超时/没网才算失败。
        settle(lookup->error() == QDnsLookup::TimeoutError ? -1 : int(clock->elapsed()));
    });
    QTimer::singleShot(kTimeoutMs, lookup, [settle] { settle(-1); });

    ++m_pending;
    lookup->lookup();
}

void LatencyProbe::finishOne()
{
    if (m_pending > 0)
        --m_pending;
    emit changed();
}
