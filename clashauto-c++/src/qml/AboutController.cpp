#include "CoreRelease.h"
#include "AboutController.h"

#include "../VersionManifest.h"

#include "MmdbFile.h"
#include "Version.h" // APP_VERSION（由 CMake configure_file 生成，QmlBridge.cpp 同款包含）

#include <QDateTime>
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
    // 手动检查必须真取一次，不吃 30 秒缓存。
    VersionManifest::invalidateCache();

    // ★ 数据源是 CI 维护的 version.json，**不是 GitHub API** —— 未登录的 API 是 60 次/
    //   小时按出口 IP 算，机场出口后面几百人共用，限流几乎必中，而它的报错读起来像网络
    //   故障。清单走的是 release 资源的 CDN，没有配额。详见 VersionManifest.h。
    //
    // 开关每次现读 AppConfig（而不是构造时的那份快照），免得用户刚在设置页打开、
    // 下一次小时检查还按旧值走。
    const bool wantBeta = AppConfigLoader::load().receiveBeta;

    VersionManifest::fetch(m_nam, this, [this, wantBeta](const QJsonDocument &doc,
                                                        const QString &err) {
        setChecking(false);
        if (doc.isNull()) {
            emit checkFailed(err.isEmpty() ? QStringLiteral("未取到版本清单") : err);
            return;
        }

        // ★ **比的是「本平台能装到的版本」，不是 release 的 tag。**
        //   同一个 tag 上各平台的包是陆续到的（mac 的 DMG 由外部签名仓库异步追加），
        //   按 tag 比会让「角标说有新版、点进去没有可下的包」——比不提示更糟。
        QString ver = VersionManifest::versionForThisPlatform(doc, QStringLiteral("release"));
        QString tag = doc.object().value(QStringLiteral("releases")).toObject()
                              .value(QStringLiteral("release")).toObject()
                              .value(QStringLiteral("tag")).toString();
        if (wantBeta) {
            // 开了测试版：两条通道里取更新的那个。正式版永远参与比较 —— 不然
            // 正式版反超时（beta 分支停更）用户会被钉死在旧 beta 上。
            const QString bver =
                    VersionManifest::versionForThisPlatform(doc, QStringLiteral("prerelease"));
            if (!bver.isEmpty() && (ver.isEmpty() || versionNewer(bver, ver))) {
                ver = bver;
                tag = doc.object().value(QStringLiteral("releases")).toObject()
                              .value(QStringLiteral("prerelease")).toObject()
                              .value(QStringLiteral("tag")).toString();
            }
        }
        if (ver.isEmpty()) {
            emit checkFailed(QStringLiteral("清单里没有适配本平台的安装包"));
            return;
        }

        if (m_latestVersion != tag) {
            m_latestVersion = tag;
            emit latestVersionChanged();
        }
        const bool newer = versionNewer(ver, QString::fromUtf8(APP_VERSION));
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

void AboutController::setCoreChecking(bool v)
{
    if (m_coreChecking == v) {
        return;
    }
    m_coreChecking = v;
    emit coreCheckingChanged();
}

void AboutController::checkCore()
{
    if (m_coreChecking) {
        return; // 检查中不重复发起（与 check() 同一条规矩）
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    setCoreChecking(true);
    // 本地内核版本：核心存在则跑 `-v` 解析（无核心 = 无「可更新」，不置角标）。
    const QString exe = m_config.clashExecutable();
    if (!QFile::exists(exe)) {
        setCoreUpdateAvailable(false);
        setCoreChecking(false);
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
        setCoreChecking(false);
        return;
    }

    // 内核版本也在清单里（见 VersionManifest.h）——同样不打 API。
    VersionManifest::fetch(m_nam, this, [this, localCoreVer](const QJsonDocument &doc,
                                                             const QString &) {
        setCoreChecking(false);
        if (doc.isNull()) {
            return; // 取不到：保持原角标状态，不动
        }
        // 按当前通道（是否接收测试版）挑，版本嵌在产物名里而不是 tag 上。
        // 清单被翻译成 GitHub releases 的形状后原样交给 CoreRelease::pick —— 按 CPU 特性
        // 分 v1/v2/v3/compatible 那套规则一行都没重写（重写就意味着两份逻辑要长期一致）。
        const bool wantBeta = AppConfigLoader::load().receiveBeta;
        const CoreRelease::Pick pick =
                CoreRelease::pick(VersionManifest::coreReleases(doc), wantBeta);
        if (!pick.isValid()) {
            return;
        }
        setCoreUpdateAvailable(CoreRelease::hasUpdate(pick.version, localCoreVer));
    });
}

void AboutController::checkGeoip()
{
    // 用户在设置页选的自动更新周期（天）。0 = 关，一次都不查。
    //
    // ★ 以前这里没有任何节流：checkAll 每小时跑一次，就跟着问一次清单，用户没有
    //   任何开关能关掉它。周期由 config.yaml 的 `geoipUpdate` 决定，节流点放在
    //   **发请求之前** —— 放在「有没有新版」之后的话，周期设成一个月也照样每小时联一次网。
    const int days = AppConfigLoader::load().geoipUpdateDays;
    if (days <= 0) {
        return;
    }
    QSettings throttle;
    const QDateTime lastCheck =
        throttle.value(QStringLiteral("geoip/lastCheck")).toDateTime();
    if (lastCheck.isValid() && lastCheck.secsTo(QDateTime::currentDateTime()) < qint64(days) * 86400) {
        return;
    }
    throttle.setValue(QStringLiteral("geoip/lastCheck"), QDateTime::currentDateTime());

    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    // 用 meta-rules-dat「最新发布时间(published_at)」当版本戳：与上次记录不同（或本地 mmdb 缺失）→ 有新数据。
    // GeoIP 的发布时间戳同样取自清单（CI 那边替我们问过 meta-rules-dat 了）。
    // 下载地址仍是上游的固定直链 —— 那是资源下载，本来就没有 API 配额问题。
    VersionManifest::fetch(m_nam, this, [this](const QJsonDocument &doc, const QString &) {
        if (doc.isNull()) {
            return;
        }
        const QString stamp = VersionManifest::geoipPublished(doc);
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
