"""Offline decode harness: python LLRs -> tests/passive_rx/decodefix_ldpc (built by decodefix_build_diag.py into $B).
   env: B=<build dir with decodefix_ldpc + libldpc.so>, OUT=<dir for the record files>"""
import sys, os, subprocess; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from raw import *
B=os.environ.get('B','cmake_targets/ran_build/build_t26')
def run(sel, fn, tag, tbs_of):
    p=os.path.join(os.environ.get('OUT','/tmp'),'abl_%s.bin'%tag)
    f=open(p,'wb')
    for i,h in enumerate(sel):
        llr=fn(h)
        c=scramble_seq(h['rnti'],h['data_id'],llr.size); llr=np.where(c==1,-llr,llr)
        A,Qm,BG=tbs_of(h)
        write_rec(f,i,A,llr,Qm,h['nl'],BG,rv=[0,2,3,1][h['rv']] if False else h['rv'])
    f.close()
    o=subprocess.run([B+'/decodefix_ldpc',B+'/libldpc.so',p],capture_output=True,text=True,env={'LD_LIBRARY_PATH':B}).stdout
    ok=[int(l.split('crc=')[1].split()[0]) for l in o.splitlines() if l.startswith('OFFLINE')]
    print('%-28s %d/%d'%(tag,sum(ok),len(sel)), ''.join(map(str,ok)))
    return ok
