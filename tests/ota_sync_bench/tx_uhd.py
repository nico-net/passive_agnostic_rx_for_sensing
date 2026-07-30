#!/usr/bin/env python3
"""Loops gen_comb_waveform.py's output out of a UHD device (B2xx/X4xx/N2xx/...).

Two independent-clock impairments can be injected here, deliberately, as Tier 1 ground
truth (see tests/ota_sync_bench/README.md):
  --cfo-hz     : TX center frequency offset from the nominal carrier the RX side tunes to.
  --sfo-ppm    : requests an actual device sample rate of rate*(1+ppm*1e-6) while the file
                 was generated at the nominal `rate` -- a genuine sample-clock-rate mismatch
                 relative to an RX side clocked at the nominal rate, not a simulated one.
With both at 0 (default), any residual CFO/SFO measured at the RX side is the two boards'
real, uncontrolled free-running-clock difference (closer to the actual autonomous-UE target
scenario -- see CLAUDE.md's OTA sync task notes).
"""
import argparse
import json

import numpy as np
import uhd


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--args", default="", help="UHD device args, e.g. 'serial=XXXX' or 'addr=192.168.10.2'")
    ap.add_argument("--file", default="tx_waveform.fc32")
    ap.add_argument("--meta", default="tx_waveform.json")
    ap.add_argument("--center-freq", type=float, required=True, help="Hz, must match rx_uhd.py's --center-freq")
    ap.add_argument("--cfo-hz", type=float, default=0.0, help="deliberate TX LO offset (Tier 1 ground truth)")
    ap.add_argument("--sfo-ppm", type=float, default=0.0, help="deliberate TX sample-rate offset (Tier 1 ground truth)")
    ap.add_argument("--gain", type=float, default=40.0)
    ap.add_argument("--duration", type=float, default=30.0)
    ap.add_argument("--channel", type=int, default=0)
    args = ap.parse_args()

    with open(args.meta) as f:
        meta = json.load(f)
    nominal_rate = meta["sample_rate_hz"]
    # Sign chosen to MATCH isac_sync.h's convention (positive ppm = delay increasing over time),
    # not the naive "TX faster = positive ppm" reading. Measured empirically (real capture,
    # --sfo-ppm 2.0): a faster TX clock (rate*(1+ppm)) makes each RX sample read progressively
    # further ahead into the TX waveform than a synced clock would predict, so the RX-referenced
    # delay DECREASES over time -- both the production tracker and an independently-implemented
    # cross-check measured -1.99/-1.91 ppm for a requested +2.0, confirming magnitude but with
    # inverted sign under the old (1+ppm) formula. (1-ppm) makes the parameter mean what the
    # tracker means, so a requested +X ppm shows up as a measured +X ppm delta.
    actual_rate = nominal_rate * (1.0 - args.sfo_ppm * 1e-6)

    waveform = np.fromfile(args.file, dtype=np.complex64)

    usrp = uhd.usrp.MultiUSRP(args.args)
    print(f"TX: {args.file} ({waveform.shape[0]} samples), "
          f"freq={args.center_freq + args.cfo_hz:.1f} Hz (nominal {args.center_freq:.1f} + cfo {args.cfo_hz:+.3f}), "
          f"rate={actual_rate:.3f} Hz (nominal {nominal_rate:.3f}, sfo {args.sfo_ppm:+.3f} ppm), "
          f"gain={args.gain} dB, duration={args.duration} s")
    n_sent = usrp.send_waveform(waveform, args.duration, args.center_freq + args.cfo_hz, actual_rate,
                                 [args.channel], args.gain)
    print(f"sent {n_sent} samples")


if __name__ == "__main__":
    main()
