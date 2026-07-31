#!/usr/bin/env python3
"""设置页里「可编辑但不落盘」的字段检查。

这类 bug 极其安静：改完当场生效（内存里的 config 更新了），重启才发现丢了，
而且丢的往往是用户花时间调过的东西（节点过滤正则）。编译器和单元测试都拦不住它 ——
界面把值写进了内存里的 config，只是没人把它写回磁盘而已。

## 判据

设置页把改动写进内存的**唯一出口**是 `state.applyConfig(...)`。所以规则是：

    每个含 `state.applyConfig(` 的函数/计算属性，必须在同一个块里
    也含 `AppConfigLoader.persist(`。

只写内存不落盘 = 重启即丢，正是要拦的那件事。

## 为什么换了判据

上一版查的是「每个 `$draft.X` 双向绑定的字段都要出现在某个 persist 调用里」。
设置页按 Qt 重做成分标签 + 开关即时生效之后，页面**不再有那个 `draft` 结构**，
于是那条正则匹配到 0 个字段、脚本永远绿 —— 一个永远通过的检查比没有检查更危险，
因为它看起来还在把关。换成上面这条与具体写法无关的判据，并在匹配到 0 个块时
**主动失败**，免得同样的事再发生一次。
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = ROOT / "Sources/Coast/SettingsPage.swift"

# 明确不需要落盘的块：写在这里并说明理由，别让豁免变成随手加的白名单。
EXEMPT = {
    # 系统页里的「系统代理」开关不进 config.yaml —— 它的真值是系统网络配置本身，
    # 由 CoastController 直接读写。这个块里其余各项都是走 bool(...) 助手落盘的。
}

source = PAGE.read_text(encoding="utf-8")

# 按「二级缩进的 func / var 声明」切块。设置页里所有写配置的代码都在这一层。
BLOCK = re.compile(r"^    (?:private )?(?:static )?(?:func|var) (\w+)", re.M)
starts = [(m.start(), m.group(1)) for m in BLOCK.finditer(source)]

blocks = []
for index, (offset, name) in enumerate(starts):
    end = starts[index + 1][0] if index + 1 < len(starts) else len(source)
    blocks.append((name, source[offset:end]))

writes_memory = [(name, body) for name, body in blocks if "state.applyConfig(" in body]
exempted = [name for name, _ in writes_memory if name in EXEMPT]
offenders = [name for name, body in writes_memory
             if "AppConfigLoader.persist(" not in body and name not in EXEMPT]

print(f"写内存 config 的块 {len(writes_memory)} 个，"
      f"同时落盘 {len(writes_memory) - len(exempted) - len(offenders)} 个，"
      f"豁免 {len(exempted)} 个")

if not writes_memory:
    # 一个块都没匹配到 = 判据和代码写法脱节了，脚本已经失去意义。
    print("\n❌ 没有匹配到任何写配置的块 —— 判据与代码写法已脱节，请更新本脚本。")
    sys.exit(1)

if offenders:
    print("\n❌ 以下块只写内存、不落盘 —— 用户改完重启即丢：")
    for name in offenders:
        print(f"     {name}")
    print("\n   要么补 AppConfigLoader.persist(...)，")
    print("   要么在本脚本的 EXEMPT 里写明为什么不需要落盘。")
    sys.exit(1)

print("✅ 没有「只写内存、不落盘」的块")
