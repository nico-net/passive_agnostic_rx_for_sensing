#!/bin/bash
# usage: post_nsys.sh <replay dir> <nsys base name>  -- export, build kx, hwm + stall windows + k_fir stats
d=$1; n=$2; cd "$d" || exit 1
[ -f $n.sqlite ] || nsys export --type sqlite -o $n.sqlite $n.nsys-rep > export.log 2>&1
python3 $(dirname $0)/nsys_prep.py $n 2>&1 | tail -6
S=$(dirname $0)
echo "=== RT kernel exec > 10 ms"; sqlite3 $n.sqlite "select kname, round((ks-(select min(las) from kx))/1e9,3), blocks, round((ke-ks)/1e6,1) from kx where tid in (select t.globalTid%16777216 from ThreadNames t join StringIds s on s.id=t.nameId where s.value like 'passivePdsch%') and ke-ks>10000000 order by ks limit 40"
echo "=== hwm"; python3 $S/hwm.py $n.sqlite | grep -v "new_hwm (blocks"
echo "=== stall windows"; python3 $S/stallwin.py $n.sqlite 20 | grep -v "^  osrt\|(no osrt)" | head -80
grep "PDTIMCLS us" run.log | tail -1 | cut -c1-400; grep -c PDTIMSLOW run.log; grep "GPU_EXACT_CHEST calls\|GPU_EXACT_DEMOD calls\|HOST_PIN" run.log | tail -3 | cut -c1-200
