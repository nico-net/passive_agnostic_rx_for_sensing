#!/usr/bin/env python3
"""TIM-PDSCH replay summary: first CORESET bank add, solved RNTIs, full-decode CRC per RNTI, MCS0-4 >= 10 dB CRC.
usage: timpdsch_stats.py run.log [run.log ...]   (offline; reads logs only)
Times are RF seconds = TIMEMUT consumed= samples / 122.88 MHz of the last TIMEMUT line before the event."""
import re
import sys
from collections import Counter

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def summary(path):
    consumed, banks, solved, ok, tot = 0, [], [], Counter(), Counter()
    lo_ok = lo_n = 0
    eof = False
    for raw in open(path, errors="replace"):
        line = ANSI.sub("", raw)
        m = re.search(r"TIMEMUT .* consumed=(\d+)", line)
        if m:
            consumed = int(m.group(1))
        if "bank add index=" in line and "multi-CORESET" in line:
            banks.append(consumed / 122.88e6)
        m = re.search(r"PDSCH_SOLVER rnti=(0x[0-9a-f]+) SOLVED", line)
        if m and m.group(1) not in solved:
            solved.append(m.group(1))
        if "SENSING: EVMGRANT" in line and " probe=0 " in line:
            d = dict(re.findall(r"(\w+)=(\S+)", line))
            if d.get("rnti") == "0xffff":
                continue
            tot[d["rnti"]] += 1
            ok[d["rnti"]] += d["crc"] == "1"
            if int(d["mcs"]) <= 4 and float(d["snr"]) >= 10:
                lo_n += 1
                lo_ok += d["crc"] == "1"
        if "SHUTDOWN" in line or "exited normally" in line:
            eof = True
    full = " ".join(f"{r}:{ok[r]}/{tot[r]}" for r in sorted(tot, key=lambda r: -ok[r]) if ok[r])
    first = f"{banks[0]:.2f}" if banks else "none"
    lo = f"{100.0 * lo_ok / lo_n:.1f}% ({lo_ok}/{lo_n})" if lo_n else "n/a"
    print(f"{path}\n  first bank add {first} s | solved {len(solved)} {' '.join(solved)}\n"
          f"  CRC-OK full decodes per RNTI: {full or 'none'}\n  MCS0-4 >= 10 dB CRC: {lo} | shutdown seen: {eof}")


for p in sys.argv[1:]:
    summary(p)
