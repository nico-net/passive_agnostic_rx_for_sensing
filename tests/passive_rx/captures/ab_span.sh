#!/bin/bash
# A/B on the CPI slot-span accumulator fix (sensing_engine.cc).
#
# DEFECT: an out-of-order CFR submission had its span step REJECTED (clamped) while
# cpi_prev_slot was advanced to that earlier slot anyway, so the next in-order submission
# measured its delta from the rejected slot and added the backwards step straight back on.
# Over 128 rows with DL/PUSCH/UL submitting from concurrent threads it accumulated without
# bound. MEASURED 2026-09-02: T_slot 285478-437334 slots against a true 1.0-3.7 on every run
# with mixed sources; the one single-producer run in the same batch read a correct 1.244.
#
# PRIMARY METRIC: T_slot (mean slot spacing per slow-time row), and the vel_max/vel_res it
# feeds. DETECTION IS ON (no SYNCONLY) -- under SYNC_ONLY the Doppler axis is never computed,
# which is exactly why this went unseen for a whole day of captures.
#
# BOTH ARMS RUN WITH CPU SEPARATION (NIC IRQs 8-13, softmodem 0-7). That knob just won its own
# A/B 5/5 on zero NIC drops, and holding it fixed removes the largest known variance source
# rather than letting it contaminate this comparison.
#
# arms: "pre" = the binary saved before the fix, "post" = current build. Alternated rep-by-rep.
set -u
cd /home/sens/NICOLA/captures || exit 1
PRE=/home/sens/NICOLA/captures/nr-uesoftmodem.prespan
COMP_IRQS="152 155 156 157 158 159 160 161 162 163 164 165 166 167"
REPS=${REPS:-4}
DUR=${DUR:-300}
rm -f /tmp/ab_span.stop
[ -x "$PRE" ] || { echo "missing control binary $PRE"; exit 1; }

n=0; for i in $COMP_IRQS; do
  echo "$(( 8 + n % 6 ))" | sudo tee /proc/irq/$i/smp_affinity_list >/dev/null; n=$((n+1)); done
echo "NIC IRQs -> 8-13 (held for both arms); irq152=$(cat /proc/irq/152/smp_affinity_list)"

for r in $(seq 1 "$REPS"); do
  for arm in pre post; do
    [ -e /tmp/ab_span.stop ] && { echo "STOPPED"; exit 0; }
    echo "=== rep $r arm=$arm $(date +%H:%M:%S) ==="
    env ARM="span${arm}" CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
        DUR="$DUR" TRIES=2 RXG=20 NANT=4 MRC=3 SENSECOMB=1 CONTFO=1 \
        SLOTPOOL=512 GAINTRIM="4.5,11.6,0,6.1" ULPROBE=1 CPUSET=0-7 \
        ${arm:+$([ "$arm" = pre ] && echo BIN=$PRE)} \
        bash ./run_arm.sh 2>&1 | grep -aE 'verdict=|PREFLIGHT|BIN override'
    D=$(ls -dt /home/sens/NICOLA/captures/span${arm}_* 2>/dev/null | head -1)
    [ -n "$D" ] && echo "   T_slot: $(grep -aoE 'T_slot=[0-9.]+' $D/run.log | tail -3 | tr '\n' ' ')"
    [ -n "$D" ] && echo "   vel:    $(grep -aoE 'vel_(max|res)[^ ,]*=[0-9.+-]+' $D/run.log | tail -2 | tr '\n' ' ')"
    sleep 3
  done
done
echo "=== AB SPAN DONE ==="
