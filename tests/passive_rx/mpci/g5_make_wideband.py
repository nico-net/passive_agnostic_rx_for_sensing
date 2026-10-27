#!/usr/bin/env python3
"""g5_make_wideband.py: TEST FIXTURE builder (MPCI G5 in-context bench), not part of the receiver.
Builds ONE wideband raw-IQ capture (raw_iq directory format: rx<c>.sc16 + capture_info.json) at rate R_out and centre F_out from several
single-cell captures recorded at their own centre (rate R_in = R_out / 2): each capture is band-limited to its cell's occupied band
(n_rb x 12 x SCS, + guard), interpolated by 2 and moved to (its centre - F_out), then the cells are summed. The result is what one radio at
F_out / R_out would have delivered had the cells been on air together (each cell's content keeps its own recording time; the receiver decodes
cells independently). Used to feed the receiver's wideband path (radio -> in-process channelizer -> cell rings) from a file through
mpci_file_pub, at the same content as the pre-channelized control (the single-cell captures published as cell rings).
usage: g5_make_wideband.py <out dir> <F_out Hz> <R_out Hz> <dir>:<n_rb>[:<scs_hz>] [<dir>:<n_rb> ...]
Inputs are read-only. Samples stay at the recorded scale (the replays right-shift by 2 when they publish)."""
import json, math, os, sys
import numpy as np
from scipy.signal import firwin

def main():
    if len(sys.argv) < 5:
        print(__doc__); return 2
    out, fout, rout = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    srcs = []
    for a in sys.argv[4:]:
        p = a.split(":")
        d, nrb, scs = p[0], int(p[1]), float(p[2]) if len(p) > 2 else 30e3
        ci = json.load(open(os.path.join(d, "capture_info.json")))
        srcs.append(dict(dir=d, ci=ci, nrb=nrb, scs=scs, fc=float(ci["rf_channels"][0]["frequency_hz"]), rate=float(ci["sample_rate_hz"]),
                         n=int(ci["samples_per_channel"]), nch=int(ci["channels"])))
    rin = rout / 2
    assert all(abs(s["rate"] - rin) < 1 for s in srcs), "every input must be recorded at R_out / 2"
    nch = srcs[0]["nch"]
    n_in = min(s["n"] for s in srcs)
    os.makedirs(out, exist_ok=True)
    # per source: complex low-pass at R_out keeping the cell's occupied band (+ 0.5 MHz guard; 2047 Kaiser taps: about 0.7 MHz transition, the 1.86 MHz gap between PCI 415 and 923 holds both edges), which also removes the interpolation image
    filt = []
    for s in srcs:
        half = 0.5 * s["nrb"] * 12 * s["scs"] + 0.5e6
        taps = firwin(2047, half, fs=rout, window=("kaiser", 9.0))
        shift = s["fc"] - fout
        num = int(round(shift / 1e3)); den = int(round(rout / 1e3))  # exact phase on a 1 kHz grid
        assert abs(num * 1e3 - shift) < 1e-3 and abs(den * 1e3 - rout) < 1e-3
        filt.append(dict(taps=taps.astype(np.float32), num=num, den=den, half=half))
        print("source %s centre %.3f MHz n_rb %d: band +-%.3f MHz, moved by %+.3f MHz" % (s["dir"], s["fc"] / 1e6, s["nrb"], half / 1e6, shift / 1e6))
    L = 1 << 20  # input samples per chunk
    M = 2047
    nfft = 1 << 22  # >= 2L + M - 1
    H = [np.fft.fft(np.concatenate([f["taps"], np.zeros(nfft - M, np.float32)])) for f in filt]
    peak = 0; clip = 0
    for c in range(nch):
        maps = [np.memmap(os.path.join(s["dir"], "rx%d.sc16" % c), dtype=np.int16, mode="r") for s in srcs]
        fo = open(os.path.join(out, "rx%d.sc16" % c), "wb")
        hist = [np.zeros(M - 1, np.complex64) for _ in srcs]
        n_out = 0
        for i0 in range(0, n_in, L):
            n = min(L, n_in - i0)
            acc = np.zeros(2 * n, np.complex64)
            for k, s in enumerate(srcs):
                x = maps[k][2 * i0: 2 * (i0 + n)].astype(np.float32)
                z = np.zeros(2 * n, np.complex64)
                z[0::2] = 2.0 * (x[0::2] + 1j * x[1::2])  # zero-stuffed, gain 2 restores the level after the interpolation low-pass
                buf = np.concatenate([hist[k], z])
                y = np.fft.ifft(np.fft.fft(buf, nfft) * H[k])[M - 1: M - 1 + 2 * n]
                hist[k] = buf[-(M - 1):]
                idx = (np.arange(2 * n, dtype=np.int64) + n_out) % filt[k]["den"]
                ph = 2 * np.pi * ((filt[k]["num"] % filt[k]["den"]) * idx % filt[k]["den"]) / filt[k]["den"]
                acc += (y * np.exp(1j * ph)).astype(np.complex64)
            o = np.empty(4 * n, np.float32)
            o[0::2] = acc.real; o[1::2] = acc.imag
            peak = max(peak, float(np.max(np.abs(o))))
            clip += int(np.count_nonzero(np.abs(o) > 32767))
            np.clip(np.rint(o), -32768, 32767).astype(np.int16).tofile(fo)
            n_out += 2 * n
        fo.close()
        print("channel %d: %d samples, peak %.0f, clipped %d" % (c, n_out, peak, clip)); sys.stdout.flush()
    ci = dict(srcs[0]["ci"])
    ci["samples_per_channel"] = 2 * n_in
    ci["sample_rate_hz"] = int(rout)
    ci["timestamp_tick_rate_hz"] = int(rout)
    ci["first_sample_tick"] = 2 * int(srcs[0]["ci"]["first_sample_tick"])
    ci["duration_s"] = 2 * n_in / rout
    ci["rf_channels"] = [dict(r, frequency_hz=int(fout), rate_hz=int(rout)) for r in ci["rf_channels"]]
    ci["status"] = "SYNTHETIC_WIDEBAND_G5"
    ci["g5_sources"] = [dict(dir=s["dir"], centre_hz=s["fc"], n_rb=s["nrb"]) for s in srcs]
    tmp = os.path.join(out, "capture_info.json.tmp")
    json.dump(ci, open(tmp, "w"), indent=1); os.rename(tmp, os.path.join(out, "capture_info.json"))
    print("wrote %s: %.3f s at %.2f MS/s centre %.3f MHz, peak %.0f, clipped %d" % (out, ci["duration_s"], rout / 1e6, fout / 1e6, peak, clip))
    return 0

if __name__ == "__main__":
    sys.exit(main())
