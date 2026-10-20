#!/bin/bash
# UL rfsim bed suite (2026-10-17): the arms of the UL root-cause study, sequential, one heavy.sh job.
# ONE heavy.sh call PER ARM (lock etiquette): heavy.sh ul_rfsim_suite.sh <outroot> 120 <arm>.
# usage: ul_rfsim_suite.sh <outroot> <rx_secs> [arm ...]   (default: all arms). Each arm dir has rx/ gnb/ ue/ and a score line in <outroot>/scores.jsonl.
# TBVARY=1 (default here): the gNB varies PRB range + MCS per grant; TBVARY= (empty) = the fixed 106 PRB / MCS 9 grant.
# Arm vocabulary: layout = FORCED explicit UL layout from the gNB log (ISAC_UL_FORCE_OPTS=1 + ul_rfsim_rx.forced.cfg, ground truth, NOT blind);
#   ids = FORCED scrambling IDs (ISAC_UL_FORCE_IDS, ground truth); bwp = only the UL BWP configured (phy-test has no SIB1), rest blind;
#   nocfg = fully blind (UL BWP unknown).
set -u
H=$(cd "$(dirname "$0")" && pwd); OUT=$1; SECS=$2; shift 2
FORCED_ENV="ISAC_UL_PUSCH_CHAIN=1 ISAC_UL_PUSCH_SOLVER=1 ISAC_UL_RETRY=1 ISAC_UL_UCI_SAFE=1 ISAC_UL_DCI_LOG=1 ISAC_UL_HARQ_COMBINE=1 ISAC_UL_SOLVER_FALLTHROUGH=2 ISAC_UL_PRIO_QUEUE=1 ISAC_UL_SNAP_POOL=1 ISAC_UL_QUEUE_DEPTH=32 ISAC_UL_SPARE_RESCUE=1"  # chain without the blind layout lock (the lock drops untagged forced grants)
ARMS=${*:-"A0_layout_nochain_pci A1_layout_pci A2_layout_ids_forced A2c_layout_ids_unforced A3_bwp_pci A4_bwp_ids A5_nocfg_pci A6_layout_tp"}
mkdir -p "$OUT"; F=$H/ul_rfsim_rx.forced.cfg; BW=$H/ul_rfsim_rx.bwp.cfg
for a in $ARMS; do
  case $a in
    A0_layout_nochain_pci) E=(RXCONF=$F RXENV=ISAC_UL_FORCE_OPTS=1 ULENV=);;
    A1_layout_pci)         E=(RXCONF=$F RXENV=ISAC_UL_FORCE_OPTS=1 "ULENV=$FORCED_ENV");;
    A2_layout_ids_forced)  E=(RXCONF=$F RXENV=ISAC_UL_FORCE_OPTS=1 "ULENV=$FORCED_ENV" TBIDS=249:324 FORCE_IDS=249:324);;
    A2c_layout_ids_unforced) E=(RXCONF=$F RXENV=ISAC_UL_FORCE_OPTS=1 "ULENV=$FORCED_ENV" TBIDS=249:324);;
    A3_bwp_pci)            E=(RXCONF=$BW);;
    A3b_bwp_pci_dl255)     E=(RXCONF=$BW DLSLOTS=255);;
    A3c_bwp_pci_dl255_tbl1) E=(RXCONF=$BW DLSLOTS=255 RXENV=ISAC_UL_MCS_TABLE=1);;
    T0_nocfg_table0)       E=(UECAP=$H/uecap_ports1_noqam256.cfg ULTAB=0);;  # UE cap without PUSCH 256QAM: truth table 0, fully blind
    A4_bwp_ids)            E=(RXCONF=$BW TBIDS=249:324);;
    A5_nocfg_pci)          E=();;
    A6_layout_tp)          E=(RXCONF=$H/ul_rfsim_rx.forced.tp.cfg RXENV=ISAC_UL_FORCE_OPTS=1 "ULENV=$FORCED_ENV" TBTP=1 ULBW=100 TBVARY=);;
    *) echo "unknown arm $a" >&2; continue;;
  esac
  env TBVARY=${TBVARY-1} "${E[@]}" flock -x /tmp/td_measure.lock "$H/ul_rfsim_arm.sh" "$OUT/$a" "$SECS" > "$OUT/$a.arm.log" 2>&1
  echo "{\"arm\":\"$a\",\"score\":$(python3 "$H/ul_rfsim_score.py" "$OUT/$a")}" | tee -a "$OUT/scores.jsonl"
done
