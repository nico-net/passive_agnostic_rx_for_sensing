#!/bin/bash
# Does the range-axis origin come from the FFT-window pull inside the CP?
#
# slot_fep_nr.c:104   rx_offset -= (nb_prefix_samples / ofdm_offset_divisor)
# starts the FFT window EARLY, inside the CP, to avoid ISI. Starting early makes every
# propagation path read LATE in the CIR by the same amount. Live: nb_prefix=288, default
# divisor 8 -> a 36-sample pull. Measured residual origin offset: 0.985 samples.
#
# PREDICTION (nb_prefix=288, 2.4397 m/sample, 3.0504 m/bin at 273 PRB):
#   div 16 -> 18 samples pull  ->  LOS bin  -14.4  vs div 8
#   div  8 -> 36 samples pull  ->  reference
#   div  4 -> 72 samples pull  ->  LOS bin  +28.8  vs div 8
#
# WHY THIS TEST WORKS WHERE THE los_delay_samples SWEEP FAILED (slope +0.06 against +1.00
# predicted): that one perturbed the CHANNEL, and the UE re-syncs to the direct path, so the
# injection was absorbed before the sensing pipeline saw it. nr_adjust_synch_ue correlates
# PSS/SSS in the TIME DOMAIN, upstream of the FEP, so it never observes the divisor at all
# and cannot compensate for it.
#
# A confirmed slope of 1.0 samples-per-sample means the origin is window placement and is
# CALIBRATABLE. A flat result falsifies it and sends the search elsewhere.
set -u
cd /home/sens/NICOLA/captures || exit 1
DUR=${DUR:-420}
rm -f /tmp/ab_offdiv.stop
for d in 8 4 16 8; do        # 8 twice, first and last, so rig drift is visible as a repeat
  [ -e /tmp/ab_offdiv.stop ] && { echo STOPPED; exit 0; }
  echo "=== divisor=$d  pull=$((288/d)) samples  $(date +%H:%M:%S) ==="
  env ARM="div${d}" CONF=/home/sens/NICOLA/nrue.passive_rx.ul.q.conf \
      DUR="$DUR" TRIES=2 RXG=20 NANT=4 MRC=3 SENSECOMB=1 CONTFO=1 \
      SLOTPOOL=512 GAINTRIM="4.5,11.6,0,6.1" ULPROBE=1 OFFDIV=$d \
      bash ./run_arm.sh 2>&1 | grep -aE 'verdict=|PREFLIGHT'
  D=$(ls -dt /home/sens/NICOLA/captures/div${d}_*/ 2>/dev/null | head -1)
  for t in "DL/gNB" "UL/UE"; do
    echo "   $t $(grep -a "LOS\[$t\]" $D/run.log 2>/dev/null | grep -oE 'bin=[0-9.-]+ -> [0-9.-]+ m' | tail -1)"
  done
  sleep 3
done
echo "=== OFFDIV SWEEP DONE ==="
