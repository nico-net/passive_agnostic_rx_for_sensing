#!/usr/bin/env python3
"""WIDE-S0 offline tools on PDL1 LLR dumps (ISAC_PDSCH_LLR_DUMP + DECODEDIAG + DCIRAW of the same replay). Run under heavy.sh.
  wides0_offline.py scan  LOG DUMP OUT MINSNR FSLIST|all : delete w RBs (4..24) of symbol 0 at every 2nd RB of each failing S=0/L=14 decode of a
                                                          TDRA-8 grant, TBS kept; decode OUT with decodefix_ldpc.
  wides0_offline.py oracle LOG DUMP OUT 0 fs:w:a,...     : one passing window per grant + the raw stream (OUT.rawN.npy); run wides0_oracle on OUT,
                                                          then align.py-style comparison (see WIDES0.md) gives the exact unavailable RBs."""
import sys
mode = sys.argv.pop(1)
#!/usr/bin/env python3
"""WIDE-S0 recorded-input hypotheses on S=0/L=14 full decodes. LOG DUMP OUTPUT [minrb] [all]
Symbol 0 is the first nrb*12*Nl*Qm LLRs of an S=0 grant. Rate matching = delete REs (later bits shift, scrambling restarts on
the shorter stream); puncture = zero LLRs in place."""
import sys, json, struct, re, collections
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import decodefix_inputs as df
_gc = {}
_g0 = df.gold
def gold(n, seed):
    g = _gc.get(seed)
    if g is None or len(g) < n:
        g = _g0(max(n, 300000), seed); _gc[seed] = g
    return g[:n]
df.gold = gold
log, dump, out = sys.argv[1:4]
minrb = int(sys.argv[4]) if len(sys.argv) > 4 else 96
dci = collections.defaultdict(list)
P = re.compile(r"DCIRAW (\S+) rnti=0x([0-9a-f]+) len=(\d+) cce=(\d+) al=(\d+) mm=(\d+) payload=0x([0-9a-f]+)")
for l in open(log, errors="replace"):
    if "DCIRAW" in l:
        m = P.search(l)
        if m:
            fs, r, ln, cce, al, mm, p = m.groups()
            dci[fs].append((int(r, 16), int(ln), int(cce), int(al), int(p, 16)))
if mode == 'scan':
    
    minsnr = float(sys.argv[4]); only = set(sys.argv[5].split(','))
    tdra = {}
    for k, v in dci.items():
        for (r, ln, c, a, p) in v: tdra[(r, k)] = ((p >> (ln - 20)) & 15, c, a)
    meta = []; seen = set()
    with open(out, 'wb') as f:
        for d, raw, desc in df.records(log, dump):
            if d['crc'] != '0' or d['S'] != '0' or d['L'] != '14' or float(d['snr']) < minsnr or d['ssb_re'] != '0' or d['ptrs_re'] != '0': continue
            t = tdra.get((int(d['rnti'], 16), d['fs']))
            if not t or t[0] != 8 or (only != {'all'} and d['fs'] not in only): continue
            rb0, nrb = map(int, d['rb'].split('+'))
            key = (d['rnti'], d['abs'])
            if key in seen: continue
            seen.add(key)
            seed = (int(d['rnti'], 16) << 15) + int(d['dataid'])
            if desc: raw = raw * df.gold(len(raw), seed)
            u = 12 * int(d['qm']) * int(d['nl'])
            A, BG = int(d['tbs']), int(d['BG'])
            s0 = raw[:nrb * u].reshape(nrb, u); rest = raw[nrb * u:]
            for w in (4, 6, 8, 10, 12, 16, 18, 24):
                for a in range(0, nrb - w + 1, 2):
                    keep = np.ones(nrb, bool); keep[a:a + w] = False
                    x = np.concatenate([s0[keep].ravel(), rest])
                    llr, sh = df.normalized(x * df.gold(len(x), seed), A, BG)
                    f.write(struct.pack('<12I', len(meta), A, len(llr), int(d['qm']), int(d['nl']), BG, int(d['rv']), int(d['lbrm']), 8, 1, 1, 0))
                    f.write(llr.astype('<i2').tobytes())
                    meta.append(dict(d, trial='w', a=a + rb0, w=w, cce=t[1], al=t[2]))
    json.dump(meta, open(out + '.json', 'w')); print(len(meta), len(seen))
else:
    
    spec = {x.split(':')[0]: (int(x.split(':')[1]), int(x.split(':')[2])) for x in sys.argv[5].split(',')}
    meta = []; seen = set(); raws = {}
    with open(out, 'wb') as f:
        for d, raw, desc in df.records(log, dump):
            if d['crc'] != '0' or d['S'] != '0' or d['L'] != '14' or d['fs'] not in spec or d['ssb_re'] != '0' or d['csi_re'] != '0': continue
            if (d['fs'], d['rnti']) in seen: continue
            r = int(d['rnti'], 16); rb0, nrb = map(int, d['rb'].split('+'))
            if not any(rr == r for (rr, ln, c, a, p) in dci[d['fs']]): continue
            seen.add((d['fs'], d['rnti']))
            seed = (r << 15) + int(d['dataid'])
            if desc: raw = raw * df.gold(len(raw), seed)
            w, a = spec[d['fs']]
            u = 12 * int(d['qm']) * int(d['nl'])
            s0 = raw[:nrb * u].reshape(nrb, u); rest = raw[nrb * u:]
            keep = np.ones(nrb, bool); keep[a - rb0:a - rb0 + w] = False
            x = np.concatenate([s0[keep].ravel(), rest])
            A, BG = int(d['tbs']), int(d['BG'])
            llr, sh = df.normalized(x * df.gold(len(x), seed), A, BG)
            f.write(struct.pack('<12I', len(meta), A, len(llr), int(d['qm']), int(d['nl']), BG, int(d['rv']), int(d['lbrm']), 8, 1, 1, 0))
            f.write(llr.astype('<i2').tobytes())
            np.save(out + '.raw%d.npy' % len(meta), raw)
            meta.append(dict(d, w=w, a=a, seed=seed))
    json.dump(meta, open(out + '.json', 'w')); print(len(meta))
