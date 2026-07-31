#include "ProxyConfigBuilder.h"

#include "RuleEngine.h" // decodeYamlEscapes：组名的 emoji 转义与规则那边共用一份实现

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <cstdio>

namespace coastcore {

namespace {

// —— 标量去引号 + 反转义 ——
// YAML 值可能是 "双引号"（SubParser 的 yq 产物，转义了 \ " \n）、'单引号' 或裸标量。统一还原成明文。
QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) {
        QString inner = v.mid(1, v.size() - 2);
        inner.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
        inner.replace(QStringLiteral("\\\""), QStringLiteral("\""));
        inner.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        return inner;
    }
    if (v.size() >= 2 && v.startsWith('\'') && v.endsWith('\'')) {
        return v.mid(1, v.size() - 2);
    }
    return v;
}

bool truthy(const QString &v)
{
    const QString t = unquote(v).trimmed().toLower();
    return t == QStringLiteral("true") || t == QStringLiteral("1");
}

// 从一段 inline flow 容器（如 `{path: "/", headers: {Host: "x"}}`）里按 key 抽标量值。
// key 前必须是行首 / `{` / `,` / 空白（保证匹配的是「一个键」而不是某个键的子串），值支持 "双引号"、
// '单引号' 或裸（到下一个 , } ] 为止）。找不到返回空。用于 ws-opts / reality-opts 的单行写法。
QString flowScalar(const QString &container, const QString &key)
{
    static const QString pat =
        QStringLiteral("(?:^|[,{\\s])%1\\s*:\\s*(?:\"((?:[^\"\\\\]|\\\\.)*)\"|'([^']*)'|([^,}\\]]+))");
    QRegularExpression re(pat.arg(QRegularExpression::escape(key)),
                          QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(container);
    if (!m.hasMatch()) {
        return {};
    }
    if (m.hasCaptured(1)) {
        return unquote('"' + m.captured(1) + '"'); // 走一遍反转义
    }
    if (m.hasCaptured(2)) {
        return m.captured(2);
    }
    return m.captured(3).trimmed();
}

// 从块式（多行）嵌套的行集合里按 key 抽标量值。用于极少数 raw clash yaml 里 ws-opts/reality-opts 写成
// 展开块（`ws-opts:` 换行后 `  path: /` / `  headers:` / `    Host: x`）的情形——两种真实产出（A/B）都用
// 单行 flow，这里只是把「直接透传的 clash yaml」也顺带吃下。
QString blockScalar(const QStringList &lines, const QString &key)
{
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        const int c = t.indexOf(':');
        if (c < 0) {
            continue;
        }
        if (t.left(c).trimmed().compare(key, Qt::CaseInsensitive) == 0) {
            return unquote(t.mid(c + 1));
        }
    }
    return {};
}

// alpn 值 → 逗号拼接明文。既吃单行 list（`[h3, h2]`），也吃裸标量（`h3`）。
QString parseAlpn(const QString &raw)
{
    QString v = raw.trimmed();
    if (v.startsWith('[') && v.endsWith(']')) {
        v = v.mid(1, v.size() - 2);
    }
    QStringList out;
    for (const QString &part : v.split(',', Qt::SkipEmptyParts)) {
        const QString s = unquote(part);
        if (!s.trimmed().isEmpty()) {
            out << s.trimmed();
        }
    }
    return out.join(QStringLiteral(","));
}

// 一条 proxy 的原始行（首行是 `- ...` 标记行，随后是同级字段行 / 更深的块式子行）→ 键值。
// top：顶层字段 key -> 原始值文本（未去引号）。blocks：块式嵌套 key -> 其下更深缩进的原始行集合。
struct NodeFields
{
    QMap<QString, QString> top;
    QMap<QString, QStringList> blocks;
};

NodeFields scanNode(const QStringList &rawLines)
{
    NodeFields nf;
    if (rawLines.isEmpty()) {
        return nf;
    }
    // 把首行 `<indent>- <content>` 归一成 `<indent+2><content>`，让它和后续字段行同一缩进层。
    QStringList lines = rawLines;
    {
        const QString &first = lines.first();
        const int markerIndent = first.size() - QStringView{first}.trimmed().size();
        const int dash = first.indexOf('-', markerIndent);
        QString content = (dash >= 0) ? first.mid(dash + 1) : first;
        if (content.startsWith(' ')) {
            content = content.mid(1);
        }
        lines[0] = QString(markerIndent + 2, ' ') + content;
    }

    int fieldIndent = -1;
    QString currentKey;
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        const int indent = line.size() - QStringView{line}.trimmed().size();
        if (fieldIndent < 0) {
            fieldIndent = indent;
        }
        if (indent > fieldIndent) {
            // 更深缩进 → 属于上一个顶层字段的块式子行
            if (!currentKey.isEmpty()) {
                nf.blocks[currentKey] << trimmed;
            }
            continue;
        }
        const int c = trimmed.indexOf(':');
        if (c < 0) {
            currentKey.clear();
            continue;
        }
        const QString key = trimmed.left(c).trimmed();
        const QString val = trimmed.mid(c + 1).trimmed();
        nf.top[key] = val;
        currentKey = key;
    }
    return nf;
}

// type 归一（对齐 proto/*Outbound 注册的 type；带 reality-opts 的 vless 归一为 "reality"）。
QString normalizeType(const QString &rawTypeLower, bool hasReality)
{
    const QString t = rawTypeLower;
    if (t == QStringLiteral("ss") || t == QStringLiteral("shadowsocks")) {
        return QStringLiteral("ss");
    }
    if (t == QStringLiteral("vmess")) {
        return QStringLiteral("vmess");
    }
    if (t == QStringLiteral("vless")) {
        return hasReality ? QStringLiteral("reality") : QStringLiteral("vless");
    }
    if (t == QStringLiteral("trojan")) {
        return QStringLiteral("trojan");
    }
    if (t == QStringLiteral("hysteria2") || t == QStringLiteral("hy2")) {
        return QStringLiteral("hysteria2");
    }
    if (t == QStringLiteral("tuic")) {
        return QStringLiteral("tuic");
    }
    return rawTypeLower; // 其它 / 认不出：保留原 type，拨号侧因未注册而回退 mihomo
}

// NodeFields → ProxyNode。成功返回 true；缺 name/server/合法 port 返回 false（调用方据此跳过并计数）。
bool toNode(const NodeFields &nf, ProxyNode *out)
{
    const QString name = unquote(nf.top.value(QStringLiteral("name")));
    const QString server = unquote(nf.top.value(QStringLiteral("server")));
    bool portOk = false;
    const int port = unquote(nf.top.value(QStringLiteral("port"))).toInt(&portOk);
    if (name.isEmpty() || server.isEmpty() || !portOk || port <= 0 || port > 65535) {
        return false;
    }

    ProxyNode n;
    n.name = name;
    n.server = server;
    n.port = static_cast<quint16>(port);

    // reality-opts（单行 flow 或块式）
    const QString roInline = nf.top.value(QStringLiteral("reality-opts"));
    const QStringList roBlock = nf.blocks.value(QStringLiteral("reality-opts"));
    const bool hasReality = !roInline.trimmed().isEmpty() || !roBlock.isEmpty();
    auto roGet = [&](const QString &k) {
        return roInline.trimmed().isEmpty() ? blockScalar(roBlock, k) : flowScalar(roInline, k);
    };

    const QString rawType = nf.top.value(QStringLiteral("type")).trimmed().toLower();
    n.type = normalizeType(rawType, hasReality);

    n.password = unquote(nf.top.value(QStringLiteral("password")));
    n.cipher = unquote(nf.top.value(QStringLiteral("cipher"))); // ss 的 cipher / vmess 的 security（clash 字段名 cipher）
    n.username = unquote(nf.top.value(QStringLiteral("username")));
    n.uuid = unquote(nf.top.value(QStringLiteral("uuid")));

    const QString sni = nf.top.value(QStringLiteral("sni"));
    n.sni = unquote(sni.isEmpty() ? nf.top.value(QStringLiteral("servername")) : sni);

    // TLS：显式 tls:true，或协议本身恒 TLS（trojan / reality）。
    n.tls = truthy(nf.top.value(QStringLiteral("tls")))
            || n.type == QStringLiteral("trojan") || n.type == QStringLiteral("reality");
    n.skipCertVerify = truthy(nf.top.value(QStringLiteral("skip-cert-verify")));

    QString net = nf.top.value(QStringLiteral("network")).trimmed().toLower();
    n.network = (net == QStringLiteral("tcp")) ? QString() : net; // 空 = tcp

    // ws-opts（单行 flow 或块式）→ wsPath / wsHost
    const QString wsInline = nf.top.value(QStringLiteral("ws-opts"));
    const QStringList wsBlock = nf.blocks.value(QStringLiteral("ws-opts"));
    if (!wsInline.trimmed().isEmpty()) {
        n.wsPath = flowScalar(wsInline, QStringLiteral("path"));
        n.wsHost = flowScalar(wsInline, QStringLiteral("Host"));
    } else if (!wsBlock.isEmpty()) {
        n.wsPath = blockScalar(wsBlock, QStringLiteral("path"));
        n.wsHost = blockScalar(wsBlock, QStringLiteral("Host"));
    }

    n.alpn = parseAlpn(nf.top.value(QStringLiteral("alpn")));

    // fingerprint：顶层 client-fingerprint 优先，回退 reality-opts.client-fingerprint。
    QString fp = unquote(nf.top.value(QStringLiteral("client-fingerprint")));
    if (fp.isEmpty()) {
        fp = roGet(QStringLiteral("client-fingerprint"));
    }
    n.fingerprint = fp;
    n.flow = unquote(nf.top.value(QStringLiteral("flow")));

    if (hasReality) {
        n.realityPublicKey = roGet(QStringLiteral("public-key"));
        n.realityShortId = roGet(QStringLiteral("short-id"));
    }

    *out = n;
    return true;
}

} // namespace

QVector<ProxyNode> parseClashProxies(const QString &proxiesYaml, QString *warn)
{
    QVector<ProxyNode> nodes;
    int skipped = 0;

    const QStringList allLines = proxiesYaml.split('\n');

    // 定位 `proxies:` 表头（列首）；找不到就把整段文本当作裸列表（SubscriptionStore::parseProxyList 产出）。
    static const QRegularExpression proxiesKey(QStringLiteral("^proxies\\s*:"));
    int startLine = 0;
    bool haveHeader = false;
    for (int i = 0; i < allLines.size(); ++i) {
        QString l = allLines[i];
        l.remove('\r');
        if (proxiesKey.match(l).hasMatch()) {
            startLine = i + 1;
            haveHeader = true;
            break;
        }
    }
    Q_UNUSED(haveHeader);

    // 切出每个节点的原始行（首行 `- `，随后更深缩进的行）。遇到缩进回退到 <= 标记缩进且非列表项 → 块结束。
    QVector<QStringList> rawNodes;
    QStringList cur;
    int markerIndent = -1;
    for (int i = startLine; i < allLines.size(); ++i) {
        QString line = allLines[i];
        line.remove('\r');
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        const int indent = line.size() - QStringView{line}.trimmed().size();
        if (trimmed.startsWith(QStringLiteral("- "))) {
            if (!cur.isEmpty()) {
                rawNodes << cur;
                cur.clear();
            }
            markerIndent = indent;
            cur << line;
            continue;
        }
        if (markerIndent >= 0 && indent > markerIndent) {
            cur << line; // 当前节点的字段行 / 块式子行
            continue;
        }
        // 缩进回退到标记层或更浅且不是列表项 → proxies 块到此为止
        if (!cur.isEmpty()) {
            rawNodes << cur;
            cur.clear();
        }
        break;
    }
    if (!cur.isEmpty()) {
        rawNodes << cur;
    }

    for (const QStringList &raw : rawNodes) {
        ProxyNode n;
        if (toNode(scanNode(raw), &n)) {
            nodes << n;
        } else {
            ++skipped;
        }
    }

    if (warn && skipped > 0) {
        if (!warn->isEmpty()) {
            warn->append(QLatin1Char('\n'));
        }
        warn->append(QStringLiteral("parseClashProxies: skipped %1 malformed node(s)").arg(skipped));
    }
    return nodes;
}

// —————————————— proxy-groups → 叶子映射（mihomo 不可用时的兜底，理由见头文件）——————————————
namespace {

// 剥掉 YAML 标量的引号，并解开双引号里的转义（\U0001F3AF 之类）。
// ★ 必须解转义：ConfigBuilder 生成的 full.yaml 里 emoji 组名是写成 `"\U0001F3AF 全球直连"` 的，
//   不解开就会拿到 10 个 ASCII 字符，和规则里的组名永远匹配不上——这个坑在 RuleEngine 上踩过一次
//   （commit 733be40：Rule 模式几乎全在回退），这里用同一份 decodeYamlEscapes。
QString unquoteScalar(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && ((v.startsWith('"') && v.endsWith('"'))
                          || (v.startsWith('\'') && v.endsWith('\'')))) {
        const bool dq = v.startsWith('"');
        v = v.mid(1, v.size() - 2);
        if (dq)
            v = decodeYamlEscapes(v);
    }
    return v;
}

} // namespace

QHash<QString, QString> parseProxyGroupsLeaf(const QString &fullYaml)
{
    // 第一遍：把每个组的成员列表读出来（只需要第一个成员，但整列表读出来便于将来扩展/调试）。
    QHash<QString, QStringList> members;
    QStringList order;
    const QStringList lines = fullYaml.split(QLatin1Char('\n'));
    bool inGroups = false;
    bool inProxiesList = false;
    QString cur;
    for (const QString &raw : lines) {
        const QString line = raw;
        const QString t = line.trimmed();
        if (t.isEmpty() || t.startsWith(QLatin1Char('#')))
            continue;
        const int indent = line.size() - QStringView(line).trimmed().size() > 0
                               ? int(line.indexOf(QRegularExpression(QStringLiteral("\\S"))))
                               : 0;
        if (indent == 0) { // 顶层键
            inGroups = (t.startsWith(QStringLiteral("proxy-groups:")));
            inProxiesList = false;
            cur.clear();
            continue;
        }
        if (!inGroups)
            continue;
        if (t.startsWith(QStringLiteral("- "))) {
            // 组列表项开头（`- name: X`）或成员项（`- 节点名`，缩进更深）
            const QString rest = t.mid(2).trimmed();
            if (inProxiesList && !rest.startsWith(QStringLiteral("name:"))) {
                members[cur].append(unquoteScalar(rest));
                continue;
            }
            inProxiesList = false;
            if (rest.startsWith(QStringLiteral("name:"))) {
                cur = unquoteScalar(rest.mid(5));
                if (!cur.isEmpty() && !members.contains(cur)) {
                    members.insert(cur, QStringList());
                    order.append(cur);
                }
            }
            continue;
        }
        if (t.startsWith(QStringLiteral("name:"))) {
            cur = unquoteScalar(t.mid(5));
            if (!cur.isEmpty() && !members.contains(cur)) {
                members.insert(cur, QStringList());
                order.append(cur);
            }
            inProxiesList = false;
            continue;
        }
        if (t == QStringLiteral("proxies:")) {
            inProxiesList = true;
            continue;
        }
        if (t.startsWith(QStringLiteral("proxies:"))) { // 行内数组 `proxies: [A, B]`
            QString arr = t.mid(8).trimmed();
            if (arr.startsWith(QLatin1Char('[')) && arr.endsWith(QLatin1Char(']'))) {
                arr = arr.mid(1, arr.size() - 2);
                for (const QString &m : arr.split(QLatin1Char(',')))
                    if (!m.trimmed().isEmpty())
                        members[cur].append(unquoteScalar(m));
            }
            inProxiesList = false;
            continue;
        }
        inProxiesList = false; // 其它键（type/url/interval…）结束成员列表
    }

    // 第二遍：每组沿「第一个成员」往下走到叶子，带环检测。
    QHash<QString, QString> leaf;
    for (const QString &g : order) {
        QSet<QString> seen;
        QString cursor = g;
        bool ok = false;
        while (true) {
            if (seen.contains(cursor))
                break; // 成环 → 该组不可解析，**不收录**（调用方据此回退核心，安全）
            seen.insert(cursor);
            const auto it = members.constFind(cursor);
            if (it == members.constEnd()) { // 不是组 = 已经是节点名或 DIRECT/REJECT
                ok = !cursor.isEmpty();
                break;
            }
            if (it->isEmpty())
                break; // 空组 → 无法解析
            cursor = it->constFirst();
        }
        if (ok)
            leaf.insert(g, cursor);
    }
    return leaf;
}

std::shared_ptr<const ProxyConfig> buildProxyConfig(const QString &proxiesYaml, const QString &selected,
                                                    ProxyConfig::Mode mode, QString *warn)
{
    QVector<ProxyNode> nodes = parseClashProxies(proxiesYaml, warn);
    // 追加内建 DIRECT（若尚无同名节点）——拨号侧的直连兜底恒在。
    bool hasDirect = false;
    for (const ProxyNode &n : nodes) {
        if (n.name == QStringLiteral("DIRECT")) {
            hasDirect = true;
            break;
        }
    }
    if (!hasDirect) {
        nodes << ProxyNode::direct();
    }
    return std::make_shared<const ProxyConfig>(std::move(nodes), selected, mode);
}

// ————————————————————————— 自测（COAST_PROXYCFG_SELFTEST=1） —————————————————————————

namespace {

int g_fail = 0;

void check(bool ok, const char *what)
{
    if (ok) {
        std::fprintf(stderr, "PROXYCFG: PASS  %s\n", what);
    } else {
        std::fprintf(stderr, "PROXYCFG: FAIL  %s\n", what);
        ++g_fail;
    }
}

const ProxyNode *byName(const QVector<ProxyNode> &v, const char *name)
{
    for (const ProxyNode &n : v) {
        if (n.name == QLatin1String(name)) {
            return &n;
        }
    }
    return nullptr;
}

// 形状 A：SubParser::toClashProxies 产出（列表项 2 空格 / 字段 4 空格 / 嵌套单行 flow / 带 `proxies:` 表头）。
const char *kSampleA = R"YAML(proxies:
  - name: "ss-node"
    type: ss
    server: "ss.example.com"
    port: 8388
    cipher: aes-128-gcm
    password: "sspass"
    udp: true
  - name: "vmess-node"
    type: vmess
    server: "vmess.example.com"
    port: 443
    uuid: "11111111-1111-1111-1111-111111111111"
    alterId: 0
    cipher: auto
    udp: true
    tls: true
    servername: "vmess.sni.com"
    client-fingerprint: chrome
    network: ws
    ws-opts: {path: "/vm", headers: {Host: "vmess.host.com"}}
  - name: "vless-node"
    type: vless
    server: "vless.example.com"
    port: 443
    uuid: "22222222-2222-2222-2222-222222222222"
    udp: true
    tls: true
    servername: "vless.sni.com"
    network: ws
    ws-opts: {path: "/vl", headers: {Host: "vless.host.com"}}
    alpn: [h2, http/1.1]
  - name: "reality-node"
    type: vless
    server: "reality.example.com"
    port: 8443
    uuid: "33333333-3333-3333-3333-333333333333"
    udp: true
    flow: xtls-rprx-vision
    tls: true
    servername: "reality.sni.com"
    client-fingerprint: chrome
    reality-opts: {public-key: "aGVsbG9yZWFsaXR5cHVibGlja2V5MDAwMDAwMDA", short-id: "0123abcd"}
  - name: "trojan-node"
    type: trojan
    server: "trojan.example.com"
    port: 443
    password: "trojanpass"
    udp: true
    sni: "trojan.sni.com"
    skip-cert-verify: true
    alpn: [h3]
  - name: "hy2-node"
    type: hysteria2
    server: "hy2.example.com"
    port: 8443
    password: "hy2pass"
    sni: "hy2.sni.com"
    skip-cert-verify: true
    alpn: [h3]
  - name: "tuic-node"
    type: tuic
    server: "tuic.example.com"
    port: 8443
    uuid: "44444444-4444-4444-4444-444444444444"
    password: "tuicpass"
    sni: "tuic.sni.com"
    alpn: [h3]
)YAML";

// 形状 B：SubscriptionStore::parseProxyList 产出（列表项 4 空格 / 字段 6 空格 / 多一条 use / 无 `proxies:` 表头）。
const char *kSampleB = R"YAML(    - name: "ssb-node"
      type: shadowsocks
      server: "b.example.com"
      port: 9999
      cipher: chacha20-ietf-poly1305
      password: "bpass"
      udp: true
      use: true
    - name: "vlessb-reality"
      type: vless
      server: "rb.example.com"
      port: 2053
      uuid: "55555555-5555-5555-5555-555555555555"
      udp: true
      tls: true
      servername: "rb.sni.com"
      network: ws
      ws-opts: {path: "/rb", headers: {Host: "rb.host.com"}}
      reality-opts: {public-key: "cmVhbGl0eWJwdWJrZXkwMDAwMDAwMDAwMDAwMDAw", short-id: "beef"}
      use: false
)YAML";

} // namespace

bool proxyConfigSelfTest()
{
    g_fail = 0;

    // —— 形状 A ——
    QString warnA;
    const QVector<ProxyNode> a = parseClashProxies(QString::fromUtf8(kSampleA), &warnA);
    check(a.size() == 7, "shape A: parsed 7 nodes");
    check(warnA.isEmpty(), "shape A: no warnings");

    if (const ProxyNode *n = byName(a, "ss-node")) {
        check(n->type == "ss" && n->server == "ss.example.com" && n->port == 8388
                  && n->cipher == "aes-128-gcm" && n->password == "sspass",
              "ss: type/server/port/cipher/password");
    } else {
        check(false, "ss: present");
    }

    if (const ProxyNode *n = byName(a, "vmess-node")) {
        check(n->type == "vmess" && n->uuid == "11111111-1111-1111-1111-111111111111"
                  && n->cipher == "auto" && n->tls && n->sni == "vmess.sni.com"
                  && n->fingerprint == "chrome",
              "vmess: type/uuid/cipher/tls/sni/fingerprint");
        check(n->network == "ws" && n->wsPath == "/vm" && n->wsHost == "vmess.host.com",
              "vmess: ws-opts path/host");
    } else {
        check(false, "vmess: present");
    }

    if (const ProxyNode *n = byName(a, "vless-node")) {
        check(n->type == "vless" && n->tls && n->sni == "vless.sni.com" && n->network == "ws"
                  && n->wsPath == "/vl" && n->wsHost == "vless.host.com",
              "vless: type/tls/sni/ws-opts");
        check(n->alpn == "h2,http/1.1", "vless: alpn list joined");
    } else {
        check(false, "vless: present");
    }

    if (const ProxyNode *n = byName(a, "reality-node")) {
        check(n->type == "reality", "reality: type normalized vless->reality");
        check(n->flow == "xtls-rprx-vision" && n->fingerprint == "chrome" && n->tls
                  && n->sni == "reality.sni.com",
              "reality: flow/fingerprint/tls/sni");
        check(n->realityPublicKey == "aGVsbG9yZWFsaXR5cHVibGlja2V5MDAwMDAwMDA"
                  && n->realityShortId == "0123abcd",
              "reality: public-key/short-id");
    } else {
        check(false, "reality: present");
    }

    if (const ProxyNode *n = byName(a, "trojan-node")) {
        check(n->type == "trojan" && n->password == "trojanpass" && n->sni == "trojan.sni.com"
                  && n->skipCertVerify && n->tls && n->alpn == "h3",
              "trojan: type/password/sni/skip-cert/tls/alpn");
    } else {
        check(false, "trojan: present");
    }

    if (const ProxyNode *n = byName(a, "hy2-node")) {
        check(n->type == "hysteria2" && n->password == "hy2pass" && n->sni == "hy2.sni.com"
                  && n->skipCertVerify && n->alpn == "h3",
              "hysteria2: type/password/sni/skip-cert/alpn");
    } else {
        check(false, "hysteria2: present");
    }

    if (const ProxyNode *n = byName(a, "tuic-node")) {
        check(n->type == "tuic" && n->uuid == "44444444-4444-4444-4444-444444444444"
                  && n->password == "tuicpass" && n->sni == "tuic.sni.com" && n->alpn == "h3",
              "tuic: type/uuid/password/sni/alpn");
    } else {
        check(false, "tuic: present");
    }

    // —— 形状 B（无表头裸列表 + use: + shadowsocks 别名 + reality）——
    QString warnB;
    const QVector<ProxyNode> b = parseClashProxies(QString::fromUtf8(kSampleB), &warnB);
    check(b.size() == 2, "shape B: parsed 2 nodes (headerless list)");
    if (const ProxyNode *n = byName(b, "ssb-node")) {
        check(n->type == "ss" && n->server == "b.example.com" && n->port == 9999
                  && n->cipher == "chacha20-ietf-poly1305" && n->password == "bpass",
              "shape B ss: alias shadowsocks->ss + fields (use: ignored)");
    } else {
        check(false, "shape B ss: present");
    }
    if (const ProxyNode *n = byName(b, "vlessb-reality")) {
        check(n->type == "reality" && n->wsPath == "/rb" && n->wsHost == "rb.host.com"
                  && n->realityPublicKey == "cmVhbGl0eWJwdWJrZXkwMDAwMDAwMDAwMDAwMDAw"
                  && n->realityShortId == "beef",
              "shape B reality: 6-space indent + ws-opts + reality-opts");
    } else {
        check(false, "shape B reality: present");
    }

    // —— buildProxyConfig：追加 DIRECT + 快照 ——
    auto cfg = buildProxyConfig(QString::fromUtf8(kSampleA), QStringLiteral("ss-node"),
                                ProxyConfig::Mode::Rule);
    check(cfg && cfg->nodes().size() == 8, "buildProxyConfig: 7 nodes + DIRECT");
    check(cfg && cfg->nodeByName(QStringLiteral("DIRECT"))
              && cfg->nodeByName(QStringLiteral("DIRECT"))->isDirect(),
          "buildProxyConfig: built-in DIRECT appended");
    check(cfg && cfg->selected() == "ss-node" && cfg->mode() == ProxyConfig::Mode::Rule,
          "buildProxyConfig: selected + mode carried");

    // —— 坏节点跳过、不丢整表 ——
    QString warnBad;
    const QVector<ProxyNode> bad = parseClashProxies(
        QString::fromUtf8("proxies:\n"
                          "  - name: \"good\"\n    type: ss\n    server: \"g.com\"\n    port: 80\n"
                          "  - name: \"noserver\"\n    type: ss\n    port: 80\n"),
        &warnBad);
    check(bad.size() == 1 && !warnBad.isEmpty(),
          "bad node skipped, table survives, warn set");

    // —— proxy-groups → 叶子（mihomo 不可用时的兜底解析）——
    // 钉住四件事：①嵌套组一路走到具体节点；②emoji 组名的 YAML 转义被解开（`\U0001F3AF`）；
    // ③内建 DIRECT 作为叶子；④互相引用成环的组**不收录**（调用方据此回退核心，绝不误路由）。
    {
        static const char kGroups[] =
            "proxies:\n"
            "  - {name: HK-01, type: trojan, server: a.com, port: 443, password: p}\n"
            "proxy-groups:\n"
            "  - name: \"\\U0001F680 \\u8282\\u70b9\\u9009\\u62e9\"\n"
            "    type: select\n"
            "    proxies:\n"
            "      - HK-01\n"
            "  - name: \"\\U0001F3AF \\u5168\\u7403\\u76f4\\u8fde\"\n"
            "    type: select\n"
            "    proxies: [DIRECT, HK-01]\n"
            "  - name: nested\n"
            "    type: select\n"
            "    proxies:\n"
            "      - \"\\U0001F680 \\u8282\\u70b9\\u9009\\u62e9\"\n"
            "  - name: loopA\n"
            "    type: select\n"
            "    proxies:\n"
            "      - loopB\n"
            "  - name: loopB\n"
            "    type: select\n"
            "    proxies:\n"
            "      - loopA\n";
        const QHash<QString, QString> leaf = parseProxyGroupsLeaf(QString::fromUtf8(kGroups));
        const QString rocket = QString::fromUtf8("\xF0\x9F\x9A\x80 \xE8\x8A\x82\xE7\x82\xB9\xE9\x80\x89\xE6\x8B\xA9");
        const QString target = QString::fromUtf8("\xF0\x9F\x8E\xAF \xE5\x85\xA8\xE7\x90\x83\xE7\x9B\xB4\xE8\xBF\x9E");
        check(leaf.value(rocket) == QStringLiteral("HK-01"), "groups: emoji 组名解转义 + 解析到节点");
        check(leaf.value(target) == QStringLiteral("DIRECT"), "groups: 行内数组 + DIRECT 叶子");
        check(leaf.value(QStringLiteral("nested")) == QStringLiteral("HK-01"), "groups: 嵌套组走到底");
        check(!leaf.contains(QStringLiteral("loopA")) && !leaf.contains(QStringLiteral("loopB")),
              "groups: 成环的组不收录（宁可回退核心也不误路由）");
    }

    std::fprintf(stderr, "PROXYCFG: %s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILED",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0;
}

} // namespace coastcore
