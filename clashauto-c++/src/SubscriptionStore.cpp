#include "SubscriptionStore.h"

#include "SubParser.h"

#include <QDir>
#include <QFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

SubscriptionStore::SubscriptionStore(AppConfig config, QObject *parent)
    : QObject(parent), m_config(std::move(config))
{
}

QString SubscriptionStore::path() const
{
    return QDir(m_config.userDir).filePath("subscribe.yaml");
}

QVector<SubscriptionSummary> SubscriptionStore::load()
{
    ensureFile();
    QVector<SubscriptionSummary> result;
    const QStringList lines = readText().split('\n');
    SubscriptionSummary current;
    bool inSub = false;
    bool inList = false;
    bool inNode = false;
    bool currentNodeUse = true;

    auto finishNode = [&] {
        if (!inNode) {
            return;
        }
        current.nodeCount++;
        if (currentNodeUse) {
            current.enabledNodeCount++;
        }
        inNode = false;
        currentNodeUse = true;
    };

    auto finishSub = [&] {
        finishNode();
        if (inSub) {
            result.push_back(current);
        }
        current = {};
        inSub = false;
        inList = false;
    };

    for (QString line : lines) {
        line.remove('\r');
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }

        if (line.startsWith("- ")) {
            finishSub();
            inSub = true;
            const QString first = line.mid(2).trimmed();
            if (first.startsWith("name:")) {
                current.name = scalar(first.mid(5));
            }
            continue;
        }

        if (!inSub) {
            continue;
        }

        if (line.startsWith("  ") && !line.startsWith("    ")) {
            const QString field = line.mid(2).trimmed();
            if (field.startsWith("name:")) {
                current.name = scalar(field.mid(5));
            } else if (field.startsWith("url:")) {
                current.url = scalar(field.mid(4));
            } else if (field.startsWith("type:")) {
                current.type = scalar(field.mid(5));
            } else if (field.startsWith("use:")) {
                current.use = scalar(field.mid(4)).toLower() == "true";
            } else if (field.startsWith("updateTime:")) {
                current.updateTime = scalar(field.mid(11)).toInt();
            } else if (field.startsWith("list:")) {
                inList = true;
            }
            continue;
        }

        if (inList && line.startsWith("    - ")) {
            finishNode();
            inNode = true;
            currentNodeUse = true;
            continue;
        }

        if (inNode && line.startsWith("      ")) {
            const QString field = line.mid(6).trimmed();
            if (field.startsWith("use:")) {
                currentNodeUse = scalar(field.mid(4)).toLower() == "true";
            }
        }
    }

    finishSub();
    return result;
}

bool SubscriptionStore::setSubscriptionEnabled(int index, bool enabled)
{
    ensureFile();
    QStringList lines = readText().split('\n');
    int current = -1;
    int insertAt = -1;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
            lines[i] = line;
        }
        if (line.startsWith("- ")) {
            current++;
            insertAt = i + 1;
            continue;
        }
        if (current == index && line.startsWith("  ") && !line.startsWith("    ")) {
            if (line.trimmed().startsWith("use:")) {
                lines[i] = QString("  use: %1").arg(enabled ? "true" : "false");
                return writeText(lines.join('\n'));
            }
            insertAt = i + 1;
        }
        if (current > index) {
            break;
        }
    }

    if (index >= 0 && insertAt >= 0) {
        lines.insert(insertAt, QString("  use: %1").arg(enabled ? "true" : "false"));
        return writeText(lines.join('\n'));
    }
    return false;
}

bool SubscriptionStore::setSubscriptionUpdateTime(int index, int minutes)
{
    // 替换目标订阅块的 updateTime: 标量；缺失时插到该块内（不越界到下一订阅）
    ensureFile();
    QStringList lines = readText().split('\n');
    int current = -1;
    int insertAt = -1;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
            lines[i] = line;
        }
        if (line.startsWith("- ")) {
            if (current == index) {
                break; // 已离开目标订阅，停在其块末尾记录的 insertAt
            }
            current++;
            if (current == index) {
                insertAt = i + 1;
            }
            continue;
        }
        if (current == index && line.startsWith("  ") && !line.startsWith("    ")) {
            if (line.trimmed().startsWith("updateTime:")) {
                lines[i] = QString("  updateTime: %1").arg(minutes);
                return writeText(lines.join('\n'));
            }
            insertAt = i + 1;
        }
    }

    if (index >= 0 && insertAt >= 0) {
        lines.insert(insertAt, QString("  updateTime: %1").arg(minutes));
        return writeText(lines.join('\n'));
    }
    return false;
}

QVector<SubscriptionNodeSummary> SubscriptionStore::nodes(int subscriptionIndex)
{
    ensureFile();
    QVector<SubscriptionNodeSummary> result;
    QStringList lines = readText().split('\n');
    int currentSub = -1;
    bool inList = false;
    bool inNode = false;
    SubscriptionNodeSummary current;

    auto finishNode = [&] {
        if (inNode) {
            result.push_back(current);
        }
        current = {};
        current.use = true;
        inNode = false;
    };

    for (QString line : lines) {
        line.remove('\r');
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        if (line.startsWith("- ")) {
            if (currentSub == subscriptionIndex) {
                finishNode();
                break;
            }
            currentSub++;
            inList = false;
            continue;
        }
        if (currentSub != subscriptionIndex) {
            continue;
        }
        if (line.startsWith("  ") && !line.startsWith("    ") && line.trimmed().startsWith("list:")) {
            inList = true;
            continue;
        }
        if (inList && line.startsWith("    - ")) {
            finishNode();
            inNode = true;
            current.use = true;
            const QString field = line.mid(6).trimmed();
            if (field.startsWith("name:")) {
                current.name = scalar(field.mid(5));
            }
            continue;
        }
        if (inNode && line.startsWith("      ")) {
            const QString field = line.mid(6).trimmed();
            if (field.startsWith("name:")) {
                current.name = scalar(field.mid(5));
            } else if (field.startsWith("server:")) {
                current.server = scalar(field.mid(7));
            } else if (field.startsWith("port:")) {
                current.port = scalar(field.mid(5));
            } else if (field.startsWith("use:")) {
                current.use = scalar(field.mid(4)).toLower() == "true";
            }
        }
    }
    finishNode();
    return result;
}

bool SubscriptionStore::setNodeEnabled(int subscriptionIndex, int nodeIndex, bool enabled)
{
    ensureFile();
    QStringList lines = readText().split('\n');
    int currentSub = -1;
    int currentNode = -1;
    bool inList = false;
    int insertAt = -1;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
            lines[i] = line;
        }
        if (line.startsWith("- ")) {
            if (currentSub == subscriptionIndex && currentNode == nodeIndex && insertAt >= 0) {
                lines.insert(insertAt, QString("      use: %1").arg(enabled ? "true" : "false"));
                return writeText(lines.join('\n'));
            }
            currentSub++;
            currentNode = -1;
            inList = false;
            continue;
        }
        if (currentSub != subscriptionIndex) {
            continue;
        }
        if (line.startsWith("  ") && !line.startsWith("    ") && line.trimmed().startsWith("list:")) {
            inList = true;
            continue;
        }
        if (inList && line.startsWith("    - ")) {
            if (currentNode == nodeIndex && insertAt >= 0) {
                lines.insert(insertAt, QString("      use: %1").arg(enabled ? "true" : "false"));
                return writeText(lines.join('\n'));
            }
            currentNode++;
            insertAt = i + 1;
            continue;
        }
        if (currentNode == nodeIndex && line.startsWith("      ")) {
            if (line.trimmed().startsWith("use:")) {
                lines[i] = QString("      use: %1").arg(enabled ? "true" : "false");
                return writeText(lines.join('\n'));
            }
            insertAt = i + 1;
        }
    }

    if (currentSub == subscriptionIndex && currentNode == nodeIndex && insertAt >= 0) {
        lines.insert(insertAt, QString("      use: %1").arg(enabled ? "true" : "false"));
        return writeText(lines.join('\n'));
    }
    return false;
}

bool SubscriptionStore::setAllNodesEnabled(int subscriptionIndex, bool enabled)
{
    // 复用逐个 setNodeEnabled（每次读写整份 YAML）；节点数不大，简单可靠
    const int count = nodes(subscriptionIndex).size();
    bool any = false;
    for (int i = 0; i < count; ++i) {
        if (setNodeEnabled(subscriptionIndex, i, enabled)) {
            any = true;
        }
    }
    return any;
}

bool SubscriptionStore::addSubscription(const QString &name, const QString &url, const QString &type)
{
    ensureFile();
    QString text = readText().trimmed();
    const QString block = QString("- name: %1\n"
                                  "  url: %2\n"
                                  "  type: %3\n"
                                  "  use: false\n"
                                  "  updateTime: 15\n"
                                  "  speedtest: false\n"
                                  "  proxy: false\n"
                                  "  list: []\n")
        .arg(quote(name.isEmpty() ? url : name), quote(url), quote(type.isEmpty() ? "sub" : type));

    if (text.isEmpty() || text == "[]") {
        text = block;
    } else {
        text += "\n" + block;
    }
    return writeText(text);
}

bool SubscriptionStore::editSubscription(int index, const QString &name, const QString &url, const QString &type)
{
    if (index < 0) {
        return false;
    }
    ensureFile();
    QStringList lines = readText().split('\n');
    int current = -1;
    bool inTarget = false;
    bool nameSet = false;
    bool urlSet = false;
    bool typeSet = false;
    int insertAt = -1;
    const QString finalName = name.isEmpty() ? url : name;
    const QString finalType = type.isEmpty() ? "sub" : type;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
            lines[i] = line;
        }
        if (line.startsWith("- ")) {
            if (inTarget) {
                break;
            }
            current++;
            if (current == index) {
                inTarget = true;
                insertAt = i + 1;
                const QString rest = line.mid(2).trimmed();
                if (rest.startsWith("name:")) {
                    lines[i] = QString("- name: %1").arg(quote(finalName));
                    nameSet = true;
                }
            }
            continue;
        }
        if (!inTarget) {
            continue;
        }
        if (line.startsWith("  ") && !line.startsWith("    ")) {
            const QString field = line.trimmed();
            if (field.startsWith("name:")) {
                lines[i] = QString("  name: %1").arg(quote(finalName));
                nameSet = true;
            } else if (field.startsWith("url:")) {
                lines[i] = QString("  url: %1").arg(quote(url));
                urlSet = true;
            } else if (field.startsWith("type:")) {
                lines[i] = QString("  type: %1").arg(quote(finalType));
                typeSet = true;
            }
        }
    }

    if (!inTarget) {
        return false;
    }

    QStringList missing;
    if (!nameSet) {
        missing << QString("  name: %1").arg(quote(finalName));
    }
    if (!urlSet) {
        missing << QString("  url: %1").arg(quote(url));
    }
    if (!typeSet) {
        missing << QString("  type: %1").arg(quote(finalType));
    }
    for (int j = missing.size() - 1; j >= 0 && insertAt >= 0; --j) {
        lines.insert(insertAt, missing[j]);
    }
    return writeText(lines.join('\n'));
}

bool SubscriptionStore::removeSubscription(int index)
{
    if (index < 0) {
        return false;
    }
    ensureFile();
    QStringList lines = readText().split('\n');
    int current = -1;
    int blockStart = -1;
    int blockEnd = -1;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
            lines[i] = line;
        }
        if (line.startsWith("- ")) {
            current++;
            if (current == index) {
                blockStart = i;
            } else if (current == index + 1) {
                blockEnd = i;
                break;
            }
        }
    }

    if (blockStart < 0) {
        return false;
    }
    if (blockEnd < 0) {
        blockEnd = lines.size();
    }
    lines.erase(lines.begin() + blockStart, lines.begin() + blockEnd);

    QString text = lines.join('\n').trimmed();
    if (text.isEmpty()) {
        text = "[]";
    }
    return writeText(text);
}

void SubscriptionStore::updateSubscription(int index)
{
    const SubscriptionSummary summary = summaryAt(index);
    if (summary.url.trimmed().isEmpty()) {
        emit subscriptionUpdated(index, false, "Subscription URL is empty");
        return;
    }

    const QUrl url = effectiveUrl(summary);
    if (url.isValid() && (url.scheme() == "http" || url.scheme() == "https")) {
        QNetworkRequest request(url);
        // 订阅服务器常按 UA 返回不同格式；用 clash 系 UA 避免被拦，且本地解析器同时兼容
        // Clash YAML 与 base64 URI 列表两种响应。
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("clash.meta"));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = m_network.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, index] {
            const QByteArray body = reply->readAll();
            const bool ok = reply->error() == QNetworkReply::NoError;
            const QString error = reply->errorString();
            reply->deleteLater();
            if (!ok) {
                emit subscriptionUpdated(index, false, error);
                return;
            }
            updateSubscriptionFromText(index, QString::fromUtf8(body));
        });
        return;
    }

    QFile file(summary.url);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        emit subscriptionUpdated(index, false, QString("Cannot read %1").arg(summary.url));
        return;
    }
    updateSubscriptionFromText(index, QString::fromUtf8(file.readAll()));
}

SubscriptionSummary SubscriptionStore::summaryAt(int index)
{
    const QVector<SubscriptionSummary> summaries = load();
    if (index < 0 || index >= summaries.size()) {
        return {};
    }
    return summaries[index];
}

void SubscriptionStore::updateSubscriptionFromText(int index, const QString &yaml)
{
    int count = 0;
    // 本地解析：把订阅原始内容（base64 URI 列表 / Clash YAML）转成 Clash proxies:，再抽取节点
    const QString clash = SubParser::toClashProxies(yaml);
    const QString source = clash.isEmpty() ? yaml : clash;
    const QString nodeList = parseProxyList(source, existingNodeUseByEndpoint(index), existingNodeYamlByEndpoint(index), &count);
    if (count <= 0 || nodeList.trimmed().isEmpty()) {
        emit subscriptionUpdated(index, false, "No proxies found in subscription");
        return;
    }
    const bool ok = replaceSubscriptionList(index, nodeList);
    emit subscriptionUpdated(index, ok, ok ? QString("Updated %1 nodes").arg(count) : "Failed to write subscribe.yaml");
}

bool SubscriptionStore::updateSubscriptionFromTextForTest(int index, const QString &yaml, QString *message)
{
    int count = 0;
    const QString clash = SubParser::toClashProxies(yaml);
    const QString source = clash.isEmpty() ? yaml : clash;
    const QString nodeList = parseProxyList(source, existingNodeUseByEndpoint(index), existingNodeYamlByEndpoint(index), &count);
    if (count <= 0 || nodeList.trimmed().isEmpty()) {
        if (message) {
            *message = "No proxies found in subscription";
        }
        return false;
    }
    const bool ok = replaceSubscriptionList(index, nodeList);
    if (message) {
        *message = ok ? QString("Updated %1 nodes").arg(count) : "Failed to write subscribe.yaml";
    }
    return ok;
}

QString SubscriptionStore::effectiveUrlForTest(int index)
{
    return effectiveUrl(summaryAt(index)).toString();
}

QUrl SubscriptionStore::effectiveUrl(const SubscriptionSummary &summary) const
{
    // 一律取订阅原始 URL 直接抓取，节点信息改为本地解析（SubParser），
    // 不再经远程 subconverter（api.v1.mk）转换——更快、更隐私，也不依赖第三方服务。
    return QUrl(summary.url);
}

QMap<QString, bool> SubscriptionStore::existingNodeUseByEndpoint(int index) const
{
    QMap<QString, bool> states;
    QStringList lines = readText().split('\n');
    int currentSub = -1;
    bool inList = false;
    bool inNode = false;
    QString server;
    QString port;
    bool use = true;

    auto finishNode = [&] {
        if (inNode && !server.isEmpty() && !port.isEmpty()) {
            states.insert(QString("%1:%2").arg(server, port), use);
        }
        inNode = false;
        server.clear();
        port.clear();
        use = true;
    };

    for (QString line : lines) {
        line.remove('\r');
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        if (line.startsWith("- ")) {
            if (currentSub == index) {
                finishNode();
                break;
            }
            currentSub++;
            inList = false;
            continue;
        }
        if (currentSub != index) {
            continue;
        }
        if (line.startsWith("  ") && !line.startsWith("    ") && line.trimmed().startsWith("list:")) {
            inList = true;
            continue;
        }
        if (inList && line.startsWith("    - ")) {
            finishNode();
            inNode = true;
            continue;
        }
        if (inNode && line.startsWith("      ")) {
            const QString field = line.mid(6).trimmed();
            if (field.startsWith("server:")) {
                server = scalar(field.mid(7));
            } else if (field.startsWith("port:")) {
                port = scalar(field.mid(5));
            } else if (field.startsWith("use:")) {
                use = scalar(field.mid(4)).toLower() == "true";
            }
        }
    }
    finishNode();
    return states;
}

QMap<QString, QString> SubscriptionStore::existingNodeYamlByEndpoint(int index) const
{
    QMap<QString, QString> nodes;
    QStringList lines = readText().split('\n');
    int currentSub = -1;
    bool inList = false;
    bool inNode = false;
    QString server;
    QString port;
    QStringList currentNode;

    auto finishNode = [&] {
        if (inNode && !server.isEmpty() && !port.isEmpty() && !currentNode.isEmpty()) {
            nodes.insert(QString("%1:%2").arg(server, port), currentNode.join('\n'));
        }
        inNode = false;
        server.clear();
        port.clear();
        currentNode.clear();
    };

    for (QString line : lines) {
        line.remove('\r');
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        if (line.startsWith("- ")) {
            if (currentSub == index) {
                finishNode();
                break;
            }
            currentSub++;
            inList = false;
            continue;
        }
        if (currentSub != index) {
            continue;
        }
        if (line.startsWith("  ") && !line.startsWith("    ") && line.trimmed().startsWith("list:")) {
            inList = true;
            continue;
        }
        if (inList && line.startsWith("    - ")) {
            finishNode();
            inNode = true;
            currentNode << line;
            continue;
        }
        if (inNode && line.startsWith("      ")) {
            currentNode << line;
            const QString field = line.mid(6).trimmed();
            if (field.startsWith("server:")) {
                server = scalar(field.mid(7));
            } else if (field.startsWith("port:")) {
                port = scalar(field.mid(5));
            }
        }
    }
    finishNode();
    return nodes;
}

QString SubscriptionStore::parseProxyList(const QString &yaml, const QMap<QString, bool> &previousUse, const QMap<QString, QString> &previousYaml, int *count) const
{
    if (count) {
        *count = 0;
    }
    const QStringList lines = yaml.split('\n');
    QStringList output;
    QSet<QString> seenEndpoints;
    bool inProxies = false;
    bool inNode = false;
    QStringList currentNode;
    int proxiesIndent = 0;

    auto finishNode = [&] {
        if (!inNode || currentNode.isEmpty()) {
            return;
        }
        bool hasName = false;
        QString name;
        QString server;
        QString port;
        for (const QString &line : currentNode) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith("name:") || trimmed.startsWith("- name:")) {
                hasName = true;
                name = scalar(trimmed.startsWith("- name:") ? trimmed.mid(7) : trimmed.mid(5));
            } else if (trimmed.startsWith("server:")) {
                server = scalar(trimmed.mid(7));
            } else if (trimmed.startsWith("port:")) {
                port = scalar(trimmed.mid(5));
                break;
            }
        }
        if (hasName && nodeAllowed(name)) {
            for (const QString &line : currentNode) {
                output << line;
            }
            const QString endpoint = QString("%1:%2").arg(server, port);
            seenEndpoints.insert(endpoint);
            const bool use = previousUse.contains(endpoint) ? previousUse.value(endpoint) : true;
            output << QString("      use: %1").arg(use ? "true" : "false");
            if (count) {
                ++(*count);
            }
        }
        currentNode.clear();
        inNode = false;
    };

    for (QString line : lines) {
        line.remove('\r');
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
        }
        if (!inProxies) {
            if (line.trimmed().startsWith("proxies:")) {
                inProxies = true;
                proxiesIndent = line.indexOf('p');
            }
            continue;
        }

        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#')) {
            continue;
        }
        const int indent = line.size() - line.trimmed().size();
        if (indent <= proxiesIndent && !trimmed.startsWith("- ")) {
            finishNode();
            break;
        }

        if (trimmed.startsWith("- ")) {
            finishNode();
            inNode = true;
            const QString rest = trimmed.mid(2).trimmed();
            if (rest.startsWith("name:")) {
                currentNode << QString("    - name: %1").arg(rest.mid(5).trimmed());
            } else {
                currentNode << QString("    - %1").arg(rest);
            }
            continue;
        }

        if (inNode && line.startsWith("  ")) {
            const QString field = trimmed;
            if (field.startsWith("use:") || field.startsWith("delay:") || field.startsWith("speed:")) {
                continue;
            }
            currentNode << QString("      %1").arg(field);
        }
    }

    finishNode();
    if (m_config.increment) {
        for (auto it = previousYaml.begin(); it != previousYaml.end(); ++it) {
            if (!seenEndpoints.contains(it.key())) {
                output << it.value();
                if (count) {
                    ++(*count);
                }
            }
        }
    }
    return output.join('\n');
}

bool SubscriptionStore::nodeAllowed(const QString &name) const
{
    if (m_config.allowRuleEnabled && !m_config.allowRule.isEmpty()) {
        QRegularExpression re(m_config.allowRule, QRegularExpression::CaseInsensitiveOption);
        if (re.isValid() && !re.match(name).hasMatch()) {
            return false;
        }
    }
    if (m_config.noAllowRuleEnabled && !m_config.noAllowRule.isEmpty()) {
        QRegularExpression re(m_config.noAllowRule, QRegularExpression::CaseInsensitiveOption);
        if (re.isValid() && re.match(name).hasMatch()) {
            return false;
        }
    }
    return true;
}

bool SubscriptionStore::replaceSubscriptionList(int index, const QString &nodeListYaml)
{
    ensureFile();
    QStringList lines = readText().split('\n');
    int current = -1;
    int listLine = -1;
    int listEnd = -1;

    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (!line.isEmpty() && line.front() == QChar::ByteOrderMark) {
            line.remove(0, 1);
            lines[i] = line;
        }
        if (line.startsWith("- ")) {
            if (current == index && listLine >= 0) {
                listEnd = i;
                break;
            }
            current++;
            continue;
        }
        if (current == index && line.startsWith("  ") && !line.startsWith("    ") && line.trimmed().startsWith("list:")) {
            listLine = i;
        }
    }

    if (current == index && listLine >= 0 && listEnd < 0) {
        listEnd = lines.size();
    }
    if (listLine < 0) {
        return false;
    }

    QStringList replacement;
    replacement << "  list:";
    for (const QString &line : nodeListYaml.split('\n')) {
        if (!line.trimmed().isEmpty()) {
            replacement << line;
        }
    }

    lines.erase(lines.begin() + listLine, lines.begin() + listEnd);
    for (int i = replacement.size() - 1; i >= 0; --i) {
        lines.insert(listLine, replacement[i]);
    }
    return writeText(lines.join('\n'));
}

void SubscriptionStore::ensureFile() const
{
    QDir().mkpath(m_config.userDir);
    const QString bundled = QDir(m_config.sourceRoot).filePath("config/subscribe.yaml");
    if (!QFile::exists(path())) {
        if (QFile::exists(bundled)) {
            QFile::copy(bundled, path());
        } else {
            QFile file(path());
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                file.write("[]\n");
            }
        }
    }
}

QString SubscriptionStore::readText() const
{
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool SubscriptionStore::writeText(const QString &text) const
{
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return false;
    }
    file.write(text.toUtf8());
    if (!text.endsWith('\n')) {
        file.write("\n");
    }
    return true;
}

QString SubscriptionStore::scalar(const QString &value) const
{
    QString out = value.trimmed();
    const qsizetype comment = out.indexOf(" #");
    if (comment >= 0) {
        out = out.left(comment).trimmed();
    }
    if ((out.startsWith('"') && out.endsWith('"')) || (out.startsWith('\'') && out.endsWith('\''))) {
        out = out.mid(1, out.size() - 2);
    }
    out.replace("''", "'");
    return out.trimmed();
}

QString SubscriptionStore::quote(const QString &value) const
{
    QString escaped = value;
    escaped.replace("'", "''");
    return QString("'%1'").arg(escaped);
}
