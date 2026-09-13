# Local DL recovery validation report

Date: 2026-09-11. Project status: INCOMPLETE. All tests are offline; X410 untouched.

## Implementation and falsification

- A settled DL context can reopen local hypothesis search after sustained contradictory CRC evidence.
- Conservative learned CRC bound plus configurable run hysteresis; no cell-specific parameters.
- Generation changes reject old feedback; unrelated contexts and manual parsing remain unchanged.
- The recovery regression fails on the unchanged old controller after 5,000 failures and passes on the new controller. A compatibility-only policy API shim was used to link the old controller.
- Source/binary build: PASS. New controller: 18/18; length sweep: 8/8.
- Blind monitor: 114/117, with the same three pre-existing failures listed below.
- Saved manual oracle: 55/55 byte-identical DL controls; 17 raw UL observations are NOT 17 UL CRC passes.
- Three independent short raw recordings: 3/3 repeated PBCH/MIB plus SIB1 acquisition checks pass.

## Long-capture repeatability

Same 120-second, single-channel recording, same rebuilt binary and replay options.
Both runs reach EOF with no transport fault or clipping.

| Run | Cumulative DL | Cumulative UL | Late DL | Late UL | Recovery events |
|---|---:|---:|---:|---:|---:|
| Run 1 | 35.61% | 44.53% | 76.85% | 45.32% | 0 |
| Run 2, unchanged binary | 16.27% | 60.68% | 76.83% | 93.37% | 0 |

Late means the final ten counter-reporting intervals, not ten seconds. Rates include search/interpretation attempts and use the last cumulative queue snapshot; they are not unique scheduled-TB coverage.
Prior baseline late CRC was 76.90% DL / 77.30% UL. The latest UL observations do not justify a reproducible 60% or 70-80% acceptance claim. Do not select only the best run.
The acquisition measurements and automatic outcomes vary despite identical IQ. The source of end-to-end nondeterminism has not been isolated. This is not evidence of a radio/capture fault.

## Acceptance gates

| Gate | Outcome |
|---|---|
| 1 Manual equivalence | OPEN: manual decoder control passes, generalized auto/manual 1_1 field equality remains unproved |
| 2 Multiple layouts | PARTIAL: previous 1_0 fixture coverage; general 1_1 interpretation remains open |
| 3 Ambiguity | PASS only for previously constructed 1_0 cases; no broader claim |
| 4 Full raw acquisition | PARTIAL: raw PBCH/MIB/SIB1 to scheduling attempts works; generalized validated interpretation is incomplete |
| 5 Reacquisition | PARTIAL: local DL hypothesis loss/change tests pass; raw-IQ interruption and full acquisition FSM are not implemented |
| 6 Invalid rejection | PASS only for previously tested 1_0 scope |
| 7 Regression | OPEN: three pre-existing suite failures and automatic UL repeatability gap |

## Remaining native failures

- `BlindPdcchTest.UlControllerPoolsEvidenceAcrossUesAtTheSameDciLength`
- `UlFieldSweep.NeverInterpretableClassCannotBlockConvergence`
- `UlInterpSweep.FullCatalogueCannotSilentlyBypassClassCap`

## Reproduction and evidence

Evidence root on sens6: `/home/sens/NICOLA/captures/agnostic_recovery_change.AY0slj`.

```sh
root=/home/sens/NICOLA/adaptive-rx-UL-DL
build="$root/cmake_targets/ran_build/build"
cmake --build "$build" --target test_nr_pdsch_config_sweep test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_monitor nr-uesoftmodem -j2
"$build/test_nr_pdsch_config_sweep"
"$build/test_nr_pdcch_dci_length_sweep"
"$build/test_nr_pdcch_blind_monitor"
# File-only replay; supply a new output path.
/home/sens/NICOLA/captures/raw_5min_last120.v51DBe/replay_last120.sh /absolute/new/output --timeout 3600
```

Next highest-value audit: deterministic acquisition/feedback ordering and matched-observation UL interpretation comparison. Do not tune deployment parameters or treat CRC-only winners as fully validated configurations.
