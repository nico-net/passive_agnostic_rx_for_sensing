"""Throughput micro-benchmark: reports exporter lines/s; asserts only a loose floor."""
import os
import subprocess
import sys
import tempfile
import time
import shutil
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import agg  # noqa: E402
import make_synthetic  # noqa: E402

FLOOR_LPS = 2000  # far below what the pure-python path reaches; guards against an accidental O(n^2) or per-line IO


def lines(n):
    mix = [make_synthetic._evm(None, 0x4601 + (i % 5), i % 1024, i % 20, i % 7 != 0, 12, 2, (5, 20), i % 16, 3000, 15.0, [100, 50, 60, 300])
           for i in range(50)]
    seen = "SENSING: blind PDCCH rnti_seen utc_ns=1 rnti=0x4601 sfn=1 slot=1 fmt=1_1 cce=0 al=4"
    noise = "[PHY]   SENSING: something unrelated that the receiver prints"
    return [(mix[i % 50] if i % 3 == 0 else seen if i % 3 == 1 else noise) for i in range(n)]


class BenchTest(unittest.TestCase):
    def test_throughput(self):
        n = 30000
        data = lines(n)
        a = agg.Aggregator(lambda d, r: None, "commercial")
        t = time.perf_counter()
        for i, ln in enumerate(data):
            a.feed_line(ln, 1e9 + i * 0.001, i * 0.001)
        core = n / (time.perf_counter() - t)
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        blob = ("\n".join(data) + "\n").encode()
        t = time.perf_counter()
        p = subprocess.run([sys.executable, os.path.join(HERE, "export.py"), "--out", d, "--no-sampler"], input=blob, capture_output=True)
        full = n / (time.perf_counter() - t)
        print("\nbench: aggregator %.0f lines/s (in-process), exporter end-to-end %.0f lines/s incl. interpreter start (%d lines)" % (core, full, n))
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertGreater(core, FLOOR_LPS)
        self.assertGreater(full, FLOOR_LPS)


if __name__ == "__main__":
    unittest.main()
