#!/usr/bin/env python3
"""GPU-DEMOD replay summary: last PDTIM per-stage means, PDSCH_SLOT_TIME span percentiles, GPU_EXACT_* counters,
EVMGRANT CRC-OK per RNTI. usage: gpudemod_stats.py run.log [run.log ...]"""
import collections
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def pct(v, q):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


for path in sys.argv[1:]:
    pdtim, demod, chest = "", "", ""
    span, jobs = [], []
    crc = collections.Counter()
    tot = collections.Counter()
    with open(path, errors="replace") as f:
        for raw in f:
            line = ANSI.sub("", raw)
            if "SENSING: PDTIM" in line:
                pdtim = line
            elif "GPU_EXACT_DEMOD calls" in line:
                demod = line
            elif "GPU_EXACT_CHEST calls" in line:
                chest = line
            elif "PDSCH_SLOT_TIME" in line:
                m = re.search(r"jobs=(\d+).* us=([\d.]+)", line)
                if m:
                    jobs.append(int(m.group(1)))
                    span.append(float(m.group(2)))
            elif "EVMGRANT rnti=" in line:
                m = re.search(r"EVMGRANT rnti=(0x[0-9a-f]+) \S+ crc=(\d)", line)
                if m:
                    tot[m.group(1)] += 1
                    crc[m.group(1)] += int(m.group(2))
    print("==", path)
    st = {}
    for name in ("fep", "chest", "demod", "ldpc"):
        m = re.search(name + r"\[n=(\d+) mean=([\d.]+)us", pdtim)
        if m:
            st[name] = float(m.group(2))
    print("PDTIM mean us/decode:", " ".join(f"{k}={v:.0f}" for k, v in st.items()), f"sum={sum(st.values()):.0f}",
          "n=" + (re.search(r"calls=(\d+)", pdtim).group(1) if pdtim else "0"))
    print(f"slot span us: n={len(span)} p50={pct(span, 0.5):.0f} p90={pct(span, 0.9):.0f} p99={pct(span, 0.99):.0f}"
          f" (jobs/slot p50={pct(jobs, 0.5)})")
    one = [s for s, j in zip(span, jobs) if j == 1]
    print(f"slot span us, 1-job slots: n={len(one)} p50={pct(one, 0.5):.0f} p99={pct(one, 0.99):.0f}")
    for l in (demod, chest):
        if l:
            print(l.strip().split("SENSING: ")[-1][:260])
    print("EVMGRANT crc-ok/total:", sum(crc.values()), "/", sum(tot.values()))
    for r, n in tot.most_common(8):
        print(f"  {r}: {crc[r]}/{n}")
