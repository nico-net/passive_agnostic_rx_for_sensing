#!/usr/bin/env python3
"""Tier 0: measures the baseband tone frequency in a CW capture via phase-unwrap + linear fit.

No dependency on the C++ tracker or the OFDM chain -- a fast first sanity check that a TX LO
offset shows up, with the right sign and magnitude, at the RX before building the full Tier 1
rig. See tests/ota_sync_bench/README.md.
"""
import argparse

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--capture", default="tone_capture.fc32")
    ap.add_argument("--rate", type=float, required=True, help="Hz, must match rx_uhd.py's --rate for this capture")
    ap.add_argument("--injected-cfo-hz", type=float, default=None, help="tx_uhd.py's --cfo-hz, if used")
    ap.add_argument("--skip-samples", type=int, default=1000, help="drop TX/RX settling transient at the start")
    args = ap.parse_args()

    x = np.fromfile(args.capture, dtype=np.complex64)[args.skip_samples :]
    if x.shape[0] < 2:
        raise SystemExit("capture too short after --skip-samples")

    phase = np.unwrap(np.angle(x))
    t = np.arange(x.shape[0]) / args.rate
    slope, intercept = np.polyfit(t, phase, 1)
    cfo_hz = slope / (2.0 * np.pi)

    mean_power_db = 10.0 * np.log10(np.mean(np.abs(x) ** 2) + 1e-30)
    print(f"loaded {args.capture}: {x.shape[0]} samples @ {args.rate/1e6:.3f} Msps, mean power {mean_power_db:.1f} dB")
    print(f"measured baseband tone: {cfo_hz:+.4f} Hz")
    if args.injected_cfo_hz is not None:
        print(f"injected (tx_uhd.py --cfo-hz): {args.injected_cfo_hz:+.4f} Hz")
        print(f"delta: {cfo_hz - args.injected_cfo_hz:+.4f} Hz")
        print("(any nonzero delta here is the two SDRs' own uncontrolled free-running CFO, "
              "riding on top of the deliberate offset -- expected, not an error)")


if __name__ == "__main__":
    main()
