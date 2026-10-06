# Plan — fully blind CSI-RS discovery (rows 1-18), OTA by 2026-10-07 morning

Spec: `docs/superpowers/specs/2026-10-06-csirs-fully-blind-discovery-design.md`. Contract: `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.h`
(owned by the orchestrator; agents may ADD fields/functions with a note in the commit, never change existing semantics).
Base branch `csd/base` (from `ota/x410-dgx-2026-10-06`). Workers fork from `csd/base`; the orchestrator merges.

| Task | Agent | Worktree / branch | Files owned | Depends |
|---|---|---|---|---|
| T1 U1 feed + ring + worker skeleton | gpt-5.6-sol | wt/rr-csd-a / csd/t1 | nr_csirs_disc_feed.c, hook in phy_procedures_nr_ue.c, CMake, tests/nr_csirs_disc_feed_test.cc | — |
| T2 U2 footprint detector (CPU ref + GPU) + synthetic grid generator | gpt-6-astra | wt/rr-csd-b / csd/t2 | nr_csirs_disc_fp.c, nr_csirs_disc_synth.{h,c} (test util), GPU part of nr_csirs_disc_gpu.cu (fp kernels), tests/nr_csirs_disc_fp_test.cc | — |
| T3 U3 hypothesis builder rows 1-18 | gpt-5.6-sol | wt/rr-csd-a / csd/t1 (after T1) | nr_csirs_disc_hyp.c, tests/nr_csirs_disc_hyp_test.cc | — |
| T4 U4 sequence sweep (CPU ref + GPU) | gpt-6-astra | wt/rr-csd-c / csd/t4 | nr_csirs_disc_sweep.c, sweep kernels in nr_csirs_disc_gpu_sweep.cu, tests/nr_csirs_disc_sweep_test.cc | — |
| T5 U5 integration: worker pipeline, append/confirm, extent+4RX scoring, IDSWEEP off, nr_csirs_monitor_add_dynamic, decouple from PDCCH occasions, metrics; end-to-end synthetic test via rt bridge | gpt-5.6-sol (astra if blocked) | wt/rr-csd-a / csd/t5 (from merged T1-T4) | nr_csirs_disc.c (pipeline), nr_csirs_blind_rt.c, nr_csirs_monitor.{h,c}, phy_procedures_nr_ue.c | T1-T4 |
| T6 docs: PROJECT_MEMORY block, env table, report | gpt-5.6-terra | wt/rr-csd-a | PROJECT_MEMORY.md, docs/superpowers/reports/csd/ | T5 |

Gates (orchestrator only, never agents): G-A offline ctest (each task + merged), G-B raw replay of
`/home/nicola/NICOLA/ota_runs/raw_pci421_aligned_20261006` with only `ISAC_CSIRS_DISC=1`, G-C OTA 4 RX < 5 s.

Per-task acceptance:
- T1: ring push copies exactly the CRB-aligned subcarriers (test with synthetic rxdataF incl. wrap at the FFT edge);
  drops counted and never block; hook compiled in but inert unless ISAC_CSIRS_DISC=1; unified alloc when the GPU module
  is present (cudaMallocManaged via the GPU module or plain aligned malloc otherwise).
- T2: synthetic grids for every row 1-18 (via nr_generate_csi_rs mapping), random rb_start/n_rb, period/offset, scramb_id,
  with QPSK PDSCH load on random RBs every slot, PDSCH DM-RS (type A pos2 + additional), SSB block, AWGN at 0..20 dB:
  footprint (sym_mask, sc_mask, density, rb extent ±0, period, offset) recovered; DM-RS/PDSCH not reported. GPU kernel
  output == CPU reference on the same ring (build the GPU module in build_gpu; GPU runtime tests may be DEFERRED).
- T3: for every row 1-18 and every legal (fd bitmap, l0, l1, cdm, density) the builder, fed the exact footprint of that
  resource, returns a hypothesis set containing it (row 9 included); hypothesis count bounded.
- T4: CPU reference recovers scramb_id and slot_delta exactly on synthetic data for rows 1, 2, 4, 6, 11, 16 (≥ 10 dB);
  noise-only z ≈ 1; GPU == CPU (scores within 1e-3, same argmax).
- T5: end-to-end synthetic: rt bridge fed synthetic slots (all rows, mixed) with only ISAC_CSIRS_DISC=1 → CONFIRMED lines with
  correct row/fd/l0/l1/density/rb extent/scramb/period; feature off ⇒ byte-identical behaviour (existing tests).
- T6: documentation only.
