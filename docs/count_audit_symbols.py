#!/usr/bin/env python3
"""Recompute docs/passive_branch_globals_audit.md's H/B/S/W summary table from its own detail
tables, so the summary can never silently drift from what the rows actually say.

For every row in every "## `<file>`" section: extract each backtick-quoted span in the Symbol(s)
column, split on ';' then on top-level ',' (bracket-depth-aware, so an array size like
`g_rbhist[3][N]` is not split at an internal comma), count the resulting declarator names, and sum
by the row's Class column (H/B/S/W). A row's Symbol(s) cell must therefore contain ONLY real
declared symbol names in backticks -- move any file:line citation, function-name cross-reference,
or bare type annotation outside the backticks (plain parenthetical text is ignored by this script)
before adding a new row, or it will silently inflate the count.

Usage: python3 docs/count_audit_symbols.py docs/passive_branch_globals_audit.md
"""
import re
import sys


def top_level_split(s, sep):
    out, depth, cur = [], 0, []
    opens, closes = "[({<", "])}>"
    for ch in s:
        if ch in opens:
            depth += 1
        elif ch in closes:
            depth = max(0, depth - 1)
        if ch == sep and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append("".join(cur))
    return out


def count_span(span):
    span = span.strip()
    if not span:
        return 0
    stmts = [s for s in top_level_split(span, ";") if s.strip()] or [span]
    return sum(len([g for g in top_level_split(stmt, ",") if g.strip()]) for stmt in stmts)


def count_row_symbol_cell(cell):
    spans = re.findall(r"`([^`]*)`", cell)
    if not spans:
        return None  # no backticks: needs a manual look, not silently skipped
    return sum(count_span(sp) for sp in spans)


def main(path):
    lines = open(path, encoding="utf-8").read().split("\n")
    section_starts = [i for i, l in enumerate(lines) if l.startswith("## `") or l.startswith("## P03 additions")]
    section_starts.append(next(i for i, l in enumerate(lines) if l.startswith("## Summary counts")))

    for idx in range(len(section_starts) - 1):
        start, end = section_starts[idx], section_starts[idx + 1]
        header = lines[start]
        counts = {"H": 0, "B": 0, "S": 0, "W": 0}
        unparsed = []
        for l in lines[start:end]:
            if not l.startswith("|") or l.startswith("|---"):
                continue
            cells = [c.strip() for c in l.strip("|").split("|")]
            if len(cells) < 3 or cells[0] == "Line(s)":
                continue
            cls = next((c for c in cells if c in ("H", "B", "S", "W")), None)
            if cls is None:
                continue
            n = count_row_symbol_cell(cells[1])
            if n is None:
                unparsed.append(l)
                continue
            counts[cls] += n
        print(f"{header}\n  H={counts['H']} B={counts['B']} S={counts['S']} W={counts['W']}  total={sum(counts.values())}")
        for l in unparsed:
            print(f"  UNPARSED (no backticks in Symbol(s) cell): {l[:160]}")
        print()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "docs/passive_branch_globals_audit.md")
