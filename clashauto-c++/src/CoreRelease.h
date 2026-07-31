#pragma once

// 内核发布源的单一事实源。SettingsController（下载）、AboutController（更新角标）、
// UpdateController（更新窗口）三处都要用同一套规则，分头写迟早会漂移。
//
// 内核来自我们自己的 fork ClashrAuto/clash（产物名 coast-*），两条通道：
//   正式版：推 master 自动发版，tag = v<major>.<minor>.<提交数>，每个 commit 最后一位 +1
//   测试版：非主分支推送，滚动 prerelease，tag = Prerelease-<分支名>
//
// 三个坑，缺一个都会静默失效：
//
//  ① 不能用 /releases/latest。一是它按 GitHub 定义排除 prerelease，测试版通道
//     根本拿不到；二是这个 fork 从上游继承了一堆**零资产**的空 tag
//     （v2025.11.25-* 之类），latest 很可能正好落在那上面 —— 表现为
//     「找不到匹配资源」。所以走 /releases 全量列表，且**必须校验该 release
//     真的带本平台产物**才认。
//  ② 判「有没有新版」用的是「和本地不一样」，不是「远端版本号更大」。
//     这样从测试版切回正式版（版本号变小）同样会提示可更新 —— 否则用户装了
//     测试版内核就再也换不回正式版。
//  ③ 本地版本正则要认得核心的自报格式。核心打印
//     "Coast Meta v1.10.3951 linux arm64 ..."，取 "Meta" 后面那个 token；
//     顺带兼容上游的 "Mihomo Meta v1.19.29"，用户机器上还留着旧核心时也能判定该换。

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QSysInfo>

namespace CoreRelease {

// 全量列表：正式版和测试版都在里面，按通道各挑各的
inline QString apiUrl()
{
    return QStringLiteral("https://api.github.com/repos/ClashrAuto/clash/releases?per_page=20");
}

inline QString osName()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("darwin");
#else
    return QStringLiteral("linux");
#endif
}

inline QString archiveExt()
{
#if defined(Q_OS_WIN)
    return QStringLiteral(".zip");
#else
    return QStringLiteral(".gz");
#endif
}

// 本平台可接受的产物名前缀，按优先级排列。
// x86-64 优先 -compatible：CI 里不带后缀的 amd64 产物是 GOAMD64=v3，要较新 CPU；
// compatible 才是 v1 基线，任何 x86-64 都能跑。
inline QStringList assetPrefixes()
{
    const QString os = osName();
    if (QSysInfo::currentCpuArchitecture().contains(QStringLiteral("arm"))) {
        return {QStringLiteral("coast-%1-arm64-").arg(os)};
    }
    return {QStringLiteral("coast-%1-amd64-compatible-").arg(os),
            QStringLiteral("coast-%1-amd64-v1-").arg(os)};
}

// 从产物名剥出版本：coast-linux-arm64-v1.10.3951.gz -> v1.10.3951
inline QString versionFromAsset(const QString &assetName, const QString &prefix)
{
    const QString ext = archiveExt();
    if (!assetName.startsWith(prefix) || !assetName.endsWith(ext)) {
        return {};
    }
    return assetName.mid(prefix.size(), assetName.size() - prefix.size() - ext.size());
}

struct Pick {
    QString version;   // 产物名里嵌的版本，如 v1.10.3951 / v1.10.3950-beta.d6e0fef9
    QString url;       // browser_download_url
    QString assetName;
    QString notes;     // release body，供更新窗口展示
    QString tag;       // release 的 tag_name（正式版即版本号；测试版是 Prerelease-<分支>）
    bool beta = false;
    bool isValid() const { return !version.isEmpty() && !url.isEmpty(); }
};

// 在一个 release 的 assets 里找本平台产物
inline Pick pickFromAssets(const QJsonArray &assets)
{
    for (const QString &prefix : assetPrefixes()) {
        for (const QJsonValue &av : assets) {
            const QJsonObject a = av.toObject();
            const QString ver = versionFromAsset(a.value(QStringLiteral("name")).toString(), prefix);
            if (!ver.isEmpty()) {
                Pick p;
                p.version = ver;
                p.url = a.value(QStringLiteral("browser_download_url")).toString();
                p.assetName = a.value(QStringLiteral("name")).toString();
                return p;
            }
        }
    }
    return {};
}

// 从 /releases 列表里挑本通道的产物。列表是新→旧排序。
// wantBeta=true 取最新的 prerelease，false 取最新的正式版。
// **必须带本平台产物才认**——见文件头 ① 的零资产空 tag 问题。
inline Pick pick(const QJsonArray &releases, bool wantBeta)
{
    for (const QJsonValue &rv : releases) {
        const QJsonObject r = rv.toObject();
        if (r.value(QStringLiteral("draft")).toBool()) {
            continue;
        }
        if (r.value(QStringLiteral("prerelease")).toBool() != wantBeta) {
            continue;
        }
        Pick p = pickFromAssets(r.value(QStringLiteral("assets")).toArray());
        if (p.isValid()) {
            p.beta = wantBeta;
            p.notes = r.value(QStringLiteral("body")).toString();
            p.tag = r.value(QStringLiteral("tag_name")).toString();
            return p;
        }
    }
    return {};
}

// 解析 `core -v` 的输出。兼容两种自报格式：
//   Coast Meta v1.10.3951 linux arm64 with go1.26.5    （我们的 fork）
//   Mihomo Meta v1.19.29 linux arm64 with go1.26.5     （上游，用户可能还留着）
inline QString localVersion(const QString &versionOutput)
{
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("\\bMeta\\s+(\\S+)")).match(versionOutput);
    return m.hasMatch() ? m.captured(1) : QString();
}

// 「和本地不一样」即可更新 —— 不是「更大」。
// 这条是测试版/正式版能互相切换的关键：从 v1.10.3951-beta.xxx 切回 v1.10.3950
// 版本号是变小的，用大小比较会判成「已是最新」，用户就被钉死在测试版上了。
inline bool hasUpdate(const QString &remote, const QString &local)
{
    return !remote.isEmpty() && !local.isEmpty() && remote != local;
}

} // namespace CoreRelease
