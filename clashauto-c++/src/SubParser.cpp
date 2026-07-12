#include "SubParser.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>

namespace {

// ---- 基础工具 --------------------------------------------------------------

// 容错 base64 解码：兼容 url-safe（-_）与缺省填充。
QByteArray b64decode(QString s)
{
    s = s.trimmed();
    s.replace('-', '+').replace('_', '/');
    s.remove(QRegularExpression(QStringLiteral("\\s")));
    while (s.size() % 4 != 0) {
        s.append('=');
    }
    return QByteArray::fromBase64(s.toUtf8());
}

// YAML 双引号标量（转义 \ 与 "），字符串值统一加引号最安全。
QString yq(const QString &v)
{
    QString e = v;
    e.replace('\\', QStringLiteral("\\\\"));
    e.replace('"', QStringLiteral("\\\""));
    e.replace('\n', QStringLiteral("\\n"));
    e.replace('\r', QString());
    return '"' + e + '"';
}

bool isNumericPort(const QString &p)
{
    if (p.isEmpty()) {
        return false;
    }
    bool ok = false;
    const int n = p.toInt(&ok);
    return ok && n > 0 && n <= 65535;
}

using Field = QPair<QString, QString>; // key -> 已格式化好的 YAML 值

// 输出一个块式代理：`  - name:` + 每字段一行；嵌套项以单行 flow 值给出（值需调用方备好）。
QString buildProxy(const QString &name, const QString &type, const QString &server,
                   const QString &port, const QVector<Field> &extra)
{
    if (name.isEmpty() || server.isEmpty() || !isNumericPort(port)) {
        return {};
    }
    QString s;
    s += QStringLiteral("  - name: ") + yq(name) + '\n';
    s += QStringLiteral("    type: ") + type + '\n';
    s += QStringLiteral("    server: ") + yq(server) + '\n';
    s += QStringLiteral("    port: ") + port + '\n';
    for (const Field &f : extra) {
        if (!f.second.isEmpty()) {
            s += QStringLiteral("    ") + f.first + QStringLiteral(": ") + f.second + '\n';
        }
    }
    return s;
}

QString pct(const QString &s)
{
    return QString::fromUtf8(QByteArray::fromPercentEncoding(s.toUtf8()));
}

// 传输层（ws/grpc/h2/http）→ 单行 flow 值字段，供 vmess/vless/trojan 复用。
void appendTransport(QVector<Field> &f, const QString &network, const QString &host,
                     const QString &path, const QString &serviceName)
{
    if (network.isEmpty() || network == QStringLiteral("tcp")) {
        return;
    }
    f.push_back({QStringLiteral("network"), network});
    if (network == QStringLiteral("ws")) {
        QString ws = QStringLiteral("{");
        ws += QStringLiteral("path: ") + yq(path.isEmpty() ? QStringLiteral("/") : path);
        if (!host.isEmpty()) {
            ws += QStringLiteral(", headers: {Host: ") + yq(host) + '}';
        }
        ws += '}';
        f.push_back({QStringLiteral("ws-opts"), ws});
    } else if (network == QStringLiteral("grpc")) {
        const QString svc = !serviceName.isEmpty() ? serviceName : path;
        if (!svc.isEmpty()) {
            f.push_back({QStringLiteral("grpc-opts"), QStringLiteral("{grpc-service-name: ") + yq(svc) + '}'});
        }
    } else if (network == QStringLiteral("h2") || network == QStringLiteral("http")) {
        QString h2 = QStringLiteral("{");
        h2 += QStringLiteral("path: ") + yq(path.isEmpty() ? QStringLiteral("/") : path);
        if (!host.isEmpty()) {
            h2 += QStringLiteral(", host: [") + yq(host) + ']';
        }
        h2 += '}';
        f.push_back({QStringLiteral("h2-opts"), h2});
    }
}

// alpn 逗号分隔 → flow list 值：`[h3, h2]`
QString alpnList(const QString &raw)
{
    if (raw.trimmed().isEmpty()) {
        return {};
    }
    QStringList out;
    for (const QString &a : raw.split(',', Qt::SkipEmptyParts)) {
        out << yq(a.trimmed());
    }
    return '[' + out.join(QStringLiteral(", ")) + ']';
}

bool truthy(const QString &v)
{
    const QString t = v.trimmed().toLower();
    return t == QStringLiteral("1") || t == QStringLiteral("true");
}

// ---- 各协议 ----------------------------------------------------------------

// ss:// —— SIP002（method:pass base64@host:port?plugin#name）与旧式（base64(method:pass@host:port)#name）
QString parseSS(const QString &uri)
{
    QString rest = uri.mid(QStringLiteral("ss://").size());
    QString name;
    const int hash = rest.indexOf('#');
    if (hash >= 0) {
        name = pct(rest.mid(hash + 1));
        rest = rest.left(hash);
    }

    QString method, password, host, port, pluginRaw;
    const int at = rest.indexOf('@');
    if (at >= 0) {
        // SIP002：userinfo@host:port[?plugin]
        QString userinfo = rest.left(at);
        QString hostpart = rest.mid(at + 1);
        const int q = hostpart.indexOf('?');
        if (q >= 0) {
            pluginRaw = hostpart.mid(q + 1);
            hostpart = hostpart.left(q);
        }
        hostpart = hostpart.remove('/');
        const int colon = hostpart.lastIndexOf(':');
        if (colon < 0) {
            return {};
        }
        host = hostpart.left(colon);
        port = hostpart.mid(colon + 1);
        // userinfo 可能是 base64(method:pass) 或明文 method:pass
        QString decoded = QString::fromUtf8(b64decode(userinfo));
        if (!decoded.contains(':')) {
            decoded = pct(userinfo);
        }
        const int c = decoded.indexOf(':');
        if (c < 0) {
            return {};
        }
        method = decoded.left(c);
        password = decoded.mid(c + 1);
    } else {
        // 旧式：base64(method:pass@host:port)
        const QString decoded = QString::fromUtf8(b64decode(rest));
        const int a2 = decoded.lastIndexOf('@');
        if (a2 < 0) {
            return {};
        }
        const QString cred = decoded.left(a2);
        QString hp = decoded.mid(a2 + 1);
        const int c = cred.indexOf(':');
        if (c < 0) {
            return {};
        }
        method = cred.left(c);
        password = cred.mid(c + 1);
        const int colon = hp.lastIndexOf(':');
        if (colon < 0) {
            return {};
        }
        host = hp.left(colon);
        port = hp.mid(colon + 1);
    }
    if (name.isEmpty()) {
        name = host + ':' + port;
    }

    QVector<Field> f;
    f.push_back({QStringLiteral("cipher"), method});
    f.push_back({QStringLiteral("password"), yq(password)});
    f.push_back({QStringLiteral("udp"), QStringLiteral("true")});
    // 插件：obfs（simple-obfs）/ v2ray-plugin
    if (!pluginRaw.isEmpty()) {
        const QStringList parts = pct(pluginRaw).split(';', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            const QString pname = parts.first();
            QString mode, phost;
            for (int i = 1; i < parts.size(); ++i) {
                const QString kv = parts[i];
                const int eq = kv.indexOf('=');
                const QString k = eq >= 0 ? kv.left(eq) : kv;
                const QString v = eq >= 0 ? kv.mid(eq + 1) : QString();
                if (k == QStringLiteral("obfs") || k == QStringLiteral("mode")) {
                    mode = v;
                } else if (k == QStringLiteral("obfs-host") || k == QStringLiteral("host")) {
                    phost = v;
                }
            }
            if (pname.contains(QStringLiteral("obfs"))) {
                f.push_back({QStringLiteral("plugin"), QStringLiteral("obfs")});
                QString opts = QStringLiteral("{mode: ") + (mode.isEmpty() ? QStringLiteral("http") : mode);
                if (!phost.isEmpty()) {
                    opts += QStringLiteral(", host: ") + yq(phost);
                }
                opts += '}';
                f.push_back({QStringLiteral("plugin-opts"), opts});
            } else if (pname.contains(QStringLiteral("v2ray"))) {
                f.push_back({QStringLiteral("plugin"), QStringLiteral("v2ray-plugin")});
                QString opts = QStringLiteral("{mode: websocket");
                if (!phost.isEmpty()) {
                    opts += QStringLiteral(", host: ") + yq(phost);
                }
                opts += '}';
                f.push_back({QStringLiteral("plugin-opts"), opts});
            }
        }
    }
    return buildProxy(name, QStringLiteral("ss"), host, port, f);
}

// ssr:// —— base64( host:port:proto:method:obfs:base64(pass)/?params )
QString parseSSR(const QString &uri)
{
    const QString decoded = QString::fromUtf8(b64decode(uri.mid(QStringLiteral("ssr://").size())));
    const int slash = decoded.indexOf(QStringLiteral("/?"));
    const QString main = slash >= 0 ? decoded.left(slash) : decoded;
    const QString params = slash >= 0 ? decoded.mid(slash + 2) : QString();
    const QStringList seg = main.split(':');
    if (seg.size() < 6) {
        return {};
    }
    const int n = seg.size();
    const QString host = seg.mid(0, n - 5).join(':'); // 兼容极少数 host 含冒号
    const QString port = seg[n - 5];
    const QString protocol = seg[n - 4];
    const QString method = seg[n - 3];
    const QString obfs = seg[n - 2];
    const QString password = QString::fromUtf8(b64decode(seg[n - 1]));

    QString name, obfsParam, protoParam;
    const QUrlQuery q(params);
    for (const auto &item : q.queryItems()) {
        if (item.first == QStringLiteral("remarks")) {
            name = QString::fromUtf8(b64decode(item.second));
        } else if (item.first == QStringLiteral("obfsparam")) {
            obfsParam = QString::fromUtf8(b64decode(item.second));
        } else if (item.first == QStringLiteral("protoparam")) {
            protoParam = QString::fromUtf8(b64decode(item.second));
        }
    }
    if (name.isEmpty()) {
        name = host + ':' + port;
    }

    QVector<Field> f;
    f.push_back({QStringLiteral("cipher"), method});
    f.push_back({QStringLiteral("password"), yq(password)});
    f.push_back({QStringLiteral("protocol"), protocol});
    if (!protoParam.isEmpty()) {
        f.push_back({QStringLiteral("protocol-param"), yq(protoParam)});
    }
    f.push_back({QStringLiteral("obfs"), obfs});
    if (!obfsParam.isEmpty()) {
        f.push_back({QStringLiteral("obfs-param"), yq(obfsParam)});
    }
    f.push_back({QStringLiteral("udp"), QStringLiteral("true")});
    return buildProxy(name, QStringLiteral("ssr"), host, port, f);
}

QString jstr(const QJsonObject &o, const QString &k)
{
    const QJsonValue v = o.value(k);
    if (v.isString()) {
        return v.toString();
    }
    if (v.isDouble()) {
        return QString::number(static_cast<qint64>(v.toDouble()));
    }
    return {};
}

// vmess:// —— base64(json)
QString parseVMess(const QString &uri)
{
    const QByteArray json = b64decode(uri.mid(QStringLiteral("vmess://").size()));
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return {};
    }
    const QJsonObject o = doc.object();
    const QString server = jstr(o, QStringLiteral("add"));
    const QString port = jstr(o, QStringLiteral("port"));
    const QString uuid = jstr(o, QStringLiteral("id"));
    if (server.isEmpty() || uuid.isEmpty()) {
        return {};
    }
    QString name = jstr(o, QStringLiteral("ps"));
    if (name.isEmpty()) {
        name = server + ':' + port;
    }
    const QString aid = jstr(o, QStringLiteral("aid"));
    QString cipher = jstr(o, QStringLiteral("scy"));
    if (cipher.isEmpty()) {
        cipher = QStringLiteral("auto");
    }
    const QString net = jstr(o, QStringLiteral("net"));
    const QString host = jstr(o, QStringLiteral("host"));
    const QString path = jstr(o, QStringLiteral("path"));
    const QString tls = jstr(o, QStringLiteral("tls"));
    const QString sni = jstr(o, QStringLiteral("sni"));
    const QString fp = jstr(o, QStringLiteral("fp"));

    QVector<Field> f;
    f.push_back({QStringLiteral("uuid"), yq(uuid)});
    f.push_back({QStringLiteral("alterId"), aid.isEmpty() ? QStringLiteral("0") : aid});
    f.push_back({QStringLiteral("cipher"), cipher});
    f.push_back({QStringLiteral("udp"), QStringLiteral("true")});
    if (tls == QStringLiteral("tls")) {
        f.push_back({QStringLiteral("tls"), QStringLiteral("true")});
        const QString servername = !sni.isEmpty() ? sni : host;
        if (!servername.isEmpty()) {
            f.push_back({QStringLiteral("servername"), yq(servername)});
        }
        if (!fp.isEmpty()) {
            f.push_back({QStringLiteral("client-fingerprint"), fp});
        }
    }
    appendTransport(f, net == QStringLiteral("h2") ? QStringLiteral("h2") : net, host, path,
                    jstr(o, QStringLiteral("serviceName")).isEmpty() ? path : jstr(o, QStringLiteral("serviceName")));
    return buildProxy(name, QStringLiteral("vmess"), server, port, f);
}

// 公共：trojan/vless/hysteria2/hysteria/tuic 用 QUrl 解析 userinfo@host:port?query#name
QString parseTrojan(const QString &uri)
{
    const QUrl u(uri, QUrl::TolerantMode);
    const QString server = u.host(QUrl::FullyDecoded);
    const int portNum = u.port();
    if (server.isEmpty() || portNum < 0) {
        return {};
    }
    const QString password = u.userInfo(QUrl::FullyDecoded);
    QString name = u.fragment(QUrl::FullyDecoded);
    if (name.isEmpty()) {
        name = server + ':' + QString::number(portNum);
    }
    const QUrlQuery q(u);
    const QString sni = !q.queryItemValue(QStringLiteral("sni"), QUrl::FullyDecoded).isEmpty()
                            ? q.queryItemValue(QStringLiteral("sni"), QUrl::FullyDecoded)
                            : q.queryItemValue(QStringLiteral("peer"), QUrl::FullyDecoded);
    const QString type = q.queryItemValue(QStringLiteral("type"), QUrl::FullyDecoded);
    const QString host = q.queryItemValue(QStringLiteral("host"), QUrl::FullyDecoded);
    const QString path = q.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
    const QString serviceName = q.queryItemValue(QStringLiteral("serviceName"), QUrl::FullyDecoded);
    const QString fp = q.queryItemValue(QStringLiteral("fp"), QUrl::FullyDecoded);
    const QString alpn = q.queryItemValue(QStringLiteral("alpn"), QUrl::FullyDecoded);
    const bool insecure = truthy(q.queryItemValue(QStringLiteral("allowInsecure")))
                          || truthy(q.queryItemValue(QStringLiteral("insecure")));

    QVector<Field> f;
    f.push_back({QStringLiteral("password"), yq(password)});
    f.push_back({QStringLiteral("udp"), QStringLiteral("true")});
    if (!sni.isEmpty()) {
        f.push_back({QStringLiteral("sni"), yq(sni)});
    }
    if (insecure) {
        f.push_back({QStringLiteral("skip-cert-verify"), QStringLiteral("true")});
    }
    const QString a = alpnList(alpn);
    if (!a.isEmpty()) {
        f.push_back({QStringLiteral("alpn"), a});
    }
    if (!fp.isEmpty()) {
        f.push_back({QStringLiteral("client-fingerprint"), fp});
    }
    appendTransport(f, type, host, path, serviceName);
    return buildProxy(name, QStringLiteral("trojan"), server, QString::number(portNum), f);
}

QString parseVLESS(const QString &uri)
{
    const QUrl u(uri, QUrl::TolerantMode);
    const QString server = u.host(QUrl::FullyDecoded);
    const int portNum = u.port();
    if (server.isEmpty() || portNum < 0) {
        return {};
    }
    const QString uuid = u.userInfo(QUrl::FullyDecoded);
    QString name = u.fragment(QUrl::FullyDecoded);
    if (name.isEmpty()) {
        name = server + ':' + QString::number(portNum);
    }
    const QUrlQuery q(u);
    const QString security = q.queryItemValue(QStringLiteral("security"), QUrl::FullyDecoded);
    const QString flow = q.queryItemValue(QStringLiteral("flow"), QUrl::FullyDecoded);
    const QString sni = q.queryItemValue(QStringLiteral("sni"), QUrl::FullyDecoded);
    const QString fp = q.queryItemValue(QStringLiteral("fp"), QUrl::FullyDecoded);
    const QString pbk = q.queryItemValue(QStringLiteral("pbk"), QUrl::FullyDecoded);
    const QString sid = q.queryItemValue(QStringLiteral("sid"), QUrl::FullyDecoded);
    const QString type = q.queryItemValue(QStringLiteral("type"), QUrl::FullyDecoded);
    const QString host = q.queryItemValue(QStringLiteral("host"), QUrl::FullyDecoded);
    const QString path = q.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
    const QString serviceName = q.queryItemValue(QStringLiteral("serviceName"), QUrl::FullyDecoded);
    const QString alpn = q.queryItemValue(QStringLiteral("alpn"), QUrl::FullyDecoded);

    QVector<Field> f;
    f.push_back({QStringLiteral("uuid"), yq(uuid)});
    f.push_back({QStringLiteral("udp"), QStringLiteral("true")});
    if (!flow.isEmpty()) {
        f.push_back({QStringLiteral("flow"), flow});
    }
    if (security == QStringLiteral("tls") || security == QStringLiteral("reality")) {
        f.push_back({QStringLiteral("tls"), QStringLiteral("true")});
        if (!sni.isEmpty()) {
            f.push_back({QStringLiteral("servername"), yq(sni)});
        }
        if (!fp.isEmpty()) {
            f.push_back({QStringLiteral("client-fingerprint"), fp});
        }
        if (security == QStringLiteral("reality") && !pbk.isEmpty()) {
            QString ro = QStringLiteral("{public-key: ") + yq(pbk);
            if (!sid.isEmpty()) {
                ro += QStringLiteral(", short-id: ") + yq(sid);
            }
            ro += '}';
            f.push_back({QStringLiteral("reality-opts"), ro});
        }
    }
    const QString a = alpnList(alpn);
    if (!a.isEmpty()) {
        f.push_back({QStringLiteral("alpn"), a});
    }
    appendTransport(f, type, host, path, serviceName);
    return buildProxy(name, QStringLiteral("vless"), server, QString::number(portNum), f);
}

QString parseHysteria2(const QString &uri)
{
    const QUrl u(uri, QUrl::TolerantMode);
    const QString server = u.host(QUrl::FullyDecoded);
    const int portNum = u.port();
    if (server.isEmpty() || portNum < 0) {
        return {};
    }
    const QUrlQuery q(u);
    QString password = u.userInfo(QUrl::FullyDecoded);
    if (password.isEmpty()) {
        password = q.queryItemValue(QStringLiteral("password"), QUrl::FullyDecoded);
    }
    QString name = u.fragment(QUrl::FullyDecoded);
    if (name.isEmpty()) {
        name = server + ':' + QString::number(portNum);
    }
    const QString sni = q.queryItemValue(QStringLiteral("sni"), QUrl::FullyDecoded);
    const QString obfs = q.queryItemValue(QStringLiteral("obfs"), QUrl::FullyDecoded);
    const QString obfsPw = q.queryItemValue(QStringLiteral("obfs-password"), QUrl::FullyDecoded);
    const QString alpn = q.queryItemValue(QStringLiteral("alpn"), QUrl::FullyDecoded);
    const bool insecure = truthy(q.queryItemValue(QStringLiteral("insecure")));

    QVector<Field> f;
    f.push_back({QStringLiteral("password"), yq(password)});
    if (!sni.isEmpty()) {
        f.push_back({QStringLiteral("sni"), yq(sni)});
    }
    if (insecure) {
        f.push_back({QStringLiteral("skip-cert-verify"), QStringLiteral("true")});
    }
    if (!obfs.isEmpty()) {
        f.push_back({QStringLiteral("obfs"), obfs});
        if (!obfsPw.isEmpty()) {
            f.push_back({QStringLiteral("obfs-password"), yq(obfsPw)});
        }
    }
    const QString a = alpnList(alpn);
    if (!a.isEmpty()) {
        f.push_back({QStringLiteral("alpn"), a});
    }
    return buildProxy(name, QStringLiteral("hysteria2"), server, QString::number(portNum), f);
}

QString parseHysteria(const QString &uri)
{
    const QUrl u(uri, QUrl::TolerantMode);
    const QString server = u.host(QUrl::FullyDecoded);
    const int portNum = u.port();
    if (server.isEmpty() || portNum < 0) {
        return {};
    }
    const QUrlQuery q(u);
    QString name = u.fragment(QUrl::FullyDecoded);
    if (name.isEmpty()) {
        name = server + ':' + QString::number(portNum);
    }
    const QString authStr = q.queryItemValue(QStringLiteral("auth"), QUrl::FullyDecoded);
    const QString protocol = q.queryItemValue(QStringLiteral("protocol"), QUrl::FullyDecoded);
    const QString peer = !q.queryItemValue(QStringLiteral("peer"), QUrl::FullyDecoded).isEmpty()
                             ? q.queryItemValue(QStringLiteral("peer"), QUrl::FullyDecoded)
                             : q.queryItemValue(QStringLiteral("sni"), QUrl::FullyDecoded);
    const QString up = !q.queryItemValue(QStringLiteral("upmbps")).isEmpty()
                           ? q.queryItemValue(QStringLiteral("upmbps"))
                           : q.queryItemValue(QStringLiteral("up"));
    const QString down = !q.queryItemValue(QStringLiteral("downmbps")).isEmpty()
                             ? q.queryItemValue(QStringLiteral("downmbps"))
                             : q.queryItemValue(QStringLiteral("down"));
    const QString alpn = q.queryItemValue(QStringLiteral("alpn"), QUrl::FullyDecoded);
    const QString obfs = q.queryItemValue(QStringLiteral("obfs"), QUrl::FullyDecoded);
    const bool insecure = truthy(q.queryItemValue(QStringLiteral("insecure")));

    QVector<Field> f;
    if (!authStr.isEmpty()) {
        f.push_back({QStringLiteral("auth-str"), yq(authStr)});
    }
    f.push_back({QStringLiteral("protocol"), protocol.isEmpty() ? QStringLiteral("udp") : protocol});
    if (!up.isEmpty()) {
        f.push_back({QStringLiteral("up"), yq(up)});
    }
    if (!down.isEmpty()) {
        f.push_back({QStringLiteral("down"), yq(down)});
    }
    if (!peer.isEmpty()) {
        f.push_back({QStringLiteral("sni"), yq(peer)});
    }
    if (insecure) {
        f.push_back({QStringLiteral("skip-cert-verify"), QStringLiteral("true")});
    }
    const QString a = alpnList(alpn);
    if (!a.isEmpty()) {
        f.push_back({QStringLiteral("alpn"), a});
    }
    if (!obfs.isEmpty()) {
        f.push_back({QStringLiteral("obfs"), yq(obfs)});
    }
    return buildProxy(name, QStringLiteral("hysteria"), server, QString::number(portNum), f);
}

QString parseTuic(const QString &uri)
{
    const QUrl u(uri, QUrl::TolerantMode);
    const QString server = u.host(QUrl::FullyDecoded);
    const int portNum = u.port();
    if (server.isEmpty() || portNum < 0) {
        return {};
    }
    const QString user = u.userName(QUrl::FullyDecoded);
    const QString pass = u.password(QUrl::FullyDecoded);
    QString name = u.fragment(QUrl::FullyDecoded);
    if (name.isEmpty()) {
        name = server + ':' + QString::number(portNum);
    }
    const QUrlQuery q(u);
    const QString sni = q.queryItemValue(QStringLiteral("sni"), QUrl::FullyDecoded);
    const QString cc = !q.queryItemValue(QStringLiteral("congestion_control")).isEmpty()
                           ? q.queryItemValue(QStringLiteral("congestion_control"))
                           : q.queryItemValue(QStringLiteral("congestion-controller"));
    const QString urm = !q.queryItemValue(QStringLiteral("udp_relay_mode")).isEmpty()
                            ? q.queryItemValue(QStringLiteral("udp_relay_mode"))
                            : q.queryItemValue(QStringLiteral("udp-relay-mode"));
    const QString alpn = q.queryItemValue(QStringLiteral("alpn"), QUrl::FullyDecoded);
    const bool insecure = truthy(q.queryItemValue(QStringLiteral("allow_insecure")))
                          || truthy(q.queryItemValue(QStringLiteral("insecure")));

    QVector<Field> f;
    if (!pass.isEmpty()) {
        // v5：uuid:password
        f.push_back({QStringLiteral("uuid"), yq(user)});
        f.push_back({QStringLiteral("password"), yq(pass)});
    } else {
        // v4：token
        f.push_back({QStringLiteral("token"), yq(user)});
    }
    if (!sni.isEmpty()) {
        f.push_back({QStringLiteral("sni"), yq(sni)});
    }
    if (!cc.isEmpty()) {
        f.push_back({QStringLiteral("congestion-controller"), cc});
    }
    if (!urm.isEmpty()) {
        f.push_back({QStringLiteral("udp-relay-mode"), urm});
    }
    const QString a = alpnList(alpn);
    if (!a.isEmpty()) {
        f.push_back({QStringLiteral("alpn"), a});
    }
    if (insecure) {
        f.push_back({QStringLiteral("skip-cert-verify"), QStringLiteral("true")});
    }
    return buildProxy(name, QStringLiteral("tuic"), server, QString::number(portNum), f);
}

QString parseOne(const QString &uriRaw)
{
    const QString uri = uriRaw.trimmed();
    if (uri.startsWith(QStringLiteral("ss://"))) {
        return parseSS(uri);
    }
    if (uri.startsWith(QStringLiteral("ssr://"))) {
        return parseSSR(uri);
    }
    if (uri.startsWith(QStringLiteral("vmess://"))) {
        return parseVMess(uri);
    }
    if (uri.startsWith(QStringLiteral("trojan://"))) {
        return parseTrojan(uri);
    }
    if (uri.startsWith(QStringLiteral("vless://"))) {
        return parseVLESS(uri);
    }
    if (uri.startsWith(QStringLiteral("hysteria2://")) || uri.startsWith(QStringLiteral("hy2://"))) {
        return parseHysteria2(uri);
    }
    if (uri.startsWith(QStringLiteral("hysteria://")) || uri.startsWith(QStringLiteral("hy://"))) {
        return parseHysteria(uri);
    }
    if (uri.startsWith(QStringLiteral("tuic://"))) {
        return parseTuic(uri);
    }
    return {};
}

} // namespace

QString SubParser::toClashProxies(const QString &rawContent)
{
    const QString trimmed = rawContent.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    // 已是 Clash YAML → 原样交给上层解析
    static const QRegularExpression proxiesKey(QStringLiteral("(?m)^\\s*proxies\\s*:"));
    if (proxiesKey.match(trimmed).hasMatch()) {
        return rawContent;
    }

    // 取得 URI 列表文本：明文（含 ://）直接用；否则按整体 base64 解码
    QString uriText;
    if (trimmed.contains(QStringLiteral("://"))) {
        uriText = trimmed;
    } else {
        const QString decoded = QString::fromUtf8(b64decode(trimmed));
        if (proxiesKey.match(decoded).hasMatch()) {
            return decoded; // base64 包裹的 Clash YAML
        }
        if (!decoded.contains(QStringLiteral("://"))) {
            return {}; // 无法识别
        }
        uriText = decoded;
    }

    QString body;
    const QStringList lines = uriText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QString node = parseOne(line);
        if (!node.isEmpty()) {
            body += node;
        }
    }
    if (body.isEmpty()) {
        return {};
    }
    return QStringLiteral("proxies:\n") + body;
}
