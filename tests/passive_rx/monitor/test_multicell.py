#!/usr/bin/env python3
"""D2: multi-cell log attribution (plan 2026-10-23-mpci-main-plan.md B.8, decision F5 = "[c<k>] " line prefix).

Fixtures: tests/passive_rx/mpci/fixtures/cell_a.log (PCI 421) and cell_b.log (PCI 508) are two single-cell (unprefixed) logs. A synthetic
2-cell log is interleaved from them (cell a -> c0, cell b -> c1) plus receiver-wide radio lines and an untagged GPU-callback line.
  * the parser gives two independent metric sets equal to the two single-cell logs the mixed one was made from;
  * receiver-wide (radio) lines are counted once and mirrored to every cell;
  * the scorers (goal_metrics, score_rx, score_rx_solver, mpci_runsum) refuse a mixed log without --cell, give the single-cell
    result with --cell K, and keep their output byte-identical on an unprefixed log (golden files made by the pre-D2 scripts).
    python3 -m unittest test_multicell      (run from this directory)
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PR = os.path.dirname(HERE)                       # tests/passive_rx
FIX = os.path.join(PR, "mpci", "fixtures")
sys.path.insert(0, HERE)
sys.path.insert(0, PR)
import rxparse  # noqa: E402
import cellsel  # noqa: E402

RADIO = [
    "RFCHAN ch0 subdev=A:0 port=RX2 gain=38.00 range=[0.0..60.0] freq=3700.080000 MHz rate=122.880000 Msps bw=100.000000 MHz",
    "SENSING: CHPOW sample=123456 dBFS ch0=-41.5 ch1=-42.0 ch2=-41.9 ch3=-43.1",
    "SENSING: RFSTALL USRP_RING reason_code=3",
]
GPU_LINE = "[GPU]    SENSING: gpu callback job done (no cell bound)"


def read(name):
    with open(os.path.join(FIX, name)) as f:
        return f.readlines()


def tag(lines, k, after_ts=True):
    """Tag lines the way the receiver does. after_ts: the run harness' seconds prefix comes first ('12.3 [c1] [PHY] ...')."""
    out = []
    for l in lines:
        if after_ts and l[:1].isdigit():
            ts, rest = l.split(" ", 1)
            out.append("%s [c%d] %s" % (ts, k, rest))
        else:
            out.append("[c%d] %s" % (k, l))
    return out


def interleave(a, b):
    out, i, j = [], 0, 0
    while i < len(a) or j < len(b):
        for _ in range(3):
            if i < len(a):
                out.append(a[i]); i += 1
        for _ in range(2):
            if j < len(b):
                out.append(b[j]); j += 1
    return out


def mixed_lines(after_ts=True):
    rad = ["1.000 [PHY]    " + r + "\n" for r in RADIO]            # radio thread: bound to no cell -> read as [c0] by the LOG layer
    body = interleave(tag(read("cell_a.log"), 0, after_ts), tag(read("cell_b.log"), 1, after_ts))
    gpu = ["200.000 " + GPU_LINE + "\n"]
    return tag(rad, 0, after_ts) + body[:50] + gpu + body[50:]


def feed(lines):
    st = rxparse.RunState()
    for l in lines:
        st.feed_line(l.rstrip("\n"))
    return st


VIEW = ("pdschq", "pdcch", "scanq", "ldpc", "grants", "timing", "td_conv", "pusch", "ul", "rnti_seen", "new_ctx")


def view(snap):
    return json.dumps({k: snap[k] for k in VIEW if k in snap}, sort_keys=True, default=str)


class Attribution(unittest.TestCase):
    def test_split_cell_forms(self):
        self.assertEqual(rxparse.split_cell("[c3] [PHY]    SENSING: PDSCHQ queued=1"), (3, "[PHY]    SENSING: PDSCHQ queued=1"))
        self.assertEqual(rxparse.split_cell("12.5 [c1] [PHY] x"), (1, "12.5 [PHY] x"))
        # colour leftovers of the previous record (ESC[0m, the ESC byte is dropped by the log reader)
        self.assertEqual(rxparse.split_cell("\x1b[0m[c1] [PHY] x")[0], 1)
        self.assertEqual(rxparse.split_cell("[0m[c1] [PHY] x")[0], 1)
        # real output of LOG_I()/nr_cprintf of common/utils/tests/test_nr_log_cell.c (N=2)
        self.assertEqual(rxparse.split_cell("[c0] [PHY]    SENSING: PROBE_LOG cell0")[0], 0)
        self.assertEqual(rxparse.split_cell("[c1] SENSING: PROBE_PRINTF cell1 7")[0], 1)
        for plain in ("[PHY]    SENSING: PDSCHQ queued=1", "5.1 [PHY]    Initial sync successful, PCI: 0", "[c] x", "[cx1] y", "[c1]no space"):
            self.assertEqual(rxparse.split_cell(plain), (None, plain))

    def test_every_parsed_line_type_is_attributable(self):
        """Per-cell types carry the prefix through the router; receiver-wide types are recognised by grammar."""
        wide = set(rxparse.WIDE_TOKENS)
        for tok, h in rxparse._SENSING.items():
            line = "[c2] [PHY]    SENSING: %s x=1" % tok
            k, rest = rxparse.split_cell(line)
            self.assertEqual(k, 2, tok)
            self.assertEqual(rxparse.is_receiver_wide(rest), tok in wide, tok)
        for key, h in rxparse._PLAIN:
            line = "[c2] [PHY]    %s" % key
            k, rest = rxparse.split_cell(line)
            self.assertEqual(k, 2, key)
            self.assertEqual(rxparse.is_receiver_wide(rest), key in ("RFCHAN ch", "CMDLINE:"), key)

    def test_two_independent_metric_sets(self):
        ma = feed(read("cell_a.log")).snapshot()
        mb = feed(read("cell_b.log")).snapshot()
        for after_ts in (True, False):
            st = feed(mixed_lines(after_ts))
            self.assertTrue(st.attributed)
            self.assertEqual(sorted(st.children), [1])
            s0, s1 = st.snapshot_for(None), st.snapshot_for("508")
            self.assertEqual(view(s0), view(ma), "cell 0 differs from the single-cell log (after_ts=%s)" % after_ts)
            self.assertEqual(view(s1), view(mb), "cell 1 differs from the single-cell log (after_ts=%s)" % after_ts)
            self.assertNotEqual(view(s0), view(s1))
            self.assertEqual(sorted(s0["cells"]), ["421", "508"])
            self.assertEqual([s0["cells"][k]["cell_idx"] for k in ("421", "508")], [0, 1])
            self.assertEqual(s1["sel"], {"key": "508", "local": "508", "cell_idx": 1})
            self.assertEqual(s1["pdschq"]["queued"], mb["pdschq"]["queued"])
            self.assertEqual(s1["timing"]["p99age"]["scanq_fresh"], mb["timing"]["p99age"]["scanq_fresh"])

    def test_receiver_wide_lines_once_and_mirrored(self):
        st = feed(mixed_lines())
        r0, r1 = st.snapshot_for("421")["radio"], st.snapshot_for("508")["radio"]
        self.assertEqual(r0["rfstall_n"], 1)          # not 2: one radio line, not one per cell
        self.assertEqual(r1["rfstall_n"], 1)
        self.assertEqual(r0["chpow"], r1["chpow"])
        self.assertEqual(r0["chpow"]["ch0"], -41.5)
        self.assertEqual(r0["rfchan"], r1["rfchan"])
        self.assertIn("ch0", r1["rfchan"])
        # a cell that appears after the radio lines still gets them (replayed into the new child)
        st2 = rxparse.RunState()
        st2.feed_line("[c0] 1.0 [PHY]    " + RADIO[1])
        st2.feed_line("[c5] 2.0 [PHY]    SENSING: PDSCHQ queued=10 decoded=9 crc_ok=8 (88.9%) dropped[full=0 stale=0] max_lag_slots=1/2")
        self.assertEqual(st2.children[5].chpow["ch0"], -41.5)
        self.assertEqual(st2.children[5].pdschq["queued"], 10)
        self.assertEqual(st2.pdschq, {})

    def test_gpu_callback_lines_stay_untagged_and_unattributed(self):
        st = feed(mixed_lines())
        self.assertEqual(rxparse.split_cell("200.000 " + GPU_LINE)[0], None)
        self.assertEqual(sorted(st.children), [1])      # the untagged line creates no cell

    def test_metrics_json_cell_key(self):
        st = rxparse.RunState()
        a = {"schema": 2, "t_mono_ns": 1_000_000_000, "abs_slot": 10, "pci": 421, "pdsch_queued": 5}
        b = {"cell": 1, "schema": 2, "t_mono_ns": 1_000_000_000, "abs_slot": 10, "pci": 508, "pdsch_queued": 7}
        st.feed_metrics(a)
        st.feed_metrics(b)
        self.assertEqual(st.metrics_n, 1)
        self.assertEqual(st.children[1].metrics_n, 1)
        self.assertEqual(st.children[1].metrics_last["pci"], 508)
        # the same record as a prefixed ISAC_METRICS log line goes to the same cell (and is de-duplicated against the file)
        st.feed_line("[c1] [PHY]    SENSING: ISAC_METRICS " + json.dumps(b))
        self.assertEqual(st.children[1].metrics_n, 1)

    def test_single_cell_unchanged(self):
        st = feed(read("cell_a.log"))
        self.assertFalse(st.attributed)
        self.assertEqual(st.children, {})
        self.assertEqual(json.dumps(st.snapshot_for(None), sort_keys=True, default=str), json.dumps(st.snapshot(), sort_keys=True, default=str))

    def test_p99age_line(self):
        # format string of nr_pdsch_passive_queue.c (nr_pdsch_passive_queue_p99_dump)
        line = ("SENSING: P99AGE slots(<2/4/8/12/16/18/24/32/inf) pdsch_enq_real=[10,2,1,0,0,0,0,0,3] pdsch_enq_probe=[0,0,0,0,0,0,0,0,0] "
                "scan_deq=[30,4,0,0,0,0,0,0,1] scanq_fresh=7 retry_chains=3 retry_yields=1 retry_gated=2")
        st = rxparse.RunState()
        st.feed_line("[PHY]    " + line)
        p = st.snapshot()["timing"]["p99age"]
        self.assertEqual(p["bins"][0], "<2")
        self.assertEqual(p["hist"]["pdsch_enq_real"][:3], [10, 2, 1])
        self.assertEqual(p["hist"]["scan_deq"][-1], 1)
        self.assertEqual((p["scanq_fresh"], p["retry_chains"], p["retry_yields"], p["retry_gated"]), (7, 3, 1, 2))
        st2 = rxparse.RunState()
        st2.feed_line("[c1] [PHY]    " + line)
        self.assertEqual(st2.children[1].snapshot()["timing"]["p99age"]["scanq_fresh"], 7)
        self.assertEqual(st2.snapshot()["timing"]["p99age"], {})


def run(script, *args, cwd=None):
    return subprocess.run([sys.executable, script, *args], capture_output=True, text=True, cwd=cwd)


def golden(name):
    with open(os.path.join(FIX, name)) as f:
        return f.read()


def cnt(g):
    """Expected counts of the mixed log's cell fed by cell_<g>.log (independent of the log tagging: counts of the source lines)."""
    return cellsel.counts(read("cell_%s.log" % g))


def cline(g):
    return cellsel.counts_line(cnt(g))


class Scorers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.mixed = os.path.join(cls.tmp, "mixed.log")
        with open(cls.mixed, "w") as f:
            f.writelines(mixed_lines())
        cls.arms = {}
        for name, lines in (("mixed", mixed_lines()), ("arm_a", read("cell_a.log")), ("arm_b", read("cell_b.log")),
                            ("one", tag(read("cell_b.log"), 1))):
            d = os.path.join(cls.tmp, name)
            os.makedirs(os.path.join(d, "rx"))
            with open(os.path.join(d, "rx", "rx.log"), "w") as f:
                f.writelines(lines)
            cls.arms[name] = d

    def test_no_cell_option_anywhere(self):
        for f in ("goal_metrics.py", "dgx/score_rx.py", "dgx/score_rx_solver.py", "mpci/mpci_runsum.py", "cellsel.py"):
            with open(os.path.join(PR, f)) as fh:
                src = fh.read()
            self.assertNotIn("--cell", src, f)
            self.assertNotIn("pop_cell_arg", src, f)
        # the programmatic selector still refuses an unnamed cell on a mixed log
        with self.assertRaises(SystemExit):
            cellsel.prepare(mixed_lines(), None)

    def test_goal_metrics_mixed_scores_every_cell(self):
        gm = os.path.join(PR, "goal_metrics.py")
        r = run(gm, self.mixed)
        self.assertEqual(r.returncode, 0, r.stderr)
        # always-on per-cell extras: PCI label ("?" = the log names no cell), CELL counts line per cell, one UNTAGGED line
        want = "".join("== cell %d (PCI ?)\n%s%s CELL pci=None %s\n" % (k, golden("golden_goal_metrics_%s.txt" % g).replace("mpci/fixtures/cell_%s.log" % g, self.mixed),
                                                                     self.mixed, cline(g)) for k, g in ((0, "a"), (1, "b"))) \
            + "%s UNTAGGED (process-wide, in no cell) %s\n" % (self.mixed, cellsel.counts_line(cellsel.counts([GPU_LINE])))
        self.assertEqual(r.stdout, want)
        r = run(gm, "--json", self.mixed)
        d = json.loads(r.stdout)
        self.assertEqual(sorted(d["cells"]), ["0", "1"])
        for k, g in (("0", "a"), ("1", "b")):
            ref = json.loads(golden("golden_goal_metrics_%s.json" % g))
            ref.pop("run")
            ref.update(pci=None, counts=cnt(g))
            self.assertEqual(d["cells"][k], ref)
        self.assertEqual(d["untagged"], cellsel.counts([GPU_LINE]))
        self.assertNotEqual(d["cells"]["0"], d["cells"]["1"])

    def test_goal_metrics_unprefixed_byte_identical(self):
        for g in "ab":
            r = run(os.path.join(PR, "goal_metrics.py"), "mpci/fixtures/cell_%s.log" % g, cwd=PR)
            self.assertEqual(r.stdout, golden("golden_goal_metrics_%s.txt" % g))
            r = run(os.path.join(PR, "goal_metrics.py"), "--json", "mpci/fixtures/cell_%s.log" % g, cwd=PR)
            self.assertEqual(r.stdout, golden("golden_goal_metrics_%s.json" % g))

    def _arm_run(self, script, arm, *args):
        return run(os.path.join(PR, "dgx", script), *args, os.path.basename(arm), cwd=os.path.dirname(arm))

    def test_score_rx_and_solver_mixed_every_cell(self):
        for script, args, gold in (("score_rx.py", ("--gate",), "golden_score_rx_gate_%s.txt"),
                                   ("score_rx_solver.py", ("--gate",), "golden_score_rx_solver_gate_%s.txt")):
            r = self._arm_run(script, self.arms["mixed"], *args)
            want = "".join("== cell %d (PCI ?)\n  cell pci=None %s\n%s" % (k, cline(g), golden(gold % g).replace("arm_%s" % g, "mixed")) for k, g in ((0, "a"), (1, "b")))
            self.assertEqual(r.stdout, want, script)
            self.assertEqual(r.returncode, 1)                       # worst cell: both FAIL on the fixtures
        for script, gold in (("score_rx.py", "golden_score_rx_%s.json"), ("score_rx_solver.py", "golden_score_rx_solver_%s.json")):
            args = ("--json",) if script == "score_rx.py" else ()
            r = self._arm_run(script, self.arms["mixed"], *args)
            d = json.loads(r.stdout)
            self.assertEqual(d["arm"], "mixed")
            for k, g in (("0", "a"), ("1", "b")):
                ref = json.loads(golden(gold % g)); ref["arm"] = "mixed"; ref.update(pci=None, counts=cnt(g))
                self.assertEqual(d["cells"][k], ref, (script, k))

    def test_gate_exit_is_worst_cell(self):
        # cell 0 passes the legacy gate (CRC >= 98 is not met by the fixture, so lower the bar by env of the scorer's own gate keys)
        env = dict(os.environ, GATE_MODE="legacy", GATE_CRC_MIN="91.0", GATE_DROP_MAX="2.5")
        r = subprocess.run([sys.executable, os.path.join(PR, "dgx", "score_rx.py"), "--gate", "mixed"], capture_output=True, text=True,
                           cwd=self.tmp, env=env)
        lines = r.stdout.splitlines()
        self.assertEqual(lines[0], "== cell 0 (PCI ?)")
        self.assertTrue(lines[1].startswith("  cell pci=None ") and lines[2][:4] in ("FAIL", "PASS"))
        self.assertEqual(r.returncode, 0 if all(l.startswith("PASS") for l in lines if not l.startswith(("==", "  cell pci="))) else 1)

    def test_score_rx_unprefixed_byte_identical(self):
        for script, args, gold in (("score_rx.py", ("--json",), "golden_score_rx_%s.json"), ("score_rx.py", ("--gate",), "golden_score_rx_gate_%s.txt"),
                                   ("score_rx_solver.py", (), "golden_score_rx_solver_%s.json"),
                                   ("score_rx_solver.py", ("--gate",), "golden_score_rx_solver_gate_%s.txt")):
            for g in "ab":
                r = self._arm_run(script, self.arms["arm_" + g], *args)
                self.assertEqual(r.stdout, golden(gold % g), (script, args, g))

    def test_one_cell_only_log_needs_no_cell_flag(self):
        # a multi-cell run in which only cell 1 logged so far: tags are stripped, nothing to mix
        r = self._arm_run("score_rx.py", self.arms["one"], "--json")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads(r.stdout)["crc_pct"], json.loads(golden("golden_score_rx_b.json"))["crc_pct"])

    def test_mpci_runsum(self):
        rs = os.path.join(PR, "mpci", "mpci_runsum.py")
        for g in "ab":
            r = run(rs, "mpci/fixtures/cell_%s.log" % g, cwd=PR)
            self.assertEqual(r.stdout, golden("golden_runsum_%s.txt" % g))
        r = run(rs, self.mixed)
        self.assertEqual(r.returncode, 0, r.stderr)
        want = self.mixed + "\n" + "".join("== cell %d (PCI ?)\n%s  cell pci=None %s\n" % (k, golden("golden_runsum_%s.txt" % g).split("\n", 1)[1], cline(g)) for k, g in ((0, "a"), (1, "b")))
        self.assertEqual(r.stdout, want)


class Payload(unittest.TestCase):
    """The HTTP payload builder with a multi-cell run directory."""

    def test_state_payload_multicell(self):
        import monitor
        d = tempfile.mkdtemp()
        with open(os.path.join(d, "run.log"), "w") as f:
            f.writelines(mixed_lines())
        mgr = monitor.Manager(_args(d))
        run_ = mgr.resolve("primary")
        run_.advance()
        p = monitor.state_payload(mgr, run_, {})
        self.assertTrue(p["meta"]["attributed"])
        self.assertEqual(p["meta"]["cells"], ["421", "508"])
        self.assertEqual(p["meta"]["cell_idx"], {"421": 0, "508": 1})
        p1 = monitor.state_payload(mgr, run_, {"cell": "508"})
        self.assertEqual(p1["meta"]["cell"], "508")
        self.assertEqual(p1["snap"]["cell_idx"], 1)
        self.assertEqual(p1["dl_totals"]["grants"], 180)       # cell_b.log: 180 EVMGRANT lines
        self.assertEqual(p["dl_totals"]["grants"], 120)        # cell_a.log
        self.assertEqual(p1["snap"]["radio"]["rfstall_n"], 1)


def _args(d):
    import argparse
    ns = argparse.Namespace()
    for k, v in dict(run=d, follow=None, log=None, metrics=None, obs=None, records=None, cfr=None, runs_root=None, scan=None,
                     offline=True, port=0, host="127.0.0.1", label=None, flags=None, replay=None).items():
        setattr(ns, k, v)
    return ns


if __name__ == "__main__":
    unittest.main()
