#!/usr/bin/env python3
"""opp.py <run dir>... : stitching opportunity per run (never merged across runs/cells).
Per DL slot: PRB sets of (a) CFR-bearing pdsch rs records (valid_mask), (b) proven grants (crc==1 or rs exists).
Output JSON lines per run."""
import sys, json, collections, statistics as st
def mask_prbs(r):
    m = bytes.fromhex(r['valid_mask']) if r.get('valid_mask') else b''
    f = r['first_prb']; return {f + 8*i + j for i, b in enumerate(m) for j in range(8) if b >> j & 1}
def runs(s):
    s = sorted(s); out = []
    for p in s:
        if out and p == out[-1][1] + 1: out[-1][1] = p
        else: out.append([p, p])
    return out
def pct(v, p):
    v = sorted(v); return v[min(len(v)-1, int(p*len(v)))] if v else None
def run(R, ue_only):
    rs = collections.defaultdict(lambda: collections.defaultdict(set))   # slot -> rnti -> prbs (CFR)
    rsmeta = collections.defaultdict(lambda: collections.defaultdict(lambda: [set(), set(), set()]))  # layers, dmrs syms, tbs
    gr = collections.defaultdict(lambda: collections.defaultdict(set))   # slot -> rnti -> prbs (proven grant)
    crcslot = set(); slots = []
    for ln in open(R + '/sense_records_last.jsonl'):
        if '"type":"rs"' in ln and '"source":"pdsch_' in ln:
            r = json.loads(ln)
            if ue_only and r['rnti'] == 65535: continue
            if not r.get('valid_cfr_prbs') or not r.get('valid_mask'): continue
            P = mask_prbs(r); rs[r['abs_slot']][r['rnti']] |= P; gr[r['abs_slot']][r['rnti']] |= P
            rsmeta[r['abs_slot']][r['rnti']][0].add(r.get('est_layers') or r.get('ports'))
            rsmeta[r['abs_slot']][r['rnti']][1].add((r['source'], tuple(r['l_syms'] or [])))
        elif '"type":"grant"' in ln:
            r = json.loads(ln)
            if r['dir'] != 'DL' or r['crc'] != 1: continue
            if ue_only and r['rnti'] == 65535: continue
            gr[r['abs_slot']][r['rnti']] |= set(range(r['start_rb'], r['start_rb'] + r['nb_rb']))
        if '"abs_slot":' in ln and '"type":"rs"' in ln or '"type":"grant"' in ln:
            pass
    allslots = list(rs) + list(gr)
    s0, s1 = min(allslots), max(allslots)
    out = {'run': R.rstrip('/').split('/')[-1], 'ue_only': ue_only, 'dur_s': (s1 - s0) * 5e-4}
    for nm, D in (('cfr', rs), ('grant', gr)):
        single, union, stitched = set(), set(), set()
        nu, nseg, gaps, user_seg = [], [], [], []
        for s, u in D.items():
            tot = set().union(*u.values())
            sw = any(len(p) >= 128 for p in u.values())
            if sw: single.add(s)
            if len(tot) >= 128: union.add(s)
            if len(tot) >= 128 and not sw:
                stitched.add(s); nu.append(len(u))
                segs = runs(tot); nseg.append(len(segs))
                gaps += [segs[i+1][0] - segs[i][1] - 1 for i in range(len(segs)-1)]
                if nm == 'cfr':
                    # users that must be stitched: largest user's share and DM-RS symbol sharing
                    pass
        o = {'slots_single_wide': len(single), 'slots_union_ge128': len(union), 'slots_stitched_only': len(stitched),
             'users_per_stitched': [pct(nu, .5), pct(nu, .9), max(nu, default=None)],
             'segments_per_stitched(contiguous union runs)': [pct(nseg, .5), pct(nseg, .9), max(nseg, default=None)],
             'gap_prb_p50_p90_max': [pct(gaps, .5), pct(gaps, .9), max(gaps, default=None)],
             'frac_stitched_slots_with_gap0': (sum(1 for x in nseg if x == 1) / len(nseg)) if nseg else None}
        for w, nm2 in ((50, '25ms'), (200, '100ms'), (2000, '1s')):
            nb = (s1 - s0) // w
            a = {(s - s0) // w for s in single if (s - s0) // w < nb}; b = a | {(s - s0) // w for s in stitched if (s - s0) // w < nb}
            o['cov_' + nm2] = [round(len(a) / nb, 3), round(len(b) / nb, 3)] if nb else None
        out[nm] = o
    # DM-RS symbol sharing among stitched (cfr) slots
    same = tot_ = 0; nlmix = 0; ndu = collections.Counter()
    for s, u in rs.items():
        if len(set().union(*u.values())) >= 128 and not any(len(p) >= 128 for p in u.values()):
            tot_ += 1
            sy = [frozenset(l for (src, ls) in rsmeta[s][r][1] if src == 'pdsch_dmrs' for l in ls) for r in u]
            same += len(set(sy)) == 1
            lay = set().union(*[rsmeta[s][r][0] for r in u]) - {None}; nlmix += len(lay) > 1
    out['cfr']['stitched_slots_same_dmrs_syms'] = [same, tot_]; out['cfr']['stitched_slots_mixed_layers'] = nlmix
    # cross-slot reference (NOT same-slot stitching): bins whose pooled CFR PRBs reach 128 with no single wide record
    for w, nm2 in ((50, '25ms'), (200, '100ms')):
        nb = (s1 - s0) // w; pool = collections.defaultdict(set); wide = set()
        for s, u in rs.items():
            b = (s - s0) // w
            if b >= nb: continue
            for p in u.values(): pool[b] |= p; wide.add(b) if len(p) >= 128 else None
        out['xslot_pool_ge128_' + nm2] = [round(len(wide) / nb, 3), round(len(wide | {b for b, p in pool.items() if len(p) >= 128}) / nb, 3)]
    return out
if __name__ == '__main__':
    for R in sys.argv[1:]:
        for ue in (False, True): print(json.dumps(run(R, ue)))
