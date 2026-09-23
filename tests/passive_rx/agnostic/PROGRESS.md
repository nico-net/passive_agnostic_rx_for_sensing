# Agnostic RX progress and acceptance report

Date: 2026-09-11. Status: INCOMPLETE. Offline development only; X410 not accessed.

## Completed increment

Automatic DCI 1_0 class interpretation now enumerates all enabled class candidates
under the supplied context instead of accepting the first passing class. Ambiguous
allocations are withheld. Every candidate has a protocol result and rejection
reason. Unique plausibility remains UNRESOLVED, not VALIDATED. Manual decoding
keeps its legacy selection. Native runtime uses the new automatic-mode API.

## Measured validation

- Fresh pre-change native build: PASS.
- Pre-change native suite: 110/113 PASS; three pre-existing failures below.
- Post-change native build: PASS.
- Four new native tests: 4/4 PASS, using real polar encoding/decoding.
- Post-change complete native suite: 114/117 PASS; identical failure set.
- Runtime integration: `nr-uesoftmodem` target builds successfully, two jobs.
- Saved-IQ decoder control: PASS, 45/45 DL TBs byte-identical, zero failed controls.
- Replay additionally recovered 15 stored raw UL observations. This is NOT 15
  successful UL decodes and is NOT evidence of autonomous UL convergence.

The synthetic equivalence test covers four frequency-reference sizes (24, 48,
106, 273 PRBs), two TDRA indices, four MCS/RV/HARQ combinations, with independently
packed allocations plus manual comparison. Those contexts are test fixtures, not
runtime inputs permitted to an agnostic receiver. No 1_1 generalization is claimed.

Pre-existing failures, preserved rather than hidden or rewritten to pass:

1. `BlindPdcchTest.UlControllerPoolsEvidenceAcrossUesAtTheSameDciLength`
2. `UlFieldSweep.NeverInterpretableClassCannotBlockConvergence`
3. `UlInterpSweep.FullCatalogueCannotSilentlyBypassClassCap`

Their causes require a separate contract audit. A stale test or an implementation
bug cannot be inferred solely from a test name. Full regression is not green.

## Acceptance gates

| Gate | Outcome | Evidence / remaining requirement |
| --- | --- | --- |
| 1 Manual equivalence | OPEN | Synthetic 1_0 equivalence and one saved-IQ DL control pass; general auto 1_1 equality across all successful stored captures is not implemented/tested |
| 2 Multiple layouts | PARTIAL, NOT PASS | Native 1_0 fixture variations pass; generalized automatic 1_1 layout/configuration combinations remain open |
| 3 Ambiguity | PASS for constructed case | One polar codeword survives both RA and TC interpretations with different PRBs; auto explicitly returns AMBIGUOUS and exports no grant, including when report is NULL |
| 4 Full raw acquisition | NOT RUN | Existing replay injects frame/configuration metadata and already-interpreted PDSCH jobs; its PASS does not count as autonomous acquisition |
| 5 Reacquisition | NOT RUN | No interruption/recovery state-machine integration has been implemented in this increment |
| 6 Invalid rejection | PASS for tested scope | Invalid RA reserved bits are rejected with per-candidate reason; UL direction is not accepted as DL; invalid context clears old diagnostics |
| 7 Regression | OPEN / SUITE FAIL | All previously passing native tests remain passing and one saved-IQ control passes, but three baseline failures and all-capture regression remain unresolved |

Gate 3 and 6 results establish specific native behaviors, not full coverage of
unknown BWP, dedicated TDRA, CSI-RS or 1_1 layouts. Do not declare the project done.

## Evidence locations on sens6

- `/tmp/agnostic-baseline-build.log`
- `/tmp/agnostic-baseline-tests.log`
- `/tmp/agnostic-runtime-build.log`
- `/tmp/agnostic-dci10-validation/focused.xml` and `focused.log`
- `/tmp/agnostic-dci10-validation/regression.xml` and `regression.log`
- `/tmp/agnostic-dci10-validation/replay.log` and `replay-status.txt`
- Original capture: `/home/sens/NICOLA/captures/auto_ul_evidence_retry.G8BYYc/replay.bin`

Temporary evidence is not a durable archive. The original root-owned recording
was read through `sudo cat` into a user-owned scratch file; the decoder itself ran
unprivileged, with replay input set and no USRP arguments. Source inspection
confirmed that this replay path returns before `nrue_init_openair0()`.

## Reproduce the saved-IQ decoder control

This command deliberately supplies capture geometry. It is an oracle regression,
NOT Gate 1 automatic inference and NOT Gate 4 raw acquisition. The recording is a
same-build ABI format; header mismatch, timeout, missing input or nonidentical
control means VOID and its downstream metrics must not be interpreted.

```sh
build="$PWD/cmake_targets/ran_build/build"
input=/tmp/agnostic-dci10-validation/replay-input.bin
# Use an existing readable replay file; never substitute a live capture launcher.
timeout 35s nice -n 10 env \
  -u ISAC_PASSIVE_REPLAY_CAPTURE \
  -u ISAC_PASSIVE_REPLAY_UL_PROBE \
  -u ISAC_PASSIVE_REPLAY_UL_CONFIG \
  ISAC_PASSIVE_REPLAY_INPUT="$input" ISAC_SYNC_ONLY=1 \
  LD_LIBRARY_PATH="$build:${LD_LIBRARY_PATH:-}" \
  "$build/nr-uesoftmodem" --passive-rx --sa \
  -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
  --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --thread-pool -1
```

## Next highest-value implementation

Extend the explicit hypothesis/evidence contract to 1_1 across configuration
contexts, rather than expanding the fixed layout family and declaring a CRC
winner to be the cell configuration. Preserve missing/unsupported domains and
feed independently checked PDSCH mapping, coding and temporal evidence. Then
connect the same generation-aware state to safe raw replay acquisition/recovery.

See `ARCHITECTURE.md` for remaining state, CSI-RS and OTA design requirements.

### Additional raw baselines and reference SSB checker

- Fixed empty-window FFT correlation normalization; retained failing synthetic
  case and added explicit zero-energy/bounded-score regression tests.
- Five SSB checker tests pass. Two further independent four-second, four-channel
  captures saved under `/home/sens/NICOLA/captures/raw_batch.vKkQwC/`; both pass
  timestamp continuity, sample-count and NIC-loss checks. Radio released.
- First 40 ms of channel 2 in all three four-second captures show repeated PCI 2
  PSS/SSS at approximately 20 ms spacing using an explicit frequency reference.
- PBCH/CSI-RS/full DL-UL coverage and raw full-receiver replay remain unvalidated;
  no new acceptance gate is claimed. Details: `../raw_baseline/SSB_VALIDATION.md`.

## Current offline baseline and local recovery increment (2026-09-11)

The earlier raw-acquisition NOT RUN entry is historical: the file-only adapter
now acquires repeated PBCH/MIB and SIB1 from three short raw captures and the
120-second single-channel baseline, then attempts DL/UL decoding without injected
PCI, CFO, SSB location, BWP or dedicated scheduling configuration. This is PARTIAL
Gate 4 evidence, not proof of generalized DCI interpretation or full gate closure.

The long capture is `captures/raw_5min_last120.v51DBe/capture_120s` on sens6.
Its replay `captures/crc_last120_mlockfix_20260911T125339Z` reached EOF without
transport faults: final cumulative DL/UL CRC 27.14%/30.04%; final ten reporting
intervals 76.90%/77.30%. Counter intervals are not asserted equal-time windows.
Raw replay memory locking was fixed and tested against the pre-fix failure.
All further development uses recordings; no radio access in this increment.

Local DL hypothesis relearning now invalidates a settled configuration after
sustained contradictory CRC evidence, preserving its legal catalog and rejecting
old-generation feedback. Other contexts and manual mode are unchanged. Gate 5
remains OPEN: controller recovery is not raw-IQ loss/reacquisition. Gate 1 and the
remaining 1_1/generalization work remain OPEN. Gate 7 must still report the known
three blind-monitor failures rather than conceal them. See RECOVERY_VALIDATION.md
for the generated build, unit-test and replay outcomes of this increment.
