# Task 1 Report: Commit RNTI-cache and CSI-RS write-back fixes

**Status:** DONE

## Summary
Successfully verified the three modified files, built the project, ran both affected tests (both passed), and committed the changes with the exact message specified in the brief.

## Step 1: Diff Verification
```
git status --short:
M openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c
M openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c
M openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc

git diff --stat:
 openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c   | 18 ++++++++++++--
 .../PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c    | 28 +++++++++++++++++-----
 .../tests/nr_pdsch_config_sweep_test.cc            | 22 +++++++++++++++++
 3 files changed, 60 insertions(+), 8 deletions(-)
```
✓ Exactly three files modified, nothing else.

## Step 2: Build and Test
**Build command:**
```
cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build
make -j12 nr-uesoftmodem test_nr_pdsch_config_sweep test_nr_csirs_blind_synth
```
✓ Build succeeded. Final targets built without errors.

**Test results:**
```
Test project /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build
    Start 24: test_nr_pdsch_config_sweep
1/2 Test #24: test_nr_pdsch_config_sweep .......   Passed   28.55 sec
    Start 26: test_nr_csirs_blind_synth
2/2 Test #26: test_nr_csirs_blind_synth ........   Passed    0.02 sec

100% tests passed, 0 tests failed out of 2
Total Test time (real) =  28.59 sec
```

✓ Both tests passed, including `PdschConfigSweepRntiCache.EvictionProtectsEvidenceFromNoiseChurn`.

## Step 3: Commit
**Commit SHA:** `918582c608`

**Commit message:**
```
Technique D: protect evidence-bearing RNTI contexts from noise churn; CSI-RS: apply the solved scramblingID

rnti_ctx() evicted by pure LRU, so a burst of one-off blind-PDCCH RNTIs evicted the one real RNTI
5x in a 200 s lab run, wiping its prior/observations. Slots with evidence are now evicted only when
every slot has evidence; RNTI_CTX_MAX 16 -> 64. Regression test fails on the old policy.

The IDSWEEP-solved scramblingID was print-only: the candidate kept the PCI-derived id, so the
confirmation path kept scoring the wrong sequence. It is now written back. The periodic status line
printed the fixed 4/3 bar as "null_median"; it now prints null_median().

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
```

✓ Working tree clean after commit.

## Changes Made
1. **nr_pdsch_config_sweep.c**: Evidence-protecting LRU eviction in `rnti_ctx()`, increased `RNTI_CTX_MAX` from 16 to 64.
2. **nr_csirs_blind_rt.c**: Write-back of solved scramblingID into pinned CSI-RS candidate; fixed `null_median` display.
3. **nr_pdsch_config_sweep_test.cc**: Added regression test `EvictionProtectsEvidenceFromNoiseChurn` to verify new eviction policy.

## Concerns
None. All requirements met.
