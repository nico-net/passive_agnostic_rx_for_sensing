#!/usr/bin/env python3
"""dtq_sum.py <replay dir>...: per run, PDSCH stage means over the whole run and over the second half (PDTIM line diff), bodies per decoded grant, CRC."""
import re, sys
ST = ("fep", "chest", "demod", "ldpc")
RE = re.compile(r"(fep|chest|alloc|demod|ldpc)\[n=(\d+) mean=([\d.]+)us")
def tot(line):
    return {s: (int(n), float(m) * int(n)) for s, n, m in RE.findall(line)}
for d in sys.argv[1:]:
    L = [re.sub(r"\x1b\[[0-9;]*m", "", l) for l in open(d + "/run.log", errors="replace")]
    pd = [l for l in L if "PDTIM calls=" in l]
    q = [l for l in L if "SENSING: PDSCHQ queued" in l]
    if not pd or not q:
        print(d, "no data"); continue
    last = tot(pd[-1]); mid = tot(pd[len(pd) // 2])
    calls = int(re.search(r"calls=(\d+)", pd[-1]).group(1))
    m = re.search(r"decoded=(\d+) crc_ok=(\d+)", q[-1]); dec, ok = int(m.group(1)), int(m.group(2))
    st = re.search(r"dropped\[full=(\d+) stale=(\d+)\]", q[-1]).groups()
    full = {s: last[s][1] / max(last[s][0], 1) for s in ST}
    half = {s: (last[s][1] - mid[s][1]) / max(last[s][0] - mid[s][0], 1) for s in ST}
    pct = [l for l in L if "PDTIMPCT" in l][-1:]
    ul = [l for l in L if "UL_PUSCH_SOLVER grants" in l][-1:]
    sc = [l for l in L if "DMRS_SCAN full" in l][-1:]
    print(d.split("/")[-1], f"bodies={calls} decoded={dec} bodies/grant={calls/max(dec,1):.2f} crc_ok={ok} ({100*ok/max(dec,1):.1f}%) stale={st[1]} full={st[0]}")
    print("   mean all :", " ".join(f"{s}={full[s]:.0f}" for s in ST), f"sum={sum(full.values()):.0f}us")
    print("   mean 2nd½:", " ".join(f"{s}={half[s]:.0f}" for s in ST), f"sum={sum(half.values()):.0f}us")
    if pct: print("   ", re.sub(r".*PDTIMPCT calls=\d+ ", "pct ", pct[0]).strip())
    if ul: print("   ", re.sub(r".*SENSING: ", "", ul[0]).strip()[:150])
    if sc: print("   ", re.sub(r".*(full scans=)", r"\1", sc[0]).strip()[:120])
