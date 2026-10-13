#!/bin/bash
# survey_capture.sh OUTROOT CENTER_MHZ...  : 0.3 s (SAMPLES=122880000 for 1 s) x 4 ch per center, ONE heavy.lock hold. SIGINT via timeout only.
CAPTURE=${CAPTURE:-/tmp/claude-1001/-home-nicola-NICOLA-passive-agnostic-rx-for-sensing/8b5f4a61-d0d8-4c81-a9a6-30286599ce28/scratchpad/capture_raw}
O=/home/nicola/NICOLA/wt/rr-orchestration
OUT=$1; shift; mkdir -p $OUT
exec 9>$O/locks/heavy.lock; flock -x -w 1800 9 || exit 5
$O/llm/guardian.sh enter-ota || exit 7
trap "sudo rm -f /dev/hugepages/rtemap_* 2>/dev/null; $O/llm/guardian.sh exit-ota" EXIT
for C in "$@"; do
  for i in $(seq 1 30); do uhd_find_devices --args mgmt_addr=192.168.30.2 2>/dev/null | grep -q 'claimed: False' && break; sleep 5; done
  sudo rm -f /dev/hugepages/rtemap_* 2>/dev/null
  mkdir -p $OUT/c$C
  sudo env LD_LIBRARY_PATH=/usr/local/lib timeout --signal=INT 120 $CAPTURE $OUT/c$C "type=x4xx,addr=192.168.10.2,mgmt_addr=192.168.30.2,use_dpdk=1,dpdk_num_mbufs=65536" ${SAMPLES:-36864000} 122880000 ${C}000000 38 RX1 > $OUT/c$C.log 2>&1
  echo "center $C rc=$?"
  sudo chown -R nicola:nicola $OUT 2>/dev/null; sudo rm -f /dev/hugepages/rtemap_* 2>/dev/null
done
