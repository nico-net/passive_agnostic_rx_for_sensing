# tests/passive_rx/test_health_grid.py
import tempfile, unittest
import health_grid as hg

def ev(fs="10.3", rnti="0x1", crc=1, mcs=5, tbl=0, nl=1, nsym=12, rb="0+20", snr="20.0,-99,-99,-99", probe=0, rv=0):
    return (f"[PHY]    SENSING: EVMGRANT rnti={rnti} {fs} crc={crc} mcs={mcs} tbl={tbl} Qm=2 Nl={nl} rb={rb} nsym={nsym} "
            f"evm=1 snr=1 probe={probe} rv={rv} ndi=0 pid=0 tbs=96 snr_l={snr} l2=1 e2e=1,1,1,1,1,1,300,1 cb=1\n")

def write(lines):
    f = tempfile.NamedTemporaryFile('w', suffix='.log', delete=False); f.writelines(lines); f.close(); return f.name

class HealthGridTest(unittest.TestCase):
    def test_class_key_and_recovery_credit(self):
        p = write([ev(fs="1.1", crc=0), "[PHY]    SENSING: RECOVLATE rnti=0x1 1.1 abs=1 pid=0 ndi=0 tbs=96 recovered_cfg=1\n",
                   ev(fs="1.2", crc=1)])
        g = hg.grid([p])
        self.assertEqual(list(g.keys()), [(0, 5, '9-24', 1, 12)])
        self.assertEqual(sorted(ok for _, ok in g[(0, 5, '9-24', 1, 12)]), [1, 1])

    def test_small_class_is_unverified(self):
        v = hg.verdict([(10.0, 1)] * 10, min_n=50)
        self.assertEqual(v['status'], 'UNVERIFIED')

    def test_cross_margin_per_class(self):
        s = [(2.5, 0)] * 60 + [(4.5, 1)] * 60   # fails at +2..+4, passes from +4
        v = hg.verdict(s, min_n=50)
        self.assertEqual(v['status'], 'FAIL')    # crc over margin >= 2 is 50 %
        self.assertEqual(v['cross80_db'], 3)     # margin >= 3 keeps only the +4.5 grants -> 100 %

    def test_pass(self):
        v = hg.verdict([(3.0, 1)] * 90 + [(3.0, 0)] * 10, min_n=50)
        self.assertEqual((v['status'], round(v['crc_q'])), ('PASS', 90))

    def test_floor_and_probe_excluded(self):
        g = hg.grid([write([ev(snr="3.9,-99,-99,-99"), ev(probe=1)])])
        self.assertEqual(g, {})

    def test_coverage_lists_missing_mcs(self):
        cov = hg.coverage({(0, 5, '9-24', 1, 12): [(3.0, 1)] * 60})
        self.assertIn((0, 6), cov)
        self.assertNotIn((0, 5), cov)

if __name__ == '__main__':
    unittest.main()
