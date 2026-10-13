#!/usr/bin/env python3
"""autofront.py : automatic blind front end -- raw IQ capture(s) -> cells.json -> receiver triple (-C, --ssb, --initial-fo).
  autofront.py discover CAPDIR... [--out cells.json] [--dur 1] [--start 0] [--nch 4] [--top 25] [--tol-ppm 2] [--sss-min 3] [--dmrs-min 3]
  autofront.py args cells.json [--log run.log]      (receiver args per cell; with --log: second-pass refinement of ONE run.log)
  autofront.py triple cells.json PCI [run.log]       ("C ssb fo" for scripts; with run.log = pass 2)
  autofront.py refine cells.json PCI run.log        (write k_ssb/SIB1 facts of a receiver run into cells.json; specs then use pass 2)
  autofront.py specs cells.json ENVFILE CFGFILE     (OTA window spec list TAG:ENV:CFG:FREQ:SSB:FO)
Detection = ssb_search2 (multi-channel non-coherent PSS fold, SSS->PCI ratio, PBCH-DMRS i_ssb) + a CFO hypothesis grid derived from the
SCS and the oscillator tolerance (no per-cell value) + PSS/SSS phase-slope CFO. Offline analysis; the pure functions below are the C/GPU port contract.
shortcut: 30 kHz SCS raster only (15 kHz NR was not seen at this site), one SSB (strongest) per GSCN, python/CPU."""
import sys, os, re, json, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'survey'))

SCS_HZ = 30e3
SSB_FIRST = 1171       # first-pass --ssb (k_SSB unknown), CARRIERS.md rule
RB_GRID_SC = 1182      # (1182 - ssb) * 30 kHz: receiver grid offset of -C from the GSCN, CARRIERS.md rule

def gscn_hz(g): return 3000_000_000 + (g - 7499) * 1_440_000      # n77/n78 range (3000-24250 MHz), exact integer Hz

def cfo_grid(fc_hz, tol_ppm=2.0, scs=SCS_HZ):
    """CFO hypotheses (Hz): step = SCS/4 (PSS loses ~1 dB at +-step/2), span = X410 + gNB tolerance. No cell-specific value."""
    step = scs / 4; n = math.ceil(tol_ppm * 1e-6 * fc_hz / step)
    return [i * step for i in range(-n, n + 1)]

def ssb_from_k(k_ssb): return 1164 + k_ssb // 2                    # CARRIERS.md: --ssb = 1164 + k_SSB/2
def center_hz(gscn, ssb): return gscn_hz(gscn) + (RB_GRID_SC - ssb) * 30_000

def tune(cell, k_ssb=None):
    """receiver triple for a cell dict {gscn, cfo_hz}; first pass if k_ssb is None."""
    ssb = SSB_FIRST if k_ssb is None else ssb_from_k(k_ssb)
    return dict(C_hz=center_hz(cell['gscn'], ssb), ssb=ssb, initial_fo=int(round(cell['cfo_hz'])), k_ssb=k_ssb,
                k_ssb_odd=bool(k_ssb is not None and k_ssb % 2), pass_=1 if k_ssb is None else 2)

def refine_from_log(text):
    """second-pass inputs from a receiver run.log: k_ssb (MIB), SIB1 carrier (PRB, offsetToPointA), PLMN, PCI."""
    text = re.sub(r'\x1b\[[0-9;]*m', '', text); o = {}
    for key, rx in (('k_ssb', r'MIB fields sfn=\d+ k_ssb=(\d+)'), ('pci', r'Initial sync successful, PCI: (\d+)'),
                    ('sib1_nrb', r'extracted from SIB1 -- PCI=\d+ DL: (\d+) PRB'), ('offset_to_point_a', r'offsetToPointA=(\d+)'),
                    ('mcc', r'SIB1 CELL mcc=(\d+)'), ('mnc', r'SIB1 CELL mcc=\d+ mnc=(\d+)')):
        m = re.search(rx, text)
        if m: o[key] = int(m.group(1)) if key not in ('mcc', 'mnc') else m.group(1)
    return o

def apply_refine(cells, pci, info):
    """pass 2: write the receiver-log facts (k_ssb, SIB1 PRB/offsetToPointA, PLMN) into the cell dict(s) of that PCI; adds tune_pass2."""
    for c in cells:
        if c['pci'] == pci and 'k_ssb' in info:
            c.update({k: info[k] for k in ('k_ssb', 'sib1_nrb', 'offset_to_point_a', 'mcc', 'mnc') if k in info}); c['tune_pass2'] = tune(c, info['k_ssb'])
    return cells

def spec_line(tag, envf, cfgf, t): return f"{tag}:{envf}:{cfgf}:{t['C_hz']}:{t['ssb']}:{t['initial_fo']}"

def pick_cells(rows, sss_min=3.0, dmrs_min=3.0):
    """keep reliable detections, best (highest SSS ratio) per (gscn, pci); rows = list of dicts."""
    best = {}
    for r in rows:
        if r['sss_ratio'] < sss_min or r['dmrs_ratio'] < dmrs_min: continue
        k = (r['gscn'], r['pci'])
        if k not in best or r['sss_ratio'] > best[k]['sss_ratio']: best[k] = r
    return sorted(best.values(), key=lambda r: r['gscn'])

def discover(capdir, dur=1.0, start=0.0, nch=4, top=25, tol_ppm=2.0):
    import numpy as np, ssb_search2 as S
    cap = S.Cap(capdir, nch); c0 = int(start / 0.02); nchunks = min(int(round(dur / 0.02)), cap.n // S.CH - c0)
    D = 16; lim = 61.44 * 0.82
    N0 = int(np.ceil((cap.c - lim - 3000) / 1.44)); N1 = int((cap.c + lim - 3000) / 1.44); Ns = np.arange(N0, N1 + 1)
    base = (3000 + Ns * 1.44 - cap.c) * 1e6 - 30e3           # PSS centre of each GSCN relative to the capture centre
    hyps = cfo_grid(cap.c * 1e6, tol_ppm); fold = 8; best = [None] * len(Ns)
    for h in hyps:                                           # signal at nominal+h appears at 0 after cutting the band at nominal+h
        acc, _ = S.pass1(cap, c0, nchunks, D, S.T_NR, fold, base + h)
        res = [S.foldmetric(acc[g], nchunks) for g in range(len(Ns))]; del acc
        null = float(np.median([r[0] for r in res]))
        for g, r in enumerate(res):
            if best[g] is None or r[0] - null > best[g][0]: best[g] = (r[0] - null, h, r)
    out = []
    for g in np.argsort([-b[0] for b in best])[:top]:
        ex, h, (m, k, sp, t, n) = best[g]
        if ex < 1.0: break
        r2 = S.pass2(cap, c0, nchunks, D, base[g] + h, t, k, sp, n, 30)
        if r2: out.append(dict(gscn=int(7499 + Ns[g]), ssb_mhz=round(3000 + Ns[g] * 1.44, 2), pci=int(r2['pci']), metric_db=round(ex, 2),
            sss_ratio=round(r2['sss_ratio'], 2), dmrs_ratio=round(r2['dmrs_ratio'], 2), i_ssb=r2['i_ssb'], scs_khz=30,
            cfo_hz=round(h + r2['cfo_hz']), cfo_hyp_hz=h, n_occ=r2['n_occ'], capture=os.path.abspath(capdir), capture_centre_mhz=cap.c))
    return out

def main(a):
    cmd = a[1]
    if cmd == 'discover':
        kw = dict(dur=1.0, start=0.0, nch=4, top=25, tol_ppm=2.0); out = 'cells.json'; sss = dmrs = 3.0; dirs = []; i = 2
        while i < len(a):
            if a[i].startswith('--'):
                k = a[i][2:].replace('-', '_')
                if k == 'out': out = a[i + 1]
                elif k == 'sss_min': sss = float(a[i + 1])
                elif k == 'dmrs_min': dmrs = float(a[i + 1])
                else: kw[k] = type(kw[k])(a[i + 1])
                i += 2
            else: dirs.append(a[i]); i += 1
        rows = [r for d in dirs for r in discover(d, **kw)]
        cells = pick_cells(rows, sss, dmrs)
        for c in cells: c['tune_pass1'] = tune(c)
        json.dump(cells, open(out, 'w'), indent=1); print(f'{len(cells)} cells -> {out}')
        for c in cells: print(f"  GSCN {c['gscn']} {c['ssb_mhz']} MHz PCI {c['pci']} i_ssb {c['i_ssb']} sss {c['sss_ratio']} dmrs {c['dmrs_ratio']} cfo {c['cfo_hz']} -> {c['tune_pass1']}")
    elif cmd == 'args':
        cells = json.load(open(a[2])); log = a[a.index('--log') + 1] if '--log' in a else None
        for c in cells:
            r = refine_from_log(open(log).read()) if log else {}
            if log and r.get('pci') not in (None, c['pci']): continue
            t = tune(c, r.get('k_ssb')); print(f"PCI {c['pci']}: -C {t['C_hz']} --ssb {t['ssb']} --initial-fo {t['initial_fo']}  (pass {t['pass_']}" +
                (f", sib1_nrb {r['sib1_nrb']} offsetToPointA {r['offset_to_point_a']}" if 'sib1_nrb' in r else '') + (', k_ssb odd: check' if t['k_ssb_odd'] else '') + ')')
    elif cmd == 'triple':                                    # triple cells.json PCI [run.log] -> "C ssb fo" for scripts
        c = next(c for c in json.load(open(a[2])) if c['pci'] == int(a[3]))
        t = tune(c, refine_from_log(open(a[4]).read()).get('k_ssb') if len(a) > 4 else None); print(t['C_hz'], t['ssb'], t['initial_fo'])
    elif cmd == 'refine':                                    # refine cells.json PCI run.log : pass 2 facts into cells.json (in place)
        cells = json.load(open(a[2])); info = refine_from_log(open(a[4]).read())
        if info.get('pci') not in (None, int(a[3])): sys.exit(f"run.log synced PCI {info['pci']}, not {a[3]}")
        json.dump(apply_refine(cells, int(a[3]), info), open(a[2], 'w'), indent=1); print(info)
    elif cmd == 'specs':
        for c in json.load(open(a[2])): print(spec_line(f"pci{c['pci']}", a[3], a[4], tune(c, c.get('k_ssb'))))
    else: sys.exit(__doc__)
if __name__ == '__main__': main(sys.argv)
