#include "ConfigBuilder.h"

#include "DeviceStore.h" // socksUser() / kGatewayPort / DevicePolicyMode 派生须与此处生成的规则一致

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

ConfigBuilder::ConfigBuilder(AppConfig config) : m_config(std::move(config))
{
}

QString ConfigBuilder::ensureFullConfig(bool tunEnabled)
{
    const QString defaultPath = QDir(m_config.configDir).filePath("default.yaml");
    const QString bundledDefault = QStringLiteral(":/assets/bundle/config/default.yaml"); // 内嵌种子
    if (!QFile::exists(defaultPath) && QFile::exists(bundledDefault)) {
        QFile::copy(bundledDefault, defaultPath);
        AppConfig::makeWritable(defaultPath); // qrc 种子只读；区域/规则编辑器要写回
    }

    const QString pluginPath = QDir(m_config.configDir).filePath("plugin.yaml");
    const QString bundledPlugin = QStringLiteral(":/assets/bundle/config/plugin.yaml"); // 内嵌种子
    if (!QFile::exists(pluginPath) && QFile::exists(bundledPlugin)) {
        QFile::copy(bundledPlugin, pluginPath);
        AppConfig::makeWritable(pluginPath);
    }

    QString yaml = readText(QFile::exists(defaultPath) ? defaultPath : bundledDefault);
    const QString plugin = readText(QFile::exists(pluginPath) ? pluginPath : bundledPlugin);

    yaml = mergePlugin(yaml, plugin);
    // 端口换位（见 setCoreMixedPort）：进程内入站占走 mixedPort 时，核心挪到 mixedPort+1 只作回退出口，
    // 系统代理指向的端口（mixedPort）不用变。平时 m_coreMixedPort=0，照写 m_config.mixedPort，行为不变。
    yaml = setScalar(yaml, "mixed-port", QString::number(coreMixedPort()));
    yaml = setScalar(yaml, "external-controller", QString("'%1:%2'").arg(m_config.host).arg(m_config.uiPort));
    // 安全加固：给 REST API 设访问密钥（secret 键 default.yaml 通常没有，setScalar 缺失时会追加/前置）
    if (!m_config.secret.isEmpty()) {
        yaml = setScalar(yaml, "secret", QString("'%1'").arg(m_config.secret));
    }
    // 安全加固：默认关闭 allow-lan，混合端口只监听本机，避免暴露成开放代理
    yaml = setScalar(yaml, "allow-lan", "false");
    // 进程归属：让核心把发起连接的进程名填进 metadata.process（连接窗口要显示它）。
    // always 而不是 strict —— strict 由核心自行决定要不要查（没有 PROCESS-NAME 规则时它就不查了，
    // 于是 UI 永远拿不到进程名）。代价是每条新连接多一次本机套接字表查询，核心内部有缓存。
    // 注意：这只对**本机发起**的连接有意义；局域网设备的进程跑在别人机器上，本机表里查不到
    //（网关代理的连接查到的会是 Coast 自己，所以 QmlBridge 那边按 inboundUser 把它丢掉）。
    yaml = setScalar(yaml, "find-process-mode", "always");
    yaml = setNestedScalar(yaml, "tun", "enable", tunEnabled ? "true" : "false");
    yaml = ensureProxyServerNameserver(yaml);
    // DNS 劫持配套：开 mihomo 的 DNS 监听端口。透明网关把被劫持设备的 UDP :53 查询转投到这里，而不是
    // 原样中继到「设备配置的 DNS」——后者常是网关/路由器 IP，经用户态栈中继到它走不通，导致名字解析
    // 时断时通（现象：直连 IP 通、换公共 DNS 通、用路由器当 DNS 全超时）。转投后设备拿到 mihomo 的
    // fake-ip 结果，连 fake-ip 再经网关回到核心做国内外分流。端口须与 NetStack::kDnsHijackPort 一致(1053)。
    yaml = setNestedScalar(yaml, "dns", "listen", "127.0.0.1:1053");
    yaml = normalizeEmptyProxies(yaml);
    yaml = applySubscriptions(yaml, readSubscriptions());
    yaml = applyCustomRules(yaml);
    yaml = applyDevicePolicies(yaml);
    // ★ 必须排在 applyCustomRules / applyDevicePolicies **之后** —— 三者都是「前插到 rules: 顶部」，
    //   后插的在最上面。私网直连要压过 applyDevicePolicies 生成的 IN-USER 规则：policy=global 的设备
    //   否则会把「访问自己家路由器后台 / 内网 NAS」也发到代理节点上（LanGateway_linux.cpp 里旁路
    //   广播/组播那段注释描述的就是这个坑的另一半）。
    yaml = applyPrivateNetworkRules(yaml);
    yaml = applySniffer(yaml);
    yaml = applyProfilePersistence(yaml);

    const QString fullPath = QDir(m_config.configDir).filePath("full.yaml");
    writeText(fullPath, yaml);
    return fullPath;
}

bool ConfigBuilder::writeTunEnabled(const QString &filePath, bool enabled) const
{
    QString yaml = readText(filePath);
    if (yaml.isEmpty()) {
        return false;
    }
    const QString updated = setNestedScalar(yaml, "tun", "enable", enabled ? "true" : "false");
    if (updated == yaml) {
        return false;
    }
    writeText(filePath, updated);
    return true;
}

QString ConfigBuilder::readText(const QString &path) const
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

void ConfigBuilder::writeText(const QString &path, const QString &text) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(text.toUtf8());
    }
}

QString ConfigBuilder::mergePlugin(const QString &base, const QString &plugin) const
{
    if (plugin.trimmed().isEmpty()) {
        return base;
    }

    QString merged = base;
    const QStringList topLevelKeys = {"dns", "tun"};
    for (const QString &key : topLevelKeys) {
        const QRegularExpression pluginBlock(QStringLiteral("(?m)^%1:\\n(?:(?:  |\\t)[^\\n]*(?:\\n|$))+").arg(QRegularExpression::escape(key)));
        const QRegularExpressionMatch match = pluginBlock.match(plugin);
        if (!match.hasMatch()) {
            continue;
        }

        const QString block = match.captured(0).trimmed() + "\n";
        const QRegularExpression baseBlock(QStringLiteral("(?m)^%1:\\n(?:(?:  |\\t)[^\\n]*(?:\\n|$))+").arg(QRegularExpression::escape(key)));
        if (baseBlock.match(merged).hasMatch()) {
            merged.replace(baseBlock, block);
        } else {
            merged.append("\n").append(block);
        }
    }
    return merged;
}

QVector<ConfigBuilder::Subscription> ConfigBuilder::readSubscriptions() const
{
    const QString userPath = QDir(m_config.configDir).filePath("subscribe.yaml");
    const QString bundledPath = QStringLiteral(":/assets/bundle/config/subscribe.yaml"); // 内嵌种子
    if (!QFile::exists(userPath) && QFile::exists(bundledPath)) {
        QFile::copy(bundledPath, userPath);
        AppConfig::makeWritable(userPath);
    }

    const QString text = readText(QFile::exists(userPath) ? userPath : bundledPath);
    QVector<Subscription> subscriptions;
    if (text.trimmed().isEmpty() || text.trimmed() == "[]") {
        return subscriptions;
    }

    Subscription currentSub;
    SubscriptionNode currentNode;
    bool inSub = false;
    bool inList = false;
    bool inNode = false;

    auto finishNode = [&] {
        if (!inNode) {
            return;
        }
        if (!currentNode.name.isEmpty() && !currentNode.yaml.trimmed().isEmpty()) {
            currentSub.nodes.push_back(currentNode);
        }
        currentNode = {};
        inNode = false;
    };

    auto finishSub = [&] {
        finishNode();
        if (inSub && currentSub.use && !currentSub.nodes.isEmpty()) {
            subscriptions.push_back(currentSub);
        }
        currentSub = {};
        inSub = false;
        inList = false;
    };

    const QStringList lines = text.split('\n');
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
            currentSub = {};
            const QString first = line.mid(2).trimmed();
            if (first.startsWith("name:")) {
                currentSub.name = yamlScalar(first.mid(QString("name:").size()));
            }
            continue;
        }

        if (!inSub) {
            continue;
        }

        if (line.startsWith("  ") && !line.startsWith("    ")) {
            const QString field = line.mid(2).trimmed();
            if (field.startsWith("name:")) {
                currentSub.name = yamlScalar(field.mid(QString("name:").size()));
            } else if (field.startsWith("use:")) {
                currentSub.use = yamlScalar(field.mid(QString("use:").size())).toLower() == "true";
            } else if (field.startsWith("speedtest:")) {
                currentSub.speedtest = yamlScalar(field.mid(QString("speedtest:").size())).toLower() == "true";
            } else if (field == "list:" || field.startsWith("list:")) {
                inList = true;
            }
            continue;
        }

        if (!inList) {
            continue;
        }

        if (line.startsWith("    - ")) {
            finishNode();
            inNode = true;
            QString item = line.mid(6).trimmed();
            if (item.startsWith("name:")) {
                currentNode.name = yamlScalar(item.mid(QString("name:").size()));
                const QString suffix = currentSub.speedtest ? "[speedtest]" : "";
                const QString fullName = QString("%1 - %2%3").arg(currentNode.name, currentSub.name, suffix);
                currentNode.yaml += QString("  - name: %1\n").arg(yamlQuote(fullName));
            } else {
                currentNode.yaml += QString("  - %1\n").arg(item);
            }
            continue;
        }

        if (inNode && line.startsWith("      ")) {
            const QString item = line.mid(6).trimmed();
            if (item.startsWith("name:")) {
                currentNode.name = yamlScalar(item.mid(QString("name:").size()));
                continue;
            }
            if (item.startsWith("use:") && yamlScalar(item.mid(QString("use:").size())).toLower() != "true") {
                currentNode.yaml.clear();
                currentNode.name.clear();
                inNode = false;
                continue;
            }
            if (item.startsWith("use:") || item.startsWith("delay:") || item.startsWith("speed:") || item.startsWith("loading:")) {
                continue;
            }
            currentNode.yaml += QString("    %1\n").arg(item);
        }
    }

    finishSub();
    return subscriptions;
}

QString ConfigBuilder::applySubscriptions(QString yaml, const QVector<Subscription> &subscriptions) const
{
    QStringList allNames;
    QString proxyBlock = "proxies:";
    QSet<QString> usedNames;

    for (const Subscription &subscription : subscriptions) {
        for (const SubscriptionNode &node : subscription.nodes) {
            const QRegularExpression nameRe("(?m)^  - name:\\s*(.+)$");
            const QRegularExpressionMatch match = nameRe.match(node.yaml);
            if (!match.hasMatch()) {
                continue;
            }
            QString name = yamlScalar(match.captured(1));
            if (name.isEmpty() || usedNames.contains(name)) {
                continue;
            }
            usedNames.insert(name);
            allNames.push_back(name);
            QString nodeYaml = node.yaml;
            while (nodeYaml.endsWith('\n')) {
                nodeYaml.chop(1);
            }
            proxyBlock += "\n" + nodeYaml + "\n";
        }
    }

    if (allNames.isEmpty()) {
        return yaml;
    }

    yaml = replaceTopLevelProxies(yaml, proxyBlock + "\n");

    const qsizetype groups = yaml.indexOf("\nproxy-groups:");
    qsizetype firstProxies = groups >= 0 ? yaml.indexOf("\n    proxies:", groups) : -1;
    if (firstProxies >= 0) {
        QStringList firstGroupValues;
        const qsizetype listStart = yaml.indexOf('\n', firstProxies + 1) + 1;
        qsizetype cursor = listStart;
        while (cursor > 0 && cursor < yaml.size()) {
            const qsizetype nextEnd = yaml.indexOf('\n', cursor);
            const QString line = yaml.mid(cursor, nextEnd < 0 ? -1 : nextEnd - cursor);
            if (!line.startsWith("      - ")) {
                break;
            }
            firstGroupValues << yamlScalar(line.mid(8));
            cursor = nextEnd < 0 ? yaml.size() : nextEnd + 1;
        }
        for (const Subscription &subscription : subscriptions) {
            if (!subscription.nodes.isEmpty()) {
                firstGroupValues << QString("%1 订阅").arg(subscription.name);
            }
        }
        firstGroupValues.append(autoGroups(allNames).keys());
        firstGroupValues.append(allNames);
        firstGroupValues.removeDuplicates();
        yaml = replaceProxyListAt(yaml, firstProxies + 1, firstGroupValues);
    }

    firstProxies = groups >= 0 ? yaml.indexOf("\n    proxies:", groups) : -1;
    const qsizetype secondProxies = firstProxies >= 0 ? yaml.indexOf("\n    proxies:", firstProxies + 1) : -1;
    if (secondProxies >= 0) {
        yaml = replaceProxyListAt(yaml, secondProxies + 1, allNames);
    }

    return appendSubscriptionGroups(yaml, subscriptions);
}

QString ConfigBuilder::applyCustomRules(QString yaml) const
{
    // 消费设置页写入的 userDir/rules.json：
    //   area: [{name, type, rule}]  -> 生成按正则匹配节点名的自定义 proxy-group（对应旧项目 def['proxy-groups'] 中带 rule 的项）
    //   rule: [{type, node, value}] -> 前插到 rules: 顶部（对应旧项目 def.rules.unshift）
    const QString rulesPath = QDir(m_config.configDir).filePath("rules.json");
    QFile file(rulesPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return yaml;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();

    const QJsonArray areas = root.value("area").toArray();
    const QJsonArray customRules = root.value("rule").toArray();
    if (areas.isEmpty() && customRules.isEmpty()) {
        return yaml;
    }

    // 1) 自定义区域分组：按正则匹配节点名，生成 proxy-group，并把组名加入首个选择组
    const QStringList nodeNames = proxyNames(yaml);
    QSet<QString> existing;
    for (const QString &groupName : existingGroupNames(yaml)) {
        existing.insert(groupName);
    }

    QString groupBlock;
    QStringList newGroupNames;
    for (const QJsonValue &value : areas) {
        const QJsonObject obj = value.toObject();
        const QString name = obj.value("name").toString().trimmed();
        QString type = obj.value("type").toString().trimmed();
        const QString rule = obj.value("rule").toString().trimmed();
        if (name.isEmpty() || rule.isEmpty() || existing.contains(name)) {
            continue;
        }
        if (type.isEmpty()) {
            type = "url-test";
        }
        const QRegularExpression re(rule);
        if (!re.isValid()) {
            continue;
        }
        QStringList matched;
        for (const QString &node : nodeNames) {
            if (re.match(node).hasMatch()) {
                matched << node;
            }
        }
        matched.removeDuplicates();
        if (matched.isEmpty()) {
            continue;
        }
        existing.insert(name);
        newGroupNames << name;
        groupBlock += QString("  - name: %1\n").arg(yamlQuote(name));
        groupBlock += QString("    type: %1\n").arg(type);
        if (type != "select") {
            groupBlock += "    url: 'http://www.gstatic.com/generate_204'\n";
            groupBlock += "    interval: 300\n";
            // 不写 lazy（默认 true，对齐旧项目 clash.js）：lazy:false 会在启动时同步健康检查，
            // 节点不可达时会卡住核心启动、REST API 迟迟不监听。延迟改由应用启动后异步测速填充。
        }
        groupBlock += "    proxies:\n";
        for (const QString &node : matched) {
            groupBlock += QString("      - %1\n").arg(yamlQuote(node));
        }
    }

    // 仅当存在 rules: 锚点时才注入（避免只更新选择组却插不进组定义，产生悬空引用）
    if (!newGroupNames.isEmpty() && yaml.indexOf("\nrules:") >= 0) {
        yaml = addToFirstGroup(yaml, newGroupNames);
        const qsizetype rulesPos = yaml.indexOf("\nrules:");
        if (rulesPos >= 0) {
            yaml.insert(rulesPos + 1, groupBlock);
        }
    }

    // 2) 自定义路由规则：前插到 rules: 顶部
    QString ruleLines;
    for (const QJsonValue &value : customRules) {
        const QJsonObject obj = value.toObject();
        const QString type = obj.value("type").toString().trimmed();
        const QString node = obj.value("node").toString().trimmed();
        const QString val = obj.value("value").toString().trimmed();
        if (type.isEmpty() || node.isEmpty()) {
            continue;
        }
        const QString rule = (type == "MATCH")
            ? QString("%1,%2").arg(type, node)
            : QString("%1,%2,%3").arg(type, val, node);
        ruleLines += QString("  - %1\n").arg(yamlQuote(rule));
    }
    if (!ruleLines.isEmpty()) {
        const qsizetype rulesPos = yaml.indexOf("\nrules:");
        if (rulesPos >= 0) {
            const qsizetype lineEnd = yaml.indexOf('\n', rulesPos + 1);
            if (lineEnd >= 0) {
                yaml.insert(lineEnd + 1, ruleLines);
            }
        }
    }

    return yaml;
}

// 私网 / 回环 / 链路本地目的地一律 DIRECT，前插到 rules: 最顶。
//
// 为什么必须有这块（不是"顺手加条保险"）：
//   透明网关只旁路**同网段且非网关 IP**的帧（见 LanGateway_linux.cpp 的"同网段直连旁路"）。
//   发往网关 IP 本身的帧（路由器后台、UPnP、非 :53 的路由器服务）和发往**本机另一个网段**的帧
//   都会进 lwIP → SOCKS → 核心。而私网地址匹配不上 GEOIP,CN，于是一路落到 MATCH 兜底 →
//   **被发到境外节点**，必然超时。真机上就撞到过：default.yaml 种子里手写了
//   `IP-CIDR,192.168.20.0/24,DIRECT`，但这台机器同时还挂着 192.168.31.0/24 的 WLAN，
//   那个网段上的被代理设备访问自家路由器就是这么断的。
//
// 为什么生成在代码里而不是写进 default.yaml 种子：种子只在用户目录里不存在时复制一次
//（ensureFullConfig 开头那个 QFile::exists 判断），之后再改种子对已装机器完全无效 ——
// 与 applySniffer / applyProfilePersistence 同一个理由，别再往种子里加。
//
// 范围只取「按定义就不可能是互联网目的地」的段，宁缺毋滥：
//   · RFC1918 三段 + 回环 + 链路本地(169.254/16, APIPA)；v6 取 ULA(fc00::/7) 与链路本地(fe80::/10)。
//   · **不含 198.18.0.0/15** —— 那是核心 fake-ip 池所在段，直连它等于把所有走 fake-ip 的域名打死。
//   · 不含 100.64/10(CGNAT)：部分 ISP 真的用它承载上网，判不准就不判。
//   · 不含组播/广播：网关侧 L2 已经旁路掉了（bypassBcast），TUN 也不路由它们。
// 一律带 no-resolve：只想拦「目的地就是私网 IP 字面量」的连接，绝不为了比对而强行解析域名
//（开了 fake-ip 之后强行解析既慢又没意义）。
QString ConfigBuilder::applyPrivateNetworkRules(QString yaml) const
{
    static const char *const kNets[] = {
        "IP-CIDR,10.0.0.0/8,DIRECT,no-resolve",
        "IP-CIDR,172.16.0.0/12,DIRECT,no-resolve",
        "IP-CIDR,192.168.0.0/16,DIRECT,no-resolve",
        "IP-CIDR,127.0.0.0/8,DIRECT,no-resolve",
        "IP-CIDR,169.254.0.0/16,DIRECT,no-resolve",
        "IP-CIDR6,fc00::/7,DIRECT,no-resolve",
        "IP-CIDR6,fe80::/10,DIRECT,no-resolve",
    };

    const qsizetype rulesPos = yaml.indexOf("\nrules:");
    if (rulesPos < 0) {
        return yaml; // 没有 rules: 锚点就不注入（与 applyCustomRules 的取舍一致）
    }
    const qsizetype lineEnd = yaml.indexOf('\n', rulesPos + 1);
    if (lineEnd < 0) {
        return yaml;
    }

    QStringList lines;
    for (const char *rule : kNets) {
        lines << QString::fromLatin1(rule);
    }
    // IPv6 没有 RFC1918 那种「固定私网段」——家用 v6 内网用的就是运营商 RA 下发的**全局单播**
    // 前缀（如电信的 240e:…/64），换网络/换 ISP 就变，静态列表根本写不出来。上面那两条
    // fc00::/7(ULA) + fe80::/10(链路本地) 一条都盖不住它。所以这一段必须动态取。
    lines += localGlobal6Prefixes();

    QString block;
    for (const QString &line : lines) {
        // 幂等：种子/订阅里可能已经写死了同一条（真机 default.yaml 就有手写的 192.168.20.0/24）。
        // 完全相同的行不重复插；不同写法的重复无害——先命中的那条赢，目标策略都是 DIRECT。
        if (yaml.contains(line)) {
            continue;
        }
        block += QStringLiteral("  - %1\n").arg(yamlQuote(line));
    }
    if (!block.isEmpty()) {
        yaml.insert(lineEnd + 1, block);
    }
    return yaml;
}

// 本机各网卡上的 IPv6 **全局单播**前缀 → IP-CIDR6 DIRECT 规则行。
//
// 为什么只取 v6、v4 一律不动态探测：
//   v4 那三段 RFC1918 按定义就穷尽了所有合法私网，动态探测的结果只会是它们的子集（更窄），
//   补不出任何东西 —— 但会补出**致命的东西**：本机上 mihomo 的 TUN 网卡带着 198.18.0.1/30，
//   探测进来写成 DIRECT 就等于把 fake-ip 池直连，所有走 fake-ip 的域名当场全死。真机实测，
//   一台双网卡机器探到的 6 个 v4 网段里 5 个已被静态段覆盖、第 6 个正是那条 198.18.0.0/30。
//   v6 则相反：真实的内网前缀是 RA 给的全局地址，静态列表写不出来，只能探。
//
// 「只收全局单播(2000::/3)」这一条筛选顺带把所有坑一次排掉：
//   · fe80::/10 链路本地、fc00::/7 ULA —— 上面已有静态规则，不必重复；
//   · mihomo TUN 的 fdfe:dcba:9876::1/126 落在 fc00::/7 里 —— 自动被排除（对应 v4 的 198.18 陷阱）；
//   · ::1 / 组播 —— 都不在 2000::/3。
// 再加两道：只收前缀长度 48–64（RA 下发的就是 /64；/128 是主机地址和临时地址，收进来毫无意义
// 且会把规则表撑爆），以及要求网卡 IsUp && IsRunning && !IsLoopBack。
//
// ★ 时效性的坑，写在这里免得以后有人踩：full.yaml 只在设置/规则/订阅/设备变更时重建，
//   **换 Wi-Fi / DHCP 续租 / 插拔网线都不会重建**。所以这份前缀在你换网络那一刻就过期了
//   （旧前缀留在规则里无害——那个网段已经不存在；新前缀则要等下一次重建才进来）。
//   要根治得让网段变化也触发 rebuildConfig()，LanScanner 本来就在周期扫，比较一下集合即可。
QStringList ConfigBuilder::localGlobal6Prefixes()
{
    QStringList out;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const QNetworkInterface::InterfaceFlags f = iface.flags();
        if (!(f & QNetworkInterface::IsUp) || !(f & QNetworkInterface::IsRunning)
            || (f & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &e : iface.addressEntries()) {
            const QHostAddress ip = e.ip();
            if (ip.protocol() != QAbstractSocket::IPv6Protocol) {
                continue;
            }
            const int plen = e.prefixLength();
            if (plen < 48 || plen > 64) {
                continue; // /128 主机地址、以及短得离谱的前缀都不要
            }
            const Q_IPV6ADDR raw = ip.toIPv6Address();
            if ((raw[0] & 0xE0) != 0x20) {
                continue; // 只收 2000::/3 全局单播；其余（LL/ULA/组播/回环）见上面注释
            }
            // 掩到前缀边界：240e:…:bbc0:xxxx:xxxx:xxxx:xxxx/64 → 240e:…:bbc0::/64
            Q_IPV6ADDR net = raw;
            for (int bit = plen; bit < 128; ++bit) {
                net[bit / 8] &= uchar(~(1u << (7 - (bit % 8))));
            }
            const QString rule = QStringLiteral("IP-CIDR6,%1/%2,DIRECT,no-resolve")
                                     .arg(QHostAddress(net).toString())
                                     .arg(plen);
            if (!out.contains(rule)) { // 同一前缀常同时挂在多张卡/多个地址上
                out << rule;
            }
        }
    }
    return out;
}

QString ConfigBuilder::applyDevicePolicies(QString yaml) const
{
    // 消费设备台账（coast.db 的 device 表，由 DeviceStore 维护；早期版本是 configDir/devices.json，
    // 已自动迁移）：为每台开启「代理网络」的设备派生一个 mihomo SOCKS 用户名 dev-<去冒号小写 mac>，
    //   并据此生成两样东西：
    //     1) 一个专用的 socks inbound listener（coast-gateway，端口 kGatewayPort=7899），
    //        带 per-user users: 认证——被劫持设备的流量经此口带用户名进核心。**主混合口
    //        (7890) 仍保持免认证**，Coast 自己的测速走 7890 不受影响。
    //     2) 对非 follow/非 rule 的设备，前插 IN-USER 规则到 rules: 顶部，按用户名分流。
    //   mihomo schema 假设：listeners 的 per-user `users:` 与 `IN-USER` 规则类型均被用户当前
    //   mihomo 构建支持（用户已确认）——标准 clash 内核可能不支持，届时核心会在 -t 校验时报错。
    //
    // 这里读的是**库**而不是 DeviceStore 对象：ConfigBuilder 是 CoreController 的值成员（换端口
    // 时整个重建），塞一个 DeviceStore* 进来要一路改构造签名和生命周期；而它只需要「此刻谁开着
    // 代理」这一次性快照，DeviceStore::proxiedDevices 开条临时只读连接查完就关。
    // 时序上没有陈旧风险：设备页改开关时先 store->save()（同步提交）再 rebuildConfig()，
    // WAL 下已提交的事务对别的连接立刻可见。
    struct ProxyDevice {
        QString user;
        QString mode;   // follow / rule / global / direct / reject
        QString target; // global 模式的目标节点/组名
    };
    QVector<ProxyDevice> proxied;
    for (const DeviceStore::ProxyDeviceRow &row : DeviceStore::proxiedDevices(m_config.configDir)) {
        const QString user = DeviceStore::socksUser(row.mac);
        if (user.isEmpty()) {
            continue; // 非法 mac → socksUser 返回空，跳过（否则会写出坏用户名）
        }
        ProxyDevice dev;
        dev.user = user;
        dev.mode = row.policyMode;
        dev.target = row.policyTarget;
        proxied.push_back(dev);
    }

    if (proxied.isEmpty()) {
        return yaml; // 没有任何设备开代理 → 不生成 listener / 规则
    }

    // 1) 生成 coast-gateway listener（>=1 台代理设备时才发）。严格 2 空格 YAML 缩进：
    //    listeners:(0) -> - name:(2) -> 字段(4) -> users:(4) -> - username:(6) -> password:(8)
    QString block = "listeners:\n";
    block += "  - name: coast-gateway\n";
    block += "    type: socks\n";
    block += "    listen: 127.0.0.1\n";
    block += QString("    port: %1\n").arg(DeviceStore::kGatewayPort);
    block += "    users:\n";
    for (const ProxyDevice &dev : proxied) {
        block += QString("      - username: %1\n").arg(dev.user); // dev-<hex>，纯 ascii 无需引号
        block += "        password: coast\n"; // 密码固定字面量 coast，与 LanGateway 拨号一致
    }

    // 若已存在我们上轮生成的顶层 listeners: 块，整块替换（从 listeners: 到下一个顶层键为止，
    // 用与 mergePlugin 相同的「顶层键 + 后续缩进行」正则匹配）；否则追加到文件末尾。
    const QRegularExpression listenersBlock(
        QStringLiteral("(?m)^listeners:\\n(?:(?:  |\\t)[^\\n]*(?:\\n|$))+"));
    if (listenersBlock.match(yaml).hasMatch()) {
        yaml.replace(listenersBlock, block);
    } else {
        if (!yaml.endsWith('\n')) {
            yaml.append('\n');
        }
        yaml.append('\n').append(block); // 空行分隔，风格对齐 mergePlugin
    }

    // 2) 非 follow/非 rule 的设备 → 前插 IN-USER 规则到 rules: 顶部（follow/rule 走正常规则，不发）。
    //    前插方式对齐 applyCustomRules：定位 "\nrules:"，在该行行尾后插入规则块，使其优先命中。
    QString ruleLines;
    for (const ProxyDevice &dev : proxied) {
        QString target;
        if (dev.mode == "reject") {
            target = "REJECT";
        } else if (dev.mode == "direct") {
            target = "DIRECT";
        } else if (dev.mode == "global") {
            if (dev.target.isEmpty()) {
                continue; // global 但没指定目标 → 跳过这台，避免写出悬空引用
            }
            target = dev.target;
        } else {
            continue; // follow / rule（及未知值）不生成 IN-USER 规则
        }
        const QString rule = QString("IN-USER,%1,%2").arg(dev.user, target);
        ruleLines += QString("  - %1\n").arg(yamlQuote(rule));
    }
    if (!ruleLines.isEmpty()) {
        const qsizetype rulesPos = yaml.indexOf("\nrules:");
        if (rulesPos >= 0) {
            const qsizetype lineEnd = yaml.indexOf('\n', rulesPos + 1);
            if (lineEnd >= 0) {
                yaml.insert(lineEnd + 1, ruleLines);
            }
        }
    }

    return yaml;
}

// ———————————————————————— 域名嗅探（sniffer）————————————————————————
// 没有它，**纯 IP 发起的连接在 mihomo 眼里就没有域名**：metadata.host 为空，UI 只能退回显示
// destinationIP（用户看到的「连接里全是 IP」），更要命的是规则里绝大多数是域名规则
// （DOMAIN-SUFFIX/GEOSITE），host 为空时一条都匹配不上，只能靠 GEOIP/IP-CIDR 兜底分流。
//
// 谁会「拿着纯 IP 来」：
//   · 透明网关代理的局域网设备——它们自己做 DNS（我们只是把那个 UDP 中继出去，没走 fake-ip），
//     拿到真实 IP 后直连；我们的用户态栈也只认识 IP，拨 SOCKS 时给的就是 IP:port。**主因**。
//   · 本机上不走系统代理域名、直接连 IP 的程序。
// sniffer 从 TLS ClientHello 的 SNI / HTTP 请求头的 Host 里把域名读回来，两个问题一起修。
//
// **生成在代码里而不是放进 plugin.yaml 种子**：plugin.yaml 首次运行就复制到用户目录了，之后
// 再改种子对老用户无效（和 listeners: 块同理），所以每次生成 full.yaml 都整块重写一次。
QString ConfigBuilder::applySniffer(QString yaml) const
{
    QString block = "sniffer:\n";
    block += "  enable: true\n";
    // 关键项：默认只对「已有域名」的连接做校正，纯 IP 连接压根不嗅探——而我们要修的正是纯 IP。
    block += "  parse-pure-ip: true\n";
    // 用嗅探出的域名覆盖连接目标，规则匹配才拿得到域名（否则只修了显示、没修分流）。
    block += "  override-destination: true\n";
    block += "  sniff:\n";
    block += "    HTTP:\n";
    block += "      ports: [80, 8080-8880]\n";
    block += "    TLS:\n";
    block += "      ports: [443, 8443]\n";
    block += "    QUIC:\n";
    block += "      ports: [443, 8443]\n";
    // 已知会被嗅探搞坏的域名：小米/米家设备的云通道用自签证书 + 非常规 SNI，覆盖目标后连不上。
    block += "  skip-domain:\n";
    block += "    - Mijia Cloud\n";
    block += "    - dlg.io.mi.com\n";

    // 整块替换上一轮生成的（同 listeners: 的做法：顶层键 + 其后所有缩进行）；没有就追加。
    const QRegularExpression snifferBlock(
        QStringLiteral("(?m)^sniffer:\\n(?:(?:  |\\t)[^\\n]*(?:\\n|$))+"));
    if (snifferBlock.match(yaml).hasMatch()) {
        yaml.replace(snifferBlock, block);
    } else {
        if (!yaml.endsWith('\n')) {
            yaml.append('\n');
        }
        yaml.append('\n').append(block);
    }
    return yaml;
}

// 顶层 profile:——让核心把「fake-ip↔域名」映射和「各选择组当前选中节点」持久化到工作目录 cache
//（-d userDir 下的 cache），扛住 coast 每次改设置/规则/订阅/设备开关触发的热重载(PUT /configs)：
//   · 无 store-fake-ip：重载即清空 fake-ip 表 → 设备 DNS 缓存里的旧 fake-ip(198.18.x) 成孤儿 → 核心
//     认不出 → 解析不出目标、也无法按域名分流 → 「找不到 ip 无法走代理」（真机复现：重载前该 fake-ip
//     访问 200，重载后同一个 fake-ip 变 000，要等设备 DNS 缓存过期重解析才恢复）。
//   · 无 store-selected：重载把每个选择组重置回配置默认 → 用户手选的节点被冲掉（「设备/本机不走我
//     设的节点」）。
// 生成在代码里而非放进 default.yaml 种子：种子首次复制到用户目录后再改无效（同 sniffer/listeners），
// 所以每次生成 full.yaml 都整块重写一次。种子里那个 cfw-conn-break-strategy.profile 是 2 空格缩进的
// 嵌套键，与这里的顶层 profile: 不是一回事，^profile: 锚点不会误匹配它。
QString ConfigBuilder::applyProfilePersistence(QString yaml) const
{
    QString block = "profile:\n";
    block += "  store-selected: true\n";
    block += "  store-fake-ip: true\n";
    const QRegularExpression profileBlock(
        QStringLiteral("(?m)^profile:\\n(?:(?:  |\\t)[^\\n]*(?:\\n|$))+"));
    if (profileBlock.match(yaml).hasMatch()) {
        yaml.replace(profileBlock, block);
    } else {
        if (!yaml.endsWith('\n')) {
            yaml.append('\n');
        }
        yaml.append('\n').append(block);
    }
    return yaml;
}

QStringList ConfigBuilder::proxyNames(const QString &yaml)
{
    QStringList names;
    const qsizetype start = yaml.indexOf(QRegularExpression("(?m)^proxies:"));
    if (start < 0) {
        return names;
    }
    qsizetype end = yaml.indexOf("\nproxy-groups:", start);
    if (end < 0) {
        end = yaml.size();
    }
    const QString block = yaml.mid(start, end - start);
    const QRegularExpression nameRe("(?m)^  - name:\\s*(.+)$");
    QRegularExpressionMatchIterator it = nameRe.globalMatch(block);
    while (it.hasNext()) {
        const QString name = yamlScalar(it.next().captured(1));
        if (!name.isEmpty()) {
            names << name;
        }
    }
    return names;
}

QStringList ConfigBuilder::existingGroupNames(const QString &yaml)
{
    QStringList names;
    const qsizetype start = yaml.indexOf("\nproxy-groups:");
    if (start < 0) {
        return names;
    }
    qsizetype end = yaml.indexOf("\nrules:", start);
    if (end < 0) {
        end = yaml.size();
    }
    const QString block = yaml.mid(start, end - start);
    const QRegularExpression nameRe("(?m)^  - name:\\s*(.+)$");
    QRegularExpressionMatchIterator it = nameRe.globalMatch(block);
    while (it.hasNext()) {
        const QString name = yamlScalar(it.next().captured(1));
        if (!name.isEmpty()) {
            names << name;
        }
    }
    return names;
}

QString ConfigBuilder::addToFirstGroup(QString yaml, const QStringList &names) const
{
    if (names.isEmpty()) {
        return yaml;
    }
    const qsizetype groups = yaml.indexOf("\nproxy-groups:");
    if (groups < 0) {
        return yaml;
    }
    const qsizetype firstProxies = yaml.indexOf("\n    proxies:", groups);
    if (firstProxies < 0) {
        return yaml;
    }

    QStringList values;
    const qsizetype listStart = yaml.indexOf('\n', firstProxies + 1) + 1;
    qsizetype cursor = listStart;
    while (cursor > 0 && cursor < yaml.size()) {
        const qsizetype nextEnd = yaml.indexOf('\n', cursor);
        const QString line = yaml.mid(cursor, nextEnd < 0 ? -1 : nextEnd - cursor);
        if (!line.startsWith("      - ")) {
            break;
        }
        values << yamlScalar(line.mid(8));
        cursor = nextEnd < 0 ? yaml.size() : nextEnd + 1;
    }
    for (const QString &name : names) {
        if (!values.contains(name)) {
            values << name;
        }
    }
    return replaceProxyListAt(yaml, firstProxies + 1, values);
}

QString ConfigBuilder::replaceTopLevelProxies(QString yaml, const QString &proxyBlock) const
{
    const qsizetype start = yaml.indexOf(QRegularExpression("(?m)^proxies:"));
    const qsizetype end = yaml.indexOf("\nproxy-groups:", start);
    if (start < 0 || end < 0) {
        return yaml;
    }
    yaml.replace(start, end - start + 1, proxyBlock);
    return yaml;
}

QString ConfigBuilder::replaceProxyListAt(QString yaml, qsizetype proxiesKey, const QStringList &values) const
{
    const qsizetype lineEnd = yaml.indexOf('\n', proxiesKey);
    if (lineEnd < 0) {
        return yaml;
    }
    qsizetype end = lineEnd + 1;
    while (end < yaml.size()) {
        const qsizetype nextEnd = yaml.indexOf('\n', end);
        const QString line = yaml.mid(end, nextEnd < 0 ? -1 : nextEnd - end);
        if (!line.startsWith("      - ")) {
            break;
        }
        end = nextEnd < 0 ? yaml.size() : nextEnd + 1;
    }

    QString replacement = "    proxies:\n";
    for (const QString &value : values) {
        replacement += QString("      - %1\n").arg(yamlQuote(value));
    }
    yaml.replace(proxiesKey, end - proxiesKey, replacement);
    return yaml;
}

QString ConfigBuilder::appendSubscriptionGroups(QString yaml, const QVector<Subscription> &subscriptions) const
{
    const qsizetype rules = yaml.indexOf("\nrules:");
    if (rules < 0) {
        return yaml;
    }

    QString groups;
    for (const Subscription &subscription : subscriptions) {
        if (subscription.nodes.isEmpty()) {
            continue;
        }
        groups += QString("  - name: %1\n").arg(yamlQuote(QString("%1 订阅").arg(subscription.name)));
        groups += "    type: url-test\n";
        groups += "    url: 'http://www.gstatic.com/generate_204'\n";
        groups += "    interval: 300\n";
        // 不写 lazy（默认 true，对齐旧项目）：避免启动时同步健康检查卡住核心，延迟改由应用异步测速填充。
        groups += "    proxies:\n";
        for (const SubscriptionNode &node : subscription.nodes) {
            const QRegularExpression nameRe("(?m)^  - name:\\s*(.+)$");
            const QRegularExpressionMatch match = nameRe.match(node.yaml);
            if (match.hasMatch()) {
                groups += QString("      - %1\n").arg(yamlQuote(yamlScalar(match.captured(1))));
            }
        }
    }

    QStringList nodeNames;
    for (const Subscription &subscription : subscriptions) {
        for (const SubscriptionNode &node : subscription.nodes) {
            const QRegularExpression nameRe("(?m)^  - name:\\s*(.+)$");
            const QRegularExpressionMatch match = nameRe.match(node.yaml);
            if (match.hasMatch()) {
                nodeNames << yamlScalar(match.captured(1));
            }
        }
    }
    nodeNames.removeDuplicates();
    const QMap<QString, QStringList> grouped = autoGroups(nodeNames);
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        groups += QString("  - name: %1\n").arg(yamlQuote(it.key()));
        groups += "    type: url-test\n";
        groups += "    url: 'http://www.gstatic.com/generate_204'\n";
        groups += "    interval: 300\n";
        // 不写 lazy（默认 true，对齐旧项目）：避免启动时同步健康检查卡住核心，延迟改由应用异步测速填充。
        groups += "    proxies:\n";
        for (const QString &nodeName : it.value()) {
            groups += QString("      - %1\n").arg(yamlQuote(nodeName));
        }
    }

    yaml.insert(rules + 1, groups);
    return yaml;
}

QString ConfigBuilder::setScalar(QString yaml, const QString &key, const QString &value) const
{
    const QRegularExpression re(QStringLiteral("(?m)^%1:\\s*.*$").arg(QRegularExpression::escape(key)));
    if (re.match(yaml).hasMatch()) {
        yaml.replace(re, QString("%1: %2").arg(key, value));
    } else {
        yaml.prepend(QString("%1: %2\n").arg(key, value));
    }
    return yaml;
}

QString ConfigBuilder::setNestedScalar(QString yaml, const QString &section, const QString &key, const QString &value) const
{
    const QRegularExpression sectionRe(QStringLiteral("(?m)^%1:\\n((?:(?:  |\\t)[^\\n]*(?:\\n|$))*)").arg(QRegularExpression::escape(section)));
    const QRegularExpressionMatch match = sectionRe.match(yaml);
    if (!match.hasMatch()) {
        yaml.append(QString("\n%1:\n  %2: %3\n").arg(section, key, value));
        return yaml;
    }

    QString block = match.captured(0);
    const QRegularExpression nestedRe(QStringLiteral("(?m)^  %1:\\s*.*$").arg(QRegularExpression::escape(key)));
    if (nestedRe.match(block).hasMatch()) {
        block.replace(nestedRe, QString("  %1: %2").arg(key, value));
    } else {
        block.append(QString("  %1: %2\n").arg(key, value));
    }
    yaml.replace(match.capturedStart(0), match.capturedLength(0), block);
    return yaml;
}

QString ConfigBuilder::ensureProxyServerNameserver(QString yaml) const
{
    // 开启增强(TUN, auto-route) 后，核心要先解析「代理服务器域名」才能拨代理，若走 dns.fallback 的境外
    // DoH，这条 DoH 出站连接会被核心自己的 TUN 路由捕获 → 命中规则丢回代理 → 拨代理又得先解析代理服务器
    // 域名 → 死循环，日志报 "dns resolve failed: couldn't find ip"，表现为「所有节点无延迟、境外全打不开」。
    // 试过改用「IP 字面量境外 DoH + IP-CIDR 直连跳过」，但那些 DoH 在墙内直连本身不稳，解析照样超时失败。
    // 稳妥方案（回到境内 DNS）：proxy-server-nameserver 用境内明文 DNS 直连解析代理服务器域名——境内 DNS
    // 恒可直连、应答快，代理服务器域名通常未被污染，能拿到可用 IP 打破环路。TUN 关时也无副作用。
    if (yaml.contains(QRegularExpression("(?m)^  proxy-server-nameserver:"))) {
        return yaml; // 已有（自定义配置里带了）就不再注入
    }
    const QRegularExpressionMatch dnsHead = QRegularExpression("(?m)^dns:\\n").match(yaml);
    if (!dnsHead.hasMatch()) {
        return yaml; // 没有 dns 块（理论上不会）：不动，避免破坏结构
    }
    const QString block = "  proxy-server-nameserver:\n    - 223.5.5.5\n    - 119.29.29.29\n";
    yaml.insert(dnsHead.capturedEnd(0), block);
    return yaml;
}

QString ConfigBuilder::normalizeEmptyProxies(QString yaml) const
{
    yaml.replace(QRegularExpression("(?m)^proxies:\\s*null\\s*$"), "proxies: []");
    yaml.replace(QRegularExpression("(?m)^    proxies:\\s*null\\s*$"), "    proxies:\n      - DIRECT");
    return yaml;
}

QString ConfigBuilder::yamlQuote(const QString &value) const
{
    if (value.isEmpty()) {
        return "''";
    }
    if (value.contains("\\U") || value.contains("\\u")) {
        QString escaped = value;
        escaped.replace("\\", "\\\\");
        escaped.replace("\"", "\\\"");
        escaped.replace("\\\\U", "\\U");
        escaped.replace("\\\\u", "\\u");
        return QString("\"%1\"").arg(escaped);
    }
    QString escaped = value;
    escaped.replace("'", "''");
    return QString("'%1'").arg(escaped);
}

QString ConfigBuilder::yamlScalar(const QString &line)
{
    QString value = line.trimmed();
    const qsizetype comment = value.indexOf(" #");
    if (comment >= 0) {
        value = value.left(comment).trimmed();
    }
    if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith('\'') && value.endsWith('\''))) {
        value = value.mid(1, value.size() - 2);
    }
    value.replace("''", "'");
    return value.trimmed();
}

QMap<QString, QStringList> ConfigBuilder::autoGroups(const QStringList &nodeNames) const
{
    QMap<QString, QStringList> groups;
    const QVector<QPair<QString, QRegularExpression>> patterns = {
        {"Auto - HK", QRegularExpression("(香港|港|HK|Hong\\s*Kong)", QRegularExpression::CaseInsensitiveOption)},
        {"Auto - TW", QRegularExpression("(台湾|台|TW|Taiwan)", QRegularExpression::CaseInsensitiveOption)},
        {"Auto - JP", QRegularExpression("(日本|日|JP|Japan|Tokyo|Osaka)", QRegularExpression::CaseInsensitiveOption)},
        {"Auto - SG", QRegularExpression("(新加坡|狮城|SG|Singapore)", QRegularExpression::CaseInsensitiveOption)},
        {"Auto - US", QRegularExpression("(美国|美|US|USA|United\\s*States|Los\\s*Angeles|LA)", QRegularExpression::CaseInsensitiveOption)},
        {"Auto - KR", QRegularExpression("(韩国|韩|KR|Korea|Seoul)", QRegularExpression::CaseInsensitiveOption)},
        {"Auto - Netflix", QRegularExpression("(nf|netflix|奈飞|奈飛)", QRegularExpression::CaseInsensitiveOption)}
    };

    for (const QString &nodeName : nodeNames) {
        for (const auto &pattern : patterns) {
            if (pattern.second.match(nodeName).hasMatch()) {
                groups[pattern.first].push_back(nodeName);
            }
        }
    }

    for (auto it = groups.begin(); it != groups.end();) {
        it.value().removeDuplicates();
        if (it.value().isEmpty()) {
            it = groups.erase(it);
        } else {
            ++it;
        }
    }
    return groups;
}
