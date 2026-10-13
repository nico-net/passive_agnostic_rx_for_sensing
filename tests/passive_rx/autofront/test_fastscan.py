import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from fastscan import *
from autofront import apply_refine, tune, spec_line

def test_cfo_phase():
    for f in (-4850.0, 0.0, 2225.0, 6900.0, -6900.0):         # lag 2*SYM at 7.68 MS/s: unambiguous +-7007 Hz
        p = np.exp(2j * np.pi * f * 2 * SYM / (FS / 16)); assert abs(cfo_from_phase(p) - f) < 1e-6
    assert abs(cfo_from_phase(np.exp(2j * np.pi * 8000 * 2 * SYM / (FS / 16))) - (8000 - 14015.3)) < 1     # alias beyond +-7007
def test_pci_group():
    sc = np.full(336, 1.0); sc[170] = 100.0; assert pci_group(sc, 1) == ([511], 10.0)                         # one cell
    sc = np.full(336, 1.0); sc[169] = 100.0; sc[1] = 60.0; g, r = pci_group(sc, 1); assert g == [508, 4] and r > 2        # two cells, same N_ID_2
    sc = np.ones(336) * np.linspace(1, 1.2, 336); assert pci_group(sc, 0)[0] == []                          # noise: no group
def test_clusters():
    rng = np.random.default_rng(1); ccs = []
    for _ in range(20):
        c = rng.random(49); c[10] += 500; c[20] += 300; ccs.append(c)                                       # two PSS timings, 10 apart
    cl = clusters(ccs); assert sorted(x[0] for x in cl) == [10, 20] and all(sum(i is not None for i in x[1]) == 20 for x in cl)
    assert clusters([rng.random(49) for _ in range(20)]) == []                                                # noise only
def test_period_k():
    a = np.ones((8, 3, 100), np.float32); a[3, 1, 40] = 50; a[7, 1, 40] = 50                               # burst in slots 3, 7: period 4 chunks
    assert period_k(a, 1, 40, 8, 16) == (4, 3)
    a[:, 1, 40] = 50; assert period_k(a, 1, 40, 8, 16)[0] == 1
def test_keep_dedupe():
    r = [dict(gscn=1, pci=2, sss_ratio=4, dmrs_ratio=4), dict(gscn=2, pci=3, sss_ratio=2.9, dmrs_ratio=2.2), dict(gscn=4, pci=5, sss_ratio=3.74, dmrs_ratio=2.47), dict(gscn=5, pci=6, sss_ratio=3.5, dmrs_ratio=1.5),
         dict(gscn=2, pci=4, sss_ratio=2.1, dmrs_ratio=2.2, n_cells=2), dict(gscn=3, pci=9, sss_ratio=1.3, dmrs_ratio=9)]
    assert sorted(c['pci'] for c in keep(r)) == [2, 4, 5]
    d = dedupe([dict(gscn=7895, pci=508, sss_ratio=2.1), dict(gscn=7896, pci=508, sss_ratio=3.9), dict(gscn=7895, pci=4, sss_ratio=2.1)])
    assert [(c['gscn'], c['pci']) for c in d] == [(7895, 4), (7896, 508)]
def test_refine_json():
    cells = [dict(gscn=8007, pci=923, cfo_hz=-4850)]; apply_refine(cells, 923, dict(k_ssb=10, sib1_nrb=217, offset_to_point_a=38, mcc='222', mnc='01'))
    c = json.loads(json.dumps(cells))[0]; assert c['tune_pass2']['ssb'] == 1169 and c['tune_pass2']['C_hz'] == 3731910000 and c['offset_to_point_a'] == 38
    assert spec_line('pci923', 'e.env', 'c.cfg', tune(c, c['k_ssb'])) == 'pci923:e.env:c.cfg:3731910000:1169:-4850'
if __name__ == '__main__':
    for n, f in list(globals().items()):
        if n.startswith('test_'): f(); print('ok', n)
