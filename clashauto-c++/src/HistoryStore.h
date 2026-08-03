#pragma once

// 上网历史库（SQLite）——「哪台设备、什么时候、连了哪个域名、走的哪个节点、多少字节」。
//
// 为什么要一个库：核心的 /connections 只给**当前活动**连接，断了就没了；设备台账
// 只留了「今日/累计」两个总数，常用域名此前完全活在内存里，重启就丢。要能回看/统计，必须落盘，
// 而这个量级（每天几万条、要按设备/域名/时间查）用 JSON 是自找麻烦，所以上 SQLite。
//
// 写入时机是**连接结束时落一条**，不是跟着每秒轮询逐条写：
//   · 每秒把 /connections 快照喂进 observe()，本类维护一张「在途连接」表；
//   · 某个 id 在新快照里消失 = 连接已断 → 用它最后一次的累计字节生成一条记录进待写缓冲；
//   · 缓冲攒够或定时到点，一个事务批量写盘（每条一次 commit 会把磁盘按在地上摩擦）。
// 退出时 flush()：仍在途的长连接也各落一条，否则一条挂了几小时的连接永远进不了库。
//
// ★★ 已知且**无法在客户端修复**的缺口：短连接基本记不到。★★
//
// 这套「快照差分」只认得「上一拍见过、这一拍不见了」的连接。一条**在两次轮询之间开完又关**
// 的连接从头到尾没进过在途表，等于不存在。而 /connections 是全量快照、拉一次开销不小，
// 轮询间隔就定在 2s（ClashService.cpp 的 m_connectionsTimer）。
//
// 真机实测过丢多少（树莓派网关，从一台被代理设备发 1500 条独立短连接，curl --no-keepalive
// 打 generate_204，实测 1497 成功）：
//     同一时段 conn 表只多了 51 行 —— **捕获率 3.4%，丢了 96.6%**。
//
// 别试图靠调小间隔来修，数量级上就不成立：捕获率 ≈ 连接寿命 / 轮询间隔。这批连接寿命
// 大约几十毫秒，要抓到 95% 得把间隔压到 ~5ms —— 那是每秒 200 次全量快照，CPU 直接爆掉，
// 而且仍然只是"少丢一点"，不是"不丢"。**这是快照差分这个机制的固有上限，不是参数没调好。**
//
// 唯一的真解在核心侧：让核心在连接关闭时主动推一条事件（SSE / WebSocket 皆可），客户端
// 收事件入库，一条不漏。核心 API 现在没有这个东西——实测 coast 1.10.0 的 /connections
// 只返回**活动**连接，单条字段只有 chains/download/id/metadata/providerChains/rule/
// rulePayload/start/upload，没有任何关闭事件通道。而核心是我们自己的 fork
// （ClashrAuto/clash），加这么一个推送是可做的，属于跨仓改动。
//
// 在那之前，这个库里的数据要理解为**长连接的样本**，不是全量：设备页的「常用域名」会明显
// 偏向视频/下载/长轮询这类连接，而正常网页浏览（大量几十毫秒的短连接）几乎看不见。
//
// 归属（mac）与 DevicesController::aggregate 同一套口径：先按 sourceIP 查设备，查不到再按
// inboundUser=dev-<mac> 查（透明网关代理的连接 sourceIP 恒为 127.0.0.1）。都查不到就记空串
// （本机自身/未归属的流量）。
//
// 没有 SQLite 驱动时（理论上不会——Qt 自带 QSQLITE）isOpen() 为 false，所有方法安全空转，
// 程序照常跑，只是没有历史。
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

class DeviceStore;
class QJsonArray;
class QTimer;

class HistoryStore final : public QObject
{
    Q_OBJECT
public:
    // 一条已结束连接的历史记录。
    struct Record {
        QString mac;      // 归属设备 MAC（空 = 本机/未归属）
        QString host;     // 域名（sniffer 之后多数有值），没有则是目标 IP
        QString destIp;
        QString chain;    // 出口策略/节点（chains[0]）
        QString network;  // tcp/udp
        QString process;  // 发起进程；仅本机连接有值
        qint64 startedAt = 0; // unix ms
        qint64 endedAt = 0;   // unix ms
        qint64 up = 0;
        qint64 down = 0;
    };
    // 「域名 → 字节」聚合（Top 域名用）。
    struct DomainTotal {
        QString host;
        qint64 bytes = 0;
    };
    // 「某天 → 上/下行」聚合（近 N 天用量用）。day 为 yyyy-MM-dd。
    struct DayTotal {
        QString day;
        qint64 up = 0;
        qint64 down = 0;
    };
    // 「某个维度的一项 → 字节」聚合（状态页今日流量卡的 进程/设备/域名 Top N）。
    // key 是库里的原值（mac / process / host），label 是给人看的（设备取备注名或主机名）。
    struct GroupTotal {
        QString key;
        QString label;
        qint64 bytes = 0;
    };
    // 统计口径：全部流量，还是只算走了代理的（chain 既不是 DIRECT 也不是 REJECT）。
    enum class Scope { All, ProxyOnly };
    // 分组维度。
    enum class Dimension { Process, Device, Host };

    HistoryStore(const QString &configDir, DeviceStore *devices, QObject *parent = nullptr);
    ~HistoryStore() override;

    bool isOpen() const { return m_ok; }
    QString databasePath() const { return m_path; }
    // 库里现有记录数（About/设置页想显示时可用；也方便自检）。
    qint64 recordCount() const;

    // 每秒的 /connections 快照（ClashService::connectionsSnapshot 直连此槽）。
    void observe(const QJsonArray &conns);
    // 待写缓冲落盘（定时器 + 退出时调用）。live=true 时把仍在途的连接也各落一条。
    void flush(bool includeLive = false);
    // 删除 keepDays 之前的记录（启动时 + 每 6 小时一次）。
    void purgeOld();

    // —— 查询 ——
    // 某设备最近 days 天的 Top 域名（字节降序）。mac 为空 = 全部设备合计。
    // **已合并仍在途连接的字节**：只查库的话，当前正开着的连接一条都算不进去，刚打开的网页
    // 在「常用域名」里会看不见。
    QVector<DomainTotal> topDomains(const QString &mac, int days, int limit) const;
    // 某设备最近 days 天的按天用量（含今天；没有记录的日子返回 0，保证长度 = days）。
    QVector<DayTotal> dailyTraffic(const QString &mac, int days) const;

    // —— 状态页「今日流量」卡 ——
    // 今日 0 点起按**本地小时**分桶的字节数，定长 24（索引 = 小时；未到的小时是 0）。
    QVector<qint64> todayHourly(Scope scope) const;
    // 今日按维度聚合的 Top N（字节降序）。
    QVector<GroupTotal> todayTop(Dimension dim, Scope scope, int limit) const;

private:
    void openDatabase(const QString &configDir);
    void createSchema();
    // 一条在途连接的最新状态（快照之间保留，消失时转成 Record）。
    struct Live {
        QString mac, host, destIp, chain, network, process;
        qint64 startedAt = 0;
        qint64 up = 0, down = 0;
        bool seen = false; // 本轮快照是否出现过（扫完后未出现的即已断开）
    };
    Record toRecord(const Live &l, qint64 endedAt) const;

    QSqlDatabase m_db;
    QString m_path;
    QString m_connName;   // QSqlDatabase 连接名（避免占用默认连接）
    bool m_ok = false;
    DeviceStore *m_devices = nullptr;

    QHash<QString, Live> m_live; // 连接 id → 在途状态
    QVector<Record> m_pending;   // 待写缓冲
    QTimer *m_flushTimer = nullptr;
    QTimer *m_purgeTimer = nullptr;

    static constexpr int kKeepDays = 30;      // 保留期：30 天（再久没人看，白占磁盘）
    static constexpr int kFlushThreshold = 256; // 缓冲到这么多条就立刻写
    static constexpr int kFlushIntervalMs = 5000;
};
