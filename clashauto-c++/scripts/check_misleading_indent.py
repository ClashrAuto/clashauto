#!/usr/bin/env python3
"""无大括号的控制语句后面跟了两条同缩进语句 —— 只有第一条真正被守卫。

    if (n->arp)
        n->arp->stopSpoof(mac);
        n->arp->healIsolation(...);   # ← 不在 if 里，无条件执行

GCC/Clang 的 -Wmisleading-indentation 抓的就是这个，CMakeLists 里已把它设为错误。
**但那道门禁在 CI 里有个洞**：Windows 专属源（NetStack.cpp、L2Endpoint_win.cpp…）
在 CI 中只被 MSVC 编译，而 MSVC 没有等价警告；macOS 专属源则两边 GCC 都碰不到。
2026-08-06 一晚上踩了三处，其中两处正好在 Windows 专属文件里 —— 编译期门禁对它们
无效。这个脚本与平台无关，把那块补上。

判据刻意保守（宁可漏报也不误报，一个会误报的门禁活不过三次）：
  · 控制语句的条件必须在本行闭合，且本行不含 `{`（排除多行条件与已有代码块）
  · 第一条语句必须自身闭合并以 `;` 结尾（排除跨行调用）
  · 第二条不能是续行/闭合符号

退出码 0 = 干净，1 = 有命中。
"""
import io
import pathlib
import re
import sys

HEAD = re.compile(r"^\s*(if|for|while)\s*\(")
CONT_START = ("}", "else", "#", "||", "&&", ".", "?", ":", ",", ")")


def indent(s: str) -> int:
    return len(s) - len(s.lstrip())


def strip_comment(s: str) -> str:
    """去掉行尾 // 注释（不误伤字符串里的 //，例如 URL）。

    ★ 这一步不是修饰。少了它，`foo(); // 说明` 这种行就不以 `;` 结尾，判据静默失效 ——
      本脚本第一版就是这样，变异测试当场证明它**抓不到自己要抓的 bug**。
    """
    q = None
    for i, ch in enumerate(s):
        if q:
            if ch == "\\":
                q = q  # 跳过转义的下一个字符由 i+1 自然处理
            elif ch == q:
                q = None
        elif ch in "\"'":
            q = ch
        elif ch == "/" and i + 1 < len(s) and s[i + 1] == "/":
            return s[:i]
    return s


def balanced(s: str) -> bool:
    return s.count("(") == s.count(")")


def scan(root: pathlib.Path):
    hits = []
    files = sorted(set(list(root.rglob("*.cpp")) + list(root.rglob("*.h"))
                       + list(root.rglob("*.mm"))))
    for p in files:
        sp = str(p).replace("\\", "/")
        if "/build-" in sp or "/rust/" in sp or "/third_party/" in sp:
            continue
        try:
            lines = p.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for i, raw in enumerate(lines):
            ln = strip_comment(raw).rstrip()
            if not HEAD.match(ln) or "{" in ln or not balanced(ln) or not ln.endswith(")"):
                continue
            body = [j for j in range(i + 1, min(i + 6, len(lines)))
                    if lines[j].strip() and not lines[j].lstrip().startswith("//")]
            if len(body) < 2:
                continue
            a = strip_comment(lines[body[0]]).rstrip()
            b = strip_comment(lines[body[1]]).rstrip()
            if not a or not b:
                continue
            if a.lstrip().startswith("{") or not balanced(a) or not a.endswith(";"):
                continue
            if b.lstrip().startswith(CONT_START):
                continue
            if indent(a) > indent(ln) and indent(b) == indent(a):
                hits.append((p.relative_to(root), i + 1, ln.strip(), a.strip(), b.strip()))
    return hits


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    hits = scan(root)
    out = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    for path, line, head, first, second in hits:
        out.write(f"{path}:{line}: 第二条语句没有被这个控制语句守卫\n")
        out.write(f"    {head}\n        {first}\n        {second}   <-- 无条件执行\n\n")
    if hits:
        out.write(f"发现 {len(hits)} 处。加大括号，或把第二条移出去对齐。\n")
        out.flush()
        return 1
    out.write("misleading-indentation 扫描：干净\n")
    out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
