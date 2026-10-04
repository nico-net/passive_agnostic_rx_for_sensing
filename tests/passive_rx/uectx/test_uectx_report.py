import csv
import json
import tempfile
import unittest
from pathlib import Path

from uectx_report import csv_rows, load_records, render, timeline


class UeContextReportTest(unittest.TestCase):
    def test_timeline_change_table_and_csv(self):
        base = {"schema": "uectx/1", "identity_gen": 2, "rnti": 0x1234,
                "incarnation": 1, "t_mono_ns": 100}
        records = [
            {**base, "type": "ue_snapshot", "state": "ACTIVE",
             "cfg": {"DCI_LEN_DL": {"value": 47}, "SIB1_HASH": None}},
            {**base, "type": "ue_change", "t_mono_ns": 110, "abs_slot": 20,
             "param": "DCI_LEN_DL", "old": 47, "new": 53, "cause": "RELOCK",
             "evidence": None},
            {**base, "type": "ue_reconfig", "t_mono_ns": 120, "abs_slot": 20,
             "params": ["DCI_LEN_DL"], "class": "DCI_SIZE"},
        ]
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "fixture.jsonl"
            source.write_text("".join(json.dumps(r) + "\n" for r in records))
            loaded = load_records(source)
            self.assertEqual(loaded, records)
            self.assertEqual(len(timeline(loaded, 0x1234)), 1)
            self.assertEqual(len(timeline(loaded, 0x5678)), 0)
            report = render(loaded)
            self.assertIn("DCI_LEN_DL: 47 -> 53", report)
            self.assertIn("reconfig DCI_SIZE", report)
            rows = list(csv_rows(loaded))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[1]["params"], "DCI_LEN_DL")
            target = Path(tmp) / "out.csv"
            with target.open("w", newline="") as stream:
                writer = csv.DictWriter(stream, fieldnames=rows[0])
                writer.writeheader()
                writer.writerows(rows)
            with target.open() as stream:
                self.assertEqual(len(list(csv.DictReader(stream))), 2)

    def test_rejects_wrong_schema(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "bad.jsonl"
            source.write_text('{"schema":"other","type":"ue_change"}\n')
            with self.assertRaises(ValueError):
                load_records(source)


if __name__ == "__main__":
    unittest.main()
