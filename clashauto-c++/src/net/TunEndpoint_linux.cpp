// TUN 端点（Linux）—— 把「本机全量流量」接进已有的用户态栈。
//
// ★ 关键设计：**它伪装成一个 IL2Endpoint（以太网端点），而不是给 NetStack 加一条 L3 通道。**
//   TUN 收发的是**裸 IP 包**，而 NetStack/lwIP 这一整套（netif、设备表、DNS 劫持、fake-ip 反查、
//   按目的地选出站）都是按以太帧写的。若为 TUN 另开一条 L3 路径，等于把这套逻辑再实现一遍——
//   正是本项目一直在避免的「两套逻辑慢慢漂移」。所以这里在最底层做转换：
//     · 读：给 IP 包**前置 14 字节以太头**（dst=本机 MAC、src=合成的设备 MAC、
//       ethertype 按 IP 版本取 0x0800/0x86DD）后交给上层；
//     · 写：把上层给的以太帧**剥掉 14 字节**，只把 IP 包写回 TUN。
//   上层一行都不用改，网关那条路验过的东西（含 lwIP 的 accept-all 补丁）直接复用。
//
// 合成的设备 MAC 是固定的：TUN 上只有「本机」这一个来源，不需要区分设备。ARP/NDP 在 TUN 上
// 不存在（没有二层邻居），所以 ARP 请求帧不会出现；lwIP 侧靠 NetStack::addDevice 登记的
// 静态邻居直接出帧。
//
// 权限：创建 /dev/net/tun 与配置地址/路由需要 CAP_NET_ADMIN（root）。拿不到就 open 失败并说明原因，
// 由上层决定回退 mihomo 的 TUN。
#include "IL2Endpoint.h"

#include <QByteArray>
#include <QProcess>
#include <QSocketNotifier>
#include <QString>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr int kEthHdr = 14;
// 合成的对端 MAC：TUN 上没有真实二层邻居，随便取一个**本地管理**地址（第二个字节 0x02 位）
// 即可，只要全程一致。用固定值而不是随机：日志/抓包里一眼认得出是 TUN 合成的。
constexpr unsigned char kPeerMac[6] = {0x02, 0x43, 0x4f, 0x41, 0x53, 0x54}; // 02:"COAST"
constexpr unsigned char kLocalMac[6] = {0x02, 0x43, 0x4f, 0x41, 0x53, 0x01};
} // namespace

class TunEndpointLinux final : public IL2Endpoint
{
public:
    explicit TunEndpointLinux(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~TunEndpointLinux() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        m_fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK);
        if (m_fd < 0) {
            if (err)
                *err = QStringLiteral("打不开 /dev/net/tun：%1（需要 root/CAP_NET_ADMIN）")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        // IFF_TUN = 三层（裸 IP）；IFF_NO_PI = 不要那 4 字节包信息头，读到的就是纯 IP 包。
        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
        const QByteArray nm = ifname.toLatin1();
        std::strncpy(ifr.ifr_name, nm.constData(), IFNAMSIZ - 1);
        if (::ioctl(m_fd, TUNSETIFF, &ifr) < 0) {
            if (err)
                *err = QStringLiteral("TUNSETIFF 失败：%1")
                           .arg(QString::fromLocal8Bit(std::strerror(errno)));
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        m_ifname = QString::fromLatin1(ifr.ifr_name);
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
            ::close(m_fd);
            m_fd = -1;
        }
    }
    bool isOpen() const override { return m_fd >= 0; }

    // 上层给的是完整以太帧：剥掉 14 字节头，只把 IP 包写进 TUN。
    bool send(const QByteArray &frame) override
    {
        if (m_fd < 0 || frame.size() <= kEthHdr)
            return false;
        const char *ip = frame.constData() + kEthHdr;
        const qsizetype n = frame.size() - kEthHdr;
        const ssize_t w = ::write(m_fd, ip, size_t(n));
        return w == ssize_t(n);
    }

    QByteArray localMac() const override
    {
        return QByteArray(reinterpret_cast<const char *>(kLocalMac), 6);
    }
    int ifIndex() const override { return 0; } // TUN 上层用不到
    int mtu() const override { return 1500; }
    QString ifname() const { return m_ifname; }

private:
    // 读一批 IP 包，各自套上以太头后交给上层。
    void drain()
    {
        if (m_fd < 0)
            return;
        char buf[65536];
        for (int i = 0; i < 64; ++i) { // 单次唤醒收帧上限，别把事件循环饿死
            const ssize_t n = ::read(m_fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            if (n < 1)
                continue;
            const unsigned char ver = static_cast<unsigned char>(buf[0]) >> 4;
            if (ver != 4 && ver != 6)
                continue; // 不是 IP 包，丢
            QByteArray frame;
            frame.reserve(int(n) + kEthHdr);
            frame.append(reinterpret_cast<const char *>(kLocalMac), 6); // dst = 我们自己
            frame.append(reinterpret_cast<const char *>(kPeerMac), 6);  // src = 合成的"设备"
            const unsigned char et[2] = {0x08, 0x00};
            const unsigned char et6[2] = {0x86, 0xDD};
            frame.append(reinterpret_cast<const char *>(ver == 4 ? et : et6), 2);
            frame.append(buf, int(n));
            emit frameReceived(frame);
        }
    }

    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QString m_ifname;
};

// 工厂：让上层不必知道具体类型。返回 nullptr 表示本平台不支持。
IL2Endpoint *createTunEndpoint(QObject *parent)
{
    return new TunEndpointLinux(parent);
}

// TUN 上「对端」的合成 MAC —— NetStack::addDevice 要用它登记静态邻居。
QByteArray tunPeerMac()
{
    return QByteArray(reinterpret_cast<const char *>(kPeerMac), 6);
}
