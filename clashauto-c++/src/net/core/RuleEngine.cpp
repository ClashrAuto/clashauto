#include "RuleEngine.h"

#include <QFile>
#include <QStringList>
#include <QtGlobal>

#include <climits>
#include <cstring>

// 本文件三块：
//   1) RuleGeoDb —— 一个**最小只读 MaxMind DB 阅读器**。MmdbFile 只做校验/落盘、没有查询 API
//      （见 MmdbFile.h），所以 GEOIP 规则要的「IP→ISO 国家码」得自己走一遍 mmdb 的搜索树 + 数据段。
//      只解到 country.iso_code 这一条路径，不做通用 mmdb 解码器。
//   2) RuleSnapshot —— 一份**已编译**的规则表（域名哈希 / 预解析 CIDR / 关键字表 / GEOIP 表 / MATCH），
//      每条都带原表序号 order；match() 取所有命中项里 order 最小者的 target（= mihomo 首命中语义）。
//   3) RuleEngine —— 门面：parse/编译/原子换快照 + 数据面 match()。线程模型见头文件。

// ============================================================================
//  RuleGeoDb —— 最小 MaxMind DB 只读阅读器（IP → ISO 国家码）
// ============================================================================
//
// MaxMind DB 布局：[搜索树][16 字节全零分隔符][数据段][metadata]。metadata 前有魔数
// "\xab\xcd\xef" "MaxMind.com"。搜索树每个 node 含 2 条 record，每条 record_size 位（24/28/32）：
//   record < node_count  → 下一个 node 的编号（继续下沉）
//   record == node_count → 该 IP 无数据
//   record > node_count  → 指向数据段：绝对偏移 = searchTreeSize + (record - node_count)
// 数据段用「控制字节 + 变长」编码；指针型(type 1) 的目标 = dataSectionStart + value。
class RuleGeoDb
{
public:
    // 打开并解析一个 mmdb 文件；任何结构不自洽都返回 null（GEOIP 规则随之恒失配，安全降级）。
    static std::shared_ptr<RuleGeoDb> open(const QString &path)
    {
        auto db = std::shared_ptr<RuleGeoDb>(new RuleGeoDb());
        if (!db->load(path)) {
            return nullptr;
        }
        return db;
    }

    // 查 IP 的 ISO 国家码（如 "CN"）；无数据 / 不支持返回空串。线程安全（纯只读，构造后不改）。
    QString country(const QHostAddress &ip) const
    {
        if (m_nodeCount == 0) {
            return QString();
        }
        quint8 addr[16];
        int nbits = 0;
        const bool v6db = (m_ipVersion == 6);
        if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 v = ip.toIPv4Address();
            if (v6db) {
                // v6 库里查 v4：走 ::a.b.c.d（前 96 位为 0，等价 libmaxminddb 的 ipv4 起始节点路径）。
                std::memset(addr, 0, 16);
                addr[12] = quint8((v >> 24) & 0xFF);
                addr[13] = quint8((v >> 16) & 0xFF);
                addr[14] = quint8((v >> 8) & 0xFF);
                addr[15] = quint8(v & 0xFF);
                nbits = 128;
            } else {
                addr[0] = quint8((v >> 24) & 0xFF);
                addr[1] = quint8((v >> 16) & 0xFF);
                addr[2] = quint8((v >> 8) & 0xFF);
                addr[3] = quint8(v & 0xFF);
                nbits = 32;
            }
        } else if (ip.protocol() == QAbstractSocket::IPv6Protocol) {
            if (!v6db) {
                return QString();
            }
            const Q_IPV6ADDR a6 = ip.toIPv6Address();
            std::memcpy(addr, a6.c, 16);
            nbits = 128;
        } else {
            return QString();
        }

        quint32 node = 0;
        for (int i = 0; i < nbits; ++i) {
            if (node >= m_nodeCount) {
                break;
            }
            const int bit = (addr[i >> 3] >> (7 - (i & 7))) & 1;
            const quint32 rec = readRecord(node, bit);
            if (rec == m_nodeCount) {
                return QString(); // 无数据
            }
            if (rec > m_nodeCount) {
                return decodeCountry(m_searchTreeSize + qsizetype(rec) - qsizetype(m_nodeCount));
            }
            node = rec; // 继续下沉
        }
        return QString();
    }

private:
    RuleGeoDb() = default;

    QByteArray m_data;
    quint32 m_nodeCount = 0;
    quint32 m_recordSize = 0; // bits：24 / 28 / 32
    quint32 m_ipVersion = 0;  // 4 / 6
    qsizetype m_bytesPerNode = 0;
    qsizetype m_searchTreeSize = 0;
    qsizetype m_dataSectionStart = 0; // searchTreeSize + 16
    qsizetype m_ptrBase = 0;          // 指针型解码的基准（数据段解码时 = dataSectionStart）

    quint8 byteAt(qsizetype o) const
    {
        return (o >= 0 && o < m_data.size()) ? quint8(m_data.at(o)) : quint8(0);
    }

    bool load(const QString &path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return false;
        }
        m_data = f.readAll();
        f.close();
        if (m_data.size() < 32) {
            return false;
        }

        static const QByteArray kMarker =
            QByteArrayLiteral("\xab\xcd\xef") + QByteArrayLiteral("MaxMind.com");
        const qsizetype markerPos = m_data.lastIndexOf(kMarker);
        if (markerPos < 0) {
            return false;
        }
        const qsizetype metaStart = markerPos + kMarker.size();

        // metadata 段里的指针相对 metadata 起点。
        m_ptrBase = metaStart;
        const qsizetype nodeCountOff = findKeyInMap(metaStart, QByteArrayLiteral("node_count"));
        const qsizetype recordSizeOff = findKeyInMap(metaStart, QByteArrayLiteral("record_size"));
        const qsizetype ipVersionOff = findKeyInMap(metaStart, QByteArrayLiteral("ip_version"));
        if (nodeCountOff < 0 || recordSizeOff < 0 || ipVersionOff < 0) {
            return false;
        }
        m_nodeCount = quint32(decodeUint(nodeCountOff));
        m_recordSize = quint32(decodeUint(recordSizeOff));
        m_ipVersion = quint32(decodeUint(ipVersionOff));

        if ((m_recordSize != 24 && m_recordSize != 28 && m_recordSize != 32) || m_nodeCount == 0) {
            return false;
        }
        m_bytesPerNode = qsizetype(m_recordSize) / 4;              // 24→6 / 28→7 / 32→8
        m_searchTreeSize = qsizetype(m_nodeCount) * m_bytesPerNode;
        m_dataSectionStart = m_searchTreeSize + 16;
        if (m_dataSectionStart >= metaStart || m_searchTreeSize > m_data.size()) {
            return false; // 树/数据段与文件长度不自洽
        }
        // 之后所有查询都在数据段里，指针基准切到数据段起点。
        m_ptrBase = m_dataSectionStart;
        return true;
    }

    // 读 node 的第 which(0=左/1=右) 条 record，返回其值。
    quint32 readRecord(quint32 node, int which) const
    {
        const qsizetype b = qsizetype(node) * m_bytesPerNode;
        if (m_recordSize == 24) {
            const qsizetype o = b + (which ? 3 : 0);
            return (quint32(byteAt(o)) << 16) | (quint32(byteAt(o + 1)) << 8) | byteAt(o + 2);
        }
        if (m_recordSize == 28) {
            if (which == 0) {
                return (quint32(byteAt(b + 3) & 0xF0) << 20) | (quint32(byteAt(b)) << 16)
                     | (quint32(byteAt(b + 1)) << 8) | byteAt(b + 2);
            }
            return (quint32(byteAt(b + 3) & 0x0F) << 24) | (quint32(byteAt(b + 4)) << 16)
                 | (quint32(byteAt(b + 5)) << 8) | byteAt(b + 6);
        }
        // 32 bit
        const qsizetype o = b + (which ? 4 : 0);
        return (quint32(byteAt(o)) << 24) | (quint32(byteAt(o + 1)) << 16)
             | (quint32(byteAt(o + 2)) << 8) | byteAt(o + 3);
    }

    // 数据段解码原语 —————————————————————————————————————————————

    // 若 off 处是指针型：解出目标绝对偏移(*target)与指针自身占用后的偏移(*after)，返回 true。
    bool asPointer(qsizetype off, qsizetype *target, qsizetype *after) const
    {
        const quint8 ctrl = byteAt(off);
        if ((ctrl >> 5) != 1) {
            return false;
        }
        const int ps = (ctrl >> 3) & 0x3;
        quint32 v = 0;
        if (ps == 0) {
            v = (quint32(ctrl & 0x7) << 8) | byteAt(off + 1);
            *after = off + 2;
        } else if (ps == 1) {
            v = (quint32(ctrl & 0x7) << 16) | (quint32(byteAt(off + 1)) << 8) | byteAt(off + 2);
            v += 2048;
            *after = off + 3;
        } else if (ps == 2) {
            v = (quint32(ctrl & 0x7) << 24) | (quint32(byteAt(off + 1)) << 16)
              | (quint32(byteAt(off + 2)) << 8) | byteAt(off + 3);
            v += 526336;
            *after = off + 4;
        } else {
            v = (quint32(byteAt(off + 1)) << 24) | (quint32(byteAt(off + 2)) << 16)
              | (quint32(byteAt(off + 3)) << 8) | byteAt(off + 4);
            *after = off + 5;
        }
        *target = m_ptrBase + qsizetype(v);
        return true;
    }

    qsizetype resolve(qsizetype off) const
    {
        qsizetype t = 0, a = 0;
        return asPointer(off, &t, &a) ? t : off;
    }

    // 读控制字节：得到 type(1..15) 与 size（payload 长度 / map·array 的元素个数），返回 payload 起点偏移。
    // 指针型(type 1) 不该走这里（size 无意义）；调用方在此之前已 resolve 掉指针。
    qsizetype readCtrl(qsizetype off, int *type, quint32 *size) const
    {
        const quint8 ctrl = byteAt(off);
        ++off;
        int t = ctrl >> 5;
        if (t == 0) { // 扩展类型：真类型 = 7 + 下一个字节
            t = 7 + int(byteAt(off));
            ++off;
        }
        quint32 sz = ctrl & 0x1F;
        if (t == 1) { // 指针：不在这里展开
            *type = 1;
            *size = 0;
            return off;
        }
        if (sz >= 29) {
            if (sz == 29) {
                sz = 29 + quint32(byteAt(off));
                off += 1;
            } else if (sz == 30) {
                sz = 285 + ((quint32(byteAt(off)) << 8) | byteAt(off + 1));
                off += 2;
            } else { // 31
                sz = 65821
                   + ((quint32(byteAt(off)) << 16) | (quint32(byteAt(off + 1)) << 8) | byteAt(off + 2));
                off += 3;
            }
        }
        *type = t;
        *size = sz;
        return off;
    }

    // 跳过一个完整的值，返回其后的偏移。指针只消费自身字节（不追目标）。
    qsizetype skipValue(qsizetype off) const
    {
        qsizetype t = 0, a = 0;
        if (asPointer(off, &t, &a)) {
            return a;
        }
        int type = 0;
        quint32 size = 0;
        qsizetype p = readCtrl(off, &type, &size);
        if (type == 7) { // map：size 个 key/value 对
            for (quint32 i = 0; i < size; ++i) {
                p = skipValue(p);
                p = skipValue(p);
            }
            return p;
        }
        if (type == 11) { // array：size 个元素
            for (quint32 i = 0; i < size; ++i) {
                p = skipValue(p);
            }
            return p;
        }
        if (type == 14) { // bool：size 即取值(0/1)，无 payload
            return p;
        }
        return p + qsizetype(size);
    }

    QByteArray decodeBytes(qsizetype off) const
    {
        off = resolve(off);
        int type = 0;
        quint32 size = 0;
        const qsizetype p = readCtrl(off, &type, &size);
        if ((type == 2 || type == 4) && p >= 0 && p + qsizetype(size) <= m_data.size()) {
            return m_data.mid(int(p), int(size));
        }
        return QByteArray();
    }

    quint64 decodeUint(qsizetype off) const
    {
        off = resolve(off);
        int type = 0;
        quint32 size = 0;
        const qsizetype p = readCtrl(off, &type, &size);
        quint64 v = 0;
        for (quint32 i = 0; i < size; ++i) {
            v = (v << 8) | byteAt(p + qsizetype(i));
        }
        return v;
    }

    // 在 map(off) 里找 key==want 的值，返回该值的偏移（可能是指针，调用方再 resolve）；找不到返回 -1。
    qsizetype findKeyInMap(qsizetype off, const QByteArray &want) const
    {
        off = resolve(off);
        int type = 0;
        quint32 size = 0;
        qsizetype p = readCtrl(off, &type, &size);
        if (type != 7) {
            return -1;
        }
        for (quint32 i = 0; i < size; ++i) {
            if (p < 0 || p >= m_data.size()) {
                return -1;
            }
            const QByteArray key = decodeBytes(p);
            p = skipValue(p); // 跨过 key
            const qsizetype valOff = p;
            p = skipValue(p); // 跨过 value
            if (key == want) {
                return valOff;
            }
        }
        return -1;
    }

    QString decodeCountry(qsizetype dataOff) const
    {
        const qsizetype countryOff = findKeyInMap(dataOff, QByteArrayLiteral("country"));
        if (countryOff < 0) {
            return QString();
        }
        const qsizetype isoOff = findKeyInMap(countryOff, QByteArrayLiteral("iso_code"));
        if (isoOff < 0) {
            return QString();
        }
        return QString::fromUtf8(decodeBytes(isoOff));
    }
};

// ============================================================================
//  RuleSnapshot —— 已编译规则表
// ============================================================================

struct RuleSnapshot
{
    struct Hit {
        int order = INT_MAX;
        QString target;
    };
    struct KeywordRule {
        QString kw; // 已小写
        int order = 0;
        QString target;
    };
    struct CidrRule {
        QHostAddress net;
        int prefix = 0;
        bool v6 = false;
        int order = 0;
        QString target;
    };
    struct GeoRule {
        QString country; // 已大写
        int order = 0;
        QString target;
    };

    QHash<QString, Hit> domainExact;  // key 已小写
    QHash<QString, Hit> domainSuffix; // key 已小写
    QVector<KeywordRule> keywords;
    QVector<CidrRule> cidrs;
    QVector<GeoRule> geoips;
    bool hasMatch = false;
    int matchOrder = INT_MAX;
    QString matchTarget;
    bool needsGeo = false; // 是否含 GEOIP 规则（决定 match 里要不要查国家码）
    // 「最靠前的、**不带 no-resolve** 的 IP 类规则」的序号（没有则 INT_MAX）。
    // ★ 这一项是 matchEx 判「判定不了」的唯一依据，来由见 matchEx 的注释：
    //   mihomo 对不带 no-resolve 的 IP 规则**会先把域名解析成 IP 再比对**；我们手上只有域名时无从得知
    //   它会不会命中，于是只要这样的规则排在我们已有最佳命中之前，整条决策就是**歧义**的。
    //   实测本项目规则表：312 条 IP 类规则里 311 条带 no-resolve（与 mihomo 零分歧），
    //   唯一不带的是 `GEOIP,CN,🎯 全球直连`。
    int minUnresolvedIpOrder = INT_MAX;
};

namespace {

// 把 payload "a.b.c.d/n" / "2001:db8::/32" 解析成 (net, prefix, v6)。失败返回 false。
bool parseCidr(const QString &payload, QHostAddress *net, int *prefix, bool *v6)
{
    const int slash = payload.indexOf('/');
    const QString ipPart = slash >= 0 ? payload.left(slash) : payload;
    QHostAddress a;
    if (!a.setAddress(ipPart.trimmed())) {
        return false;
    }
    const bool isV6 = (a.protocol() == QAbstractSocket::IPv6Protocol);
    int pfx = isV6 ? 128 : 32;
    if (slash >= 0) {
        bool ok = false;
        pfx = payload.mid(slash + 1).trimmed().toInt(&ok);
        if (!ok || pfx < 0 || pfx > (isV6 ? 128 : 32)) {
            return false;
        }
    }
    *net = a;
    *prefix = pfx;
    *v6 = isV6;
    return true;
}

bool v4InCidr(quint32 ip, quint32 net, int prefix)
{
    if (prefix <= 0) {
        return true;
    }
    if (prefix >= 32) {
        return ip == net;
    }
    const quint32 mask = 0xFFFFFFFFu << (32 - prefix);
    return (ip & mask) == (net & mask);
}

bool v6InCidr(const Q_IPV6ADDR &ip, const Q_IPV6ADDR &net, int prefix)
{
    int fullBytes = prefix / 8;
    const int remBits = prefix % 8;
    for (int i = 0; i < fullBytes; ++i) {
        if (ip.c[i] != net.c[i]) {
            return false;
        }
    }
    if (remBits) {
        const quint8 mask = quint8(0xFF << (8 - remBits));
        if ((ip.c[fullBytes] & mask) != (net.c[fullBytes] & mask)) {
            return false;
        }
    }
    return true;
}

} // namespace

// ============================================================================
//  RuleEngine
// ============================================================================

RuleEngine::RuleEngine() = default;
RuleEngine::~RuleEngine() = default;

RuleEngine::Type RuleEngine::typeFromString(const QString &s)
{
    const QString u = s.trimmed().toUpper();
    if (u == QLatin1String("DOMAIN")) return Type::Domain;
    if (u == QLatin1String("DOMAIN-SUFFIX")) return Type::DomainSuffix;
    if (u == QLatin1String("DOMAIN-KEYWORD")) return Type::DomainKeyword;
    if (u == QLatin1String("IP-CIDR")) return Type::IpCidr;
    if (u == QLatin1String("IP-CIDR6")) return Type::IpCidr6;
    if (u == QLatin1String("GEOIP")) return Type::GeoIp;
    if (u == QLatin1String("MATCH")) return Type::Match;
    return Type::Unknown;
}

QString RuleEngine::typeToString(Type t)
{
    switch (t) {
    case Type::Domain: return QStringLiteral("DOMAIN");
    case Type::DomainSuffix: return QStringLiteral("DOMAIN-SUFFIX");
    case Type::DomainKeyword: return QStringLiteral("DOMAIN-KEYWORD");
    case Type::IpCidr: return QStringLiteral("IP-CIDR");
    case Type::IpCidr6: return QStringLiteral("IP-CIDR6");
    case Type::GeoIp: return QStringLiteral("GEOIP");
    case Type::Match: return QStringLiteral("MATCH");
    case Type::Unknown: break;
    }
    return QStringLiteral("UNKNOWN");
}

// 解 YAML **双引号**标量里的转义序列。只做真会遇到的那几种。
// （声明在 RuleEngine.h：ProxyConfigBuilder 解析 proxy-groups 组名时要用同一份实现。）
//
// ★★ 为什么非做不可：ConfigBuilder 生成的 full.yaml 把策略组名写成转义形式 ——
//     `- "GEOIP,CN,\U0001F3AF 全球直连"`。剥掉引号后我们拿到的 target 是**字面量**
//     `\U0001F3AF 全球直连`（14 个 ASCII 字符 + 中文），而 groupToNode 的键来自核心 REST API，
//     是**真正的 emoji** `🎯 全球直连`。两者永远匹配不上 → resolveTarget 恒返回空 →
//     **凡是 emoji 命名的策略组，规则一律解析失败、整类流量回退 mihomo**。
//     真实订阅几乎清一色用 emoji 组名，所以这一条卡死了 Rule 模式的绝大部分进程内路由
//     （真机诊断：cc=4/7/…，CCROUTE 显示 target=🎯 全球直连 而 node= 空）。
//   只在**双引号**标量里解：单引号 YAML 标量里反斜杠是字面量，解了反而错。
QString decodeYamlEscapes(const QString &in)
{
    if (!in.contains(QLatin1Char('\\')))
        return in; // 绝大多数规则没有转义，直接走
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= in.size()) {
            out.append(c);
            continue;
        }
        const QChar e = in.at(++i);
        int hex = 0;
        switch (e.unicode()) {
        case 'U': hex = 8; break;   // \U0001F3AF —— 码点，可能超出 BMP（emoji 全在这一档）
        case 'u': hex = 4; break;   // \u4E2D
        case 'x': hex = 2; break;   // \x41
        case 'n': out.append(QLatin1Char('\n')); continue;
        case 't': out.append(QLatin1Char('\t')); continue;
        case 'r': out.append(QLatin1Char('\r')); continue;
        case '0': out.append(QChar(0)); continue;
        case '\\': case '"': case '\'': case '/': out.append(e); continue;
        default:
            // 不认识的转义：**原样保留**（连反斜杠一起），绝不吞字符 —— 宁可不解也别改错规则。
            out.append(QLatin1Char('\\')).append(e);
            continue;
        }
        if (i + hex >= in.size()) { // 位数不够 = 不是合法转义，原样保留
            out.append(QLatin1Char('\\')).append(e);
            continue;
        }
        bool ok = false;
        const uint cp = in.mid(i + 1, hex).toUInt(&ok, 16);
        if (!ok || cp > 0x10FFFF) {
            out.append(QLatin1Char('\\')).append(e);
            continue;
        }
        out.append(QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1));
        i += hex;
    }
    return out;
}

RuleEngine::Rule RuleEngine::parseRule(const QString &line)
{
    Rule r;
    QString s = line.trimmed();
    if (s.startsWith('-')) { // 容忍 YAML 列表项前缀 "- "
        s = s.mid(1).trimmed();
    }
    if (s.isEmpty() || s.startsWith('#')) {
        return r; // Unknown
    }
    // ★ 必须剥掉整条规则外面的引号。ConfigBuilder 生成的 full.yaml 把**每一条**规则都写成带引号的
    //   标量（`- "DOMAIN-SUFFIX,51.la,🚀 节点选择"`，因为 target 里有 emoji/空格）。不剥的话第一段
    //   变成 `"DOMAIN-SUFFIX`，typeFromString 认不出 → 整表 1393 条全成 Unknown 被丢掉 → 规则表为空、
    //   连 MATCH 兜底都没有 → Rule 模式的进程内路由永远命中不了（真机联调就是这么发现的：
    //   诊断显示 target 恒空、needsResolve 恒 false）。单引号同理。
    if (s.size() >= 2 && ((s.startsWith('"') && s.endsWith('"'))
                          || (s.startsWith('\'') && s.endsWith('\'')))) {
        const bool wasDoubleQuoted = s.startsWith('"');
        s = s.mid(1, s.size() - 2).trimmed();
        if (s.isEmpty()) {
            return r;
        }
        // 双引号标量里的 \Uxxxxxxxx / \uxxxx 必须解开，否则 emoji 组名永远匹配不上（见 decodeYamlEscapes）。
        if (wasDoubleQuoted) {
            s = decodeYamlEscapes(s);
        }
    }
    const QStringList f = s.split(',');
    if (f.isEmpty()) {
        return r;
    }
    const Type t = typeFromString(f.at(0));
    r.type = t;
    if (t == Type::Match) {
        if (f.size() >= 2) {
            r.target = f.at(1).trimmed();
        } else {
            r.type = Type::Unknown;
        }
        return r;
    }
    if (t == Type::Unknown) {
        return r;
    }
    if (f.size() < 3) {
        r.type = Type::Unknown;
        return r;
    }
    r.payload = f.at(1).trimmed();
    r.target = f.at(2).trimmed();
    for (int i = 3; i < f.size(); ++i) {
        if (f.at(i).trimmed().compare(QLatin1String("no-resolve"), Qt::CaseInsensitive) == 0) {
            r.noResolve = true;
        }
    }
    return r;
}

void RuleEngine::setRules(QVector<Rule> rules)
{
    auto snap = std::make_shared<RuleSnapshot>();
    int order = 0;
    for (const Rule &r : rules) {
        const int o = order++;
        switch (r.type) {
        case Type::Domain: {
            const QString k = r.payload.toLower();
            RuleSnapshot::Hit &h = snap->domainExact[k];
            if (o < h.order) { // 保首命中：同 key 取更靠前的
                h.order = o;
                h.target = r.target;
            }
            break;
        }
        case Type::DomainSuffix: {
            const QString k = r.payload.toLower();
            RuleSnapshot::Hit &h = snap->domainSuffix[k];
            if (o < h.order) {
                h.order = o;
                h.target = r.target;
            }
            break;
        }
        case Type::DomainKeyword:
            snap->keywords.push_back({r.payload.toLower(), o, r.target});
            break;
        case Type::IpCidr:
        case Type::IpCidr6: {
            QHostAddress net;
            int prefix = 0;
            bool v6 = false;
            if (parseCidr(r.payload, &net, &prefix, &v6)) {
                snap->cidrs.push_back({net, prefix, v6, o, r.target});
                if (!r.noResolve && o < snap->minUnresolvedIpOrder) {
                    snap->minUnresolvedIpOrder = o; // 见 minUnresolvedIpOrder 的说明
                }
            }
            break;
        }
        case Type::GeoIp:
            snap->geoips.push_back({r.payload.toUpper(), o, r.target});
            snap->needsGeo = true;
            if (!r.noResolve && o < snap->minUnresolvedIpOrder) {
                snap->minUnresolvedIpOrder = o; // 同上（本项目里正是这条 GEOIP,CN 触发）
            }
            break;
        case Type::Match:
            if (o < snap->matchOrder) {
                snap->matchOrder = o;
                snap->matchTarget = r.target;
                snap->hasMatch = true;
            }
            break;
        case Type::Unknown:
            break; // 丢弃
        }
    }
    QMutexLocker lock(&m_mutex);
    m_snapshot = std::move(snap);
}

void RuleEngine::setRulesFromLines(const QStringList &lines)
{
    QVector<Rule> rules;
    rules.reserve(lines.size());
    for (const QString &line : lines) {
        Rule r = parseRule(line);
        if (r.type != Type::Unknown) {
            rules.push_back(r);
        }
    }
    setRules(std::move(rules));
}

void RuleEngine::setGeoipDatabasePath(const QString &mmdbPath)
{
    std::shared_ptr<const RuleGeoDb> geo;
    if (!mmdbPath.isEmpty()) {
        geo = RuleGeoDb::open(mmdbPath); // 打不开/不合法 → 保持 null（GEOIP 恒失配）
    }
    QMutexLocker lock(&m_mutex);
    m_geo = std::move(geo);
}

void RuleEngine::setCountryResolver(CountryResolver resolver)
{
    QMutexLocker lock(&m_mutex);
    m_resolver = std::move(resolver);
}

std::shared_ptr<const RuleSnapshot> RuleEngine::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    return m_snapshot;
}

QString RuleEngine::lookupCountry(const QHostAddress &ip) const
{
    CountryResolver res;
    std::shared_ptr<const RuleGeoDb> geo;
    {
        QMutexLocker lock(&m_mutex);
        res = m_resolver;
        geo = m_geo;
    }
    if (res) {
        return res(ip); // 外部注入优先
    }
    if (geo) {
        return geo->country(ip);
    }
    return QString();
}

QString RuleEngine::match(const QString &host, const QHostAddress &ip) const
{
    return matchEx(host, ip).target; // 老接口：只要结论，不区分「判定不了」
}

RuleEngine::MatchOutcome RuleEngine::matchEx(const QString &host, const QHostAddress &ip) const
{
    const std::shared_ptr<const RuleSnapshot> snap = snapshot();
    if (!snap) {
        return {};
    }

    int bestOrder = INT_MAX;
    QString bestTarget;
    auto consider = [&](int order, const QString &target) {
        if (order < bestOrder) {
            bestOrder = order;
            bestTarget = target;
        }
    };

    // —— 域名规则 ——
    if (!host.isEmpty()) {
        const QString h = host.toLower();
        const auto exact = snap->domainExact.constFind(h);
        if (exact != snap->domainExact.constEnd()) {
            consider(exact->order, exact->target);
        }
        // 后缀：host 自身及每一级父域（按标签边界）
        QString cur = h;
        for (;;) {
            const auto sit = snap->domainSuffix.constFind(cur);
            if (sit != snap->domainSuffix.constEnd()) {
                consider(sit->order, sit->target);
            }
            const int dot = cur.indexOf('.');
            if (dot < 0) {
                break;
            }
            cur = cur.mid(dot + 1);
        }
        // 关键字：只能线性扫
        for (const RuleSnapshot::KeywordRule &k : snap->keywords) {
            if (k.order < bestOrder && h.contains(k.kw)) {
                consider(k.order, k.target);
            }
        }
    }

    // —— IP / GEOIP 规则 ——
    if (!ip.isNull()) {
        const bool ipIsV6 = (ip.protocol() == QAbstractSocket::IPv6Protocol);
        if (ipIsV6) {
            const Q_IPV6ADDR v6 = ip.toIPv6Address();
            for (const RuleSnapshot::CidrRule &c : snap->cidrs) {
                if (c.v6 && c.order < bestOrder && v6InCidr(v6, c.net.toIPv6Address(), c.prefix)) {
                    consider(c.order, c.target);
                }
            }
        } else if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 v4 = ip.toIPv4Address();
            for (const RuleSnapshot::CidrRule &c : snap->cidrs) {
                if (!c.v6 && c.order < bestOrder
                    && v4InCidr(v4, c.net.toIPv4Address(), c.prefix)) {
                    consider(c.order, c.target);
                }
            }
        }
        if (snap->needsGeo) {
            const QString country = lookupCountry(ip); // 每次 match 至多查一次
            if (!country.isEmpty()) {
                for (const RuleSnapshot::GeoRule &g : snap->geoips) {
                    if (g.order < bestOrder && g.country == country) {
                        consider(g.order, g.target);
                    }
                }
            }
        }
    }

    // —— MATCH 兜底 ——
    if (snap->hasMatch) {
        consider(snap->matchOrder, snap->matchTarget);
    }

    // ★★ 「判定不了」的判据（务必与 mihomo 的首命中语义对齐）★★
    //   mihomo 遇到**不带 no-resolve** 的 IP 类规则时，会把域名解析成 IP 再比对；我们此刻只有域名
    //   （ip 为空），无从得知它会不会命中。只要这样的规则**排在我们最佳命中之前**，谁赢就取决于那次
    //   解析结果 —— 这就是歧义。此时绝不能拿自己的结论去路由（那可能与核心不一致 = 误路由），
    //   而应告诉调用方「我判不了」，让它回退核心（核心会解析，结论必然正确）。
    //   带 no-resolve 的 IP 规则在无 ip 时 mihomo 同样跳过，故上面直接跳过=零分歧，不算歧义。
    if (ip.isNull() && snap->minUnresolvedIpOrder < bestOrder) {
        return {QString(), true};
    }
    return {(bestOrder == INT_MAX) ? QString() : bestTarget, false};
}

bool RuleEngine::selfTest()
{
    RuleEngine e;
    e.setRulesFromLines(QStringList{
        QStringLiteral("DOMAIN-SUFFIX,google.com,PROXY"),
        QStringLiteral("DOMAIN,example.com,DIRECT"),
        QStringLiteral("DOMAIN-KEYWORD,facebook,PROXY"),
        QStringLiteral("IP-CIDR,192.168.0.0/16,DIRECT,no-resolve"),
        QStringLiteral("IP-CIDR6,fd00::/8,DIRECT,no-resolve"),
        QStringLiteral("MATCH,FALLBACK"),
    });

    struct Case {
        QString host;
        QString ip;
        QString want;
    };
    const Case cases[] = {
        {QStringLiteral("www.google.com"), QString(), QStringLiteral("PROXY")},
        {QStringLiteral("google.com"), QString(), QStringLiteral("PROXY")},
        {QStringLiteral("notgoogle.com"), QString(), QStringLiteral("FALLBACK")}, // 边界：非子域不命中后缀
        {QStringLiteral("example.com"), QString(), QStringLiteral("DIRECT")},
        {QStringLiteral("sub.example.com"), QString(), QStringLiteral("FALLBACK")}, // DOMAIN 精确不含子域
        {QStringLiteral("x.facebook.net"), QString(), QStringLiteral("PROXY")},
        {QString(), QStringLiteral("192.168.1.1"), QStringLiteral("DIRECT")},
        {QString(), QStringLiteral("8.8.8.8"), QStringLiteral("FALLBACK")},
        {QString(), QStringLiteral("fd00::1"), QStringLiteral("DIRECT")},
        {QString(), QStringLiteral("2001:4860::1"), QStringLiteral("FALLBACK")},
    };

    bool ok = true;
    for (const Case &c : cases) {
        const QHostAddress ip = c.ip.isEmpty() ? QHostAddress() : QHostAddress(c.ip);
        const QString got = e.match(c.host, ip);
        if (got != c.want) {
            ok = false;
            qWarning("RuleEngine::selfTest FAIL host=%s ip=%s got=%s want=%s",
                     qUtf8Printable(c.host), qUtf8Printable(c.ip), qUtf8Printable(got),
                     qUtf8Printable(c.want));
        }
    }

    // ⑤★ **YAML 转义**必须解开：ConfigBuilder 把组名写成 "\U0001F3AF 全球直连"，而 groupToNode 的
    //    键来自核心 REST API、是真 emoji。不解转义两边永远匹配不上 → 凡是 emoji 命名的策略组，
    //    规则全部解析失败、整类流量回退 mihomo（真机诊断 cc=4/7/… 就是这么定位到的）。
    //    真实订阅几乎清一色用 emoji 组名，所以这条一错，Rule 模式的进程内路由基本全废。
    {
        const Rule esc = parseRule(QString::fromUtf8("\"GEOIP,CN,\\U0001F3AF 全球直连\""));
        const QString want = QString::fromUtf8("\xF0\x9F\x8E\xAF 全球直连");
        if (esc.target != want) {
            qWarning("selfTest FAIL: YAML \\U 转义没解开 (got=%s)", qUtf8Printable(esc.target));
            return false;
        }
        const Rule u4 = parseRule(QString::fromUtf8("\"MATCH,\\u4E2D国\""));
        if (u4.target != QString::fromUtf8("中国")) {
            qWarning("selfTest FAIL: \\u 转义没解开 (got=%s)", qUtf8Printable(u4.target));
            return false;
        }
        // 不认识的转义要**原样保留**（宁可不解，也别把字符吞掉改错规则）
        const Rule keep = parseRule(QString::fromUtf8("\"MATCH,a\\qb\""));
        if (keep.target != QString::fromUtf8("a\\qb")) {
            qWarning("selfTest FAIL: 不认识的转义应原样保留 (got=%s)", qUtf8Printable(keep.target));
            return false;
        }
        // 单引号 YAML 标量里反斜杠是字面量，**不该**解
        const Rule sq = parseRule(QString::fromUtf8("'MATCH,a\\u4E2Db'"));
        if (!sq.target.contains(QString::fromUtf8("\\u4E2D"))) {
            qWarning("selfTest FAIL: 单引号标量不该解转义 (got=%s)", qUtf8Printable(sq.target));
            return false;
        }
    }

    // ⑥ **带引号的规则行**必须能解析：ConfigBuilder 生成的 full.yaml 每条规则都是带引号的标量
    //    （target 含 emoji/空格），真机上就是因为没剥引号导致整表被丢、Rule 模式恒不命中。
    {
        RuleEngine e3;
        e3.setRulesFromLines(QStringList{
            QStringLiteral("  - \"DOMAIN-SUFFIX,telegram.org,ð èç¹éæ©\""),
            QStringLiteral("  - \"IP-CIDR,91.108.0.0/16,PROXY,no-resolve\""),
            QStringLiteral("  - \"MATCH,FALLBACK\""),
        });
        const QString hit = e3.match(QStringLiteral("x.telegram.org"), QHostAddress());
        if (hit.isEmpty() || hit == QStringLiteral("FALLBACK")) {
            ok = false;
            qWarning("selfTest FAIL: 带引号的规则行没解析出来 (got=%s)", qUtf8Printable(hit));
        }
        const QString ipHit = e3.match(QString(), QHostAddress(QStringLiteral("91.108.4.5")));
        if (ipHit != QStringLiteral("PROXY")) {
            ok = false;
            qWarning("selfTest FAIL: 带引号的 IP-CIDR 规则没命中 (got=%s)", qUtf8Printable(ipHit));
        }
        if (e3.match(QStringLiteral("nothing.example"), QHostAddress()) != QStringLiteral("FALLBACK")) {
            ok = false;
            qWarning("selfTest FAIL: 带引号的 MATCH 兜底没生效");
        }
    }

    // —— matchEx 的「判定不了」语义（见 matchEx 注释）——
    // ① 上面这张表里所有 IP 规则都带 no-resolve → 只有域名时它们被跳过，决策仍然是确定的：
    //    needsResolve 必须恒为 false（否则等于把本可进程内的流量白白推回核心）。
    {
        const RuleEngine::MatchOutcome mo = e.matchEx(QStringLiteral("notgoogle.com"), QHostAddress());
        if (mo.needsResolve || mo.target != QStringLiteral("FALLBACK")) {
            ok = false;
            qWarning("selfTest FAIL: no-resolve 的 IP 规则在无 ip 时应被跳过并落到 MATCH "
                     "(got target=%s needsResolve=%d)",
                     qUtf8Printable(mo.target), int(mo.needsResolve));
        }
    }
    // ② 有一条**不带** no-resolve 的 IP 规则排在 MATCH 之前：只有域名时判不了 → needsResolve=true 且 target 空。
    {
        RuleEngine e2;
        e2.setRulesFromLines(QStringList{
            QStringLiteral("DOMAIN-SUFFIX,google.com,PROXY"),
            QStringLiteral("GEOIP,CN,DIRECT"), // 不带 no-resolve：mihomo 会先解析再判
            QStringLiteral("MATCH,FALLBACK"),
        });
        const RuleEngine::MatchOutcome amb = e2.matchEx(QStringLiteral("unknown-host.example"),
                                                        QHostAddress());
        if (!amb.needsResolve || !amb.target.isEmpty()) {
            ok = false;
            qWarning("selfTest FAIL: 不带 no-resolve 的 IP 规则在无 ip 时应报 needsResolve "
                     "(got target=%s needsResolve=%d)",
                     qUtf8Printable(amb.target), int(amb.needsResolve));
        }
        // ③ 命中更靠前的域名规则时不算歧义（那条 IP 规则排在后面，赢不了）。
        const RuleEngine::MatchOutcome dec = e2.matchEx(QStringLiteral("www.google.com"),
                                                        QHostAddress());
        if (dec.needsResolve || dec.target != QStringLiteral("PROXY")) {
            ok = false;
            qWarning("selfTest FAIL: 更靠前的域名命中应直接判定 (got target=%s needsResolve=%d)",
                     qUtf8Printable(dec.target), int(dec.needsResolve));
        }
        // ④ 给了 ip 就不存在歧义（IP 规则能真判）：CN 段的 IP 应落 DIRECT。
        const RuleEngine::MatchOutcome withIp = e2.matchEx(QString(),
                                                           QHostAddress(QStringLiteral("114.114.114.114")));
        if (withIp.needsResolve) {
            ok = false;
            qWarning("selfTest FAIL: 有 ip 时不应报 needsResolve");
        }
    }
    return ok;
}
