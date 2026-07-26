#include "IssueReporter.h"

#include "CoreController.h"
#include "Version.h" // APP_VERSION（CMake configure_file 生成）

#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

constexpr const char *kRepo = "ClashrAuto/clashauto";
constexpr const char *kLabel = "auto-report";
// 浏览器路径下正文的长度上限：issues/new 是把正文塞在 URL 查询串里的，太长会被浏览器/网关截断。
constexpr int kMaxUrlBodyChars = 6000;

// —— 全局 Qt 消息处理器 ——
QtMessageHandler g_prevHandler = nullptr;
QPointer<IssueReporter> g_reporter;
// report() 内部若自己抛了警告（网络栈很爱抛），不能再回到这里，否则无限递归。
thread_local bool t_inHandler = false;

// 该不该把这条 Qt 消息当成"报错"。Critical/Fatal 一律算；Warning 太杂（Qt 自身有大量无害警告），
// 只收看起来真的是错误的那些——QML 的 TypeError/ReferenceError 就走在这条路上。
bool reportableQtMessage(QtMsgType type, const QString &msg)
{
    if (type == QtCriticalMsg || type == QtFatalMsg) {
        return true;
    }
    if (type != QtWarningMsg) {
        return false;
    }
    static const QRegularExpression re(
            QStringLiteral("(?i)(error|failed|failure|exception|TypeError|ReferenceError|"
                           "SyntaxError|cannot |unable to |失败|错误|无法)"));
    return re.match(msg).hasMatch();
}

void messageHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (g_prevHandler) {
        g_prevHandler(type, ctx, msg); // 控制台输出照旧
    }
    if (t_inHandler || g_reporter.isNull() || !reportableQtMessage(type, msg)) {
        return;
    }
    t_inHandler = true;
    QString line = msg;
    if (ctx.file && *ctx.file) {
        // QML 报错的文件:行号是最有用的一段上下文，务必带上。
        line += QStringLiteral(" (%1:%2)").arg(QString::fromUtf8(ctx.file)).arg(ctx.line);
    }
    // 消息处理器可能在任意线程被调用 → 排队回 reporter 自己的线程。
    QMetaObject::invokeMethod(g_reporter, "report", Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("qt")), Q_ARG(QString, line));
    t_inHandler = false;
}

// URL 的 host 是否可以原样留在公开 issue 里。只放行本项目排障真正会用到的公共服务，
// 其余（尤其是订阅/节点域名）连 host 一起抹掉。
bool publicHost(const QString &host)
{
    static const QStringList allow = {
        QStringLiteral("github.com"),        QStringLiteral("api.github.com"),
        QStringLiteral("githubusercontent.com"), QStringLiteral("ghfast.top"),
        QStringLiteral("npcap.com"),         QStringLiteral("nmap.org"),
        QStringLiteral("localhost"),         QStringLiteral("127.0.0.1"),
    };
    const QString h = host.section(QLatin1Char(':'), 0, 0).toLower();
    for (const QString &a : allow) {
        if (h == a || h.endsWith(QLatin1Char('.') + a)) {
            return true;
        }
    }
    return false;
}

} // namespace

IssueReporter::IssueReporter(AppConfig config, CoreController *core, QObject *parent)
    : QObject(parent), m_config(std::move(config)), m_core(core)
{
    QSettings s;
    m_enabled = s.value(QStringLiteral("issues/autoReport"), false).toBool();
    m_asked = s.value(QStringLiteral("issues/asked"), false).toBool();
    m_token = s.value(QStringLiteral("issues/token")).toString();

    loadSeen();

    m_autoTimer = new QTimer(this);
    m_autoTimer->setSingleShot(true);
    connect(m_autoTimer, &QTimer::timeout, this, [this] {
        if (!m_enabled || m_pending.isEmpty() || m_submitting) {
            return;
        }
        // 限流：距上次提交不足 kMinSubmitIntervalMs 就把这批往后推，不丢。
        if (m_sinceSubmit.isValid() && m_sinceSubmit.elapsed() < kMinSubmitIntervalMs) {
            m_autoTimer->start(static_cast<int>(kMinSubmitIntervalMs - m_sinceSubmit.elapsed()));
            return;
        }
        submit();
    });
}

IssueReporter::~IssueReporter()
{
    // 卸掉处理器，避免退出过程中还有消息打到已析构的对象上。
    if (g_reporter == this) {
        qInstallMessageHandler(g_prevHandler);
        g_prevHandler = nullptr;
        g_reporter = nullptr;
    }
}

QString IssueReporter::repo() const { return QString::fromLatin1(kRepo); }

void IssueReporter::installMessageHandler()
{
    g_reporter = this;
    g_prevHandler = qInstallMessageHandler(messageHandler);
}

void IssueReporter::setStatus(const QString &s)
{
    if (m_status == s) {
        return;
    }
    m_status = s;
    emit statusChanged();
}

void IssueReporter::setEnabled(bool on)
{
    if (on == m_enabled) {
        return;
    }
    m_enabled = on;
    QSettings().setValue(QStringLiteral("issues/autoReport"), on);
    emit enabledChanged();
    if (on) {
        scheduleAutoSubmit();
    } else {
        m_autoTimer->stop();
    }
}

void IssueReporter::markAsked()
{
    if (m_asked) {
        return;
    }
    m_asked = true;
    QSettings().setValue(QStringLiteral("issues/asked"), true);
    emit askedChanged();
}

void IssueReporter::setToken(const QString &token)
{
    const QString t = token.trimmed();
    if (t == m_token) {
        return;
    }
    m_token = t;
    // 明文存在 QSettings 里（Windows 注册表 / mac plist / Linux conf）。这是用户自备的
    // 个人 PAT，建议只给 public_repo 这一个权限——UI 上也是这么写的。
    QSettings().setValue(QStringLiteral("issues/token"), m_token);
    emit tokenChanged();
    setStatus(m_token.isEmpty()
                      ? QString::fromUtf8("已清除令牌：上报改为打开浏览器、由你确认后提交。")
                      : QString::fromUtf8("已保存令牌：上报将直接提交到 issue 区。"));
}

// ———————————————————————————— 采集 ————————————————————————————

void IssueReporter::report(const QString &source, const QString &message)
{
    const QString trimmed = message.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    const QString clean = redact(trimmed);
    const QString fp = fingerprintOf(source, clean);

    ++m_captured;

    // 已经上报过（含历史会话）或已被用户丢弃 → 不再骚扰。
    if (m_seen.contains(fp)) {
        emit pendingChanged();
        return;
    }
    // 本批里已有同一个指纹 → 只累加次数。每轮扫描都失败的网关就属于这一类。
    for (Entry &e : m_pending) {
        if (e.fingerprint == fp) {
            ++e.repeats;
            e.when = QDateTime::currentDateTime();
            emit pendingChanged();
            return;
        }
    }
    if (m_pending.size() >= kMaxPending) {
        emit pendingChanged();
        return; // 雪崩了：这一批先提交完，多的下一批再说
    }

    Entry e;
    e.when = QDateTime::currentDateTime();
    e.source = source;
    e.message = clean;
    e.fingerprint = fp;
    m_pending.append(e);
    emit pendingChanged();
    emit pendingErrorArrived(clean.left(160));

    scheduleAutoSubmit();
}

void IssueReporter::scheduleAutoSubmit()
{
    if (!m_enabled || m_pending.isEmpty() || m_submitting || m_autoTimer->isActive()) {
        return;
    }
    m_autoTimer->start(kBatchDelayMs);
}

void IssueReporter::discard()
{
    for (const Entry &e : m_pending) {
        m_seen.insert(e.fingerprint); // 记为已处理，别下次又冒出来
    }
    m_pending.clear();
    saveSeen();
    m_autoTimer->stop();
    emit pendingChanged();
    setStatus(QString::fromUtf8("已忽略这些报错（同样的问题不会再提示）。"));
}

// ———————————————————————————— 脱敏 ————————————————————————————

QString IssueReporter::redact(const QString &text) const
{
    QString s = text;

    // 1) URL —— 订阅地址本身就是凭据，白名单外的连 host 一起抹掉。
    static const QRegularExpression reUrl(
            QStringLiteral("\\b([a-zA-Z][a-zA-Z0-9+.\\-]*)://([^\\s/\"'<>]+)([^\\s\"'<>]*)"));
    {
        QString out;
        int last = 0;
        auto it = reUrl.globalMatch(s);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += QStringView{s}.mid(last, m.capturedStart() - last);
            const QString host = m.captured(2);
            if (publicHost(host)) {
                out += m.captured(1) + QStringLiteral("://") + host;
                const QString path = m.captured(3);
                if (!path.isEmpty() && path != QStringLiteral("/")) {
                    out += QStringLiteral("/<path>");
                }
            } else {
                out += QStringLiteral("<url>");
            }
            last = m.capturedEnd();
        }
        out += QStringView{s}.mid(last);
        s = out;
    }

    // 2) token / 密码这类键值对。值的字符集排除了引号和各种右括号：用 \S 会把 "(secret=xxx)"
    //    的右括号一起吞掉，脱敏没错但读起来像截断了。
    static const QRegularExpression reSecret(
            QStringLiteral("(?i)\\b(token|secret|password|passwd|pwd|api[_-]?key|authorization|"
                           "bearer)\\b\\s*[:=]?\\s*[^\\s,;\"'()\\[\\]{}]{6,}"));
    s.replace(reSecret, QStringLiteral("\\1=<redacted>"));

    // 3) MAC 要排在 IP 之前：它也由十六进制段组成，先抹掉免得被后面的规则切碎。
    static const QRegularExpression reMac(
            QStringLiteral("\\b([0-9a-fA-F]{2}[:\\-]){5}[0-9a-fA-F]{2}\\b"));
    s.replace(reMac, QStringLiteral("<mac>"));

    // 4) 网卡 GUID（Windows 上报错里常带，能标识机器）。
    static const QRegularExpression reGuid(
            QStringLiteral("\\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                           "[0-9a-fA-F]{12}\\}?"));
    s.replace(reGuid, QStringLiteral("<guid>"));

    // 5) IPv4：回环、0.0.0.0 和掩码留着（排障要看，且不敏感），其余抹掉。
    static const QRegularExpression reIp(QStringLiteral("\\b\\d{1,3}(\\.\\d{1,3}){3}\\b"));
    {
        QString out;
        int last = 0;
        auto it = reIp.globalMatch(s);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString ip = m.captured(0);
            const bool keep = ip.startsWith(QStringLiteral("127."))
                    || ip.startsWith(QStringLiteral("255."))
                    || ip == QStringLiteral("0.0.0.0");
            out += QStringView{s}.mid(last, m.capturedStart() - last);
            out += keep ? ip : QStringLiteral("<ip>");
            last = m.capturedEnd();
        }
        out += QStringView{s}.mid(last);
        s = out;
    }

    // 6) 路径与身份：主目录、应用数据目录、用户名、主机名。
    //    先替换更长的路径，再替换用户名——否则用户名会先把路径里的那一段吃掉。
    const QStringList dirs = {m_config.configDir, m_config.userDir, QDir::homePath()};
    const QStringList tags = {QStringLiteral("<configDir>"), QStringLiteral("<userDir>"),
                              QStringLiteral("<home>")};
    for (int i = 0; i < dirs.size(); ++i) {
        const QString d = dirs.at(i);
        if (d.size() < 4) {
            continue;
        }
        s.replace(d, tags.at(i), Qt::CaseInsensitive);
        s.replace(QDir::toNativeSeparators(d), tags.at(i), Qt::CaseInsensitive);
    }
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("USER");
    }
    if (user.size() >= 3) {
        s.replace(user, QStringLiteral("<user>"), Qt::CaseInsensitive);
    }
    const QString hostName = QSysInfo::machineHostName();
    if (hostName.size() >= 3) {
        s.replace(hostName, QStringLiteral("<hostname>"), Qt::CaseInsensitive);
    }

    return s;
}

QString IssueReporter::fingerprintOf(const QString &source, const QString &redacted)
{
    QString norm = redacted.toLower();
    // 变动部分（端口、耗时、字节数、句柄、错误码…）归一，否则"同一个问题"会算成无数个。
    static const QRegularExpression reVar(
            QStringLiteral("0x[0-9a-f]+|\\b[0-9a-f]{8,}\\b|\\d+"));
    norm.replace(reVar, QStringLiteral("#"));
    static const QRegularExpression reWs(QStringLiteral("\\s+"));
    norm.replace(reWs, QStringLiteral(" "));
    const QByteArray h = QCryptographicHash::hash((source + QLatin1Char('|') + norm.trimmed()).toUtf8(),
                                                  QCryptographicHash::Sha1);
    return QString::fromLatin1(h.toHex().left(12));
}

// ———————————————————————————— 正文 ————————————————————————————

QString IssueReporter::environmentBlock() const
{
    return QStringLiteral("| 项 | 值 |\n|---|---|\n"
                          "| Coast | %1 |\n"
                          "| 系统 | %2 |\n"
                          "| 内核 | %3 %4 |\n"
                          "| 架构 | %5 |\n"
                          "| 语言 | %6 |\n"
                          "| Qt | %7 |\n")
            .arg(QString::fromUtf8(APP_VERSION), QSysInfo::prettyProductName(),
                 QSysInfo::kernelType(), QSysInfo::kernelVersion(),
                 QSysInfo::currentCpuArchitecture(), QLocale().name(),
                 QString::fromLatin1(qVersion()));
}

QString IssueReporter::buildTitle() const
{
    if (m_pending.isEmpty()) {
        return QStringLiteral("[auto] 运行时报错");
    }
    const Entry &first = m_pending.constFirst();
    QString head = first.message.section(QLatin1Char('\n'), 0, 0).simplified();
    if (head.size() > 90) {
        head = head.left(89) + QChar(0x2026);
    }
    QString title = QStringLiteral("[auto] %1: %2").arg(first.source, head);
    if (m_pending.size() > 1) {
        title += QStringLiteral(" (+%1)").arg(m_pending.size() - 1);
    }
    return title;
}

QString IssueReporter::buildBody() const
{
    QString b;
    b += QStringLiteral("> 本 issue 由 Coast 自动收集运行时报错生成，正文已在本机脱敏"
                        "（URL / IP / MAC / 网卡 GUID / 用户名与路径已替换为占位符）。\n"
                        "> `fingerprint` 行用于去重，请勿修改。\n\n");
    b += QStringLiteral("### 运行环境\n\n") + environmentBlock() + QLatin1Char('\n');
    b += QStringLiteral("### 报错（%1 条）\n\n").arg(m_pending.size());
    int i = 0;
    for (const Entry &e : m_pending) {
        ++i;
        b += QStringLiteral("#### %1. `%2` · %3")
                     .arg(QString::number(i), e.source,
                          e.when.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        if (e.repeats > 1) {
            b += QStringLiteral(" · 本会话重复 %1 次").arg(e.repeats);
        }
        b += QStringLiteral("\n\n```\n") + e.message + QStringLiteral("\n```\n");
        b += QStringLiteral("`fingerprint: %1`\n\n").arg(e.fingerprint);
    }
    return b;
}

QString IssueReporter::preview() const
{
    return buildTitle() + QStringLiteral("\n\n") + buildBody();
}

// ———————————————————————————— 去重台账 ————————————————————————————

QString IssueReporter::seenPath() const
{
    return QDir(m_config.configDir).filePath(QStringLiteral("issue-reported.json"));
}

void IssueReporter::loadSeen()
{
    QFile f(seenPath());
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QJsonArray arr = root.value(QStringLiteral("fingerprints")).toArray();
    for (const QJsonValue &v : arr) {
        const QString fp = v.toString();
        if (!fp.isEmpty()) {
            m_seen.insert(fp);
        }
    }
}

void IssueReporter::saveSeen()
{
    QDir().mkpath(m_config.configDir);
    QJsonArray arr;
    for (const QString &fp : m_seen) {
        arr.append(fp);
    }
    QJsonObject root;
    root.insert(QStringLiteral("fingerprints"), arr);
    root.insert(QStringLiteral("updated"), QDateTime::currentDateTime().toString(Qt::ISODate));
    QFile f(seenPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.close();
    AppConfig::makeWritable(seenPath());
}

// ———————————————————————————— 提交 ————————————————————————————

QNetworkAccessManager *IssueReporter::nam()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    // 与 UpdateController/NpcapInstaller 同策略：核心在跑就经混合端口出去（GitHub 被墙也能提交）。
    if (m_core && m_core->isRunning()) {
        m_nam->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_config.host,
                                      static_cast<quint16>(m_config.mixedPort)));
    } else {
        m_nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }
    return m_nam;
}

void IssueReporter::submit()
{
    if (m_pending.isEmpty() || m_submitting) {
        return;
    }
    m_autoTimer->stop();
    if (m_token.isEmpty()) {
        submitViaBrowser();
    } else {
        submitViaApi();
    }
}

void IssueReporter::submitViaBrowser()
{
    // 零密钥路径：把标题/正文预填进 issues/new，剩下的交给用户在浏览器里点 Submit。
    // 这样发出去的是**用户自己的** GitHub 身份，程序里也就不需要任何凭据。
    QString body = buildBody();
    if (body.size() > kMaxUrlBodyChars) {
        body = body.left(kMaxUrlBodyChars)
                + QStringLiteral("\n\n> …正文过长已截断，完整内容可在应用内「预览」查看。\n");
    }
    QUrl url(QStringLiteral("https://github.com/%1/issues/new").arg(QString::fromLatin1(kRepo)));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("title"), buildTitle());
    q.addQueryItem(QStringLiteral("body"), body);
    q.addQueryItem(QStringLiteral("labels"), QString::fromLatin1(kLabel));
    url.setQuery(q);

    if (!QDesktopServices::openUrl(url)) {
        setStatus(QString::fromUtf8("打不开浏览器，请手动到 issue 区提交（可先点「预览」复制正文）。"));
        return;
    }
    // 浏览器已打开、但用户是否真的点了 Submit 我们无从得知。仍然记为已处理：否则同一批
    // 报错会一直卡在队列里反复弹窗，比漏报一次更烦人。
    onSubmitted(true, QString::fromUtf8("已在浏览器打开预填好的 issue，请点 Submit 完成提交。"));
}

void IssueReporter::submitViaApi()
{
    m_submitting = true;
    setStatus(QString::fromUtf8("正在提交到 issue 区…"));
    emit statusChanged();

    // 先搜有没有同指纹的 issue：有就追加评论，别开重复的。
    const QString fp = m_pending.constFirst().fingerprint;
    QUrl url(QStringLiteral("https://api.github.com/search/issues"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"),
                   QStringLiteral("repo:%1 is:issue \"%2\"").arg(QString::fromLatin1(kRepo), fp));
    q.addQueryItem(QStringLiteral("per_page"), QStringLiteral("1"));
    url.setQuery(q);

    QNetworkRequest req{url};
    req.setRawHeader("User-Agent", "Coast-IssueReporter");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    req.setTransferTimeout(15000);

    QNetworkReply *reply = nam()->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        int number = 0;
        if (ok) {
            const QJsonObject r = QJsonDocument::fromJson(data).object();
            const QJsonArray items = r.value(QStringLiteral("items")).toArray();
            if (!items.isEmpty()) {
                number = items.first().toObject().value(QStringLiteral("number")).toInt();
            }
        }
        // 搜索失败（限流/权限不足）不算致命：退化成"直接建新 issue"。
        if (number > 0) {
            apiCommentOn(number);
        } else {
            apiCreateIssue();
        }
    });
}

void IssueReporter::apiCreateIssue()
{
    QJsonObject payload;
    payload.insert(QStringLiteral("title"), buildTitle());
    payload.insert(QStringLiteral("body"), buildBody());
    payload.insert(QStringLiteral("labels"), QJsonArray{QString::fromLatin1(kLabel)});

    QNetworkRequest req{QUrl(QStringLiteral("https://api.github.com/repos/%1/issues")
                                     .arg(QString::fromLatin1(kRepo)))};
    req.setRawHeader("User-Agent", "Coast-IssueReporter");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    req.setTransferTimeout(20000);

    QNetworkReply *reply = nam()->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const QByteArray data = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString err = reply->errorString();
        reply->deleteLater();
        if (!ok) {
            onSubmitted(false, QString::fromUtf8("提交失败：%1").arg(err));
            return;
        }
        const int number = QJsonDocument::fromJson(data).object()
                                   .value(QStringLiteral("number")).toInt();
        onSubmitted(true, QString::fromUtf8("已提交 issue #%1").arg(number));
    });
}

void IssueReporter::apiCommentOn(int issueNumber)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("body"),
                   QString::fromUtf8("又一次命中（自动上报）：\n\n") + buildBody());

    QNetworkRequest req{QUrl(QStringLiteral("https://api.github.com/repos/%1/issues/%2/comments")
                                     .arg(QString::fromLatin1(kRepo))
                                     .arg(issueNumber))};
    req.setRawHeader("User-Agent", "Coast-IssueReporter");
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("Content-Type", "application/json");
    req.setRawHeader("Authorization", QByteArray("Bearer ") + m_token.toUtf8());
    req.setTransferTimeout(20000);

    QNetworkReply *reply = nam()->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueNumber] {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString err = reply->errorString();
        reply->deleteLater();
        onSubmitted(ok,
                    ok ? QString::fromUtf8("已追加到已有 issue #%1").arg(issueNumber)
                       : QString::fromUtf8("提交失败：%1").arg(err));
    });
}

void IssueReporter::onSubmitted(bool ok, const QString &detail)
{
    m_submitting = false;
    if (ok) {
        m_submitted += m_pending.size();
        for (const Entry &e : m_pending) {
            m_seen.insert(e.fingerprint);
        }
        m_pending.clear();
        saveSeen();
        m_sinceSubmit.start();
        emit pendingChanged();
    }
    setStatus(detail);
    emit statusChanged();
}
