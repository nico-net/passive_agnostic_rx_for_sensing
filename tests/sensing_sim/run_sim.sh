#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Fictitious-gNB sensing loop: launches nr-softmodem (gNB, --do-ra, core-less) and nr-uesoftmodem
# (UE, --do-ra) over the rfsimulator loopback, with the synthetic moving-target sensing channel
# (openair1/SIMULATION/TOOLS/sensing_channel.{h,c}) injected on the downlink. See README.md.
#
# Usage: ./run_sim.sh [duration_s] [out_dir] [ue_conf_name]
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../../cmake_targets/ran_build/build}"
DURATION="${1:-30}"
OUT_DIR="${2:-/tmp/sensing_sim}"
UE_CONF="${3:-ue.sensing.rfsim.conf}"

mkdir -p "$OUT_DIR"
GNB_LOG="$OUT_DIR/gnb.log"
UE_LOG="$OUT_DIR/ue.log"

for exe in nr-softmodem nr-uesoftmodem; do
  if [ ! -x "$BUILD_DIR/$exe" ]; then
    echo "ERROR: $BUILD_DIR/$exe not found or not executable. Build it first:" >&2
    echo "  cmake --build $BUILD_DIR --target nr-softmodem nr-uesoftmodem -- -j\$(nproc)" >&2
    exit 1
  fi
done

echo "gNB log:  $GNB_LOG"
echo "UE log:   $UE_LOG"
echo "Duration: ${DURATION}s"

"$BUILD_DIR/nr-softmodem" -O "$SCRIPT_DIR/gnb.sensing.rfsim.conf" --do-ra --rfsim >"$GNB_LOG" 2>&1 &
GNB_PID=$!
trap 'kill "$GNB_PID" "$UE_PID" 2>/dev/null || true' EXIT

sleep 3 # let the gNB open the rfsim server socket before the UE tries to connect

# Carrier matches gnb.sensing.rfsim.conf: band 78, 106 PRB, numerology 1 (30 kHz SCS), PCI 2.
# -C is the carrier centre frequency (dl_absoluteFrequencyPointA=3600 MHz + BW/2 = 3600+19.08 MHz).
"$BUILD_DIR/nr-uesoftmodem" -O "$SCRIPT_DIR/$UE_CONF" --do-ra --rfsim \
  -r 106 --numerology 1 --band 78 -C 3619080000 \
  --rfsimulator.[0].serveraddr 127.0.0.1 >"$UE_LOG" 2>&1 &
UE_PID=$!

echo "gNB PID=$GNB_PID  UE PID=$UE_PID"
sleep "$DURATION"

echo "Stopping..."
kill "$GNB_PID" "$UE_PID" 2>/dev/null || true
sleep 2
kill -9 "$GNB_PID" "$UE_PID" 2>/dev/null || true
trap - EXIT

echo ""
echo "=== RA outcome ==="
grep -m1 "RA procedure succeeded" "$UE_LOG" || echo "(not found -- check $UE_LOG)"
echo ""
# NOTE: the DL channel model (and hence the synthetic sensing channel, and its ground-truth log)
# is applied on the CLIENT side of the rfsim loopback -- i.e. inside nr-uesoftmodem, not
# nr-softmodem. Both ground truth and detections land in the UE log. See README.md "Which process
# logs what" for the source-level confirmation (radio/rfsimulator/simulator.cpp's
# legacy_model_name / set_channeldesc_direction).
echo "=== Ground truth (sensing_channel, UE log) ==="
grep "SENSING_CHANNEL gt:" "$UE_LOG" | tail -5
echo ""
echo "=== Detections (sensing report, UE log) ==="
grep "SENSING: CPI #" "$UE_LOG" | tail -5
echo ""
echo "Full logs: $GNB_LOG / $UE_LOG"
echo "DetectionReport JSON-lines (if any): $OUT_DIR/oaiue_reports.jsonl"
