// TUN 端点（macOS / utun）—— 把「本机全量流量」接进已有的用户态栈。
//
// 设计总纲（为什么伪装成 IL2Endpoint、合成 MAC 为什么固定）见 **TunEndpoint.h**。
// 一句话：读时给裸 IP 包前置 14 字节以太头、写时剥掉，上层 NetStack/lwIP 一行不改。
//
// —— macOS 与另外两个平台的**唯一实质差异**：每个包多一个 4 字节前缀 ——
//   utun 收发的不是裸 IP 包，而是「**网络序的地址族** + IP 包」。Linux 用 IFF_NO_PI 明确要了
//   无前缀、Windows/wintun 本来就是裸 IP，只有这里有。所以本文件的收发比另两份各多一步：
//     · 读：跳过 4 字节前缀，其余同 Linux；
//     · 写：先写 4 字节 htonl(AF_INET/AF_INET6)，再写 IP 包。
//   忘了这 4 字节的表现是「网卡建起来了、包也在动，但一个都解不出来」——所以下面用 iovec
//   一次 writev/readv，别拆成两次系统调用（拆开在非阻塞 fd 上可能只写进去半个包）。
//
// 网卡不是按名字创建的：utun 单元由内核分配，真实名字要用 UTUN_OPT_IFNAME 问回来。
// open() 传进来的 ifname 若形如 "utunN" 则申请该单元，否则（含空串）让内核挑第一个空闲的。
//
// 权限：建 utun 需要 **root**（configureIpv4 那步的 ifconfig/route 亦然）。拿不到就 open() 失败
// 并说明原因，由上层决定回退 mihomo 的 TUN。
//
// 本文件的系统调用序列已在真机（macOS 13.7.8 / x86_64）用等价的纯 C 驱动验过：
// utun 建立、内核投包、4 字节前缀为网络序 AF_INET、回程 write 成功。
#include "TunEndpoint.h"

#include "IL2Endpoint.h"

#include <QByteArray>
#include <QSocketNotifier>
#include <QString>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <net/if.h>
#include <net/if_utun.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {
constexpr int kEthHdr = coastcore::kTunEthHdr;
constexpr const unsigned char *kPeerMac = coastcore::kTunPeerMac;
constexpr const unsigned char *kLocalMac = coastcore::kTunLocalMac;
constexpr int kAfPrefix = 4; // utun 的地址族前缀
} // namespace

class TunEndpointMac final : public IL2Endpoint
{
public:
    explicit TunEndpointMac(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~TunEndpointMac() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        m_fd = ::socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
        if (m_fd < 0) {
            if (err)
                *err = QStringLiteral("socket(PF_SYSTEM) 失败：%1")
                               .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }

        struct ctl_info ci;
        std::memset(&ci, 0, sizeof(ci));
        std::strncpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof(ci.ctl_name) - 1);
        if (::ioctl(m_fd, CTLIOCGINFO, &ci) < 0) {
            fail(err, QStringLiteral("CTLIOCGINFO"), errno);
            return false;
        }

        // "utunN" → 单元号 N+1；其余（含空串）→ 0 = 让内核挑第一个空闲的。
        unsigned unit = 0;
        if (ifname.startsWith(QLatin1String("utun"))) {
            bool ok = false;
            const unsigned n = ifname.mid(4).toUInt(&ok);
            if (ok)
                unit = n + 1;
        }

        struct sockaddr_ctl sc;
        std::memset(&sc, 0, sizeof(sc));
        sc.sc_len = sizeof(sc);
        sc.sc_family = AF_SYSTEM;
        sc.ss_sysaddr = AF_SYS_CONTROL;
        sc.sc_id = ci.ctl_id;
        sc.sc_unit = unit;
        if (::connect(m_fd, reinterpret_cast<struct sockaddr *>(&sc), sizeof(sc)) < 0) {
            const int e = errno;
            fail(err,
                 QStringLiteral("connect(utun%1)%2")
                         .arg(unit ? QString::number(unit - 1) : QStringLiteral("<auto>"),
                              e == EPERM ? QStringLiteral("（需要 root）") : QString()),
                 e);
            return false;
        }

        // 名字由内核决定，必须问回来（上层配地址/路由要用）。
        char name[IFNAMSIZ + 8] = {0};
        socklen_t nl = sizeof(name);
        if (::getsockopt(m_fd, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, name, &nl) < 0) {
            fail(err, QStringLiteral("UTUN_OPT_IFNAME"), errno);
            return false;
        }
        m_ifname = QString::fromLatin1(name);
        m_ifIndex = int(::if_nametoindex(name));

        ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL, 0) | O_NONBLOCK);
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        connect(m_notifier, &QSocketNotifier::activated, this, [this] { drain(); });
        return true;
    }

    void close() override
    {
        if (m_notifier) {
            m_notifier->setEnabled(false);
            delete m_notifier;
            m_notifier = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd); // 关 fd 即销毁 utun 网卡
            m_fd = -1;
        }
    }
    bool isOpen() const override { return m_fd >= 0; }

    // 上层给的是完整以太帧：剥掉 14 字节以太头，换上 4 字节 AF 前缀写进 utun。
    // 用 writev 一次交付：非阻塞 fd 上拆成两次写有写进去半个包的风险。
    bool send(const QByteArray &frame) override
    {
        if (m_fd < 0 || frame.size() <= kEthHdr)
            return false;
        const char *ip = frame.constData() + kEthHdr;
        const qsizetype n = frame.size() - kEthHdr;
        const unsigned char ver = static_cast<unsigned char>(ip[0]) >> 4;
        const uint32_t af = htonl(ver == 6 ? uint32_t(AF_INET6) : uint32_t(AF_INET));
        struct iovec iov[2];
        iov[0].iov_base = const_cast<uint32_t *>(&af);
        iov[0].iov_len = kAfPrefix;
        iov[1].iov_base = const_cast<char *>(ip);
        iov[1].iov_len = size_t(n);
        const ssize_t w = ::writev(m_fd, iov, 2);
        return w == ssize_t(n) + kAfPrefix;
    }

    QByteArray localMac() const override
    {
        return QByteArray(reinterpret_cast<const char *>(kLocalMac), 6);
    }
    int ifIndex() const override { return m_ifIndex; }
    int mtu() const override { return 1500; }
    QString ifname() const { return m_ifname; }

private:
    // errno 必须由调用方**在出错的那一行之后立刻**取好传进来：从系统调用返回到这里，中间隔着
    // QString 构造等一堆东西，任何一处都可能覆写 errno，报出来的就成了另一个错误码。
    void fail(QString *err, const QString &what, int e)
    {
        if (err)
            *err = QStringLiteral("%1 失败：%2")
                           .arg(what, QString::fromLocal8Bit(std::strerror(e)));
        ::close(m_fd);
        m_fd = -1;
    }

    // 读一批包：跳过 4 字节 AF 前缀，其余各自套上以太头交给上层。
    void drain()
    {
        if (m_fd < 0)
            return;
        char buf[65536];
        for (int i = 0; i < 64; ++i) { // 单次唤醒收帧上限，别把事件循环饿死
            const ssize_t n = ::read(m_fd, buf, sizeof(buf));
            if (n <= kAfPrefix)
                break;
            const char *ip = buf + kAfPrefix;
            const qsizetype len = n - kAfPrefix;
            const unsigned char ver = static_cast<unsigned char>(ip[0]) >> 4;
            if (ver != 4 && ver != 6)
                continue;
            QByteArray frame;
            frame.reserve(int(len) + kEthHdr);
            frame.append(reinterpret_cast<const char *>(kLocalMac), 6); // dst = 我们自己
            frame.append(reinterpret_cast<const char *>(kPeerMac), 6);  // src = 合成的"设备"
            const unsigned char et4[2] = {0x08, 0x00};
            const unsigned char et6[2] = {0x86, 0xDD};
            frame.append(reinterpret_cast<const char *>(ver == 4 ? et4 : et6), 2);
            frame.append(ip, int(len));
            emit frameReceived(frame);
        }
    }

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QString m_ifname;
    int m_ifIndex = 0;
};

// 工厂：让上层不必知道具体类型。
IL2Endpoint *createTunEndpoint(QObject *parent)
{
    return new TunEndpointMac(parent);
}
