#!/usr/bin/env python3
"""Fail the bed smoke check unless recent receiver traffic reaches 100 C-RNTI grants/s."""
import argparse
import re
from pathlib import Path
import time

from score_r13 import read_jsonl


def recent_rate(rows, window_s=10):
    rows = [r for r in rows if "t_mono_ns" in r and "pdcch_accepts_c" in r]
    if len(rows) < 2:
        return None
    last = rows[-1]
    # A stopped receiver must not pass using old high-rate samples.
    if time.monotonic_ns() - last["t_mono_ns"] > 5e9:
        return None
    first = next((r for r in reversed(rows[:-1])
                  if last["t_mono_ns"] - r["t_mono_ns"] >= window_s * 1e9), None)
    if first is None:
        return None
    elapsed = (last["t_mono_ns"] - first["t_mono_ns"]) / 1e9
    return (last["pdcch_accepts_c"] - first["pdcch_accepts_c"]) / elapsed


def gnb_rate(log, window_s=10):
    """DL grants/s to the busiest UE from the gNB's own per-UE `dlsch_rounds` stats (any DCI format:
    the receiver's pdcch_accepts_c counts DCI 1_0 only, so it reads ~0 for a DCI 1_1 UE)."""
    pat = re.compile(r"^(\d+\.\d+) .*UE ([0-9a-f]{4}): dlsch_rounds (\d+)/")
    per = {}
    for line in Path(log).read_text(errors="replace").splitlines():
        m = pat.match(line)
        if m:
            per.setdefault(m.group(2), []).append((float(m.group(1)), int(m.group(3))))
    best = None
    for rows in per.values():
        if time.time() - rows[-1][0] > 15:  # stale: gNB stopped printing
            continue
        first = next((r for r in reversed(rows[:-1]) if rows[-1][0] - r[0] >= window_s), None)
        if first:
            rate = (rows[-1][1] - first[1]) / (rows[-1][0] - first[0])
            best = rate if best is None else max(best, rate)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("metrics", type=Path)
    ap.add_argument("gnb_log", type=Path)
    args = ap.parse_args()
    # Collect a new ten-second interval, including in short smoke runs.
    time.sleep(11)
    rx = recent_rate(read_jsonl(args.metrics))
    rate = gnb_rate(args.gnb_log)
    if rate is None or rate < 100:
        ap.exit(1, f"traffic smoke failed: gNB DL grants/s={rate} (rx DCI1_0 C accepts/s={rx}), need >=100; check ogstun route and UE netns\n")
    print(f"traffic smoke: gNB DL grants/s={rate:.2f} (rx DCI1_0 C accepts/s={rx})")


if __name__ == "__main__":
    main()
