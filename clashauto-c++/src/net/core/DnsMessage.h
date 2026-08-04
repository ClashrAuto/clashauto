#pragma once

// DNS 报文层 —— **纯函数**，只做「解析设备发来的查询」与「合成回给设备的应答」两件事。
//
// ★ 存在的理由：网关此前把设备的每一条 :53 查询**原样转投 mihomo 的 fake-ip DNS**（见 NetStack 的
//   hijackDns），于是即便进程内出站开着，**DNS 这一环仍然离不开核心**。要做到「整条数据面不需要
//   mihomo」，就得自己出应答：判定该走代理的域名当场发一个 fake-ip 回去（设备随后连这个假 IP，栈在
//   accept 时反查回域名交给出站），其余的转发给上游。fake-ip 的分配/反查由 DnsResolver 负责，本文件
//   只管**线格式**。拆开是因为报文解析是最容易写错、也最该被单测钉住的部分（见 dnsSelfTest）。
//
// 只实现网关真正会遇到的那一小撮：单问题段的标准查询、A/AAAA/其它类型的判别、合成一条 A 应答、
// 合成「NOERROR 但无记录」的空应答。不做压缩指针的**写**（应答里问题段原样拷贝、答案段用指向问题段
// 的 0xC00C 指针，这是所有实现都认的最小形态）。
#include <QByteArray>
#include <QHostAddress>
#include <QString>

namespace coastcore {

// DNS 记录类型（只列本文件用得上的）
enum : quint16 {
    kDnsTypeA = 1,
    kDnsTypeAAAA = 28,
    kDnsTypePTR = 12,
};

struct DnsQuestion
{
    bool ok = false;          // 解析是否成功（失败时其余字段无意义）
    quint16 id = 0;           // 事务 ID
    QString qname;            // 已去掉末尾点的域名（小写）
    quint16 qtype = 0;        // A / AAAA / …
    quint16 qclass = 0;       // 一般是 1(IN)
    int questionEnd = 0;      // 问题段结束偏移（= 12 + qname 编码长 + 4），合成应答时要原样拷到这里
    bool recursionDesired = false;
};

// 解析一条 DNS 查询报文。只接受「QDCOUNT==1 且是标准查询(QR=0, OPCODE=0)」；其余一律 ok=false，
// 调用方据此走「转发上游」这条保守路径 —— 我们只在**完全看得懂**的报文上自作主张。
DnsQuestion parseDnsQuestion(const QByteArray &wire);

// 合成一条 A 应答：问题段原样回抄 + 一条指向问题段的 A 记录。ttl 秒。
// 只在 q.qtype==A 时调用（调用方保证）。
QByteArray buildDnsAnswerA(const QByteArray &query, const DnsQuestion &q, const QHostAddress &ip,
                           quint32 ttl);

// 合成「NOERROR 但零条记录」的应答（RFC 2308 的 NODATA 形态）。
// ★ 用途：fake-ip 池是 v4 的，对启用 fake-ip 的域名收到 AAAA 查询时回它 —— 设备据此回落 v4，
//   于是仍然走 fake-ip。**不能回 NXDOMAIN**：那会让设备认为域名不存在，连 v4 都不试了。
QByteArray buildDnsNoData(const QByteArray &query, const DnsQuestion &q, quint32 ttl);

// 合成 SERVFAIL（上游失败时用；设备会自己重试/换服务器，比干等超时体面）。
QByteArray buildDnsServFail(const QByteArray &query, const DnsQuestion &q);

// 报文层自检（KAT）：解析/合成两个方向逐字节比对。返回 false 时 why 写明哪一条不符。
// 报文解析写错是静默的（表现为「上网时好时坏」），所以这一层必须有向量钉住。
bool dnsSelfTest(QString *why = nullptr);

} // namespace coastcore
