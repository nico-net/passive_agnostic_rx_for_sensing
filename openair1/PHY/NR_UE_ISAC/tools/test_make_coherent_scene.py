#!/usr/bin/env python3
"""Self-test for make_coherent_scene.py: record structure, DL-only TDD cadence, irregular allocations,
and physics (per-channel LOS-referenced range profile peaks at the target's excess delay)."""
import json, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
HERE = Path(__file__).resolve().parent
C = 299792458.0

def records(path):
    b = open(path, "rb").read(); i = 0; out = []
    while i < len(b):
        assert b[i:i + 4] == b"CFR1"
        kind = struct.unpack_from("<I", b, i + 4)[0]; ant, re_ = struct.unpack_from("<II", b, i + 40)
        n = 68 + (0 if kind else 8 * ant * re_ + 8 * re_)
        hdr = b[i:i + 68]; pay = b[i + 68:i + n]; out.append((kind, hdr, pay, ant, re_)); i += n
    return out

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    subprocess.check_call([sys.executable, str(HERE / "make_coherent_scene.py"), "--out", str(d / "rows.bin"),
                           "--truth", str(d / "truth.json"), "--survey-out", str(d / "survey.json"), "--seconds", "0.5"])
    tr = json.load(open(d / "truth.json")); sv = json.load(open(d / "survey.json"))
    assert "resid_delay_s" in tr and len(tr["resid_delay_s"]) == 4, tr.get("resid_delay_s")
    rec = [r for r in records(d / "rows.bin") if r[0] == 0]
    assert len(rec) > 100, len(rec)
    widths = {r[4] for r in rec}; assert len(widths) > 5, "allocations must vary per row"
    assert all(r[3] == 4 for r in rec)
    g = np.array(tr["geometry"]["gnb"]); rx = [np.array(p) for p in tr["geometry"]["rx"]]
    rx_s = [np.array(sv["rx_antennas_m"][f"ch{i}"]) for i in range(4)]
    err = max(np.linalg.norm(a - b) for a, b in zip(rx, rx_s)); assert 0.01 < err < 0.4, err
    # physics on the widest row: LOS-referenced profile of channel 0 peaks near 0 excess delay
    kind, hdr, pay, ant, re_ = max(rec, key=lambda r: r[4])
    iq = np.frombuffer(pay[:8 * ant * re_], np.float32).reshape(ant, 2 * re_)
    h = iq[:, 0::2] + 1j * iq[:, 1::2]
    k = np.frombuffer(pay[8 * ant * re_:8 * ant * re_ + 4 * re_], np.uint32).astype(float)
    f = (k - 273 * 6) * 30000.0
    d_los = np.linalg.norm(rx[0] - g) / C
    taus = np.arange(-50, 400) * 1e-9
    prof = np.abs(np.exp(2j * np.pi * np.outer(taus, f)) @ h[0])
    assert abs(taus[np.argmax(prof)] - d_los) < 20e-9, (taus[np.argmax(prof)], d_los)
print("test_make_coherent_scene: PASS")
