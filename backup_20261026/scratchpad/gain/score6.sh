#!/bin/bash
G=/tmp/claude-1000/-home-sens-NICOLA-adaptive-rx-UL-DL/6de2bc9d-5f38-466a-9776-f15a57560647/scratchpad/gain
for d in "$@"; do systemd-run --user --scope --quiet -p MemoryMax=1G -p MemorySwapMax=0 choom -n 1000 -- nice -n 19 python3 -I $G/mcsw.py $d | grep -E "lab_|mcs (20|24)"; systemd-run --user --scope --quiet -p MemoryMax=1G -p MemorySwapMax=0 choom -n 1000 -- nice -n 19 python3 -I $G/an.py $d | grep -E "^   (1-8|9-64|>64) |pusch_passive" | cut -c1-90; $G/disc_score.sh $d; done
