#include "UpdateController.h"

#include "CoreController.h"
#include "Version.h" // APP_VERSION

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>
#include <QVector>

#if defined(Q_OS_WIN)
#  include <windows.h>
#  include <shellapi.h>
#endif

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

// 人类可读字节数（用于下载量/总量/速度显示）：1023 -> "1023 B"，1536 -> "1.5 KB"。
QString fmtBytes(qint64 v)
{
    double n = v < 0 ? 0 : static_cast<double>(v);
    static const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) {
        n /= 1024.0;
        ++i;
    }
    return QString::number(n, 'f', i == 0 ? 0 : 1) + QLatin1Char(' ') + QLatin1String(u[i]);
}
} // namespace

UpdateController::UpdateController(AppConfig config, CoreController *core, QObject *parent)
    : QObject(parent), m_config(std::move(config)), m_core(core)
{
}

QString UpdateController::currentVersion() const
{
    return QString::fromUtf8(APP_VERSION);
}

void UpdateController::setChecking(bool v)
{
    if (m_checking == v) {
        return;
    }
    m_checking = v;
    emit checkingChanged();
}

void UpdateController::setDownloading(bool v)
{
    if (m_downloading == v) {
        return;
    }
    m_downloading = v;
    emit downloadingChanged();
}

void UpdateController::setProgress(int v)
{
    if (m_progress == v) {
        return;
    }
    m_progress = v;
    emit progressChanged();
}

void UpdateController::setStatus(const QString &s)
{
    if (m_status == s) {
        return;
    }
    m_status = s;
    emit statusChanged();
}

void UpdateController::applyDownloadProxy(QNetworkAccessManager *nam) const
{
    // 核心在跑 = 有可用代理：把该 QNAM 的请求经混合端口转发，GitHub 等被墙资源也能下到。
    // 核心没跑就保持直连（对齐 MainWindow::applyDownloadProxy）。
    if (nam && m_core && m_core->isRunning()) {
        nam->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, m_config.host,
                                    static_cast<quint16>(m_config.mixedPort)));
    }
}

void UpdateController::refresh()
{
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    applyDownloadProxy(m_nam);
    fetchReleases();
    fetchCore();
}

int UpdateController::recommendedIndex(int tab) const
{
    const QVector<Asset> &assets = tab == 1 ? m_betaAssets : m_releaseAssets;
    if (assets.isEmpty()) {
        return -1;
    }
    // 各平台优先推荐「可一键安装」的资源，且必须**确定性**选取——不能只 return 0，因为
    // GitHub 资源顺序 = 上传顺序，Linux 上 0 号常是 .tar.gz，「一键更新」就变成下压缩包 →
    // 弹归档管理器 → 退出，什么都没装（即 Linux「更新错乱」的根因）。按扩展名精确择优：
    //   Windows → NSIS 安装器（名含 setup 的 .exe，支持 /S 静默安装 + 自动重启）
    //   Linux   → .deb（openUrl 交系统包管理器安装）
    //   macOS   → .dmg（launchSilentUpdateAndRestartMac 覆盖 .app + 自动重启）
    auto firstMatch = [&assets](auto pred) -> int {
        for (int i = 0; i < assets.size(); ++i) {
            if (pred(assets.at(i).name)) {
                return i;
            }
        }
        return -1;
    };
#if defined(Q_OS_WIN)
    // 更新页改造：程序更新统一走「下载 portable zip → 解压覆盖到项目目录 → 重启」，故
    // 优先便携版 zip（名含 portable 的 .zip）→ 任意 .zip → 退回 NSIS 安装器（setup.exe / .exe）。
    int idx = firstMatch([](const QString &n) {
        return n.contains(QStringLiteral("portable"), Qt::CaseInsensitive)
               && n.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive);
    });
    if (idx < 0) {
        idx = firstMatch([](const QString &n) { return n.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive); });
    }
    if (idx < 0) {
        idx = firstMatch([](const QString &n) {
            return n.contains(QStringLiteral("setup"), Qt::CaseInsensitive)
                   && n.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
        });
    }
    if (idx < 0) {
        idx = firstMatch([](const QString &n) { return n.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive); });
    }
#elif defined(Q_OS_MACOS)
    int idx = firstMatch([](const QString &n) { return n.endsWith(QStringLiteral(".dmg"), Qt::CaseInsensitive); });
#else
    int idx = firstMatch([](const QString &n) { return n.endsWith(QStringLiteral(".deb"), Qt::CaseInsensitive); });
#endif
    return idx >= 0 ? idx : 0;
}

void UpdateController::fetchReleases()
{
    // 资源过滤（对齐旧项目 formatAssets）：仅当前平台+架构；命名 windows/macos/linux + x64/arm64（mac universal 通吃）。
    const QString plat =
#if defined(Q_OS_WIN)
        QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
        QStringLiteral("macos");
#else
        QStringLiteral("linux");
#endif
    const QString arch = QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))
                             ? QStringLiteral("arm64")
                             : QStringLiteral("x64");

    setChecking(true);
    m_releaseNotes = QString::fromUtf8("正在获取...");
    m_betaNotes = QString::fromUtf8("正在获取...");
    emit releaseChanged();
    emit betaChanged();

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/ClashrAuto/clashauto/releases")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "clashauto-cpp");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(15000);
#endif

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, plat, arch] {
        reply->deleteLater();
        setChecking(false);
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray body = reply->readAll();
        if (!ok) {
            const QString apiMsg = QJsonDocument::fromJson(body).object().value(QStringLiteral("message")).toString();
            const QString failText = QString::fromUtf8("获取失败: ")
                                     + (apiMsg.isEmpty() ? reply->errorString() : apiMsg);
            m_releaseNotes = failText;
            m_betaNotes = failText;
            emit releaseChanged();
            emit betaChanged();
            return;
        }
        const QJsonArray arr = QJsonDocument::fromJson(body).array();

        auto fill = [this, plat, arch](const QJsonObject &r, bool beta) {
            QString &verOut = beta ? m_betaVersion : m_releaseVersion;
            QString &notesOut = beta ? m_betaNotes : m_releaseNotes;
            QVector<Asset> &assetsOut = beta ? m_betaAssets : m_releaseAssets;
            QStringList &namesOut = beta ? m_betaAssetNames : m_releaseAssetNames;

            // VERSION 去字母（v0.1.81 → 0.1.81），对齐旧项目。
            const QString tag = r.value(QStringLiteral("tag_name")).toString();
            verOut = QString::fromUtf8("VERSION: ")
                     + QString(tag).remove(QRegularExpression(QStringLiteral("[A-Za-z]")));
            const QString notes = r.value(QStringLiteral("body")).toString();
            notesOut = notes.trimmed().isEmpty() ? QString::fromUtf8("（此版本未附更新说明）") : notes;

            assetsOut.clear();
            namesOut.clear();
            const QJsonArray assets = r.value(QStringLiteral("assets")).toArray();
            // 先全量登记 name → url（含 .sha256 边车），供下载后按 <资源名>.sha256 查边车校验。
            QHash<QString, QString> urlByName;
            for (const QJsonValue &av : assets) {
                const QJsonObject a = av.toObject();
                urlByName.insert(a.value(QStringLiteral("name")).toString(),
                                 a.value(QStringLiteral("browser_download_url")).toString());
            }
            for (const QJsonValue &av : assets) {
                const QJsonObject a = av.toObject();
                const QString name = a.value(QStringLiteral("name")).toString();
                if (!name.contains(plat, Qt::CaseInsensitive)
                    || !(name.contains(arch, Qt::CaseInsensitive)
                         || name.contains(QStringLiteral("universal"), Qt::CaseInsensitive))) {
                    continue;
                }
                if (name.endsWith(QStringLiteral(".yml"), Qt::CaseInsensitive)
                    || name.endsWith(QStringLiteral(".yaml"), Qt::CaseInsensitive)
                    || name.endsWith(QStringLiteral(".sha256"), Qt::CaseInsensitive)) {
                    continue; // 忽略 metadata 与校验边车（边车仅供按名查表校验，不作可下载项展示）
                }
                Asset item;
                item.name = name;
                item.url = a.value(QStringLiteral("browser_download_url")).toString();
                item.sha256Url = urlByName.value(name + QStringLiteral(".sha256"));
                assetsOut.append(item);
                namesOut.append(name);
            }
        };

        bool haveRel = false;
        bool haveBeta = false;
        for (const QJsonValue &value : arr) {
            const QJsonObject r = value.toObject();
            if (!r.value(QStringLiteral("prerelease")).toBool() && !haveRel) {
                fill(r, false);
                haveRel = true;
            } else if (r.value(QStringLiteral("prerelease")).toBool() && !haveBeta) {
                fill(r, true);
                haveBeta = true;
            }
            if (haveRel && haveBeta) {
                break;
            }
        }
        if (!haveRel) {
            m_releaseNotes = QString::fromUtf8("暂无正式版");
        }
        if (!haveBeta) {
            m_betaNotes = QString::fromUtf8("暂无测试版");
        }
        emit releaseChanged();
        emit betaChanged();
    });
}

void UpdateController::fetchCore()
{
    // 本地版本用 `mihomo -v` 探测（无内核则「未安装」）；最新版走 GitHub API。
    QString localCoreVer;
    bool coreInstalled = false;
    {
        const QString exe = m_config.clashExecutable();
        if (QFile::exists(exe)) {
            coreInstalled = true;
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
    }
    const QString localShown = !coreInstalled ? QString::fromUtf8("未安装")
                               : localCoreVer.isEmpty() ? QString::fromUtf8("未知")
                                                        : localCoreVer;
    m_coreVersion = QString::fromUtf8("内核版本: %1（正在查询最新版...）").arg(localShown);
    m_coreNotes = QString::fromUtf8("正在获取...");
    emit coreChanged();

    QNetworkRequest coreReq(QUrl(QStringLiteral("https://api.github.com/repos/MetaCubeX/mihomo/releases/latest")));
    coreReq.setRawHeader("Accept", "application/vnd.github+json");
    coreReq.setRawHeader("User-Agent", "clashauto-cpp");
    coreReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    coreReq.setTransferTimeout(10000);
#endif
    QNetworkReply *coreReply = m_nam->get(coreReq);
    connect(coreReply, &QNetworkReply::finished, this, [this, coreReply, localCoreVer, localShown] {
        coreReply->deleteLater();
        const bool ok = coreReply->error() == QNetworkReply::NoError;
        const QByteArray body = coreReply->readAll();
        const QString err = coreReply->errorString();
        if (!ok) {
            m_coreVersion = QString::fromUtf8("内核版本: %1（查询最新版失败）").arg(localShown);
            m_coreNotes = QString::fromUtf8("获取失败: ") + err;
            emit coreChanged();
            return;
        }
        const QJsonObject r = QJsonDocument::fromJson(body).object();
        const QString tag = r.value(QStringLiteral("tag_name")).toString();
        if (tag.isEmpty()) {
            const QString apiMsg = r.value(QStringLiteral("message")).toString();
            m_coreVersion = QString::fromUtf8("内核版本: %1（查询最新版失败）").arg(localShown);
            m_coreNotes = QString::fromUtf8("获取失败: 未取到版本号%1")
                              .arg(apiMsg.isEmpty() ? QString() : QString::fromUtf8("（%1）").arg(apiMsg));
            emit coreChanged();
            return;
        }
        const bool newer = localCoreVer.isEmpty() || versionNewer(tag, localCoreVer);
        m_coreVersion = QString::fromUtf8("内核版本: %1 → 最新 %2 %3")
                            .arg(localShown, tag,
                                 newer ? QString::fromUtf8("（可更新）") : QString::fromUtf8("（已是最新）"));
        const QString notes = r.value(QStringLiteral("body")).toString();
        m_coreNotes = notes.trimmed().isEmpty() ? QString::fromUtf8("（此版本未附更新说明）") : notes;
        emit coreChanged();
    });
}

void UpdateController::oneClickUpdate(int tab, int index, bool useMirror)
{
    if (m_downloading) {
        return;
    }
    const QVector<Asset> &assets = tab == 1 ? m_betaAssets : m_releaseAssets;
    if (index < 0 || index >= assets.size()) {
        return;
    }
    const Asset asset = assets.at(index);
    if (asset.url.isEmpty()) {
        return;
    }
    if (!m_nam) {
        m_nam = new QNetworkAccessManager(this);
    }
    applyDownloadProxy(m_nam);

    const QString name = asset.name;
    const QString sidecarUrl = asset.sha256Url;
    // 国内代理下载：给下载链接加国内镜像前缀，并用「直连」的 QNAM 下载（镜像国内可直达，绝不套节点代理）。
    const QString downloadUrl = useMirror ? QStringLiteral("https://ghfast.top/") + asset.url : asset.url;
    QNetworkAccessManager *dlNam = m_nam;
    if (useMirror) {
        dlNam = new QNetworkAccessManager(this);
        dlNam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty()) {
        dir = QDir::tempPath();
    }
    const QString savePath = QDir(dir).filePath(name);

    QNetworkRequest dreq{QUrl(downloadUrl)};
    dreq.setRawHeader("User-Agent", "clashauto-cpp");
    dreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    // 关掉 HTTP/2：大文件经代理隧道到 GitHub CDN 常「下一点就卡死」，强制 HTTP/1.1 更稳。
    dreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    dreq.setTransferTimeout(30000);
#endif

    QNetworkReply *reply = dlNam->get(dreq);
    // 记住当前下载 reply + 复位计速状态（供 cancelDownload / 速度计算用）。
    m_dlReply = reply;
    m_cancelled = false;
    m_dlTimer.start();
    m_lastMs = 0;
    m_lastBytes = 0;
    m_speedText.clear();
    m_downloadedText.clear();
    m_totalText.clear();
    emit downloadStatsChanged();
    // 边下边写到磁盘：out 挂在 reply 名下，取消/关窗随 reply 一起销毁。
    auto *out = new QFile(savePath);
    out->setParent(reply);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        setStatus(QString::fromUtf8("无法写入临时文件：%1").arg(savePath));
        emit failed(QString::fromUtf8("无法写入临时文件：%1").arg(savePath));
        return;
    }
    setDownloading(true);
    setProgress(0);
    setStatus(QString::fromUtf8("开始下载: %1%2").arg(name, useMirror ? QString::fromUtf8("（国内代理）") : QString()));

    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            setProgress(static_cast<int>(received * 100 / total));
        }
        // 已下载 / 总量文本（总量未知时显示 "?"）。
        m_downloadedText = fmtBytes(received);
        m_totalText = total > 0 ? fmtBytes(total) : QString::fromUtf8("?");
        // 速度：每 ≥500ms 采样一次，避免抖动。
        const qint64 nowMs = m_dlTimer.elapsed();
        const qint64 dt = nowMs - m_lastMs;
        if (dt >= 500) {
            const qint64 db = received - m_lastBytes;
            const double bps = db * 1000.0 / static_cast<double>(dt);
            m_speedText = fmtBytes(static_cast<qint64>(bps)) + QStringLiteral("/s");
            m_lastMs = nowMs;
            m_lastBytes = received;
        }
        emit downloadStatsChanged();
    });
    connect(reply, &QNetworkReply::readyRead, this, [reply, out] {
        out->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, out, savePath, name, sidecarUrl, useMirror] {
        out->write(reply->readAll());
        out->close();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString err = reply->errorString();
        const bool cancelled = m_cancelled;
        m_dlReply = nullptr; // reply 即将 deleteLater，清引用防悬空
        reply->deleteLater(); // 连带删除 out
        // 用户主动取消：删临时文件、复位状态，不弹「失败」。
        if (cancelled) {
            QFile::remove(savePath);
            m_cancelled = false;
            setDownloading(false);
            setProgress(0);
            setStatus(QString::fromUtf8("已取消下载"));
            return;
        }
        if (!ok) {
            QFile::remove(savePath);
            setDownloading(false);
            setStatus(QString::fromUtf8("下载失败: %1 (%2)").arg(name, err));
            emit failed(QString::fromUtf8("下载失败：%1\n\n若在墙内，可勾选「国内代理下载」走国内镜像，或启动核心以经代理下载后重试。").arg(err));
            return;
        }
        setProgress(100);

        // 边车缺失 / 取不到校验文件时的处置：镜像下载不可信 → 拒绝清理；直连老版本可能本就没边车 → 跳过校验。
        auto onSidecarUnavailable = [this, savePath, name, useMirror] {
            if (useMirror) {
                QFile::remove(savePath);
                setDownloading(false);
                setStatus(QString::fromUtf8("更新已取消: 经镜像下载但未取到校验文件（.sha256），无法校验完整性"));
                emit failed(QString::fromUtf8("无法校验完整性（未取到校验文件），经镜像下载已取消，请关闭「国内代理下载」后重试。"));
                return;
            }
            setStatus(QString::fromUtf8("未取到校验文件，跳过完整性校验（直连）"));
            doExecute(savePath, name);
        };

        if (sidecarUrl.isEmpty()) {
            onSidecarUnavailable();
            return;
        }

        // 完整性校验：直连（复用已 applyDownloadProxy 的 m_nam，绝不加镜像前缀）下载官方边车比对。
        setStatus(QString::fromUtf8("正在校验安装包完整性..."));
        QNetworkRequest sreq{QUrl(sidecarUrl)};
        sreq.setRawHeader("User-Agent", "clashauto-cpp");
        sreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        sreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        sreq.setTransferTimeout(30000);
#endif
        QNetworkReply *sreply = m_nam->get(sreq);
        connect(sreply, &QNetworkReply::finished, this,
                [this, sreply, savePath, name, onSidecarUnavailable] {
            const bool sok = sreply->error() == QNetworkReply::NoError;
            const QByteArray sdata = sreply->readAll();
            sreply->deleteLater();
            if (!sok || sdata.trimmed().isEmpty()) {
                setStatus(QString::fromUtf8("校验文件下载失败"));
                onSidecarUnavailable();
                return;
            }
            if (verifySha256(savePath, QString::fromUtf8(sdata))) {
                setStatus(QString::fromUtf8("安装包完整性校验通过"));
                doExecute(savePath, name);
            } else {
                QFile::remove(savePath);
                setDownloading(false);
                setStatus(QString::fromUtf8("安装包校验失败，可能被篡改，已取消"));
                emit failed(QString::fromUtf8("安装包校验失败，可能被篡改，已取消。"));
            }
        });
    });
}

void UpdateController::doExecute(const QString &savePath, const QString &name)
{
#if defined(Q_OS_WIN)
    // 便携版 zip：解压覆盖到项目目录（保留已下载内核）→ 自动重启。
    if (name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        setStatus(QString::fromUtf8("下载完成，正在解压更新并自动重启: %1").arg(savePath));
        launchZipUpdateAndRestart(savePath);
        return;
    }
    // NSIS 安装包支持 /S 静默安装——等本进程退出后原地升级并自动重启。
    if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        setStatus(QString::fromUtf8("下载完成，正在静默安装并自动重启: %1").arg(savePath));
        launchSilentUpdateAndRestart(savePath);
        return;
    }
#elif defined(Q_OS_MACOS)
    // 一键更新：挂载 DMG → ditto 覆盖当前 .app → 卸载 → 自动重启。
    if (name.endsWith(QStringLiteral(".dmg"), Qt::CaseInsensitive)) {
        setStatus(QString::fromUtf8("下载完成，正在安装并自动重启: %1").arg(savePath));
        launchSilentUpdateAndRestartMac(savePath);
        return;
    }
#endif
    setStatus(QString::fromUtf8("下载完成，启动安装并退出: %1").arg(savePath));
    QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
    qApp->quit();
}

bool UpdateController::verifySha256(const QString &filePath, const QString &expectedHexLower) const
{
    QString expected = expectedHexLower.trimmed();
    const qsizetype ws = expected.indexOf(QRegularExpression(QStringLiteral("\\s")));
    if (ws > 0) {
        expected = expected.left(ws);
    }
    if (expected.isEmpty()) {
        return false;
    }
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        return false;
    }
    return QString::fromLatin1(h.result().toHex()).compare(expected, Qt::CaseInsensitive) == 0;
}

void UpdateController::skipCurrentRelease()
{
    const QString t = m_releaseVersion;
    const QString tag = t.startsWith(QStringLiteral("VERSION: ")) ? t.mid(9).trimmed() : QString();
    if (!tag.isEmpty() && tag != QStringLiteral("-")) {
        QSettings().setValue(QStringLiteral("update/skipTag"), tag);
    }
}

void UpdateController::cancelDownload()
{
    if (!m_downloading || !m_dlReply) {
        return;
    }
    // 标记为主动取消后 abort：finished 里据 m_cancelled 走「已取消」分支（删临时文件、不弹失败）。
    m_cancelled = true;
    m_dlReply->abort();
}

#if defined(Q_OS_WIN)
void UpdateController::launchSilentUpdateAndRestart(const QString &installerPath)
{
    // 交给隐藏的 PowerShell 守护进程：等本进程退出 → NSIS 静默安装（/S，/D= 装回当前目录）→ 重启。
    QDir exeDir(QCoreApplication::applicationDirPath());
    QString installRoot;
    if (exeDir.dirName() == QStringLiteral("clashauto-c++")) {
        QDir root = exeDir;
        root.cdUp();
        installRoot = root.absolutePath();
    } else {
        installRoot = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(QStringLiteral("ClashAuto"));
    }
    // 重启新装的应用：产物统一名为 clashauto.exe（去掉历史 -qml/-cpp 后缀）。用当前进程的
    // 可执行名动态取，避免再因二进制改名而重启到不存在的 exe（旧代码写死 clashauto-cpp.exe，
    // QML 版产物却是 clashauto-qml.exe → 更新后无法自动重启，即 Windows「更新错乱」的根因）。
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const QString newExe = QDir(installRoot).filePath(QStringLiteral("clashauto-c++/") + exeName);
    const auto psq = [](const QString &s) {
        QString r = QDir::toNativeSeparators(s);
        r.replace(QLatin1Char('\''), QStringLiteral("''"));
        return r;
    };
    const QString script = QStringLiteral(
        "Wait-Process -Id %1 -Timeout 60 -ErrorAction SilentlyContinue\n"
        "$psi = New-Object System.Diagnostics.ProcessStartInfo\n"
        "$psi.FileName = '%2'\n"
        "$psi.Arguments = '/S /D=%3'\n"
        "$psi.UseShellExecute = $false\n"
        "[System.Diagnostics.Process]::Start($psi).WaitForExit()\n"
        "Start-Process -FilePath '%4'\n")
        .arg(QString::number(QCoreApplication::applicationPid()),
             psq(installerPath), psq(installRoot), psq(newExe));
    const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()),
                           static_cast<qsizetype>(script.size()) * 2);
    const QString params = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ")
        + QString::fromLatin1(utf16.toBase64());
    const std::wstring fileW = L"powershell.exe";
    const std::wstring paramsW2 = params.toStdWString();

    SHELLEXECUTEINFOW sei2{};
    sei2.cbSize = sizeof(sei2);
    sei2.fMask = SEE_MASK_NOASYNC;
    sei2.lpFile = fileW.c_str();
    sei2.lpParameters = paramsW2.c_str();
    sei2.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei2)) {
        qApp->quit();
    } else {
        setStatus(QString::fromUtf8("自动安装启动失败（错误码 %1），已打开安装包请手动安装").arg(GetLastError()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(installerPath));
        qApp->quit();
    }
}

void UpdateController::launchZipUpdateAndRestart(const QString &zipPath)
{
    // 便携版 zip 更新：隐藏的 PowerShell 守护进程等本进程退出 → 解压 zip →
    // robocopy 覆盖到项目根目录（/E 合并、不带 /PURGE，故保留 command\clash 下已下载的内核）→ 重启。
    // 目录布局与安装器一致：<root>\clashauto-c++\<exe> + <root>\Clashr-Auto\...，而 zip 根内即这两个目录。
    QDir exeDir(QCoreApplication::applicationDirPath());
    QString installRoot;
    if (exeDir.dirName() == QStringLiteral("clashauto-c++")) {
        QDir root = exeDir;
        root.cdUp();
        installRoot = root.absolutePath();
    } else {
        installRoot = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(QStringLiteral("ClashAuto"));
    }
    const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const QString newExe = QDir(installRoot).filePath(QStringLiteral("clashauto-c++/") + exeName);
    const QString extractDir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("clashauto-zip-update"));
    const auto psq = [](const QString &s) {
        QString r = QDir::toNativeSeparators(s);
        r.replace(QLatin1Char('\''), QStringLiteral("''"));
        return r;
    };
    const QString script = QStringLiteral(
        "Wait-Process -Id %1 -Timeout 60 -ErrorAction SilentlyContinue\n"
        "$zip = '%2'\n"
        "$extract = '%3'\n"
        "$root = '%4'\n"
        "if (Test-Path $extract) { Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue }\n"
        "Add-Type -AssemblyName System.IO.Compression.FileSystem\n"
        "[System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $extract)\n"
        "robocopy $extract $root /E /NFL /NDL /NJH /NJS /NP | Out-Null\n"
        "Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue\n"
        "Remove-Item -Force $zip -ErrorAction SilentlyContinue\n"
        "Start-Process -FilePath '%5'\n")
        .arg(QString::number(QCoreApplication::applicationPid()),
             psq(zipPath), psq(extractDir), psq(installRoot), psq(newExe));
    const QByteArray utf16(reinterpret_cast<const char *>(script.utf16()),
                           static_cast<qsizetype>(script.size()) * 2);
    const QString params = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand ")
        + QString::fromLatin1(utf16.toBase64());
    const std::wstring fileW = L"powershell.exe";
    const std::wstring paramsW = params.toStdWString();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpFile = fileW.c_str();
    sei.lpParameters = paramsW.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei)) {
        qApp->quit(); // aboutToQuit 停核心并还原系统代理，脚本随后解压覆盖并重启
    } else {
        setStatus(QString::fromUtf8("自动更新启动失败（错误码 %1），已打开压缩包请手动解压覆盖").arg(GetLastError()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(zipPath));
        qApp->quit();
    }
}
#elif defined(Q_OS_MACOS)
void UpdateController::launchSilentUpdateAndRestartMac(const QString &dmgPath)
{
    // 写一个分离运行的 shell 脚本：等本进程退出 → 挂载 DMG → ditto 覆盖当前 .app → 卸载 → 去隔离 → 重启。
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp(); // Contents
    dir.cdUp(); // <bundle>.app
    const QString appBundle = dir.absolutePath();
    if (!appBundle.endsWith(QStringLiteral(".app"))) {
        setStatus(QString::fromUtf8("非 .app 方式运行，改为打开安装包手动安装"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(dmgPath));
        qApp->quit();
        return;
    }

    const auto shq = [](const QString &s) {
        QString r = s;
        r.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + r + QLatin1Char('\'');
    };

    const QString script = QStringLiteral(
        "#!/bin/sh\n"
        "PID=%1\n"
        "DMG=%2\n"
        "APP=%3\n"
        "do_replace() {\n"
        "  MNT=$(mktemp -d /tmp/clashauto-upd.XXXXXX)\n"
        "  hdiutil attach \"$DMG\" -nobrowse -noverify -mountpoint \"$MNT\" >/dev/null 2>&1 || return 2\n"
        "  SRC=$(ls -d \"$MNT\"/*.app 2>/dev/null | head -n1)\n"
        "  RC=1\n"
        "  if [ -n \"$SRC\" ] && rm -rf \"$APP\" 2>/dev/null && ditto \"$SRC\" \"$APP\" 2>/dev/null; then RC=0; fi\n"
        "  hdiutil detach \"$MNT\" >/dev/null 2>&1\n"
        "  return $RC\n"
        "}\n"
        "if [ \"$1\" = replace ]; then do_replace; exit $?; fi\n"
        "i=0; while kill -0 \"$PID\" 2>/dev/null; do sleep 0.5; i=$((i+1)); [ $i -ge 120 ] && break; done\n"
        "if ! do_replace; then\n"
        "  osascript -e \"do shell script \\\"/bin/sh '$0' replace\\\" with administrator privileges\" || { open \"$DMG\"; exit 1; }\n"
        "fi\n"
        "xattr -dr com.apple.quarantine \"$APP\" 2>/dev/null\n"
        "open -n \"$APP\"\n"
        "rm -f \"$0\"\n")
        .arg(QString::number(QCoreApplication::applicationPid()), shq(dmgPath), shq(appBundle));

    const QString scriptPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                   .filePath(QStringLiteral("clashauto-update.sh"));
    QFile f(scriptPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setStatus(QString::fromUtf8("无法写更新脚本，改为打开安装包手动安装"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(dmgPath));
        qApp->quit();
        return;
    }
    f.write(script.toUtf8());
    f.close();

    if (QProcess::startDetached(QStringLiteral("/bin/sh"), {scriptPath})) {
        qApp->quit(); // aboutToQuit 停核心并还原系统代理，脚本随后接手安装+重启
    } else {
        setStatus(QString::fromUtf8("自动安装启动失败，已打开安装包请手动安装"));
        QDesktopServices::openUrl(QUrl::fromLocalFile(dmgPath));
        qApp->quit();
    }
}
#endif
