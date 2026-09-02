#!/bin/bash
# A/B on ISAC_TSYNC_RESET. Arms ALTERNATE rather than running in blocks: this rig's health drifts
# over tens of minutes, and blocked arms would attribute that drift to the knob.
# Trace decimation is identical in both arms so it cannot confound.
set -u
cd /home/sens/NICOLA/captures || exit 1
REPS=${REPS:-5}
DUR=${DUR:-300}
rm -f /tmp/ab_tsync.stop
for r in $(seq 1 "$REPS"); do
  for arm in off on; do
    [ -e /tmp/ab_tsync.stop ] && { echo "STOPPED"; exit 0; }
    if [ "$arm" = on ]; then export ISAC_TSYNC_RESET_ARM=1; else unset ISAC_TSYNC_RESET_ARM; fi
    echo "=== rep $r arm=$arm $(date +%H:%M:%S) ==="
    env ARM="ab${arm}" CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
        DUR="$DUR" TRIES=1 RXG=20 NANT=4 MRC=3 SENSECOMB=1 SYNCONLY=1 CONTFO=1 \
        SLOTPOOL=512 GAINTRIM="4.5,11.6,0,6.1" ULPROBE=1 \
        TSYNCRESET=$([ "$arm" = on ] && echo 1 || echo 0) \
        bash ./run_arm.sh 2>&1 | grep -aE 'verdict=' 
    sleep 3
  done
done
echo "=== AB DONE ==="
