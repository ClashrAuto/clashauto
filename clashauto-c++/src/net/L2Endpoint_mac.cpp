// macOS 二层端点（BPF）—— 透明网关在 mac 上的 IL2Endpoint 实现。
// 打开 /dev/bpfN、BIOCSETIF 绑网卡，读/写完整以太帧。
//
// 权限：/dev/bpf* 需 root（或加入 access_bpf 组）。App 常态非 root → open 失败 → 工厂返回的端点
// open() 返回 false → LanGateway.isAvailable()=false（优雅降级）。真机联调可先 sudo 跑；
// 无 root 的正式方案是由 root helper 打开 bpf 后经 XPC 传 fd 回来（M3.5，待做，见 helper/）。
#include "IL2Endpoint.h"

#if defined(Q_OS_MACOS)

#include "../MacHelperClient.h" // M3.5：非 root 时经 root helper 拿 bpf fd

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QSocketNotifier>
#include <QVector>

#include <cerrno>
#include <cstring>
#include <deque>
#include <vector>

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

// 发方积压队列上限与丢帧日志节流间隔，取值与 Linux 端一致（定容量的判据见 L2Endpoint_linux.cpp
// 里 kTxBacklogMaxBytes 那段：排队时延必须明显短于 lwIP 的重传下限 ≈0.6s，不是越大越好）。
// bpf 没有 SO_SNDBUF 那种旋钮，所以这层就是 mac 上唯一能自己控制的缓冲。
constexpr qint64 kTxBacklogMaxBytes = 1 * 1024 * 1024;
constexpr qint64 kDropReportMinIntervalMs = 30000;

// BPF 读缓冲里每个包前有 struct bpf_hdr，包间按 BPF_WORDALIGN 对齐。
class MacL2Endpoint final : public IL2Endpoint
{
public:
    explicit MacL2Endpoint(QObject *parent = nullptr) : IL2Endpoint(parent) {}
    ~MacL2Endpoint() override { close(); }

    bool open(const QString &ifname, QString *err) override
    {
        // 优先经 root helper 拿「已开好、已配好」的 bpf fd（正式运行：app 非 root，M3.5）。
        // helper 没装/没批准（isReady()=false，快速返回不阻塞）→ 落到直接开（需 root，联调 sudo / 自测）。
        if (MacHelper::isReady()) {
            QString herr;
            const int fd = MacHelper::openBpf(ifname, &herr);
            if (fd >= 0)
                return finishOpen(fd, ifname);
            // helper 在但开失败：不致命，继续尝试直接开（herr 仅备参考）。
        }

        // 直接打开 /dev/bpf0../bpf255（需 root）并配置。
        int fd = -1;
        for (int i = 0; i < 256 && fd < 0; ++i) {
            char dev[16];
            std::snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
            fd = ::open(dev, O_RDWR);
            if (fd < 0 && errno != EBUSY)
                break; // 非「设备忙」（多为权限）→ 不再试
        }
        if (fd < 0) {
            if (err) *err = QStringLiteral("打开 /dev/bpf 失败（需 root 或安装 helper）: ")
                            + strerror(errno);
            return false;
        }
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, ifname.toLatin1().constData(), IFNAMSIZ - 1);
        if (::ioctl(fd, BIOCSETIF, &ifr) < 0) {
            if (err) *err = QStringLiteral("BIOCSETIF(%1) 失败: %2").arg(ifname, strerror(errno));
            ::close(fd);
            return false;
        }
        unsigned int on = 1, off = 0;
        ::ioctl(fd, BIOCIMMEDIATE, &on);  // 立即返回，不缓冲等待
        // ★ 默认**不开混杂**（与 Windows 侧同款根因修复，见 L2Endpoint_win）。这套「先投毒、再抓
        //   设备发来的帧」的透明网关根本不需要混杂：设备被投毒后发往「网关」的帧目的 MAC 就是本机
        //   MAC，非混杂也照收；广播 ARP 同样照收。而开混杂时网卡把**所有**帧上交，本机自己的网络栈
        //   也可能据此把我们单播给别人的 ARP 欺骗应答学进 ARP 缓存 → 本机自投毒断网。宁可关掉。
        //   联调兜底：COAST_GATEWAY_PROMISC=1 时才开混杂。
        if (qEnvironmentVariableIsSet("COAST_GATEWAY_PROMISC"))
            ::ioctl(fd, BIOCPROMISC, &on);
        ::ioctl(fd, BIOCSHDRCMPLT, &on);  // 写时我们提供完整链路头
        ::ioctl(fd, BIOCSSEESENT, &off);  // 不回显自己发出的帧
        return finishOpen(fd, ifname);
    }

    // 共用收尾：无论 fd 来自 helper 还是直接开——查缓冲长度、置非阻塞、读本机 MAC、挂 notifier。
    bool finishOpen(int fd, const QString &ifname)
    {
        m_fd = fd;
        if (::ioctl(m_fd, BIOCGBLEN, &m_bufLen) < 0 || m_bufLen <= 0)
            m_bufLen = 32768;
        ::fcntl(m_fd, F_SETFL, O_NONBLOCK);
        m_localMac = readMac(ifname);
        m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
        QObject::connect(m_notifier, &QSocketNotifier::activated, this, [this] { drain(); });
        // 写就绪通知器：常态关闭，只在积压非空期间打开（理由同 Linux 端，见下面 send()）。
        m_txNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
        m_txNotifier->setEnabled(false);
        QObject::connect(m_txNotifier, &QSocketNotifier::activated, this, [this] { drainTx(); });
        return true;
    }

    void close() override
    {
        if (m_notifier) { delete m_notifier; m_notifier = nullptr; }
        if (m_txNotifier) { delete m_txNotifier; m_txNotifier = nullptr; }
        m_txQueue.clear();
        m_txQueuedBytes = 0;
        if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
    }
    bool isOpen() const override { return m_fd >= 0; }

    // 发一帧。fd 是**非阻塞**的，bpf 的写缓冲/网卡发送队列一满就返回 ENOBUFS/EAGAIN ——
    // 老写法在这里直接 return false 把帧丢掉，而上层（NetStack 的 lwipLinkOutput）忽略返回值、
    // 恒回 ERR_OK，于是变成「本机→设备方向的静默丢包」，只能等 lwIP 几百毫秒后重传。
    // 与 Linux 端同治：满了就排队等可写，队列顶满才丢并计数。详细论证见 L2Endpoint_linux.cpp
    // 文件头「发方」一节（那边还多一层 SO_SNDBUF 抬高，bpf 没有对应的旋钮）。
    bool send(const QByteArray &frame) override
    {
        if (m_fd < 0 || frame.size() < 14)
            return false;
        if (!m_txQueue.empty()) {
            // ★ 先机会性排空一次，别只指望写通知器：XNU 的 bpf 字符设备历来只在**读**方向做
            //   select/kqueue 唤醒，写方向能不能拿到就绪事件不保证。真拿不到的话，队列就只能靠
            //   后续每次 send() 来推进——所以这一下必须有，否则积压会一直囤到上限才开始丢。
            drainTx();
            if (!m_txQueue.empty())
                return enqueueTx(frame); // 仍有积压：必须排队尾，插队会打乱同一条流的段序
        }
        const int r = rawSend(frame);
        if (r > 0)
            return true;
        if (r == 0)
            return enqueueTx(frame);
        return false;
    }
    QByteArray localMac() const override { return m_localMac; }
    int ifIndex() const override { return 0; }
    int mtu() const override { return 1500; } // NetStack 固定用 1500，端点 mtu 仅接口占位

    // 内核态源 MAC 过滤：BIOCSETF 给 bpf 设备下发一段 BPF 程序。只影响**读**（BPF 抓包），::write
    // 发帧不受影响；也和内核给本机正常协议栈的投递互不相干。契约与「该收哪些帧」见 IL2Endpoint.h。
    // 指令布局与 Linux 端完全一致（cBPF 通用）：每 MAC 一个 4 条块比对以太头 offset 6..11，末尾 ret。
    bool setSourceMacFilter(const QVector<QByteArray> &macs) override
    {
        if (m_fd < 0)
            return false;

        QVector<QByteArray> valid;
        valid.reserve(macs.size());
        for (const QByteArray &m : macs)
            if (m.size() == 6)
                valid.append(m);

        std::vector<struct bpf_insn> code;
        if (valid.isEmpty()) {
            // 没有被劫持设备：整段全丢（ret 0），越早在内核丢越省 CPU。
            code.push_back(BPF_STMT(BPF_RET | BPF_K, 0));
        } else {
            if (valid.size() > 60) // 8bit 跳转偏移保护；真实场景远达不到
                return false;
            const int n = valid.size();
            // 末尾追加「或 ARP」分支（与 L2Endpoint_linux 完全一致）：所有 victim 源 MAC 失配后再看
            // ethertype 是否 0x0806，是则也放行。**必须捕获 ARP**——反应式反制要靠捕获真网关自己
            // 广播的 who-has(携带「网关在真 MAC」会把设备解毒)来立刻重投盖回(见 LanGateway
            // frameReceived / ArpSpoofer::reassertNow)。ARP 低频，全收几乎零成本。
            const int arpLd = 4 * n;       // ldh [12] ; A = ethertype
            const int accept = 4 * n + 2;  // ret 0xffffffff
            const int reject = 4 * n + 3;  // ret 0
            for (int i = 0; i < n; ++i) {
                const auto *b = reinterpret_cast<const unsigned char *>(valid[i].constData());
                const quint16 first2 = (quint16(b[0]) << 8) | b[1];
                const quint32 last4 = (quint32(b[2]) << 24) | (quint32(b[3]) << 16)
                                    | (quint32(b[4]) << 8) | quint32(b[5]);
                const int base = 4 * i;
                // 本块失配后的去处：不是最后一个 MAC → 下一块；最后一个 → ARP 判定块。
                const int failIdx = (i < n - 1) ? (4 * (i + 1)) : arpLd;
                // [base+0] A = offset 8 的 4 字节 = src[2..5]
                code.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 8));
                // [base+1] 比 last4；不等跳失败目标
                code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, last4, 0,
                                        static_cast<u_char>(failIdx - (base + 1) - 1)));
                // [base+2] A = offset 6 的 2 字节 = src[0..1]
                code.push_back(BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 6));
                // [base+3] 比 first2；等跳 accept，不等跳失败目标
                code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, first2,
                                        static_cast<u_char>(accept - (base + 3) - 1),
                                        static_cast<u_char>(failIdx - (base + 3) - 1)));
            }
            code.push_back(BPF_STMT(BPF_LD | BPF_H | BPF_ABS, 12));            // arpLd
            code.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x0806, 0, 1)); // ==ARP→accept 否则→reject
            code.push_back(BPF_STMT(BPF_RET | BPF_K, 0xFFFFFFFFu));            // accept：整帧
            code.push_back(BPF_STMT(BPF_RET | BPF_K, 0));                     // reject
        }

        struct bpf_program prog;
        prog.bf_len = static_cast<u_int>(code.size());
        prog.bf_insns = code.data();
        // BIOCSETF 会替换旧过滤器并冲掉缓冲——动态重设直接再调即可。装不上则返回 false，用户态兜底。
        if (::ioctl(m_fd, BIOCSETF, &prog) < 0)
            return false;
        return true;
    }

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

    // 裸写一帧：1 = 已发出；0 = 缓冲满（可重试）；-1 = 真错误。
    int rawSend(const QByteArray &frame)
    {
        for (;;) {
            const ssize_t n = ::write(m_fd, frame.constData(), size_t(frame.size()));
            if (n == ssize_t(frame.size()))
                return 1;
            if (n >= 0)
                return -1;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)
                return 0;
            return -1;
        }
    }

    bool enqueueTx(const QByteArray &frame)
    {
        if (m_txQueuedBytes + frame.size() > kTxBacklogMaxBytes) {
            ++m_txDropped;
            reportDropsThrottled();
            return false;
        }
        m_txQueue.push_back(frame);
        m_txQueuedBytes += frame.size();
        if (m_txNotifier && !m_txNotifier->isEnabled())
            m_txNotifier->setEnabled(true);
        return true;
    }

    void drainTx()
    {
        while (!m_txQueue.empty()) {
            const int r = rawSend(m_txQueue.front());
            if (r == 0)
                return; // 又满了，等下一次可写
            if (r < 0) {
                ++m_txDropped; // 发不出去的帧不能永远堵住队头
                reportDropsThrottled();
            }
            m_txQueuedBytes -= m_txQueue.front().size();
            m_txQueue.pop_front();
        }
        if (m_txNotifier)
            m_txNotifier->setEnabled(false);
    }

    void reportDropsThrottled()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastDropReportMs < kDropReportMinIntervalMs)
            return;
        m_lastDropReportMs = now;
        qWarning().noquote() << "L2Endpoint(bpf): 发方积压溢出丢帧" << m_txDropped
                             << "帧 (当前积压" << m_txQueuedBytes
                             << "字节)。链路喂不进去会让被代理设备「什么都慢」。";
    }

    int m_fd = -1;
    int m_bufLen = 32768;
    QByteArray m_localMac;
    QSocketNotifier *m_notifier = nullptr;
    // 发方积压（见 send()）。写通知器常态关闭，只在队列非空期间打开。
    QSocketNotifier *m_txNotifier = nullptr;
    std::deque<QByteArray> m_txQueue;
    qint64 m_txQueuedBytes = 0;
    qint64 m_txDropped = 0;
    qint64 m_lastDropReportMs = -kDropReportMinIntervalMs;
};

} // namespace

IL2Endpoint *createL2Endpoint(QObject *parent)
{
    return new MacL2Endpoint(parent);
}

#endif // Q_OS_MACOS
