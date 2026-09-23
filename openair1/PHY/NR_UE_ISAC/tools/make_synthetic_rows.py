#!/usr/bin/env python3
# openair1/PHY/NR_UE_ISAC/tools/make_synthetic_rows.py
"""Physically consistent 4-receiver CFR rows (cfr_rows.bin) for offline tests: a direct path and one
constant-velocity point target per receiver, per-channel cable delay and phase, optional mid-run
silence with a recorded gate close. Survey geometry comes from the same survey.json the launcher uses."""
import argparse, json, struct
import numpy as np

C = 299792458.0

def header(kind, slot, frac, source, prb, scs, fc, pci, spf, ant, re, noise, session, t_ns):
    return (b"CFR1" + struct.pack("<IIfiIIQHHIIfQQ", kind, slot, frac, source, prb, scs, fc, pci, spf,
                                  ant, re, noise, session, t_ns))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--survey", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=10.0); ap.add_argument("--gap", nargs=2, type=float, default=None)
    ap.add_argument("--target", nargs=6, type=float, default=[60, 40, 1.5, 3.0, -2.0, 0.0], help="x y z vx vy vz")
    ap.add_argument("--cable-delay-ns", nargs=4, type=float, default=[0, 0, 0, 0])
    ap.add_argument("--snr-db", type=float, default=25.0); ap.add_argument("--ue-rnti", type=lambda s: int(s, 0), default=0x4601)
    a = ap.parse_args()
    s = json.load(open(a.survey)); gnb = np.array(s["gnb_m"], float)
    rx = [np.array(s["rx_antennas_m"][f"ch{i}"], float) for i in range(4)]
    prb, scs, fc, spf = 273, 30000, 3450000000, 20
    k = np.arange(0, prb * 12, 2, dtype=np.uint32)               # DM-RS comb-2, full band
    f = (k.astype(float) - prb * 6) * scs                         # baseband frequency of each RE
    rng = np.random.default_rng(7); p0 = np.array(a.target[:3]); v = np.array(a.target[3:])
    ue = gnb + np.array([30.0, -20.0, -gnb[2] + 1.5])             # static UE for the UL leg
    lam = C / fc; sigma = 10 ** (-a.snr_db / 20)
    with open(a.out, "wb") as out:
        closed = False
        for n in range(int(a.seconds / 0.0005)):
            t = n * 0.0005
            if a.gap and a.gap[0] <= t < a.gap[1]:
                if not closed and t >= a.gap[0] + 2.0:
                    out.write(header(1, 0, 0.0, -1, 0, 0, 0, 0, 0, 0, 0, 0.0, 0, int(t * 1e9))); closed = True
                continue
            closed = False
            pt = p0 + v * t
            for leg, src, session, tx, every in (("dl", 3, 0, gnb, 1), ("ul", 4, a.ue_rnti, ue, 5)):
                if n % every: continue
                h = np.empty((4, k.size), np.complex64)
                for i in range(4):
                    d_los = np.linalg.norm(rx[i] - tx); d_tgt = np.linalg.norm(pt - tx) + np.linalg.norm(rx[i] - pt)
                    tau_c = a.cable_delay_ns[i] * 1e-9
                    los = np.exp(-2j * np.pi * f * (d_los / C + tau_c))
                    tgt = 0.1 * np.exp(-2j * np.pi * f * (d_tgt / C + tau_c)) * np.exp(-2j * np.pi * d_tgt / lam)
                    h[i] = (los + tgt + sigma * (rng.standard_normal(k.size) + 1j * rng.standard_normal(k.size)) / np.sqrt(2)) \
                           * np.exp(1j * 0.7 * i)
                inter = np.empty((4, 2 * k.size), np.float32); inter[:, 0::2] = h.real; inter[:, 1::2] = h.imag
                out.write(header(0, n % (1024 * spf), 0.0, src, prb, scs, fc, 2, spf, 4, k.size, sigma ** 2, session, int(t * 1e9)))
                out.write(inter.tobytes()); out.write(k.tobytes()); out.write(np.full(k.size, 2, np.uint32).tobytes())
    print(json.dumps({"out": a.out, "target_start": list(p0), "velocity": list(v)}))

if __name__ == "__main__":
    main()
