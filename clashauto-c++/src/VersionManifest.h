#pragma once

// version.json —— 查更新的唯一数据源，用来绕开 GitHub API 的限流。
//
// **为什么不直接打 api.github.com。** 未登录的 GitHub API 是 **60 次/小时、按出口 IP 算**。
// 用户经机场出网时几百人共用一个出口，限流几乎必中；而 GitHub 返回的是一个 JSON 对象
// （不是数组）带一句 `API rate limit exceeded for <ip>`，在界面上表现为「获取失败」，
// 读起来完全像网络故障 —— 用户只会觉得更新坏了，没人会想到是配额。
//
// 清单由 CI 维护（`.github/scripts/build_version_manifest.py`），作为**固定 tag `version`
// 的 release 资源**发布。下载它走的是 release 资源的 CDN，不是 API，没有配额。
//
// ★ **本文件只做「适配」，不做「挑选」。** 清单被翻译成 GitHub releases 的形状，再交给
//   原有的解析器（`UpdateController::fetchReleases` 的资源过滤、`CoreRelease::pick` 的
//   CPU 特性分级、mac 两条产品线的 `-qt` 排除……）。那些规则一条都没重写 —— 重写一遍
//   就意味着两套挑包逻辑要长期保持一致，而它们只要漂移一次，用户就会装到错的包上。

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

class QNetworkAccessManager;
class QObject;

namespace VersionManifest {

/// 清单直链。固定 tag `version` 上的资源 —— 走 CDN，不是 API。
inline QString url()
{
    return QStringLiteral(
        "https://github.com/ClashrAuto/clashauto/releases/download/version/version.json");
}

/// 程序包 → GitHub releases 形状的数组（下标 0 = 正式版，1 = 测试版；缺哪条就不放）。
///
/// 每个元素带 `tag_name` / `name` / `body` / `prerelease` / `draft:false` / `assets[]`，
/// assets 里是 `name` / `browser_download_url` / `size` —— 与 API 的字段名逐个对齐，
/// 现有解析器拿到手认不出区别。
///
/// ★ **`.sha256` 边车要补回去**：清单里校验地址是挂在包上的 `sha256` 字段，而下载完
///   校验那段是按「`<资源名>.sha256` 这个**资源**」查表的。不补的话下载能成、校验静默跳过。
QJsonArray appReleases(const QJsonDocument &manifest);

/// 内核 → GitHub releases 形状的数组，直接喂 `CoreRelease::pick`。
QJsonArray coreReleases(const QJsonDocument &manifest);

/// **本机这个平台**在该通道里能装到的最高版本（`release` / `prerelease`），拿不到返回空串。
///
/// ★ 这条是清单存在的核心理由之一。以前「有没有新版」比的是 release 的 **tag**，
///   而同一个 tag 上各平台的包是**陆续到的**（mac 的两个 DMG 由外部签名仓库异步追加，
///   Windows 与 Linux 也不同时完成）。于是 Windows 用户会看到「有新版 1.0.878」，
///   点更新却发现没有 Windows 包 —— 角标亮着、下不了东西，比不提示更糟。
///   这里只看**本平台本架构**（mac 上还要认准 Qt 那条产品线）的包，比的是它自己的版本。
QString versionForThisPlatform(const QJsonDocument &manifest, const QString &channel);

/// GeoIP 上游的最新发布时间戳（`published_at` 的等价物），拿不到返回空串。
QString geoipPublished(const QJsonDocument &manifest);

/// 取清单。**30 秒内的重复请求走进程内缓存** —— `checkAll()` 会连着查程序/内核/GeoIP
/// 三样，各自去下一遍同一个文件纯属浪费（而且首启那一下三个请求并发，谁都不快）。
///
/// `onDone` 一定会被调用：失败时给一个空的 QJsonDocument，调用方据此保持原状而不是清空。
/// 回调绑在 `ctx` 上，`ctx` 先死则不回调（与 QNetworkReply 的常规用法一致）。
void fetch(QNetworkAccessManager *nam, QObject *ctx,
           std::function<void(const QJsonDocument &, const QString &error)> onDone);

/// 不联网的适配自测（`COAST_MANIFEST_SELFTEST=1`）。exit 0 = 通过。
///
/// 钉的是「清单 → GitHub releases 形状」这一层：它一旦翻错，表现是**更新页一片空白**
/// 或者**挑到别的产品线的包**，而两者都不会在编译期暴露。
bool runSelfTest();

/// 清掉缓存 —— 用户手动点「获取更新」时必须真取一次，不能拿 30 秒前的旧结果糊弄。
void invalidateCache();

} // namespace VersionManifest
