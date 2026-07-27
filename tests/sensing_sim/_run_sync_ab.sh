#!/bin/bash
# Controlled A/B: sync_correction ON vs OFF, everything else identical.
#
# WHY THIS EXISTS. The standing claim "sync correction is net-negative" was measured 2026-07-23 on a
# DIFFERENT scene and a much earlier pipeline (before ECA+, matrix completion, cfar_per_row,
# subbin_interp, sub-slot sampling, the sparse-tap rfsim fix and the AoA path all landed). Before
# either "fixing" sync or running a big validation with it on, establish whether it is still harmful
# AT ALL on the current code -- the premise may simply have expired.
#
# Single receiver (rx1) on purpose: this asks a per-receiver DSP question (does correcting STO/CFO/SFO
# help or hurt detection?), which needs no fusion and runs ~3x cheaper than the 3-receiver harness.
#
# Alternates ON/OFF rep by rep rather than running all-ON then all-OFF, so any slow drift in machine
# load is shared evenly between the two arms instead of loading onto one of them.
#
# Usage: sudo setsid ./_run_sync_ab.sh [pairs] [dur_s] [scene] > /tmp/ms/syncab.log 2>&1
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
PAIRS=${1:-4}
DUR=${2:-200}
SC=${3:-four}
ROOT=/tmp/ms/sync_ab
rm -rf "$ROOT"; mkdir -p "$ROOT"
RES=$ROOT/results.csv
echo "rep,sync,det_cpis,track_cpis,raw_total,raw_match,raw_prec,trk_total,trk_match,trk_prec,cov0,cov1,cov2,cov3,fly_mean,locked_mean" > "$RES"

for rep in $(seq 1 "$PAIRS"); do
  for sync in 0 1; do
    OUT=$ROOT/rep${rep}_sync${sync}
    echo ""; echo "######## rep $rep  sync_correction=$sync  ($(date +%H:%M:%S)) ########"
    for n in 2 3 4; do ip netns del oai_isac_gnb$n 2>/dev/null; ip netns del oai_isac_ue$n 2>/dev/null; done

    # Build a per-run conf with ONLY sync_correction changed.
    c="_syncab_run.conf"
    cp "_scene_${SC}_rx1.conf" "$c"
    sed -i "s/^  sync_correction *= *[01];/  sync_correction = $sync;/" "$c"
    grep -q "sync_correction = $sync;" "$c" || { echo "FAILED to set sync_correction=$sync"; exit 1; }

    UE_NB_ANT_RX=4 ./_run_mot_variant.sh "$c" "$OUT" "$DUR" >"$ROOT/rep${rep}_sync${sync}.log" 2>&1
    rm -f "$c"

    # Per-receiver detection + track quality (the metric the original 2026-07-23 ablation used).
    python3 score_run.py "$OUT" --csv "$rep,$sync" >> "$RES" 2>/dev/null

    pkill -9 -f "nr-.*softmodem" 2>/dev/null; pkill -9 -f iperf3 2>/dev/null; sleep 3
  done
done

echo ""; echo "######## SYNC A/B COMPLETE ########"
python3 report_sync_ab.py "$RES"
