#!/bin/bash
# Ablate the features whose thresholds are still hand-set, to decide which are worth ADAPTING and
# which should simply be REMOVED. A feature that changes nothing when switched off is not a knob to
# tune -- it is dead weight, and the honest fix is deletion, not a cleverer default.
# Baseline is the all-auto config, i.e. det_quality + auto NMS/harmonic_tol/max_range already on.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")"
DUR=${1:-400}
OUT=/tmp/ghostkin/abl; mkdir -p "$OUT"
run() { # name  sed-expression
  local n=$1 e=$2 c="_abl_$1.conf"
  sed -e "$e" _scene_four_rx1_auto.conf > "$c"
  sudo env UE_NB_ANT_RX=4 ./_run_mot_variant.sh "$c" "$OUT/$n" "$DUR" > "$OUT/$n.log" 2>&1
  rm -f "$c"
  printf "%-22s " "$n"
  grep -m1 "RAW DETECTIONS" "$OUT/$n.log" | sed 's/RAW DETECTIONS: //'
  grep -m1 "CONFIRMED TRACKS" "$OUT/$n.log" | sed 's/^/                       /'
  grep -E "obj[0-9] detection coverage" "$OUT/$n.log" | tr '\n' ' ' | sed 's/^/                       cov: /'; echo
}
run baseline            's/^#UNUSED//'
run no_far_harmonic     's/^  far_harmonic_reject .*/  far_harmonic_reject     = 0;/'
run no_harmonic_reject  's/^  harmonic_reject .*/  harmonic_reject         = 0;/'
run no_subslot          's/^  subslot_symbols .*/  subslot_symbols         = 0;/'
echo "############ ablation done ############"
