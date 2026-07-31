#pragma once

// 内核发布源的单一事实源。SettingsController（下载）、AboutController（更新角标）、
// UpdateController（更新窗口）三处都要用同一套规则，分头写迟早会漂移。
//
// 内核已从上游 MetaCubeX/mihomo 换成我们自己的 fork ClashrAuto/clash（产物名 coast-*）。
// 有三个坑，换仓库时缺一个都会静默失效：
//
//  ① 不能用 /releases/latest。我们的 CI 发的是**滚动 prerelease**（tag 固定
//     Prerelease-master），而 GitHub 的 latest 按定义排除 prerelease —— 它会返回
//     从上游继承来的 v2025.11.25-6 那种**零资产**的旧 tag，于是"找不到匹配资源"。
//     必须按 tag 直取。
//  ② 版本不是语义化的。产物版本形如 master-<sha7>（CI 里 VERSION=分支名-短sha），
//     拿它做 semver 大小比较毫无意义。滚动通道里「有没有新版」就是「和本地不一样」。
//  ③ 本地版本正则要跟着改。核心自报 "Coast Meta master-d6e0fef linux arm64 ..."，
//     旧正则找的是 v<数字>，在这上面匹配不到 —— 探测失败会被当成"无更新"，角标永不亮。

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QSysInfo>

namespace CoreRelease {

// 滚动 prerelease，每次推 master 都会重刷同一个 tag 的资产
inline QString apiUrl()
{
    return QStringLiteral("https://api.github.com/repos/ClashrAuto/clash/releases/tags/Prerelease-master");
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

// 从产物名剥出版本：coast-linux-arm64-master-d6e0fef.gz -> master-d6e0fef
inline QString versionFromAsset(const QString &assetName, const QString &prefix)
{
    const QString ext = archiveExt();
    if (!assetName.startsWith(prefix) || !assetName.endsWith(ext)) {
        return {};
    }
    return assetName.mid(prefix.size(), assetName.size() - prefix.size() - ext.size());
}

// 解析 `core -v` 的输出。兼容两种自报格式：
//   Coast Meta master-d6e0fef linux arm64 with go1.26.5   （我们的 fork）
//   Mihomo Meta v1.19.29 linux arm64 with go1.26.5        （上游，用户可能还留着）
// 取 "Meta" 后面那个 token 即可，两种都覆盖。
inline QString localVersion(const QString &versionOutput)
{
    const QRegularExpressionMatch m =
        QRegularExpression(QStringLiteral("\\bMeta\\s+(\\S+)")).match(versionOutput);
    return m.hasMatch() ? m.captured(1) : QString();
}

// 滚动通道：不一样就是有更新。本地探测不到版本时不报更新，避免误报。
inline bool hasUpdate(const QString &remote, const QString &local)
{
    return !remote.isEmpty() && !local.isEmpty() && remote != local;
}

} // namespace CoreRelease
