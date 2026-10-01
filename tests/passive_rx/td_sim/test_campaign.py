import json, os, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CAMP = os.path.join(HERE, "campaign.py")

FAKE = """#!/usr/bin/env python3
import sys, json
a = sys.argv[1:]
d = dict(zip(a[0::2], a[1::2]))
acq = int(d["--acq"]); sib = int(d.get("--sib1", 0)); rx = int(d["--n-rx"])
for i in range(acq):
    for k in range(4):
        print(json.dumps({"acq": i, "rnti_rank": k, "grants": 100, "seconds": 10.0 * (1 + sib) + (0 if k < 2 else -5),
                          "truth_table": k % 3, "winner_ok": True, "wrong": int(rx == 1 and i == 0 and k == 0), "undecidable": int(i == 1 and k == 3),
                          "n_full": 100, "n_probe": 200, "gated_phys": 1, "gated_chan": 2, "promotions": 0, "withdrawals": int(k == 1),
                          "oracle_state": "miss" if (k == 0 and i == 0) else ("wrong" if (k == 1 and i == 0) else "ok")}))
print(json.dumps({"summary": {"acq": acq, "harq_trap_passes": 7, "false_passes": 2,
                                  "fail_opens": 5, "active_start_mean": 12.5, "recovery_grants": 321.0, "recovery_rntis": 2.5, "untrusted_after": 3}}))
"""


class Campaign(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.d, True)
        self.sim = os.path.join(self.d, "fake_sim.py")
        open(self.sim, "w").write(FAKE)
        os.chmod(self.sim, 0o755)
        self.mx = os.path.join(self.d, "m.json")
        json.dump({"arms": {"base": {}, "k2": {"K": 2}}, "cells": {"SA": {"sib1": 1}, "NSA-like": {"sib1": 0}},
                   "rx": [4, 1], "acq": 3}, open(self.mx, "w"))

    def test_matrix_results_and_summary(self):
        out = os.path.join(self.d, "o")
        subprocess.run([sys.executable, CAMP, "--sim", self.sim, "--matrix", self.mx, "--out", out], check=True)
        res = [json.loads(l) for l in open(os.path.join(out, "results.jsonl"))]
        self.assertEqual(len(res), 2 * 2 * 2 * 3 * 4)  # arms x cells x rx x acq x rntis
        self.assertEqual({(r["arm"], r["cell"], r["rx"]) for r in res}.__len__(), 8)
        md = open(os.path.join(out, "summary.md")).read()
        self.assertIn("SIMULATED", md)
        sa = [l for l in md.splitlines() if l.startswith("| base | SA | 1 | - ")][0]
        # SA: sib1=1 -> 20 s cold, 15 s steady; the censored RNTI (acq 1, rank 3) is excluded from the quantiles/means;
        # wrong 1 (acq 0 rank 0 at rx 1); undecidable 1 (one per cell/arm/rx); gated 3 per RNTI x 12 = 36 on 12 RNTIs.
        self.assertIn("| 20.0 | 20.0 | 15.0 |", sa)
        self.assertIn("| 1 | 1 | 1200 | 2400 | 36 |", sa)
        # mean over 11 decided RNTIs: (4*20 + ... ) computed by hand: ranks 0,1 -> 20 s (6 RNTIs), ranks 2,3 -> 15 s (5 decided)
        self.assertIn("| %.2f |" % ((6 * 20.0 + 5 * 15.0) / 11), sa)
        nsa = [l for l in md.splitlines() if l.startswith("| k2 | NSA-like | 4 | - ")][0]
        hdr = [c.strip() for c in [l for l in md.splitlines() if l.startswith("| arm ")][0].strip("|").split("|")]
        row = dict(zip(hdr, [c.strip() for c in nsa.strip("|").split("|")]))
        self.assertEqual(row["cold median s"], "10.0")
        self.assertEqual(row["cold p95 s"], "10.0")
        self.assertEqual(row["steady median s"], "5.0")
        self.assertEqual(row["wrong"], "0")
        self.assertEqual(row["undecidable"], "1")
        self.assertEqual(row["gated"], "36")
        # truth table 0: 5 decided (censored one excluded), median 10, mean (3*10+2*5)/5 = 8
        self.assertEqual(row["tbl 0"], "5/10.0/8.0/0")
        # new realism columns: one miss and one wrong RNTI (acq 0, ranks 0/1); summary counters 7 and 2
        self.assertEqual(row["oracle_miss_rntis"], "1")
        self.assertEqual(row["oracle_wrong_rntis"], "1")
        self.assertEqual(row["harq_trap_passes"], "7")
        self.assertEqual(row["false_passes"], "2")
        # field-book-2 columns: summary values pass through, withdrawals are summed over RNTI records (one per acquisition: 3)
        self.assertEqual(row["fail_opens"], "5")
        self.assertEqual(row["active_start_mean"], "12.5")
        self.assertEqual(row["recovery_grants"], "321.0")
        self.assertEqual(row["recovery_rntis"], "2.50")
        self.assertEqual(row["withdrawals"], "3")
        self.assertEqual(row["untrusted_after"], "3")


if __name__ == "__main__":
    unittest.main()
