"""Reader tests use a fixture emitted by the C exporter itself."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from read_sense import load, main, read_frames, scan


ROOT = Path(__file__).resolve().parents[2]
BINARY = Path(os.environ.get("NR_SENSE_API_TEST_BIN", ROOT / "cmake_targets/ran_build/build/test_nr_sense_api"))


class ReaderTest(unittest.TestCase):
    def setUp(self):
        if not BINARY.is_file():
            self.skipTest(f"build {BINARY} first")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = dict(os.environ, NR_SENSE_GOLDEN_DIR=self.temp.name)
        subprocess.run([str(BINARY), "--gtest_filter=SenseApi.GoldenReaderFixture"],
                       env=env, check=True, stdout=subprocess.DEVNULL)
        self.records = Path(self.temp.name) / "records.jsonl"
        self.cfr = Path(self.temp.name) / "frames.cfr"

    def test_join_records_and_frames(self):
        result = load(self.records, self.cfr)
        self.assertEqual(len(result["records"]), 3)
        self.assertEqual(len(result["frames"]), 1)
        self.assertEqual(result["missing_links"], [])
        rs = [record for record in result["records"] if record["type"] == "rs"]
        self.assertEqual(rs[0]["cfr_frame"]["frame_id"], 1)
        self.assertIsNone(rs[1]["cfr_frame"])
        frame = rs[0]["cfr_frame"]
        self.assertEqual(frame["iq_shape"], (2, 2))
        self.assertEqual(frame["iq"], ((1+2j, 3+4j), (5+6j, 7+8j)))
        self.assertEqual(frame["k_abs"], (120, 121))
        self.assertEqual(result["record_sources"], {"csirs": 1, "prach": 1})
        self.assertEqual(result["frame_sources"], {"csirs": 1})
        self.assertEqual(scan(self.records, self.cfr)["iq_shapes"], {(2, 2): 1})
        self.assertEqual(main([str(self.records), str(self.cfr)]), 0)

    def test_frame_id_gap_and_missing_link(self):
        data = bytearray(self.cfr.read_bytes())
        data[8] = 3
        self.cfr.write_bytes(data)
        result = load(self.records, self.cfr)
        self.assertEqual(result["frame_id_gaps"], [(1, 2)])
        self.assertEqual(result["frame_id_drops"], 2)
        self.assertEqual(result["missing_links"], [1])
        self.assertEqual(main([str(self.records), str(self.cfr)]), 1)

    def test_truncated_frame(self):
        self.cfr.write_bytes(self.cfr.read_bytes()[:-1])
        with self.assertRaisesRegex(ValueError, "truncated"):
            list(read_frames(self.cfr))

    def test_npz_optional(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy unavailable")
        output = Path(self.temp.name) / "export.npz"
        self.assertEqual(main([str(self.records), str(self.cfr), "--npz", str(output)]), 0)
        with np.load(output) as arrays:
            self.assertEqual(arrays["frame_1_iq"].shape, (2, 2))


if __name__ == "__main__":
    unittest.main()
