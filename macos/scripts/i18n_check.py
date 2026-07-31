#!/usr/bin/env python3
"""翻译覆盖率检查。

用法：python3 scripts/i18n_check.py [--missing <lang>]

界面字符串以**中文源串**为 key（见 Sources/Coast/I18n.swift），所以「哪些串需要翻译」
可以直接从源码里扫出来：凡是形如 "…中文…".t 的字面量都是。

漏翻的后果不是崩溃、也不是显示 key，而是**那一条回落成中文** —— 混在译文里很不显眼，
所以需要这个脚本主动报数，而不是等用户发现。
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

def main():
    keys = used_keys()
    want = sys.argv[2] if len(sys.argv) > 2 and sys.argv[1] == '--missing' else None
    print(f"界面中文串共 {len(keys)} 条\n")
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

main()
