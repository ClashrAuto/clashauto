#include "SettingsController.h"

#include "AppConfig.h"
#include "ClashService.h"
#include "CoreController.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QSysInfo>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <tlhelp32.h>
#endif

#if defined(Q_OS_MACOS)
#  include "MacHelperClient.h"
#endif

// ————————————————————— YAML 文本工具（复刻 MainWindow.cpp 的同名文件级函数）—————————————————————
namespace {

QString yamlUnescapeDq(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const QChar c = in.at(i);
        if (c == QLatin1Char('\\') && i + 1 < in.size()) {
            const QChar n = in.at(i + 1);
            if (n == QLatin1Char('U') && i + 9 < in.size()) {
                bool ok = false;
                const char32_t cp = in.mid(i + 2, 8).toUInt(&ok, 16);
                if (ok) { out += QString::fromUcs4(&cp, 1); i += 9; continue; }
            } else if (n == QLatin1Char('u') && i + 5 < in.size()) {
                bool ok = false;
                const uint cp = in.mid(i + 2, 4).toUInt(&ok, 16);
                if (ok) { out += QChar(cp); i += 5; continue; }
            } else if (n == QLatin1Char('n')) { out += QLatin1Char('\n'); ++i; continue; }
            else if (n == QLatin1Char('t')) { out += QLatin1Char('\t'); ++i; continue; }
            else if (n == QLatin1Char('"')) { out += QLatin1Char('"'); ++i; continue; }
            else if (n == QLatin1Char('\\')) { out += QLatin1Char('\\'); ++i; continue; }
            else if (n == QLatin1Char('/')) { out += QLatin1Char('/'); ++i; continue; }
        }
        out += c;
    }
    return out;
}

QString yamlListItemRaw(const QString &line)
{
    const int dash = line.indexOf(QLatin1String("- "));
    return dash < 0 ? QString() : line.mid(dash + 2).trimmed();
}

QString yamlScalarText(const QString &raw)
{
    if (raw.size() >= 2 && raw.startsWith(QLatin1Char('"')) && raw.endsWith(QLatin1Char('"'))) {
        return yamlUnescapeDq(raw.mid(1, raw.size() - 2));
    }
    if (raw.size() >= 2 && raw.startsWith(QLatin1Char('\'')) && raw.endsWith(QLatin1Char('\''))) {
        return raw.mid(1, raw.size() - 2).replace(QLatin1String("''"), QLatin1String("'"));
    }
    return raw;
}

QString yamlEscapeDq(const QString &in)
{
    QString out = in;
    out.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    out.replace(QLatin1Char('"'), QLatin1String("\\\""));
    return out;
}

QString yamlInlineValue(const QString &line)
{
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon < 0) return QString();
    return yamlScalarText(line.mid(colon + 1).trimmed());
}

// 解压内核压缩包（复刻 MainWindow::extractCoreBinary）
QString extractCoreBinary(const QString &archivePath, const QString &tmpDir)
{
    const QString outDir = QDir(tmpDir).filePath("extract");
    QDir(outDir).removeRecursively();
    QDir().mkpath(outDir);
#if defined(Q_OS_WIN)
    QProcess p;
    p.start(QStringLiteral("tar"), {"-xf", archivePath, "-C", outDir});
    p.waitForFinished(30000);
    int code = p.exitCode();
    if (code != 0) {
        QProcess ps;
        ps.start(QStringLiteral("powershell"), {"-NoProfile", "-Command",
            QStringLiteral("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force").arg(archivePath, outDir)});
        ps.waitForFinished(60000);
        code = ps.exitCode();
    }
    if (code != 0) {
        return QString();
    }
    QDirIterator it(outDir, {"*.exe"}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext() ? it.next() : QString();
#else
    const QString outBin = QDir(outDir).filePath("mihomo");
    QProcess gz;
    gz.setStandardOutputFile(outBin);
    gz.start("gzip", {"-dc", archivePath});
    if (!gz.waitForFinished(30000) || gz.exitCode() != 0) {
        return QString();
    }
    return QFileInfo(outBin).size() > 0 ? outBin : QString();
#endif
}

} // namespace

// —————————————————————————————————————————————————————————————————————————

SettingsController::SettingsController(CoreController *core, ClashService *clash, QObject *parent)
    : QObject(parent), m_core(core), m_clash(clash)
{
    loadInitialValues();
    reloadRules();
    reloadAreas();
    refreshMacHelperStatus();
}

void SettingsController::loadInitialValues()
{
    const AppConfig c = AppConfigLoader::load();
    m_userDir = c.userDir;
    m_host = c.host;
    m_uiPort = c.uiPort;
    m_mixedPort = c.mixedPort;
    m_webProxy = c.webProxy;
    m_nodeOnly = c.nodeOnlyAvailable;
    m_clearConnections = c.clearConnections;
    m_increment = c.increment;
    m_closeToTray = c.closeToTray;
    m_autoStart = c.autoStart;
    m_nodeNote = c.nodeSwitchNote;
    m_mirror = c.mirror;
    m_autoUpdate = c.autoUpdateMinutes;
    m_themeLight = c.theme.compare(QLatin1String("light"), Qt::CaseInsensitive) == 0;
    m_autoTheme = c.autoTheme;
    m_language = c.language;
    m_allowUse = c.allowRuleEnabled;
    m_allowRule = c.allowRule.isEmpty() ? QStringLiteral("\\[0\\.[0-9]+\\]") : c.allowRule;
    m_noAllowUse = c.noAllowRuleEnabled;
    m_noAllowRule = c.noAllowRule.isEmpty() ? QStringLiteral("CN|^CN|CN_") : c.noAllowRule;
    m_downloadHost = c.host;
    m_downloadMixedPort = c.mixedPort;
}

QStringList SettingsController::allowRulePresets() const
{
    return {QStringLiteral("\\[0\\.[0-9]+\\]")};
}

QStringList SettingsController::noAllowRulePresets() const
{
    return {QStringLiteral("\\[[0-9]+\\]"), QStringLiteral("[0-9\\[\\]]+"),
            QStringLiteral("[\\[0-9\\]]+$"), QStringLiteral("[0-9]{1}\\]$"),
            QStringLiteral("CN"), QStringLiteral("^CN"), QStringLiteral("CN_"), QStringLiteral(" ")};
}

QString SettingsController::userConfigPath() const
{
    return QDir(m_userDir).filePath("config.yaml");
}

void SettingsController::setMessage(const QString &msg)
{
    m_lastMessage = msg;
    emit messageChanged();
}

void SettingsController::setCoreStatus(const QString &msg, bool updating)
{
    if (m_coreUpdateStatus != msg) { m_coreUpdateStatus = msg; emit coreUpdateStatusChanged(); }
    if (m_coreUpdating != updating) { m_coreUpdating = updating; emit coreUpdatingChanged(); }
}

void SettingsController::setGeoipStatus(const QString &msg, bool updating)
{
    if (m_geoipStatus != msg) { m_geoipStatus = msg; emit geoipStatusChanged(); }
    if (m_geoipUpdating != updating) { m_geoipUpdating = updating; emit geoipUpdatingChanged(); }
}

// —————————————————————————— 持久化 ——————————————————————————

void SettingsController::persistConfigBool(const QString &key, bool value)
{
    // 轻量持久化：只改 config.yaml 的单个键、保留其余内容（复刻 MainWindow::persistConfigBool）
    const QString path = userConfigPath();
    QString yaml;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        yaml = QString::fromUtf8(in.readAll());
        in.close();
    }
    const QString line = QStringLiteral("%1: %2").arg(key, value ? "true" : "false");
    const QRegularExpression re(QStringLiteral("(?m)^%1:.*$").arg(QRegularExpression::escape(key)));
    if (re.match(yaml).hasMatch()) {
        yaml.replace(re, line);
    } else {
        if (!yaml.isEmpty() && !yaml.endsWith('\n')) {
            yaml += '\n';
        }
        yaml += line + '\n';
    }
    QFile out(path);
    if (out.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        out.write(yaml.toUtf8());
        out.close();
    }
}

void SettingsController::setNodeOnly(bool on)
{
    if (m_nodeOnly == on) {
        return;
    }
    m_nodeOnly = on;
    persistConfigBool(QStringLiteral("node"), on);
    // 注：Widgets 版此处还会 populateNodeList() 即时重建可见节点集；QML 版节点列表由
    // QmlBridge/NodeListModel 拥有，本适配层不改 bridge，故仅落盘，下次刷新/重启核心生效。
}

void SettingsController::setMirror(bool on)
{
    if (m_mirror == on) {
        return;
    }
    m_mirror = on;
    persistConfigBool(QStringLiteral("mirror"), on);
    emit mirrorChanged();
}

void SettingsController::applyAutoStart(bool enabled)
{
#if defined(Q_OS_WIN)
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (enabled) {
        const QString path = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        reg.setValue("ClashAuto", QString("\"%1\"").arg(path));
    } else {
        reg.remove("ClashAuto");
    }
#else
    Q_UNUSED(enabled);
#endif
}

void SettingsController::apply(const QString &host, int uiPort, int mixedPort, bool webProxy,
                               bool nodeOnly, bool clearConnections, bool increment, bool closeToTray,
                               bool autoStart, bool nodeNote, bool mirror, int autoUpdate,
                               bool themeLight, bool autoTheme, const QString &language,
                               const QString &allowRule, bool allowUse,
                               const QString &noAllowRule, bool noAllowUse)
{
    // 整表写 config.yaml —— 字段/顺序严格对齐 Widgets buildSettingsPage() 的保存 lambda。
    const QString path = userConfigPath();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setMessage(QStringLiteral("保存失败: %1").arg(file.errorString()));
        return;
    }
    {
        QTextStream out(&file);
        out << "host: " << host.trimmed() << "\n";
        out << "ui: " << QString::number(uiPort) << "\n";
        out << "port: " << QString::number(mixedPort) << "\n";
        out << "web: " << (webProxy ? "true" : "false") << "\n";
        // use:(增强/TUN) 写「当前实际状态」而非表单快照（由底部开关独占管理）
        out << "use: " << (m_core && m_core->isTunEnabled() ? "true" : "false") << "\n";
        out << "node: " << (nodeOnly ? "true" : "false") << "\n";
        out << "clearConnections: " << (clearConnections ? "true" : "false") << "\n";
        out << "increment: " << (increment ? "true" : "false") << "\n";
        out << "mini: " << (closeToTray ? "true" : "false") << "\n";
        out << "sys: " << (autoStart ? "true" : "false") << "\n";
        out << "note: " << (nodeNote ? "true" : "false") << "\n";
        out << "mirror: " << (mirror ? "true" : "false") << "\n";
        out << "autoUpdate: " << QString::number(autoUpdate) << "\n";
        out << "theme: " << (themeLight ? "light" : "black") << "\n";
        out << "autoTheme: " << (autoTheme ? "true" : "false") << "\n";
        out << "language: " << language << "\n";
        out << "use_rule:\n";
        out << "  allow: " << allowRule << "\n";
        out << "  noallow: " << noAllowRule << "\n";
        out << "  allowUse: " << (allowUse ? "true" : "false") << "\n";
        out << "  noallowUse: " << (noAllowUse ? "true" : "false") << "\n";
    }
    file.close();

    // —— 副作用（对齐 Widgets 保存 lambda）——
    // 「系统代理」应用即生效
    if (m_core && webProxy != m_core->isProxyEnabled()) {
        m_core->toggleProxy();
    }
    if (m_clash) {
        m_clash->setClearConnectionsOnSwitch(clearConnections);
    }
    applyAutoStart(autoStart);
    // autoUpdate：Widgets 版仅记录全局默认周期（主定时器 1 分钟节拍）；QML 版无此定时器逻辑，落盘即可。

    // 即时同步本地缓存值
    m_mirror = mirror;
    emit mirrorChanged();
    m_nodeOnly = nodeOnly;

    // 语言切换：码变了才发信号，交给 main_qml 的 I18n 运行时切换界面语言（装/卸翻译器 + retranslate）。
    if (language != m_language) {
        m_language = language;
        emit languageChangeRequested(language);
    }

    // API 端口变更：external-controller 不能热重载，改后需重启核心
    const int newPort = uiPort;
    const bool portChanged = (newPort > 0 && m_core && newPort != m_core->uiPort());
    if (portChanged) {
        m_core->setUiPort(newPort);
        if (m_clash) {
            m_clash->setEndpoint(host.trimmed(), newPort);
        }
    }
    if (mixedPort > 0 && m_clash) {
        m_clash->setMixedPort(mixedPort);
        m_downloadMixedPort = mixedPort;
    }
    if (!host.trimmed().isEmpty()) {
        m_downloadHost = host.trimmed();
    }
    if (portChanged && m_core->isRunning()) {
        m_core->stopCore();
        m_core->startCore();
        setMessage(QStringLiteral("API 端口已改为 %1，核心已重启").arg(newPort));
    } else if (m_core) {
        m_core->rebuildConfig();
        setMessage(portChanged ? QStringLiteral("API 端口已改为 %1（下次启动核心生效）").arg(newPort)
                               : QStringLiteral("设置已保存"));
    } else {
        setMessage(QStringLiteral("设置已保存"));
    }
}

// —————————————————————————— 规则 / 区域 ——————————————————————————

QString SettingsController::defaultConfigPath() const
{
    const QString userDef = QDir(m_userDir).filePath("default.yaml");
    if (!QFile::exists(userDef)) {
        const AppConfig cfg = AppConfigLoader::load();
        const QString bundled = QDir(cfg.sourceRoot).filePath("config/default.yaml");
        if (QFile::exists(bundled)) {
            QDir().mkpath(m_userDir);
            QFile::copy(bundled, userDef);
        }
    }
    return userDef;
}

QJsonArray SettingsController::loadRuleSection(const QString &section) const
{
    QFile f(defaultConfigPath());
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString yaml = QString::fromUtf8(f.readAll());
    f.close();
    const QStringList lines = yaml.split(QLatin1Char('\n'));
    QJsonArray arr;

    if (section == QLatin1String("rule")) {
        bool inRules = false;
        for (const QString &line : lines) {
            if (!inRules) {
                if (line.startsWith(QLatin1String("rules:"))) {
                    inRules = true;
                }
                continue;
            }
            if (line.startsWith(QLatin1String("  - "))) {
                const QString raw = yamlListItemRaw(line);
                const QStringList p = yamlScalarText(raw).split(QLatin1Char(','));
                QJsonObject o;
                o["raw"] = raw;
                if (p.value(0) == QLatin1String("MATCH")) {
                    o["type"] = QStringLiteral("MATCH");
                    o["node"] = p.value(1);
                    o["value"] = QString();
                } else {
                    o["type"] = p.value(0);
                    o["value"] = p.value(1);
                    o["node"] = p.value(2);
                }
                arr.append(o);
            } else if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1Char(' '))) {
                break;
            }
        }
        return arr;
    }

    if (section == QLatin1String("area")) {
        bool inGroups = false;
        QJsonObject cur;
        QStringList curRaw;
        QStringList curProxies;
        auto flush = [&]() {
            if (!curRaw.isEmpty()) {
                cur["raw"] = curRaw.join(QLatin1Char('\n'));
                cur["proxies"] = QJsonArray::fromStringList(curProxies);
                if (cur.value("rule").toString().isEmpty()) {
                    cur["rule"] = curProxies.join(QLatin1String(", "));
                }
                arr.append(cur);
            }
            cur = QJsonObject();
            curRaw.clear();
            curProxies.clear();
        };
        for (const QString &line : lines) {
            if (!inGroups) {
                if (line.startsWith(QLatin1String("proxy-groups:"))) {
                    inGroups = true;
                }
                continue;
            }
            if (!line.trimmed().isEmpty() && !line.startsWith(QLatin1Char(' '))) {
                flush();
                break;
            }
            if (line.startsWith(QLatin1String("  - "))) {
                flush();
                curRaw << line;
                const QString afterDash = yamlListItemRaw(line);
                if (afterDash.startsWith(QLatin1String("name:"))) {
                    cur["name"] = yamlInlineValue(afterDash);
                }
            } else {
                curRaw << line;
                const QString t = line.trimmed();
                if (t.startsWith(QLatin1String("name:"))) {
                    cur["name"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("type:"))) {
                    cur["type"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("rule:"))) {
                    cur["rule"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("url:"))) {
                    cur["url"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("interval:"))) {
                    cur["interval"] = yamlInlineValue(t);
                } else if (t.startsWith(QLatin1String("- "))) {
                    curProxies << yamlScalarText(yamlListItemRaw(t));
                }
            }
        }
        flush();
        return arr;
    }
    return {};
}

bool SettingsController::saveRuleSection(const QString &section, const QJsonArray &array)
{
    if (section != QLatin1String("rule") && section != QLatin1String("area")) {
        return false;
    }
    const QString path = defaultConfigPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QString yaml = QString::fromUtf8(f.readAll());
    f.close();

    const QString key = section == QLatin1String("area") ? QStringLiteral("proxy-groups:")
                                                         : QStringLiteral("rules:");
    QString block = key + QLatin1Char('\n');
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        const QString raw = o.value("raw").toString();
        if (section == QLatin1String("area")) {
            if (!raw.isEmpty()) {
                block += raw + QLatin1Char('\n');
            } else {
                QString type = o.value("type").toString();
                if (type.isEmpty()) {
                    type = QStringLiteral("select");
                }
                block += QStringLiteral("  - name: \"") + yamlEscapeDq(o.value("name").toString()) + QStringLiteral("\"\n");
                block += QStringLiteral("    type: ") + type + QLatin1Char('\n');
                if (type == QLatin1String("url-test") || type == QLatin1String("fallback")
                    || type == QLatin1String("load-balance")) {
                    QString url = o.value("url").toString();
                    if (url.isEmpty()) {
                        url = QStringLiteral("http://www.gstatic.com/generate_204");
                    }
                    QString interval = o.value("interval").toString();
                    if (interval.isEmpty()) {
                        interval = QStringLiteral("300");
                    }
                    block += QStringLiteral("    url: ") + url + QLatin1Char('\n');
                    block += QStringLiteral("    interval: ") + interval + QLatin1Char('\n');
                }
                block += QStringLiteral("    proxies:\n");
                const QJsonArray proxies = o.value("proxies").toArray();
                if (proxies.isEmpty()) {
                    block += QStringLiteral("      - DIRECT\n");
                } else {
                    for (const QJsonValue &pv : proxies) {
                        block += QStringLiteral("      - \"") + yamlEscapeDq(pv.toString()) + QStringLiteral("\"\n");
                    }
                }
            }
        } else { // rule
            if (!raw.isEmpty()) {
                block += QStringLiteral("  - ") + raw + QLatin1Char('\n');
            } else {
                const QString type = o.value("type").toString();
                const QString node = o.value("node").toString();
                const QString value = o.value("value").toString();
                const QString s = (type == QLatin1String("MATCH"))
                                      ? type + QLatin1Char(',') + node
                                      : type + QLatin1Char(',') + value + QLatin1Char(',') + node;
                block += QStringLiteral("  - \"") + yamlEscapeDq(s) + QStringLiteral("\"\n");
            }
        }
    }

    int start = -1;
    if (yaml.startsWith(key)) {
        start = 0;
    } else {
        const int p = yaml.indexOf(QLatin1Char('\n') + key);
        if (p >= 0) {
            start = p + 1;
        }
    }
    if (start < 0) {
        if (!yaml.endsWith(QLatin1Char('\n'))) {
            yaml += QLatin1Char('\n');
        }
        yaml += block;
    } else {
        int end = yaml.size();
        int pos = yaml.indexOf(QLatin1Char('\n'), start);
        while (pos >= 0) {
            const int lineStart = pos + 1;
            if (lineStart >= yaml.size()) {
                end = yaml.size();
                break;
            }
            const int nextNl = yaml.indexOf(QLatin1Char('\n'), lineStart);
            const QString ln = yaml.mid(lineStart, (nextNl < 0 ? yaml.size() : nextNl) - lineStart);
            if (ln.startsWith(QLatin1Char(' ')) || ln.trimmed().isEmpty()) {
                if (nextNl < 0) {
                    end = yaml.size();
                    break;
                }
                pos = nextNl;
            } else {
                end = lineStart;
                break;
            }
        }
        yaml.replace(start, end - start, block);
    }

    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    f.write(yaml.toUtf8());
    f.close();
    return true;
}

void SettingsController::reloadRules()
{
    const QJsonArray array = loadRuleSection(QStringLiteral("rule"));
    m_ruleTotal = array.size();
    const QString filter = m_ruleFilter.trimmed();
    const int cap = 400;
    const QStringList keys{QStringLiteral("type"), QStringLiteral("node"), QStringLiteral("value")};
    QVariantList rows;
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject obj = array[i].toObject();
        if (!filter.isEmpty()) {
            bool match = false;
            for (const QString &k : keys) {
                if (obj.value(k).toString().contains(filter, Qt::CaseInsensitive)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                continue;
            }
        }
        if (rows.size() >= cap) {
            break;
        }
        QVariantMap m;
        m["type"] = obj.value("type").toString();
        m["node"] = obj.value("node").toString();
        m["value"] = obj.value("value").toString();
        m["index"] = i; // 完整数组的真实下标（过滤/截断下仍有效）
        rows.append(m);
    }
    m_ruleRows = rows;
    emit rulesChanged();
}

void SettingsController::reloadAreas()
{
    const QJsonArray array = loadRuleSection(QStringLiteral("area"));
    QVariantList rows;
    for (int i = 0; i < array.size(); ++i) {
        const QJsonObject obj = array[i].toObject();
        QVariantMap m;
        m["name"] = obj.value("name").toString();
        m["type"] = obj.value("type").toString();
        m["rule"] = obj.value("rule").toString();
        m["index"] = i;
        rows.append(m);
    }
    m_areaRows = rows;
    emit areasChanged();
}

void SettingsController::setRuleFilter(const QString &text)
{
    if (m_ruleFilter == text) {
        return;
    }
    m_ruleFilter = text;
    reloadRules();
}

QVariantMap SettingsController::ruleAt(int index) const
{
    const QJsonArray array = loadRuleSection(QStringLiteral("rule"));
    QVariantMap m;
    if (index >= 0 && index < array.size()) {
        const QJsonObject o = array[index].toObject();
        m["type"] = o.value("type").toString();
        m["node"] = o.value("node").toString();
        m["value"] = o.value("value").toString();
    }
    return m;
}

QVariantMap SettingsController::areaAt(int index) const
{
    const QJsonArray array = loadRuleSection(QStringLiteral("area"));
    QVariantMap m;
    if (index >= 0 && index < array.size()) {
        const QJsonObject o = array[index].toObject();
        m["name"] = o.value("name").toString();
        m["type"] = o.value("type").toString();
        m["url"] = o.value("url").toString();
        m["interval"] = o.value("interval").toString();
        QStringList members;
        for (const QJsonValue &pv : o.value("proxies").toArray()) {
            members << pv.toString();
        }
        m["proxiesText"] = members.join(QLatin1Char('\n'));
    }
    return m;
}

void SettingsController::saveRule(int editIndex, const QString &type, const QString &value, const QString &node)
{
    const QString t = type.trimmed();
    const QString n = node.trimmed();
    if (t.isEmpty() || n.isEmpty()) {
        return; // 类型/节点必填
    }
    QJsonObject obj; // 不含 raw → 回写按 3 段（或 MATCH 2 段）重新序列化
    obj["type"] = t;
    obj["node"] = n;
    obj["value"] = value.trimmed();
    QJsonArray current = loadRuleSection(QStringLiteral("rule"));
    if (editIndex >= 0 && editIndex < current.size()) {
        current[editIndex] = obj;
    } else {
        current.prepend(obj); // 新增前插（对齐旧项目 def.rules.unshift）
    }
    saveRuleSection(QStringLiteral("rule"), current);
    reloadRules();
    if (m_core) {
        m_core->rebuildConfig();
    }
    setMessage(QStringLiteral("已保存一条规则并重建配置"));
}

void SettingsController::deleteRule(int index)
{
    QJsonArray current = loadRuleSection(QStringLiteral("rule"));
    if (index >= 0 && index < current.size()) {
        current.removeAt(index);
        saveRuleSection(QStringLiteral("rule"), current);
        reloadRules();
        if (m_core) {
            m_core->rebuildConfig();
        }
        setMessage(QStringLiteral("已删除一条规则并重建配置"));
    }
}

void SettingsController::saveArea(int editIndex, const QString &name, const QString &type,
                                  const QString &proxiesText, const QString &url, const QString &interval)
{
    const QString nm = name.trimmed();
    if (nm.isEmpty()) {
        return;
    }
    QJsonObject obj; // 不含 raw → 回写时重新序列化整组
    obj["name"] = nm;
    obj["type"] = type;
    obj["url"] = url.trimmed();
    obj["interval"] = interval.trimmed();
    QJsonArray proxies;
    const QStringList lines = proxiesText.split(QLatin1Char('\n'));
    for (const QString &m : lines) {
        const QString v = m.trimmed();
        if (!v.isEmpty()) {
            proxies.append(v);
        }
    }
    obj["proxies"] = proxies;

    QJsonArray current = loadRuleSection(QStringLiteral("area"));
    const bool isEdit = editIndex >= 0 && editIndex < current.size();
    if (isEdit) {
        current[editIndex] = obj;
    } else {
        current.append(obj);
    }
    saveRuleSection(QStringLiteral("area"), current);
    reloadAreas();
    if (m_core) {
        m_core->rebuildConfig();
    }
    setMessage(QStringLiteral("已保存区域分组并重建配置"));
}

QStringList SettingsController::proxyGroupNames() const
{
    QStringList names;
    const QJsonArray groups = loadRuleSection(QStringLiteral("area"));
    for (const QJsonValue &v : groups) {
        const QString n = v.toObject().value("name").toString();
        if (!n.isEmpty()) {
            names << n;
        }
    }
    return names;
}

QStringList SettingsController::processChoices(bool wantPath) const
{
    QStringList list;
    QSet<QString> seen;
#if defined(Q_OS_WIN)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snap, &entry)) {
            do {
                QString value;
                if (!wantPath) {
                    value = QString::fromWCharArray(entry.szExeFile);
                } else {
                    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                    if (proc) {
                        wchar_t buf[MAX_PATH];
                        DWORD sz = MAX_PATH;
                        if (QueryFullProcessImageNameW(proc, 0, buf, &sz)) {
                            value = QString::fromWCharArray(buf, static_cast<int>(sz));
                        }
                        CloseHandle(proc);
                    }
                }
                if (!value.isEmpty() && !seen.contains(value)) {
                    seen.insert(value);
                    list << value;
                }
            } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
    }
#else
    QProcess ps;
    ps.start(QStringLiteral("ps"), {QStringLiteral("-eo"), QStringLiteral("comm=")});
    if (ps.waitForFinished(3000)) {
        const QList<QByteArray> lines = ps.readAllStandardOutput().split('\n');
        for (const QByteArray &l : lines) {
            const QString p = QString::fromUtf8(l).trimmed();
            const QString value = wantPath ? p : QFileInfo(p).fileName();
            if (!value.isEmpty() && !seen.contains(value)) {
                seen.insert(value);
                list << value;
            }
        }
    }
#endif
    list.sort(Qt::CaseInsensitive);
    return list;
}

// —————————————————————————— 下载：内核 / GeoIP ——————————————————————————

void SettingsController::applyDownloadProxy(QNetworkAccessManager *nam) const
{
    if (nam && m_core && m_core->isRunning()) {
        nam->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_downloadHost,
                                    static_cast<quint16>(m_downloadMixedPort)));
    }
}

void SettingsController::updateGeoip()
{
    if (m_geoipUpdating) {
        return;
    }
    setGeoipStatus(QStringLiteral("下载中..."), true);
    const AppConfig cfg = AppConfigLoader::load();
    auto *nam = new QNetworkAccessManager(this);
    applyDownloadProxy(nam);
    const QUrl mmdbUrl(QStringLiteral(
        "https://github.com/MetaCubeX/meta-rules-dat/releases/latest/download/country.mmdb"));
    QNetworkRequest req(mmdbUrl);
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 r, qint64 t) {
        if (t > 0) {
            setGeoipStatus(QStringLiteral("下载中 %1%").arg(int(r * 100 / t)), true);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, cfg] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray data = reply->readAll();
        const QString err = reply->errorString();
        reply->deleteLater();
        nam->deleteLater();
        if (!ok || data.isEmpty()) {
            setGeoipStatus(QStringLiteral("更新 GeoIP"), false);
            setMessage(QStringLiteral("GeoIP 更新失败: %1").arg(err));
            return;
        }
        int saved = 0;
        const QStringList targets = {QDir(cfg.userDir).filePath("Country.mmdb"),
                                     QDir(cfg.sourceRoot).filePath("config/Country.mmdb")};
        for (const QString &path : targets) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile out(path);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(data);
                out.close();
                ++saved;
            }
        }
        setGeoipStatus(QStringLiteral("更新 GeoIP"), false);
        setMessage(QStringLiteral("GeoIP 已更新（%1 处，%2 KB），重启核心生效")
                       .arg(QString::number(saved), QString::number(data.size() / 1024)));
    });
}

void SettingsController::updateCore()
{
    if (m_coreUpdating) {
        return;
    }
    setCoreStatus(QStringLiteral("检查中..."), true);
    const AppConfig cfg = AppConfigLoader::load();
    const bool useMirror = m_mirror;
    const QString mirror = useMirror ? QStringLiteral("https://ghfast.top/") : QString();

    auto *nam = new QNetworkAccessManager(this);
    applyDownloadProxy(nam);
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")));
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, cfg, mirror] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = reply->readAll();
        const QString err = reply->errorString();
        reply->deleteLater();
        if (!ok) {
            nam->deleteLater();
            setCoreStatus(QStringLiteral("更新内核"), false);
            setMessage(QStringLiteral("内核更新失败: 查询版本出错 %1").arg(err));
            return;
        }
        const QJsonObject rel = QJsonDocument::fromJson(body).object();
        const QString tag = rel.value("tag_name").toString();

#if defined(Q_OS_WIN)
        const QString os = QStringLiteral("windows");
        const QString ext = QStringLiteral(".zip");
#elif defined(Q_OS_MACOS)
        const QString os = QStringLiteral("darwin");
        const QString ext = QStringLiteral(".gz");
#else
        const QString os = QStringLiteral("linux");
        const QString ext = QStringLiteral(".gz");
#endif
        const bool isArm = QSysInfo::currentCpuArchitecture().contains("arm");
        const QString arch = isArm ? QStringLiteral("arm64") : QStringLiteral("amd64");
        const QJsonArray assets = rel.value("assets").toArray();
        QStringList candidates;
        if (!isArm) {
            candidates << QStringLiteral("mihomo-%1-amd64-compatible-%2%3").arg(os, tag, ext);
        }
        candidates << QStringLiteral("mihomo-%1-%2-%3%4").arg(os, arch, tag, ext);
        QString url, assetName;
        for (const QString &want : candidates) {
            for (const QJsonValue &av : assets) {
                if (av.toObject().value("name").toString() == want) {
                    url = av.toObject().value("browser_download_url").toString();
                    assetName = want;
                    break;
                }
            }
            if (!url.isEmpty()) {
                break;
            }
        }
        if (url.isEmpty()) {
            const QString pat = isArm ? QStringLiteral("^mihomo-%1-arm64-v\\d+\\.\\d").arg(os)
                                      : QStringLiteral("^mihomo-%1-amd64-compatible-v\\d+\\.\\d").arg(os);
            const QRegularExpression re(pat);
            for (const QJsonValue &av : assets) {
                const QString n = av.toObject().value("name").toString();
                if (re.match(n).hasMatch() && n.endsWith(ext)) {
                    url = av.toObject().value("browser_download_url").toString();
                    assetName = n;
                    break;
                }
            }
        }
        if (url.isEmpty()) {
            nam->deleteLater();
            setCoreStatus(QStringLiteral("更新内核"), false);
            setMessage(QStringLiteral("内核更新失败: 未找到匹配的 mihomo 资源 (%1/%2)").arg(os, arch));
            return;
        }

        setCoreStatus(QStringLiteral("下载中..."), true);
        setMessage(QStringLiteral("下载 mihomo %1 ...%2").arg(tag, mirror.isEmpty() ? QString() : QStringLiteral("（国内加速）")));
        QNetworkRequest dreq{QUrl(mirror + url)};
        dreq.setRawHeader("User-Agent", "clashauto-cpp");
        dreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        dreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        dreq.setTransferTimeout(30000);
#endif
        // 国内加速时直连镜像下载（不套节点代理）；挂在 nam 名下随之销毁
        QNetworkAccessManager *dlNam = nam;
        if (!mirror.isEmpty()) {
            dlNam = new QNetworkAccessManager(nam);
            dlNam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        }
        QNetworkReply *dl = dlNam->get(dreq);
        connect(dl, &QNetworkReply::downloadProgress, this, [this](qint64 r, qint64 t) {
            if (t > 0) {
                setCoreStatus(QStringLiteral("下载中 %1%").arg(int(r * 100 / t)), true);
            }
        });
        connect(dl, &QNetworkReply::finished, this, [this, dl, nam, cfg, tag, assetName] {
            const bool ok2 = dl->error() == QNetworkReply::NoError;
            const QByteArray data = dl->readAll();
            const QString err2 = dl->errorString();
            dl->deleteLater();
            if (!ok2 || data.isEmpty()) {
                nam->deleteLater();
                setCoreStatus(QStringLiteral("更新内核"), false);
                setMessage(QStringLiteral("内核下载失败: %1").arg(err2));
                return;
            }
            const QString tmpDir = QDir(cfg.userDir).filePath("core-update");
            QDir().mkpath(tmpDir);
            const QString archivePath = QDir(tmpDir).filePath(assetName);
            QFile f(archivePath);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                nam->deleteLater();
                setCoreStatus(QStringLiteral("更新内核"), false);
                setMessage(QStringLiteral("内核更新失败: 无法写入临时文件"));
                return;
            }
            f.write(data);
            f.close();
            nam->deleteLater();

            setCoreStatus(QStringLiteral("安装中..."), true);
            const QString extracted = extractCoreBinary(archivePath, tmpDir);
            if (extracted.isEmpty()) {
                QDir(tmpDir).removeRecursively();
                setCoreStatus(QStringLiteral("更新内核"), false);
                setMessage(QStringLiteral("内核更新失败: 解压失败（缺少 tar/gzip 或压缩包异常）"));
                return;
            }
            const QString target = cfg.clashExecutable();
            const bool wasRunning = m_core && m_core->isRunning();
            if (wasRunning) {
                m_core->stopCore();
            }
            QDir().mkpath(QFileInfo(target).absolutePath());
            bool replaced = false;
            for (int attempt = 0; attempt < 8 && !replaced; ++attempt) {
                QFile::remove(target);
                if (QFile::copy(extracted, target)) {
                    replaced = true;
                } else {
                    QThread::msleep(150);
                }
            }
#if !defined(Q_OS_WIN)
            if (replaced) {
                QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                                  | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                                  | QFileDevice::ReadOther | QFileDevice::ExeOther);
            }
#endif
            QDir(tmpDir).removeRecursively();
            if (wasRunning) {
                m_core->startCore();
            }
            setCoreStatus(QStringLiteral("更新内核"), false);
            if (replaced) {
                setMessage(QStringLiteral("内核已更新到 mihomo %1%2").arg(tag, wasRunning ? QStringLiteral("，已重启核心") : QString()));
            } else {
                setMessage(QStringLiteral("内核更新失败: 无法替换文件（可能被占用）"));
            }
        });
    });
}

// —————————————————————————— macOS 免密助手 ——————————————————————————

bool SettingsController::macHelperSupported() const
{
#if defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

void SettingsController::refreshMacHelperStatus()
{
#if defined(Q_OS_MACOS)
    QString text;
    switch (MacHelper::status()) {
        case MacHelper::RegStatus::Enabled:          text = QStringLiteral("已启用（代理/增强免密）"); break;
        case MacHelper::RegStatus::RequiresApproval: text = QStringLiteral("待批准：请在系统设置→登录项允许"); break;
        case MacHelper::RegStatus::NotRegistered:    text = QStringLiteral("未安装"); break;
        case MacHelper::RegStatus::NotFound:         text = QStringLiteral("不可用（bundle 布局异常）"); break;
        default:                                     text = QStringLiteral("未知"); break;
    }
    if (m_macHelperStatus != text) {
        m_macHelperStatus = text;
        emit macHelperStatusChanged();
    }
#endif
}

void SettingsController::installMacHelper()
{
#if defined(Q_OS_MACOS)
    QString err;
    const MacHelper::RegStatus st = MacHelper::registerDaemon(&err);
    refreshMacHelperStatus();
    if (st == MacHelper::RegStatus::Enabled) {
        setMessage(QStringLiteral("免密助手已启用"));
    } else if (st == MacHelper::RegStatus::RequiresApproval) {
        MacHelper::openLoginItemsSettings();
        setMessage(QStringLiteral("免密助手需在「系统设置 → 登录项」允许 Clash Auto 的后台项，批准后自动生效"));
        startMacHelperApprovalWatch();
    } else {
        setMessage(QStringLiteral("免密助手安装失败：%1").arg(err));
    }
#endif
}

void SettingsController::uninstallMacHelper()
{
#if defined(Q_OS_MACOS)
    QString err;
    if (MacHelper::unregisterDaemon(&err)) {
        setMessage(QStringLiteral("免密助手已卸载"));
    } else {
        setMessage(QStringLiteral("免密助手卸载失败：%1").arg(err));
    }
    if (m_helperApprovalTimer) {
        m_helperApprovalTimer->stop();
    }
    refreshMacHelperStatus();
#endif
}

void SettingsController::startMacHelperApprovalWatch()
{
#if defined(Q_OS_MACOS)
    if (!m_helperApprovalTimer) {
        m_helperApprovalTimer = new QTimer(this);
        m_helperApprovalTimer->setInterval(2000);
        connect(m_helperApprovalTimer, &QTimer::timeout, this, [this] {
            refreshMacHelperStatus();
            if (MacHelper::status() == MacHelper::RegStatus::Enabled) {
                m_helperApprovalTimer->stop();
                setMessage(QStringLiteral("免密助手已获批准并启用"));
            } else if (++m_helperApprovalTicks >= 60) {
                m_helperApprovalTimer->stop();
            }
        });
    }
    m_helperApprovalTicks = 0;
    m_helperApprovalTimer->start();
#endif
}
