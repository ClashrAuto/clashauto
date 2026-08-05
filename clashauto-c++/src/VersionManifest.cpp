#include "VersionManifest.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSysInfo>

namespace {

/// 把清单里的一条通道翻译成一个 GitHub release 对象。
///
/// `channel` 是 `packages[].type` 里的取值（`release` / `prerelease`）。
QJsonObject channelToRelease(const QJsonObject &root, const QString &channel)
{
    const QJsonObject head =
        root.value(QStringLiteral("releases")).toObject().value(channel).toObject();
    if (head.isEmpty()) {
        return {};
    }

    QJsonArray assets;
    const QJsonArray packages = root.value(QStringLiteral("packages")).toArray();
    for (const QJsonValue &pv : packages) {
        const QJsonObject p = pv.toObject();
        if (p.value(QStringLiteral("type")).toString() != channel) {
            continue;
        }
        const QString name = p.value(QStringLiteral("name")).toString();
        const QString address = p.value(QStringLiteral("address")).toString();
        if (name.isEmpty() || address.isEmpty()) {
            continue;
        }
        QJsonObject a;
        a.insert(QStringLiteral("name"), name);
        a.insert(QStringLiteral("browser_download_url"), address);
        a.insert(QStringLiteral("size"), p.value(QStringLiteral("size")).toDouble());
        assets.append(a);

        // ★ 边车要作为**独立资源**补回去：下载后的校验是按「有没有一个叫 <名字>.sha256
        //   的资源」查表的（见 UpdateController 下载完那段 / AppUpdater.pickAsset 旁边）。
        //   清单里它是包上的一个字段，不补的话下载能成、校验被静默跳过 —— 那正是校验
        //   最该起作用的场合（网络中间人 / CDN 缓存坏块）。
        const QString sha = p.value(QStringLiteral("sha256")).toString();
        if (!sha.isEmpty()) {
            QJsonObject s;
            s.insert(QStringLiteral("name"), name + QStringLiteral(".sha256"));
            s.insert(QStringLiteral("browser_download_url"), sha);
            s.insert(QStringLiteral("size"), 0);
            assets.append(s);
        }
    }
    if (assets.isEmpty()) {
        return {}; // 一个包都没有的通道不该被当成「有版本」——那会让角标亮着却下不到东西
    }

    QJsonObject rel;
    // ★ tag 用 `releases.<ch>.tag`，但**版本比较不该看它**：清单里的 `version` 才是
    //   「真的发布出来的最高版本」（新 tag 上可能一个包都还没传完）。调用方比的是
    //   资源名里的版本，与 tag 无关，所以这里原样带上即可。
    rel.insert(QStringLiteral("tag_name"), head.value(QStringLiteral("tag")));
    rel.insert(QStringLiteral("name"), head.value(QStringLiteral("name")));
    rel.insert(QStringLiteral("body"), head.value(QStringLiteral("notes")));
    rel.insert(QStringLiteral("published_at"), head.value(QStringLiteral("published")));
    rel.insert(QStringLiteral("html_url"), head.value(QStringLiteral("url")));
    rel.insert(QStringLiteral("prerelease"), channel == QLatin1String("prerelease"));
    rel.insert(QStringLiteral("draft"), false);
    rel.insert(QStringLiteral("assets"), assets);
    return rel;
}

QJsonObject coreChannelToRelease(const QJsonObject &root, const QString &channel)
{
    const QJsonObject c = root.value(QStringLiteral("core")).toObject().value(channel).toObject();
    if (c.isEmpty()) {
        return {};
    }
    QJsonArray assets;
    for (const QJsonValue &av : c.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = av.toObject();
        QJsonObject out;
        out.insert(QStringLiteral("name"), a.value(QStringLiteral("name")));
        out.insert(QStringLiteral("browser_download_url"), a.value(QStringLiteral("url")));
        out.insert(QStringLiteral("size"), a.value(QStringLiteral("size")));
        assets.append(out);
    }
    if (assets.isEmpty()) {
        return {};
    }
    QJsonObject rel;
    rel.insert(QStringLiteral("tag_name"), c.value(QStringLiteral("tag")));
    rel.insert(QStringLiteral("name"), c.value(QStringLiteral("name")));
    rel.insert(QStringLiteral("body"), c.value(QStringLiteral("notes")));
    rel.insert(QStringLiteral("published_at"), c.value(QStringLiteral("published")));
    rel.insert(QStringLiteral("html_url"), c.value(QStringLiteral("url")));
    rel.insert(QStringLiteral("prerelease"), channel == QLatin1String("prerelease"));
    rel.insert(QStringLiteral("draft"), false);
    rel.insert(QStringLiteral("assets"), assets);
    return rel;
}

/// 版本比较：`a` 是否比 `b` 新。与 AboutController / UpdateController 里那份同款
/// （先砍掉第一个 '-' 之后的一切，再去掉非数字/点，逐段比）。
///
/// ⚠️ **不能用 `CoreRelease::hasUpdate`**：那个是「和本地不一样就算可更新」，为的是
///   让内核在正式版/测试版之间能来回切（切回旧版时版本号是变小的）。拿它来挑「最高的
///   那个版本」会得到最后遍历到的那个，而不是最高的。
bool newerThan(const QString &a, const QString &b)
{
    auto parse = [](QString s) {
        const qsizetype dash = s.indexOf(QLatin1Char('-'));
        if (dash >= 0) {
            s.truncate(dash);
        }
        s.remove(QRegularExpression(QStringLiteral("[^0-9.]")));
        QVector<int> v;
        for (const QString &p : s.split(QLatin1Char('.'), Qt::SkipEmptyParts)) {
            v << p.toInt();
        }
        return v;
    };
    const QVector<int> x = parse(a);
    const QVector<int> y = parse(b);
    for (int i = 0; i < qMax(x.size(), y.size()); ++i) {
        const int l = i < x.size() ? x.at(i) : 0;
        const int r = i < y.size() ? y.at(i) : 0;
        if (l != r) {
            return l > r;
        }
    }
    return false;
}

// —— 进程内缓存 ——
QJsonDocument g_cached;
QDateTime g_cachedAt;
constexpr int kCacheSeconds = 30;

} // namespace

namespace VersionManifest {

QJsonArray appReleases(const QJsonDocument &manifest)
{
    const QJsonObject root = manifest.object();
    QJsonArray out;
    // 顺序即「谁更像最新」：正式版在前。调用方自己按 prerelease 标志分流，不依赖顺序，
    // 但保持与 API 一致（API 也是新的在前）省得有人写出依赖顺序的代码还以为没问题。
    for (const QString &ch : {QStringLiteral("release"), QStringLiteral("prerelease")}) {
        const QJsonObject rel = channelToRelease(root, ch);
        if (!rel.isEmpty()) {
            out.append(rel);
        }
    }
    return out;
}

QJsonArray coreReleases(const QJsonDocument &manifest)
{
    const QJsonObject root = manifest.object();
    QJsonArray out;
    for (const QString &ch : {QStringLiteral("release"), QStringLiteral("prerelease")}) {
        const QJsonObject rel = coreChannelToRelease(root, ch);
        if (!rel.isEmpty()) {
            out.append(rel);
        }
    }
    return out;
}

QString versionForThisPlatform(const QJsonDocument &manifest, const QString &channel)
{
#if defined(Q_OS_WIN)
    const QString plat = QStringLiteral("win");
#elif defined(Q_OS_MACOS)
    const QString plat = QStringLiteral("mac");
#else
    const QString plat = QStringLiteral("linux");
#endif
    const QString arch = QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))
                                 ? QStringLiteral("arm64")
                                 : QStringLiteral("x64");

    QString best;
    for (const QJsonValue &pv : manifest.object().value(QStringLiteral("packages")).toArray()) {
        const QJsonObject p = pv.toObject();
        if (p.value(QStringLiteral("type")).toString() != channel
            || p.value(QStringLiteral("p")).toString() != plat) {
            continue;
        }
#if defined(Q_OS_MACOS)
        // ★ mac 上架构不是判据，**产品线**才是：同一个 release 上并排放着两条线的 DMG，
        //   本二进制是 Qt 线（`-qt.dmg`，13.0+、universal）。认错的话会把用户推到 Swift 线
        //   那个包上 —— 那是 macOS 26 专供，装完在旧系统上根本起不来，而一键更新会先删掉
        //   旧 .app，没有退路。见 CLAUDE.md 里「-qt 是硬契约」。
        if (p.value(QStringLiteral("kind")).toString() != QLatin1String("dmg-qt")) {
            continue;
        }
#else
        if (p.value(QStringLiteral("芯片")).toString() != arch) {
            continue;
        }
#endif
        const QString v = p.value(QStringLiteral("version")).toString();
        if (best.isEmpty() || newerThan(v, best)) {
            best = v;
        }
    }
    return best;
}

QString geoipPublished(const QJsonDocument &manifest)
{
    return manifest.object()
            .value(QStringLiteral("geoip"))
            .toObject()
            .value(QStringLiteral("published"))
            .toString();
}

void invalidateCache()
{
    g_cached = QJsonDocument();
    g_cachedAt = QDateTime();
}

void fetch(QNetworkAccessManager *nam, QObject *ctx,
           std::function<void(const QJsonDocument &, const QString &)> onDone)
{
    if (!nam || !onDone) {
        return;
    }
    if (!g_cached.isNull() && g_cachedAt.isValid()
        && g_cachedAt.secsTo(QDateTime::currentDateTimeUtc()) < kCacheSeconds) {
        onDone(g_cached, QString());
        return;
    }

    QNetworkRequest req{QUrl(url())};
    req.setRawHeader("User-Agent", "clashauto-cpp");
    // release 资源是 302 跳到对象存储的 —— 不跟随就永远拿到一个空响应。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(15000);
#endif
    QNetworkReply *reply = nam->get(req);
    QPointer<QObject> guard(ctx);
    QObject::connect(reply, &QNetworkReply::finished, ctx, [reply, onDone, guard] {
        reply->deleteLater();
        if (!guard) {
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            onDone(QJsonDocument(), reply->errorString());
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (!doc.isObject()) {
            onDone(QJsonDocument(), QStringLiteral("版本清单格式不对"));
            return;
        }
        g_cached = doc;
        g_cachedAt = QDateTime::currentDateTimeUtc();
        onDone(doc, QString());
    });
}

} // namespace VersionManifest

// ———————————————————————————— 自测 ————————————————————————————

#include "CoreRelease.h"

#include <QDebug>

#include <cstdio>

namespace VersionManifest {

bool runSelfTest()
{
    int fails = 0;
    // ★ 用 std::fputs 而不是 qInfo：本二进制是 **GUI 子系统**，Qt 的日志在 Windows 上
    //   走 OutputDebugString，stdout/stderr 都收不到 —— 自测会变成「只有退出码、没有
    //   任何线索」。与 ConfigBuilder/LanScanner 那几个自测同一个写法。
    auto check = [&fails](bool cond, const char *what) {
        std::fputs(cond ? "  ok   " : "  FAIL ", stdout);
        std::fputs(what, stdout);
        std::fputs("\n", stdout);
        if (!cond) {
            ++fails;
        }
    };

    // 造一份「正式版全套、测试版缺 mac」的清单 —— 正是真机上最常见的形态。
    const QByteArray raw = R"JSON({
      "schema": 1,
      "packages": [
        {"type":"release","p":"win","芯片":"x64","kind":"setup","version":"1.0.639",
         "address":"https://x/Coast-1.0.639-windows-x64-setup.exe","size":100,
         "name":"Coast-1.0.639-windows-x64-setup.exe",
         "sha256":"https://x/Coast-1.0.639-windows-x64-setup.exe.sha256"},
        {"type":"release","p":"win","芯片":"arm64","kind":"setup","version":"1.0.639",
         "address":"https://x/Coast-1.0.639-windows-arm64-setup.exe","size":100,
         "name":"Coast-1.0.639-windows-arm64-setup.exe"},
        {"type":"release","p":"linux","芯片":"x64","kind":"deb","version":"1.0.639",
         "address":"https://x/Coast-1.0.639-linux-x64.deb","size":100,
         "name":"Coast-1.0.639-linux-x64.deb"},
        {"type":"release","p":"mac","芯片":"universal","kind":"dmg-qt","version":"1.0.639",
         "address":"https://x/Coast-1.0.639-macos-universal-qt.dmg","size":100,
         "name":"Coast-1.0.639-macos-universal-qt.dmg"},
        {"type":"release","p":"mac","芯片":"arm64","kind":"dmg","version":"1.0.640",
         "address":"https://x/Coast-1.0.640-macos-arm64.dmg","size":100,
         "name":"Coast-1.0.640-macos-arm64.dmg"},
        {"type":"prerelease","p":"win","芯片":"x64","kind":"setup","version":"1.0.881",
         "address":"https://x/Coast-1.0.881-windows-x64-setup.exe","size":100,
         "name":"Coast-1.0.881-windows-x64-setup.exe"}
      ],
      "releases": {
        "release":    {"tag":"v1.0.639","name":"Coast 1.0.639","version":"1.0.640","notes":"正式说明"},
        "prerelease": {"tag":"v1.0.881-beta.abc1234","name":"beta","version":"1.0.881","notes":"测试说明"}
      },
      "core": {
        "release": {"tag":"v1.10.4393","notes":"内核说明","assets":[
          {"name":"coast-windows-amd64-compatible-v1.10.4393.zip","url":"https://x/w.zip","size":1},
          {"name":"coast-darwin-arm64-v1.10.4393.gz","url":"https://x/m.gz","size":1},
          {"name":"coast-linux-amd64-compatible-v1.10.4393.gz","url":"https://x/l.gz","size":1},
          {"name":"coast-linux-arm64-v1.10.4393.gz","url":"https://x/la.gz","size":1}
        ]}
      },
      "geoip": {"published":"2026-08-04T23:33:18Z"}
    })JSON";

    const QJsonDocument doc = QJsonDocument::fromJson(raw);
    check(doc.isObject(), "夹具本身是合法 JSON");

    const QJsonArray rels = appReleases(doc);
    check(rels.size() == 2, "两条通道各翻译出一个 release");
    const QJsonObject r0 = rels.at(0).toObject();
    check(r0.value(QStringLiteral("tag_name")).toString() == QLatin1String("v1.0.639")
                  && !r0.value(QStringLiteral("prerelease")).toBool(),
          "下标 0 是正式版，且 prerelease 标志为假");
    check(rels.at(1).toObject().value(QStringLiteral("prerelease")).toBool(),
          "下标 1 是测试版，prerelease 标志为真");
    check(r0.value(QStringLiteral("body")).toString() == QString::fromUtf8("正式说明"),
          "更新说明来自 releases.<通道>.notes");

    // ★ 边车必须作为**独立资源**出现：下载后的校验是按「有没有一个叫 <名字>.sha256
    //   的资源」查表的，不补的话校验会被静默跳过。
    const QJsonArray a0 = r0.value(QStringLiteral("assets")).toArray();
    bool hasSidecar = false, hasPkg = false;
    for (const QJsonValue &v : a0) {
        const QString n = v.toObject().value(QStringLiteral("name")).toString();
        if (n == QLatin1String("Coast-1.0.639-windows-x64-setup.exe.sha256")) {
            hasSidecar = true;
        }
        if (n == QLatin1String("Coast-1.0.639-windows-x64-setup.exe")) {
            hasPkg = true;
        }
    }
    check(hasPkg, "包本身在 assets 里");
    check(hasSidecar, "有 sha256 的包会补出一条 <名字>.sha256 资源（否则校验静默跳过）");
    check(a0.size() == 6, "5 个包 + 1 条边车 = 6 项（没有 sha256 的不凭空造边车）");
    for (const QJsonValue &v : a0) {
        const QJsonObject a = v.toObject();
        if (a.value(QStringLiteral("browser_download_url")).toString().isEmpty()) {
            check(false, "每条资源都有 browser_download_url");
            break;
        }
    }

    // 内核：翻译完必须能被既有的 CoreRelease::pick 认出来（那套按 CPU 特性分级的规则没重写）。
    const QJsonArray cores = coreReleases(doc);
    check(cores.size() == 1, "内核只给了正式版通道，就只翻译出一个");
    const CoreRelease::Pick pick = CoreRelease::pick(cores, false);
    check(pick.isValid(), "CoreRelease::pick 能从翻译后的内核清单里挑出本平台产物");
    // versionFromAsset 剥出来的是**带 v 的**那一段（coast-…-v1.10.4393.zip → v1.10.4393）。
    check(pick.version == QLatin1String("v1.10.4393"), "挑出来的内核版本对（带 v，与产物名一致）");

    check(geoipPublished(doc) == QLatin1String("2026-08-04T23:33:18Z"), "GeoIP 时间戳取到了");

    // 本平台版本：只认本平台本架构（mac 上认 Qt 那条线），与 release 的 tag 无关。
    const QString v = versionForThisPlatform(doc, QStringLiteral("release"));
#if defined(Q_OS_MACOS)
    // ★ mac 上必须拿到 **Qt 线**那个 1.0.639，而不是 Swift 线更新的 1.0.640 ——
    //   挑错就是把用户推到一个他系统起不来的 app 上，且一键更新会先删旧包，没有退路。
    check(v == QLatin1String("1.0.639"), "mac 认的是 -qt 那条线（不是更新的 Swift 线）");
#else
    check(v == QLatin1String("1.0.639"), "本平台在正式通道里的版本");
#endif
    // 测试通道只有 windows-x64 —— 其它平台/架构必须拿到空串，而不是拿 release 的 tag 顶上。
    const QString bv = versionForThisPlatform(doc, QStringLiteral("prerelease"));
#if defined(Q_OS_WIN)
    const bool x64 = !QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"));
    check(bv == (x64 ? QStringLiteral("1.0.881") : QString()),
          "测试通道只有 x64 包：x64 拿到版本，arm64 拿到空串");
#else
    check(bv.isEmpty(), "测试通道里没有本平台的包 → 空串（不能拿 tag 顶上说有新版）");
#endif

    if (fails) {
        std::printf("版本清单自测：%d 条失败\n", fails);
    } else {
        std::printf("版本清单自测：全部通过\n");
    }
    std::fflush(stdout);
    return fails == 0;
}

} // namespace VersionManifest
