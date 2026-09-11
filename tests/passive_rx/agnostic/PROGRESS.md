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
