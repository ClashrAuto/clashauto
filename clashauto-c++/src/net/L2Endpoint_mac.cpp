// macOS 二层端点（BPF）—— 透明网关在 mac 上的 IL2Endpoint 实现。
// 打开 /dev/bpfN、BIOCSETIF 绑网卡，读/写完整以太帧。
//
// 权限：/dev/bpf* 需 root（或加入 access_bpf 组）。App 常态非 root → open 失败 → 工厂返回的端点
// open() 返回 false → LanGateway.isAvailable()=false（优雅降级）。真机联调可先 sudo 跑；
// 无 root 的正式方案是由 root helper 打开 bpf 后经 XPC 传 fd 回来（M3.5，待做，见 helper/）。
#include "IL2Endpoint.h"

#if defined(Q_OS_MACOS)

#include <QByteArray>
#include <QSocketNotifier>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

// BPF 读缓冲里每个包前有 struct bpf_hdr，包间按 BPF_WORDALIGN 对齐。
class MacL2Endpoint final : public IL2Endpoint
{
public:
    explicit MacL2Endpoint(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~MacL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        // 逐个尝试 /dev/bpf0../bpf255（新系统也支持克隆节点，但逐个更稳）。
        for (int i = 0; i < 256 && m_fd < 0; ++i) {
            char dev[16];
            std::snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
            m_fd = ::open(dev, O_RDWR);
            if (m_fd < 0 && errno != EBUSY)
                break; // 非「设备忙」（多为权限）→ 不再试
        }
        if (m_fd < 0) {
            if (err) *err = QStringLiteral("打开 /dev/bpf 失败（需 root）: ") + strerror(errno);
            return false;
        }

        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, ifname.toLatin1().constData(), IFNAMSIZ - 1);
        if (::ioctl(m_fd, BIOCSETIF, &ifr) < 0) {
            if (err) *err = QStringLiteral("BIOCSETIF(%1) 失败: %2").arg(ifname, strerror(errno));
            close();
            return false;
        }

        // 取内核选定的缓冲长度（读时按此大小 read）。
        if (::ioctl(m_fd, BIOCGBLEN, &m_bufLen) < 0 || m_bufLen <= 0)
            m_bufLen = 32768;
        unsigned int on = 1, off = 0;
        ::ioctl(m_fd, BIOCIMMEDIATE, &on);  // 立即返回，不缓冲等待
        ::ioctl(m_fd, BIOCPROMISC, &on);    // 混杂：收目的非本机 MAC 的帧（被劫持设备发给「网关」的帧）
        ::ioctl(m_fd, BIOCSHDRCMPLT, &on);  // 写时我们提供完整链路头，内核不改源 MAC
        ::ioctl(m_fd, BIOCSSEESENT, &off);  // 不回显自己发出的帧，避免自环
        ::fcntl(m_fd, F_SETFL, O_NONBLOCK);

        m_localMac = readMac(ifname);

        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        QObject::connect(m_notifier, &QSocketNotifier::activated, this, [this] { drain(); });
        return true;
    }

    void close() override
    {
        if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    }
    bool isOpen() const override { return m_fd >= 0; }

    bool send(const QByteArray &frame) override
    {
        return m_fd >= 0 && ::write(m_fd, frame.constData(), frame.size()) == frame.size();
    }
    QByteArray localMac() const override { return m_localMac; }
    int ifIndex() const override { return 0; }
    int mtu() const override { return 1500; } // NetStack 固定用 1500，端点 mtu 仅接口占位

private:
    void drain()
    {
        QByteArray buf(m_bufLen, char(0));
        for (;;) {
            const ssize_t n = ::read(m_fd, buf.data(), m_bufLen);
            if (n <= 0)
                break; // EAGAIN / 关闭
            int p = 0;
            while (p + static_cast<int>(sizeof(struct bpf_hdr)) <= n) {
                const auto *bh = reinterpret_cast<const struct bpf_hdr *>(buf.constData() + p);
                const int hdr = bh->bh_hdrlen;
                const int cap = bh->bh_caplen;
                if (hdr <= 0 || cap < 0 || p + hdr + cap > n)
                    break;
                emit frameReceived(QByteArray(buf.constData() + p + hdr, cap));
                p += BPF_WORDALIGN(hdr + cap);
            }
        }
    }

    static QByteArray readMac(const QString &ifname)
    {
        QByteArray mac;
        struct ifaddrs *ifap = nullptr;
        if (::getifaddrs(&ifap) == 0) {
            for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
                if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_LINK
                    && ifname == QString::fromLatin1(ifa->ifa_name)) {
                    const auto *sdl = reinterpret_cast<const struct sockaddr_dl *>(ifa->ifa_addr);
                    if (sdl->sdl_alen == 6) {
                        mac = QByteArray(LLADDR(const_cast<struct sockaddr_dl *>(sdl)), 6);
                        break;
                    }
                }
            }
            ::freeifaddrs(ifap);
        }
        return mac;
    }

    int m_fd = -1;
    int m_bufLen = 32768;
    QByteArray m_localMac;
    QSocketNotifier *m_notifier = nullptr;
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new MacL2Endpoint(parent);
}

#endif // Q_OS_MACOS
