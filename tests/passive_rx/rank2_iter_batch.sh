#!/bin/bash
# rank2_iter_batch.sh <rep-count> : the RANK2-ITER replay matrix (replay only): per capture, per repetition, caps 8 12 16 24, interleaved.
# Geometry per capture = blind-front auto spec (docs/superpowers/reports/csd/BLIND-FRONT-20261023.md section 4). Run dirs are listed in replays/rank2iter_index.tsv.
cd "$(dirname "$0")/../.."
reps=${1:-2}; IDX=replays/rank2iter_index.tsv
declare -A G
G[v415b]="/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b 3680010000 3680010000 1171 -4850"
G[v415o]="/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 3680010000 3680010000 1171 6900"
G[t923]="/home/nicola/NICOLA/ota_runs/raw_pci923_tim_12s_tile3 3731910000 3759990000 233 -4836"
for rep in $(seq 1 "$reps"); do
  for cap in ${CAPTURES:-v415b t923 v415o}; do
    for it in ${ARMS:-8 12 16 24}; do
      read -r CAP CAPC RXC SSB FO <<< "${G[$cap]}"; export CAP CAPC RXC SSB FO
      DUR=${DUR:-260} tests/passive_rx/rank2_iter_replay.sh "$cap" "$it" > /dev/null 2>&1
      printf '%s\t%s\t%s\t%s\n' "$cap" "$it" "$rep" "$(cat replays/last)" >> "$IDX"
    done
  done
done
echo BATCH_DONE >> "$IDX"
