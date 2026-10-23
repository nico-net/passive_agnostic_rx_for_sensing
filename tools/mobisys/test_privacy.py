"""Adversarial privacy tests: the commercial exporter is run end to end (subprocess, real pipe) on input full of
C-RNTIs and payload markers; every file it leaves, plus stderr/stdout and exception paths, must be marker-free."""
import json
import os
import re
import subprocess
import sys
import tempfile
import shutil
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_synthetic  # noqa: E402

EXPORT = os.path.join(HERE, "export.py")
TEXT = [r"(?<![0-9.])(4601|17921|4602|17922)(?![0-9.])", r"0x4601", r"0x4602", "DEADBEEF", "PAYLOADMARKER", "payload=", "rnti"]
BIN = [b"0x4601", b"17921", b"DEADBEEF", b"PAYLOADMARKER", b"payload="]


def rd(p, mode="rb"):
    with open(p, mode) as f:
        return f.read()


def hostile_input(n=120):
    out = ["Starting sync detection", "Initial sync successful, PCI: 101"]
    for i in range(n):
        out.append(make_synthetic._evm(None, 0x4601, i % 1024, i % 20, i % 3 != 0, 12, 2, (5, 20), i % 16, 3000, 15.0,
                                       [100, 50, 60, 300]) + " payload=0xDEADBEEF PAYLOADMARKER_FIELD=PAYLOADMARKER")
        out.append("SENSING: blind PDCCH rnti_seen utc_ns=%d rnti=0x4601 sfn=%d slot=%d fmt=1_1 cce=0 al=4 payload=0xDEADBEEF"
                   % (10 ** 18 + i, i % 1024, i % 20))
        out.append("SENSING: UL_PUSCH_CRC rnti=0x4602 ok=1 mcs=9 prb=3+20 ta=0 t=100 cpu_us=5 status=0 reason=- uci_hint=0 uci_re=0 "
                   "dmrs_id=1 data_id=1 snr_db=11.5 delay=0 seg=1/1 tbs=100 cdm=2 tb=DEADBEEF probe=0")
        out.append("SENSING: DCIRAW rnti=0x4601 payload=0xDEADBEEF PAYLOADMARKER")
    out.append("SENSING: ISAC_METRICS " + json.dumps({"schema": 1, "t_mono_ns": 5, "pci": 101, "acq_state": "PAYLOADMARKER",
                                                      "recov_x": 1, "PAYLOADMARKER": 7, "recov_PAYLOADMARKER": "0x4601"}))
    return ("\n".join(out) + "\n").encode()


def scan(testcase, outdir, extra_streams=()):
    n = 0
    for root, _, files in os.walk(outdir):
        for fn in files:
            n += 1
            raw = rd(os.path.join(root, fn))
            for b in BIN:
                testcase.assertNotIn(b, raw, (fn, b))
            if not fn.endswith(".parquet"):
                txt = raw.decode("utf-8", "replace")
                for rx in TEXT:
                    testcase.assertIsNone(re.search(rx, txt), (fn, rx))
    for s in extra_streams:
        for b in BIN:
            testcase.assertNotIn(b, s)
    return n


class PrivacyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.sentinel = os.path.join(self.tmp, "tmpdir")
        os.mkdir(self.sentinel)
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.env = dict(os.environ, TMPDIR=self.sentinel)

    def run_export(self, out, stdin_bytes, extra=(), code=None):
        cmd = [sys.executable, code or EXPORT, "--experiment-mode", "commercial", "--out", out, "--no-sampler"] + list(extra)
        return subprocess.run(cmd, input=stdin_bytes, capture_output=True, env=self.env)

    def test_end_to_end_all_files_clean(self):
        for fmt in ("jsonl", "auto"):
            out = os.path.join(self.tmp, "o_" + fmt)
            p = self.run_export(out, hostile_input(), ["--format", fmt, "--rotate-rows", "3", "--flush-s", "0.2"])
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertGreater(scan(self, out, (p.stdout, p.stderr)), 4)  # part/rotated files included
            names = os.listdir(out)
            self.assertNotIn("rx_grants", names)
            self.assertNotIn("dci_attempts", names)
            summ = json.loads(rd(os.path.join(out, "summary.json")))
            self.assertEqual(summ["mode"], "commercial")
            self.assertTrue(os.path.isdir(os.path.join(out, "cell_aggregates")))
        self.assertEqual(os.listdir(self.sentinel), [])  # no temp file outside the output dir

    def test_metric_values_are_not_persisted_as_strings(self):
        out = os.path.join(self.tmp, "o")
        self.run_export(out, hostile_input(), ["--format", "jsonl"])
        for r in (json.loads(x) for fn in os.listdir(os.path.join(out, "system_telemetry"))
                  for x in rd(os.path.join(out, "system_telemetry", fn), "r").splitlines()):
            self.assertEqual(r.get("acq_state"), None if r.get("src") != "metrics" else r["acq_state"])
            self.assertTrue(all(not isinstance(v, str) or v in ("metrics", "host", "UNKNOWN") for v in r.values()), r)

    def test_regular_file_stdin_refused(self):
        f = os.path.join(self.tmp, "raw.log")
        with open(f, "wb") as fh:
            fh.write(hostile_input())
        out = os.path.join(self.tmp, "o")
        with open(f, "rb") as fh:
            p = subprocess.run([sys.executable, EXPORT, "--experiment-mode", "commercial", "--out", out], stdin=fh, capture_output=True)
        self.assertEqual(p.returncode, 2)
        self.assertIn(b"refused", p.stderr)
        self.assertFalse(os.path.exists(out))
        p = subprocess.run([sys.executable, EXPORT, "--out", out, "--input", f], capture_output=True)  # commercial is the default
        self.assertEqual(p.returncode, 2)
        self.assertNotIn(b"DEADBEEF", p.stderr)

    def _patched(self, body):
        code = os.path.join(self.tmp, "run.py")
        with open(code, "w") as f:
            f.write("import sys\nsys.path.insert(0, %r)\nimport export, agg, store\n%s\nexport.cli()\n" % (HERE, body))
        return code

    def test_forced_exception_paths_do_not_echo_input(self):
        out = os.path.join(self.tmp, "o")
        # (a) per-line failure whose message carries the whole line
        code = self._patched("def boom(self, d, t):\n    raise ValueError('PAYLOADMARKER payload=0xDEADBEEF %r' % (d,))\n"
                             "agg.Aggregator._on_evmgrant = boom")
        p = self.run_export(out, hostile_input(), code=code)
        self.assertEqual(p.returncode, 0)
        self.assertGreater(json.loads(rd(os.path.join(out, "summary.json")))["telemetry_loss"]["parse_errors"], 0)
        scan(self, out, (p.stdout, p.stderr))
        # (b) fatal failure at shutdown
        out2 = os.path.join(self.tmp, "o2")
        code = self._patched("def boom(self):\n    raise RuntimeError('PAYLOADMARKER payload=0xDEADBEEF 0x4601')\n"
                             "agg.Aggregator.close = boom")
        p = self.run_export(out2, hostile_input(), code=code)
        self.assertEqual(p.returncode, 4)
        self.assertIn(b"RuntimeError", p.stderr)
        scan(self, out2, (p.stdout, p.stderr))
        self.assertNotIn(b"Traceback", p.stderr)
        # (c) writer-thread failure
        out3 = os.path.join(self.tmp, "o3")
        code = self._patched("def boom(self, *a):\n    raise OSError('PAYLOADMARKER 0x4601')\nstore.Sink.write = boom")
        p = self.run_export(out3, hostile_input(), code=code)
        scan(self, out3, (p.stdout, p.stderr))

    def test_unknown_field_fails_closed_end_to_end(self):
        out = os.path.join(self.tmp, "o")
        code = self._patched("orig = agg.Aggregator.feed_line\n"
                             "def rogue(self, line, now, t):\n"
                             "    orig(self, line, now, t)\n"
                             "    if self.c['lines'] == 3:\n"
                             "        self.emit('cell_aggregates', {'rnti': 17921, 'n_grants': 99})\n"
                             "        self.emit('rx_grants', {'seq': 1})\n"
                             "agg.Aggregator.feed_line = rogue")
        p = self.run_export(out, hostile_input(), ["--format", "jsonl"], code=code)
        self.assertEqual(p.returncode, 3)
        loss = json.loads(rd(os.path.join(out, "summary.json")))["telemetry_loss"]
        self.assertEqual(loss["policy_refused_rows"], 2)
        self.assertGreaterEqual(loss["policy_refused_fields"], 2)
        scan(self, out, (p.stdout, p.stderr))
        self.assertFalse(os.path.exists(os.path.join(out, "rx_grants")))


if __name__ == "__main__":
    unittest.main()
