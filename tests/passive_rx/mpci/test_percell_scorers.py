#!/usr/bin/env python3
"""Per-cell scoring of the real multi-cell OTA logs (fixtures/ota_20261027, task C-DASH).
P1 (PCI 415 c0 + 923 c1) and Q1 (415 c0 + 403 c1) carry "[c<k>] " after an ANSI colour prefix; S1 and TN1 are N=1 logs without a tag.
Expected counts are the hand counts: zcat | sed 's/\\x1b\\[[0-9;]*m//g' | grep -F '[c<k>] ' | grep ... on the excerpt, [OFFLINE VERIFIED, cloud x86].
N=1 outputs are compared with golden/ (made by the pre-C-DASH scripts, path-independent: run from the fixture directory)."""
import gzip, json, os, re, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
PR = os.path.dirname(HERE)
FIX = os.path.join(HERE, "fixtures", "ota_20261027")
sys.path.insert(0, PR)
import cellsel  # noqa: E402

KEYS = ("crnti_grants", "crnti_crc_ok", "len_locks", "len_locks_dl", "len_locks_ul", "uldci", "si_grants", "syncs", "rxdiscont")
EXPECT = {  # name -> {cell: (PCI, counts in KEYS order, DL n_real)}
    # len_locks, syncs, rxdiscont equal the full log (the excerpt keeps all those lines; P1 c1 1964 = STOP figure, c0 0);
    # crnti_grants, crnti_crc_ok, uldci, si_grants are excerpt-only
    "mp27_P1_1_115718": {0: (415, (10, 8, 0, 0, 0, 0, 312, 148, 147), 322), 1: (923, (10061, 2028, 1964, 1944, 20, 0, 322, 145, 144), 3309)},
    "mp27_Q1_1_121424": {0: (415, (11717, 891, 1467, 1343, 124, 826, 112, 25, 24), 3992), 1: (403, (1, 0, 84, 62, 22, 0, 643, 25, 24), 644)},
}
N1 = ("mp27_S1_1_115134", "me_TN1_1_220653")
ANSI = re.compile(r"\x1b\[[0-9;]*m")


def gz(name):
    return os.path.join(FIX, name + ".run.log.gz")


def run(script, *args, cwd=None):
    r = subprocess.run([sys.executable, os.path.join(PR, script), *args], capture_output=True, text=True, cwd=cwd)
    assert r.returncode in (0, 1), r.stderr
    return r.stdout


def hand_counts(name, k):
    """Independent of cellsel: plain substring tests on the ANSI-stripped lines of one cell."""
    c = dict.fromkeys(KEYS, 0)
    with gzip.open(gz(name), "rt", errors="replace") as f:
        for l in f:
            l = ANSI.sub("", l)
            if "[c%d] " % k not in l:
                continue
            if "EVMGRANT rnti=0xffff " in l:
                c["si_grants"] += 1
            elif "EVMGRANT rnti=" in l:
                c["crnti_grants"] += 1
                c["crnti_crc_ok"] += " crc=1 " in l
            c["len_locks"] += "length locked" in l
            c["len_locks_dl"] += "DCI 1_1 length locked" in l
            c["len_locks_ul"] += "UL automatic DCI length locked" in l
            c["uldci"] += "SENSING: ULDCI rnti=" in l
            c["syncs"] += "Got synch" in l
            c["rxdiscont"] += "RXDISCONT abs_slot=" in l
    return c


class Tag(unittest.TestCase):
    def test_tag_after_ansi_and_timestamp(self):
        self.assertEqual(cellsel.split("\x1b[0m[c1] \x1b[32m[PHY]    SENSING: x\n"), (1, "[PHY]    SENSING: x\n"))
        self.assertEqual(cellsel.split("[c0] [PHY] y\n"), (0, "[PHY] y\n"))
        self.assertEqual(cellsel.split("12.5 \x1b[0m[c2] a\n"), (2, "12.5 a\n"))
        self.assertEqual(cellsel.split("[0m[c3] a\n"), (3, "a\n"))                  # ESC lost in the capture
        self.assertEqual(cellsel.split("\x1b[0m[PHY]    SENSING: [c1] not a prefix\n")[0], None)

    def test_gz_read_by_magic_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "renamed.log")
            shutil.copy(gz(N1[0]), p)
            self.assertEqual(cellsel.read_lines(p), cellsel.read_lines(gz(N1[0])))
            self.assertEqual(cellsel.read_lines(p, text=False), [l for l in gzip.open(p, "rt", errors="replace")])
        self.assertEqual(len(cellsel.read_lines(gz(N1[0]))), 62930)

    def test_cells_and_pci(self):
        for name, want in EXPECT.items():
            info = cellsel.cell_info(gz(name))
            self.assertEqual(info["cells"], [0, 1])
            self.assertEqual(info["pci"], {k: v[0] for k, v in want.items()}, name)
            for k, (pci, cnt, _) in want.items():
                self.assertEqual(tuple(info["counts"][k][x] for x in KEYS), cnt, (name, k))
                self.assertEqual(info["counts"][k], hand_counts(name, k), (name, k))
            self.assertEqual(sum(info["untagged"].values()), 0, "no scored line is untagged in these logs")
        for n in N1:
            self.assertIsNone(cellsel.cell_info(gz(n)))


class PerCell(unittest.TestCase):
    def test_goal_metrics(self):
        for name, want in EXPECT.items():
            d = json.loads(run("goal_metrics.py", "--json", gz(name)))
            self.assertEqual(sorted(d["cells"]), ["0", "1"])
            for k, (pci, cnt, n_real) in want.items():
                c = d["cells"][str(k)]
                self.assertEqual((c["pci"], c["dl"]["n_real"]), (pci, n_real), (name, k))
                self.assertEqual(tuple(c["counts"][x] for x in KEYS), cnt)
            txt = run("goal_metrics.py", gz(name))
            self.assertEqual(re.findall(r"^== cell (\d) \(PCI (\d+)\)$", txt, re.M), [(str(k), str(v[0])) for k, v in want.items()])
            self.assertEqual(txt.count(" CELL pci="), 2)
            self.assertEqual(txt.count(" DL "), 2)
            self.assertEqual(txt.count(" UL "), 2)
            self.assertEqual(txt.count(" UNTAGGED "), 1)
            self.assertIn("untagged", d)

    def test_runsum(self):
        for name, want in EXPECT.items():
            txt = run("mpci/mpci_runsum.py", gz(name))
            for k, (pci, cnt, _) in want.items():
                self.assertIn("== cell %d (PCI %d)\n  log: pci=%d " % (k, pci, pci), txt)
                self.assertIn("  cell pci=%d %s\n" % (pci, " ".join("%s=%d" % kv for kv in zip(KEYS, cnt))), txt)

    def test_arm_scorers(self):
        for name, want in EXPECT.items():
            with tempfile.TemporaryDirectory() as d:
                os.makedirs(os.path.join(d, name, "rx"))
                shutil.copy(gz(name), os.path.join(d, name, "rx", "rx.log"))      # gzip content, plain name: magic bytes
                for script in ("dgx/score_rx.py", "dgx/score_rx_solver.py"):
                    j = json.loads(run(script, *(("--json",) if "solver" not in script else ()), name, cwd=d))
                    for k, (pci, cnt, _) in want.items():
                        c = j["cells"][str(k)]
                        self.assertEqual((c["pci"], tuple(c["counts"][x] for x in KEYS)), (pci, cnt), (script, name, k))
                    g = run(script, "--gate", name, cwd=d)
                    self.assertEqual(re.findall(r"^== cell (\d) \(PCI (\d+)\)$", g, re.M), [(str(k), str(v[0])) for k, v in want.items()])
                    self.assertEqual(g.count("\n  cell pci="), 2)


SYNTH = os.path.join(HERE, "fixtures", "synth_2cell_ul.txt")
# per cell: (PCI, UL n_real, UL CRC ok, UL n_q, UL drops %, probe=1 lines that must NOT count)
SYNTH_EXPECT = {0: (415, 6, 4, 4, 3.0, 3), 1: (923, 9, 2, 2, 6.0, 4)}


class SyntheticUL(unittest.TestCase):
    """Per-cell UL scoring on fixtures/synth_2cell_ul.txt. P1/Q1 hold no UL line and S1's UL lines are all probe=1, so no real log exercises
    per-cell UL. The fixture is built from REAL UL_E2E / UL_PUSCH_CRC / PUSCHQ2 line formats of me_TN1_1_220653 (probe=0 lines; probe=1 lines
    kept as probes), prefixed "\\x1b[0m[c<k>] \\x1b[32m", with MPCI_CELL lines for PCI 415 (c0) and 923 (c1). SYNTHETIC edits: ok=1 and snr_db=30.0 on some
    probe=0 CRC lines (every real probe=0 line is ok=0), ok=1 on every probe=1 CRC line (a probe counted by mistake shows at once), PUSCHQ2
    real[queued full] / real_stale set to 100/3/0 (c0) and 200/10/2 (c1). Different counts per cell so a mix-up cannot pass."""

    def hand(self, k, probe):
        """Independent of the scorers: substring counts on the tagged lines of cell k -> (CRC lines, CRC ok, E2E lines)."""
        crc = ok = e2e = 0
        with open(SYNTH) as f:
            for l in f:
                l = ANSI.sub("", l)
                if "[c%d] " % k not in l or (" probe=%d" % probe) not in l:
                    continue
                if "UL_PUSCH_CRC" in l:
                    crc += 1
                    ok += " ok=1 " in l
                elif "UL_E2E" in l:
                    e2e += 1
        return crc, ok, e2e

    def test_fixture_shape(self):
        for k, (pci, n, ok, nq, drops, probes) in SYNTH_EXPECT.items():
            self.assertEqual(self.hand(k, 0), (n, ok, n))
            self.assertEqual(self.hand(k, 1), (probes, probes, probes))     # every probe CRC line carries ok=1

    def test_goal_metrics_ul_per_cell(self):
        d = json.loads(run("goal_metrics.py", "--json", SYNTH))
        self.assertEqual(sorted(d["cells"]), ["0", "1"])
        for k, (pci, n, ok, nq, drops, probes) in SYNTH_EXPECT.items():
            c = d["cells"][str(k)]
            ul = c["ul"]
            self.assertEqual(c["pci"], pci)
            self.assertEqual((ul["n_real"], ul["n_q"], ul["drops_pct"]), (n, nq, drops), k)
            self.assertAlmostEqual(ul["crc_all"], 100.0 * ok / n)           # probe=1 lines (all ok=1) are excluded
            self.assertEqual(ul["crc_q"], 100.0)
            self.assertEqual(ul["unqualified_low_snr"], n - nq)
            self.assertEqual(ul["skipped"], 0)
            self.assertIsNotNone(ul["tot_p99"])
            self.assertEqual(c["dl"]["n_real"], 0)
            self.assertEqual(sum(c["counts"].values()), 0)                 # UL CRC / E2E lines are not C-RNTI / SI facts
        self.assertEqual(sum(d["untagged"].values()), 0)
        # the two cells' UL latency sets differ (no mixing)
        self.assertNotEqual(d["cells"]["0"]["ul"]["tot_p50"], d["cells"]["1"]["ul"]["tot_p50"])
        txt = run("goal_metrics.py", SYNTH)
        self.assertEqual(re.findall(r"^== cell (\d) \(PCI (\d+)\)$", txt, re.M), [("0", "415"), ("1", "923")])
        self.assertEqual(len(re.findall(r" UL \S+ n_real=6 ", txt)), 1)
        self.assertEqual(len(re.findall(r" UL \S+ n_real=9 ", txt)), 1)

    def test_other_scorers_split_cells(self):
        txt = run("mpci/mpci_runsum.py", SYNTH)
        for k, v in SYNTH_EXPECT.items():
            self.assertIn("== cell %d (PCI %d)\n" % (k, v[0]), txt)
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "synth", "rx"))
            shutil.copy(SYNTH, os.path.join(d, "synth", "rx", "rx.log"))
            for script, args in (("dgx/score_rx.py", ("--json",)), ("dgx/score_rx_solver.py", ())):
                j = json.loads(run(script, *args, "synth", cwd=d))
                self.assertEqual(sorted(j["cells"]), ["0", "1"])
                for k, v in SYNTH_EXPECT.items():
                    self.assertEqual(j["cells"][str(k)]["pci"], v[0])
                    self.assertEqual(sum(j["cells"][str(k)]["counts"].values()), 0)


class UnnamedCells(unittest.TestCase):
    """A multi-cell log without MPCI_CELL / ISAC_METRICS: extras are still printed, PCI shown as '?' / null."""

    def test_unknown_pci_marker(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "u.log")
            with open(SYNTH) as f, open(p, "w") as o:
                o.writelines(l for l in f if not l.startswith("MPCI_CELL"))
            txt = run("goal_metrics.py", p)
            self.assertEqual(re.findall(r"^== cell (\d) \(PCI \?\)$", txt, re.M), ["0", "1"])
            self.assertEqual(txt.count(" CELL pci=None "), 2)
            self.assertEqual(txt.count(" UNTAGGED "), 1)
            j = json.loads(run("goal_metrics.py", "--json", p))
            self.assertEqual([j["cells"][k]["pci"] for k in "01"], [None, None])
            self.assertIn("untagged", j)
            self.assertIn("== cell 0 (PCI ?)\n", run("mpci/mpci_runsum.py", p))


class N1Identical(unittest.TestCase):
    """The N=1 logs (no tag) score exactly as before the change: golden/ comes from the pre-change scripts."""

    def golden(self, f):
        with open(os.path.join(FIX, "golden", f)) as fh:
            return fh.read()

    def test_goal_metrics_runsum(self):
        for n in N1:
            f = n + ".run.log.gz"
            self.assertEqual(run("goal_metrics.py", f, cwd=FIX), self.golden(n + ".goal_metrics.txt"))
            self.assertEqual(run("goal_metrics.py", "--json", f, cwd=FIX), self.golden(n + ".goal_metrics.json"))
            self.assertEqual(run("mpci/mpci_runsum.py", f, cwd=FIX), self.golden(n + ".runsum.txt"))

    def test_arm_scorers(self):
        for n in N1:
            with tempfile.TemporaryDirectory() as d:
                os.makedirs(os.path.join(d, n, "rx"))
                shutil.copy(gz(n), os.path.join(d, n, "rx", "rx.log"))
                for script, args, g in (("dgx/score_rx.py", ("--json",), "score_rx.json"), ("dgx/score_rx.py", ("--gate",), "score_rx_gate.txt"),
                                        ("dgx/score_rx_solver.py", (), "score_rx_solver.json"), ("dgx/score_rx_solver.py", ("--gate",), "score_rx_solver_gate.txt")):
                    self.assertEqual(run(script, *args, n, cwd=d), self.golden(n + "." + g), (script, args, n))


if __name__ == "__main__":
    unittest.main()
