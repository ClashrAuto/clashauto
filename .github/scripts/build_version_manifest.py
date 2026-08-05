#!/usr/bin/env python3
"""更新 version.json —— 客户端查更新的唯一数据源，用来绕开 GitHub API 的限流。

**为什么要有这个东西。** 客户端原本每次查更新都直接打 api.github.com：程序 release 列表、
内核 release 列表、GeoIP 发布时间，三个端点。未登录的 API 是 **60 次/小时/IP**，而且按
出口 IP 算 —— 同一个机场出口后面挂着几百个用户时谁都查不动，报错还长得像网络故障
（写这段时本机就正撞上：`API rate limit exceeded for 18.144.87.130`）。

改成：CI 每次发完包，把版本信息汇总进一个 `version.json`，作为**固定 tag（`version`）的
release 资源**发布。客户端只下载这一个文件 —— 走 release 资源的 CDN，不是 API，没有限流。

**只更新「刚发的这一版」，不扫历史列表。** 拿旧清单 → 把本次这条通道（release /
prerelease）换掉 → 传回去。整次只打 3 个 API：本次 release、内核正式、内核测试。

**包是逐条合并而不是整段覆盖**，这一条是「每个端版本号不一定一致」的实现：
本仓库的 CI 只发 Windows/Linux，macOS 的两个 DMG 由外部签名仓库
（integemjack/schat.build）**异步**追加，晚几分钟到几小时都正常；Windows 与 Linux 之间也
不同时完成。所以本次 release 里没有的包，**保留旧清单里的那一条**，它自带自己的版本号。
整段覆盖的后果是：Windows 用户看到「有新版」，点更新却没有可下的包 —— 角标亮着却下不了，
比不提示更糟。

产物结构：

    {
      "schema": 1,
      "generated": "…",
      "packages": [ {type,p,芯片,kind,version,address,name,size,tag,published,sha256} … ],
      "releases":  {"release": {tag,name,version,published,notes,url}, "prerelease": {…}},
      "core":      {"release": {tag,…,assets:[{name,url,size}]}, "prerelease": {…}},
      "geoip":     {"published": "…", "tag": "…"}
    }

`packages` 是**扁平数组**，一条 = 一个能下载的包。客户端按 (type,p,芯片,kind) 挑自己那条，
比对 `version`，下 `address`。更新说明不塞进每一条（会重复几十遍），放 `releases` 那段。

用法：
    GITHUB_TOKEN=… python3 build_version_manifest.py \\
        --tag v1.0.878-beta.096318e --channel prerelease --prev old.json -o version.json
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from datetime import datetime, timezone

APP_REPO = "ClashrAuto/clashauto"
CORE_REPO = "ClashrAuto/clash"
GEOIP_REPO = "MetaCubeX/meta-rules-dat"

# 内核的滚动测试版固定挂在这个 tag 上（与客户端 UpdateController.h 的注释同一处口径）。
CORE_BETA_TAG = "Prerelease-master"

SCHEMA = 1

# 资源名形如 `Coast-1.0.876-windows-x64-setup.exe`（老包是 `ClashAuto-0.1.35-…`，前缀不固定，
# 所以只认「前缀-版本号-」这个头，剩下整段拿去分类）。
NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]*-(\d+(?:\.\d+)*)-(.+)$")

# 版本号之后那一段 → (p, 芯片, kind)。
#
# ★ **-qt 必须排在普通 dmg 前面判**：两条 macOS 产品线只靠这个后缀区分（Swift 线 macOS 26+，
#   Qt 线 macOS 13+），认错的后果是把用户推到一个他系统起不来的包上，而一键更新会先删掉
#   旧 .app —— 没有退路。见 CLAUDE.md 里那段「-qt 是硬契约」。
KIND_RULES = [
    (re.compile(r"^macos-(universal)-qt\.dmg$"), "mac", "dmg-qt"),
    (re.compile(r"^macos-(universal)\.dmg$"), "mac", "dmg"),
    (re.compile(r"^windows-(x64|arm64)-setup\.exe$"), "win", "setup"),
    (re.compile(r"^windows-(x64|arm64)-portable\.zip$"), "win", "portable"),
    (re.compile(r"^linux-(x64|arm64)\.deb$"), "linux", "deb"),
    (re.compile(r"^linux-(x64|arm64)-portable\.tar\.gz$"), "linux", "portable"),
    (re.compile(r"^linux-(x64|arm64)-portable\.zip$"), "linux", "portable-zip"),
]


def api(url, token, optional=False):
    req = urllib.request.Request(url)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "coast-manifest")
    if token:
        req.add_header("Authorization", "token " + token)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError) as e:
        if optional:
            print(f"::warning::{url} 取不到（{e}），沿用旧清单里的这一段", file=sys.stderr)
            return None
        raise


def version_tuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "")) or (0,)


def classify(name):
    """资源名 → (version, p, 芯片, kind)；不是包（或认不出来）返回 None。

    ★ 认不出来的**不丢**，落到 kind=原始后缀那一条兜底里。以后 CI 加一种包（比如
      windows-x64-portable-slim.zip），忘了改这里的话，它照样进清单、客户端至少看得到，
      而不是「CI 发了但清单里没有 → 用户永远下不到」这种静默漏包。
    """
    m = NAME_RE.match(name)
    if not m:
        return None
    version, rest = m.group(1), m.group(2)
    if rest.endswith(".sha256"):
        return None
    for pattern, p, kind in KIND_RULES:
        hit = pattern.match(rest)
        if hit:
            return version, p, hit.group(1), kind
    # 兜底：`<平台>-<芯片>-<其余>` 拆一刀，拆不动就整段当 kind。
    parts = rest.split("-", 2)
    if len(parts) >= 2:
        return version, parts[0], parts[1].split(".")[0], (parts[2] if len(parts) > 2 else rest)
    return version, "", "", rest


def packages_from_release(rel, channel, prev_packages):
    """本次 release 的包 + 旧清单里本次没有的那些（各带各的版本号）。"""
    checksums = {}
    for a in rel.get("assets", []):
        n = a.get("name") or ""
        if n.endswith(".sha256"):
            checksums[n[: -len(".sha256")]] = a.get("browser_download_url") or ""

    fresh = {}
    for a in rel.get("assets", []):
        name = a.get("name") or ""
        info = classify(name)
        if not info:
            continue
        version, p, chip, kind = info
        entry = {
            "type": channel,
            "p": p,
            "芯片": chip,
            "kind": kind,
            "version": version,
            "address": a.get("browser_download_url") or "",
            "name": name,
            "size": a.get("size") or 0,
            "tag": rel.get("tag_name") or "",
            "published": rel.get("published_at") or "",
        }
        if name in checksums:
            entry["sha256"] = checksums[name]
        fresh[(p, chip, kind)] = entry

    # 合并：本次没有的包沿用旧的；旧的比新的还新时也保留（CI 重跑老 tag 不该把新包顶回去）。
    for old in prev_packages:
        if old.get("type") != channel:
            continue
        key = (old.get("p"), old.get("芯片"), old.get("kind"))
        cur = fresh.get(key)
        if cur is None or version_tuple(old.get("version")) > version_tuple(cur.get("version")):
            fresh[key] = old
    return sorted(fresh.values(), key=lambda e: (e["p"], e["芯片"], e["kind"]))


def release_head(rel, packages, channel):
    """通道抬头：tag / 更新说明 / 以及**真正发布出来的最高版本**。

    版本不取 release 的 tag —— 新 tag 上可能一个包都还没传完（CI 正在跑），那时它不该被
    当成「已发布的版本」：客户端会拿它去比对，然后找不到可下的包。
    """
    mine = [p for p in packages if p.get("type") == channel]
    top = max((version_tuple(p.get("version")) for p in mine), default=(0,))
    return {
        "tag": rel.get("tag_name") or "",
        "name": rel.get("name") or "",
        "version": ".".join(str(x) for x in top) if top != (0,) else "",
        "published": rel.get("published_at") or "",
        "notes": rel.get("body") or "",
        "url": rel.get("html_url") or "",
    }


def core_channel(rel):
    """内核通道：资源清单原样带上。

    不在这里挑平台 —— 客户端侧 `CoreRelease::pick` / `CoreDownloader` 已有一套按名字挑的
    规则（还要按 CPU 特性分 v1/v2/v3/compatible），喂给它同样形状的列表即可，不必重写一遍。
    """
    if not isinstance(rel, dict) or not rel.get("tag_name"):
        return None
    return {
        "tag": rel.get("tag_name") or "",
        "name": rel.get("name") or "",
        "published": rel.get("published_at") or "",
        "notes": rel.get("body") or "",
        "url": rel.get("html_url") or "",
        "assets": [
            {
                "name": a.get("name") or "",
                "url": a.get("browser_download_url") or "",
                "size": a.get("size") or 0,
            }
            for a in rel.get("assets", [])
        ],
    }


def selftest():
    """不联网的合并自测。

    ★ 为什么非要有：真机上这条路**不容易被撞到** —— 等一会儿 mac/windows 的包就补齐了，
      于是「本次没有的包沿用旧的」那一支跑不到，测了等于没测（本地第一次跑就是这样：
      v1.0.878 在两次调用之间被 CI 补全了，看起来一切正常，其实合并分支一次都没走）。
    """
    fails = []

    def check(cond, what):
        print(("  ok   " if cond else "  FAIL ") + what)
        if not cond:
            fails.append(what)

    def asset(name, size=10):
        return {"name": name, "browser_download_url": "https://x/" + name, "size": size}

    # 旧清单：全套 1.0.876
    full = {
        "tag_name": "v1.0.876", "name": "Coast 1.0.876", "published_at": "T0", "body": "旧说明",
        "assets": [asset(n) for n in [
            "Coast-1.0.876-windows-x64-setup.exe", "Coast-1.0.876-windows-x64-setup.exe.sha256",
            "Coast-1.0.876-windows-x64-portable.zip", "Coast-1.0.876-linux-x64.deb",
            "Coast-1.0.876-macos-universal.dmg", "Coast-1.0.876-macos-universal-qt.dmg"]],
    }
    prev = packages_from_release(full, "prerelease", [])
    check(len(prev) == 5, "首份：5 个包（.sha256 不算独立的包）")
    by = {(e["p"], e["芯片"], e["kind"]): e for e in prev}
    check(by.get(("mac", "universal", "dmg-qt"), {}).get("name", "").endswith("-qt.dmg"),
          "-qt 与普通 dmg 分成两条（认错会把用户推到起不来的包上）")
    check(by.get(("win", "x64", "setup"), {}).get("sha256", "").endswith(".sha256"),
          ".sha256 挂到对应包上")

    # 新 release 只有 linux —— mac/windows 必须沿用旧的，并保留**旧的版本号**
    linux_only = {
        "tag_name": "v1.0.878", "name": "Coast 1.0.878", "published_at": "T1", "body": "新说明",
        "assets": [asset("Coast-1.0.878-linux-x64.deb")],
    }
    merged = packages_from_release(linux_only, "prerelease", prev)
    by = {(e["p"], e["芯片"], e["kind"]): e for e in merged}
    check(len(merged) == 5, "合并后仍是 5 个包（没被整段覆盖）")
    check(by.get(("linux", "x64", "deb"), {}).get("version") == "1.0.878", "linux 换成了新版")
    check(by.get(("mac", "universal", "dmg"), {}).get("version") == "1.0.876", "mac 沿用旧版")
    check(by.get(("mac", "universal", "dmg-qt"), {}).get("version") == "1.0.876", "mac -qt 沿用旧版")
    check(by.get(("win", "x64", "setup"), {}).get("version") == "1.0.876", "windows 沿用旧版")
    head = release_head(linux_only, merged, "prerelease")
    check(head["version"] == "1.0.878", "通道版本取所有包里最高的那个")
    check(head["notes"] == "新说明", "更新说明跟着本次 release 走")

    # CI 重跑一个**老** tag：不能把已经在清单里的新包顶回去
    old_rerun = {
        "tag_name": "v1.0.870", "name": "Coast 1.0.870", "published_at": "T2", "body": "老说明",
        "assets": [asset("Coast-1.0.870-linux-x64.deb")],
    }
    again = {(e["p"], e["芯片"], e["kind"]): e
             for e in packages_from_release(old_rerun, "prerelease", merged)}
    check(again.get(("linux", "x64", "deb"), {}).get("version") == "1.0.878",
          "重跑老 tag 不会把新包顶回去")

    # 一个包都认不出来的 release：不能拿它清空旧清单
    empty = {"tag_name": "v1.0.879", "name": "x", "published_at": "T3", "body": "",
             "assets": [asset("random-thing.txt")]}
    kept = packages_from_release(empty, "prerelease", merged)
    check(len(kept) == 5, "空 release 不清空旧清单")

    # 没见过的包形态要兜底进清单，而不是静默消失
    novel = {"tag_name": "v1.0.880", "name": "x", "published_at": "T4", "body": "",
             "assets": [asset("Coast-1.0.880-windows-x64-portable-slim.zip")]}
    got = [e for e in packages_from_release(novel, "prerelease", [])]
    check(len(got) == 1 and got[0]["p"] == "windows", "没见过的包形态也进清单（不静默漏包）")

    print("清单合并自测：" + ("全部通过" if not fails else f"{len(fails)} 条失败"))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true", help="不联网的合并自测")
    ap.add_argument("--tag", help="本次发布的 release tag")
    ap.add_argument("--channel", choices=["release", "prerelease"])
    ap.add_argument("--prev", default="", help="旧的 version.json（没有就当首次生成）")
    ap.add_argument("-o", "--out", default="version.json")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if not args.tag or not args.channel:
        ap.error("--tag 与 --channel 必填（除非 --selftest）")
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN") or ""

    prev = {}
    if args.prev and os.path.exists(args.prev) and os.path.getsize(args.prev) > 0:
        try:
            with open(args.prev, encoding="utf-8") as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                prev = loaded
        except (ValueError, OSError) as e:
            # 旧清单坏了不该让发布卡住，但要喊出来 —— 静默重建会把 mac 那几条丢掉。
            print(f"::warning::旧 version.json 读不了（{e}），本次按首份生成", file=sys.stderr)

    prev_packages = prev.get("packages") if isinstance(prev.get("packages"), list) else []
    rel = api(f"https://api.github.com/repos/{APP_REPO}/releases/tags/{args.tag}", token)

    # 本通道重算，另一通道原样留着。
    packages = [p for p in prev_packages if p.get("type") != args.channel]
    packages += packages_from_release(rel, args.channel, prev_packages)
    packages.sort(key=lambda e: (e.get("type", ""), e.get("p", ""), e.get("芯片", ""),
                                 e.get("kind", "")))

    releases = dict(prev.get("releases") or {}) if isinstance(prev.get("releases"), dict) else {}
    releases[args.channel] = release_head(rel, packages, args.channel)

    core = dict(prev.get("core") or {}) if isinstance(prev.get("core"), dict) else {}
    # 内核两条各一个 API：正式版走 /releases/latest，测试版是固定 tag 的滚动预发布。
    # 取不到就沿用旧清单里那一段 —— 内核版本不该因为一次网络抖动被清空。
    got = core_channel(api(f"https://api.github.com/repos/{CORE_REPO}/releases/latest",
                           token, optional=True))
    if got:
        core["release"] = got
    got = core_channel(api(f"https://api.github.com/repos/{CORE_REPO}/releases/tags/{CORE_BETA_TAG}",
                           token, optional=True))
    if got:
        core["prerelease"] = got

    geoip = dict(prev.get("geoip") or {}) if isinstance(prev.get("geoip"), dict) else {}
    got = api(f"https://api.github.com/repos/{GEOIP_REPO}/releases/latest", token, optional=True)
    if isinstance(got, dict) and got.get("published_at"):
        geoip = {"published": got.get("published_at"), "tag": got.get("tag_name") or ""}

    manifest = {
        "schema": SCHEMA,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "packages": packages,
        "releases": releases,
        "core": core,
        "geoip": geoip,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1, sort_keys=False)
        f.write("\n")

    for ch in ("release", "prerelease"):
        head = releases.get(ch)
        mine = [p for p in packages if p.get("type") == ch]
        if not head:
            print(f"  {ch}: 无")
            continue
        mark = "  ← 本次更新" if ch == args.channel else ""
        print(f"  {ch}: {head['tag']} (版本 {head['version']}) {len(mine)} 个包{mark}")
        for e in mine:
            old = "  ← 沿用旧版（本次没有这个包）" if e["version"] != head["version"] else ""
            print(f"      {e['p']:<6} {e['芯片']:<10} {e['kind']:<14} {e['version']}{old}")
    for ch in ("release", "prerelease"):
        c = core.get(ch)
        print(f"  core/{ch}: {c['tag'] if c else '无'}")
    print(f"  geoip: {geoip.get('published') or '无'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
