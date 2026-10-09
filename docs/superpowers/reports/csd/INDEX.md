# csd/* report index (2026-10-09)

One line per report under `docs/superpowers/reports/csd/`. Columns: report, branch and commit that hold its latest
version (the branch named is where the file's last commit sits; integration branches such as `csd/chest-fast` carry
many earlier reports by merge), one-line verdict copied or condensed from the report's own status labels.
Evidence labels follow PROJECT_MEMORY §0.1. Nothing here is a new measurement.

## Headline reports (2026-10-07 .. 2026-10-09)

| Report | Branch @ commit | Verdict |
|---|---|---|
| SCANFAST.md | csd/scan-fast @ af1b85f8da | Target unmet: dedicated-only scan p99 1,127 us (baseline 1,200) on raw_pci415_12s_b; 1,687 us (baseline 1,397) on raw_pci415_12s_20261008. 0.66 cutoff rejected (discards admitted DCIs). |
| SCANFAST_COUNTS.json | csd/scan-fast @ af1b85f8da | Raw counts behind SCANFAST.md. |
| CHESTFAST.md | csd/chest-fast @ 1065830474 | <250 us full PDSCH job gate not met; `ISAC_CHEST_STAGE_TIMING` profiler added; FFTW delay shortcut rejected (slower, different delay peak). No fast estimator enabled. |
| T26.md | csd/t26b @ 3458d912f2 | Blind C-RNTI DCI 1_1 and PDSCH on PCI 415 (offline replay). T26c queue-saturation fix (replay). T26d: SNR limit is per-layer SINR of multi-layer grants [INFERRED]; IRC, ML, OCC, LDPC iterations, SOLVER_PREF all null. Recommended OTA env not OTA-validated. |
| SNRAUDIT.md | csd/snr-audit @ a992b52161 | PDSCH post-EQ SNR 6-14 dB is co-channel interference from PCI 415/380/249 [OFFLINE]; front end clean (flat noise on four chains); RX2 fault is ahead of the LNA (cable/antenna). |
| ULDBG.md | csd/ul-dbg @ ce7ce9a877 | UL slots sit at the guard/noise floor at this position [OFFLINE VERIFIED]; PUSCH mapping confirmed by DM-RS control; first PUSCH CRC out of reach here; RX2 dead in capture. |
| RXR.md | csd/rxr @ 7296a4d9e3 | Ring recovery and direct ring read; NOCTRL recovery (no X410 control reads) offline verified; OTA of recovery path still deferred. |
| STARTUPDROPS.md | csd/startup-drops @ 6303219962 | Drops on the 2026-10-08 boot attributed to missing RX-core isolation [INFERRED]; 8/8 zero-drop starts on the 15-19 boot; decisive reproduction (non-isolated producer core) not yet run. |

## Other csd reports (integration and earlier tasks)

| Report | Branch @ commit | Verdict |
|---|---|---|
| T1.md | csd/chest-fast @ c4a3b9d9a4 | U1 slot feed and CRB-aligned ring behind `ISAC_CSIRS_DISC=1`, default off [OFFLINE VERIFIED]. |
| T2.md | csd/chest-fast @ 57c5db78fd | Periodic CSI-RS footprints and synthetic grids (CPU, no CUDA in sandbox). |
| T2b.md | csd/chest-fast @ c7beac4543 | Footprint history beyond the IQ ring, indexed by absolute slot mod 640 [CODE-READ]. |
| T3.md | csd/chest-fast @ 6a7d81fa18 | U3 CSI-RS hypothesis builder. |
| T4.md | csd/chest-fast @ 398a24a345 | U4 scramblingID / slot-offset sweep, gated CPU entry; no run. |
| T5.md | csd/chest-fast @ 22bc572478 | U5 blind CSI-RS discovery wired to U1 ring and U2 detector, `ISAC_CSIRS_DISC=1` [OFFLINE VERIFIED]; no run. |
| T7.md | csd/chest-fast @ 227cf64609 | Recovers all eight periodic resources in the 2026-10-06 PCI 415 ring dump [OFFLINE VERIFIED]; CPU worker budget (50 ms / 100 slots) not met (mean 245 ms). |
| T8.md | csd/chest-fast @ b00bf5874a | Deferred CSI-RS FEP: bit-identical CRB IQ to `nr_slot_fep`; performance acceptance NOT complete. |
| T9.md | csd/chest-fast @ e3349dcd96 | GPU worker FEP and incremental detector [IMPLEMENTED, NOT DEVICE VALIDATED]. |
| T10.md | csd/t10 @ fbb13a319f | Ingestion split from cadence-driven clustering [IMPLEMENTED, NOT DEVICE VALIDATED]; 2000 slots/s GPU gate deferred. |
| T12.md | csd/chest-fast @ 1a3ef15837 | Per-sweep noise-adaptive CSI-RS winner gate; zero false winners in its 60000-outcome check (offline). |
| T13.md | csd/chest-fast @ d85fb530f7 | Pre-stream init and bounded DL GPU storage; CUDA build only, no GPU device in sandbox. |
| T14.md | csd/chest-fast @ cbd8702351 | Canonical CSI-RS admission and period/extent refinement; 5,468-slot dump processed [OFFLINE VERIFIED]. |
| T15.md | csd/chest-fast @ bafcedf0ea | Extra discovered CORESETs per nID; focused 7/7, ctest 151/160 [OFFLINE VERIFIED]; live behaviour not validated. |
| T16.md | csd/chest-fast @ 14c0cc741d | One-worker discovery and asynchronous confirmation; no run. |
| T17.md | csd/chest-fast @ d8f95bfb7b | In-process dedicated PDCCH nID / CORESET discovery; no run. |
| T18.md | csd/chest-fast @ ff988f2e08 | GPU platform and SHARED memory backend; no GPU in sandbox. |
| T19.md | csd/chest-fast @ 10e0c64632 | Multi-symbol dedicated PDCCH nID discovery; no run. |
| T20.md | csd/chest-fast @ 2024588d27 | GPU nID pass parallelism; 13 `test_nr_pdcch_nid_*` pass (host-only); no GPU timing. |
| T21.md | csd/chest-fast @ 4fca5731e9 | IQ lifetime guards and stage histograms for CSI-RS discovery [IMPLEMENTED, NOT VALIDATED]. |
| T22.md | csd/chest-fast @ 3cb42032a7 | Incremental dedicated PDCCH nID discovery; no run. |
| T23.md | csd/chest-fast @ 4256e651c1 | Core-plan solver [CODE-READ]; no run. |
| T23b.md | csd/chest-fast @ 34c62d325d | Dedicated layout reserves numbered affinities before automatic RT roles [CODE-READ]. |
| T24.md | csd/chest-fast @ 56e7de579d | No GPU, receiver or OTA run; see report for the change set. |
| T24b.md | csd/chest-fast @ 95d6561852 | Flag-off CSI-RS CUDA schedule restored [IMPLEMENTED, NOT VALIDATED ON DEVICE]; CPU suite 184 passed, nine ARM/sandbox failures. |
| T25.md | csd/chest-fast @ d639309fa2 | Device parity and performance DEFERRED. |
| T28.md | csd/int2 @ 30e81a1be3 | Two default-off flags [IMPLEMENTED, NOT VALIDATED LIVE]. |
| T29.md | csd/int2 @ 30df40e25a | Ring catch-up pacing, restart-until-sync and ISAC_MLOCK merged onto int2; see report for status. |

## UL reports (csd/ul-*, 2026-10-07 .. 2026-10-08)

| Report | Branch @ commit | Verdict |
|---|---|---|
| ULSCOPE.md | csd/ulscope @ 04d0575bb9 | Read-only: a substantial passive PUSCH receiver exists; first blocker is missing UL BWP knowledge. No demonstrated UL chain. |
| ULA.md | csd/ul-a @ a172cf6da0 | UL pipeline lifecycle and per-UE UL state seeded from DL, behind `ISAC_UL_PIPELINE` / `ISAC_UL_UE_STATE` (default off) [CODE-READ + IMPLEMENTED]. |
| ULB.md | csd/ul-b @ bb90fbbd4e | DCI 0_1 on banked CORESETs and blind UL BWP/layout, behind `ISAC_UL_DCI_BANK` [CODE-READ + IMPLEMENTED]. |
| ULC.md | csd/ul-c @ 388d39099e | PUSCH timing and DM-RS discovery, behind `ISAC_UL_TIMING_DISC` / `ISAC_UL_DMRS_DISC` (default off); CPU reference only. |
| ULR2.md | csd/ul-r2 @ 4ba577a1ec | Blind PUSCH grant solver per DL-locked C-RNTI, behind `ISAC_UL_PUSCH_SOLVER` [CODE-READ + IMPLEMENTED]; no live PUSCH CRC. |
| ULINT.md | csd/ul-int @ dcdc9877db | UL-A/B/C merged onto t26b; records the first-PUSCH replay gate; first nonzero PUSCH TB CRC is the open milestone. |

## Not in the .md index

- `docs/superpowers/reports/csd/*.md` on `csd/docs-1009` also includes the T-series and UL reports above through merges of csd/int3; the table reflects the last commit per file across all csd/* branches.
