#!/usr/bin/env python3
"""Tier 0: writes a plain constant-envelope baseband waveform (no OFDM structure).

TX'd unmodified, the deliberate CFO comes entirely from tx_uhd.py's --cfo-hz center-frequency
offset relative to rx_uhd.py's --center-freq -- after downconversion at the RX's (uncshifted)
tuning frequency, that RF offset appears as a pure baseband tone at exactly --cfo-hz, which
tone_cfo_check.py then measures by a simple phase-unwrap + linear fit. Reuses tx_uhd.py/
rx_uhd.py unmodified (they only read meta["sample_rate_hz"] and the raw fc32 samples).
"""
import argparse
import json

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rate", type=float, default=1e6)
    ap.add_argument("--duration", type=float, default=2.0)
    ap.add_argument("--amplitude", type=float, default=0.3)
    ap.add_argument("--out", default="tone_waveform.fc32")
    ap.add_argument("--meta-out", default="tone_waveform.json")
    args = ap.parse_args()

    n = int(args.rate * args.duration)
    waveform = np.full(n, args.amplitude, dtype=np.complex64)
    waveform.tofile(args.out)
    with open(args.meta_out, "w") as f:
        json.dump({"sample_rate_hz": args.rate}, f, indent=2)
    print(f"wrote {args.out}: {n} samples @ {args.rate/1e6:.3f} Msps ({args.duration} s)")


if __name__ == "__main__":
    main()
