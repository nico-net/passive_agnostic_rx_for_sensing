#!/usr/bin/env python3
"""Summarise 'SENSING: ULEN' lines of a run.log per cell (PCI tag "[cN]" prefix when present).
usage: python3 -I tools/ulen_summary.py run.log
Rule: rise~0 and corr~null = UE too weak here; rise>>0 and corr~null = energy but pilot/allocation wrong; corr>>null at best_off!=0 = timing."""
import re, sys, statistics as st
from collections import Counter, defaultdict

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
CELL = re.compile(r"\[(c\d+)\]")
KV = re.compile(r"(\w+)=(\S+)")


def mcls(m):
    return "mcs0-4" if m <= 4 else ("mcs5-16" if m <= 16 else "mcs17+")


def med(v):
    return "%.3f" % st.median(v) if v else "n/a"


def report(name, rows):
    rise = [r["rise_db"] for r in rows if r["rise_db"] > -90]
    print("%s n=%d" % (name, len(rows)))
    print("  rise_db median=%s frac>3dB=%s (n=%d)" % (med(rise), ("%.2f" % (sum(x > 3 for x in rise) / len(rise))) if rise else "n/a", len(rise)))
    print("  dmrs_corr median=%s null median=%s frac(corr>2*null)=%.2f" % (
        med([r["dmrs_corr"] for r in rows]), med([r["dmrs_corr_null"] for r in rows]),
        sum(r["dmrs_corr"] > 2 * r["dmrs_corr_null"] for r in rows) / len(rows)))
    print("  best_off:", " ".join("%d:%d" % kv for kv in sorted(Counter(int(r["best_off"]) for r in rows).items())))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    cells = defaultdict(list)
    with open(sys.argv[1], errors="replace") as f:
        for line in f:
            line = ANSI.sub("", line)
            if "SENSING: ULEN " not in line:
                continue
            c = CELL.search(line)
            d = dict(KV.findall(line.split("SENSING: ULEN ", 1)[1]))
            try:
                row = {k: float(d[k]) for k in ("mcs", "rise_db", "dmrs_corr", "dmrs_corr_null", "best_off")}
            except (KeyError, ValueError):
                continue
            cells[c.group(1) if c else "all"].append(row)
    if not cells:
        sys.exit("no ULEN lines")
    for cell in sorted(cells):
        rows = cells[cell]
        report("cell " + cell, rows)
        for cls in ("mcs0-4", "mcs5-16", "mcs17+"):
            sub = [r for r in rows if mcls(int(r["mcs"])) == cls]
            if sub:
                report("  " + cls, sub)


main()
