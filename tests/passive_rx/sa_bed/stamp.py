#!/usr/bin/env python3
"""Prefix each process log line with a Unix wall timestamp for cross-process scoring."""
import sys
import time

with open(sys.argv[1], "w", buffering=1) as log:
    for line in sys.stdin:
        log.write(f"{time.time():.6f} {line}")
