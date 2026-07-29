#include "AboutController.h"

#include "MmdbFile.h"
#include "Version.h" // APP_VERSION（由 CMake configure_file 生成，QmlBridge.cpp 同款包含）

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVector>

namespace {
// 与 Widgets 版 MainWindow.cpp 的 versionNewer() 同款：去掉非数字/点，逐段比较。
bool versionNewer(const QString &remote, const QString &local)
{
    auto parse = [](QString s) {
        // ★ 先砍掉第一个 '-' 之后的一切。测试版的 tag 形如 v0.1.94-beta.a1b2c3d：不砍的话
        //   下面那句「去掉非数字/点」会把短 sha 里的数字留下来（"0.1.94.123"），版本比较就
        //   变成拿提交哈希在比大小 —— 两个同版本号的 beta 谁新全看运气。正式版 tag 里没有
        //   '-'，这一刀对它无影响。
        const qsizetype dash = s.indexOf(QLatin1Char('-'));
        if (dash >= 0) {
            s.truncate(dash);
        }
        s.remove(QRegularExpression(QStringLiteral("[^0-9.]")));
        QVector<int> v;
        for (const QString &p : s.split('.', Qt::SkipEmptyParts)) {
            v << p.toInt();
        }
        return v;
    };
    const QVector<int> a = parse(remote);
    const QVector<int> b = parse(local);
    for (int i = 0; i < qMax(a.size(), b.size()); ++i) {
        const int x = i < a.size() ? a.at(i) : 0;
        const int y = i < b.size() ? b.at(i) : 0;
        if (x != y) {
            return x > y;
        }
    }
    return false;
}
} // namespace

AboutController::AboutController(AppConfig config, QObject *parent)
    : QObject(parent), m_config(std::move(config))
{
}

QString AboutController::currentVersion() const
{
    return QString::fromUtf8(APP_VERSION);
}

void AboutController::setChecking(bool v)
{
    if (m_checking == v) {
        return;
    }
    m_checking = v;
    emit checkingChanged();
}

void AboutController::check()
{
    if (m_checking) {
        return; // 检查中不重复发起
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    setChecking(true);

    // ★ 用 /releases 全量列表而不是 /releases/latest —— 后者按 GitHub 的定义**排除 prerelease**，
    //   开了「接收测试版」也永远看不到 beta。这里一次拉回来自己挑：正式版永远参与比较，
    //   prerelease 只在开关打开时参与。开关每次现读 AppConfig（而不是构造时的那份快照），
    //   免得用户刚在设置页打开、下一次小时检查还按旧值走。
    const bool wantBeta = AppConfigLoader::load().receiveBeta;
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.github.com/repos/ClashrAuto/clashauto/releases?per_page=20")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    // 组织/仓库改名会以 301 跳转——必须跟随，否则拿到空响应而「查不到版本」。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(10000);
#endif

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, wantBeta] {
        reply->deleteLater();
        setChecking(false);

        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isArray()) {
            // 出错时 GitHub 返的是对象（rate limit / 404），把它的 message 原样交给用户。
            const QString apiMsg = doc.object().value(QStringLiteral("message")).toString();
            emit checkFailed(apiMsg.isEmpty() ? QStringLiteral("未取到版本号") : apiMsg);
            return;
        }
        // 列表按发布时间倒序，但 tag 版本号才是权威（补发/改期都可能打乱时间序），所以逐条比。
        const QString local = QString::fromUtf8(APP_VERSION);
        QString tag;
        for (const QJsonValue &v : doc.array()) {
            const QJsonObject r = v.toObject();
            if (r.value(QStringLiteral("draft")).toBool()) {
                continue;
            }
            if (r.value(QStringLiteral("prerelease")).toBool() && !wantBeta) {
                continue; // 没开测试版：prerelease 当作不存在
            }
            const QString t = r.value(QStringLiteral("tag_name")).toString();
            if (t.isEmpty()) {
                continue;
            }
            if (tag.isEmpty() || versionNewer(t, tag)) {
                tag = t;
            }
        }
        if (tag.isEmpty()) {
            emit checkFailed(QStringLiteral("未取到版本号"));
            return;
        }

        if (m_latestVersion != tag) {
            m_latestVersion = tag;
            emit latestVersionChanged();
        }
        const bool newer = versionNewer(tag, local);
        if (m_updateAvailable != newer) {
            m_updateAvailable = newer;
            emit updateAvailableChanged();
        }
        if (!newer) {
            emit upToDate();
        }
    });
}

void AboutController::setCoreUpdateAvailable(bool v)
{
    if (m_coreUpdateAvailable == v) {
        return;
    }
    m_coreUpdateAvailable = v;
    emit coreUpdateAvailableChanged();
}

void AboutController::startAutoCheck()
{
    checkAll(); // 立即查一次
    if (m_autoTimer) {
        return; // 已在跑，避免重复启定时器
    }
    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(60 * 60 * 1000); // 每小时一次
    connect(m_autoTimer, &QTimer::timeout, this, [this] { checkAll(); });
    m_autoTimer->start();
}

void AboutController::checkAll()
{
    check();      // 程序（updateAvailable → "new" 角标）
    checkCore();  // 内核（coreUpdateAvailable → "core" 角标）
    checkGeoip(); // GeoIP（有新发布则静默下载）
}

void AboutController::checkCore()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    // 本地内核版本：核心存在则跑 `-v` 解析（无核心 = 无「可更新」，不置角标）。
    const QString exe = m_config.clashExecutable();
    if (!QFile::exists(exe)) {
        setCoreUpdateAvailable(false);
        return;
    }
    QString localCoreVer;
    {
        QProcess p;
#if defined(Q_OS_WIN)
        p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
            a->flags |= 0x08000000u; // CREATE_NO_WINDOW
        });
#endif
        p.start(exe, {QStringLiteral("-v")});
        if (p.waitForFinished(3000)) {
            const QRegularExpressionMatch m =
                QRegularExpression(QStringLiteral("\\bv\\d+[0-9A-Za-z.\\-]*"))
                    .match(QString::fromUtf8(p.readAllStandardOutput()));
            if (m.hasMatch()) {
                localCoreVer = m.captured(0);
            }
        } else {
            p.kill();
        }
    }
    if (localCoreVer.isEmpty()) {
        setCoreUpdateAvailable(false); // 版本探测失败，宁可不误报
        return;
    }

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(10000);
#endif
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, localCoreVer] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return; // 网络失败：保持原角标状态，不动
        }
        const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            return;
        }
        setCoreUpdateAvailable(versionNewer(tag, localCoreVer));
    });
}

void AboutController::checkGeoip()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    // 用 meta-rules-dat「最新发布时间(published_at)」当版本戳：与上次记录不同（或本地 mmdb 缺失）→ 有新数据。
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.github.com/repos/MetaCubeX/meta-rules-dat/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(10000);
#endif
    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
        const QString stamp = r.value(QStringLiteral("published_at")).toString();
        if (stamp.isEmpty()) {
            return;
        }
        QSettings st;
        const QString last = st.value(QStringLiteral("geoip/lastPublished")).toString();
        const QString target = QDir(m_config.userDir).filePath(QStringLiteral("Country.mmdb"));
        const bool haveLocal = QFile::exists(target);
        // 首次(无记录)且本地已有 mmdb → 只记基线、不下载（避免每次全新安装都触发一次下载）。
        if (last.isEmpty() && haveLocal) {
            st.setValue(QStringLiteral("geoip/lastPublished"), stamp);
            return;
        }
        // 记录一致且本地存在 → 无需下载。
        if (last == stamp && haveLocal) {
            return;
        }
        // 有新发布 / 本地缺失 → 静默下载 country.mmdb 到用户目录 + 资源目录。
        QNetworkRequest dreq(QUrl(QStringLiteral(
            "https://github.com/MetaCubeX/meta-rules-dat/releases/latest/download/country.mmdb")));
        dreq.setRawHeader("User-Agent", "clashauto-cpp");
        dreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        dreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        dreq.setTransferTimeout(30000);
#endif
        QNetworkReply *dl = m_nam->get(dreq);
        connect(dl, &QNetworkReply::finished, this, [this, dl, stamp] {
            dl->deleteLater();
            const QByteArray data = dl->readAll();
            if (dl->error() != QNetworkReply::NoError || data.isEmpty()) {
                return; // 后台静默：失败就下次再试，不打扰用户
            }
            // ★ 校验 + 暂存，不碰线上库（理由见 MmdbFile.h）。这条路径是**启动时静默**跑的，
            //   一旦写坏用户完全无感 —— 真机上就是这么在 2026-07-29 09:37 把 GEOIP 弄失效的。
            //   所以这里比手动那条更需要校验：失败只记日志、不推进版本戳，下次发布再试。
            const QString target = QDir(m_config.userDir).filePath(QStringLiteral("Country.mmdb"));
            QDir().mkpath(QFileInfo(target).absolutePath());
            QString why;
            if (!MmdbFile::stage(data, target, &why)) {
                qWarning().noquote()
                    << "GeoIP 静默更新：下载到的库校验不通过，已保留原有数据库 —" << why;
                return; // 不写 lastPublished：这个版本还没成功装上，下次启动应当再试
            }
            QSettings().setValue(QStringLiteral("geoip/lastPublished"), stamp);
        });
    });
}
