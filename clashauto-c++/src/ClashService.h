#pragma once

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
};

class ClashService final : public QObject
{
    Q_OBJECT

public:
    explicit ClashService(QObject *parent = nullptr);

    void start();
    void setMode(const QString &mode);
    void setSelectedGroup(const QString &group);
    void selectNode(const QString &name);
    void clearConnections();
    void refreshNodes();

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
    QVector<NodeInfo> fallbackNodes() const;

    QNetworkAccessManager m_network;
    QTimer m_trafficTimer;
    QTimer m_connectionsTimer;
    QTimer m_nodesTimer;
    QString m_host = "127.0.0.1";
    int m_port = 9090;
    QString m_selectedGroup = "GLOBAL";
    QString m_selectedNode;
};
