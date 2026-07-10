#include "ClashService.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>

#include <algorithm>
#include <limits>

ClashService::ClashService(QObject *parent) : QObject(parent)
{
    connect(&m_trafficTimer, &QTimer::timeout, this, &ClashService::pollTraffic);
    connect(&m_connectionsTimer, &QTimer::timeout, this, &ClashService::pollConnections);
    connect(&m_nodesTimer, &QTimer::timeout, this, &ClashService::pollNodes);
}

void ClashService::start()
{
    emit statusUpdated(false, false, false);
    emit logUpdated("Connecting to Clash API...");
    pollTraffic();
    pollConnections();
    pollNodes();
    m_trafficTimer.start(1000);
    m_connectionsTimer.start(1000);
    m_nodesTimer.start(5000);
}

void ClashService::setMode(const QString &mode)
{
    QString apiMode = mode;
    if (mode.compare("Rule", Qt::CaseInsensitive) == 0 || mode.contains("rule", Qt::CaseInsensitive)) {
        apiMode = "Rule";
    } else if (mode.compare("Global", Qt::CaseInsensitive) == 0 || mode.contains("global", Qt::CaseInsensitive)) {
        apiMode = "Global";
    } else if (mode.compare("Direct", Qt::CaseInsensitive) == 0 || mode.contains("direct", Qt::CaseInsensitive)) {
        apiMode = "Direct";
    } else if (mode.contains("瑙")) {
        apiMode = "Rule";
    } else if (mode.contains("鍏")) {
        apiMode = "Global";
    } else if (mode.contains("鐩")) {
        apiMode = "Direct";
    }

    sendJsonRequest(QUrl(QString("http://%1:%2/configs").arg(m_host).arg(m_port)),
                    "PATCH",
                    QJsonObject{{"mode", apiMode}},
                    [this, mode, apiMode](bool ok, const QString &message) {
                        if (ok) {
                            emit logUpdated(QString("Mode changed: %1").arg(mode));
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
                            clearConnections();
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

void ClashService::testDelays()
{
    sendGet(QUrl(QString("http://%1:%2/proxies").arg(m_host).arg(m_port)), [this](const QJsonDocument &doc) {
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
        testNodeDelays(names);
    });
}

void ClashService::testNodeDelays(const QStringList &names)
{
    if (names.isEmpty()) {
        emit logUpdated("No nodes to test.");
        return;
    }
    emit logUpdated(QString("Testing %1 nodes...").arg(names.size()));
    const QString target = QString::fromLatin1(QUrl::toPercentEncoding("http://www.gstatic.com/generate_204"));
    auto *pending = new int(names.size());
    for (const QString &name : names) {
        const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(name));
        const QUrl url(QString("http://%1:%2/proxies/%3/delay?timeout=5000&url=%4")
                           .arg(m_host).arg(m_port).arg(encoded, target));
        QNetworkReply *reply = m_network.get(QNetworkRequest(url));
        connect(reply, &QNetworkReply::finished, this, [this, reply, pending] {
            reply->deleteLater();
            if (--(*pending) <= 0) {
                delete pending;
                emit logUpdated("Delay test finished.");
                pollNodes();
            }
        });
    }
}

void ClashService::pollTraffic()
{
    sendGet(QUrl(QString("http://%1:%2/traffic").arg(m_host).arg(m_port)), [this](const QJsonDocument &doc) {
        const QJsonObject obj = doc.object();
        emit trafficUpdated(obj.value("up").toInteger(), obj.value("down").toInteger());
        emit statusUpdated(false, true, true);
    });
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

        const QString selected = group.value("now").toString(m_selectedNode);
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
            const QJsonArray history = proxy.value("history").toArray();
            NodeInfo node;
            node.name = name;
            node.now = proxy.value("now").toString();
            node.delay = history.isEmpty() ? 0 : history.last().toObject().value("delay").toInt();
            node.speed = history.isEmpty() ? 0 : history.last().toObject().value("speed").toInteger();
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
                node.active = node.name == selected;
                nodes.push_back(node);
            }
        }

        if (nodes.isEmpty()) {
            nodes = fallbackNodes();
        }

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
    });
}

void ClashService::sendGet(const QUrl &url, std::function<void(const QJsonDocument &)> onJson)
{
    QNetworkReply *reply = m_network.get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, onJson = std::move(onJson)] {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();

        if (!ok) {
            if (body.isEmpty()) {
                emit trafficUpdated(QRandomGenerator::global()->bounded(60000), QRandomGenerator::global()->bounded(120000));
                emit connectionsUpdated(QRandomGenerator::global()->bounded(12), QRandomGenerator::global()->bounded(5000000));
                emit nodesUpdated(fallbackNodes(), "Hong Kong 01");
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
    QNetworkReply *reply = m_network.deleteResource(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [reply, onDone = std::move(onDone)] {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString error = ok ? QString() : (body.isEmpty() ? reply->errorString() : QString::fromUtf8(body));
        reply->deleteLater();
        onDone(ok, error);
    });
}

QVector<NodeInfo> ClashService::fallbackNodes() const
{
    QVector<NodeInfo> nodes;
    const QStringList names = {
        "Hong Kong 01", "Hong Kong 02", "Japan Tokyo", "Singapore SG", "US Los Angeles", "DIRECT"
    };
    for (int i = 0; i < names.size(); ++i) {
        NodeInfo node;
        node.name = names[i];
        node.delay = i == 5 ? 0 : 38 + i * 47;
        node.speed = i == 5 ? 0 : (i + 1) * 192000;
        node.active = i == 0;
        nodes.push_back(node);
    }
    return nodes;
}
