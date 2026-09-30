#!/bin/bash
# score.sh <armdir>... : one line per run
for d in "$@"; do X=$d/rx; [ -f $X/rx.log ] || X=$d/gnb; L=$X/rx.log; C=$(mktemp); sed 's/\x1b\[[0-9;]*m//g' $L > $C
 t_sync=$(grep -m1 "Initial sync successful" $C | awk '{print $1}')
 t_acc=$(grep -m1 "rnti=0x1234" $C | awk '{print $1}')
 t_conv=$(grep -m1 "Technique D CONVERGED" $C | awk '{print $1}')
 nconv=$(grep -c "Technique D CONVERGED" $C)
 bank=$(grep -m1 -o "bank add .*len=[0-9]*" $C | grep -o "len=[0-9]*")
 ldpc=$(grep "LDPCDIAG" $C | tail -1 | grep -o "ok=[0-9]* seg_fail=[0-9]*")
 q=$(grep "PDSCHQ queued" $C | tail -1 | grep -o "queued=[0-9]* decoded=[0-9]* crc_ok=[0-9]* ([0-9.]*%) dropped\[full=[0-9]* stale=[0-9]*\]")
 scan=$(grep "monitor summary" $C | tail -1 | grep -o "scanq\[[^]]*\]")
 conv=$(grep "Technique D CONVERGED" $C | grep -o "S=[0-9]* L=[0-9]*" | sort -u | tr '\n' ',')
 cpu=$(grep "Percent of CPU" $X/time.txt | awk '{print $NF}'); rss=$(grep "Maximum resident" $X/time.txt | awk '{print $NF}')
 ttc=$(awk -v a="$t_acc" -v c="$t_conv" 'BEGIN{if(a!=""&&c!="")printf "%.2f", c-a; else print "NA"}')
 echo "$(basename $d) sync@${t_sync}s first_acc@${t_acc}s conv@${t_conv}s ttc=${ttc}s nconv=$nconv [$conv] bank:$bank LDPC:$ldpc PDSCHQ:$q $scan cpu=$cpu rssKB=$rss"
 rm -f $C; done
