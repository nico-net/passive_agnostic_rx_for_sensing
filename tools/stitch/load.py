import numpy as np, collections
from cfr import frames, records
def load_pdsch(R, min_prb=1):
    """list of dict(slot,rnti,nl,l,k[],h[4,n]) for pdsch_dmrs records of run dir R, sorted by (slot, rnti)."""
    rec = records(R + '/sense_records_last.jsonl', ('pdsch_dmrs',)); out = []
    for f in frames(R + '/sense_cfr_last.bin'):
        r = rec.get(f['fid'])
        if r is None or (r.get('valid_cfr_prbs') or 0) < min_prb: continue
        out.append(dict(slot=r['abs_slot'], rnti=r['rnti'], nl=r['ports'], l=int(f['l'][0]), k=f['k'].astype(np.int32), h=np.array(f['h']), nv=f['nv'],
                        start=r['start_rb'], nb=r['nb_rb'], valid=r['valid_cfr_prbs'], est=r.get('cfr_est'), snr=r.get('snr_db')))
    return out
def pairs(k, h, step=2):
    """adjacent comb pairs (k, k+step) on one symbol: returns idx arrays i, j."""
    o = np.argsort(k); ks = k[o]; d = np.diff(ks); m = np.where(d == step)[0]
    return o[m], o[m + 1]
def jump(h, i, j):
    """per pair: complex spatial correlation c_ij = sum_a conj(h_a(i)) h_a(j), coherence |c|/(|hi||hj|), amplitude ratio."""
    c = np.sum(np.conj(h[:, i]) * h[:, j], axis=0); ni = np.linalg.norm(h[:, i], axis=0); nj = np.linalg.norm(h[:, j], axis=0)
    return c, np.abs(c) / np.maximum(ni * nj, 1e-12), nj / np.maximum(ni, 1e-12)
