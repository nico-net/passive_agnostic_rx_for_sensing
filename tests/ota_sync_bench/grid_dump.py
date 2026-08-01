"""Binary grid-dump format shared between process_capture.py (writer) and the C++
isac_sync_replay tool (reader, openair1/PHY/NR_UE_ISAC/tools/isac_sync_replay.cc).

All fields little-endian, written as flat scalars/arrays (no padded struct) so the C++ side
can read them with plain sequential fread calls with no alignment ambiguity:

  char[8]   magic = "ISACOTA1"
  uint32    cpi_rows
  uint32    nof_subc
  uint32    nof_prb
  uint32    scs_hz
  uint64    dl_center_hz
  uint16    pci
  uint16    slots_per_frame
  float64   nominal_los_range_m       -- seeds the trackers' search window (isac_sync.h)
  float64   trusted_cfo_hz            -- process_capture.py's own independent CFO cross-check
  float64   trusted_sfo_ppm           -- ditto, SFO cross-check
  float64   trusted_mean_range_bin    -- ditto, mean delay cross-check
  uint8     has_injected_gt           -- 1 if the three fields below are known Tier-1 ground truth
  float64   injected_sto_samples
  float64   injected_cfo_hz
  float64   injected_sfo_ppm
  uint32[cpi_rows]   row_comb
  float64[cpi_rows]  row_time_slots
  float32[cpi_rows*nof_subc*2]  h_cpi, interleaved (re,im), row-major
  uint8[cpi_rows*nof_subc]      occ_all, row-major
"""
import struct

import numpy as np

MAGIC = b"ISACOTA1"


def write_grid_dump(path, *, h_cpi, occ_all, row_comb, row_time_slots, nof_subc, nof_prb, scs_hz,
                     dl_center_hz, pci, slots_per_frame, nominal_los_range_m,
                     trusted_cfo_hz, trusted_sfo_ppm, trusted_mean_range_bin,
                     injected_sto_samples=None, injected_cfo_hz=None, injected_sfo_ppm=None):
    cpi_rows = row_comb.shape[0]
    has_gt = injected_sto_samples is not None or injected_cfo_hz is not None or injected_sfo_ppm is not None

    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<IIII", cpi_rows, nof_subc, nof_prb, int(scs_hz)))
        f.write(struct.pack("<Q", int(dl_center_hz)))
        f.write(struct.pack("<HH", pci, slots_per_frame))
        f.write(struct.pack("<dddd", nominal_los_range_m, trusted_cfo_hz, trusted_sfo_ppm, trusted_mean_range_bin))
        f.write(struct.pack("<B", 1 if has_gt else 0))
        f.write(struct.pack("<ddd", injected_sto_samples or 0.0, injected_cfo_hz or 0.0, injected_sfo_ppm or 0.0))
        row_comb.astype("<u4").tofile(f)
        row_time_slots.astype("<f8").tofile(f)
        h_interleaved = np.empty(cpi_rows * nof_subc * 2, dtype="<f4")
        h_interleaved[0::2] = h_cpi.real.reshape(-1)
        h_interleaved[1::2] = h_cpi.imag.reshape(-1)
        h_interleaved.tofile(f)
        occ_all.astype("<u1").tofile(f)
