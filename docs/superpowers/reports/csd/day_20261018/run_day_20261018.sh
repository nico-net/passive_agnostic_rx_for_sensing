#!/bin/bash
# Daytime on-air A/B/C 2026-10-18: G = t26gE (DL default) vs L = t26uL (UL latency levers) vs C = t26uC (L + UL consumers on big cores 5,6).
# One binary for all arms: csd/int16-ulrfsim d45b8db2fe build_t26 (contains csd/int16 b5c60c2dc8 DL code). 150 s, n=3 per arm per cell, interleaved.
set -u
O=/home/nicola/NICOLA/wt/rr-orchestration; cd $O
SPECS="$(cat ota/specs_day_20261018.txt)" BDIR=/home/nicola/NICOLA/wt/rr-csd-int16ulr/cmake_targets/ran_build/build_t26 ota/ota_window_gen.sh
