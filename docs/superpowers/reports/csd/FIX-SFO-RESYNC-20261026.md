# FIX-SFO-RESYNC 2026-10-26 (branch fix/sfo-resync, from adaptive-rx-UL-DL @ 57df6abb85)

## Problem [CODE-READ]
The SFO estimate (openair1/PHY/MODULATION/nr_sfo_state.{c,h}) is relative to the sample clock of the gNB currently tracked. It was one
process-wide state whose only reset was a test hook. After a resync to another cell, a timing rebase or a stream loss it kept the old
eps (window of 256 frames + DM-RS fine term), so the window mixed frames from before and after.

## Change
- nr_sfo_state.c: all state moved into one struct `nr_sfo_state_t g_st` (single instance; macros keep the code unchanged). New
  `nr_sfo_resync(reason)`: `nr_sfo_reset()` (window, published eps -> 0, `g_dmrs_corr` -> 0, counters) plus
  `LOG_I "SENSING: SFO reset (<reason>); previous eps ..."`. `nr_sfo_reset()` stays silent for tests.
- executables/nr-ue.c, reset points (every point where timing is lost or re-acquired):
  1. UE_synch() success, just before `is_synchronized = 1` ("initial sync"): every (re)acquisition, including cell change, scan confirm
     passes and auto-anchor re-syncs, goes through here. Frame reads are only noted while is_synchronized, so no race with the reset.
  2. applied coarse timing rebase ("timing rebase").
  3. RXDISCONT invalidation ("stream discontinuity").
  4. RFSTALL reacquisition ("RF stall / PBCH lock lost").
  5. handle_sync_req_from_mac (MAC-requested cell/frequency change, "MAC sync request").
  The per-frame timing correction (`nr_sfo_note_frame_read`) does not reset; its existing >100 ppm-of-a-frame guard still rejects jumps.
- Consumers:
  - slot_fep_nr.c (`nr_sfo_eps_q_for_fep`, per symbol, per-thread pin): unchanged. [CODE-READ]
  - nr_pdsch_passive_decode.c GPU pass: `t_gp_sfo_q` and the pass's own eps were TWO separate reads of the published value (a publish or
    reset between them made `gp_sfo_same()` wrong). Now one read, pinned around `nr_gpu_pass_dl_run`; nr_gpu_pass_dl.c:617 reads
    `nr_sfo_eps_q_for_fep()` (the pin). Shadow comparisons (`gp_sfo_same`) then correctly skip a grant whose pass ran at the old eps and
    whose old-path run happens after a reset.
  - nr_pdsch_gpu_exact.c:297: reads `nr_sfo_eps_q_for_fep()` (same value as before when unpinned).
  - nr_pusch_passive_decode.c:1872: one read of `nr_sfo_eps_q_for_fep()` into the 14-entry table for the whole grant (CPU and GPU UL
    share it): already self-consistent. [CODE-READ]

## Known residuals (not fixed, small)
- The CPU DL FEP reads the eps per symbol; a slot whose symbols straddle a reset (or a hysteresis publish, pre-existing) can use two
  values. Only one slot per epoch change; a reset happens while not synced (initial sync, discontinuity, stall) or at the frame
  boundary (rebase). [CODE-READ]
- A DM-RS residual from a grant decoded across the reset is added to the new epoch (<= 1 % of the residual per grant, clamped 1 ppm).
- After a reset the correction is 0 for >= 64 frames (0.64 s) until the window is publishable: wide 256QAM grants in that interval are
  uncorrected. The rebase reset costs this on the same cell where eps would have remained valid; if the OTA A/B shows a CRC dip after
  "REBASE", drop that one call (same clock, same cell). [HYPOTHESIS]

## Design note: multi-PCI single process (mpci-main, common/utils/nr_cell.h, NOT implemented)
Each cell's gNB has its own clock, so eps and its window are per cell.
- Per-cell slot: the whole `nr_sfo_state_t` (eps_q, mutex, win/n/w/sum/spf, dmrs_corr, frames, dmrs_n): `g_st` becomes
  `nr_sfo_state_t g_st[NR_CELL_STATIC]` (or the per-cell context struct). Thread-local `t_pin` stays global (it is a value, not cell state).
- Reader resolves the cell: `nr_sfo_eps_q_for_fep()` / `nr_sfo_eps_q()` / `nr_sfo_eps()` use `nr_cell_idx()` (TLS cell index; FEP,
  decode and UL worker threads are bound to a cell). Call sites that pass a value across threads (GPU job `j->sfo_q`, `t_gp_sfo_q`)
  already carry the number, so they keep working; `gp_sfo_same()` must compare against the same cell's eps.
- Writers: `nr_sfo_note_frame_read(cell, extra, spf)` from that cell's timing loop (explicit cell argument, since the per-cell tracking
  loop may not run on the cell's TLS), `nr_sfo_note_dmrs_residual(cell, ...)` from the decode (TLS cell).
- Reset hook becomes `nr_sfo_resync(cell, reason)`, called at that cell's initial sync / rebase / discontinuity / MAC sync request;
  the log line gains `cell=<idx> pci=<n>`. A resync of cell A must not touch cell B. Process-wide stream loss (shared radio, RXDISCONT,
  RFSTALL) resets all cells.
- Caveat: with one radio and one receive clock, all cells share the receiver clock, so each cell's eps differs only by the gNB clocks
  (ppm-level apart); per-cell windows are still right because the timing loop that produces `extra` is per cell. [HYPOTHESIS]

## STOP result
- `ninja -k 0 -j6 nr-uesoftmodem nr-softmodem tests`: 0 failures, both executables link [OFFLINE VERIFIED, DGX GB10 aarch64 build].
- `ctest -R 'sfo|fep|pdsch|pusch|gpu_pass' -j1`: 68 tests; test_nr_sfo (incl. new Sfo.ResyncStartsFromZero, Sfo.PinSurvivesResync) pass.
  Failures: test_nr_pusch_ra0_qam64/qam256 (known ARM list); test_nr_pusch_uci_* x6 "Not Run" (executables not built by the `tests`
  target, unrelated to this change) [OFFLINE VERIFIED, DGX GB10 aarch64 unit].
- New tests: drift A, resync, drift B converges to B with no A or DM-RS contribution; normal timing steps (-3, +40 samples) do not
  reset; DM-RS term cleared; a pinned value survives the reset. Function hooks only, no env.
- Not tested: the receiver itself (no runs allowed). The rfsim gate should check that the log shows exactly one "SFO reset (initial sync)"
  per acquisition and no "SFO reset" in steady state. rfsim has eps 0, so it is unaffected by construction.
