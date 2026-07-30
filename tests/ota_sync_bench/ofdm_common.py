"""Shared OFDM reference-waveform definitions for the OTA sync bench.

Both gen_comb_waveform.py (TX) and process_capture.py (RX) import this module so the
known pilot sequence and subcarrier-to-FFT-bin mapping can never drift out of sync between
the two sides -- X is regenerated from (seed, nof_subc) on the RX side rather than
transmitted or stored separately.

Carrier convention matches openair1/PHY/NR_UE_ISAC/isac_sync.h's nr_isac_carrier_t: nof_subc
= nof_prb*12, and slot_dur_s = 1e-3 * 15000/scs_hz (the NR-numerology slots-per-subframe
formula the tracker code uses internally). Because that formula ties slot_dur_s to scs_hz
via a fixed 14-symbols/slot assumption we don't actually replicate here (we send one OFDM
symbol per "row", not 14), row_time_slots must be scaled by T_sym/slot_dur_s rather than
left as consecutive integers -- see gen_grid_dump() in process_capture.py.
"""
import numpy as np

ISAC_NRE = 12  # subcarriers per PRB, matches defs_nr_UE_ISAC.h


def nof_subc_for(nof_prb: int) -> int:
    return nof_prb * ISAC_NRE


def slot_dur_s(scs_hz: float) -> float:
    slots_per_sf = max(1.0, scs_hz / 15000.0)
    return 1e-3 / slots_per_sf


def gen_pilots(nof_subc: int, seed: int) -> np.ndarray:
    """Deterministic QPSK pilot per occupied subcarrier, regenerable from (nof_subc, seed) alone."""
    rng = np.random.default_rng(seed)
    bits = rng.integers(0, 2, size=(nof_subc, 2))
    return ((1 - 2 * bits[:, 0]) + 1j * (1 - 2 * bits[:, 1])) / np.sqrt(2.0)


def map_to_fft_bins(x: np.ndarray, fft_size: int) -> np.ndarray:
    """Centered mapping, DC left null: bins [1..half] <- upper half of x, [-half..-1] <- lower half."""
    nof_subc = x.shape[0]
    half = nof_subc // 2
    assert nof_subc % 2 == 0 and 2 * half < fft_size, "nof_subc must be even and fit under fft_size"
    freq = np.zeros(fft_size, dtype=np.complex128)
    freq[1 : half + 1] = x[half:]
    freq[fft_size - half :] = x[:half]
    return freq


def extract_from_fft_bins(freq: np.ndarray, nof_subc: int) -> np.ndarray:
    half = nof_subc // 2
    fft_size = freq.shape[0]
    out = np.zeros(nof_subc, dtype=np.complex128)
    out[half:] = freq[1 : half + 1]
    out[:half] = freq[fft_size - half :]
    return out


def ofdm_modulate(x: np.ndarray, fft_size: int, cp_len: int) -> np.ndarray:
    freq = map_to_fft_bins(x, fft_size)
    time_sym = np.fft.ifft(freq) * fft_size
    return np.concatenate([time_sym[-cp_len:], time_sym])


def ofdm_demodulate(rx_symbol_with_cp: np.ndarray, fft_size: int, cp_len: int, nof_subc: int) -> np.ndarray:
    time_sym = rx_symbol_with_cp[cp_len : cp_len + fft_size]
    freq = np.fft.fft(time_sym) / fft_size
    return extract_from_fft_bins(freq, nof_subc)
