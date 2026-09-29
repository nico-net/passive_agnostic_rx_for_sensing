# Fix2 report: passive UL scrambling-ID library/PHY_NR_UE layering defect

Lane: fix2, worktree `sens6:/home/sens/NICOLA/agn-wt/fix2`, branch `sdd/agn-fix2`.
Commit: `f20670eaa2` — "Passive UL: keep scrambling-ID decision state in the library (no PHY_NR_UE
dependency)".

## Root cause
`nr_pdcch_blind_monitor.c`'s `blind_ul_apply_scrambling_ids()` (in the offline-gtest-linked
`nr_pdcch_blind_monitor` library) called `nr_pusch_passive_ul_dmrs_id()` /
`nr_pusch_passive_data_id_current()` / `nr_pusch_passive_ul_crc_stalled()`, all defined in
`nr_pusch_passive_decode.c`, compiled into `PHY_NR_PASSIVE_UL` (the gNB PUSCH receive chain).
`nr-uesoftmodem` masked this (both libraries land in one `--start-group`), but
`test_nr_pdcch_blind_monitor` cannot and must not link `PHY_NR_PASSIVE_UL`, so it failed with 4
undefined references (same class of defect `nr_pdcch_coreset_bank.{h,c}` fixed for the CORESET
bank per `fix-link-report.md` R14).

## Fix
New `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_ul_ids.{h,c}`, added to the
`nr_pdcch_blind_monitor` library's `add_library()` sources. Holds only the pure decision state:
- Per-nSCID UL DM-RS estimate storage (`nr_dmrs_id_state_t[2]`), its mutex array, and the stage-2
  throttle counter — exposed via `nr_pusch_passive_ul_dmrs_id()` (unchanged read-only getter),
  `nr_pusch_passive_ul_dmrs_trylock()`/`_unlock()` (same non-blocking trylock-and-skip-this-grant
  pattern as before, just behind an accessor), `nr_pusch_passive_ul_dmrs_stage2_tick()`.
- Cell-wide data-scrambling-ID sweep (`nr_scrambling_id_sweep_t`) + CRC-stalled-window counter —
  `nr_pusch_passive_data_id_current()`/`_feed()`/`nr_pusch_passive_ul_crc_stalled()` (unchanged
  contracts) + new `nr_pusch_passive_ul_crc_note()` (replaces the two direct
  atomic_store/atomic_fetch_add call sites in `nr_pusch_passive_decode.c`).

`nr_pusch_passive_decode.c` no longer owns any of this storage; it calls the new accessors, and
still does the actual estimation (`nr_dmrs_id_init/_accumulate/_decide/_set_range`, needs live IQ)
in place, since that logic legitimately depends on PHY buffers and stays out of the library.
`nr_pusch_passive_decode.h` now `#include`s the new header instead of declaring these functions
itself, so `nr_pdcch_blind_monitor.c` needed **zero changes** (same include, same symbols).

`nr_scrambling_id_sweep.c` itself was **not** moved out of `PHY_NR_UE` — it's also called directly
by the DL side (`nr_pdsch_passive_decode.c`), and several other executable targets link `PHY_NR_UE`
without `nr_pdcch_blind_monitor` (e.g. the various `-nr-XXXsim`/`-uesoftmodem`-adjacent link groups
at CMakeLists.txt lines ~2057-2247); moving it would have broken those. Instead it's compiled
directly into `test_nr_pdcch_blind_monitor`'s own sources, mirroring the existing pattern for
`dci_nr.c`/`refsig.c`/`hashtable.c` on that same target.

No decision logic changed; no test stubs added.

## Build hazard caught before committing
A `sed` used to append `nr_pusch_passive_ul_ids.c` to the `nr_pdcch_blind_monitor`
`add_library()` line matched the literal `nr_pdsch_qm_oracle.c)` suffix on **two other,
unrelated** `add_executable()` lines (`test_nr_pdsch_config_sweep`, `test_nr_pdsch_qm_oracle`),
silently injecting the same source into both and breaking their link (undefined
`nr_scrambling_id_sweep_*`, since those targets never included that file). Caught by the very next
build attempt (`test_nr_pdsch_config_sweep` failed to link) and reverted with a scoped Python
edit before the final `git diff --stat` (3 files, +36/-87 net across the two decode files) was
confirmed clean.

## Verification (lane script, sens6:/home/sens/NICOLA/agn-wt/fix2)
- `nr-uesoftmodem`: builds and links clean.
- `test_nr_pdcch_blind_monitor`: **141/143 passed, 2 skipped, 0 failed**
  (`PdcchReplay.OtaCss0DecoderContract`, `PdcchReplay.BudgetTimingEveryWidth` — pre-existing,
  unrelated skips).
- `ctest -R 'test_nr_pdcch_blind_monitor|test_nr_dl_adaptive|test_nr_pdcch_ul_field_sweep|test_nr_pdcch_ul_interp_sweep'`:
  4/4 passed.
- `ctest -R 'test_nr_dmrs_id_estimate|test_nr_scrambling_id_sweep|test_nr_passive_acq_state|test_nr_pdsch_config_sweep'`:
  4/4 passed (test_nr_pdsch_config_sweep 36.4s, others <1s).

## Files touched
- `CMakeLists.txt` (library source list + test target sources/comment)
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.{c,h}`
- `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_ul_ids.{h,c}` (new)

## Concerns
None outstanding. No receiver was run; no X410 use; build only, via the lane script.
