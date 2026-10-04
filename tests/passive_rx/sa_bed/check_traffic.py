#!/usr/bin/env python3
"""Fail the bed smoke check unless recent receiver traffic reaches 100 C-RNTI grants/s."""
import argparse
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("metrics", type=Path)
    args = ap.parse_args()
    # Collect a new ten-second interval, including in short smoke runs.
    time.sleep(11)
    rate = recent_rate(read_jsonl(args.metrics))
    if rate is None or rate < 100:
        ap.exit(1, f"traffic smoke failed: C-RNTI grants/s={rate}, need >=100; check ogstun route and UE netns\n")
    print(f"traffic smoke: C-RNTI grants/s={rate:.2f}")


if __name__ == "__main__":
    main()
