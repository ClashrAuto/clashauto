#!/usr/bin/env python3
"""翻译覆盖率检查。

用法：python3 scripts/i18n_check.py [--missing <lang>]

界面字符串以**中文源串**为 key（见 Sources/Coast/I18n.swift），所以「哪些串需要翻译」
可以直接从源码里扫出来：凡是形如 "…中文…".t 的字面量都是。

漏翻的后果不是崩溃、也不是显示 key，而是**那一条回落成中文** —— 混在译文里很不显眼，
所以需要这个脚本主动报数，而不是等用户发现。

除覆盖率外还查两件事（都会让脚本以非零码退出）：

1. **有中文、却没标 `.t` 的界面串。** 这是覆盖率统计的盲区：没标的串根本不进分母，
   于是显示 100%，而它对所有非中文用户直接漏出中文。踩过一次 —— 13 处插值文案
   （「有新版 %@」「下载中 %d%%」…）就这样躲了很久。
   确实不该翻的（只在 `COAST_*_TESTHOOK` 之类环境变量下才出现的排障日志）在行尾写
   `// i18n-ignore: <理由>` 豁免；豁免的行会被逐条打印出来，不会悄悄消失。

2. **译文与源串的格式占位符不一致。** 这些串要喂给 `String(format:)`，占位符数量或
   类型对不上时它会按错误的类型去读参数 —— 直接崩，而且**只在那一种语言下崩**，
   开发机上（中文）永远复现不了。
"""
import json, re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
I18N = ROOT.parent / 'clashauto-c++' / 'assets' / 'i18n'
CJK = re.compile(r'[一-鿿]')

# Swift 源码里的转义序列 → 运行时真正的字符。
# 查表用的 key 是**运行时**那个串,不是源码里那串字符:源码写 `\n` 是两个字符,
# 而与 Qt 共用的 JSON 表里存的是一个真换行。不还原的话,任何带转义的文案都会被
# 报成「缺翻译」,而它其实好好地在表里。
ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '"': '"', '\\': '\\', "'": "'", '0': '\0'}

def unescape(raw):
    out, i = [], 0
    while i < len(raw):
        if raw[i] == '\\' and i + 1 < len(raw):
            out.append(ESCAPES.get(raw[i + 1], raw[i + 1]))
            i += 2
        else:
            out.append(raw[i])
            i += 1
    return ''.join(out)

# 扫描范围。
#
# ★ 原来只扫 `Sources/Coast`，于是 **CoastKit 里的托盘菜单整份是裸中文**
# （控制面板 / 启动核心 / 打开网页代理 / 打开增强模式 / 退出程序 / 运行中 / 已停止），
# 对 11 种非中文语言直接漏中文，而这个脚本连报都报不出来 —— 又一次「看起来在把关」。
# 现在两个目录一起扫。
SOURCE_DIRS = [ROOT / 'Sources' / 'Coast', ROOT / 'Sources' / 'CoastKit']

def swift_files():
    for directory in SOURCE_DIRS:
        yield from sorted(directory.glob('*.swift'))

# 「有中文却没标 .t」这一检查的扫描范围。
#
# 覆盖率那一项两个目录一起扫（上面 `swift_files`），但这一项**不能**照搬 ——
# CoastKit 里大量中文是**匹配用的字面量**，翻了反而坏事：`ClashService` 拿
# 「节点 / 选择 / 代理」认策略组名、拿「规则 / 全局 / 直连」归一化模式，
# 那些串要和**核心返回的数据**对得上，不是给人看的。一股脑扫会报出 130 多条假阳性，
# 检查也就没人看了。
#
# 所以 app 层整层扫，CoastKit 只点名扫真正有界面文案的那几个文件。
UNMARKED_EXTRA = ['TrayController.swift', 'Notifier.swift']

def unmarked_scan_files():
    yield from sorted((ROOT / 'Sources' / 'Coast').glob('*.swift'))
    for name in UNMARKED_EXTRA:
        path = ROOT / 'Sources' / 'CoastKit' / name
        if path.exists():
            yield path

def used_keys():
    keys = set()
    for path in swift_files():
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"\.t', path.read_text(encoding='utf-8')):
            if CJK.search(m.group(1)):
                keys.add(unescape(m.group(1)))
    return keys

PLACEHOLDER = re.compile(r'%[@d]|%%')

# 控制台自检输出，不是界面文案，本就不该翻译。
UNTRANSLATED_OK = {'SelfTests.swift', 'I18n.swift'}

# 单行豁免：行尾写 `// i18n-ignore: <理由>`。
#
# 为什么要有：有些日志**只在设了 `COAST_GATEWAY_TESTDEV` / `COAST_POWER_TESTHOOK`
# 这类环境变量时才出现**，是我们自己在真机上排障看的，翻成 11 种语言纯属噪音。
# 这与上面整份豁免 `SelfTests.swift` 是同一类判断，只是粒度到行。
#
# 为免它变成新的盲区（这个脚本存在的全部理由就是消灭盲区），被豁免的行**逐条打印**，
# 谁拿它糊弄一眼就看得出来，而不是悄悄从分母里消失。
IGNORE_MARK = re.compile(r'//\s*i18n-ignore\b:?\s*(.*)')


def unmarked_chinese():
    """界面代码里含中文、却没标 `.t` 的字符串字面量。返回 (待办, 已豁免)。"""
    found, ignored = [], []
    for path in unmarked_scan_files():
        if path.name in UNTRANSLATED_OK:
            continue
        for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            if line.lstrip().startswith('//'):
                continue
            mark = IGNORE_MARK.search(line)
            # 同时覆盖普通串与插值串（插值串正是此前漏掉的那一类）
            for m in re.finditer(r'"([^"]*[一-鿿][^"]*)"(\s*\.t\b)?', line):
                if m.group(2):
                    continue
                if mark:
                    ignored.append((path.name, number, m.group(1), mark.group(1).strip()))
                else:
                    found.append((path.name, number, m.group(1)))
    return found, ignored


def placeholder_mismatches(keys):
    """译文里占位符与源串对不上的条目。"""
    bad = []
    for path in sorted(I18N.glob('*.json')):
        table = json.loads(path.read_text(encoding='utf-8'))
        for key in keys:
            value = table.get(key)
            if value is None:
                continue
            if sorted(PLACEHOLDER.findall(key)) != sorted(PLACEHOLDER.findall(value)):
                bad.append((path.stem, key, value))
    return bad


def main():
    keys = used_keys()
    want = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == '--missing' else None
    print(f"界面中文串共 {len(keys)} 条\n")
    problems = 0
    unmarked, ignored = unmarked_chinese()
    if ignored:
        print(f"ⓘ {len(ignored)} 处标了 // i18n-ignore（不进覆盖率分母，逐条列出以免变成盲区）：")
        for name, number, text, reason in ignored:
            print(f"     {name}:{number}  「{text[:40]}」  ← {reason or '（没写理由）'}")
        print()
    if unmarked:
        problems += 1
        print(f"❌ {len(unmarked)} 处含中文却没标 .t（不进覆盖率分母，对非中文用户直接漏中文）：")
        for name, number, text in unmarked[:20]:
            print(f"     {name}:{number}  「{text[:52]}」")
        if len(unmarked) > 20:
            print(f"     … 共 {len(unmarked)} 处")
        print()
    mismatches = placeholder_mismatches(keys)
    if mismatches:
        problems += 1
        print(f"❌ {len(mismatches)} 条译文的格式占位符与源串不一致（String(format:) 会崩）：")
        for lang, key, value in mismatches[:20]:
            print(f"     [{lang}] 「{key}」 → 「{value}」")
        print()
    for path in sorted(I18N.glob('*.json')):
        lang = path.stem
        table = json.load(open(path, encoding='utf-8'))
        missing = sorted(k for k in keys if k not in table)
        covered = len(keys) - len(missing)
        bar = '█' * round(covered / max(len(keys), 1) * 20)
        print(f"  {lang:<7} {covered:>3}/{len(keys)}  {bar}")
        if want == lang:
            print()
            for k in missing:
                print(f"    {k}")
    print("\n（zh-CN 是源语言，没有表也不需要表）")
    sys.exit(1 if problems else 0)

main()
