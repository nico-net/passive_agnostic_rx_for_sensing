#!/usr/bin/env python3
"""Converts between this bench's fc32 (numpy complex64) format and bladeRF-cli's raw
SC16 Q11 'bin' format (int16 I,Q interleaved, [-2048,2047], little-endian).

bladeRF-cli's tx/rx config only speak csv or this raw SC16Q11 bin format (see
`bladeRF-cli --help-interactive`'s tx/rx config sections) -- UHD's fc32 is the bench's
common format everywhere else (gen_comb_waveform.py's output, process_capture.py's input),
so bladeRF users convert at the TX/RX boundary with this script.
"""
import argparse

import numpy as np

Q11_SCALE = 2048.0


def fc32_to_sc16q11(path_in, path_out):
    x = np.fromfile(path_in, dtype=np.complex64)
    i = np.clip(np.round(x.real * Q11_SCALE), -2048, 2047).astype("<i2")
    q = np.clip(np.round(x.imag * Q11_SCALE), -2048, 2047).astype("<i2")
    out = np.empty(2 * x.shape[0], dtype="<i2")
    out[0::2] = i
    out[1::2] = q
    out.tofile(path_out)


def sc16q11_to_fc32(path_in, path_out):
    raw = np.fromfile(path_in, dtype="<i2")
    i = raw[0::2].astype(np.float32) / Q11_SCALE
    q = raw[1::2].astype(np.float32) / Q11_SCALE
    (i + 1j * q).astype(np.complex64).tofile(path_out)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("direction", choices=["to-sc16q11", "from-sc16q11"])
    ap.add_argument("infile")
    ap.add_argument("outfile")
    args = ap.parse_args()
    if args.direction == "to-sc16q11":
        fc32_to_sc16q11(args.infile, args.outfile)
    else:
        sc16q11_to_fc32(args.infile, args.outfile)
    print(f"wrote {args.outfile}")


if __name__ == "__main__":
    main()
