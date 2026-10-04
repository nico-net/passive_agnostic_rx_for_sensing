import re, sys
for d in sys.argv[1:]:
    prev=None; drops=[]; n=0
    for line in open(d+'/rx/rx.log', errors='replace'):
        line=re.sub(r'\x1b\[[0-9;]*m','',line)
        if 'Technique D CONVERGED rnti=0x1234 tda=0' in line: break
        m=re.search(r'Technique D evidence rnti=0x1234 config=\S+ tda=0 outcomes=(\d+) min_trials=(\d+) best=(\d+)/(\d+)',line)
        if m:
            n+=1; o,mt,b,t=map(int,m.groups())
            if prev and t < prev[3]: drops.append((prev[0],o,prev[3],t))
            prev=(o,mt,b,t)
    print(d.split('/')[-2]+'/'+d.split('/')[-1], 'evidence_lines=%d'%n, 'leader-trial drops (wipe or leader change):', drops[:6], 'final', prev)
