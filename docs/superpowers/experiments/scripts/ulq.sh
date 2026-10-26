#!/bin/bash
# quick UL verdict per run: layout lock path, PUSCH ok/try, UL by width
G=$(cd "$(dirname "$0")" && pwd)
for d in "$@"; do f=$(ls $d/*/run.log); echo "== $(basename $d)"
 sed 's/\x1b\[[0-9;]*m//g' $f | grep -aoE "ULLAYOUT (SEED_LOCK|LOCK) [^ ]+ [^ ]+|ULLAYOUT_CRC_LOCK len=[0-9]+ bwp=[0-9@]+|ULLAYOUT FIT_STRICT" | sort | uniq -c | head -4
 sed 's/\x1b\[[0-9;]*m//g' $f | grep -a "ULLAYOUT STATS" | tail -1 | grep -oE "bwp=[A-Za-z]+ [0-9]+@[0-9]+"
 systemd-run --user --scope --quiet -p MemoryMax=1G -p MemorySwapMax=0 choom -n 1000 -- nice -n 19 python3 -I $G/an.py $d | grep -E "^   (1-8|9-64|>64) |pusch_passive" | cut -c1-70
done
