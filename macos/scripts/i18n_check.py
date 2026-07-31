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

2. **译文与源串的格式占位符不一致。** 这些串要喂给 `String(format:)`，占位符数量或
   类型对不上时它会按错误的类型去读参数 —— 直接崩，而且**只在那一种语言下崩**，
   开发机上（中文）永远复现不了。
"""
import json, re, sys, pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
I18N = ROOT.parent / 'clashauto-c++' / 'assets' / 'i18n'
CJK = re.compile(r'[一-鿿]')

def used_keys():
    keys = set()
    for path in (ROOT / 'Sources' / 'Coast').glob('*.swift'):
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"\.t', path.read_text(encoding='utf-8')):
            if CJK.search(m.group(1)):
                keys.add(m.group(1))
    return keys

PLACEHOLDER = re.compile(r'%[@d]|%%')

# 控制台自检输出，不是界面文案，本就不该翻译。
UNTRANSLATED_OK = {'SelfTests.swift', 'I18n.swift'}


def unmarked_chinese():
    """界面代码里含中文、却没标 `.t` 的字符串字面量。"""
    found = []
    for path in (ROOT / 'Sources' / 'Coast').glob('*.swift'):
        if path.name in UNTRANSLATED_OK:
            continue
        for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            if line.lstrip().startswith('//'):
                continue
            # 同时覆盖普通串与插值串（插值串正是此前漏掉的那一类）
            for m in re.finditer(r'"([^"]*[一-鿿][^"]*)"(\s*\.t\b)?', line):
                if not m.group(2):
                    found.append((path.name, number, m.group(1)))
    return found


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
    unmarked = unmarked_chinese()
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
