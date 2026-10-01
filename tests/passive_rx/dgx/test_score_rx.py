import json, os, shutil, subprocess, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
SCORE = os.path.join(HERE, "score_rx.py")
FIXTURE = os.path.join(HERE, "fixtures", "base_r1_rx.log")

LOG = """\
5.149 [PHY]    Initial sync successful, PCI: 0
14.147 [PHY]    blind PDCCH rnti_seen x rnti=0x1234 sfn=1
14.300 [PHY]    SENSING: multi-CORESET bank add index=0 offset=0 span=102 symbol=0 mapping=0/0/0 len=46
14.892 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=0 S=1 L=13 mask=0x804 table=0
19.000 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=2 S=1 L=5 mask=0x4 table=0
140.0 [PHY]    SENSING: LDPCDIAG ok=57897 seg_fail=468 tb_fail=0 zero_tb=0 iface_err=0 segs_decoded=0/1 (0.0%)
140.1 [PHY]    SENSING: PDSCHQ queued=58414 decoded=58414 crc_ok=57897 (99.1%) dropped[full=0 stale=0] max_lag_slots=12/20
140.2 [PHY]    SENSING: blind PDCCH monitor summary: occasions=1 scanq[queued=404224 done=404061 drop_full=161 drop_stale=0 maxlag=8] last_reject=x
"""
TIME = "\tPercent of CPU this job got: 299%\n\tMaximum resident set size (kbytes): 687992\nrx_rc=124\n"

class ScoreRx(unittest.TestCase):
    def make_arm(self, log=LOG, time=TIME):
        d = tempfile.mkdtemp(); os.makedirs(os.path.join(d, "rx"))
        for name, txt in (("rx.log", log), ("time.txt", time)):
            with open(os.path.join(d, "rx", name), "w") as f:
                f.write(txt)
        return d

    def score(self, d):
        out = subprocess.run([sys.executable, SCORE, "--json", d], capture_output=True, text=True, check=True).stdout
        return json.loads(out.strip().splitlines()[-1])

    def test_full_run(self):
        s = self.score(self.make_arm())
        self.assertAlmostEqual(s["ttc_s"], 0.745, places=3)
        self.assertEqual(s["n_converged"], 2)
        self.assertEqual(s["bank_len"], 46)
        self.assertAlmostEqual(s["crc_pct"], 99.11, places=2)
        self.assertAlmostEqual(s["drop_full_pct"], 0.0398, places=4)
        self.assertEqual(s["cpu_pct"], 299)

    def test_run_that_never_synced_scores_nulls_not_crash(self):
        s = self.score(self.make_arm(log="4.2 [PHY] Initial sync: pbch not decoded on any branch\n", time="rx_rc=124\n"))
        self.assertIsNone(s["sync_s"]); self.assertIsNone(s["crc_pct"]); self.assertEqual(s["n_converged"], 0)

    def test_ansi_colours_are_stripped(self):
        s = self.score(self.make_arm(log="\x1b[32m5.0 [PHY]    Initial sync successful, PCI: 0\x1b[0m\n"))
        self.assertEqual(s["sync_s"], 5.0)

    @unittest.skipUnless(os.path.exists(FIXTURE), "fixture base_r1_rx.log not yet created (plan Task A1 Step 7)")
    def test_real_baseline_fixture(self):
        d = tempfile.mkdtemp(); os.makedirs(os.path.join(d, "rx"))
        shutil.copy(FIXTURE, os.path.join(d, "rx", "rx.log"))
        s = self.score(d)
        self.assertIsNotNone(s["sync_s"])
        self.assertGreaterEqual(s["n_converged"], 1)
        self.assertIsNotNone(s["crc_pct"]); self.assertTrue(80 <= s["crc_pct"] <= 100)
        self.assertIsNotNone(s["drop_full_pct"]); self.assertIsNotNone(s["bank_len"])

if __name__ == "__main__":
    unittest.main()
