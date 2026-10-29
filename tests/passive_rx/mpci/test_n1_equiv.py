#!/usr/bin/env python3
"""Tests of n1_equiv.py (A9) on fixture logs: excerpts of the real N=1 OTA log fixtures/ota_20261027/me_TN1_1_220653 (main 57df6abb85, TIM 923,
direct radio path: n1/main_n1_ota_excerpt.txt) and of the CORE_PLAN line of /home/nicola/NICOLA/ota_runs/replay_ref_123108 (n1/core_plan_excerpt.txt).
Candidate variants are made by editing those real lines (synthetic, labelled in each test). [OFFLINE VERIFIED] class only."""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import n1_equiv as N  # noqa: E402

FX = os.path.join(HERE, "fixtures", "n1")
BASE = open(os.path.join(FX, "main_n1_ota_excerpt.txt"), errors="replace").read()
PLAN = open(os.path.join(FX, "core_plan_excerpt.txt")).read().splitlines()[0]
LAST_PDSCHQ = [l for l in BASE.splitlines() if "PDSCHQ queued=" in l][-1]


def mk(text, rss=None, gate=None):
    d = tempfile.mkdtemp(prefix="n1eq_")
    open(os.path.join(d, "run.log"), "w").write(text)
    if rss is not None:
        open(os.path.join(d, "time.txt"), "w").write("\tMaximum resident set size (kbytes): %d\n" % int(rss * 1024))
    if gate is not None:
        open(os.path.join(d, "gate_rc"), "w").write(str(gate))
    return d


def variant(text, k=0):
    """run-to-run noise: SYNC_SCAN ms and a few counters differ, structure identical."""
    t = text.replace("SYNC_SCAN ms=361.6", "SYNC_SCAN ms=%.1f" % (361.6 + 3 * k))
    return t + "\n" + PLAN + "\n"


def pdschq(text, decoded, ok, queued=10000, full=100):
    new = re.sub(r"queued=\d+ decoded=\d+ crc_ok=\d+", "queued=%d decoded=%d crc_ok=%d" % (queued, decoded, ok), LAST_PDSCHQ)
    new = re.sub(r"dropped\[full=\d+", "dropped[full=%d" % full, new)
    return text.replace(LAST_PDSCHQ, new)


def arm(texts, **kw):
    return [N.load_run(mk(t, **kw)) for t in texts]


class Norm(unittest.TestCase):
    def test_numbers_hex_timestamp_tag(self):
        a = N.norm("\x1b[0m123.456 [c0] \x1b[0m[PHY]    SENSING: EVMGRANT rnti=0x6613 1001.4 crc=0 ndmrs=1")
        b = N.norm("9.1 [PHY]    SENSING: EVMGRANT rnti=0x00ab 77.2 crc=1 ndmrs=3")
        self.assertEqual(a, b)
        self.assertEqual(N.norm("SENSING: CORE_POOL: 6 cores: 6 7 8 9 3 4"), "SENSING: CORE_POOL: N cores: N+")

    def test_parse_fixture(self):
        p = N.parse_log(variant(BASE).splitlines())
        self.assertEqual(p["pool_cores"], [3, 4, 6, 7, 8, 9])
        self.assertEqual(p["sync_scan_ms"], [361.6])
        self.assertEqual(len(p["core_plan"]), 1)
        self.assertIn("host=H", p["core_plan"][0])
        self.assertTrue(p["stats_keys"][:2] == ["n", "yields"])
        self.assertIn("passivePdsch0", p["lease_names"])
        self.assertGreater(p["dmrs_scan_lines"], 0)
        self.assertFalse(any("CORE_POOL" in t or "CORE_PLAN" in t for t in p["templates"]))


class Compare(unittest.TestCase):
    def setUp(self):
        self.base = pdschq(BASE, 5000, 4900)
        self.ref = arm([variant(self.base, 0), variant(pdschq(BASE, 5100, 5000, full=110), 1)], rss=1000)

    def cmp(self, cand, **kw):
        return N.compare(self.ref, cand, **kw)

    def test_identical_is_equivalent(self):
        r = self.cmp(arm([variant(self.base, 2), variant(pdschq(BASE, 5050, 4950), 0)], rss=1002))
        self.assertEqual(r["verdict"], "EQUIVALENT", N.render(r))
        self.assertEqual(r["exit"], 0)
        self.assertEqual(r["structural"], [])
        names = [x[0] for x in r["rows"]]
        self.assertIn("sync_scan_ms (A10, MEASURED)", names)       # measured, not hidden

    def test_whitelisted_a10_template_and_audit_rss(self):
        extra = "\n[NR_PHY] SENSING: MPCI_SYNC cell=0 want=923 got=923 locked\n"
        r = self.cmp(arm([variant(self.base, 0) + extra, variant(self.base, 1) + extra], rss=1007))   # +7 MB <= 8 MB allowance
        self.assertEqual(r["verdict"], "EQUIVALENT", N.render(r))
        self.assertTrue(any(s[0] == "WHITELISTED[A10-pin-log]" for s in r["structural"]))
        self.assertIn("A10-pin-log", r["whitelist_used"])
        self.assertIn("AUDIT-8MB-rss", r["whitelist_used"])

    def test_rss_beyond_allowance_is_drift(self):
        r = self.cmp(arm([variant(self.base, 0)] * 2, rss=1030))
        self.assertEqual(r["exit"], 1)
        self.assertTrue(any(x[0] == "peak_rss_mb" and x[4] == "DRIFT" for x in r["rows"]))

    def test_unknown_template_in_every_cand_run_is_drift(self):
        extra = "\n[PHY] SENSING: NEW_THING_APPEARED state=3\n"
        r = self.cmp(arm([variant(self.base, 0) + extra, variant(self.base, 1) + extra], rss=1000))
        self.assertEqual(r["exit"], 1)
        self.assertTrue(any(s[0] == "DRIFT" and "NEW_THING_APPEARED" in s[1] for s in r["structural"]))

    def test_template_in_one_cand_run_only_is_sporadic_not_drift(self):
        extra = "\n[PHY] SENSING: RARE_EVENT x=3\n"
        r = self.cmp(arm([variant(self.base, 0) + extra, variant(self.base, 1)], rss=1000))
        self.assertEqual(r["verdict"], "EQUIVALENT", N.render(r))

    def test_template_missing_in_cand_is_drift(self):
        cut = "\n".join(l for l in self.base.splitlines() if "MIB fields" not in l and "SYNC_SCAN" not in l)
        r = self.cmp(arm([variant(cut, 0)] * 2, rss=1000))
        self.assertTrue(any("only in ref" in s[1] for s in r["structural"]), N.render(r))
        self.assertEqual(r["exit"], 1)

    def test_pool_set_drift(self):
        t = self.base.replace("CORE_POOL: 6 cores: 6 7 8 9 3 4", "CORE_POOL: 5 cores: 6 7 8 9 3")
        r = self.cmp(arm([variant(t, 0), variant(t, 1)], rss=1000))
        self.assertTrue(any("pool set" in s[1] for s in r["structural"]))
        self.assertEqual(r["exit"], 1)

    def test_core_plan_drift(self):
        t = lambda k: self.base + "\n" + PLAN.replace("dl[0]:8", "dl[0]:10") + "\n"
        r = self.cmp(arm([t(0), t(1)], rss=1000))
        self.assertTrue(any("CORE_PLAN" in s[1] for s in r["structural"]))
        self.assertEqual(r["exit"], 1)

    def test_pool_stats_grammar_drift_and_b6_whitelist(self):
        # synthetic edit of the real statistics line: rename a field (drift) / append a dmrs block (B6, whitelisted)
        drift = self.base.replace(" promo=", " promoted=")
        r = self.cmp(arm([variant(drift, 0), variant(drift, 1)], rss=1000))
        self.assertTrue(any("key list" in s[1] for s in r["structural"]), N.render(r))
        self.assertEqual(r["exit"], 1)
        b6 = re.sub(r"(SENSING: CORE_POOL n=.*)$", r"\1 dmrs[n=0 waited=0]", self.base, flags=re.M)
        r = self.cmp(arm([variant(b6, 0), variant(b6, 1)], rss=1000))
        self.assertEqual(r["verdict"], "EQUIVALENT", N.render(r))
        self.assertTrue(any(s[0] == "WHITELISTED[B6-dmrs-token]" for s in r["structural"]))

    def test_crc_worse_is_drift_better_is_not(self):
        worse = arm([variant(pdschq(BASE, 5000, 4500), 0), variant(pdschq(BASE, 5000, 4510), 1)], rss=1000)
        r = self.cmp(worse)
        self.assertTrue(any(x[0] == "crc_ok_pct" and x[4] == "DRIFT" for x in r["rows"]), N.render(r))
        self.assertEqual(r["exit"], 1)
        better = arm([variant(pdschq(BASE, 5000, 4990), 0), variant(pdschq(BASE, 5000, 4995), 1)], rss=1000)
        r = self.cmp(better)
        self.assertEqual(r["exit"], 0, N.render(r))
        self.assertTrue(any(x[0] == "crc_ok_pct" and x[4] == "better" for x in r["rows"]))

    def test_drop_full_and_decoded(self):
        r = self.cmp(arm([variant(pdschq(BASE, 5000, 4900, full=400), 0)] * 2, rss=1000))
        self.assertTrue(any(x[0] == "drop_full_pct" and x[4] == "DRIFT" for x in r["rows"]))
        r = self.cmp(arm([variant(pdschq(BASE, 3000, 2940), 0)] * 2, rss=1000))
        self.assertTrue(any(x[0] == "decoded" and x[4] == "DRIFT" for x in r["rows"]))

    def test_gate_regression(self):
        ref = arm([variant(self.base, 0), variant(self.base, 1)], rss=1000, gate=0)
        cand = arm([variant(self.base, 0), variant(self.base, 1)], rss=1000, gate=1)
        r = N.compare(ref, cand)
        self.assertTrue(any(x[0] == "gate_pass" and x[4] == "DRIFT" for x in r["rows"]))

    def test_sync_scan_measured_warn_never_drift(self):
        slow = self.base.replace("SYNC_SCAN ms=361.6", "SYNC_SCAN ms=900.0")
        r = self.cmp(arm([variant(slow, 0), variant(slow, 1)], rss=1000))
        row = [x for x in r["rows"] if x[0].startswith("sync_scan_ms")][0]
        self.assertEqual(row[4], "WARN")
        self.assertEqual(r["exit"], 0, N.render(r))

    def test_insufficient_n(self):
        r = self.cmp(arm([variant(self.base, 0)], rss=1000))
        self.assertEqual(r["exit"], 3)
        self.assertEqual(r["verdict"], "INSUFFICIENT")


class Cli(unittest.TestCase):
    def test_extract_compare_roundtrip_and_host_class_refusal(self):
        base = pdschq(BASE, 5000, 4900)
        refs = [mk(variant(base, 0), rss=1000), mk(variant(base, 1), rss=1000)]
        cands = [mk(variant(base, 2), rss=1001), mk(variant(base, 3), rss=1001)]
        j = os.path.join(tempfile.mkdtemp(prefix="n1eq_"), "ref.json")
        self.assertEqual(N.main(["extract"] + refs + ["-o", j, "--host-class", "hostA", "--label", "main"]), 0)
        out = os.path.join(os.path.dirname(j), "verdict")
        self.assertEqual(N.main(["compare", "--ref", j, "--cand"] + cands + ["--host-class", "hostA", "--out", out]), 0)
        self.assertTrue(os.path.exists(out + ".txt") and os.path.exists(out + ".json"))
        self.assertEqual(N.main(["compare", "--ref", j, "--cand"] + cands + ["--host-class", "hostB"]), 3)

    def test_gz_log_and_script_syntax(self):
        import gzip
        d = tempfile.mkdtemp(prefix="n1eq_")
        with gzip.open(os.path.join(d, "run.log"), "wt") as f:
            f.write(variant(BASE))
        self.assertEqual(N.load_run(d)["pool_cores"], [3, 4, 6, 7, 8, 9])
        self.assertEqual(subprocess.call(["bash", "-n", os.path.join(HERE, "n1_equiv.sh")]), 0)


if __name__ == "__main__":
    unittest.main()
