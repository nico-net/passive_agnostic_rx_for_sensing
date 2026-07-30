#!/usr/bin/env python3
"""Turns a raw RX capture (rx_uhd.py / rx_bladerf.sh + convert_iq.py) into the CFR grid dump
that openair1/PHY/NR_UE_ISAC/tools/isac_sync_replay.cc feeds directly into the REAL
cpi_sto_tracker/cpi_cfo_tracker/cpi_sfo_tracker C++ classes -- see tests/ota_sync_bench/README.md.

Also computes its own independent "trusted" CFO/delay/SFO estimates via a plain
frequency-domain phase-difference method (NOT the production Hann+IFFT+peak-search code
path) as a cross-check to run alongside the tracker's own numbers.
"""
import argparse
import json

import numpy as np
from scipy.signal import correlate

from grid_dump import write_grid_dump
from ofdm_common import gen_pilots, ofdm_demodulate, ofdm_modulate, slot_dur_s

SPEED_OF_LIGHT = 299792458.0


def coarse_align(capture, template, search_samples):
    """Finds ONE valid symbol-boundary alignment via matched filtering.

    The reference waveform is exactly periodic (one symbol repeated back to back), so its
    autocorrelation has near-equal-magnitude peaks every symbol_len samples -- searching far
    past the first few periods risks argmax landing on a later replica whose position has
    drifted (SFO) or faded more than an earlier one, which is *worse*, not better, since any
    valid replica is equally usable as the row-0 anchor (rows only need consistent symbol_len
    spacing from there, not a globally unique time origin). Callers should therefore pass a
    small search_samples (a handful of symbol periods), not the whole capture.
    """
    n = min(search_samples, capture.shape[0] - template.shape[0])
    window = capture[: n + template.shape[0]]
    corr = correlate(window, template, mode="valid", method="fft")
    mag = np.abs(corr)
    peak = int(np.argmax(mag))
    snr_db = 20.0 * np.log10((mag[peak] + 1e-30) / (np.median(mag) + 1e-30))
    return peak, snr_db


def trusted_delay_cfo_sfo(h_cpi, row_time_slots, scs_hz, nof_subc):
    bin_to_delay_s = 1.0 / (nof_subc * scs_hz)

    # Frequency-domain single-lag delay estimator: phase(H[k]) = -2*pi*k*scs_hz*tau, so the
    # adjacent-subcarrier phase difference (averaged over k for noise robustness) recovers tau
    # unambiguously across the full nof_subc-bin range, via a code path that shares nothing with
    # the production isac_sync.cc (which builds a compact CIR via IFFT and searches its peak).
    lag_prod_k = h_cpi[:, 1:] * np.conj(h_cpi[:, :-1])
    dphi_k = np.angle(np.sum(lag_prod_k, axis=1))
    tau_est_s = -dphi_k / (2.0 * np.pi * scs_hz)
    range_bin_est = tau_est_s / bin_to_delay_s

    # Adjacent-row phase difference at fixed subcarriers (averaged over k) isolates the
    # row-to-row common phase rotation a CFO produces, independent of delay (which varies with k,
    # not with row, for a static channel).
    row_dt = np.diff(row_time_slots) * slot_dur_s(scs_hz)
    lag_prod_r = h_cpi[1:, :] * np.conj(h_cpi[:-1, :])
    dphi_r = np.angle(np.sum(lag_prod_r, axis=1))
    valid_dt = row_dt > 0
    cfo_est_hz = float(np.mean(dphi_r[valid_dt] / (2.0 * np.pi * row_dt[valid_dt])))

    # SFO: linear drift of the delay estimate over the CPI.
    t = row_time_slots * slot_dur_s(scs_hz)
    slope_bins_per_s = float(np.polyfit(t, range_bin_est, 1)[0])
    sfo_ppm_est = slope_bins_per_s * bin_to_delay_s * 1e6

    return range_bin_est, cfo_est_hz, sfo_ppm_est


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--capture", default="rx_capture.fc32")
    ap.add_argument("--meta", default="tx_waveform.json")
    ap.add_argument("--dl-center-hz", type=float, default=0.0, help="RX-tuned center freq, cosmetic only")
    ap.add_argument("--pci", type=int, default=0)
    ap.add_argument("--max-rows", type=int, default=0, help="0 = use every full symbol available")
    ap.add_argument("--search-samples", type=int, default=0,
                     help="window searched for coarse sync; 0 = auto (a handful of symbol periods -- "
                          "see coarse_align()'s docstring for why a LARGE window is actively worse here")
    ap.add_argument("--los-bin-offset", type=float, default=20.0,
                     help="park the CIR peak this many range bins from 0, away from the wrap "
                          "boundary the tracker's non-circular search window can't handle "
                          "(0 disables; see the comment at the application site)")
    ap.add_argument("--injected-sto-samples", type=float, default=None,
                     help="pass through gen_comb_waveform.py's --sto-samples if you used it (Tier 1 GT)")
    ap.add_argument("--injected-cfo-hz", type=float, default=None, help="pass through tx_uhd.py's --cfo-hz")
    ap.add_argument("--injected-sfo-ppm", type=float, default=None, help="pass through tx_uhd.py's --sfo-ppm")
    ap.add_argument("--out", default="grid_dump.bin")
    args = ap.parse_args()

    with open(args.meta) as f:
        meta = json.load(f)
    nof_subc, scs_hz = meta["nof_subc"], meta["scs_hz"]
    fft_size, cp_len, symbol_len = meta["fft_size"], meta["cp_len"], meta["symbol_len"]
    fs_hz = meta["sample_rate_hz"]

    pilots = gen_pilots(nof_subc, meta["seed"])
    template = ofdm_modulate(pilots, fft_size, cp_len).astype(np.complex64)

    capture = np.fromfile(args.capture, dtype=np.complex64)
    print(f"loaded {args.capture}: {capture.shape[0]} samples @ {fs_hz/1e6:.3f} Msps "
          f"({capture.shape[0]/fs_hz:.3f} s)")

    search_samples = args.search_samples if args.search_samples > 0 else 6 * symbol_len
    offset, snr_db = coarse_align(capture, template, search_samples)
    print(f"coarse sync: offset={offset} samples ({offset/fs_hz*1e6:.3f} us), correlation SNR~{snr_db:.1f} dB")
    if snr_db < 10.0:
        print("WARNING: low correlation SNR -- check gain/frequency/rate before trusting the rest of this output")

    n_avail = (capture.shape[0] - offset) // symbol_len
    cpi_rows = n_avail if args.max_rows <= 0 else min(args.max_rows, n_avail)
    if cpi_rows < 4:
        raise SystemExit(f"only {cpi_rows} full symbols available after sync -- capture too short or sync failed")

    h_cpi = np.empty((cpi_rows, nof_subc), dtype=np.complex64)
    for r in range(cpi_rows):
        block = capture[offset + r * symbol_len : offset + (r + 1) * symbol_len]
        y = ofdm_demodulate(block, fft_size, cp_len, nof_subc)
        h_cpi[r, :] = (y / pilots).astype(np.complex64)

    # Park the CIR peak mid-array, away from the bin-0 / bin-(M-1) wrap boundary.
    #
    # Why this is REQUIRED, not cosmetic: coarse_align() removes the bulk delay so precisely that
    # the residual sits at ~0 bins -- which is the one place cpi_sto_tracker cannot search. Its
    # window is NOT circular (isac_sync.cc estimate_row(): lo=max(1, c-halfwin),
    # hi=min(M-2, c+halfwin)), so bins 0 and M-1 are structurally excluded, and a slightly NEGATIVE
    # residual wraps to ~M where the clamped window then searches pure noise -- measured on a real
    # capture: every row fade-gated, n_valid=0, and a nonsense -1142 ppm SFO fitted to that noise.
    # Real deployments never hit this because their LOS tap sits at a comfortable mid-array bin
    # (~6 / 49 m on this project's own cell), so this offset makes the bench match that geometry
    # rather than working around a tracker limitation that doesn't exist in the field.
    #
    # A frequency-domain phase ramp is exact for any (including fractional) bin offset and is the
    # same primitive isac_sync.cc itself uses for its corrections: multiplying H[k] by
    # exp(-j*2*pi*k*offset/nof_subc) adds exactly `offset` bins of delay, moving the peak without
    # touching row-to-row (Doppler/CFO) phase or the delay DRIFT the SFO fit measures.
    if args.los_bin_offset != 0.0:
        k = np.arange(nof_subc, dtype=np.float64)
        ramp = np.exp(-2j * np.pi * k * args.los_bin_offset / nof_subc).astype(np.complex64)
        h_cpi *= ramp[np.newaxis, :]

    occ_all = np.ones((cpi_rows, nof_subc), dtype=np.uint8)
    row_comb = np.ones(cpi_rows, dtype=np.uint32)
    t_sym_s = symbol_len / fs_hz
    row_time_slots = np.arange(cpi_rows, dtype=np.float64) * (t_sym_s / slot_dur_s(scs_hz))

    range_bin_est, trusted_cfo_hz, trusted_sfo_ppm = trusted_delay_cfo_sfo(
        h_cpi.astype(np.complex128), row_time_slots, scs_hz, nof_subc)
    bin_to_delay_s = 1.0 / (nof_subc * scs_hz)
    # No /2 here: matches isac_sync.cc's nominal_los_bin() ("Differential bistatic range per bin --
    # no monostatic /2") and range_doppler.cc's rvm.range_res_m. Getting this wrong doesn't just
    # mis-seed the tracker's search window by a factor of 2 -- nominal_los_bin() casts a NEGATIVE
    # nominal_range_m to uint32_t, which wraps around to a huge bin index and makes every row fail
    # structurally (n_valid=0 across STO/CFO/SFO at once).
    range_res_m = SPEED_OF_LIGHT * bin_to_delay_s
    trusted_mean_range_bin = float(np.mean(range_bin_est))
    # trusted_mean_range_bin legitimately can be negative (coarse_align() doesn't guarantee which
    # side of the periodic ambiguity it lands on) -- but the CIR the tracker actually searches is
    # built via IFFT over the occupied subcarriers, which is inherently CIRCULAR: bin -9.5 in that
    # representation is the same physical point as bin (nof_subc - 9.5), not +9.5. abs() was tried
    # first and is wrong -- it reflects a negative residual onto the wrong side of the search
    # window (measured: 99.8% flywheel on a real capture, tracker never locking) instead of
    # wrapping it to where the CIR actually places it. Python's % already yields the correct
    # non-negative representative for a positive modulus, matching the CIR's own indexing.
    wrapped_range_bin = trusted_mean_range_bin % nof_subc
    nominal_los_range_m = wrapped_range_bin * range_res_m

    print("\n--- trusted (independent, frequency-domain phase-difference) cross-check ---")
    print(f"  mean delay:  {trusted_mean_range_bin:+.3f} bins ({nominal_los_range_m:+.3f} m equiv, "
          f"range_res={range_res_m:.3f} m/bin, std={np.std(range_bin_est):.3f} bins)")
    print(f"  CFO:         {trusted_cfo_hz:+.3f} Hz")
    print(f"  SFO:         {trusted_sfo_ppm:+.4f} ppm")
    if args.injected_cfo_hz is not None:
        print(f"  (Tier 1 injected CFO was {args.injected_cfo_hz:+.3f} Hz -- compare against the delta "
              f"between this run and a --cfo-hz 0 baseline run, not this absolute number alone)")
    if args.injected_sfo_ppm is not None:
        print(f"  (Tier 1 injected SFO was {args.injected_sfo_ppm:+.4f} ppm -- same caveat)")

    write_grid_dump(
        args.out,
        h_cpi=h_cpi, occ_all=occ_all, row_comb=row_comb, row_time_slots=row_time_slots,
        nof_subc=nof_subc, nof_prb=meta["nof_prb"], scs_hz=scs_hz,
        dl_center_hz=int(args.dl_center_hz), pci=args.pci,
        slots_per_frame=max(1, round(scs_hz / 15000.0)),
        nominal_los_range_m=nominal_los_range_m,
        trusted_cfo_hz=trusted_cfo_hz, trusted_sfo_ppm=trusted_sfo_ppm,
        trusted_mean_range_bin=trusted_mean_range_bin,
        injected_sto_samples=args.injected_sto_samples,
        injected_cfo_hz=args.injected_cfo_hz,
        injected_sfo_ppm=args.injected_sfo_ppm,
    )
    print(f"\nwrote {args.out}: {cpi_rows} rows x {nof_subc} subcarriers")
    print("next: run the C++ replay tool against this file (see README.md)")


if __name__ == "__main__":
    main()
