#!/bin/bash
# Live stage 1-2: wait for the receiver's snapshot dumps (ISAC_COREMAP_IDSWEEP), run GPU stage 1 + stage 2
# on cores the receiver's FIFO threads do not use (DISC_CPUS, default the sensing cores, idle until the
# gate opens -- which needs this result), and publish /tmp/coresets_discovered.txt for the receiver.
N=${N:-10} DISC_CPUS=${DISC_CPUS:-11,12,13} OUT=${OUT:-.}
cd "$(dirname "$0")" || exit 1
NT=$(echo "$DISC_CPUS" | tr ',' '\n' | wc -l)
until [ "$(find /tmp/passive_rx -maxdepth 1 -name 'idsweep_0*.bin' | wc -l)" -ge "$N" ]; do sleep 2; done
F=$(find /tmp/passive_rx -maxdepth 1 -name 'idsweep_0*.bin' | sort | head -n "$N")
t0=$(date +%s.%N)
OMP_NUM_THREADS=$NT taskset -c "$DISC_CPUS" ./idsweep_offline_gpu $F > "$OUT/live_stage1.txt" 2>&1
t1=$(date +%s.%N)
OMP_NUM_THREADS=$NT taskset -c "$DISC_CPUS" ./idsweep_offline_gpu --stage2 "$OUT/live_stage1.txt" $F > "$OUT/live_stage2.txt" 2>&1
t2=$(date +%s.%N)
echo "DISCOVERY stage1 $(echo "$t1 - $t0" | bc) s, stage2 $(echo "$t2 - $t1" | bc) s"
grep STAGE1LIST "$OUT/live_stage1.txt"
grep -E "HANDOFF|SUMMARY" "$OUT/live_stage2.txt"
