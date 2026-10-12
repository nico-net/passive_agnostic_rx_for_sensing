import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from autofront import *

def test_gscn(): assert gscn_hz(7971) == 3679_680_000 and gscn_hz(8007) == 3731_520_000 and gscn_hz(7933) == 3624_960_000
def test_first_pass_415(): t = tune(dict(gscn=7971, cfo_hz=6900.4)); assert (t['C_hz'], t['ssb'], t['initial_fo']) == (3680_010_000, 1171, 6900)
def test_second_pass():
    t = tune(dict(gscn=8007, cfo_hz=-4800), 10); assert (t['C_hz'], t['ssb']) == (3731_910_000, 1169)    # TIM 923 hand triple
    t = tune(dict(gscn=7971, cfo_hz=0), 14); assert (t['C_hz'], t['ssb']) == (3680_010_000, 1171)
    t = tune(dict(gscn=7933, cfo_hz=0), 12); assert (t['C_hz'], t['ssb']) == (3625_320_000, 1170)        # receiver KSSB_GRID hint
    assert tune(dict(gscn=7933, cfo_hz=0), 13)['k_ssb_odd']
def test_grid():
    g = cfo_grid(3.6e9); assert g == sorted(g) and 0 in g and max(g) >= 2e-6*3.6e9 and len(g) == 3     # 7.2 kHz tol, 7.5 kHz step
    assert len(cfo_grid(3.6e9, 0)) == 1
def test_pick():
    r = [dict(gscn=1, pci=2, sss_ratio=4, dmrs_ratio=4), dict(gscn=1, pci=2, sss_ratio=5, dmrs_ratio=4), dict(gscn=2, pci=3, sss_ratio=1.1, dmrs_ratio=9)]
    p = pick_cells(r); assert len(p) == 1 and p[0]['sss_ratio'] == 5
def test_log():
    o = refine_from_log("\x1b[0m[MAC] SENSING: MIB fields sfn=662 k_ssb=12 scs_common=1\nInitial sync successful, PCI: 403\n"
        "[NR_MAC] PASSIVE: cell config extracted from SIB1 -- PCI=923 DL: 217 PRB @ SCS=1 offsetToPointA=38 UL\nSIB1 CELL mcc=222 mnc=01 x")
    assert o == dict(k_ssb=12, pci=403, sib1_nrb=217, offset_to_point_a=38, mcc='222', mnc='01')
def test_spec_json():
    c = dict(gscn=7971, cfo_hz=6900, pci=415); s = spec_line('pci415', 'e.env', 'c.cfg', tune(c)); assert s == 'pci415:e.env:c.cfg:3680010000:1171:6900'
    assert json.loads(json.dumps(dict(c, tune_pass1=tune(c))))['tune_pass1']['ssb'] == 1171
if __name__ == '__main__':
    for n, f in list(globals().items()):
        if n.startswith('test_'): f(); print('ok', n)
