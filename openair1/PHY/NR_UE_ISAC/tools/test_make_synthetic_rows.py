#!/usr/bin/env python3
"""Self-test for make_synthetic_rows.py.

Independent of the generator's internals: parses cfr_rows.bin with its own struct reader (per
the Task 10 brief's record format) and checks structure, cadence/gap bookkeeping, and a real
DSP physics check (range-profile IFFT must peak at the true LOS and target delays) -- not just a
size/exists check.

Deviation from the literal `--seconds 2 --gap 0.8 1.5` suggested for this test: the generator's
gate-close only fires once `t >= gap_start + 2.0` (hardcoded in the verbatim Step 3 code), so a
0.7 s gap inside a 2 s run can never produce a close record. Using `--seconds 3.0 --gap 0.5 2.6`
instead (gap width 2.1 s) so the close event is reachable while still exercising rows before AND
after the gap. Also picks an explicit --target away from the default: at the default
[60,40,1.5]/t=0 the target's bistatic delay is <1 bin from the LOS delay for this survey's
geometry, so the two peaks are not separable even with a Hann window -- not testable as "second
peak = target". [200,150,5], v=(3,-2,0) separates by tens of bins on every antenna (verified
numerically before writing this assertion).
"""
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
GEN = HERE / "make_synthetic_rows.py"
SURVEY = HERE / "survey_synth.json"

C = 299792458.0
HEADER_FMT = "<IIfiIIQHHIIfQQ"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
SLOT_S = 0.0005

SECONDS = 3.0
GAP = (0.5, 2.6)
CABLE_NS = [0.0, 35.0, 80.0, 120.0]
TARGET = [200.0, 150.0, 5.0, 3.0, -2.0, 0.0]


def read_records(path):
    records = []
    with open(path, "rb") as f:
        while True:
            magic = f.read(4)
            if not magic:
                break
            assert magic == b"CFR1", f"bad magic {magic!r}"
            (kind, slot, frac, source, prb, scs, fc, pci, spf, ant, re, noise, session,
             t_ns) = struct.unpack(HEADER_FMT, f.read(HEADER_SIZE))
            rec = dict(kind=kind, slot=slot, frac=frac, source=source, prb=prb, scs=scs, fc=fc,
                       pci=pci, spf=spf, ant=ant, re=re, noise=noise, session=session, t_ns=t_ns)
            assert kind in (0, 1), f"bad kind {kind}"
            if kind == 0:
                h = np.frombuffer(f.read(4 * 2 * ant * re), dtype="<f4").reshape(ant, re, 2)
                k = np.frombuffer(f.read(4 * re), dtype="<u4")
                l = np.frombuffer(f.read(4 * re), dtype="<u4")
                rec["h"], rec["k"], rec["l"] = h, k, l
            records.append(rec)
    return records


def main():
    survey = json.loads(SURVEY.read_text())
    gnb = np.array(survey["gnb_m"], float)
    rx = [np.array(survey["rx_antennas_m"][f"ch{i}"], float) for i in range(4)]

    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "rows.bin"
        cmd = [sys.executable, str(GEN), "--survey", str(SURVEY), "--out", str(out),
               "--seconds", str(SECONDS), "--gap", str(GAP[0]), str(GAP[1]),
               "--cable-delay-ns", *[str(c) for c in CABLE_NS],
               "--target", *[str(t) for t in TARGET]]
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        truth = json.loads(result.stdout.strip().splitlines()[-1])

        recs = read_records(out)
        assert recs, "no records written"

        # --- kind / gate-close bookkeeping ---
        closes = [r for r in recs if r["kind"] == 1]
        assert len(closes) == 1, f"expected exactly one gate close, got {len(closes)}"
        rows = [r for r in recs if r["kind"] == 0]
        for r in rows:
            t = r["t_ns"] / 1e9
            assert not (GAP[0] <= t < GAP[1]), f"row at t={t} falls inside the gap"

        # --- per-row structure ---
        re_expect = 273 * 6
        for r in rows:
            assert r["ant"] == 4, r["ant"]
            assert r["re"] == re_expect, r["re"]
            assert np.all(r["k"] % 2 == 0), "k must be even (comb-2)"
            assert np.all(r["k"] < 273 * 12), "k out of band"
            assert np.all(r["l"] == 2), "l must be DM-RS symbol 2"

        # --- cadence: DL every slot, UL every 5th slot, both outside the gap only ---
        n_total = int(SECONDS / SLOT_S)

        def outside_gap_n(step):
            return {n for n in range(0, n_total, step) if not (GAP[0] <= n * SLOT_S < GAP[1])}

        dl_n = {round(r["t_ns"] / 1e9 / SLOT_S) for r in rows if r["source"] == 3}
        ul_n = {round(r["t_ns"] / 1e9 / SLOT_S) for r in rows if r["source"] == 4}
        assert dl_n == outside_gap_n(1), "DL rows must cover every slot outside the gap"
        assert ul_n == outside_gap_n(5), "UL rows must cover every 5th slot outside the gap"
        assert all(r["session"] == 0x4601 for r in rows if r["source"] == 4), "UL session must be the UE RNTI"
        assert all(r["session"] == 0 for r in rows if r["source"] == 3), "DL session must be 0"

        # --- physics: range profile of the first DL row peaks at the LOS delay and,
        #     second-strongest, at the target's bistatic delay -- both within one range bin ---
        dl0 = min((r for r in rows if r["source"] == 3), key=lambda r: r["t_ns"])
        t0 = dl0["t_ns"] / 1e9
        N = dl0["k"].size
        dt = 1.0 / (N * 2 * dl0["scs"])
        win = np.hanning(N)
        p0 = np.array(truth["target_start"]); v = np.array(truth["velocity"])
        pt = p0 + v * t0

        for i in range(4):
            hc = dl0["h"][i, :, 0] + 1j * dl0["h"][i, :, 1]
            prof = np.abs(np.fft.ifft(np.fft.ifftshift(hc * win)))
            order = np.argsort(prof)[::-1]
            peaks = []
            for idx in order:
                if all(abs(int(idx) - p) > 4 for p in peaks):
                    peaks.append(int(idx))
                if len(peaks) == 2:
                    break

            d_los = np.linalg.norm(rx[i] - gnb)
            d_tgt = np.linalg.norm(pt - gnb) + np.linalg.norm(rx[i] - pt)
            los_bin = (d_los / C + CABLE_NS[i] * 1e-9) / dt
            tgt_bin = (d_tgt / C + CABLE_NS[i] * 1e-9) / dt

            assert abs(peaks[0] - los_bin) <= 1.0, (
                f"ant{i}: strongest peak {peaks[0]} not within 1 bin of LOS delay bin {los_bin:.2f}")
            assert abs(peaks[1] - tgt_bin) <= 1.0, (
                f"ant{i}: 2nd peak {peaks[1]} not within 1 bin of target delay bin {tgt_bin:.2f}")

    print("test_make_synthetic_rows: PASS")


if __name__ == "__main__":
    main()
