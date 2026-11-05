import json, os, tempfile, unittest, sys
sys.path.insert(0, os.path.dirname(__file__))
import score_fields as sf

J = lambda d: json.dumps(d, separators=(",", ":"))

def dci(D, rnti, fmt, **kw):
    d = dict(type="dci", dir=D, rnti=rnti, dci_format=fmt, rnti_class=0, layout_state="solved", layout_evidence="x",
             fields_evidence="bits", harq_pid=1, ndi=0, rv=0, mcs=10, mcs_table=0, nb_rb=200, nl=2)
    d.update(kw); return json.dumps(d, separators=(",", ":"))

class T(unittest.TestCase):
    def test_counts(self):
        L = [dci("DL", 5, 0), dci("DL", 5, 0, nb_rb=10, mcs=None), dci("DL", 6, 1, rnti_class=2, nb_rb=None),
             dci("UL", 7, 2, layout_state="unsolved"),
             J(dict(type="decode", dir="DL", probe=False, crc=1)), J(dict(type="decode", dir="UL", probe=True, crc=0)),
             "garbage", json.dumps(dict(type="config"))]
        s = sf.score_records(L)
        self.assertEqual((s["DL|dci"], s["rnti2|DL"], s["rnti1|DL"]), (3, 1, 2))
        self.assertEqual((s["DL|fmt|1_1"], s["DL|fmt|1_0"], s["UL|fmt|0_1"]), (2, 1, 1))
        self.assertEqual(s["DL|cls|SI"], 1)
        self.assertEqual((s["DL|nn.1_1|mcs"], s["DL|nn.1_1|nb_rb"], s["DL|nbrb|>=128"], s["DL|nbrb|<16"], s["max|DL|nbrb"]), (1, 2, 1, 1, 200))
        self.assertEqual((s["DL|dec|check|ok"], s["UL|dec|probe|fail"], s.get("UL|dec|probe|ok", 0)), (1, 1, 0))
        self.assertEqual(s["UL|state|unsolved"], 1)

    def test_merge_and_run(self):
        a = sf.score_records([dci("DL", 1, 0)]); b = sf.score_records([dci("DL", 1, 0), dci("DL", 2, 0)])
        m = sf.merge(dict(a), b); self.assertEqual(m["DL|dci"], 3); self.assertEqual(m["max|DL|nbrb"], 200)
        with tempfile.TemporaryDirectory() as t:
            open(t + "/run.log", "w").write('x SENSING: ISAC_METRICS {"cell":0,"pci":415,"scanq_queued":100,"scanq_drop_full":5}\n')
            open(t + "/sense_records_last.jsonl", "w").write(dci("DL", 1, 0) + "\n")
            r = sf.score_run(t)
            self.assertEqual(r[415]["DL|dci"], 1); self.assertAlmostEqual(r[415]["m|drop_pct"], 5.0)

if __name__ == "__main__": unittest.main()
