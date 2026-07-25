#!/usr/bin/env python3
"""生成 assets/oui.txt —— MAC OUI(前 24 位) → 厂商名 的紧凑映射，供 LanScanner 内嵌查询。

用法:
    python tools/fetch_oui.py            # 从 IEEE 官方 CSV 下载并生成
    python tools/fetch_oui.py <in.csv>   # 用本地 IEEE oui.csv 生成（离线）

输出行格式（LanScanner::loadOui 解析，制表符须恰在第 6 位）:
    AABBCC<TAB>Vendor Name

说明:
  - 数据源: http://standards-oui.ieee.org/oui/oui.csv (IEEE MA-L Assignments, 公有登记)
  - 厂商名做了轻量清洗（去公司后缀噪声、截断过长名），可按需调整。
  - 生成后请重新构建（oui.txt 经 resources.qrc 内嵌进二进制）。
"""
import csv
import io
import re
import sys

URL = "https://standards-oui.ieee.org/oui/oui.csv"
OUT = "assets/oui.txt"

# 常见法律后缀/噪声，去掉让展示更干净。
_SUFFIX = re.compile(
    r"[ ,]+(inc|inc\.|ltd|ltd\.|co\.,?ltd\.?|corporation|corp\.?|company|"
    r"technologies|technology|electronics|communications|gmbh|s\.a\.|s\.r\.l\.|"
    r"limited|llc|co\.|plc)\b\.?",
    re.IGNORECASE,
)


def clean(name: str) -> str:
    name = name.strip().strip('"')
    prev = None
    while prev != name:
        prev = name
        name = _SUFFIX.sub("", name).strip().rstrip(",").strip()
    return name[:40] if name else ""


def load_rows(text: str):
    reader = csv.DictReader(io.StringIO(text))
    for row in reader:
        assignment = (row.get("Assignment") or "").strip().upper()
        org = row.get("Organization Name") or ""
        if len(assignment) == 6 and assignment.isalnum():
            yield assignment.lower(), clean(org)


def main() -> int:
    if len(sys.argv) > 1:
        with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
            text = f.read()
    else:
        try:
            import urllib.request
            print(f"downloading {URL} ...")
            with urllib.request.urlopen(URL, timeout=60) as r:
                text = r.read().decode("utf-8", errors="replace")
        except Exception as e:  # noqa: BLE001
            print(f"download failed: {e}\n"
                  f"下载失败——可手动下载 {URL} 后运行: python tools/fetch_oui.py oui.csv",
                  file=sys.stderr)
            return 1

    seen = {}
    for prefix, vendor in load_rows(text):
        if vendor and prefix not in seen:
            seen[prefix] = vendor

    lines = [
        "# assets/oui.txt — 由 tools/fetch_oui.py 生成，请勿手改。",
        "# 格式: AABBCC<TAB>Vendor（制表符在第 6 位）。以 # 开头或列错的行被解析器忽略。",
    ]
    for prefix in sorted(seen):
        lines.append(f"{prefix}\t{seen[prefix]}")

    with io.open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {OUT}: {len(seen)} OUI entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
