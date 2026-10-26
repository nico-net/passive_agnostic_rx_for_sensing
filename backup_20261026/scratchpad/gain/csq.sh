#!/bin/bash
# CSI-RS verdict: KEEP rows + time, ZP_HOLES l0 split, CRC slot 4/44 mod 80 vs others (Nl<=2, all widths)
for d in "$@"; do f=$(ls $d/*/run.log); n=$(wc -l < $f); echo "== $(basename $d)"
 sed 's/\x1b\[[0-9;]*m//g' $f | grep -an "verdict=KEEP" | sed -E 's/^([0-9]+):.*HOLES (row=[0-9]+ fd=[0-9]+ l0=[0-9]+).*(lattice_variant=[0-9]+)?.*/\1 \2/' | awk -v n=$n '{printf "  KEEP at %.2f %s %s %s\n",$1/n,$2,$3,$4}' | head -5
 echo "  ZP_HOLES l0: $(sed 's/\x1b\[[0-9;]*m//g' $f | grep -a 'ZP_HOLES EXACT\|ZP_HOLES COVER' | grep -oE 'l0=[0-9]+' | sort | uniq -c | tr '\n' ' ')"
 sed 's/\x1b\[[0-9;]*m//g' $f | python3 -c "
import sys,re
t={}
for l in sys.stdin:
  if 'EVMGRANT' not in l or ' probe=0' not in l or ' nsym=0 ' in l: continue
  m=re.search(r'EVMGRANT rnti=\S+ (\d+)\.(\d+) crc=(\d).*?Nl=([12]) ',l)
  if not m: continue
  s=(int(m.group(1))*20+int(m.group(2)))%80; k='s4' if s==4 else 's44' if s==44 else 'other'
  t.setdefault(k,[0,0]); t[k][0]+=int(m.group(3)); t[k][1]+=1
print('  CRC Nl<=2:', ' '.join('%s=%.2f/%d'%(k,v[0]/v[1],v[1]) for k,v in sorted(t.items())))"
done
