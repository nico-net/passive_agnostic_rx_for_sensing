#!/usr/bin/env python3
"""Prefix each stdin line with a Unix wall timestamp (cross-process scoring); write to argv[1]."""
import sys
import time

with open(sys.argv[1], "w", buffering=1, errors="replace") as log:
    for line in sys.stdin.buffer:
        log.write(f"{time.time():.6f} {line.decode(errors='replace')}")
