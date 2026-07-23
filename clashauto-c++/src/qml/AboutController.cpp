#include "AboutController.h"

#include "Version.h" // APP_VERSION（由 CMake configure_file 生成，QmlBridge.cpp 同款包含）

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/ClashrAuto/clashauto/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    // 组织/仓库改名会以 301 跳转——必须跟随，否则拿到空响应而「查不到版本」。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(10000);
#endif

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        setChecking(false);

        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        const QString local = QString::fromUtf8(APP_VERSION);
        if (tag.isEmpty()) {
            const QString apiMsg = r.value(QStringLiteral("message")).toString();
            emit checkFailed(apiMsg.isEmpty() ? QStringLiteral("未取到版本号") : apiMsg);
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
            const QStringList targets = {QDir(m_config.userDir).filePath(QStringLiteral("Country.mmdb")),
                                         QDir(m_config.sourceRoot).filePath(QStringLiteral("config/Country.mmdb"))};
            bool anySaved = false;
            for (const QString &path : targets) {
                QDir().mkpath(QFileInfo(path).absolutePath());
                QFile out(path);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    out.write(data);
                    out.close();
                    anySaved = true;
                }
            }
            if (anySaved) {
                QSettings().setValue(QStringLiteral("geoip/lastPublished"), stamp);
            }
        });
    });
}
