"""Multi-layer PDSCH data-aided CFR: one frame + one rs record per layer, joined by cfr_frame_id (no C fixture needed)."""

import json
from pathlib import Path
import struct
import tempfile
import unittest

from read_sense import HEADER, load


def frame(frame_id, n_ant, ks, ls, values):
    n_re = len(ks)
    header = HEADER.pack(b"NRCF", 1, HEADER.size, frame_id, 3, n_ant, 0, n_re, 1000, 0.5, 2.0, 1, 1)
    body = struct.pack(f"<{n_re}H", *ks) + bytes(ls)
    body += bytes(-(HEADER.size + 3 * n_re) % 4)
    body += struct.pack(f"<{2 * n_ant * n_re}f", *[x for v in values for x in (v.real, v.imag)])
    return header + body


class LayerFramesTest(unittest.TestCase):
    def test_one_frame_per_layer_joined_to_records(self):
        with tempfile.TemporaryDirectory() as d:
            rec, cfr = Path(d) / "r.jsonl", Path(d) / "f.cfr"
            ks, ls = [100, 108], [3, 3]
            blob = b""
            lines = []
            for layer in range(3):
                vals = [complex(layer, a) for a in range(2) for _ in ks]  # 2 antennas x 2 REs
                blob += frame(layer + 1, 2, ks, ls, vals)
                lines.append(json.dumps({"schema": 2, "type": "rs", "source": "pdsch_data", "dir": "DL",
                                         "cfr_frame_id": layer + 1, "ports": 1, "port_mask": 1 << layer,
                                         "layer": layer, "n_layers": 3}))
            rec.write_text("\n".join(lines) + "\n")
            cfr.write_bytes(blob)
            result = load(rec, cfr)
            rs = result["records"]
            self.assertEqual(result["missing_links"], [])
            self.assertEqual([r["layer"] for r in rs], [0, 1, 2])
            for layer, r in enumerate(rs):
                self.assertEqual(r["n_layers"], 3)
                self.assertEqual(r["port_mask"], 1 << layer)
                self.assertEqual(r["cfr_frame"]["source"], "pdsch_data")
                self.assertEqual(r["cfr_frame"]["k_abs"], (100, 108))
                self.assertEqual(r["cfr_frame"]["iq"][1][0], complex(layer, 1))


if __name__ == "__main__":
    unittest.main()
