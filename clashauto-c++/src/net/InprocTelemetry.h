#pragma once

// 进程内引擎的控制面出口 —— 连接快照 / 流量合计 / 日志，三样一起。
//
// ★ 为什么需要它：数据面已经能整条走进程内（网关 / 本机入站 / 本机 TUN），但控制面
//   仍全靠 mihomo 的 REST（/connections、/traffic、核心 stdout）。于是流量真走了进程内
//   那条路时，UI 的连接列表/流量卡/日志页**一个字都看不到** —— 功能是进步，可见性是倒退。
//   本单元就是那个「统一的进程内快照出口」：三条路共用的拨号咽喉
//   （CoreDialerFactory 的路由包装器）把每条连接登记进来，UI 侧再与 REST 数据合并。
//
// ★★ 与 mihomo REST 的合并策略（防止两个来源打架，调用方必须遵守）：
//   1) **只登记「真走进程内出站」的连接**（DIRECT / 进程内协议实现）。回退核心的连接
//      不登记 —— 那些连接 mihomo 自己的 /connections、/traffic 会报，两边各报各的、
//      集合天然不相交，"REST + 本表" 直接相加/拼接就是全量，**不存在重复计数**。
//   2) 核心在时行为不变：REST 仍是它那部分的唯一权威；本表只补「REST 看不见的那部分」。
//      核心不在时 REST 归零，本表就是全部。
//   3) 连接 id 一律 "coast-<n>" 前缀，与 mihomo 的 UUID 永不冲突；UI 按前缀分流关闭请求。
//
// 线程性：登记/计数发生在各数据面线程（网关工作线程 / GUI 线程的 TUN 与本机入站），
// 读取发生在 GUI 线程 —— 全部接口内部用互斥锁保护，任何线程可调。logged 信号可能从
// 任意线程发出，接收方（GUI 的 LogModel / QmlBridge）经 Qt 自动排队投递，无需额外处理。
//
// 关闭连接的跨线程约定：登记时带上 owner（出站包装器）与 closer 回调；requestClose 在
// **持锁状态下**把 closer 排到 owner 所在线程（QMetaObject::invokeMethod, Queued）。
// 包装器析构必须调 unregisterConn（同一把锁）—— 于是「查到记录时 owner 一定还活着，
// 排进队后 owner 若析构，Qt 会随 context 取消未投递的调用」，没有竞态窗口。
#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class InprocTelemetry : public QObject
{
    Q_OBJECT
public:
    // 进程级单例（首次调用时构造，C++11 magic static 保证线程安全）。
    static InprocTelemetry &instance();

    // 登记一条进程内连接（TCP 在 connectTo 时、UDP 在 associate 时）。
    //   host/port  目的地（UDP 关联时还没有，可先空、后用 noteUdpDst 补）
    //   network    "tcp" | "udp"
    //   chain      真正出网的那一跳（节点名或 "DIRECT"）—— 对齐 mihomo chains[0] 的语义
    //   user       设备身份（网关 dev-<mac>；本机入站/TUN 为 "local"）
    //   inboundTag 入口类型（进快照 metadata.type，UI 的「类型」列）
    //   owner      出站包装器自身；closer 会被排到它所在线程执行（见文件头「跨线程约定」）
    // 返回连接句柄；0 = 无效（不会发生，占位）。
    quint64 registerConn(const QString &host, quint16 port, const QString &network,
                         const QString &chain, const QString &user, const QString &inboundTag,
                         QObject *owner, std::function<void()> closer);
    // 记流量（up=发往远端的字节，down=远端来的字节）。id 已注销时静默忽略。
    void addUp(quint64 id, qint64 n);
    void addDown(quint64 id, qint64 n);
    // UDP 会话没有单一目的地：记录最近一次 sendTo 的目标，快照里显示它。
    void noteUdpDst(quint64 id, const QString &host, quint16 port);
    // 连接结束（closed/failed/析构都要调；幂等）。字节合计保留在 totalUp/totalDown 里。
    void unregisterConn(quint64 id);

    // —— 控制面读取（GUI 线程用，任何线程安全）——
    // 活动连接快照，条目形状对齐 mihomo GET /connections 的元素
    // （id/upload/download/start/chains/metadata{network,type,host,destinationPort,sourceIP,inboundUser}），
    // 于是消费侧（QmlBridge 的合并循环 / HistoryStore::observe）不需要任何特判。
    QJsonArray snapshot() const;
    int activeCount() const;
    // 累计字节（含已关闭连接；单调不减）。速率由调用方按秒差分。
    quint64 totalUp() const;
    quint64 totalDown() const;
    // 累计登记过的连接数（自检/反向对照用：数据面通了它却是 0 = 接线断了）。
    quint64 totalConns() const;
    // 关闭一条进程内连接（id 形如 "coast-<n>"）。返回是否找到了这条连接。
    bool requestClose(const QString &connId);
    // 关闭全部进程内连接（对齐 UI 的「关闭全部」）。返回请求关闭的条数。
    int closeAll();

    // 打一条进程内引擎日志（进 UI 的日志页 + 页脚）。throttleKey 非空时做节流：同 key 3 秒
    // 内只发第一条，其间被吞掉的条数会缀在下一条允许发出的消息后（「（其间同类 ×N 条被合并）」）
    // —— 数据面每秒可能几千连接，逐条打日志会把 UI 日志页刷成瀑布。key 建议用「事件类型:节点」。
    void log(const QString &throttleKey, const QString &message);

signals:
    // 一条给人看的日志（可能从任意线程发出；接收方经 Qt 排队投递到自己的线程）。
    void logged(const QString &message);

private:
    InprocTelemetry() = default;

    struct Rec {
        QString host;
        quint16 port = 0;
        QString network;   // "tcp" | "udp"
        QString chain;     // 节点名 / "DIRECT"
        QString user;      // dev-<mac> / "local"
        QString tag;       // 入口类型（metadata.type）
        QString start;     // RFC3339(UTC)，与 mihomo 同序可比
        qint64 up = 0;
        qint64 down = 0;
        QPointer<QObject> owner;        // 出站包装器（其析构会先 unregister，见文件头）
        std::function<void()> closer;
    };
    struct Throttle {
        qint64 lastMs = 0;
        int suppressed = 0;
    };

    mutable QMutex m_mu;
    QHash<quint64, Rec> m_conns;
    quint64 m_nextId = 1;
    quint64 m_totalUp = 0;
    quint64 m_totalDown = 0;
    quint64 m_totalConns = 0;
    QHash<QString, Throttle> m_throttle;
};
