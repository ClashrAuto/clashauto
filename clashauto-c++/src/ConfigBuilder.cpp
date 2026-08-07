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
#include <QTemporaryDir> // COAST_NICEGRESS_SELFTEST 的临时 configDir
#include <QTextStream>

#include <cstdio> // 自测直接打 stdout（与其余 COAST_*_SELFTEST 的惯例一致）

ConfigBuilder::ConfigBuilder(AppConfig config) : m_config(std::move(config))
{
}

QString ConfigBuilder::ensureFullConfig(bool tunEnabled, bool ipv6Enabled)
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
    yaml = setScalar(yaml, "mixed-port", QString::number(m_config.mixedPort));
    yaml = setScalar(yaml, "external-controller", QString("'%1:%2'").arg(m_config.host).arg(m_config.uiPort));
    // 安全加固：给 REST API 设访问密钥（secret 键 default.yaml 通常没有，setScalar 缺失时会追加/前置）
    if (!m_config.secret.isEmpty()) {
        yaml = setScalar(yaml, "secret", QString("'%1'").arg(m_config.secret));
    }
    // 安全加固：默认关闭 allow-lan，混合端口只监听本机，避免暴露成开放代理
    yaml = setScalar(yaml, "allow-lan", "false");
    // 进程归属：让核心把发起连接的进程名填进 metadata.process（连接窗口要显示它）。
    // always 而不是 strict —— strict 由核心自行决定要不要查（没有 PROCESS-NAME 规则时它就不查了，
    // 于是 UI 永远拿不到进程名）。真机实测证实了这一点：strict 下本机连接的 process 同样是空的。
    //
    // ★★ **但它很贵，所以只在"当网关"之外才开。** 真机实测（树莓派网关，同两端、只改这一项、
    //    交替重复两轮，每轮 60 次取分位）：
    //
    //        find-process-mode   min    p50    p90    p99
    //        always             5.5ms  7.4ms  11.0ms 11.4ms
    //        off / strict       1.3ms  1.7ms   2.3ms  2.5ms
    //
    //    直连（完全不经代理）的基线是 p50 0.97ms —— 也就是说代理这一跳本身只值 ~0.8ms，
    //    而 always **每条新连接多花约 5.6ms**。
    //
    //    ★ 这 5.6ms 花在哪：**inode → 进程那一步的 /proc 全扫**，不是 netlink。
    //      核心查进程分两步：① netlink inet_diag 由四元组拿到 socket 的 uid/inode；
    //      ② 拿 inode 去 `/proc/<pid>/fd/*` 里 readlink 匹配 `socket:[inode]`。
    //      第 ① 步 fork 已经优化过（82d8c4f0，精确查询 0.022ms 取代全表 dump 5.3ms），
    //      第 ② 步照旧是 os.ReadDir("/proc") 逐 PID 逐 fd 扫。真机 strace 计数（25 次代理请求）：
    //          readlinkat 21202（每连接 848）  getdents64 7575（303）  newfstatat 5151（206）
    //          openat 3800（152）  close 3900（156）   —— 每连接约 1565 次 /proc 相关系统调用
    //          而 netlink 侧 sendto 只有 143 次（每连接 5.7）
    //      结论很清楚：代价在 ②。（我一度据 fork 那条注释把它改口成"netlink dump"，错了，
    //      按 strace 计数改回来。）
    //
    //    ★★ **但这 5.6ms 只落在「本机发起」的连接上，被代理设备走 TPROXY 那条路量不到。**
    //      设备的 socket 在设备自己机器上，网关本机 netlink 查不到就直接返回错误，
    //      **根本走不到第 ② 步的 /proc 扫描** —— 代价最大的那一步被短路了。
    //      真机实测（239 经网关取外网目标，40 次取分位，always/strict 交替两轮）：
    //          always  p50 29.0 / 28.6 ms      strict  p50 28.9 / 29.3 ms   —— 无可测差异
    //      （量设备侧必须用 time_starttransfer：TPROXY 下 TCP 握手由核心在本地先完成，
    //        time_connect 恒为 ~0.3ms，量的是假连通，看不出任何后端代价。）
    //      所以这条开关的真实收益是**网关本机自身发起的连接**（App 自己的请求、机器上跑的其它
    //      程序经混合口出网），不是设备流量。前面那句「每条新连接白花 5.6ms」对设备路径不成立，
    //      是我先在网关本机上量、又照着推到设备路径的过度外推，按实测改回。
    //      取舍因此变成：在当网关的机器上，用「连接窗口对本机连接显示进程名」换本机连接
    //      每条 5.6ms —— 这台机器的角色是路由器，换得值。
    //
    //    ★ 顺带记下另一处（与延迟无关，但排查时会撞上）：TProxy 入站的 metadata.InIP 是
    //      **监听器的通配地址**（实测 /connections 里 inboundIP='::'、inboundPort=7898；
    //      listener/tproxy/tproxy.go 显式 `WithInAddr(l.listener.Addr())`，因为 TProxy 下
    //      conn.LocalAddr() 是原始目的地）。于是第 ① 步的精确查询判据
    //      `InIP.Is4()==SrcIP.Is4()` 对 IPv4 客户端不成立，netlink 会退回全表 dump。
    //      本机实测这条 dump 并不贵（设备路径没有可测差异），但换台 ehash 更大的机器可能就贵了。
    //    偏偏这只对**本机发起**的连接有意义：局域网设备的进程跑在别人机器上，本机表里查不到，
    //    网关代理的连接查到的会是 Coast 自己 —— QmlBridge 那边按 inboundUser=dev-* 主动丢弃。
    //    **也就是说当网关时，我们花 5.6ms 算出一个随后被扔掉的值。**
    //
    //    所以按角色分：有设备开着代理（= 在当网关，连接率高、延迟敏感、值又用不上）→ strict；
    //    纯桌面客户端（连接窗口的进程名是真能看到的功能）→ 维持 always。
    //    用 strict 而不是 off：语义上"必要时才查"更贴切，实测延迟与 off 无差别，且将来若加
    //    PROCESS-NAME 规则它还能自动生效。
    const bool actingAsGateway = !DeviceStore::proxiedDevices(m_config.configDir).isEmpty();
    yaml = setScalar(yaml, "find-process-mode", actingAsGateway ? "strict" : "always");
    yaml = setNestedScalar(yaml, "tun", "enable", tunEnabled ? "true" : "false");
    yaml = pruneUnreachableDns(yaml); // 必须在 ensureProxyServerNameserver 之前：剔完才知道还剩几条
    yaml = ensureProxyServerNameserver(yaml);
    // DNS 劫持配套：开 mihomo 的 DNS 监听端口。透明网关把被劫持设备的 UDP :53 查询转投到这里，而不是
    // 原样中继到「设备配置的 DNS」——后者常是网关/路由器 IP，经用户态栈中继到它走不通，导致名字解析
    // 时断时通（现象：直连 IP 通、换公共 DNS 通、用路由器当 DNS 全超时）。转投后设备拿到 mihomo 的
    // fake-ip 结果，连 fake-ip 再经网关回到核心做国内外分流。端口须与 NetStack::kDnsHijackPort 一致(1053)。
    yaml = setNestedScalar(yaml, "dns", "listen", "127.0.0.1:1053");
    yaml = applyIpv6(yaml, ipv6Enabled);
    yaml = normalizeEmptyProxies(yaml);
    yaml = applySubscriptions(yaml, readSubscriptions());
    yaml = applyCustomRules(yaml);
    yaml = applyDevicePolicies(yaml);
    // ★ 必须排在 applyCustomRules / applyDevicePolicies **之后** —— 三者都是「前插到 rules: 顶部」，
    //   后插的在最上面。私网直连要压过 applyDevicePolicies 生成的 IN-USER 规则：policy=global 的设备
    //   否则会把「访问自己家路由器后台 / 内网 NAS」也发到代理节点上（LanGateway_linux.cpp 里旁路
    //   广播/组播那段注释描述的就是这个坑的另一半）。
    yaml = applyPrivateNetworkRules(yaml);
    // ★ 再压一层：「禁网」要连内网一起禁，所以必须排在私网直连之后（后插的在最上面）。
    //   与 global/direct 分开两趟的理由见 applyRejectDevices。
    yaml = applyRejectDevices(yaml);
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
        // 无任何订阅节点。默认（noNodeReject=false）维持 clash 既定语义：原样返回，"该走代理"
        // 的流量最终经「节点选择→自动选择」回落 DIRECT —— 即被劫持设备静默直连（真机实测出口
        // IP = 网关本地公网 IP）。这是用户 2026-08-03 明确要保留的默认（见 AppConfig.h）。
        //
        // fail-closed（noNodeReject=true）：把第一个组「🚀 节点选择」的成员改成 [REJECT]。
        //   · 它是 proxy-groups 的第一个组，也正是有节点时下面 300+ 行往里填节点列表的同一个组；
        //   · 它是 **select** 组，填 REJECT 后直接选 REJECT，不像 url-test「♻️ 自动选择」那样
        //     会对 REJECT 做健康检查而行为不定 —— 所以改这个组而不是自动选择组；
        //   · 所有"该走代理"的路径最终都经它：直接 →节点选择 的 580 条规则、以及 MATCH→漏网之鱼
        //     (默认首选=节点选择)。一处改动全覆盖。
        //   · **不碰「🎯 全球直连」组**（仍是 DIRECT）：803 条 CN/私网 →全球直连 的规则照常直连，
        //     局域网互访、国内站点不受影响。reject 设备指内置 REJECT，也与此无关。
        // 范围：只覆盖"订阅完全无节点"这个静态可判、也最常见的场景（机场没配/跑路/订阅过期解析空）。
        //   "有节点但运行时全部测速失败回落 DIRECT" 是运行时状态，静态配置抓不到，本项不拦。
        if (m_config.noNodeReject) {
            const qsizetype g = yaml.indexOf("\nproxy-groups:");
            const qsizetype fp = g >= 0 ? yaml.indexOf("\n    proxies:", g) : -1;
            if (fp >= 0)
                yaml = replaceProxyListAt(yaml, fp + 1, {QStringLiteral("REJECT")});
        }
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
            // ★ 60 而不是 300：这个间隔决定了「节点恢复之后，分组还要多久才认它」。
            // 2026-08-07 树莓派实测（同一台真实服务端 docker compose restart，等 healthy 后才计数）：
            //   interval 300 → 恢复要先失败 20 次；interval 60 → 0 次；
            //   把 MATCH 直接指向节点、绕开所有分组 → 也是 0 次。
            // 也就是说底层（tide 会话）早就恢复了，用户却还在失败——卡住的是 url-test 分组里
            // 那份过期的健康状态。30 也测过是 0，但 60 已经够，健康检查流量只有 30 的一半；
            // 且 lazy 默认 true，分组没被用到时根本不测，代价有限。
            groupBlock += "    interval: 60\n";
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
/// 「禁网」设备的 REJECT 规则。**单独一趟、排在 applyPrivateNetworkRules 之后**，
/// 这样它落在 rules: 最顶、压过私网直连。
///
/// ★ 为什么不能和 global/direct 一起在 applyDevicePolicies 里发：那一趟在私网规则**之前**，
///   于是私网直连会盖在禁网之上 —— 真机实测（Pi 网关、设备设为 reject）：访问公网被拦
///   （http_code=000）✓，但访问 10.99.0.2 **照样通** ✗。也就是被禁的设备仍能访问用户内网的
///   NAS、摄像头、路由器后台、其它设备。而「禁网」的典型使用场景恰恰是「这台不可信」，
///   把内网整个敞开与用户预期相反。
///   私网规则压过 IN-USER 这个顺序**本身是对的**，但它的理由（见 ensureFullConfig 里的注释）
///   只针对 `policy=global`：不该把「访问自家路由器后台」发到代理节点上。那条理由对
///   `reject` 不成立 —— 禁网就是要连内网也一起禁。所以按模式分开两趟，各归各位。
///
/// ★ **仍有一块管不到**：与设备**同网段**的流量在二层就被旁路了（LanGateway 的 bypassLan），
///   根本不进核心，任何规则都拦不住它。所以这条修复覆盖的是「其它私网段」（10/8、172.16/12
///   以及别的子网），同子网内的互访要拦得换机制（例如不旁路、或在二层直接丢）。
///   这一点必须说清楚，免得以为「禁网」已经是密不透风的。
QString ConfigBuilder::applyRejectDevices(QString yaml) const
{
    QString ruleLines;
    for (const DeviceStore::ProxyDeviceRow &row : DeviceStore::proxiedDevices(m_config.configDir)) {
        if (row.policyMode != QLatin1String("reject"))
            continue;
        const QString user = DeviceStore::socksUser(row.mac);
        if (user.isEmpty())
            continue; // 非法 mac：与 applyDevicePolicies 同样跳过，别写出坏用户名
        // 身份载体按数据面分，理由同 applyDevicePolicies。**这一条尤其不能漏**：
        // 漏了就是「用户把设备设成禁网、它却照常上网」——比策略不生效更糟，是安全预期被违背。
        if (m_config.gatewayTproxy) {
            if (row.ip.isEmpty())
                continue; // 台账还没记到 IP，下一轮扫描到就有了
            ruleLines += QStringLiteral("  - %1\n")
                             .arg(yamlQuote(QStringLiteral("SRC-IP-CIDR,%1/32,REJECT").arg(row.ip)));
        } else {
            ruleLines += QStringLiteral("  - %1\n")
                             .arg(yamlQuote(QStringLiteral("IN-USER,%1,REJECT").arg(user)));
        }
    }
    if (ruleLines.isEmpty())
        return yaml;
    const qsizetype rulesPos = yaml.indexOf("\nrules:");
    if (rulesPos < 0)
        return yaml;
    const qsizetype lineEnd = yaml.indexOf('\n', rulesPos + 1);
    if (lineEnd < 0)
        return yaml;
    yaml.insert(lineEnd + 1, ruleLines);
    return yaml;
}

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
        QString ip;     // TPROXY 模式的身份载体（见下面 policyMatcher）
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
        dev.ip = row.ip;
        proxied.push_back(dev);
    }

    if (proxied.isEmpty()) {
        return yaml; // 没有任何设备开代理 → 不生成 listener / 规则
    }

    // 1) —— 每张网卡一个 socks 入站 ——
    //
    // ★ 这是「设备从它自己那条上行出去」的**全部**实现。核心的 listener 支持 `interface-name`
    //   （见 core 的 component/dialer/egress.go）：从这个入站进来的每一条连接，无论最终走
    //   DIRECT 还是走某个节点，都从指定的那张网卡出去。
    //
    //   为什么必须落在**入站**上：出口网卡是「连接从哪来」的属性，而核心的配置语言只能把网卡绑
    //   在**出站对象**上（proxy 的 interface-name / dialer-proxy；策略组两者都不接，本仓库也没有
    //   relay 组类型）。用出站表达就得把 身份 × 出站 做笛卡尔积 —— 每张卡复制一份节点表与策略组，
    //   健康检查跟着翻倍。上一版走的正是那条路，而且真机上还发现它**只能改写 DIRECT**，可真实
    //   订阅里公网目的地一条 DIRECT 都没有（全指向策略组），于是整套完全惰性。
    //
    // ★ 老核心**忽略**未知的 listener 键（实测 `core -t` rc=0），所以这个键可以无条件发：
    //   新核心生效、老核心退回今天的行为。不需要版本门。
    //
    // 端口 = kGatewayPort + 网卡下标（见 DeviceStore::gatewayPortFor）；LanGateway 按设备所在
    // 网卡拨对应端口。只给「确实有代理设备」的网卡发 listener，设备按网卡分进各自的 users。
    // 严格 2 空格 YAML 缩进：listeners:(0) -> - name:(2) -> 字段(4) -> users:(4) ->
    //                        - username:(6) -> password:(8)
    QMap<int, QVector<const ProxyDevice *>> byNic;
    for (const ProxyDevice &dev : proxied) {
        const int idx = egressNicFor(dev.ip);
        // 判不出网卡（不在任何已知网段 / 还没配过 NicEgress）→ 归 0 号，即今天的行为。
        byNic[idx >= 0 ? idx : 0].append(&dev);
    }
    QString block = "listeners:\n";
    for (auto it = byNic.constBegin(); it != byNic.constEnd(); ++it) {
        const int idx = it.key();
        block += QString("  - name: coast-gw-%1\n").arg(idx);
        block += "    type: socks\n";
        block += "    listen: 127.0.0.1\n";
        block += QString("    port: %1\n").arg(DeviceStore::gatewayPortFor(idx));
        // 发这个键的唯一前提：友好名拿得到。核心的 iface.ResolveInterface 是**精确查找**，
        // 名字对不上会让这个入站的每一次拨号都失败且没有回退 —— 名字没拿到时宁可不写。
        // ★ **不分单卡多卡，一律发**：一张卡就是长度 1 的数组，没有「单网卡模式」可回退。
        //   主卡也钉死还顺带治了「Windows 自动跃点一变默认路由就换卡、在途连接成批断」那个
        //   抖动 —— 那是"不稳定"的另一半。拓扑还没探到时这张表是空的，那时才少写这一行，
        //   出口交回核心自己的默认路由判断。
        if (idx < m_egressNics.size() && !m_egressNics.at(idx).friendlyName.isEmpty()) {
            block += QString("    interface-name: %1\n")
                             .arg(yamlQuote(m_egressNics.at(idx).friendlyName));
        }
        block += "    users:\n";
        for (const ProxyDevice *dev : *it) {
            block += QString("      - username: %1\n").arg(dev->user); // dev-<hex>，纯 ascii
            block += "        password: coast\n"; // 密码固定字面量，与 LanGateway 拨号一致
        }
    }

    // TPROXY 数据面：额外发一个 tproxy inbound。**与上面那个 socks listener 并存**——
    // 切换数据面不需要改配置结构,而且 lwIP 那条路随时能退回去(它用的是 socks 那个口)。
    //   · listen 必须是 0.0.0.0：TPROXY 投递保留的是**原始目的地址**,绑在 127.0.0.1 上收不到。
    //   · 不带 users:：TPROXY 下核心直接看得到设备的真实源 IP,设备身份用 SRC-IP-CIDR 规则表达,
    //     比 socks 用户名那套更原生(也少一层认证开销)。
    //   · TCP 与 UDP 共用这一个口,mihomo 的 tproxy inbound 两者都收。
    //   · ★ **也是每张卡一个**：出口网卡是 listener 的属性（interface-name），所以 tproxy 这条
    //     数据面同样要按卡分口，否则 Linux 上「设备从自己那条上行出去」根本不生效 —— 流量全从
    //     那个唯一的 tproxy 口进来，核心也就只有一个出口。nft 侧按 `iifname` 把每张卡的流量投给
    //     对应的口（见 TproxyRules 的 nicPorts）。端口向下编号，与 socks 那侧向上编号背向展开。
    if (m_config.gatewayTproxy) {
        for (auto it = byNic.constBegin(); it != byNic.constEnd(); ++it) {
            const int idx = it.key();
            block += QString("  - name: coast-tproxy-%1\n").arg(idx);
            block += "    type: tproxy\n";
            // ★ `::` 而不是 `0.0.0.0`：Linux 的双栈套接字（net.ipv6.bindv6only=0，发行版默认）
            //   一个 `::` 监听同时收 v4 与 v6，v4 连接以 ::ffff:a.b.c.d 形式到达。写 0.0.0.0 则
            //   **只收 v4**，v6 的 TPROXY 投递会因无监听而失败——这是 v6 走 TPROXY 的第一个必要
            //   条件（另一半是 TproxyRules 里那套 ip6 规则 + v6 策略路由）。
            block += "    listen: '::'\n";
            block += QString("    port: %1\n").arg(DeviceStore::tproxyPortFor(idx));
            // 与 socks 那侧同一条判据：友好名拿得到就绑（拿不到才少写这一行）。
            if (idx < m_egressNics.size() && !m_egressNics.at(idx).friendlyName.isEmpty()) {
                block += QString("    interface-name: %1\n")
                                 .arg(yamlQuote(m_egressNics.at(idx).friendlyName));
            }
        }
    } else if (m_config.gatewayPf) {
        // macOS 的数据面：pf rdr 把转发流量重定向到这个口，核心用 redir 入站接收，
        // 原始目的地由 PfRules::lookupOriginalDest（/dev/pf 的 DIOCNATLOOK）还原。
        //
        // ★ **redir 只代理 TCP，不支持 UDP**（mihomo 官方文档明确写明；tproxy 才两者都收）。
        //   所以 macOS 上：
        //     · TCP        → 走这条 redir，正常接管；
        //     · DNS(UDP53) → 由 PfRules 单独下一条 `rdr ... proto udp to port 53 -> DNS口` 打到
        //                    核心的 dns.listen 上，**不经过本 listener**，所以不受此限制；
        //     · 其余 UDP（QUIC/HTTP3、游戏、WireGuard…）→ **无法接管**，会按内核转发直连出去。
        //   这是 BSD 平台的固有限制（没有 TPROXY），不是实现没写完。双栈站点大多有 TCP 回退，
        //   但 QUIC-only 的流量在 macOS 网关下确实绕过代理 —— 记在这里，别当 bug 反复查。
        // ★ 与 socks / tproxy 两侧同构：**每张卡一个 redir 入站**，pf 的 rdr 规则本来就是逐卡
        //   下的（`rdr pass on <ifname>`），只是让它重定向到本卡专属的那个口。少了这一层，
        //   macOS 上「设备从自己那条上行出去」不生效 —— 与 Linux 上 tproxy 那次是同一个坑。
        for (auto it = byNic.constBegin(); it != byNic.constEnd(); ++it) {
            const int idx = it.key();
            block += QString("  - name: coast-redir-%1\n").arg(idx);
            block += "    type: redir\n";
            // ★★ **必须写 127.0.0.1，不能写 0.0.0.0。** 写 0.0.0.0 时 mihomo 建出来的是**IPv6
            //   通配套接字**（真机 lsof：`core IPv6 *:7897 (LISTEN)`），v4 连接以 v4-mapped
            //   形式到达；随后核心用 /dev/pf 的 DIOCNATLOOK 还原原始目的地时地址族对不上，查不
            //   到那条状态 —— 而 mihomo 的 darwin redir 在拿不到原始目的地时**不报错、不打
            //   日志**，accept 之后直接把连接丢掉。表现就是"三边看起来都对，唯独不通"。
            //   真机对照（同一条路由、同一个 pf 表，只改这一行）：
            //     listen: 0.0.0.0   → lsof 显示 IPv6 *:7897 → 设备请求 http=000，核心日志 0 行
            //     listen: 127.0.0.1 → lsof 显示 IPv4 127.0.0.1:7897 → 设备请求 http=404（真响应）
            //   两次 pf 的 rdr 计数都在涨，区别只在核心还不还得出原始目的地。
            //   绑 127.0.0.1 够用且更严：rdr 的目标本来就是 127.0.0.1，绑通配反而把这个口暴露
            //   给整个局域网。（与 TPROXY 那条相反——那边**必须**写 `::`。别照抄。）
            block += "    listen: 127.0.0.1\n";
            block += QString("    port: %1\n").arg(DeviceStore::redirPortFor(idx));
            if (idx < m_egressNics.size() && !m_egressNics.at(idx).friendlyName.isEmpty()) {
                block += QString("    interface-name: %1\n")
                                 .arg(yamlQuote(m_egressNics.at(idx).friendlyName));
            }
        }
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
            continue; // ★ 禁网**不在这里发**，见 applyRejectDevices（必须压过私网直连）
        } else if (dev.mode == "direct") {
            target = "DIRECT";
        } else if (dev.mode == "global") {
            if (dev.target.isEmpty()) {
                continue; // global 但没指定目标 → 跳过这台，避免写出悬空引用
            }
            target = dev.target;
        } else {
            // follow / rule：走整套订阅规则，这里什么都不发。
            // 「从哪张网卡出去」不再靠规则表达 —— 它是**入站的属性**，写在这台设备所属那个
            // listener 的 interface-name 上（见上面生成 listeners 的那段）。
            continue;
        }
        // 设备身份的载体按数据面分：
        //   lwIP   → IN-USER,<socks 用户名>   （设备流量经 coast-gateway 那个 socks 口带用户名进来）
        //   TPROXY → SRC-IP-CIDR,<ip>/32     （tproxy 入站没有用户名,但**原始源 IP 被完整保留**，
        //                                     核心直接看得到设备真实 IP,这是更原生的表达）
        // ★ 漏了这一步的后果不是"策略打折"而是**策略完全失效**：tproxy 下 IN-USER 永远匹配不上，
        //   于是 global/direct 形同虚设，被标为「禁网」的设备照样畅通无阻上网（见 applyRejectDevices）。
        // ★ 已知限制：SRC-IP-CIDR 绑的是**当前** IP。设备换址(DHCP 续约)后这条规则会指向旧地址,
        //   要靠台账更新触发 rebuildConfig 才纠正；MAC 是稳的而 IP 不是,这是 tproxy 这条路的固有代价。
        const QString rule = m_config.gatewayTproxy
                                 ? QString("SRC-IP-CIDR,%1/32,%2").arg(dev.ip, target)
                                 : QString("IN-USER,%1,%2").arg(dev.user, target);
        if (m_config.gatewayTproxy && dev.ip.isEmpty())
            continue; // 台账还没记到 IP：写不出规则，跳过（下一轮扫描到就有了）
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
        groups += "    interval: 60\n"; // 60 vs 300 的实测理由见本文件上方那处同名注释
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
        groups += "    interval: 60\n"; // 60 vs 300 的实测理由见本文件上方那处同名注释
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

QString ConfigBuilder::pruneUnreachableDns(QString yaml) const
{
    // 订阅方会用自己的 dns 段整体覆盖我们的模板，其中常年混着**在墙内根本连不上**的 DoH。
    // 后果比"少一个解析器"严重得多：核心按列表顺序挨个试，每撞一条不可达的就等一次连接超时，
    // 而这发生在**每条新连接**的名字解析上。
    //
    // 真机实测（Windows 2026-08-04，同一台机器、同一订阅、同一节点 JP-2）：
    //     DoH 服务器                 角色                 直连可达性
    //     https://dns.rubyfish.cn    nameserver 第一条    超时 6.0s
    //     https://1.0.0.1            fallback             超时 6.0s
    //     https://dns.twnic.tw       fallback             超时 18.0s
    //     https://public.dns.iij.jp  fallback             5.1s（勉强通但极慢）
    //     https://223.5.5.5          nameserver           32ms  ✓
    //     https://dns.pub            nameserver           32ms  ✓
    // 表现：**每条代理连接的 TTFB 稳定 5.1~6.2 秒**，而裸 TCP 到节点只要 0.19s、
    // 核心走 DIRECT 只要 0.016s。所有传输类型（vless/grpc、tuic、hysteria2）一起中招，
    // 因此很容易被误判成"节点烂""Windows 平台慢"——我们就误判过一整轮。
    // 剔掉这几条之后同一口径复测：**TTFB 5.14~6.24s → 0.158~0.213s（32 倍）**，
    // 与 Linux 端(Pi)同订阅同节点的 0.18s 完全一致，证明平台本身没有问题。
    //
    // 只剔**实测确认不可达**的固定名单，不做运行时探测：探测本身要花时间、还会因网络抖动误杀，
    // 而这几条是长期失效（rubyfish 域名已废弃、1.0.0.1/twnic 属被墙段）。名单之外一概不动，
    // 保持"订阅说什么就是什么"。剔完若某个列表空了，补回我们模板里那两条实测可用的。
    static const QStringList kUnreachable = {
        QStringLiteral("dns.rubyfish.cn"),
        QStringLiteral("1.0.0.1"),
        QStringLiteral("dns.twnic.tw"),
        QStringLiteral("public.dns.iij.jp"),
    };
    // 逐行扫：只删 dns 块里以 "- " 开头、且命中名单的那一行。default-nameserver 里的
    // 1.0.0.1 是**明文 UDP**（不是 DoH），墙内可达，不能按同一把尺子砍 —— 故只在
    // nameserver / fallback / proxy-server-nameserver 这三张表里剔。
    const QStringList lines = yaml.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    bool inDns = false;      // 是否在顶层 dns: 块内
    QString curList;         // 当前所在的子列表名
    int removed = 0;
    for (const QString &ln : lines) {
        if (!ln.startsWith(QLatin1Char(' ')) && !ln.startsWith(QLatin1Char('\t'))) {
            inDns = ln.startsWith(QStringLiteral("dns:")); // 顶格键 → 进/出 dns 块
            curList.clear();
        } else if (inDns) {
            const QRegularExpressionMatch key =
                QRegularExpression(QStringLiteral("^  ([a-z-]+):\\s*$")).match(ln);
            if (key.hasMatch())
                curList = key.captured(1);
        }
        const bool inTargetList = inDns
            && (curList == QLatin1String("nameserver") || curList == QLatin1String("fallback")
                || curList == QLatin1String("proxy-server-nameserver"));
        if (inTargetList && ln.trimmed().startsWith(QLatin1Char('-'))) {
            bool hit = false;
            for (const QString &bad : kUnreachable) {
                if (ln.contains(bad)) { hit = true; break; }
            }
            if (hit) { ++removed; continue; } // 丢掉这一行
        }
        out.append(ln);
    }
    if (removed == 0)
        return yaml;
    yaml = out.join(QLatin1Char('\n'));
    // 剔空的列表补回可用项：空列表在 mihomo 里等于"没有解析器"，比留着慢的还糟。
    static const QString kGood =
        QStringLiteral("\n    - https://223.5.5.5/dns-query\n    - https://dns.pub/dns-query");
    for (const QString &list : {QStringLiteral("nameserver"), QStringLiteral("fallback")}) {
        const QRegularExpression empty(
            QStringLiteral("(?m)^  %1:[ \\t]*\\n(?=\\S|  [a-z-]+:)").arg(list));
        const QRegularExpressionMatch m = empty.match(yaml);
        if (m.hasMatch())
            yaml.insert(m.capturedEnd(0) - 1, kGood);
    }
    return yaml;
}

QString ConfigBuilder::applyIpv6(QString yaml, bool enabled) const
{
    // IPv6 是一个整体，三个键缺一不可，所以放在一处一起写，别拆到各处去：
    //
    //   ipv6              核心是否使用 v6 出站；也决定 parseIPV6 保不保留 tun.inet6-address。
    //                     这个键我们以前从来不写，于是核心用的是 mihomo 默认值 true ——
    //                     结果是「用户看得见的地方（AAAA 应答）关着，看不见的地方半开着」：
    //                     TUN 拿到了 v6 地址和默认路由，却没有任何应用能解析出 AAAA。
    //                     现在无论开关处于哪一侧都显式写死，行为才和开关一致。
    //   dns.ipv6          本地 DNS 服务器是否对客户端返回 AAAA。关着时核心回空答案。
    //   dns.fake-ip-range6  fake-ip 模式下 AAAA 的假地址池。**没有它，前两个开了也白搭** ——
    //                     核心的 withFakeIP 在没有 v6 池时对 AAAA 一律回空答案。
    //
    // 取值沿用上游文档里的示例网段，和 tun.inet6-address 的默认值 fdfe:dcba:9876::1/126
    // 同前缀且不冲突（池子从 ::4 起分配，正好落在那个 /126 之外）；auto-route 会装 ::/0，
    // 所以这些假地址能回到 TUN。
    yaml = setScalar(yaml, "ipv6", enabled ? "true" : "false");
    yaml = setNestedScalar(yaml, "dns", "ipv6", enabled ? "true" : "false");
    if (enabled) {
        yaml = setNestedScalar(yaml, "dns", "fake-ip-range6", "fdfe:dcba:9876::1/64");
    }
    return yaml;
}

// ———————————— 每网卡出口的生成自测（COAST_NICEGRESS_SELFTEST）————————————
bool ConfigBuilder::runNicEgressSelfTest()
{
    int failed = 0;
    auto check = [&failed](bool ok, const char *what) {
        std::fputs(ok ? "  ok   " : "  FAIL ", stdout);
        std::fputs(what, stdout);
        std::fputc('\n', stdout);
        if (!ok)
            ++failed;
    };

    // 临时 configDir。**不自动删** —— 产物要留给 `core -t -f <path>` 让核心自己判合不合法。
    QTemporaryDir tmp;
    tmp.setAutoRemove(false);
    if (!tmp.isValid()) {
        std::printf("每网卡出口自测: 建不了临时目录\n");
        return false;
    }
    const QString configDir = tmp.path();

    // 造台账：三台设备。
    //   A 在**主**网卡网段、follow  → 不该被改写（主卡本来就走默认出口）
    //   B 在副网卡网段、follow      → 该走 SUB-RULE 进副卡的规则副本
    //   C 在副网卡网段、direct      → 该直接指到 COAST-OUT-1
    const QString macA = QStringLiteral("aa:aa:aa:aa:aa:01");
    const QString macB = QStringLiteral("bb:bb:bb:bb:bb:02");
    const QString macC = QStringLiteral("cc:cc:cc:cc:cc:03");
    {
        DeviceStore store(configDir);
        QVector<DeviceRecord> found;
        for (const auto &pair : {qMakePair(macA, QStringLiteral("192.168.1.50")),
                                 qMakePair(macB, QStringLiteral("192.168.2.60")),
                                 qMakePair(macC, QStringLiteral("192.168.2.61"))}) {
            DeviceRecord d;
            d.mac = pair.first;
            d.ip = pair.second;
            d.online = true;
            d.lastSeen = QDateTime::currentDateTime();
            found.append(d);
        }
        store.mergeDiscovered(found);
        store.setProxyEnabled(macA, true);
        store.setProxyEnabled(macB, true);
        store.setProxyEnabled(macC, true);
        store.setPolicy(macC, DevicePolicyMode::Direct, QString());
        store.save();
    }

    // 造几条自定义规则。**夹具必须自带 DIRECT 目标**：种子 default.yaml 的规则全指向策略组，
    // 私网那几条 DIRECT 又是 applyPrivateNetworkRules 在**复制之后**才加的 —— 光用种子跑，
    // 副本里一条 DIRECT 都没有，"改写对不对"这件事等于没测（第一版就是这样，白绿了一轮）。
    // 顺带把 REJECT 混进去，验证只改目标位上的 DIRECT、不误伤别的目标。
    {
        QFile rj(QDir(configDir).filePath(QStringLiteral("rules.json")));
        if (rj.open(QIODevice::WriteOnly)) {
            rj.write(R"({"rule":[
  {"type":"GEOIP","node":"DIRECT","value":"CN"},
  {"type":"DOMAIN-SUFFIX","node":"DIRECT","value":"cn"},
  {"type":"DOMAIN-SUFFIX","node":"REJECT","value":"ads.example.com"}
]})");
            rj.close();
        }
    }

    AppConfig cfg;
    cfg.userDir = configDir;
    cfg.configDir = configDir;
    // ★ 数据面必须**显式钉死**，不能吃平台默认值：AppConfig::gatewayTproxy 在 Linux 上默认
    //   true，于是同一份代码在 Linux 上生成的是 SRC-IP-CIDR、在 Windows 上是 IN-USER。
    //   第一版没钉，在 Linux 上跑出来的断言全是错的对象 —— 而"每网卡出口"要修的正是 Windows
    //   那条路。这里先测 IN-USER 形态，tproxy 形态在后面单独再测一遍。
    cfg.gatewayTproxy = false;
    ConfigBuilder builder(cfg);
    QVector<NicEgress> nics;
    NicEgress primary;      // 192.168.1.0/24，主网卡
    primary.friendlyName = QStringLiteral("以太网");
    primary.mask = 0xFFFFFF00u;
    primary.base = 0xC0A80100u;
    primary.primary = true;
    NicEgress second;       // 192.168.2.0/24，副网卡
    second.friendlyName = QStringLiteral("WLAN");
    second.mask = 0xFFFFFF00u;
    second.base = 0xC0A80200u;
    nics << primary << second;
    builder.setEgressNics(nics);

    const QString path = builder.ensureFullConfig(false, false);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        std::printf("每网卡出口自测: 生成失败 (%s)\n", qUtf8Printable(path));
        return false;
    }
    // ★ 断行统一成 \n 再断言。写盘走的是 QTextStream，**Windows 上落地的是 CRLF**（实测一份
    //   产物 2910 个 CRLF、0 个裸 LF），于是所有跨行的断言在 Windows 上全部匹配不上 ——
    //   第一次在 Windows 原生跑这个自测就是这样：产物明明是对的（拿给核心 `-t` 判过 rc=0），
    //   却报了 6 条 FAIL。**在它要保护的那个平台上会假报警的测试，比没有测试更糟**：
    //   下一个人不是去调查，就是把断言改松。这里只归一化断行，不放松任何一条判据。
    const QString yaml = QString::fromUtf8(f.readAll()).replace(QStringLiteral("\r\n"),
                                                                QStringLiteral("\n"));
    f.close();

    const QString userA = DeviceStore::socksUser(macA);
    const QString userB = DeviceStore::socksUser(macB);
    const QString userC = DeviceStore::socksUser(macC);

    // —— 每张卡一个入站，出口网卡写在入站上 ——
    check(yaml.contains(QStringLiteral(
                  "  - name: coast-gw-0\n    type: socks\n    listen: 127.0.0.1\n"
                  "    port: 7899\n    interface-name: '以太网'\n")),
          "主网卡的入站绑到主网卡");
    check(yaml.contains(QStringLiteral(
                  "  - name: coast-gw-1\n    type: socks\n    listen: 127.0.0.1\n"
                  "    port: 7900\n    interface-name: 'WLAN'\n")),
          "副网卡的入站绑到副网卡（端口 = 7899 + 卡号）");

    // 设备按所在网卡分进各自入站的 users —— 这就是「哪台设备从哪张卡出去」的全部表达。
    const qsizetype gw0 = yaml.indexOf(QStringLiteral("  - name: coast-gw-0"));
    const qsizetype gw1 = yaml.indexOf(QStringLiteral("  - name: coast-gw-1"));
    const QString seg0 = (gw0 >= 0 && gw1 > gw0) ? yaml.mid(gw0, gw1 - gw0) : QString();
    // gw-1 那一段到下一个顶层键为止（\n 后面直接跟非空格 = 顶层）。
    qsizetype end1 = -1;
    if (gw1 >= 0) {
        static const QRegularExpression topKey(QStringLiteral("(?m)^[^ \n]"));
        const auto m = topKey.match(yaml, gw1);
        end1 = m.hasMatch() ? m.capturedStart() : yaml.size();
    }
    const QString seg1 = gw1 >= 0 ? yaml.mid(gw1, end1 - gw1) : QString();
    check(seg0.contains(userA) && !seg0.contains(userB) && !seg0.contains(userC),
          "主网卡网段的设备只在 gw-0 里");
    check(seg1.contains(userB) && seg1.contains(userC) && !seg1.contains(userA),
          "副网卡网段的两台设备只在 gw-1 里");

    // 旧那套（给每张卡造 direct 出站 + 复制一份规则树）**必须一个字都不剩**：真机上它只能改写
    // DIRECT，而真实订阅里公网目的地一条 DIRECT 都没有，整套完全惰性；现在由入站属性取代。
    check(!yaml.contains(QStringLiteral("COAST-OUT")) && !yaml.contains(QStringLiteral("sub-rules:"))
                  && !yaml.contains(QStringLiteral("SUB-RULE")),
          "没有残留的 COAST-OUT / sub-rules / SUB-RULE");
    // follow 设备不再产生任何规则（它走整套订阅规则，出口由它所在的入站决定）。
    check(!yaml.contains(QStringLiteral("IN-USER,%1,").arg(userB)), "follow 设备不产生规则");
    // policy=direct 的设备仍旧指向普通 DIRECT —— 出口同样由入站决定，与规则无关。
    check(yaml.contains(QStringLiteral("IN-USER,%1,DIRECT").arg(userC)),
          "direct 设备指向普通 DIRECT");

    // 下面两轮都**换一个 configDir** 再生成。
    // ★ 全用同一个目录时，后一轮的 full.yaml 会把前一轮的**覆盖掉** —— 断言读的是内存里的
    //   字符串所以照样过，但最后打出来那条"配置在:"指的是被覆盖后的文件，拿它去跑 `core -t`
    //   验的就不是刚断言过的那份。第一版正是这样，差点照着错的产物下结论。
    auto rebuildIn = [&](const QString &dir, bool tproxy, bool pf,
                         const QVector<NicEgress> &use) -> QString {
        QDir().mkpath(dir);
        AppConfig c = cfg;
        c.userDir = dir;
        c.configDir = dir;
        c.gatewayTproxy = tproxy;
        c.gatewayPf = pf;
        // 台账（coast.db）与自定义规则（rules.json）都得跟着复制过去 —— 前者 proxiedDevices 按
        // configDir 打开，后者决定规则表长什么样。
        for (const char *f : {"coast.db", "rules.json"}) {
            QFile::copy(QDir(configDir).filePath(QLatin1String(f)),
                        QDir(dir).filePath(QLatin1String(f)));
        }
        ConfigBuilder b(c);
        b.setEgressNics(use);
        const QString p = b.ensureFullConfig(false, false);
        QFile fh(p);
        if (!fh.open(QIODevice::ReadOnly))
            return QString();
        const QString text = QString::fromUtf8(fh.readAll())
                                     .replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        fh.close();
        return text;
    };

    // —— 单网卡：**走的是同一段代码**，一张卡就是长度 1 的数组 ——
    // 所以照样发 interface-name（把主卡钉死，顺带治 Windows 自动跃点抖动），只是只有一个入站、
    // 端口就是 7899。这一档是绝大多数用户走的，钉住它别多冒出第二个入站。
    {
        QVector<NicEgress> one;
        one << primary;
        const QString y2 = rebuildIn(configDir + QStringLiteral("/../single"), false, false, one);
        check(!y2.isEmpty() && y2.contains(QStringLiteral("  - name: coast-gw-0\n"))
                      && !y2.contains(QStringLiteral("coast-gw-1"))
                      && y2.contains(QStringLiteral("    port: 7899\n    interface-name: '以太网'\n")),
              "单网卡：只有一个入站，且照样绑在那张卡上");
        check(!y2.contains(QStringLiteral("COAST-OUT")) && !y2.contains(QStringLiteral("sub-rules:")),
              "单网卡：不靠出站/子规则表达出口（那套是笛卡尔积，已删）");
    }
    // 拓扑还没探到的窗口：网卡表是空的 —— 没有名字可写就少写那一行，出口交回核心自己判断。
    // 口还是要开的，不然设备被投毒到本机却没人监听，那是断网不是回退。
    {
        const QString y2b = rebuildIn(configDir + QStringLiteral("/../nonic"), false, false,
                                      QVector<NicEgress>());
        check(!y2b.isEmpty() && y2b.contains(QStringLiteral("  - name: coast-gw-0\n"))
                      && !y2b.contains(QStringLiteral("interface-name:")),
              "网卡表为空：仍开口，只是不带 interface-name");
    }
    // TPROXY 形态（Linux 的默认数据面）：身份载体是 SRC-IP-CIDR，而**入站仍然按卡分**，
    // 所以出口绑定这件事在两条数据面上是同一套。
    {
        const QString y3 = rebuildIn(configDir + QStringLiteral("/../tproxy"), true, false, nics);
        // ★ tproxy **也要按卡分口**：Linux 的数据面走的是 tproxy 入站，不是上面那个 socks 口。
        //   这一条不成立时，Linux 上「设备从自己那条上行出去」整个不生效 —— 而配置看起来是对的
        //   （socks 那两个入站都带着 interface-name），最容易被当成已经修好。
        check(!y3.isEmpty()
                      && y3.contains(QStringLiteral("  - name: coast-tproxy-0\n    type: tproxy\n"
                                                    "    listen: '::'\n    port: 7898\n"
                                                    "    interface-name: '以太网'\n")),
              "tproxy 形态：主卡的 tproxy 入站绑到主卡");
        check(y3.contains(QStringLiteral("  - name: coast-tproxy-1\n    type: tproxy\n"
                                         "    listen: '::'\n    port: 7897\n"
                                         "    interface-name: 'WLAN'\n")),
              "tproxy 形态：副卡的 tproxy 入站绑到副卡（端口向下编号）");
        check(!y3.contains(QStringLiteral("- name: coast-tproxy\n")),
              "tproxy 形态：没有残留那个不分卡的老入站");
        check(y3.contains(QStringLiteral("SRC-IP-CIDR,192.168.2.61/32,DIRECT")),
              "tproxy 形态：direct 设备按源 IP 指向普通 DIRECT");
    }
    // pf 形态（macOS 的数据面）。★ 这一档在**任何平台**都测得到：redir 入站的生成是纯字符串
    //   拼装、与平台无关；只有 PfRules 那半截才是 macOS 专属。没有它，macOS 的配置生成就是
    //   零覆盖 —— 而这条线上没人能在本地编 macOS，CI 的 mac 任务又在外部仓库。
    {
        const QString y4 = rebuildIn(configDir + QStringLiteral("/../pf"), false, true, nics);
        check(!y4.isEmpty()
                      && y4.contains(QStringLiteral("  - name: coast-redir-0\n    type: redir\n"
                                                    "    listen: 127.0.0.1\n    port: 7897\n"
                                                    "    interface-name: '以太网'\n")),
              "pf 形态：主卡的 redir 入站绑到主卡");
        check(y4.contains(QStringLiteral("  - name: coast-redir-1\n    type: redir\n"
                                         "    listen: 127.0.0.1\n    port: 7896\n"
                                         "    interface-name: 'WLAN'\n")),
              "pf 形态：副卡的 redir 入站绑到副卡");
        check(!y4.contains(QStringLiteral("- name: coast-redir\n"))
                      && !y4.contains(QStringLiteral("coast-tproxy")),
              "pf 形态：没有老的单口 redir，也不该有 tproxy 入站");
    }

    std::printf("每网卡出口自测: %s\n配置在: %s\n（可直接用 `core -t -f <上面这个路径>` 让核心判合法性）\n",
                failed == 0 ? "全部通过" : "有失败项", qUtf8Printable(path));
    return failed == 0;
}

// ———————————————————————— 每网卡出口（NicEgress）————————————————————————
int ConfigBuilder::egressNicFor(const QString &ip) const
{
    const QHostAddress a(ip);
    if (a.protocol() != QAbstractSocket::IPv4Protocol) {
        return -1;
    }
    const quint32 v = a.toIPv4Address();
    for (int i = 0; i < m_egressNics.size(); ++i) {
        const NicEgress &n = m_egressNics.at(i);
        if (n.mask && (v & n.mask) == n.base) {
            return i;
        }
    }
    return -1;
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

// ———————————— TIDE 节点的生成自测（COAST_TIDE_SELFTEST）————————————
bool ConfigBuilder::runTideSelfTest()
{
    int failed = 0;
    auto check = [&failed](bool ok, const char *what) {
        std::fputs(ok ? "  ok   " : "  FAIL ", stdout);
        std::fputs(what, stdout);
        std::fputc('\n', stdout);
        if (!ok)
            ++failed;
    };

    QTemporaryDir tmp;
    tmp.setAutoRemove(false); // 产物要留给 `core -t -f <path>`
    if (!tmp.isValid()) {
        std::printf("TIDE 自测: 建不了临时目录\n");
        return false;
    }
    const QString configDir = tmp.path();

    // 一个真实长度的 TIDE 公钥：X25519(32) + ML-KEM-768 封装公钥(1184) = 1216 字节，
    // base64（RawURL，无填充）后 ceil(1216*4/3) = 1622 个字符。
    // **必须用真实长度**——这个自测存在的全部理由就是长标量，
    // 拿一个短的占位串来测等于什么都没测。
    constexpr int kPubKeyChars = 1622;
    QString pub;
    pub.reserve(kPubKeyChars);
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < kPubKeyChars; ++i)
        pub.append(QLatin1Char(alphabet[(i * 7 + 13) % 64]));

    const QString nodeName = QStringLiteral("自建-TIDE");
    {
        QFile sub(QDir(configDir).filePath(QStringLiteral("subscribe.yaml")));
        if (!sub.open(QIODevice::WriteOnly)) {
            std::printf("TIDE 自测: 写不了 subscribe.yaml\n");
            return false;
        }
        // 节点缩进与 SubscriptionStore::parseProxyList 的产出保持一致（6 空格字段）。
        const QString text = QStringLiteral(
            "- name: 'tide-test'\n"
            "  url: ''\n"
            "  type: 'clash'\n"
            "  use: true\n"
            "  updateTime: 15\n"
            "  speedtest: false\n"
            "  proxy: false\n"
            "  list:\n"
            "    - name: '%1'\n"
            "      type: tide\n"
            "      server: tide.example.com\n"
            "      port: 8443\n"
            "      password: 'sup3r-s3cret'\n"
            "      public-key: '%2'\n"
            "      sni: www.example.com\n"
            "      udp: true\n"
            "      quic: true\n"
            "      redundancy: true\n"
            "      use: true\n").arg(nodeName, pub);
        sub.write(text.toUtf8());
        sub.close();
    }

    AppConfig cfg;
    cfg.userDir = configDir;
    cfg.configDir = configDir;
    cfg.gatewayTproxy = false;
    ConfigBuilder builder(cfg);

    const QString path = builder.ensureFullConfig(false, false);
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) {
        std::printf("TIDE 自测: 生成失败 (%s)\n", qUtf8Printable(path));
        return false;
    }
    // Windows 上写盘落地的是 CRLF，跨行断言必须先归一化，否则在它要保护的平台上假报警
    // （同 runNicEgressSelfTest 的说明）。
    const QString yaml = QString::fromUtf8(f.readAll()).replace(QStringLiteral("\r\n"),
                                                                QStringLiteral("\n"));
    f.close();

    check(yaml.contains(QStringLiteral("type: tide")), "节点类型 tide 落进了 full.yaml");
    check(yaml.contains(nodeName), "节点名保留");
    // ★ 最重要的一条：1623 字符的公钥必须**逐字节完整**，不能被截断、折行或转义。
    check(yaml.contains(pub), "1622 字符的后量子公钥一字不差地穿过了字符串拼接");
    check(!yaml.contains(pub.left(200) + QStringLiteral("\n")),
          "公钥没有被折行（折行会让核心拒绝整份配置）");
    check(yaml.contains(QStringLiteral("password: 'sup3r-s3cret'"))
              || yaml.contains(QStringLiteral("password: sup3r-s3cret")),
          "password 保留");
    check(yaml.contains(QStringLiteral("quic: true")), "quic 开关保留");
    check(yaml.contains(QStringLiteral("redundancy: true")), "redundancy 开关保留");
    // 节点要真的被挂进策略组，否则生成是对的但用户选不到。
    check(yaml.indexOf(QStringLiteral("proxy-groups:")) >= 0
              && yaml.lastIndexOf(nodeName) > yaml.indexOf(QStringLiteral("proxy-groups:")),
          "节点出现在 proxy-groups 里（不然界面上选不到）");

    std::printf("\nfull.yaml: %s\n", qUtf8Printable(path));
    std::printf("拿核心自己判一遍： core -t -f \"%s\" -d \"%s\"\n",
                qUtf8Printable(path), qUtf8Printable(configDir));
    std::printf("（-d 目录里要有 Country.mmdb，否则会卡在 GeoIP 下载超时上，"
                "报错读起来像配置错误）\n");
    return failed == 0;
}
