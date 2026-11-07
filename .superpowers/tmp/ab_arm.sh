#!/bin/bash
# ab_arm.sh <name> <build dir> <speed> <sensing|traffic>: one replay arm of raw_pci415_12s_b (4 RX, 217 PRB), per-thread CPU sampled. Output dir via $OUT.
set -u
W=/home/nicola/NICOLA/wt/rr-single1d
name=$1; B=$2; SPEED=$3; MODE=$4
O=/home/nicola/NICOLA/wt/rr-orchestration
OUT=$W/.superpowers/tmp/ab; mkdir -p $OUT
case $MODE in sensing) CFGSRC=$W/tests/passive_rx/sensing/sensing_ota.dgx.cfg;; traffic) CFGSRC=$W/tests/passive_rx/traffic/traffic_ota.dgx.cfg;; esac
CFG=$OUT/$name.cfg
sed "s#records_path = \"[^\"]*\"#records_path = \"$OUT/$name.records.jsonl\"#" $CFGSRC > $CFG
rm -f $OUT/$name.records.jsonl
TMO=$(python3 -c "print(int(12/$SPEED)+70)")
B=$B CAPSET=b ENVFILE=$O/ota/MPCI_20261025.env CFG=$CFG TMO=$TMO $W/tests/passive_rx/scan2_arm.sh $name ISAC_RAW_IQ_SPEED=$SPEED ${EXTRA:-} > $OUT/$name.out 2>&1
R=$(sed -n 's/^replay dir //p' /tmp/claude-1001/scan2_arm_$name.out | head -1)
echo "$name $B $SPEED $MODE $R" >> $OUT/arms.txt
echo "$R"
