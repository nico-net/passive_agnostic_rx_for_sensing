# tests/passive_rx/test_gpugraph_tail.py
import os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugraph_tail as gt

def ev(tot, stg, probe=0):
    return (f"[PHY]    SENSING: EVMGRANT rnti=0x1 1.2 crc=1 mcs=10 tbl=0 Qm=2 Nl=1 rb=0+5 nsym=12 probe={probe} rv=0 "
            f"e2e=100,20,30,400,380,1,{tot},1 stg={stg},prep:1,wq:0 cb=1\n")

PASS = "fep:3/3/1,chest:1/1/1,demod:1/1/1,ldpc:0/0/0"
STAGE = "fep:115/115/64,chest:127/127/50,demod:409/368/18,ldpc:216/216/66"

def write(lines):
    f = tempfile.NamedTemporaryFile('w', suffix='.log', delete=False)
    f.writelines(lines); f.close(); return f.name

class GpuGraphTailTest(unittest.TestCase):
    def test_pass_and_stage_body_counted(self):
        rows = gt.parse(write([ev(500, PASS), ev(500, STAGE), ev(600, STAGE, probe=1)]))
        self.assertEqual(len(rows), 2)  # the probe is skipped
        self.assertEqual(sum(not r['stage'] for r in rows), 1)
        self.assertEqual(sum(r['stage'] for r in rows), 1)

    def test_p99_index_rule(self):
        # 100 grants 1..100: goal_metrics' p99 = sorted[int(.99 * 100)] = 100 -> one tail grant, a stage body
        lines = [ev(t, PASS) for t in range(1, 100)] + [ev(100, STAGE)]
        s = gt.summarize(gt.parse(write(lines)))
        self.assertEqual(s['p99'], 100)
        self.assertEqual(s['n_tail'], 1)
        self.assertEqual(s['tail_stage'], 1)
        self.assertAlmostEqual(s['stage_share_all'], 1.0)
        self.assertEqual(s['tail_dec'], 400)

if __name__ == '__main__':
    unittest.main()
