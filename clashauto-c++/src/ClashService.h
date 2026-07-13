#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <functional>

class QJsonDocument;
class QJsonObject;

struct NodeInfo {
    QString name;
    QString now;
    int delay = 0;
    qint64 speed = 0;
    bool active = false;

    bool operator==(const NodeInfo &o) const
    {
        return name == o.name && now == o.now && delay == o.delay
               && speed == o.speed && active == o.active;
    }
    bool operator!=(const NodeInfo &o) const { return !(*this == o); }
};

class ClashService final : public QObject
{
    Q_OBJECT

public:
    explicit ClashService(QObject *parent = nullptr);

    void start();
    void setEndpoint(const QString &host, int port); // REST API 地址（对齐 config.uiPort，默认 9191）
    void setMode(const QString &mode);
    void setSelectedGroup(const QString &group);
    void setClearConnectionsOnSwitch(bool enabled);
    void selectNode(const QString &name);
    void clearConnections();
    void fetchConnections(std::function<void(QJsonArray)> callback);
    void closeConnection(const QString &id);
    void refreshNodes();
    void testDelays();
    void testNodeDelays(const QStringList &names);

signals:
    void trafficUpdated(qint64 up, qint64 down);
    void connectionsUpdated(int count, qint64 downloadTotal);
    void nodesUpdated(QVector<NodeInfo> nodes, QString selected);
    void proxyGroupsUpdated(QStringList groups, QString selectedGroup);
    void logUpdated(QString message);
    void statusUpdated(bool tun, bool proxy, bool core);

private:
    void pollTraffic();
    void pollConnections();
    void pollNodes();
    void sendGet(const QUrl &url, std::function<void(const QJsonDocument &)> onJson);
    void sendJsonRequest(const QUrl &url, const QByteArray &method, const QJsonObject &payload, std::function<void(bool, QString)> onDone);
    void sendDelete(const QUrl &url, std::function<void(bool, QString)> onDone);

    QNetworkAccessManager m_network;
    QTimer m_trafficTimer;
    QTimer m_connectionsTimer;
    QTimer m_nodesTimer;
    QString m_host = "127.0.0.1";
    int m_port = 9191; // 默认对齐 AppConfig::uiPort / default.yaml；实际由 setEndpoint 按配置设定
    QString m_selectedGroup; // 空 = 未定；首轮 pollNodes 选主组（按模式：Rule→🚀 节点选择, Global→GLOBAL）
    QString m_mode = "Rule";  // 当前代理模式，决定主选择组（对齐旧项目 getProxies）
    QString m_selectedNode;
    bool m_clearOnSwitch = true;
    bool m_autoTested = false; // 核心起来后自动测一次延迟（异步，非阻塞）；核心掉线后重置以便重测
};
