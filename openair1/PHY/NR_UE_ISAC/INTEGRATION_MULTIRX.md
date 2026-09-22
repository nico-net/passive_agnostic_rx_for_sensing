# Multi-RX CLEAN detector + multistatic tracker — integration note (2026-09-22)

## What this branch lands

The multi-receiver passive sensing chain that the Sionna macro campaign was validated on
(`/data/sionna/macro_campaign_20260920`). Until now it existed only as an out-of-tree working copy
(`/data/sionna/isac_microdoppler_src`) and was **not under version control in any repository**.

New sources (no counterpart in the previous tree):

| file | role |
|---|---|
| `clean_detector.cc/.h` | CLEAN detector: RD likelihood map, OS/GO-CFAR, sub-cell refinement, component collapse, micro-Doppler family tagging |
| `causal_clutter_filter.cc/.h` | per-family, per-subcarrier causal clutter cancellation |
| `multistatic_imm_tracker.cc/.h`, `multistatic_evidence.inc` | multistatic IMM tracker |
| `multipath_fusion.cc/.h`, `origin_hypothesis.cc/.h` | path-origin handling |
| `ul_dtd_dfs_validator.cc/.h` | UL DTD/DFS validation |
| `birth_map_evidence.h`, `existence_evidence.h`, `explanation_evidence.h`, `lifecycle_evidence.h` | evidence accumulators |
| `tools/` | offline stages 8-10: extended-object families, multistatic solve, state tracker, MHT variant, UE localiser |

`detector.cc`/`detector.h` (single-RX lineage) are **removed**: they are superseded by
`clean_detector.cc` and no longer compile against the multi-RX `PipelineConfig`
(they reference `maximum_path_doppler_hz`, which does not exist there).

## Divergence that still needs reconciling

`sensing_engine.cc/.h`, `report_writer.cc/.h`, `pipeline_types.h`, `detector_cuda.*` and
`sync_correction.*` existed in both lineages and were **overwritten** with the multi-RX versions.
The previous (X410 receiver) lineage carried features that this version does not:

* `branch_id` / `NR_ISAC_BRANCH_NONE` — per-receive-branch row tagging and routing (P10a).

Anything else that the X410 multi-branch work added to those five files after the two lineages split
is likewise not present here. Reconcile before using this branch on the X410 path; it is safe for
the four-receiver passive/Sionna path it was validated on.

## Configuration defaults adopted in this branch

Set as defaults after measured A/B on frozen captures (all off previously):

| flag | effect |
|---|---|
| `NR_ISAC_MULTI_DWELL=1` | nested 150/300 ms coherent dwells beside the 75 ms CPI |
| `NR_ISAC_FAMILY_MAX_RANGE_BINS=2` | family tagging radius bound |
| `STAGE8_MULTI_DWELL=1` | long-dwell components injected into the midpoint CPI families |
| `STAGE9_HEIGHT_MODEL=1` | ground/air two-hypothesis height model with the identifiability rule |
| `STAGE9_SEEDED_DLUL=dl` | carry DL-only fits (required for continuity in DL-only scenes) |
| `STAGE9_CARRY_RETIRE=1` | evidence-based carry retirement (see below) |

Measured, left **off** (each recorded because the negative result is the useful part):

| flag | measured outcome |
|---|---|
| `NR_ISAC_UL_DWELL` + `STAGE8_UL_DWELL` | UL leg 0 -> 973 detections, person recall 91 % near the UE, but only 27-45 % GT-consistent; end-to-end precision 47 % -> 33 % |
| `NR_ISAC_RANGE_WALK` | +15 % long-dwell detections at equal purity; end-to-end neutral to negative |
| `NR_ISAC_SKIRT_SIDELOBE` | both variants rejected: v1 flagged no skirts at all (detections 780 -> 8384, precision 59 % -> 25 %); v2 recovered +24 true detections for ~3800 false ones |
| `STAGE8_DWELL_AUTHORITY` | large win on car+bike (precision 38 % -> 83 %), regression on four_classes (car 49 % -> 41 %) |
| `STAGE9_TEMPORAL` | 2-RX MAP continuation gated on innovation chi-square: car +2 points at equal precision, runner precision 82 % -> 44 % |
| `STAGE9_MIN_RX_GROUND=2` | 2-RX exactly-determined ground births quadrupled false states (26 -> 99) |

### `STAGE9_CARRY_RETIRE`

A carried object used to be retired only by elapsed time (2 s). Nothing tested whether it was still
supported, so a stale prediction kept claiming blocks with a widening gate -- worst exactly when the
target sits in the +-0.57 m/s clutter notch. Retirement now also requires evidence: drop after
`STAGE9_CARRY_MAX_UNSUPPORTED` (3) consecutive CPIs with no gated update, or when the predicted
position sigma exceeds the association gate (`STAGE9_CARRY_MAX_SIGMA_M`, one range cell).

Measured (frozen captures, coverage over the scoreable window):

| scene | before | after |
|---|---|---|
| car + bike (car) | 54.7 % cov / 37.8 % precision | **87.8 % / 86.9 %** |
| runner | 64.0 % / 81.7 % | **68.3 % / 88.8 %** |
| person + bike (detectable) | 50.9 % / 81.8 % | **57.6 % / 92.0 %** |
| four classes | 51.1 % / 85.7 % | 47.5 % / **92.8 %** |
| car (single object) | 77.0 % / 75.9 % | **57.6 % / 56.7 % — REGRESSION, not yet diagnosed** |

The single-object `car` regression is open: DL-only carry drifts while the car crosses the bistatic
blind zone. Reverting `STAGE9_SEEDED_DLUL` to `1` disables carrying (and with it the gains above).

## Status, honestly

Validated per class on the macro campaign (coverage over the scoreable window, confirmed precision):
person 80.6 % / 100 %, runner 68.3 % / 88.8 %, car 57.6 % / 56.7 %, bike 8.6 %, drone 0-19 %.
Only the person class meets the project's 85 % precision floor with useful coverage. The bike and
drone failures are measured as aspect-driven RCS fluctuation (20-40 dB swings) plus, for the bike,
a class model that is a bare frame with no rider. See `MULTISTATIC_DETECTOR_OVERVIEW.md` in the
sionna-rk experiment tree for the full stage-by-stage evidence.

## Build

`NR_UE_ISAC_SRC` in the top-level `CMakeLists.txt` lists the new sources. CUDA sources
(`detector_cuda.cu`, `family_processing_cuda.cu`, `sync_correction_cuda.cu`) build under
`ENABLE_CHANNEL_SIM_CUDA`; `detector_cuda_stub.cc` replaces them otherwise. The sensing pipeline
remains behind `ENABLE_ISAC_SENSING` (default OFF = passive receiver only).

Every C++ source in this directory passes `g++ -std=c++17 -fsyntax-only` in place
(`nr_isac.cc` excepted: it needs the OAI include paths). A full OAI build has NOT been run on this
branch.
