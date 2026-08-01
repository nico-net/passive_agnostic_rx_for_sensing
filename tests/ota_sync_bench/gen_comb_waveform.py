#!/usr/bin/env python3
"""Generates the known repeating OFDM reference waveform for the OTA sync bench (Tier 1).

Continuously repeats one dense (comb-1), fully-occupied OFDM symbol -- each repetition is
one "row" / CFR snapshot in isac_sync's terms (see openair1/PHY/NR_UE_ISAC/isac_sync.h). No
NR frame structure (no SSB/PBCH/slot boundaries) -- this is a self-contained RF link test,
not a protocol test.

Output: raw interleaved float32 IQ (fc32, i.e. numpy complex64.tofile()) playable directly
via tx_uhd.py or (after conversion) bladeRF-cli. A companion .json sidecar records every
parameter process_capture.py needs to regenerate the identical pilot sequence and demodulate.
"""
import argparse
import json

import numpy as np

from ofdm_common import gen_pilots, nof_subc_for, ofdm_modulate


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nof-prb", type=int, default=52)
    ap.add_argument("--scs-hz", type=float, default=30000.0)
    ap.add_argument("--fft-size", type=int, default=1024)
    ap.add_argument("--cp-frac", type=float, default=1.0 / 14.0, help="CP length as a fraction of fft-size")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--nof-symbols", type=int, default=4000, help="repeated OFDM symbols = rows in one capture")
    ap.add_argument("--sto-samples", type=float, default=0.0,
                     help="deliberate fractional-sample delay pre-applied to the whole waveform "
                          "(Tier 1 controlled-STO ground truth; 0 = none)")
    ap.add_argument("--amplitude", type=float, default=0.3, help="peak IQ magnitude after scaling")
    ap.add_argument("--out", default="tx_waveform.fc32")
    ap.add_argument("--meta-out", default="tx_waveform.json")
    args = ap.parse_args()

    nof_subc = nof_subc_for(args.nof_prb)
    cp_len = int(round(args.cp_frac * args.fft_size))
    fs_hz = args.fft_size * args.scs_hz

    pilots = gen_pilots(nof_subc, args.seed)
    symbol = ofdm_modulate(pilots, args.fft_size, cp_len)
    symbol_len = symbol.shape[0]

    waveform = np.tile(symbol, args.nof_symbols)

    if args.sto_samples != 0.0:
        n_total = waveform.shape[0]
        freqs = np.fft.fftfreq(n_total)
        ramp = np.exp(-2j * np.pi * freqs * args.sto_samples)
        waveform = np.fft.ifft(np.fft.fft(waveform) * ramp)

    peak = np.max(np.abs(waveform))
    waveform = (waveform / peak * args.amplitude).astype(np.complex64)
    waveform.tofile(args.out)

    meta = {
        "nof_prb": args.nof_prb,
        "nof_subc": nof_subc,
        "scs_hz": args.scs_hz,
        "fft_size": args.fft_size,
        "cp_len": cp_len,
        "symbol_len": symbol_len,
        "sample_rate_hz": fs_hz,
        "seed": args.seed,
        "nof_symbols": args.nof_symbols,
        "injected_sto_samples": args.sto_samples,
        "amplitude": args.amplitude,
    }
    with open(args.meta_out, "w") as f:
        json.dump(meta, f, indent=2)

    dur_s = args.nof_symbols * symbol_len / fs_hz
    print(f"wrote {args.out}: {waveform.shape[0]} samples @ {fs_hz/1e6:.3f} Msps "
          f"({dur_s:.3f} s, {args.nof_symbols} symbols, {symbol_len} samp/symbol incl. CP)")
    print(f"wrote {args.meta_out}")
    if args.sto_samples != 0.0:
        print(f"NOTE: {args.sto_samples:+.3f}-sample STO pre-applied to the whole waveform (Tier 1 ground truth)")


if __name__ == "__main__":
    main()
