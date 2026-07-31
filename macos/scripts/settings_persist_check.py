#!/usr/bin/env python3
"""设置页里「可编辑但不落盘」的字段检查。

这类 bug 极其安静：改完当场生效（内存里的 config 更新了），重启才发现丢了，
而且丢的往往是用户花时间调过的东西（节点过滤正则）。编译器和单元测试都拦不住它 ——
界面绑了 $draft.X，只是没人把 X 写回磁盘而已。

判据：SettingsPage 里每个被双向绑定（$draft.X）的字段，都必须出现在某个
AppConfigLoader.persist(...) 调用的参数里。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "Sources/Coast/SettingsPage.swift"

# 明确不需要落盘的字段：写在这里并说明理由，别让豁免变成随手加的白名单。
EXEMPT = {
    # （目前没有豁免项）
}

source = PAGE.read_text(encoding="utf-8")
edited = set(re.findall(r"\$draft\.(\w+)", source))
persisted = set(re.findall(r"persist\([^)]*draft\.(\w+)", source, re.S))

missing = sorted(edited - persisted - set(EXEMPT))
print(f"设置页可编辑字段 {len(edited)} 个，已落盘 {len(edited & persisted)} 个")
if missing:
    print("\n❌ 以下字段有界面、无落盘 —— 用户改完重启即丢：")
    for name in missing:
        print(f"     draft.{name}")
    print("\n   要么在「应用」里加 AppConfigLoader.persist(...)，")
    print("   要么在本脚本的 EXEMPT 里写明为什么不需要落盘。")
    sys.exit(1)
print("✅ 没有「有界面、无落盘」的字段")
