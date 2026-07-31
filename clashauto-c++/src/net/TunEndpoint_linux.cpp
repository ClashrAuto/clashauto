// TUN 端点（Linux）—— 把「本机全量流量」接进已有的用户态栈。
//
// 设计总纲（为什么伪装成 IL2Endpoint、合成 MAC 为什么固定）见 **TunEndpoint.h**，那里也是
// 两个平台共用的以太头长度与合成 MAC 的唯一出处；Windows 版是 TunEndpoint_win.cpp。
// 一句话：读时给裸 IP 包前置 14 字节以太头、写时剥掉，上层 NetStack/lwIP 一行不改。
//
// 权限：创建 /dev/net/tun 与配置地址/路由需要 CAP_NET_ADMIN（root）。拿不到就 open 失败并说明原因，
// 由上层决定回退 mihomo 的 TUN。
#include "TunEndpoint.h"

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
constexpr int kEthHdr = coastcore::kTunEthHdr;
constexpr const unsigned char *kPeerMac = coastcore::kTunPeerMac;
constexpr const unsigned char *kLocalMac = coastcore::kTunLocalMac;
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
    QString ifname() const override { return m_ifname; }

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
