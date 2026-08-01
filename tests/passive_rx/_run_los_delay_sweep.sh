#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Does the ~40 m systematic range bias TRACK an injected LOS delay?
#
# sensing_channel.c places the direct path at tau = los_delay_samples and each target at
# tau = los_delay_samples + dR/c*fs. The target-to-LOS separation is therefore exactly dR, whatever
# los_delay_samples is. That makes the sweep a clean discriminator:
#
#   H1  the receiver reports (absolute delay - a fixed receiver-side offset d)
#       -> bias = (los_delay - d) * c/fs, i.e. bias moves +2.44 m per injected sample, and nulls at
#          los_delay = d. A constant d would most likely be the UE's FFT window sitting inside the
#          cyclic prefix, i.e. a real, explainable receive-chain offset.
#
#   H2  the receiver references ranges to the MEASURED LOS position
#       -> bias is independent of los_delay and stays put. The offset is then intrinsic to the
#          processing (window group delay, sub-bin interpolation, ECA) and the sweep rules out the
#          whole "range origin bookkeeping" family in one shot.
#
# Four points (0/8/16/32 samples) rather than two, so the answer is a SLOPE and not a coincidence:
# H1 predicts slope +1.00 sample-per-sample, H2 predicts 0.00.
#
# Usage: ./_run_los_delay_sweep.sh [duration_s] [out_root]
set -u
DUR="${1:-300}"
OUT_ROOT="${2:-/tmp/los_sweep}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$OUT_ROOT"
: > "$OUT_ROOT/RESULTS.txt"

run_point() { # run_point <label> <CONF_TAG> <injected_samples>
  local label="$1" tag="$2" inj="$3" out="$OUT_ROOT/$1"
  local conf="$SCRIPT_DIR/ue.passive${tag}.100mhz.conf"
  local rep
  rep=$(sed -n 's/^[[:space:]]*report_path[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$conf" | head -1)
  echo "=== $label: los_delay_samples=$inj  conf=$(basename "$conf")  report=$rep ===" \
    | tee -a "$OUT_ROOT/RESULTS.txt"
  # Reports are opened with ios::app and never cleared by the softmodem -- a leftover file would be
  # scored as if it belonged to this point.
  rm -f "$rep"
  NUM_RX=1 NUM_UE=3 BW100=1 IPERF_RATE=6M CONF_TAG="$tag" \
    "$SCRIPT_DIR/run_passive_rx.sh" "$DUR" "$out" > "$out.log" 2>&1
  {
    sed -n '/DL LIVENESS/,/timeline:/p' "$out.log" | sed 's/^/  /'
    if [ -f "$rep" ] && [ -f "$out/ue_rx1.log" ]; then
      cp -f "$rep" "$out/reports.jsonl"
      python3 "$SCRIPT_DIR/score_range_bias.py" "$rep" "$out/ue_rx1.log" | sed 's/^/  /'
    else
      echo "  SCORING SKIPPED (missing $rep or $out/ue_rx1.log)"
    fi
    echo
  } >> "$OUT_ROOT/RESULTS.txt"
}

run_point los00 ".aoa"    0
run_point los08 ".los8"   8
run_point los16 ".los16" 16
run_point los32 ".los32" 32

echo "=== sweep done ==="
cat "$OUT_ROOT/RESULTS.txt"
