#!/usr/bin/env python3
# openair1/PHY/NR_UE_ISAC/tools/show_block.py
"""Plot one CFR-stage dump: |H| (antenna x row x subcarrier), the per-row range profile, and slow-time
phase at the strongest range bin. Usage: show_block.py DUMP.bin [--png out.png]

Dump format (written by sensing_engine.cc's dump_window lambda):
  uint32 antennas, rows, subcarriers
  double row_time_slots[rows]
  uint8 observed[rows * subcarriers]                              -- CfrWindow::observed, [row][subcarrier]
  complex64 values[antennas * rows * subcarriers]                  -- CfrWindow::values, antenna-major [a][row][subcarrier]
Confirmed against the CfrWindow struct in pipeline_types.h (comment on `values`: "antenna-major
[a][row][subcarrier]"; `observed`: "[row][subcarrier]") -- reshape below matches that order as-is,
no transpose needed.
"""
import argparse, numpy as np

def load(path):
    with open(path, "rb") as f:
        ant, rows, sub = np.frombuffer(f.read(12), np.uint32)
        t = np.frombuffer(f.read(8 * rows), np.float64)
        rest = f.read()
    nval = ant * rows * sub
    obs = np.frombuffer(rest[: len(rest) - 8 * nval], np.uint8)
    val = np.frombuffer(rest[len(rest) - 8 * nval:], np.complex64).reshape(ant, rows, sub)
    return int(ant), int(rows), int(sub), t, obs, val

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("dump"); ap.add_argument("--png")
    a = ap.parse_args(); ant, rows, sub, t, obs, H = load(a.dump)
    import matplotlib; matplotlib.use("Agg" if a.png else matplotlib.get_backend()); import matplotlib.pyplot as plt
    prof = np.abs(np.fft.ifft(H, axis=2)) ** 2
    fig, ax = plt.subplots(3, ant, figsize=(4 * ant, 9), squeeze=False)
    for i in range(ant):
        ax[0, i].imshow(20 * np.log10(np.abs(H[i]) + 1e-12), aspect="auto"); ax[0, i].set_title(f"ch{i} |H| dB")
        ax[1, i].imshow(10 * np.log10(prof[i] + 1e-12), aspect="auto"); ax[1, i].set_title("range profile per row")
        b = int(np.argmax(prof[i].mean(0))); ax[2, i].plot(t, np.unwrap(np.angle(np.fft.ifft(H[i], axis=1)[:, b])))
        ax[2, i].set_title(f"slow-time phase @bin {b}")
    fig.suptitle(f"{a.dump}: {ant} ant x {rows} rows x {sub} sc, observed={obs.mean():.3f}")
    fig.tight_layout(); fig.savefig(a.png) if a.png else plt.show()

if __name__ == "__main__":
    main()
