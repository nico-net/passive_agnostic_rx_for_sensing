#!/usr/bin/env python3
"""GPU-span root cause, unprofiled on-air run (merge19 + measurement commit): per real DL grant on the GPU pass, from EVMGRANT
gp=total,before_try,try,prep,launch,gstart,gspan  gq=C,D,L,seq  li=valid,ncb,nconv,nlast,itconv_max,itconv_sum,kspan,swait,dconv_max,dlast_max,g2l,d2l,npad,npad_last,dpad_max
usage: gqrep.py run.log [run.log ...]   (one run per argument, never pooled)"""
import re, sys, statistics as st, collections
KV = re.compile(r'(\w+)=(\S+)')
def q(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))] if v else float('nan')
def cbs(tbs, bg_hint=None):
    """TS 38.212 5.2.2 code-block count from TBS (BG1 if TBS > 3824 else BG2 for large TBs is a simplification: BG2 when TBS <= 3824 or R <= 0.25)"""
    B = tbs + 24
    Kcb = 8448 if tbs > 3824 else 3840
    return 1 if B <= Kcb else -(-B // (Kcb - 24))
def parse(path):
    R = []
    for l in open(path, errors='replace'):
        if 'EVMGRANT' not in l or ' gq=' not in l: continue
        k = dict(KV.findall(re.sub(r'\x1b\[[0-9;]*m', '', l)))
        gp = [int(x) for x in k['gp'].split(',')]; gq = [int(x) for x in k['gq'].split(',')]; li = [int(x) for x in k['li'].split(',')]
        e = [int(x) for x in k['e2e'].split(',')] if 'e2e' in k else [None] * 8
        r = dict(probe=k.get('probe', '0') != '0', crc=int(k['crc']), tbs=int(k['tbs']), mcs=int(k['mcs']), nl=int(k['Nl']), rb=int(k['rb'].split('+')[1]),
                 skip=int(k.get('skip', 0)), gstart=gp[5], gspan=gp[6], C=gq[0], D=gq[1], L=gq[2], seq=gq[3],
                 lv=li[0], ncb=li[1], nconv=li[2], nlast=li[3], itc=li[4], itsum=li[5], kspan=li[6], swait=li[7], dconv=li[8], dlast=li[9], g2l=li[10], d2l=li[11],
                 qwait=e[1], dec=e[3], tot=e[6], bodies=e[5])
        r['cbs_spec'] = cbs(r['tbs'])
        R.append(r)
    return R
def table(name, G, keys):
    print(f"  {name}: n={len(G)}")
    for k in keys:
        v = [g[k] for g in G if g[k] is not None]
        if v: print(f"    {k:8s} p10 {q(v,.1):8.0f} p50 {q(v,.5):8.0f} p90 {q(v,.9):8.0f} p99 {q(v,.99):8.0f} mean {st.mean(v):8.1f}")
for path in sys.argv[1:]:
    R = parse(path)
    real = [r for r in R if not r['probe'] and not r['skip'] and r['gspan'] > 0]
    print(f"== {path}: EVMGRANT lines with gq {len(R)}; real, decoded on the pass, C ran: {len(real)}; probes {sum(r['probe'] for r in R)}; gate skips {sum(r['skip'] for r in R)}")
    if not real: continue
    keys = ['gstart', 'gspan', 'C', 'D', 'L', 'kspan', 'swait', 'g2l', 'd2l', 'ncb', 'nconv', 'nlast', 'itc', 'dconv', 'dlast', 'qwait', 'dec', 'tot']
    table('ALL real', real, keys)
    sp = sorted(r['gspan'] for r in real)
    typ = [r for r in real if q(sp, .45) <= r['gspan'] <= q(sp, .55)]
    tail = [r for r in real if r['gspan'] >= q(sp, .99)]
    table('TYPICAL (gspan p45-p55)', typ, keys)
    table('TAIL (gspan >= p99)', tail, keys)
    lv = [r for r in real if r['lv']]
    print(f"  li valid {len(lv)} of {len(real)}; ncb (pass) vs TS 38.212 count from TBS mismatches: {sum(1 for r in lv if r['ncb'] != r['cbs_spec'])} (BG choice simplified)")
    # L decomposition: L = g2l (IF + rdm + memset) + kspan (LDPC) + rest (crc, pick, crc, result copy, graph end); D2L covers IF-D + D body
    for name, G in (('TYPICAL', typ), ('TAIL', tail)):
        G = [r for r in G if r['lv']]
        if not G: continue
        rest = [r['L'] - r['g2l'] - r['kspan'] for r in G]
        print(f"  {name} L split (us, p50): L {q([r['L'] for r in G],.5)} = gate_l->LDPC {q([r['g2l'] for r in G],.5)} + LDPC kspan {q([r['kspan'] for r in G],.5)} + after-LDPC {q(rest,.5)};"
              f" LDPC: swait {q([r['swait'] for r in G],.5)}, dconv_max {q([r['dconv'] for r in G],.5)}, dlast_max {q([r['dlast'] for r in G],.5)}")
    # LDPC behaviour: CRC fail vs ok, by number of limit-reached blocks
    print('  LDPC by CRC and limit-reached blocks (real, li valid): n, ncb p50, kspan p50/p99, dconv_max p50, dlast_max p50, swait p50 (us)')
    by = collections.defaultdict(list)
    for r in lv:
        b = 'crc_ok' if r['crc'] else ('fail nlast=0' if r['nlast'] == 0 else 'fail nlast=1' if r['nlast'] == 1 else 'fail nlast<half' if r['nlast'] < r['ncb'] / 2 else 'fail nlast>=half')
        by[b].append(r)
    for b, G in sorted(by.items()):
        print(f"    {b:16s} n={len(G):6d} ncb {q([r['ncb'] for r in G],.5):4.0f} kspan {q([r['kspan'] for r in G],.5):6.0f}/{q([r['kspan'] for r in G],.99):6.0f} dconv {q([r['dconv'] for r in G],.5):6.0f} dlast {q([r['dlast'] for r in G],.5):6.0f} swait {q([r['swait'] for r in G],.5):6.0f} L {q([r['L'] for r in G],.5):6.0f} gspan {q([r['gspan'] for r in G],.5):6.0f}")
    # one failing block holds the TB: within the same ncb class, kspan of CRC-fail grants with exactly 1 limit block vs all blocks at the limit
    print('  kspan p50 by ncb bucket: converged-only vs >=1 limit block (us)')
    nb = collections.defaultdict(lambda: ([], []))
    for r in lv:
        bkt = 1 if r['ncb'] == 1 else 1 << (r['ncb'] - 1).bit_length()
        nb[bkt][1 if r['nlast'] else 0].append(r)
    for bkt, (a, b) in sorted(nb.items()):
        f1 = [r['kspan'] for r in b if r['nlast'] == 1]
        print(f"    ncb<={bkt:3d}: conv-only n={len(a):6d} kspan {q([r['kspan'] for r in a],.5):6.0f} | >=1 limit n={len(b):6d} kspan {q([r['kspan'] for r in b],.5):6.0f} (exactly 1 limit: n={len(f1)} kspan {q(f1,.5):6.0f})")
    # iterations of converged blocks
    it = [r['itsum'] / r['nconv'] for r in lv if r['nconv']]
    print(f"  converged-block snapshots per grant: mean-of-grant p50 {q(it,.5):.2f} p90 {q(it,.9):.2f}; it_conv_max p50 {q([r['itc'] for r in lv if r['nconv']],.5)} p99 {q([r['itc'] for r in lv if r['nconv']],.99)}")
