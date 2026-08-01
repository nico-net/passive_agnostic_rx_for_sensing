#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Controlled A/B for the passive data-aided PDSCH source (PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md
# §B.6 step 3): does reconstructing X at comb-1 over every data RE actually improve BEARING
# accuracy, which is what §B.1 claims and what §A.3 item 1 names as the thing gating fusion quality?
#
# Two arms, identical in every respect except the source set:
#   data   : ue.passive{,2}.aoa.100mhz.conf        sources = csi_rs,pdsch_dmrs_blind,pdsch_data
#   nodata : ue.passive{,2}.aoanodata.100mhz.conf  sources = csi_rs,pdsch_dmrs_blind
# The control arm still DECODES (pdcch_blind_monitor_pdsch decode=1) so both arms pay the same LDPC
# cost and the comparison isolates the CFR submission, not the CPU load.
#
# Arms run SEQUENTIALLY, alternating, on the same machine. This harness has large run-to-run
# variance (documented in CLAUDE.md §10-11: a 3-run spread of 21.7/0.0/29.4 % at ~90 s), so single
# runs are not a result -- REPS alternating pairs is the minimum that lets machine-load drift be
# shared between the arms rather than land on one of them.
#
# Usage: ./_run_pdsch_data_ab.sh [duration_s] [reps] [out_root]
set -u
DUR="${1:-1800}"
REPS="${2:-2}"
OUT_ROOT="${3:-/tmp/pdsch_data_ab}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "$OUT_ROOT"
: > "$OUT_ROOT/RESULTS.txt"

run_arm() {
  local tag="$1" rep="$2" out="$OUT_ROOT/${1}_rep${2}"
  echo "=== arm=$tag rep=$rep dur=${DUR}s -> $out ===" | tee -a "$OUT_ROOT/RESULTS.txt"
  # Report files are opened with ios::app and are never cleared by the softmodem -- leftovers from a
  # previous run would be scored as if they were this one's (the bug behind CLAUDE.md §10's
  # retracted "491 fused track updates"). Clear them explicitly per arm.
  rm -f /tmp/passive_rx/passive_reports*.jsonl
  NUM_RX=1 NUM_UE=3 BW100=1 IPERF_RATE=6M CONF_TAG="$3" \
    "$SCRIPT_DIR/run_passive_rx.sh" "$DUR" "$out" > "$out.log" 2>&1
  # run_passive_rx.sh does NOT call check_detection.py -- it prints its own lighter summary. The
  # ground-truth-referenced per-target detection rate and the bearing error (the metric this whole
  # A/B exists to measure, PASSIVE_PDSCH_DATA_AIDED_HANDOVER.md §B.6 step 3) only come out of
  # check_detection.py, so run it here explicitly against this arm's own report file.
  local rep_file
  rep_file=$(sed -n 's/^[[:space:]]*report_path[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
             "$SCRIPT_DIR/ue.passive${3}.100mhz.conf" | head -1)
  {
    echo "--- arm=$tag rep=$rep ---"
    grep -E "CPI duration|CFR occupancy|pdsch_decode\[|detections carrying azimuth" "$out.log" | sed 's/^/  /'
    if [ -f "$rep_file" ] && [ -f "$out/ue_rx1.log" ]; then
      echo "  check_detection ($rep_file):"
      # RANGE TOLERANCE 40 m, not the 12 m default. There is a ~40 m SYSTEMATIC bias between the
      # reported bistatic range and ground truth (measured 2026-07-30: median -38 m and -42 m on two
      # independent runs, constant across objects at 120 m and 360 m, so a fixed ~16-sample delay
      # offset, not a scale error). The simulator is not the source -- sensing_channel.c places the
      # target tap at exactly los_delay + dR/c*fs -- so it is a receiver range-axis calibration
      # defect, pre-existing and unrelated to pdsch_data. At 12 m it scores 0/77 while the targets
      # are demonstrably being found with 0.04-0.17 deg median bearing error; at 40 m it scores 18%.
      # Both arms carry the SAME bias, so it cancels in the comparison this script exists to make.
      # Revisit the tolerance if the bias is ever fixed -- do not treat 40 m as an accuracy claim.
      python3 "$SCRIPT_DIR/check_detection.py" "$rep_file" "$out/ue_rx1.log" 40 4.0 2>&1 | sed 's/^/    /'
      # Keep this arm's reports; RESULTS.txt only summarises, and a re-score later needs the raw file.
      cp -f "$rep_file" "$out/reports.jsonl" 2>/dev/null || true
    else
      echo "  check_detection SKIPPED (missing $rep_file or $out/ue_rx1.log)"
    fi
    echo
  } >> "$OUT_ROOT/RESULTS.txt"
}

for r in $(seq 1 "$REPS"); do
  run_arm data   "$r" ".aoa"
  run_arm nodata "$r" ".aoanodata"
done

echo "=== done; summary in $OUT_ROOT/RESULTS.txt ==="
cat "$OUT_ROOT/RESULTS.txt"
