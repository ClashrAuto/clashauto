#include "ClashService.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <limits>

namespace {
// 下载测速目标：Cloudflare 全球 CDN，稳定且各地区节点都能就近命中；请求经混合端口走选中节点。
// 只测「一段时间/一段字节」内的吞吐，不会真的下满 100MB（见下方 MAX_MS/MAX_BYTES 上限）。
const char *kSpeedTestUrl = "https://speed.cloudflare.com/__down?bytes=104857600";
constexpr int kSpeedConcurrency = 5;              // 并发下载数
constexpr qint64 kSpeedMaxMs = 3000;              // 单节点测速窗口上限（毫秒，从首字节起算）
constexpr qint64 kSpeedMaxBytes = 20LL * 1024 * 1024; // 单节点下载字节上限，避免快节点/整表测速跑满流量
} // namespace

ClashService::ClashService(QObject *parent) : QObject(parent)
{
    connect(&m_trafficTimer, &QTimer::timeout, this, &ClashService::ensureTrafficStream);
    connect(&m_connectionsTimer, &QTimer::timeout, this, &ClashService::pollConnections);
    connect(&m_nodesTimer, &QTimer::timeout, this, &ClashService::pollNodes);
}

void ClashService::setEndpoint(const QString &host, int port)
{
    if (!host.isEmpty()) {
        m_host = host;
    }
    if (port > 0) {
        m_port = port;
    }
    if (m_trafficReply) {
        m_trafficReply->abort(); // host/端口变了：断开旧流，finished 会置空，看门狗按新地址重连
    }
}

void ClashService::setMixedPort(int port)
{
    if (port > 0) {
        m_mixedPort = port;
    }
}

void ClashService::start()
{
    emit statusUpdated(false, false, false);
    emit logUpdated("Connecting to Clash API...");
    startTrafficStream(); // /traffic 常开单流（不能每秒 GET，见头文件说明）
    pollConnections();
    pollNodes();
    m_trafficTimer.start(2000);      // 看门狗：流断了就重连（核心重启/端口变更）
    m_connectionsTimer.start(1000);
    m_nodesTimer.start(1000); // 1s 实时拉取节点列表状态（对齐旧项目 getProxies 每秒轮询）
}

void ClashService::setMode(const QString &mode)
{
    QString apiMode = mode;
    if (mode.compare("Rule", Qt::CaseInsensitive) == 0 || mode.contains("rule", Qt::CaseInsensitive)
        || mode.contains(QString::fromUtf8("规则"))) {
        apiMode = "Rule";
    } else if (mode.compare("Global", Qt::CaseInsensitive) == 0 || mode.contains("global", Qt::CaseInsensitive)
               || mode.contains(QString::fromUtf8("全局")) || mode.contains(QString::fromUtf8("全部"))) {
        apiMode = "Global";
    } else if (mode.compare("Direct", Qt::CaseInsensitive) == 0 || mode.contains("direct", Qt::CaseInsensitive)
               || mode.contains(QString::fromUtf8("直连")) || mode.contains(QString::fromUtf8("直"))) {
        apiMode = "Direct";
    }

    m_mode = apiMode;        // 记录模式，pollNodes 据此选主组
    m_selectedGroup.clear(); // 模式变了，默认视图重选主组
    sendJsonRequest(QUrl(QString("http://%1:%2/configs").arg(m_host).arg(m_port)),
                    "PATCH",
                    QJsonObject{{"mode", apiMode}},
                    [this, mode, apiMode](bool ok, const QString &message) {
                        if (ok) {
                            emit logUpdated(QString("Mode changed: %1").arg(mode));
                            pollNodes();
                        } else {
                            emit logUpdated(QString("Mode change failed (%1): %2").arg(apiMode, message));
                        }
                    });
}

void ClashService::setSelectedGroup(const QString &group)
{
    if (group.isEmpty() || group == m_selectedGroup) {
        return;
    }
    m_selectedGroup = group;
    pollNodes();
}

void ClashService::setClearConnectionsOnSwitch(bool enabled)
{
    m_clearOnSwitch = enabled;
}

void ClashService::selectNode(const QString &name)
{
    if (name.isEmpty()) {
        return;
    }

    const QString group = m_selectedGroup.isEmpty() ? QStringLiteral("GLOBAL") : m_selectedGroup;
    const QString encodedGroup = QString::fromLatin1(QUrl::toPercentEncoding(group));
    sendJsonRequest(QUrl(QString("http://%1:%2/proxies/%3").arg(m_host).arg(m_port).arg(encodedGroup)),
                    "PUT",
                    QJsonObject{{"name", name}},
                    [this, group, name](bool ok, const QString &message) {
                        if (ok) {
                            m_selectedNode = name;
                            emit logUpdated(QString("Node selected: %1 -> %2").arg(group, name));
                            if (m_clearOnSwitch) { // 对应设置「切换时清理连接」
                                clearConnections();
                            }
                            pollNodes();
                        } else {
                            emit logUpdated(QString("Select node failed: %1").arg(message));
                        }
                    });
}

void ClashService::clearConnections()
{
    sendDelete(QUrl(QString("http://%1:%2/connections").arg(m_host).arg(m_port)),
               [this](bool ok, const QString &message) {
                   if (ok) {
                       emit logUpdated("Connections cleared.");
                       pollConnections();
                   } else {
                       emit logUpdated(QString("Clear connections failed: %1").arg(message));
                   }
               });
}

void ClashService::fetchConnections(std::function<void(QJsonArray)> callback)
{
    sendGet(QUrl(QString("http://%1:%2/connections").arg(m_host).arg(m_port)),
            [callback](const QJsonDocument &doc) {
                callback(doc.object().value("connections").toArray());
            });
}

void ClashService::closeConnection(const QString &id)
{
    sendDelete(QUrl(QString("http://%1:%2/connections/%3").arg(m_host).arg(m_port).arg(id)),
               [this, id](bool ok, const QString &message) {
                   emit logUpdated(ok ? QString("Connection closed: %1").arg(id)
                                      : QString("Close connection failed: %1").arg(message));
               });
}

void ClashService::refreshNodes()
{
    pollNodes();
}

void ClashService::testDelays(bool thenSpeed)
{
    sendGet(QUrl(QString("http://%1:%2/proxies").arg(m_host).arg(m_port)), [this, thenSpeed](const QJsonDocument &doc) {
        const QJsonObject proxies = doc.object().value("proxies").toObject();
        const QString groupName = m_selectedGroup.isEmpty() ? QStringLiteral("GLOBAL") : m_selectedGroup;
        const QJsonArray all = proxies.value(groupName).toObject().value("all").toArray();
        QStringList names;
        for (const QJsonValue &value : all) {
            const QString name = value.toString();
            if (!name.isEmpty()) {
                names << name;
            }
        }
        testNodeDelays(names, thenSpeed);
    });
}

void ClashService::testNodeDelays(const QStringList &names, bool thenSpeed)
{
    if (names.isEmpty()) {
        emit logUpdated("No nodes to test.");
        if (thenSpeed) {
            emit speedTestRunning(false); // 无节点：直接结束测速加载态
        }
        return;
    }
    emit logUpdated(QString("Testing %1 nodes...").arg(names.size()));
    const QString target = QString::fromLatin1(QUrl::toPercentEncoding("http://www.gstatic.com/generate_204"));
    auto *pending = new int(names.size());
    for (const QString &name : names) {
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(name));
        const QUrl url(QString("http://%1:%2/proxies/%3/delay?timeout=5000&url=%4")
                           .arg(m_host).arg(m_port).arg(encoded, target));
        QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        request.setTransferTimeout(9000); // 略大于核心侧 5s，避免任何一条延迟请求挂死占用连接槽
#endif
        QNetworkReply *reply = m_delayNetwork.get(request); // 专用连接池，不挤占轮询/切换
        connect(reply, &QNetworkReply::finished, this, [this, reply, pending, thenSpeed] {
            reply->deleteLater();
            if (--(*pending) <= 0) {
                delete pending;
                emit logUpdated("Delay test finished.");
                pollNodes();
                if (thenSpeed) {
                    startSpeedTestForValidNodes(); // 延迟测完 → 对有效延迟节点逐个测下载速度（并发 5）
                }
            }
        });
    }
}

void ClashService::startSpeedTestForValidNodes()
{
    // 重新拉一次 /proxies，按当前主选择组的 all 列表筛出「有延迟(>0)」的节点后开测。
    // 沿 now 链找承载延迟的叶子（组自身常无 history），与 pollNodes 的 resolveHistory 一致。
    sendGet(QUrl(QString("http://%1:%2/proxies").arg(m_host).arg(m_port)), [this](const QJsonDocument &doc) {
        const QJsonObject proxies = doc.object().value("proxies").toObject();
        auto resolveDelay = [&proxies](const QString &start) -> int {
            QString cur = start;
            QSet<QString> seen;
            for (int i = 0; i < 16 && !cur.isEmpty() && !seen.contains(cur); ++i) {
                seen.insert(cur);
                const QJsonObject o = proxies.value(cur).toObject();
                const QJsonArray h = o.value("history").toArray();
                if (!h.isEmpty()) {
                    return h.last().toObject().value("delay").toInt();
                }
                cur = o.value("now").toString();
            }
            return 0;
        };
        const QString groupName = m_selectedGroup.isEmpty() ? QStringLiteral("GLOBAL") : m_selectedGroup;
        const QJsonArray all = proxies.value(groupName).toObject().value("all").toArray();
        QStringList valid;
        for (const QJsonValue &value : all) {
            const QString name = value.toString();
            if (!name.isEmpty() && resolveDelay(name) > 0) {
                valid << name; // 只测有效延迟（能连通）的节点，超时/无延迟的跳过
            }
        }
        startSpeedTest(valid);
    });
}

void ClashService::startSpeedTest(const QStringList &names)
{
    if (m_speedTesting) {
        emit logUpdated(QString::fromUtf8("下载测速进行中，忽略重复请求"));
        return;
    }
    if (names.isEmpty()) {
        emit logUpdated(QString::fromUtf8("没有可测速的有效节点"));
        emit speedTestRunning(false);
        return;
    }
    m_speedTesting = true;
    m_speedQueue = names;
    m_speedActive = 0;
    m_speedSelecting = false;
    m_speedGroup = m_selectedGroup.isEmpty() ? QStringLiteral("GLOBAL") : m_selectedGroup;
    m_speedOriginalNode = m_selectedNode;
    m_measuredSpeeds.clear(); // 每轮重测：清掉上轮速度，避免旧值残留
    // 代理指向混合端口：本 QNAM 的所有请求都经核心代理，路由由规则送回主选择组 → 钉在当前选中的测速节点
    m_speedNetwork.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_host, static_cast<quint16>(m_mixedPort)));
    emit logUpdated(QString::fromUtf8("开始下载测速：%1 个节点（并发 %2）").arg(names.size()).arg(kSpeedConcurrency));
    emit speedTestRunning(true);
    pumpSpeedTest(); // 启动第一个握手；后续在每个下载「建连」后自动补位到并发上限
}

void ClashService::pumpSpeedTest()
{
    if (!m_speedTesting) {
        return;
    }
    if (m_speedQueue.isEmpty() && m_speedActive <= 0) {
        if (m_speedRestoring) {
            return; // 收尾（恢复原节点）已在进行，勿重复发起
        }
        // 全部测完：先恢复测速前的活动节点（原始 raw PUT，不触发清连接/通知），恢复确认后再收尾。
        // 关键：恢复确认前保持 m_speedTesting=true，让轮询仍上报原节点，避免最后残留的测速节点
        // 被当成「已切换」而误弹通知/触发整表重建（见 pollNodes 里的 m_speedTesting 覆盖）。
        m_speedRestoring = true;
        auto finish = [this] {
            m_speedTesting = false;
            m_speedRestoring = false;
            emit logUpdated(QString::fromUtf8("下载测速完成"));
            emit speedTestRunning(false);
            pollNodes(); // 立即刷新一次，让最终速度/排序落到列表
        };
        if (!m_speedOriginalNode.isEmpty()) {
            const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(m_speedGroup));
            sendJsonRequest(QUrl(QString("http://%1:%2/proxies/%3").arg(m_host).arg(m_port).arg(enc)),
                            "PUT", QJsonObject{{"name", m_speedOriginalNode}},
                            [finish](bool, const QString &) { finish(); });
        } else {
            finish();
        }
        return;
    }
    if (m_speedSelecting || m_speedActive >= kSpeedConcurrency || m_speedQueue.isEmpty()) {
        return; // 正在握手 / 并发已满 / 暂无可起（等在途下载完成后再补位）
    }
    const QString name = m_speedQueue.takeFirst();
    m_speedSelecting = true; // 串行化「选组+建连」：拿到锁，直到该下载建连成功才释放
    const QString enc = QString::fromLatin1(QUrl::toPercentEncoding(m_speedGroup));
    sendJsonRequest(QUrl(QString("http://%1:%2/proxies/%3").arg(m_host).arg(m_port).arg(enc)),
                    "PUT", QJsonObject{{"name", name}}, [this, name](bool ok, const QString &error) {
                        if (!ok) {
                            m_measuredSpeeds.insert(name, 0);
                            m_speedSelecting = false;
                            emit logUpdated(QString::fromUtf8("测速选中失败 %1: %2").arg(name, error));
                            pumpSpeedTest();
                            return;
                        }
                        beginSpeedDownload(name); // 已选中 → 立刻建连（钉在该节点上）
                    });
}

void ClashService::beginSpeedDownload(const QString &name)
{
    ++m_speedActive;
    QNetworkRequest request(QUrl(QString::fromLatin1(kSpeedTestUrl)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(6000); // 停顿兜底：6s 无数据即中止，防止卡死占用并发槽
#endif
    QNetworkReply *reply = m_speedNetwork.get(request);
    auto *bytes = new qint64(0);
    auto *timer = new QElapsedTimer;
    auto *released = new bool(false);
    timer->start();
    // 释放串行锁并补位下一个握手：一旦本下载建连成功，就可以开始下一个节点的「选组+建连」。
    // 此后本下载已钉死在当前节点，后续再切组也不影响它，于是多个下载得以真正并发。
    auto release = [this, released] {
        if (!*released) {
            *released = true;
            m_speedSelecting = false;
            pumpSpeedTest();
        }
    };
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, bytes, timer, released, release] {
        const qint64 chunk = reply->readAll().size(); // 必须读掉，避免缓冲无限增长
        if (!*released) {
            // 首批数据到达 = 建连+TLS 完成：以此刻为测速基准（归零字节/计时），排除建连耗时对速度的低估，
            // 同时释放串行锁让下一个节点开始握手。
            *bytes = 0;
            timer->restart();
            release();
            return;
        }
        *bytes += chunk;
        if (*bytes >= kSpeedMaxBytes || timer->elapsed() >= kSpeedMaxMs) {
            reply->abort(); // 到达字节/时间上限：主动结束本次测速
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, name, bytes, timer, released, release] {
        const qint64 ms = timer->elapsed();
        const qint64 bps = (ms > 0) ? (*bytes * 1000 / ms) : 0; // 字节/秒——与 UI speedText 的字节单位一致
        m_measuredSpeeds.insert(name, bps);
        emit logUpdated(QString::fromUtf8("测速 %1 -> %2 KB/s").arg(name).arg(bps / 1024));
        reply->deleteLater();
        release(); // 兜底：若从未 readyRead（失败/立即结束），在此释放串行锁
        delete bytes;
        delete timer;
        delete released;
        --m_speedActive;
        pumpSpeedTest();
    });
}

void ClashService::startTrafficStream()
{
    if (m_trafficReply) {
        return; // 已有活跃流，勿重复开（否则又会泄漏连接）
    }
    const QUrl url(QString("http://%1:%2/traffic").arg(m_host).arg(m_port));
    m_trafficReply = m_network.get(QNetworkRequest(url));
    // mihomo 每秒推一行 JSON：{"up":..,"down":..,"upTotal":..,"downTotal":..}\n —— 按行读、逐条发
    connect(m_trafficReply, &QNetworkReply::readyRead, this, [this] {
        while (m_trafficReply && m_trafficReply->canReadLine()) {
            const QByteArray line = m_trafficReply->readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            emit trafficUpdated(obj.value("up").toInteger(), obj.value("down").toInteger());
            emit statusUpdated(false, true, true); // 有流数据 = 核心在跑
        }
    });
    connect(m_trafficReply, &QNetworkReply::finished, this, [this] {
        // 流结束（核心退出/端口变更/网络错误）：释放句柄，2s 看门狗会重连
        if (m_trafficReply) {
            m_trafficReply->deleteLater();
            m_trafficReply = nullptr;
        }
    });
}

void ClashService::ensureTrafficStream()
{
    if (!m_trafficReply) {
        startTrafficStream();
    }
}

void ClashService::pollConnections()
{
    sendGet(QUrl(QString("http://%1:%2/connections").arg(m_host).arg(m_port)), [this](const QJsonDocument &doc) {
        const QJsonObject obj = doc.object();
        emit connectionsUpdated(obj.value("connections").toArray().size(), obj.value("downloadTotal").toInteger());
    });
}

void ClashService::pollNodes()
{
    sendGet(QUrl(QString("http://%1:%2/proxies").arg(m_host).arg(m_port)), [this](const QJsonDocument &doc) {
        QVector<NodeInfo> nodes;
        const QJsonObject proxies = doc.object().value("proxies").toObject();

        // 沿 now 链递归找到真正承载延迟的节点：组自身通常无 history，其 now 可能仍是组
        // （如 节点选择→自动选择→某节点），需一路跟到有 history 的叶子，否则这些组不显示延迟。
        auto resolveHistory = [&proxies](const QString &start) -> QJsonArray {
            QString cur = start;
            QSet<QString> seen;
            for (int i = 0; i < 16 && !cur.isEmpty() && !seen.contains(cur); ++i) {
                seen.insert(cur);
                const QJsonObject o = proxies.value(cur).toObject();
                const QJsonArray h = o.value("history").toArray();
                if (!h.isEmpty()) {
                    return h;
                }
                cur = o.value("now").toString();
            }
            return {};
        };

        // 沿 now 链走到底，取「最终实际使用的叶子节点名」（如 节点选择→自动选择→HK-6 取 HK-6）：
        // 供组行显示「组名 → 使用节点」与「禁用」时定位真正在用的节点。
        auto resolveFinalName = [&proxies](const QString &start) -> QString {
            QString cur = start;
            QSet<QString> seen;
            for (int i = 0; i < 16 && !cur.isEmpty() && !seen.contains(cur); ++i) {
                seen.insert(cur);
                const QString next = proxies.value(cur).toObject().value("now").toString();
                if (next.isEmpty()) {
                    return cur;
                }
                cur = next;
            }
            return cur;
        };

        QString groupName = m_selectedGroup;
        QJsonObject group = proxies.value(groupName).toObject();
        QStringList groups;
        for (auto it = proxies.begin(); it != proxies.end(); ++it) {
            const QJsonObject candidate = it.value().toObject();
            if (!candidate.value("all").toArray().isEmpty()) {
                groups.push_back(it.key());
            }
        }

        if (group.value("all").toArray().isEmpty()) {
            // 主组按模式选（对齐旧项目 getProxies）：Global 用 GLOBAL；否则（Rule）用主选择组
            // 🚀 节点选择——只有在正确的主组里「应用」才真正切换路由。
            if (m_mode.compare(QLatin1String("Global"), Qt::CaseInsensitive) == 0) {
                groupName = QStringLiteral("GLOBAL");
                group = proxies.value(groupName).toObject();
            } else {
                for (auto it = proxies.begin(); it != proxies.end(); ++it) {
                    const QJsonObject candidate = it.value().toObject();
                    if (candidate.value("type").toString() != QLatin1String("Selector")
                        || candidate.value("all").toArray().isEmpty() || it.key() == QLatin1String("GLOBAL")) {
                        continue;
                    }
                    if (it.key().contains(QString::fromUtf8("节点")) || it.key().contains(QString::fromUtf8("选择"))
                        || it.key().contains(QString::fromUtf8("代理")) || it.key().contains(QLatin1String("Proxy"), Qt::CaseInsensitive)) {
                        groupName = it.key();
                        group = candidate;
                        break;
                    }
                }
            }
        }
        if (group.value("all").toArray().isEmpty()) {
            groupName = "GLOBAL";
            group = proxies.value(groupName).toObject();
        }
        if (group.value("all").toArray().isEmpty()) {
            for (auto it = proxies.begin(); it != proxies.end(); ++it) {
                const QJsonObject candidate = it.value().toObject();
                const QString type = candidate.value("type").toString();
                if (type == "Selector" || type == "URLTest" || type == "Fallback") {
                    groupName = it.key();
                    group = candidate;
                    break;
                }
            }
        }

        QString selected = group.value("now").toString(m_selectedNode);
        // 测速期间，主选择组的 now 会随逐个测速临时切换——对 UI 强制上报原活动节点，
        // 否则活动节点每秒都在变，会触发整表重建、通知乱弹（结束时 pumpSpeedTest 会恢复真实选择）。
        if (m_speedTesting && !m_speedOriginalNode.isEmpty()) {
            selected = m_speedOriginalNode;
        }
        const QJsonArray all = group.value("all").toArray();
        m_selectedGroup = groupName.isEmpty() ? QStringLiteral("GLOBAL") : groupName;
        m_selectedNode = selected;
        emit proxyGroupsUpdated(groups, m_selectedGroup);

        for (const QJsonValue &value : all) {
            const QString name = value.toString();
            if (name.isEmpty()) {
                continue;
            }

            const QJsonObject proxy = proxies.value(name).toObject();
            const QString immediateNow = proxy.value("now").toString();
            // 组（url-test/select 等）自身通常没有 history：递归沿 now 链取实际叶子的延迟/速度，
            // 否则国家组、以及 now 指向其它组的选择组（节点选择/漏网之鱼等）都会「无延迟」。
            const QJsonArray history = resolveHistory(name);
            NodeInfo node;
            node.name = name;
            // now = 最终实际使用的叶子（组）；叶子节点自身无 now
            node.now = immediateNow.isEmpty() ? QString() : resolveFinalName(name);
            node.delay = history.isEmpty() ? 0 : history.last().toObject().value("delay").toInt();
            // 速度优先取本程序实测值（不依赖核心补丁）；无实测时回退核心 history.speed（若用了补丁核心）。
            const qint64 histSpeed = history.isEmpty() ? 0 : history.last().toObject().value("speed").toInteger();
            node.speed = m_measuredSpeeds.value(name, histSpeed);
            node.active = node.name == selected;
            nodes.push_back(node);
        }

        if (nodes.isEmpty()) {
            for (auto it = proxies.begin(); it != proxies.end(); ++it) {
                const QJsonObject proxy = it.value().toObject();
                const QString type = proxy.value("type").toString();
                if (type == "Selector" || type == "URLTest" || type == "Fallback" || it.key() == "GLOBAL") {
                    continue;
                }

                const QJsonArray history = proxy.value("history").toArray();
                NodeInfo node;
                node.name = it.key();
                node.delay = history.isEmpty() ? 0 : history.last().toObject().value("delay").toInt();
                const qint64 histSpeed = history.isEmpty() ? 0 : history.last().toObject().value("speed").toInteger();
                node.speed = m_measuredSpeeds.value(it.key(), histSpeed);
                node.active = node.name == selected;
                nodes.push_back(node);
            }
        }

        // 节点为空就如实显示为空（列表显示 No nodes），不再回退到演示节点

        // 排序：对齐旧项目 clash.js getProxies —— 当前节点置顶，其次速度降序，再延迟升序，超时/无延迟垫底
        auto sortKey = [&selected](const NodeInfo &n) -> double {
            double key = (n.delay <= 0) ? 0.0 : (10000.0 - n.delay);
            if (n.speed > 0) {
                key = static_cast<double>(n.speed);
            }
            if (n.name == selected) {
                key = std::numeric_limits<double>::max();
            }
            return key;
        };
        std::stable_sort(nodes.begin(), nodes.end(), [&sortKey](const NodeInfo &a, const NodeInfo &b) {
            return sortKey(a) > sortKey(b);
        });

        emit nodesUpdated(nodes, selected);

        // 核心刚起来、首次拿到节点：自动异步测一次延迟（对齐旧项目「进列表即可见延迟」的体验）。
        // 配置已不再用 lazy:false（会卡启动），改由这里用 REST /delay 异步补测，不阻塞任何东西。
        if (!nodes.isEmpty() && !m_autoTested) {
            m_autoTested = true;
            testDelays();
        }
    });
}

void ClashService::sendGet(const QUrl &url, std::function<void(const QJsonDocument &)> onJson)
{
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(8000); // 兜底：任何请求最多挂 8s，杜绝卡死接口耗尽 6 连接/主机池
#endif
    QNetworkReply *reply = m_network.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, onJson = std::move(onJson)] {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError err = reply->error();
        reply->deleteLater();

        if (err != QNetworkReply::NoError) {
            // 只有「核心确实连不上」(连接被拒/对端关闭) 才清空 UI 并允许下次自动测延迟。
            // 请求超时/被取消(连接池一时繁忙)不代表核心掉线——若在此清空列表或重置 m_autoTested，
            // 会造成「列表闪空」以及与自动测延迟的死循环（超时→重置→再测→占满池→再超时）。
            if (err == QNetworkReply::ConnectionRefusedError || err == QNetworkReply::RemoteHostClosedError) {
                emit trafficUpdated(0, 0);
                emit connectionsUpdated(0, 0);
                emit nodesUpdated({}, QString());
                m_autoTested = false; // 核心掉线：下次起来重新自动测一次延迟
                m_measuredSpeeds.clear();
                if (m_speedTesting) { // 测速途中核心掉线：中止本轮，清运行态（在途下载会各自 finished 退出）
                    m_speedTesting = false;
                    m_speedRestoring = false;
                    m_speedSelecting = false;
                    m_speedActive = 0;
                    m_speedQueue.clear();
                    emit speedTestRunning(false);
                }
            }
            return;
        }

        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
        if (error.error == QJsonParseError::NoError) {
            onJson(doc);
        }
    });
}

void ClashService::sendJsonRequest(const QUrl &url, const QByteArray &method, const QJsonObject &payload, std::function<void(bool, QString)> onDone)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(8000); // 兜底：selectNode 的 PUT 等最多挂 8s，不会永久占用连接
#endif
    QNetworkReply *reply = m_network.sendCustomRequest(request, method, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, onDone = std::move(onDone)] {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString error = ok ? QString() : (body.isEmpty() ? reply->errorString() : QString::fromUtf8(body));
        reply->deleteLater();
        onDone(ok, error);
    });
}

void ClashService::sendDelete(const QUrl &url, std::function<void(bool, QString)> onDone)
{
    QNetworkRequest request(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(8000);
#endif
    QNetworkReply *reply = m_network.deleteResource(request);
    connect(reply, &QNetworkReply::finished, this, [reply, onDone = std::move(onDone)] {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString error = ok ? QString() : (body.isEmpty() ? reply->errorString() : QString::fromUtf8(body));
        reply->deleteLater();
        onDone(ok, error);
    });
}

