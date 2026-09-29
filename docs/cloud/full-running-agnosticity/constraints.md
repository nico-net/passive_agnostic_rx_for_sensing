## Global Constraints

- Code lives on **sens6** in `/home/sens/NICOLA/adaptive-rx-UL-DL` (branch `adaptive-rx-UL-DL`). Every command runs via `ssh sens6 "…"`. Below: `R=/home/sens/NICOLA/adaptive-rx-UL-DL`, `B=$R/cmake_targets/ran_build/build` (Makefile generator: `make -j12 <target>`, tests via `ctest -R <name> --output-on-failure`).
- Match OAI style: surrounding brace/indent style, `LOG_I/LOG_W/LOG_E/LOG_A(PHY, …)`, never `printf` in library code, `nr_`/`NR_` prefixes.
- New modules go in `openair1/PHY/NR_UE_TRANSPORT/`, tests in `openair1/PHY/NR_UE_TRANSPORT/tests/`, registered in the root `CMakeLists.txt` inside `if(ENABLE_TESTS)` using the `test_nr_pdcch_joint_solve` block (~line 2548) as the pattern. New library sources are appended to `add_library(nr_pdcch_blind_monitor …)` (~line 1464).
- A change that alters the **order of discovery** is opt-in behind an `ISAC_*` environment knob, default off, so nothing changes silently (`ISAC_AL1_COVER=1`). Bug fixes default on. The Qm oracle defaults on with `ISAC_QM_ORACLE=0` to disable.
- **Never relax a test assertion to make it pass.** If a specified assertion fails, stop and report the measured value.
- `test_nr_csirs_blind_search` has two pre-existing failures (`CsirsBlindEnum.EnumeratesRealConfigurationsOnly`, `CsirsBlindCorrelate.WholeBandMeanDilutesAPartialBandResource`). Task 2 fixes both; until then do not mistake them for regressions.
- Agnosticity: nothing in this plan may seed dedicated parameters from SIB1 or from lab ground truth. The AL1 cover uses only the TS 38.211 mapping rules; ground truth (gNB log) is used for validation only.
- OTA rules (memory): never build during a capture; ≥5 runs per arm; after killing a capture wait 60 s and check `uhd_find_devices` before the next; arm a `Monitor` instead of blocking; resolve the live gNB log from the running process (`/proc/<pid>/fd`), not a fixed path.
- Every commit ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
  ```
  Commit with `ssh sens6 "cd $R && git add <files> && git commit -F -" <<'EOF' … EOF` (stdin passes through ssh).

## Review Focus
## Review Focus

- **The AL1 cover silently drops the true mapping.** A cover that misses one REG set makes that CORESET undiscoverable at AL1. Pinned by Task 4 `CoverUnionEqualsFullUnion` over five real shapes.
- **The cover is larger than the lane loop bound.** Task 5 caps the cover lap at `NR_PDCCH_AL1_MAX_COVER` mappings; a larger cover would skip mappings. Pinned by Task 4 `CoverFitsTheLaneBoundForEveryLegalShape` (all spans 6–270 RB, D=1–3).
- **A false IDSWEEP solve pins the CSI-RS search forever.** Pinned by Task 3 `PinnedCandidateIsServedUntilBudgetThenRoundRobinResumes`.
- **The Qm oracle reads noise as the densest constellation.** A dense grid always fits noise better in raw EVM; a misfire would prune the true MCS table. Pinned by Task 6 `PureNoiseAbstains` and `LowSnr256QamAbstains`.
- **The joint-solver speed-up changes results, not just speed.** Pinned by Task 7 `OptimisationKeepsResultsBitIdentical` (golden hash over 2,400 solves).
- **Segmented decode changes the contiguous path.** Task 9 must leave the one-segment wideband case on the exact old code path; a reviewer checks the guard, not just the tests.
- **Interleaved data order.** PDSCH maps to VRBs in increasing order; REs must be gathered in VRB (data) order, not PRB order. Pinned by Task 8 `SegmentsFollowDataOrderAndPrgBoundaries` and Task 9 `GatherConcatenatesSegmentsInDataOrder`.
- **The data-ID walk burns decodes on a config mismatch.** Task 13 may advance it only for an RNTI whose Technique D context has converged and whose CRC rate is 0 over ≥ 20 TBs.
- **A wrong scrambling decision is silently fatal** (garbage X, like a wrong `csirs_monitor` scramb_id). The DM-RS estimator's margin gate must stay; never apply an undecided ID.

---
