#!/usr/bin/env python3
"""scale_tile.py SRC DST GAIN N : DST = N tiles of the 1 s capture SRC with digital gain GAIN (clipped to int16); emulates a higher RX gain
for the fixed SSS_METRIC_FLOOR_NR=30000 in sss_nr.h. Same capture_info.json except samples_per_channel."""
import numpy as np, json, sys, os
src, dst, g, n = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4])
os.makedirs(dst, exist_ok=True); clip = 0
for c in range(4):
    x = np.fromfile(f"{src}/rx{c}.sc16", dtype=np.int16).astype(np.float32) * g
    clip += int((np.abs(x) > 32767).sum()); y = np.clip(np.rint(x), -32768, 32767).astype(np.int16)
    with open(f"{dst}/rx{c}.sc16", "wb") as f:
        for _ in range(n): y.tofile(f)
j = json.load(open(f"{src}/capture_info.json")); j["samples_per_channel"] *= n; json.dump(j, open(f"{dst}/capture_info.json", "w"))
print("clipped samples", clip)
