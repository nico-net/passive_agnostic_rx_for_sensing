#!/bin/bash
# A/B of the kinematic Doppler-consistency gate (GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md Phase B)
# against a RECORDED capture, scored on world-frame tracks.
#
# Offline by construction, and that is the point: the gate lives in `repos/isac`, downstream of every
# report, so both arms can be replayed from the SAME detections. This harness's fused precision has
# been measured swinging 8-58% across identical repetitions (CLAUDE.md 9), which would swamp a gate
# this selective if the arms were separate captures. Same design as _run_aoa_vs_3rx.sh.
#
# Two fusion configurations, because Phase B's value depends on which one you are in:
#   1pair   ONE receiver + its bearings (--birth-single-pair-bearing). This is the arrangement the
#           handover is aiming at: births with no cross-receiver time synchronisation at all. It is
#           also where ghost rejection is weakest, so it is where a kinematic test matters most.
#   npair   every receiver whose reports were passed in, merged. Needs >= 2 to birth at all.
#
# Usage: ./_run_kinematic_ab.sh <out_dir> <run_dir> [run_dir2 ...]
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
ISAC=/home/sens/NICOLA/repos/isac/target/release/isac-track
OUT=${1:?usage: _run_kinematic_ab.sh <out_dir> <run_dir> [run_dir2 ...]}; shift
RUNS=("$@")
mkdir -p "$OUT"
GT="${RUNS[0]}/logs/ue.log"
TOL=${TOL:-15}

# Gates carried from the AoA work (CLAUDE.md 9). NOTE the asymmetry, which is not an oversight:
# --birth-min-pos-redundancy 1 must NOT be applied to the single-pair arms. Redundancy is
# (pairs + bearings - 2), so one pair plus its bearing scores 0 and the flag would refuse EVERY birth,
# silently turning that arm into "no tracks" rather than into a comparison. (Measured, not reasoned:
# the 1pair arm emits 135 track updates without it and 0 with it.)
COMMON=(--quiet --birth-max-pos-chi2 9)
NPAIR_ONLY=(--birth-min-pos-redundancy 1)

score() { # name  tracks-file  stderr-file
  local n=$1 t=$2 e=$3
  local vet
  vet=$(grep -m1 "gate-stats TOTAL" "$e" 2>/dev/null || echo "gate-stats TOTAL: (gate off)")
  echo "--- $n"
  echo "    $vet"
  python3 score_world_tracks.py "$t" "$GT" "$TOL" 2>&1 | sed -n '2,8p' | sed 's/^/    /'
}

arm() { # name  extra-args...
  local n=$1; shift
  $ISAC replay "$OUT/merged.jsonl" "${COMMON[@]}" --gate-stats "$@" \
    --out "$OUT/tracks_$n.jsonl" 2> "$OUT/$n.err"
  score "$n" "$OUT/tracks_$n.jsonl" "$OUT/$n.err"
}

echo "############ kinematic-gate A/B  (tol ${TOL} m, ${#RUNS[@]} receiver(s)) ############"

# ---- single-receiver arms (bearing-only births) --------------------------------------------------
cp "${RUNS[0]}/oaiue_reports.jsonl" "$OUT/merged.jsonl"
echo
echo "=== 1 pair + bearings (--birth-single-pair-bearing) ==="
arm 1pair_off --birth-single-pair-bearing
arm 1pair_on  --birth-single-pair-bearing --kinematic-harmonic-reject

# ---- multi-receiver arms, when more than one capture was supplied --------------------------------
if [ ${#RUNS[@]} -ge 2 ]; then
  args=()
  for r in "${RUNS[@]}"; do args+=("$r/oaiue_reports.jsonl@$r/logs/ue.log"); done
  python3 merge_receivers_n.py "$OUT/merged.jsonl" 150 "${args[@]}" 2>&1 | grep -E "fused CPIs" || true
  echo
  echo "=== ${#RUNS[@]} pairs fused ==="
  arm npair_off "${NPAIR_ONLY[@]}"
  arm npair_on  "${NPAIR_ONLY[@]}" --kinematic-harmonic-reject
fi

echo
echo "Read the gate-stats line FIRST: a gate that vetoed 0 detections has measured nothing about"
echo "itself, and looks identical to one that works on a scene with no harmonics."
