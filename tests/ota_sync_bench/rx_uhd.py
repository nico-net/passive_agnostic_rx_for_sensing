#!/usr/bin/env python3
"""Captures a fixed duration of IQ from a UHD device to a raw fc32 file for process_capture.py."""
import argparse

import numpy as np
import uhd


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--args", default="", help="UHD device args, e.g. 'serial=YYYY' or 'addr=192.168.10.3'")
    ap.add_argument("--center-freq", type=float, required=True, help="Hz, must match tx_uhd.py's --center-freq")
    ap.add_argument("--rate", type=float, required=True, help="Hz, should match the waveform's nominal sample_rate_hz")
    ap.add_argument("--gain", type=float, default=30.0)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--out", default="rx_capture.fc32")
    args = ap.parse_args()

    usrp = uhd.usrp.MultiUSRP(args.args)
    num_samps = int(np.ceil(args.duration * args.rate))
    print(f"RX: freq={args.center_freq:.1f} Hz, rate={args.rate:.3f} Hz, gain={args.gain} dB, "
          f"duration={args.duration} s ({num_samps} samples)")
    samps = usrp.recv_num_samps(num_samps, args.center_freq, args.rate, [args.channel], args.gain)
    samps[0].astype(np.complex64).tofile(args.out)
    print(f"wrote {args.out}: {samps.shape[1]} samples")


if __name__ == "__main__":
    main()
