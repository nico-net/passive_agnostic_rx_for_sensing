import re,sys
seen={};hits=[];adds=[]
rs=re.compile(r'blind PDCCH (?:rnti_seen|dl_seen|ul_seen) .*?rnti=(0x\w+) sfn=(\d+) slot=(\d+) fmt=(\S+).*?bank=(\d+)')
rb=re.compile(r'USS_BOOT_HIT count=\d+ frame=(\d+) slot=(\d+) rnti=(0x\w+) len=(\d+) .*?offset=(\d+) span=(\d+)')
ra=re.compile(r'multi-CORESET bank add index=(\d+) offset=(\d+) span=(\d+).*mapping=(\S+)')
with open(sys.argv[1],errors='replace') as f:
  for l in f:
    if 'blind PDCCH' in l:
      m=rs.search(l)
      if m: seen.setdefault((m[2],m[3],m[1]),set()).add(m[5])
    elif 'USS_BOOT_HIT' in l:
      m=rb.search(l)
      if m: hits.append((m[1],m[2],m[3],m[5],m[6]))
    elif 'bank add' in l:
      m=ra.search(l)
      if m: adds.append((m[1],m[2],m[3],m[4],len(hits)))
tot=0;dup=0
for h in hits:
  if h[3]=='0' and h[4]=='270': continue
  tot+=1
  if '0' in seen.get((h[0],h[1],h[2]),()): dup+=1
print(sys.argv[1].split('/')[-3] if 0 else '', 'adds',adds,'extra-geom boot hits',tot,'same slot+rnti decoded by a bank',dup)
