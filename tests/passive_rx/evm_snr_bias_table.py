#!/usr/bin/env python3
"""Generates the TRUE_TO_EST table pasted into goal_metrics.py (decision-directed EVM bias, DLCRC-ROOTCAUSE-20261016 sec.1).
Estimator mirrors nr_dlsch_demodulation.c:2166-2300: AWGN, rescale to ideal power incl. noise, error to nearest point.
Deterministic: fixed seed, fixed draw order. Needs numpy (generator only; goal_metrics stays stdlib).
usage: evm_snr_bias_table.py   (prints the dict literal)"""
import numpy as np

GRID = list(range(-10, 31))

def dd(rng, qm, snr_db, n=400000):
    lmax = (1 << (qm // 2)) - 1; lv = np.arange(-lmax, lmax + 1, 2); ideal = 2 * np.mean(lv ** 2)
    a = rng.choice(lv, (n, 2)); sig = np.sqrt(ideal / 2 / 10 ** (snr_db / 10))
    v = a + rng.normal(0, sig, (n, 2)); v = v * np.sqrt(ideal / np.mean(np.sum(v ** 2, 1)))
    si = np.clip(2 * np.floor(v / 2) + 1, -lmax, lmax)
    return -10 * np.log10(np.mean(np.sum((v - si) ** 2, 1)) / ideal)

if __name__ == '__main__':
    print('TRUE_TO_EST = {')
    for qm in (2, 4, 6, 8):
        rng = np.random.default_rng(1000 + qm)
        est = np.maximum.accumulate([round(float(dd(rng, qm, t)), 2) for t in GRID])
        print(f"    {qm}: {list(map(float, est))},")
    print('}')
