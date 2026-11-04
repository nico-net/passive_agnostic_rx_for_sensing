import os, sys, unittest
sys.path.insert(0, os.path.dirname(__file__))
import score_traffic_ab as S

def run(acc=100, gen=None, lost=0, solved=50, drop=10, cpu=5.0, p99=20):
    return dict(acc=acc, gen=acc if gen is None else gen, lost=lost, solved=solved, drop=drop, cpu=cpu, emit_p99=p99)

L = [run(solved=100, drop=10, cpu=6.0), run(solved=100, drop=14, cpu=6.2), run(solved=100, drop=12, cpu=6.1)]
def res(T):
    return {b: ok for b, ok, _ in S.evaluate(L, T)}

class T(unittest.TestCase):
    def test_bounds(self):
        good = [run(solved=95, drop=11, cpu=4.0)] * 3
        r = res(good)
        self.assertTrue(all(v is not False for v in r.values()), r)
        self.assertFalse(res([run(acc=100, gen=99)] * 3)["accepted==generated"])
        self.assertFalse(res([run(), run(lost=1), run()])["rec_lost==0"])
        self.assertFalse(res([run(solved=80)] * 3)["solved>=90%L"])
        self.assertFalse(res([run(drop=20)] * 3)["scanq_drop<=L+sd"])
        self.assertFalse(res([run(cpu=6.5)] * 3)["cpu T<L"])

    def test_metrics_last_line(self):
        log = ('x ISAC_METRICS {"cell":0,"pci":1,"dci_rec_lost":0}\n'
               'x ISAC_METRICS {"cell":0,"pci":1,"dci_rec_lost":3}\n'
               'x ISAC_METRICS {"cell":1,"pci":2,"dci_rec_lost":7}\n')
        m = S.parse_metrics(log)
        self.assertEqual((m[0]["dci_rec_lost"], m[1]["dci_rec_lost"]), (3, 7))

    def test_tag(self):
        self.assertEqual(S.parse_tag("tmT2S415"), ("T", "S415"))
        self.assertEqual(S.parse_tag("tmL1Br"), ("L", "B"))

if __name__ == "__main__":
    unittest.main()
