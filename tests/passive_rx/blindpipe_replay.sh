#!/bin/bash
# blindpipe_replay.sh ARM CAPKEY [extra env...] : one BLINDPIPE replay arm (2026-10-13) on a named capture, then the
# per-run summary (blindpipe_stats.py). ARM: off = t26z + ISAC_CARRIER_FROM_SIB1=1 (CORESETALL baseline);
# fix = off + ISAC_PDCCH_DWELL_OWN_BUDGET=1 ISAC_DL_TRAFFIC_PROBE=16 ISAC_POINTA_SWEEP=1; fix3 = fix + ISAC_COREMAP_IDSWEEP_DUR=3.
# CAPKEY: b (raw_pci415_12s_b), v8 (raw_pci415_12s_20261008), tile3 (raw_pci923_tim_12s_tile3), tim4 (raw_pci923_tim_4s_1010_1853),
# c421 (raw_pci421_aligned_20261006). Replay only; heavy.lock via timacq_replay.sh; TMO sized to the capture.
set -u
D=$(cd "$(dirname "$0")/../.." && pwd); ARM=$1; KEY=$2; shift 2
export B=${B:-$D/cmake_targets/ran_build/build_t26} ENVFILE=${ENVFILE:-/home/nicola/NICOLA/wt/rr-orchestration/ota/t26z.env} SPEED=${SPEED:-0.25}
O=/home/nicola/NICOLA/ota_runs
case $KEY in
  b)     export CAP=$O/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900; TMO=${TMO:-200};;
  v8)    export CAP=$O/raw_pci415_12s_20261008 CAP_SAMPLES=1474560000 CAP_TICK=210036277 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=6900; TMO=${TMO:-200};;
  tile3) export CAP=$O/raw_pci923_tim_12s_tile3 CAP_SAMPLES=1474560000 CAP_TICK=195322029 CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4800; TMO=${TMO:-200};;
  tim4)  export CAP=$O/raw_pci923_tim_4s_1010_1853 CAP_SAMPLES=491520000 CAP_TICK=195322029 CAPC=3731910000 RXC=3759990000 SSB=233 FO=-4800; TMO=${TMO:-120};;
  c421)  export CAP=$O/raw_pci421_aligned_20261006 CAP_SAMPLES=491520000 CAP_TICK=209744976 CAPC=3680010000 RXC=3680010000 SSB=1171 FO=4100; TMO=${TMO:-120};; # MIB-barred, no SIB1: Point A = grid RB 0 (memory: x410-34b2af1 calibration)
  *) echo "unknown CAPKEY $KEY"; exit 2;;
esac
export TMO
base=(ISAC_CARRIER_FROM_SIB1=1)
fix=(ISAC_PDCCH_DWELL_OWN_BUDGET=1 ISAC_DL_TRAFFIC_PROBE=16 ISAC_POINTA_SWEEP=1)
case $ARM in
  off)  extra=("${base[@]}");;
  fix)  extra=("${base[@]}" "${fix[@]}");;
  fix3) extra=("${base[@]}" "${fix[@]}" ISAC_COREMAP_IDSWEEP_DUR=3);;
  *) echo "unknown ARM $ARM"; exit 2;;
esac
"$D/tests/passive_rx/timacq_replay.sh" "bp_${KEY}_${ARM}" "${extra[@]}" "$@"; RC=$?
R=$(cat "$D/replays/last"); echo "== rc=$RC arm=$ARM cap=$KEY -> $R"
python3 "$D/tests/passive_rx/blindpipe_stats.py" "$R"
