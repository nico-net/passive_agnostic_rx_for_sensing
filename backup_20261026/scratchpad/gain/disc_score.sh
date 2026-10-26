#!/bin/bash
# per run: bank adds, evidence-ignored, scan drops, SCANTIME p50, PUSCH try, DCI recall (old scorer)
for d in "$@"; do f=$(ls $d/*/run.log 2>/dev/null|head -1); [ -n "$f" ] || continue
 m=$(sed 's/\x1b\[[0-9;]*m//g' $f | grep ISAC_METRICS | tail -1); q=$(echo "$m" | grep -oE '"scanq_queued":[0-9]+' | cut -d: -f2); df=$(echo "$m" | grep -oE '"scanq_drop_full":[0-9]+' | cut -d: -f2)
 o=$(grep -l "$(basename $d)" /tmp/claude-1000/-home-sens-NICOLA-adaptive-rx-UL-DL/6de2bc9d-5f38-466a-9776-f15a57560647/scratchpad/gain/*.out 2>/dev/null | head -1)
 echo "$(basename $d) bank=$(grep -c 'multi-CORESET bank add' $f) ignored=$(grep -c 'evidence ignored' $f) nidgpu=$(grep -c 'PDCCH_NID_GPU pass' $f) drop=$(( ${df:-0}*100/${q:-1} ))% $(sed 's/\x1b\[[0-9;]*m//g' $f | grep 'SCANTIME full' | tail -1 | grep -oE 'p50<=[0-9]+us') $(sed 's/\x1b\[[0-9;]*m//g' $f | grep 'pusch_passive\[' | tail -1 | grep -oE 'try=[0-9]+ crc_ok=[0-9]+') $(grep -h groundtruth $o 2>/dev/null | awk '{split($2,a,"=");split($3,b,"="); printf "%s=%.0f%% ", $1, 100*b[2]/a[2]}')"
done
