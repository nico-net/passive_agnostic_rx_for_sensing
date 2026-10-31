"""Receiver-state report and coherence check, on synthetic records/frames written here (no C binary needed) [SYNTHETIC]."""

import cmath
import json
import math
from pathlib import Path
import struct
import tempfile
import unittest

from read_sense import HEADER, coherence_check, main, read_records, rx_state_report, session_fft_size, SOURCES

N_FFT = 2048
KS = tuple(range(100, 160, 2))


def frame_bytes(frame_id, source, abs_slot, h):
    n = len(KS)
    head = HEADER.pack(b"NRCF", 1, HEADER.size, frame_id, SOURCES.index(source), 1, 0, n, abs_slot, 0.0, 0.1, 0, 0)
    body = struct.pack(f"<{n}H", *KS) + bytes([0] * n)
    body += bytes(-(HEADER.size + 3 * n) % 4)
    body += struct.pack(f"<{2 * n}f", *[v for z in h for v in (z.real, z.imag)])
    return head + body


def rs(frame_id, abs_slot, shift_cum, shift_count, cfo=12.5, source="pdsch_data", rnti=0x1234):
    return dict(schema=2, type="rs", source=source, abs_slot=abs_slot, rnti=rnti, start_rb=0, csirs_resource=None, cfr_frame_id=frame_id,
                rx_ts=1000 * abs_slot, rx_cfo_applied_hz=cfo, rx_shift_cum_samples=shift_cum, rx_shift_count=shift_count, rx_gain_db=38,
                k_abs_convention="crb0_point_a")


SESSION = dict(schema=2, type="session", fs_hz=N_FFT * 30000, scs_khz=30)


def channel(slot_step, shift_cum, tau=3.0):
    """H(k) of a fixed 3-sample-delay channel seen through a window that moved `shift_cum` samples later, plus a per-slot common phase"""
    return [cmath.exp(-2j * math.pi * k * tau / N_FFT) * cmath.exp(2j * math.pi * k * shift_cum / N_FFT) * cmath.exp(0.3j * slot_step) for k in KS]


class RxState(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.rec, self.cfr = Path(self.tmp.name) / "r.jsonl", Path(self.tmp.name) / "f.cfr"

    def write(self, specs):
        """specs: (frame_id, abs_slot, shift_cum, shift_count, h)"""
        with open(self.rec, "w") as f:
            f.write(json.dumps(SESSION) + "\n")
            for fid, slot, cum, cnt, _ in specs:
                f.write(json.dumps(rs(fid, slot, cum, cnt)) + "\n")
        self.cfr.write_bytes(b"".join(frame_bytes(fid, "pdsch_data", slot, h) for fid, slot, _, _, h in specs))

    def test_fft_size_and_state_report(self):
        self.write([(1, 10, 0, 0, channel(0, 0)), (2, 11, 0, 0, channel(1, 0)), (3, 12, -4, 1, channel(2, -4))])
        recs = list(read_records(self.rec))
        self.assertEqual(session_fft_size(recs), N_FFT)
        rep = rx_state_report(recs)
        e = rep["per_source"]["pdsch_data"]
        self.assertEqual((e["n"], e["with_state"], e["with_rx_ts"]), (3, 3, 3))
        self.assertEqual(e["cfo_hz"], [12.5, 12.5]); self.assertEqual(e["gain_db"], [38])
        self.assertEqual(rep["window_shifts"], [dict(abs_slot_before=11, abs_slot_after=12, steps=1, shift_samples=-4)])

    def test_phase_continuous_without_shift(self):
        self.write([(i + 1, 10 + i, 0, 0, channel(i, 0)) for i in range(4)])
        co = coherence_check(self.rec, self.cfr)
        self.assertEqual(dict(co["counts"]), {"ok": 3})
        self.assertTrue(all(abs(p["residual_slope"]) < 1e-6 for p in co["pairs"]))

    def test_window_step_is_explained_by_recorded_shift(self):
        self.write([(1, 10, 0, 0, channel(0, 0)), (2, 11, 0, 0, channel(1, 0)), (3, 12, 5, 1, channel(2, 5)), (4, 13, 5, 1, channel(3, 5))])
        co = coherence_check(self.rec, self.cfr)
        self.assertEqual([p["flag"] for p in co["pairs"]], ["ok", "window_shift", "ok"])
        step = co["pairs"][1]
        self.assertAlmostEqual(step["slope"], 2 * math.pi * 5 / N_FFT, places=5)
        self.assertAlmostEqual(step["expected_slope"], step["slope"], places=5)

    def test_unrecorded_window_step_is_a_break(self):
        # the signal's window moved by 5 samples but the records still say shift_cum 0 -> the phase slope is unexplained
        self.write([(1, 10, 0, 0, channel(0, 0)), (2, 11, 0, 0, channel(1, 5))])
        co = coherence_check(self.rec, self.cfr)
        self.assertEqual([p["flag"] for p in co["pairs"]], ["break"])

    def test_records_without_state_are_reported_as_such(self):
        self.write([(1, 10, 0, 0, channel(0, 0)), (2, 11, 0, 0, channel(1, 0))])
        with open(self.rec) as f:
            lines = [json.loads(l) for l in f]
        for l in lines[1:]:
            l["rx_shift_cum_samples"] = None; l["rx_shift_count"] = None
        with open(self.rec, "w") as f:
            f.write("".join(json.dumps(l) + "\n" for l in lines))
        self.assertEqual(dict(coherence_check(self.rec, self.cfr)["counts"]), {"no_state": 1})
        self.assertEqual(rx_state_report(list(read_records(self.rec)))["per_source"]["pdsch_data"]["with_state"], 0)

    def test_cli_prints_the_report(self):
        self.write([(1, 10, 0, 0, channel(0, 0)), (2, 11, 3, 1, channel(1, 3))])
        self.assertEqual(main([str(self.rec), str(self.cfr), "--coherence"]), 0)


if __name__ == "__main__":
    unittest.main()
