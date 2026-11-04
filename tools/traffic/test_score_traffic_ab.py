import os, sys, unittest
sys.path.insert(0, os.path.dirname(__file__))
import score_traffic_ab as S

def run(acc=100, gen=None, lost=0, solved=50, drop=10, cpu=5.0, p99=20, pct=None):
    return dict(drop_pct=drop / 100.0 if pct is None else pct, acc=acc, gen=acc if gen is None else gen, lost=lost, solved=solved, drop=drop, cpu=cpu, emit_p99=p99)

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
        self.assertIsNone(res([run(solved=0)] * 3)["solved RNTIs"])  # n/a, never a failure
        self.assertFalse(res([run(drop=20)] * 3)["scanq drop_pct<=L+sd"])
        self.assertFalse(res([run(cpu=6.05)] * 3)["cpu T<L-sd"])  # within L sd (6.1 +- 0.1)

    def test_metrics_last_line(self):
        log = ('x ISAC_METRICS {"cell":0,"pci":1,"dci_rec_lost":0}\n'
               'x ISAC_METRICS {"cell":0,"pci":1,"dci_rec_lost":3}\n'
               'x ISAC_METRICS {"cell":1,"pci":2,"dci_rec_lost":7}\n')
        m = S.parse_metrics(log)
        self.assertEqual((m[0]["dci_rec_lost"], m[1]["dci_rec_lost"]), (3, 7))

    def test_records_mapping(self):
        import tempfile, json
        d = tempfile.mkdtemp()
        dci = lambda r, st: json.dumps({"type": "dci", "rnti": r, "layout_state": st}, separators=(",", ":")) + "\n"
        with open(d + "/sense_records_last.jsonl", "w") as f: f.write(dci(1, "solved") + dci(2, "fixed") + '{"type":"dci","rnt\n')
        with open(d + "/sense_records_last.cell1.jsonl", "w") as f: f.write(dci(7, "solved") + dci(8, "solved"))
        self.assertEqual(S.scan_records(d + "/sense_records_last.jsonl"), (1, 0))
        m = {"scanq_queued": 1000, "scanq_drop_full": 5, "dci_accepted_10": 3}
        self.assertEqual(S.cell_stats(d, "T", 1, m)["solved"], 2)
        self.assertAlmostEqual(S.cell_stats(d, "T", 0, m)["drop_pct"], 0.5)

    def test_tag(self):
        self.assertEqual(S.parse_tag("tmT2S415"), ("T", "S415"))
        self.assertEqual(S.parse_tag("tmL1Br"), ("L", "B"))

if __name__ == "__main__":
    unittest.main()
