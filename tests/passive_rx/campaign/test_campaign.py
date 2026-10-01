import json, os, shutil, signal, subprocess, sys, tempfile, time, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); CAMP = os.path.join(HERE, "campaign.py")
sys.path.insert(0, HERE)

def _jl(p):
    with open(p) as f:
        return json.load(f)

def _rd(p):
    with open(p) as f:
        return f.read()

def run(*a, env=None, **kw):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([sys.executable, CAMP, *a], capture_output=True, text=True, env=e, **kw)

class Campaign(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, self.root, True)

    def new(self):
        r = run("new", "--name", "t", "--site", "DEIB", "--cell", "unknown", "--notes", "unit", "--root", self.root, check=True)
        return r.stdout.strip()

    def test_manifest_records_build_and_host(self):
        d = self.new()
        m = _jl(os.path.join(d, "manifest.json"))
        for k in ("git_commit", "git_dirty", "hostname", "arch", "kernel", "created_utc", "site", "cell", "uhd_version", "sens6_frozen_ok"):
            self.assertIn(k, m)

    def test_run_captures_log_and_verdict(self):
        d = self.new()
        script = "for i in 1 2 3; do echo \"$i.0 [PHY]    Initial sync successful, PCI: 7\"; done; " \
                 "echo '{\"schema\":1,\"acq_state\":\"PBCH_LOCKED\"}' > \"$ISAC_METRICS_PATH\""
        run("run", d, "--arm", "fake", "--secs", "5", "--", "bash", "-c", script, check=True)
        rd = os.path.join(d, "runs", "001_fake")
        self.assertTrue(os.path.exists(os.path.join(rd, "rx.log")))
        v = _jl(os.path.join(rd, "verdict.json"))
        self.assertEqual(v["last_metrics"]["acq_state"], "PBCH_LOCKED")
        self.assertEqual(len(_rd(os.path.join(d, "index.jsonl")).splitlines()), 1)

    def test_rfstall_is_void(self):
        d = self.new()
        run("run", d, "--arm", "st", "--secs", "5", "--", "bash", "-c",
            "echo '1.0 [PHY] Initial sync successful, PCI: 1'; echo '2.0 SENSING: RFSTALL USRP_RX_READ reason=x'", check=True)
        v = _jl(os.path.join(d, "runs", "001_st", "verdict.json"))
        self.assertEqual(v["verdict"], "VOID_RFSTALL")

    def test_sigterm_mid_run_leaves_interrupted_record(self):
        d = self.new()
        p = subprocess.Popen([sys.executable, CAMP, "run", d, "--arm", "long", "--secs", "60", "--", "bash", "-c", "echo partial_line; sleep 60"], stdout=subprocess.DEVNULL)
        time.sleep(1.5); p.send_signal(signal.SIGTERM); p.wait(timeout=20)
        rd = os.path.join(d, "runs", "001_long")
        rj = _jl(os.path.join(rd, "run.json"))
        self.assertEqual(rj["status"], "interrupted")
        self.assertTrue(os.path.exists(os.path.join(d, "manifest.json")))
        self.assertIn("partial_line", _rd(os.path.join(rd, "rx.log")))
        self.assertEqual(_jl(os.path.join(rd, "verdict.json"))["verdict"], "INTERRUPTED")

    def test_sighup_mid_run_leaves_interrupted_record(self):
        d = self.new()
        p = subprocess.Popen([sys.executable, CAMP, "run", d, "--arm", "long", "--secs", "60", "--", "sleep", "60"], stdout=subprocess.DEVNULL)
        time.sleep(1.5); p.send_signal(signal.SIGHUP); p.wait(timeout=20)
        with open(os.path.join(d, "runs", "001_long", "run.json")) as f:
            self.assertEqual(json.load(f)["status"], "interrupted")

    def test_manifest_sens6_gate_uses_conf_glob(self):
        with open(CAMP) as f:
            self.assertIn("tests/passive_rx/*.conf", f.read())

    def test_sigint_mid_run_leaves_interrupted_record(self):
        d = self.new()
        p = subprocess.Popen([sys.executable, CAMP, "run", d, "--arm", "long", "--secs", "60", "--", "sleep", "60"], stdout=subprocess.DEVNULL)
        time.sleep(1.5); p.send_signal(signal.SIGINT); p.wait(timeout=20)
        rj = _jl(os.path.join(d, "runs", "001_long", "run.json"))
        self.assertEqual(rj["status"], "interrupted")

    def test_silent_child_hits_deadline(self):
        d = self.new()
        t0 = time.time()
        run("run", d, "--arm", "silent", "--secs", "1", "--", "sleep", "60", env={"CAMPAIGN_GRACE_S": "1"}, check=True, timeout=30)
        self.assertLess(time.time() - t0, 20)
        rj = _jl(os.path.join(d, "runs", "001_silent", "run.json"))
        self.assertTrue(rj["timed_out"])

    def test_escalation_is_sigterm_not_sigkill(self):
        d = self.new()
        script = 'trap "" INT; trap "echo got_term; exit 0" TERM; while :; do sleep 0.2; done'
        run("run", d, "--arm", "stub", "--secs", "1", "--", "bash", "-c", script,
            env={"CAMPAIGN_GRACE_S": "1", "CAMPAIGN_TERM_GRACE_S": "1"}, check=True, timeout=30)
        rd = os.path.join(d, "runs", "001_stub")
        self.assertIn("got_term", _rd(os.path.join(rd, "rx.log")))
        self.assertEqual(_jl(os.path.join(rd, "run.json"))["rc"], 0)

def mk(rd, rel, text):
    os.makedirs(os.path.dirname(os.path.join(rd, rel)), exist_ok=True)
    with open(os.path.join(rd, rel), "w") as f:
        f.write(text)

def tmp(tc):
    d = tempfile.mkdtemp(); tc.addCleanup(shutil.rmtree, d, True); return d

class Summarize(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, self.root, True)
        self.d = run("new", "--name", "t", "--site", "s", "--cell", "c", "--root", self.root, check=True).stdout.strip()

    def test_two_runs_summary(self):
        ok = "echo '1.0 [PHY] Initial sync successful, PCI: 1'"
        run("run", self.d, "--arm", "a", "--secs", "5", "--", "bash", "-c", ok, check=True)
        run("run", self.d, "--arm", "a", "--secs", "5", "--", "bash", "-c", "echo nothing", check=True)
        r = run("summarize", self.d, check=True)
        with open(os.path.join(self.d, "summary.json")) as f:
            sj = json.load(f)
        self.assertEqual(sj["a"]["n"], 2); self.assertEqual(sj["a"]["valid"], 1)
        self.assertEqual(sorted(sj["a"]["verdicts"]), ["VALID", "VOID_NO_SYNC"])
        with open(os.path.join(self.d, "summary.md")) as f:
            self.assertIn("| a | 2 | 1 |", f.read())

    def test_stale_running_run_listed_as_interrupted(self):
        rd = os.path.join(self.d, "runs", "001_dead"); os.makedirs(rd)
        mk(rd, "run.json", json.dumps({"arm": "dead", "status": "running"}))
        mk(rd, "rx.log", "1.0 [PHY] Initial sync successful, PCI: 1\n")
        run("summarize", self.d, check=True)
        with open(os.path.join(self.d, "summary.json")) as f:
            sj = json.load(f)
        self.assertEqual(sj["dead"]["verdicts"], ["INTERRUPTED"])
        from verdict import verdict
        v = verdict(rd)
        self.assertEqual(v["verdict"], "INTERRUPTED"); self.assertIn("stale running (runner died)", v["reasons"])

    def test_missing_index_clean_error(self):
        r = run("summarize", self.d)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn("Traceback", r.stderr); self.assertIn("index.jsonl", r.stderr)

class VerdictLayout(unittest.TestCase):
    LOG = "1.0 [PHY]    Initial sync successful, PCI: 0\n"

    def test_arm_subdir_log_is_used(self):
        from verdict import verdict
        rd = tmp(self)
        mk(rd, "rx.log", "wrapper stdout, no sync here\n")  # runner-captured wrapper output
        mk(rd, "arm/rx/rx.log", self.LOG)
        v = verdict(rd)
        self.assertEqual(v["score"]["sync_s"], 1.0)
        self.assertEqual(v["verdict"], "VALID")

    def test_arm_subdir_fault_detected(self):
        from verdict import verdict
        rd = tmp(self); mk(rd, "arm/rx/rx.log", self.LOG + "2.0 SENSING: RFSTALL x\n")
        self.assertEqual(verdict(rd)["verdict"], "VOID_RFSTALL")

    def test_flat_layout_still_works(self):
        from verdict import verdict
        rd = tmp(self); mk(rd, "rx.log", self.LOG)
        self.assertEqual(verdict(rd)["verdict"], "VALID")

if __name__ == "__main__":
    unittest.main()
