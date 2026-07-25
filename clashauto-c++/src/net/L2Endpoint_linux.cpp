// IL2Endpoint 的 Linux 后端 —— AF_PACKET(SOCK_RAW) 二层收发。
//
// 为什么 AF_PACKET：透明网关要在以太层「窃听 + 注入」被劫持设备的完整帧（含 14 字节以太头），
// 普通 socket 拿不到二层。绑定到指定网卡后，内核把该口收到的每个帧原样交给我们（ETH_P_ALL），
// 我们再决定终结/转发；发帧则由调用方自填 dst/src MAC + ethertype，内核不改。
//
// 为什么用 QSocketNotifier 而非线程轮询：契约要求 frameReceived() 在 Qt 事件循环线程发出，
// 且不得阻塞。把 fd 设成非阻塞 + Notifier(Read) 就绪回调里 recv 到 EAGAIN 为止——零轮询、零线程。
//
// 权限：需要 CAP_NET_RAW / root，否则 socket() 直接 EPERM。open() 失败时置 *err 并清理半开 fd。
//
// 本文件在所有平台都参与编译：非 Linux 时只保留一个返回 nullptr 的工厂（LanGateway 据此判不可用）。
#include "IL2Endpoint.h"

#if defined(Q_OS_LINUX)

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <net/ethernet.h> // ETH_P_ALL / ETH_ALEN（内部再引 <linux/if_ether.h>，有 UAPI 守卫防冲突）
#include <arpa/inet.h>    // htons
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

#include <QByteArray>
#include <QSocketNotifier>

namespace {

// 把 errno 格式化进 *err（"<what>: <strerror>"）。必须在触发错误的系统调用之后、
// 任何可能改写 errno 的调用（如 ::close）之前调用。
void setSysError(QString *err, const char *what)
{
    if (err)
        *err = QString::fromLatin1(what) + QStringLiteral(": ")
             + QString::fromLocal8Bit(std::strerror(errno));
}

// Linux 二层端点：一张网卡一个 AF_PACKET 原始套接字。
// 注意：本类不加 Q_OBJECT——它不声明新信号/槽，只重写基类虚函数并 emit 基类的 frameReceived，
// 且用 functor 版 connect 绑定 Notifier，因此无需 moc（避免在 .cpp 里 #include ".moc"）。
class LinuxL2Endpoint final : public IL2Endpoint
{
public:
    explicit LinuxL2Endpoint(QObject *parent) : IL2Endpoint(parent) {}
    ~LinuxL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        if (isOpen())
            close();

        // 网卡名转 C 字符串（接口名是 ASCII，latin1 足够）。
        const QByteArray name = ifname.toLatin1();

        int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (fd < 0) {
            setSysError(err, "socket(AF_PACKET)");
            return false;
        }

        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, name.constData(), IFNAMSIZ - 1);
        // memset 已保证末尾 '\0'，strncpy 只写到 IFNAMSIZ-1，安全截断。

        // 接口索引（bind 用）。
        if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
            setSysError(err, "ioctl(SIOCGIFINDEX)");
            ::close(fd);
            return false;
        }
        const int idx = ifr.ifr_ifindex;

        // 本机 MAC（伪造 ARP/以太源地址用）。SIOCGIFHWADDR 把 6 字节放在 ifr_hwaddr.sa_data[0..5]。
        if (::ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
            setSysError(err, "ioctl(SIOCGIFHWADDR)");
            ::close(fd);
            return false;
        }
        const QByteArray mac(ifr.ifr_hwaddr.sa_data, 6);

        // MTU（用户态栈分片参考）。取不到不算致命——回落 1500（契约）。
        int mtu = 1500;
        if (::ioctl(fd, SIOCGIFMTU, &ifr) >= 0)
            mtu = ifr.ifr_mtu;

        // 绑定到该网卡，只收/发这张口的帧。
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_protocol = htons(ETH_P_ALL);
        sll.sll_ifindex = idx;
        if (::bind(fd, reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll)) < 0) {
            setSysError(err, "bind(sockaddr_ll)");
            ::close(fd);
            return false;
        }

        // 非阻塞：就绪回调里循环 recv 到 EAGAIN，绝不在无数据时阻塞事件循环。
        int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0)
            flags = 0;
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        m_fd = fd;
        m_ifIndex = idx;
        m_localMac = mac;
        m_mtu = mtu;

        m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        // Qt6 的 activated 只剩 (QSocketDescriptor, Type) 一个重载，取地址无歧义；
        // 用零参 lambda 转发（尾随实参可丢弃）。
        connect(m_notifier, &QSocketNotifier::activated, this, [this]() { drainSocket(); });
        return true;
    }

    void close() override
    {
        if (m_notifier) {
            delete m_notifier; // 先删 Notifier，停止对 fd 的监听，再关 fd
            m_notifier = nullptr;
        }
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_ifIndex = -1;
        m_localMac.clear();
        m_mtu = 1500;
    }

    bool isOpen() const override { return m_fd >= 0; }

    bool send(const QByteArray &frame) override
    {
        if (m_fd < 0 || frame.size() < 14) // 至少要够以太头
            return false;

        // 发送地址结构：内核按 sll_ifindex 选口，sll_halen/sll_addr 给目的 MAC（= 帧头前 6 字节）。
        struct sockaddr_ll sll;
        std::memset(&sll, 0, sizeof(sll));
        sll.sll_family = AF_PACKET;
        sll.sll_ifindex = m_ifIndex;
        sll.sll_halen = 6;
        std::memcpy(sll.sll_addr, frame.constData(), 6); // dst MAC

        const ssize_t n = ::sendto(m_fd, frame.constData(), static_cast<size_t>(frame.size()), 0,
                                   reinterpret_cast<struct sockaddr *>(&sll), sizeof(sll));
        return n == static_cast<ssize_t>(frame.size());
    }

    QByteArray localMac() const override { return m_localMac; }
    int ifIndex() const override { return m_ifIndex; }
    int mtu() const override { return m_mtu; }

private:
    // fd 可读：把内核缓冲里所有帧一次性抽干（每帧 emit 一次），直到 EAGAIN。
    void drainSocket()
    {
        char buf[65536]; // 单帧上限，容得下巨帧
        for (;;) {
            const ssize_t n = ::recv(m_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                emit frameReceived(QByteArray(buf, static_cast<int>(n)));
            } else if (n < 0) {
                if (errno == EINTR)
                    continue;               // 被信号打断，重试
                break;                      // EAGAIN/EWOULDBLOCK 或其它错误：结束本轮
            } else {
                break;                      // n == 0
            }
        }
    }

    int m_fd = -1;
    int m_ifIndex = -1;
    int m_mtu = 1500;
    QByteArray m_localMac;
    QSocketNotifier *m_notifier = nullptr;
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new LinuxL2Endpoint(parent);
}

#else // 非 Linux：M1 不实现二层，工厂返回 nullptr（LanGateway.isAvailable() 据此为 false）。

IL2Endpoint *createL2Endpoint(QObject * /*parent*/)
{
    return nullptr;
}

#endif
