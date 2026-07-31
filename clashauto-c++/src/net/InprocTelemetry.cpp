#include "InprocTelemetry.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMetaObject>

InprocTelemetry &InprocTelemetry::instance()
{
    static InprocTelemetry s; // 首个调用者的线程上构造；所有接口自带锁，线程归属无关紧要
    return s;
}

quint64 InprocTelemetry::registerConn(const QString &host, quint16 port, const QString &network,
                                      const QString &chain, const QString &user,
                                      const QString &inboundTag, QObject *owner,
                                      std::function<void()> closer)
{
    Rec r;
    r.host = host;
    r.port = port;
    r.network = network;
    r.chain = chain;
    r.user = user;
    r.tag = inboundTag;
    // RFC3339(UTC)：与 mihomo 的 start 字段同格式同时区，字典序即时间序 ——
    // QmlBridge::observeConnections 的「最近连接」排序直接混排两个来源。
    r.start = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    r.owner = owner;
    r.closer = std::move(closer);

    QMutexLocker lk(&m_mu);
    const quint64 id = m_nextId++;
    m_conns.insert(id, std::move(r));
    ++m_totalConns;
    return id;
}

void InprocTelemetry::addUp(quint64 id, qint64 n)
{
    if (n <= 0)
        return;
    QMutexLocker lk(&m_mu);
    const auto it = m_conns.find(id);
    if (it != m_conns.end())
        it->up += n;
    m_totalUp += quint64(n); // 合计不随连接注销回退：已关连接的字节仍是「跑过的流量」
}

void InprocTelemetry::addDown(quint64 id, qint64 n)
{
    if (n <= 0)
        return;
    QMutexLocker lk(&m_mu);
    const auto it = m_conns.find(id);
    if (it != m_conns.end())
        it->down += n;
    m_totalDown += quint64(n);
}

void InprocTelemetry::noteUdpDst(quint64 id, const QString &host, quint16 port)
{
    QMutexLocker lk(&m_mu);
    const auto it = m_conns.find(id);
    if (it != m_conns.end()) {
        it->host = host;
        it->port = port;
    }
}

void InprocTelemetry::unregisterConn(quint64 id)
{
    QMutexLocker lk(&m_mu);
    m_conns.remove(id); // 幂等：closed/failed/析构都会来一次
}

QJsonArray InprocTelemetry::snapshot() const
{
    QMutexLocker lk(&m_mu);
    QJsonArray out;
    for (auto it = m_conns.cbegin(); it != m_conns.cend(); ++it) {
        const Rec &r = it.value();
        QJsonObject meta;
        meta.insert(QStringLiteral("network"), r.network);
        meta.insert(QStringLiteral("type"), r.tag);
        meta.insert(QStringLiteral("host"), r.host);
        meta.insert(QStringLiteral("destinationIP"), QString());
        meta.insert(QStringLiteral("destinationPort"), QString::number(r.port));
        // user=="local"（本机入站/进程内 TUN）→ sourceIP 给回环地址：QmlBridge::deviceNameFor
        // 的回环分支据此把连接归到「本机」；网关设备（dev-<mac>）走 inboundUser 那条路归属。
        meta.insert(QStringLiteral("sourceIP"),
                    r.user == QLatin1String("local") ? QStringLiteral("127.0.0.1") : QString());
        meta.insert(QStringLiteral("inboundUser"), r.user);
        QJsonObject c;
        c.insert(QStringLiteral("id"), QStringLiteral("coast-%1").arg(it.key()));
        c.insert(QStringLiteral("upload"), r.up);
        c.insert(QStringLiteral("download"), r.down);
        c.insert(QStringLiteral("start"), r.start);
        c.insert(QStringLiteral("chains"), QJsonArray{r.chain});
        c.insert(QStringLiteral("metadata"), meta);
        out.append(c);
    }
    return out;
}

int InprocTelemetry::activeCount() const
{
    QMutexLocker lk(&m_mu);
    return int(m_conns.size());
}

quint64 InprocTelemetry::totalUp() const
{
    QMutexLocker lk(&m_mu);
    return m_totalUp;
}

quint64 InprocTelemetry::totalDown() const
{
    QMutexLocker lk(&m_mu);
    return m_totalDown;
}

quint64 InprocTelemetry::totalConns() const
{
    QMutexLocker lk(&m_mu);
    return m_totalConns;
}

bool InprocTelemetry::requestClose(const QString &connId)
{
    if (!connId.startsWith(QLatin1String("coast-")))
        return false;
    bool okNum = false;
    const quint64 id = connId.mid(6).toULongLong(&okNum);
    if (!okNum)
        return false;

    // ★ 必须**持锁**排队：包装器析构会先在同一把锁下 unregister（见头文件的跨线程约定），
    //   所以「记录还在 = owner 还活着」；排进队后 owner 若析构，Qt 随 context 取消投递。
    QMutexLocker lk(&m_mu);
    const auto it = m_conns.find(id);
    if (it == m_conns.end() || !it->owner || !it->closer)
        return false;
    QMetaObject::invokeMethod(it->owner.data(), it->closer, Qt::QueuedConnection);
    return true;
}

int InprocTelemetry::closeAll()
{
    QMutexLocker lk(&m_mu);
    int n = 0;
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
        if (it->owner && it->closer) {
            QMetaObject::invokeMethod(it->owner.data(), it->closer, Qt::QueuedConnection);
            ++n;
        }
    }
    return n;
}

void InprocTelemetry::log(const QString &throttleKey, const QString &message)
{
    QString out = message;
    if (!throttleKey.isEmpty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        QMutexLocker lk(&m_mu);
        Throttle &t = m_throttle[throttleKey];
        if (now - t.lastMs < 3000) {
            ++t.suppressed; // 3 秒窗口内同类只发第一条，其余计数、并入下一条
            return;
        }
        if (t.suppressed > 0) {
            out += QStringLiteral("（其间同类 ×%1 条被合并）").arg(t.suppressed);
            t.suppressed = 0;
        }
        t.lastMs = now;
        // 节流表只增不减，给个上限：满了整个清掉（键是「事件:节点」，量级本就很小，这只是兜底）
        if (m_throttle.size() > 512)
            m_throttle.clear();
    }
    emit logged(out); // 锁外发（QMutexLocker 已随作用域释放）——接收方可能同步连接在本线程
}
