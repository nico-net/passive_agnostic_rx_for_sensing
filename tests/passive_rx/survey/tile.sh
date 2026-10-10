#!/bin/bash
# tile.sh SRC DST N : concatenate N copies of a 0.3 s survey capture (0.3 s = 15 SSB periods = 30 frames, so SSB timing/SFN stay continuous)
SRC=$1; DST=$2; N=${3:-10}; mkdir -p $DST
for c in 0 1 2 3; do : > $DST/rx$c.sc16; for i in $(seq 1 $N); do cat $SRC/rx$c.sc16 >> $DST/rx$c.sc16; done; done
python3 - <<PY
import json
j=json.load(open("$SRC/capture_info.json")); j["samples_per_channel"]*=$N; json.dump(j,open("$DST/capture_info.json","w"))
PY
