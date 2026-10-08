#!/usr/bin/env python3
"""Offline energy census for a 30-kHz, four-channel sc16 PUSCH capture.

Windows are relative to raw sample zero, so low-power phases are candidate UL
windows rather than frame-synchronized UL slot labels. This is an energy gate,
not a PUSCH or DM-RS detector.
"""
import argparse
import json
from pathlib import Path

import numpy as np


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", type=Path)
    ap.add_argument("--phases", default="3,4,13,14")
    ap.add_argument("--rb", type=int, default=217)
    ap.add_argument("--min-rb", type=int, default=6)
    ap.add_argument("--db", type=float, default=6.0)
    args = ap.parse_args()
    info = json.loads((args.capture / "capture_info.json").read_text())
    rate = info["sample_rate_hz"]
    slot_samples = rate // 2000
    nslots = info["samples_per_channel"] // slot_samples
    phases = tuple(int(s) for s in args.phases.split(","))
    if rate != 122880000 or info["channels"] != 4 or args.rb * 12 > 4096:
        raise SystemExit("requires 122.88 Msps, four channels, and <=341 RB")
    powers = np.empty((4, nslots, 4), np.float32)
    iq = [np.memmap(args.capture / f"rx{a}.sc16", dtype="<i2", mode="r").reshape(-1, 2)
          for a in range(4)]
    offsets = np.array([slot_samples // 8, 3 * slot_samples // 8,
                        5 * slot_samples // 8, 7 * slot_samples // 8])
    for a in range(4):
        for start in range(0, nslots, 500):
            stop = min(start + 500, nslots)
            idx = (np.arange(start, stop)[:, None, None] * slot_samples
                   + offsets[None, :, None] + np.arange(256)[None, None, :])
            x = iq[a][idx].astype(np.float32)
            powers[a, start:stop] = np.mean(x[..., 0] ** 2 + x[..., 1] ** 2, axis=2)
    slot_power = powers.mean(axis=2)
    phase_median = [round(float(np.median(slot_power[:, p::20])), 1) for p in range(20)]
    selected = np.array([s for s in range(nslots) if s % 20 in phases], dtype=np.int64)
    fft_size = 4096
    first = fft_size - args.rb * 12 // 2
    bins = (first + np.arange(args.rb * 12)) % fft_size
    rb_power = np.empty((4, len(selected), args.rb), np.float32)
    for a in range(4):
        for start in range(0, len(selected), 100):
            s = selected[start:start + 100]
            idx = s[:, None] * slot_samples + slot_samples // 2 + np.arange(fft_size)[None, :]
            x = iq[a][idx].astype(np.float32)
            f = np.fft.fft(x[:, :, 0] + 1j * x[:, :, 1], axis=1)
            rb_power[a, start:start + len(s)] = np.mean(
                np.abs(f[:, bins].reshape(-1, args.rb, 12)) ** 2, axis=2)
    baseline = np.median(rb_power, axis=1)
    ratio = rb_power / np.maximum(baseline[:, None, :], 1)
    strong = (ratio > 10 ** (args.db / 10)).sum(axis=0) >= 2
    hits = []
    for i, slot in enumerate(selected):
        run = np.convolve(strong[i].astype(np.int8),
                          np.ones(args.min_rb, np.int8), mode="valid")
        positions = np.flatnonzero(run == args.min_rb)
        if len(positions):
            hits.append({"window": int(slot), "first_rb": int(positions[0])})
    print(json.dumps({"capture": str(args.capture), "windows": nslots,
                      "candidate_phases_mod20": phases,
                      "candidate_windows": len(selected),
                      "phase_median_power": phase_median,
                      "min_rb": args.min_rb, "threshold_db": args.db,
                      "min_rx": 2, "hits": len(hits), "first_hits": hits[:20]}, indent=2))


if __name__ == "__main__":
    main()
