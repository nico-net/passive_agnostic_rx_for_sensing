#!/usr/bin/env python3
"""UL DCI 0_1 field-level consistency check on ULDBG_DCI replay lines (ISAC_UL_DBG_LOG=1).

usage: dci_fields.py run.log [--bwp-size N]
The layouts below were fitted on raw_pci415_12s_b (see docs/superpowers/reports/csd/ULREADY.md); they are
receiver-side readings of the payload bits (TS 38.212 7.3.1.1.2 field order), never gNB input.
Order: id | bwp ind | FDRA(RIV) | TDRA | [FH] | MCS 5 | NDI 1 | RV 2 | HARQ 4 | DAI1 | [DAI2] | TPC 2 | [SRI|PREC] |
       ANT | SRS req | [CSI req|CBG|PTRS|beta] | [DMRS init] | UL-SCH ind."""
import re, sys, collections

LAYOUTS = {  # len -> list of (name, width)
    44: [('id', 1), ('bwp', 1), ('riv', 15), ('tda', 4), ('mcs', 5), ('ndi', 1), ('rv', 2), ('harq', 4),
         ('dai1', 2), ('tpc', 2), ('ant', 3), ('srs', 2), ('dmrs_init', 1), ('ulsch', 1)],
    42: [('id', 1), ('bwp', 1), ('riv', 15), ('tda', 4), ('mcs', 5), ('ndi', 1), ('rv', 2), ('harq', 4),
         ('dai1', 2), ('tpc', 2), ('ant', 2), ('srs', 2), ('ulsch', 1)],   # transform-precoded reading (no DM-RS init bit)
}
UL_SLOTS = {8, 9, 18, 19}   # DDDDDDDSUU x2, mu=1


def fields(payload, length, layout):
    pos, out = 0, {}
    for name, w in layout:
        out[name] = (payload >> (length - pos - w)) & ((1 << w) - 1)
        pos += w
    assert pos == length
    return out


def riv_decode(riv, n):
    l1, s = divmod(riv, n)
    if l1 + 1 + s <= n:
        return s, l1 + 1
    l = n - l1 + 1
    s = n - 1 - s
    return (s, l) if l >= 1 and s + l <= n else None


def main():
    path = sys.argv[1]
    nbwp = int(sys.argv[sys.argv.index('--bwp-size') + 1]) if '--bwp-size' in sys.argv else 217
    rows = []
    for l in open(path, errors='ignore'):
        if 'ULDBG_DCI' not in l:
            continue
        l = re.sub(r'\x1b\[[0-9;]*m', '', l)
        d = dict(x.split('=', 1) for x in l.split() if '=' in x)
        rows.append(dict(abs=int(d['abs']), s=int(d['s']), rnti=d['rnti'], len=int(d['len']), pl=int(d['payload'], 16)))
    for L, lay in sorted(LAYOUTS.items()):
        R = sorted((r for r in rows if r['len'] == L), key=lambda r: r['abs'])
        if not R:
            continue
        print(f'== DCI 0_1 length {L}: {len(R)} grants, {len(set(r["rnti"] for r in R))} RNTIs')
        F = [fields(r['pl'], L, lay) for r in R]
        for k in ('id', 'bwp', 'tda', 'rv', 'dai1', 'tpc', 'ant', 'srs', 'dmrs_init', 'ulsch'):
            if k in F[0]:
                print(f'  {k:9s}', dict(collections.Counter(f[k] for f in F).most_common(6)))
        bad = sum(riv_decode(f['riv'], nbwp) is None for f in F)
        print(f'  FDRA: RIV invalid for BWP={nbwp}: {bad}/{len(F)}')
        mc = collections.Counter(f['mcs'] for f in F)
        print('  MCS>=20:', sum(v for k, v in mc.items() if k >= 20), ' MCS<=9:', sum(v for k, v in mc.items() if k <= 9), ' reserved(>=28):', sum(v for k, v in mc.items() if k >= 28))
        print('  DCI slot-in-frame:', dict(collections.Counter(r['s'] for r in R)))
        # k2 consistent with TDD pattern: TDRA row 0 -> k2 fixed; slots 6,7,16,17 + k2 -> UL slots
        # HARQ / NDI sequences per RNTI
        by = collections.defaultdict(list)
        for r, f in zip(R, F):
            by[r['rnti']].append(f)
        tog = same = rv_bad = 0
        pid_step_ok = pid_step = 0
        for rn, lst in by.items():
            last, prev_pid = {}, None
            for f in lst:
                p = f['harq']
                if prev_pid is not None:
                    pid_step += 1
                    pid_step_ok += (p - prev_pid) % 16 in (0, 1, 2, 3)
                prev_pid = p
                if p in last:
                    if f['ndi'] == last[p]:
                        same += 1
                        rv_bad += f['rv'] == 0   # same NDI with RV0: duplicate detection or missed toggle
                    else:
                        tog += 1
                last[p] = f['ndi']
        print(f'  HARQ pid advances by 0..3 between consecutive grants of an RNTI: {pid_step_ok}/{pid_step}')
        print(f'  NDI toggles on pid reuse: {tog}; unchanged: {same} (of which RV0 {rv_bad})')
        print(f'  max RIV {max(f["riv"] for f in F)}, distinct RNTIs {len(by)}')


if __name__ == '__main__':
    main()
