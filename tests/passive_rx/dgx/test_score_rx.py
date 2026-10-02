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

POSTCONV_FIXTURE = os.path.join(HERE, "fixtures", "idle_on_r1_trim_rx.log")

# Synthetic: two RNTIs, one with two TDAs; periodic cumulative PDSCHQ samples every 20 s.
LOG_PC = """\
5.0 [PHY]    Initial sync successful, PCI: 0
10.0 [PHY]    SENSING: DCI 1_1 length locked coreset=1 rnti=0x1234 len=46 occasions=1 decodes=1
12.0 [PHY]    SENSING: DCI 1_1 length locked coreset=1 rnti=0x4321 len=46 occasions=1 decodes=1
13.0 [PHY]    SWEEP: rnti=0x1234 CONVERGED tda=0 mapping=A k0=0 mcs_table=0 dmrs_add_pos=1 dmrs_max_len=1 (50/60 trials on the winner, cfg=0x1, plaus_k0=0x1) -- private
13.0 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=0 S=1 L=13 mask=0x804 table=0
30.0 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=2 S=1 L=5 mask=0x4 table=0
20.0 [PHY]    SENSING: LDPCDIAG ok=80 seg_fail=20 tb_fail=0 zero_tb=7 iface_err=0 segs_decoded=0/1 (0.0%)
20.0 [PHY]    SENSING: PDSCHQ queued=100 decoded=100 crc_ok=80 (80.0%) dropped[full=0 stale=0] max_lag_slots=1/20
40.0 [PHY]    SENSING: LDPCDIAG ok=160 seg_fail=30 tb_fail=0 zero_tb=9 iface_err=0 segs_decoded=0/1 (0.0%)
40.0 [PHY]    SENSING: PDSCHQ queued=200 decoded=200 crc_ok=160 (80.0%) dropped[full=0 stale=0] max_lag_slots=1/20
60.0 [PHY]    SENSING: LDPCDIAG ok=258 seg_fail=30 tb_fail=0 zero_tb=9 iface_err=0 segs_decoded=0/1 (0.0%)
60.0 [PHY]    SENSING: PDSCHQ queued=300 decoded=300 crc_ok=258 (86.0%) dropped[full=0 stale=0] max_lag_slots=1/20
"""

class PostConv(unittest.TestCase):
    make_arm = ScoreRx.make_arm
    score = ScoreRx.score

    def test_per_context_convergence(self):
        s = self.score(self.make_arm(LOG_PC))
        c = {(x["rnti"], x["tda"]): x for x in s["contexts"]}
        self.assertEqual(set(c), {("0x1234", 0), ("0x1234", 2)})
        self.assertAlmostEqual(c[("0x1234", 0)]["ttc_s"], 3.0, places=3)   # 13.0 - first sighting 10.0
        self.assertAlmostEqual(c[("0x1234", 2)]["ttc_s"], 20.0, places=3)  # 30.0 - 10.0
        self.assertEqual(c[("0x1234", 0)]["trials"], 60)
        self.assertEqual(c[("0x1234", 0)]["trials_ok"], 50)
        self.assertIsNone(c[("0x1234", 2)]["trials"])  # no SWEEP line for this context
        self.assertEqual(s["ttc_by_tda"], {"0": 3.0, "2": 20.0})
        self.assertEqual(s["n_contexts"], 2)

    def test_post_and_search_crc(self):
        s = self.score(self.make_arm(LOG_PC))
        # last convergence at 30.0 -> first PDSCHQ sample at/after is 40.0 (160/200)
        self.assertAlmostEqual(s["postconv_t_s"], 40.0)
        self.assertEqual(s["postconv_decoded"], 100)
        self.assertAlmostEqual(s["postconv_crc_pct"], 98.0, places=2)   # (258-160)/(300-200)
        self.assertAlmostEqual(s["search_crc_pct"], 80.0, places=2)     # 160/200
        self.assertEqual(s["ldpc_zero_tb"], 9)
        self.assertAlmostEqual(s["crc_pct"], 86.0, places=2)            # legacy key unchanged

    def test_no_sample_after_convergence_gives_none(self):
        log = LOG_PC.replace("30.0 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=2", "70.0 [PHY]    SENSING: Technique D CONVERGED rnti=0x1234 tda=2")
        s = self.score(self.make_arm(log))
        self.assertIsNone(s["postconv_crc_pct"]); self.assertIsNone(s["postconv_t_s"])

    def test_no_convergence(self):
        s = self.score(self.make_arm("5.0 [PHY]    Initial sync successful, PCI: 0\n"))
        self.assertEqual(s["contexts"], []); self.assertEqual(s["n_contexts"], 0)
        self.assertIsNone(s["postconv_crc_pct"]); self.assertIsNone(s["search_crc_pct"])

    def test_real_idle_on_r1_trim(self):
        d = tempfile.mkdtemp(); os.makedirs(os.path.join(d, "rx"))
        shutil.copy(POSTCONV_FIXTURE, os.path.join(d, "rx", "rx.log"))
        s = self.score(d)
        self.assertAlmostEqual(s["ttc_by_tda"]["0"], 2.154, places=3)
        self.assertAlmostEqual(s["ttc_by_tda"]["2"], 22.563, places=3)
        self.assertEqual(s["contexts"][0]["trials"], 110)
        self.assertEqual(s["ldpc_zero_tb"], 259)
        self.assertEqual(s["postconv_crc_pct"], 100.0)
        self.assertAlmostEqual(s["search_crc_pct"], 90.28, places=2)
        self.assertAlmostEqual(s["crc_pct"], 97.57, places=2)

def _gate_cli(d, env=None, mode=None):
    e = {k: v for k, v in os.environ.items() if not k.startswith("GATE_")}
    e.update(env or {})
    r = subprocess.run([sys.executable, SCORE, "--gate", d], capture_output=True, text=True, env=e)
    return r.returncode, r.stdout.strip().splitlines()[-1]

class Gate(unittest.TestCase):
    make_arm = ScoreRx.make_arm
    # LOG_PC-like run, 2 contexts, ttc 3 / 20 s, post 98.0 %, overall 86 %: fails the overall floor by default
    def good(self, post_ok=258, total_ok=None):
        log = LOG_PC.replace("crc_ok=258 (86.0%)", "crc_ok=%d (x)" % post_ok)
        log += "60.0 [PHY]    SENSING: blind PDCCH monitor summary: occasions=1 scanq[queued=1000 done=1000 drop_full=5 drop_stale=0 maxlag=8]\n"
        return self.make_arm(log)

    def test_pass_with_relaxed_floor(self):
        rc, line = _gate_cli(self.good(), dict(GATE_CRC_FLOOR="80", GATE_POSTCONV_CRC_MIN="98.0"))
        self.assertEqual(rc, 0, line); self.assertTrue(line.startswith("PASS "), line)
        for k in ("n_contexts=2", "postconv_crc=98.00", "ttc_tda0=3.000", "ttc_tda2=20.000", "crc_floor", "drop_full="):
            self.assertIn(k, line)

    def test_default_floor_fails_overall_crc(self):
        rc, line = _gate_cli(self.good())
        self.assertEqual(rc, 1); self.assertTrue(line.startswith("FAIL ")); self.assertIn("crc_floor=86.00<", line)

    def test_each_criterion_can_fail(self):
        base = dict(GATE_CRC_FLOOR="80", GATE_POSTCONV_CRC_MIN="98.0")
        for over, key in ((dict(GATE_POSTCONV_CRC_MIN="98.5"), "postconv_crc"), (dict(GATE_TTC_MAX_TDA0="2.9"), "ttc_tda0"),
                          (dict(GATE_TTC_MAX_TDA2="19"), "ttc_tda2"), (dict(GATE_NCTX_MIN="3"), "n_contexts"),
                          (dict(GATE_DROP_MAX="0.1"), "drop_full")):
            rc, line = _gate_cli(self.good(), dict(base, **over))
            self.assertEqual(rc, 1, (key, line)); self.assertIn(key + "=", line.split("FAIL", 1)[1])

    def test_legacy_mode(self):
        rc, line = _gate_cli(self.good(), dict(GATE_MODE="legacy"))
        self.assertEqual(rc, 1); self.assertIn("crc=86.00<98.0", line)
        rc, line = _gate_cli(self.good(), dict(GATE_MODE="legacy", GATE_CRC_MIN="85"))
        self.assertEqual(rc, 0, line)

    def test_missing_context_fails(self):
        rc, line = _gate_cli(self.make_arm("5.0 [PHY]    Initial sync successful, PCI: 0\n"))
        self.assertEqual(rc, 1); self.assertTrue(line.startswith("FAIL "))

if __name__ == "__main__":
    unittest.main()
