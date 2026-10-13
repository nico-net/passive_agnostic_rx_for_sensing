You are working on the passive, fully blind, cell-agnostic 5G NR receiver (OAI UE fork) in this repository.
Your task: design and implement the FIRST MULTI-PCI / MULTI-OPERATOR MILESTONES so that one radio stream feeds N independent
cell pipelines (several PCIs, several operators) simultaneously.

READ FIRST, completely: CLAUDE.md; PROJECT_MEMORY.md (the single source of truth; especially §0.1 evidence labels and the
2026-10-10/11, 2026-10-12 and 2026-10-13 blocks in §25); the multi-cell spec on branch csd/mcell-spec if present; and in
docs/superpowers/reports/csd/: AUTOFRONT-20261012, FRONTLIVE-20261013 (blind cell scanner + live chain), GRIDCARRIER-20261013
(N_RB per carrier, retune_restart.sh), CARRIERGEOM-20261012, BLINDPIPE-20261013, DECODETIME-20261012, QUIETTIMING-20261012,
OTATAIL-20261013 and MRCBUG-20261013 (if present), CARRIERS2-20261011.

CONTEXT
- Site cells (n78, 30 kHz, all inside one 122.88 MS/s window only partly): Vodafone PCI 415 (222-10), TIM PCI 923 and 675 (222-01),
  Iliad PCI 403 (222-50, barred, NSA-style SIB1), WindTre PCI 508 (222-88, 162-PRB carrier) and PCI 4 (co-channel with 508).
- Today: one nr-uesoftmodem process per cell, tuned by -C/--ssb/--initial-fo (now produced automatically by fastscan.py +
  live_chain.sh), N_RB fixed per start (-r 51..273), one X410 claim per process, DPDK cores 17-19, uhd_ctrl 15, USRP producer 16.
- Per-cell pipeline state: acquisition (sync, MIB, SIB1, CORESET#0), blind CORESET/nID discovery (GPU), DCI layout solver, PDSCH
  solver/decode (GPU exact path), CSI-RS discovery, UL layout lock, sensing API. Global singletons exist in several modules.
- Compute budget is the hard constraint: per-decode PDSCH p50 ~0.35-0.45 ms, mean ~0.58 ms (replay), on-air p99 3-7 ms driven by
  GPU contention with discovery kernels; N cells multiply load.

MILESTONES (deliver M1-M3; M4 as design only)
M1  IQ fan-out: one radio claim, one ring, N consumers. Either (a) one process hosting N cell contexts, or (b) a shared-memory
    ring published by one radio process and consumed by N receiver processes (raw_iq-like device reading from shm). Pick by
    analysis (state isolation, GPU sharing, failure isolation, effort); document the choice. Each cell gets its own digital
    down-conversion/decimation to its own -C/N_RB from the wideband stream (frequency shift + resample), so cells inside the
    radio bandwidth are served without retuning.
M2  Per-cell context isolation: audit every global/static singleton on the receive path (acquisition state, banks, solver tables,
    caches, CSI-RS discovery, UL lock, metrics, GPU contexts/streams, thread pools, TLS — keep static TLS < 3 MB) and make it
    per-cell (context struct or process isolation). Unit tests proving two cells do not share state.
M3  Cross-cell scheduler: a budget-aware scheduler that shares GPU/CPU among cells (per-cell budget as a PARAMETER, not
    today's numbers), priority to real grants of solved RNTIs, discovery throttled per cell, stale-drop accounting per cell.
M4  (design only) Multi-radio / multi-window: cells outside one 122.88 MHz window, retune policy, multiple radios.

RULES
- Branch from adaptive-rx-UL-DL: `git checkout -b csd/mpci-m1 origin/adaptive-rx-UL-DL` (then m2, m3). Never merge into
  adaptive-rx-UL-DL. git add explicit paths, never -A, never stash. New config files under tests/passive_rx/ use .cfg.
- sens6 is FROZEN: never edit tests/passive_rx/captures/*, tests/passive_rx/*.conf, tests/passive_rx/sens6_host_snapshot_2026-09-30/*.
- New behaviour behind default-off flags; single-cell behaviour must stay byte-identical with the flags off.
- FULLY BLIND: no per-cell constants, nothing tuned on a specific operator.
- AVOID these files (actively edited on the DGX right now): the PDSCH demod/MMSE/MRC numerics (nr_dlsch_demodulation.c,
  nr_pdsch_gpu_demod*), GPU discovery scheduling and NID sweep (nr_pdcch_nid_gpu*, nr_csirs_disc*), the PDCCH dwell code
  (nr_pdcch_blind_monitor*.c dwell/budget parts). If you must touch them, keep the hunk minimal and separate, and say so.
- Cloud host: build the x86 variant per CLAUDE.md (`-DOAI_USRP=OFF`, ninja nr-uesoftmodem rfsimulator params_libconfig
  nr-softmodem tests && ctest -j4). You have no X410, no GB10 GPU and none of the IQ captures: validate with unit tests,
  synthetic IQ (generate multi-cell test signals: two SSBs/carriers at different offsets inside one band) and the rfsimulator;
  label evidence "[MEASURED, cloud x86 ...]" and never claim DGX/OTA results.
- Write reports in docs/superpowers/reports/csd/MPCI-M1-<date>.md etc. with evidence labels, a design section, open risks, and
  the exact steps the DGX orchestrator must run to validate on the real radio (commands, env, expected log lines).
- Commit trailer: end every commit message with the co-author/session lines the harness gives you.

OUTPUT
Per milestone: commits on its branch, unit tests, the report, and a short handover note listing files changed and what must be
validated on the DGX (GPU, X410, OTA).
