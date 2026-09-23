#!/usr/bin/env python3
# openair1/PHY/NR_UE_ISAC/tools/make_coherent_scene.py
"""4-channel phase-coherent CFR rows for the coherent fuser tests: one LO (common CFO/phase noise),
per-channel constant phase offsets, identical cables (+ sub-ns residuals), DL-only TDD cadence with
irregular grants and per-row allocations, a static wall reflection of the gNB that is STRONGER than
the LOS on channel 2, a person, a car and a drone (with a ground bounce), and a tape-perturbed survey."""
import argparse, json
import numpy as np
from make_synthetic_rows import header

C = 299792458.0
PRB, SCS, FC, SPF = 273, 30000, 3450000000, 20

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True); ap.add_argument("--truth", required=True)
    ap.add_argument("--survey-out", required=True); ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--square", type=float, default=10.0); ap.add_argument("--snr-db", type=float, default=20.0)
    ap.add_argument("--scramble-phases", action="store_true", help="random phase per channel per ROW (no coherence)")
    a = ap.parse_args()
    rng = np.random.default_rng(11)
    s = a.square
    rx = [np.array(p) for p in ([0, 0, 0.5], [s, 0, 3.5], [0, s, 3.5], [s, s, 0.5])]
    gnb = np.array([35.0, 20.0, 6.0])
    phases = rng.uniform(-np.pi, np.pi, 4); phases[0] = 0.0
    resid_delay = rng.uniform(-0.5e-9, 0.5e-9, 4)                  # identical cables, sub-ns residuals
    wall_gnb = gnb * np.array([1, 1, 1]); wall_gnb[1] = 2 * 30.0 - gnb[1]   # mirror of gNB in plane y = 30 m
    targets = [dict(name="person", p0=[4.0, 18.0, 1.0], v=[0.0, -1.2, 0.0], a=0.25),
               dict(name="car",    p0=[-12.0, 5.0, 0.8], v=[8.0, 0.0, 0.0], a=0.6),
               dict(name="drone",  p0=[8.0, 8.0, 15.0], v=[-3.0, 2.0, 0.5], a=0.12)]
    k_all = np.arange(PRB * 12, dtype=np.uint32)
    f_all = (k_all.astype(float) - PRB * 6) * SCS
    lam = C / FC; sigma = 10 ** (-a.snr_db / 20); cfo_hz = 37.0
    with open(a.out, "wb") as out:
        for n in range(int(a.seconds / 0.0005)):
            t = n * 0.0005
            if (n % 10) >= 7 or rng.random() > 0.6:                # DL slots of a 7D pattern, 60 % grant probability
                continue
            nprb = int(rng.choice([4, 8, 16, 32, 64, 128, 200, 273])); start = int(rng.integers(0, PRB - nprb + 1))
            k = k_all[start * 12:(start + nprb) * 12]; f = f_all[start * 12:(start + nprb) * 12]
            common = np.exp(2j * np.pi * cfo_hz * t) * np.exp(1j * rng.normal(0, 0.05))   # one LO for all channels
            h = np.empty((4, k.size), np.complex64)
            for i in range(4):
                taus, amps = [], []
                taus.append(np.linalg.norm(rx[i] - gnb) / C); amps.append(1.0 if i != 2 else 0.3)   # LOS weak on ch2
                taus.append((np.linalg.norm(wall_gnb - rx[i])) / C); amps.append(0.6 if i == 2 else 0.2)  # static wall
                for tg in targets:
                    p = np.array(tg["p0"]) + np.array(tg["v"]) * t
                    taus.append((np.linalg.norm(p - gnb) + np.linalg.norm(rx[i] - p)) / C); amps.append(tg["a"])
                    if tg["name"] == "drone":                        # ground bounce: image at -z, reflection -0.5
                        pm = p * np.array([1, 1, -1])
                        taus.append((np.linalg.norm(pm - gnb) + np.linalg.norm(rx[i] - pm)) / C); amps.append(-0.5 * tg["a"])
                acc = np.zeros(k.size, complex)
                for tau, amp in zip(taus, amps):
                    acc += amp * np.exp(-2j * np.pi * (FC + f) * (tau + resid_delay[i]))
                ph = rng.uniform(-np.pi, np.pi) if a.scramble_phases else phases[i]
                noise = sigma * (rng.standard_normal(k.size) + 1j * rng.standard_normal(k.size)) / np.sqrt(2)
                h[i] = (acc * common * np.exp(1j * ph) + noise).astype(np.complex64)
            inter = np.empty((4, 2 * k.size), np.float32); inter[:, 0::2] = h.real; inter[:, 1::2] = h.imag
            out.write(header(0, n % (1024 * SPF), 0.0, 3, PRB, SCS, FC, 2, SPF, 4, k.size, sigma ** 2, 0, int(t * 1e9)))
            out.write(inter.tobytes()); out.write(k.tobytes()); out.write(np.full(k.size, 2, np.uint32).tobytes())
    tape = np.random.default_rng(3)
    pert = lambda p: (np.array(p) + tape.normal(0, 0.05, 3)).round(3).tolist()
    json.dump({"frame": "ENU m, origin at the X410 (tape-perturbed)", "gnb_m": pert(gnb),
               "rx_antennas_m": {f"ch{i}": pert(rx[i]) for i in range(4)}, "max_range_m": 120},
              open(a.survey_out, "w"))
    json.dump({"phases_rad": phases.tolist(), "resid_delay_s": resid_delay.tolist(), "cfo_hz": cfo_hz,
               "targets": [{"name": t_["name"], "p0": t_["p0"], "v": t_["v"]} for t_ in targets],
               "geometry": {"gnb": gnb.tolist(), "rx": [r.tolist() for r in rx]}}, open(a.truth, "w"))

if __name__ == "__main__":
    main()
