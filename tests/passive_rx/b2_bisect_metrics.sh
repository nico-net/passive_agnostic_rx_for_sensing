#!/bin/bash
# per run: arm rep solved=1 lines, last PDSCHQ decoded/crc_ok, solved rnti count
while read a r d; do
  f=$d/run.log; [ -f $f ] || continue
  c=$(sed 's/\x1b\[[0-9;]*m//g' $f)
  s=$(echo "$c" | grep -c "solved=1")
  q=$(echo "$c" | grep "PDSCHQ queued" | tail -1 | sed -E 's/.*decoded=([0-9]+) crc_ok=([0-9]+).*/\1 \2/')
  echo "$a $r solved1=$s decoded/crc_ok=$q"
done < $1
