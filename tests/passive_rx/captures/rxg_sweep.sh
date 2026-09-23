#!/bin/bash
# RX-gain sweep on the live rank-4 cell: is the 0 % OTA decode an ADC-headroom problem?
# One armval run per gain (settle + lock + DUR s), same arm otherwise; per gain it reports
# ADCPEAK / RFPOW per branch, CHESTDIAG per-layer power/residual, CRC by layer count, PREFERRED.
#   GAINS="49 43 37 31 25" DUR=180 CONF=<pinned or agnostic conf> bash rxg_sweep.sh
# Runs are sequential; refuses to start while a receiver is up. Output: /tmp/rxg_sweep.txt
set -u
GAINS=${GAINS:-"49 43 37 31 25"}
DUR=${DUR:-180}
ARM=${ARM:-r4g}
OUT=${OUT:-/tmp/rxg_sweep.txt}
CAPDIR=/home/sens/NICOLA/captures
cd /home/sens/NICOLA/adaptive-rx-UL-DL/tests/passive_rx/captures || exit 1
pgrep -x nr-uesoftmodem >/dev/null && { echo "receiver running, abort"; exit 1; }
echo "# rxg sweep $(date +%F_%T) gains=[$GAINS] dur=$DUR conf=${CONF:-default}" >> "$OUT"
for g in $GAINS; do
  # env, not a shell assignment-prefix: ${CONF:+CONF=$CONF} as a bare prefix word is NOT recognized
  # as an assignment by bash (that recognition is syntactic, on the unexpanded token) -- it tries to
  # execve() the expanded text as the command itself and fails with "No such file or directory".
  env ARM=$ARM NIC=enp2s0f0np0 NANT=4 RXG=$g TINTERP=1 AGNV2=1 CHESTDIAG=1 MRC=3 INITIALFO=-16300 DUR=$DUR \
    ${CONF:+CONF="$CONF"} ${XENV:+XENV="$XENV"} bash ./armval.sh > /tmp/${ARM}_val.out 2>&1
  while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
  d=$(ls -td $CAPDIR/${ARM}_* | head -1)
  T() { tr -d "\033" < "$d/run.log"; }
  {
    echo "== RXG=$g ${d##*/} $(tail -1 /tmp/${ARM}_status)"
    T | grep -a "SENSING: ADCPEAK" | tail -1 | sed 's/.*ADCPEAK/ADCPEAK/' | cut -c1-200
    T | grep -a "SENSING: RFPOW" | tail -1 | grep -oE "ch[0-9]=[^ ]+" | tr '\n' ' '; echo
    T | grep -a "SENSING: ANTPOW" | tail -1 | grep -oE "dB=\[[^]]*\]"
    for nl in 1 2 3 4; do
      T | grep -a "CHESTDIAG nl=$nl" | grep -oE "orth=[0-9.]+" | awk -F= -v nl=$nl '{s+=$2;n++} END{if(n) printf "chest nl=%d n=%d mean_orth=%.3f\n", nl, n, s/n}'
    done
    T | grep -a "CHESTDIAG nl=4" | tail -1 | grep -oE "rel_dB=L0\[[^]]*\].*rel_dB=L3\[[^]]*\]" | cut -c1-200
    T | grep -a "PDSCHQ queued" | tail -1 | grep -oE "queued=[0-9]+ decoded=[0-9]+ crc_ok=[0-9]+ \([0-9.]+%\)"
    T | grep -aE "SENSING: (NLHIST|PDSCHQ per-nl|MCSHIST WIDE)" | tail -2 | cut -c1-240
    T | grep -a "LAYOUT_PROBE" | tail -1 | grep -oE "n=[0-9]+ cb0_ok=[0-9]+"
    T | grep -a "LLRDIAG FAILED" | tail -1 | grep -oE "mean_abs=[0-9.]+.*zero=[0-9.]+%"
    echo "preferred=$(T | grep -ac PREFERRED) rfstall=$(T | grep -ac RFSTALL)"
  } | tee -a "$OUT"
done
