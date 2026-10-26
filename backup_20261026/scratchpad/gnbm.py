import sys,glob,re,statistics as st,collections,os
C='/home/sens/NICOLA/captures/'
for r in sorted(open('runs.txt').read().split(), key=lambda r:r.split('_lab_')[1]):
    if not r.split('_lab_')[1].startswith('2026102'): continue
    sc=C+r+'/gnb/sched.csv'; so=glob.glob(C+r+'/gnb/*.stdout.log')
    rk=collections.Counter()
    if os.path.exists(sc):
        for i,l in enumerate(open(sc)):
            if i==0: continue
            x=l.split(',')
            if len(x)>16 and x[3]=='dl' and x[4]=='pdsch': rk[x[15] or '?']+=1
    tot=sum(v for k,v in rk.items() if k!='?')
    rks=' '.join('r%s:%.0f%%'%(k,100*v/tot) for k,v in sorted(rk.items()) if k!='?' and v/tot>0.02) if tot else '-'
    cqi=[];ri=[];push=[];dok=dnk=uok=unk=0;rsrp=[]
    for f in so:
        for l in open(f,errors='replace'):
            m=re.match(r'\s+2\s+(4[0-9a-f]{3})\s+\|\s+(\S+)\s+(\S+)\s+(\d+)\s+\S+\s+(\d+)\s+(\d+)\s+\S+\s+\d+\s+\|\s+(\S+)\s+(\S+)\s+(\d+)\s+(\d+)\s+\S+\s+(\d+)\s+(\d+)',l)
            if not m: continue
            if m.group(2)!='n/a': cqi.append(float(m.group(2))); 
            if m.group(3)!='n/a': ri.append(float(m.group(3)))
            dok+=int(m.group(5)); dnk+=int(m.group(6)); uok+=int(m.group(11)); unk+=int(m.group(12))
            try: push.append(float(m.group(7))); rsrp.append(float(m.group(8)))
            except: pass
    print(r.split('_lab_')[0][:9].ljust(9),r.split('_lab_')[1][4:],'gNB DL ranks',rks.ljust(28),'| phones: cqi %s ri %s pusch_snr %s rsrp %s | DL nack %.1f%% (%d) UL nack %.1f%% (%d)'%(('%.1f'%st.median(cqi)) if cqi else '-',('%.1f'%st.mean(ri)) if ri else '-',('%.1f'%st.median(push)) if push else '-',('%.1f'%st.median(rsrp)) if rsrp else '-',100*dnk/max(1,dok+dnk),dok+dnk,100*unk/max(1,uok+unk),uok+unk))
