# R8c — continuity-loss classification (operator decision 2026-10-02)

Host: DGX Spark (`spark-74c3`, aarch64). Branch: `rr/reconfig-robustness`.

## Changes and decision

- [IMPLEMENTED, NOT VALIDATED on live traffic] `executables/nr-ue.c` passes the signed RF timestamp gap and samples per millisecond from the existing RXDISCONT detector to the epoch authority. Receiver sync invalidation and reacquisition remain unchanged.
- [IMPLEMENTED, NOT VALIDATED on live traffic] `nr_passive_cfg_epoch.{c,h}` classifies a positive gap shorter than `ISAC_RECONF_GAP_HARD_MS` (default 10 ms) as SOFT with cause `CONTINUITY_LOSS`. A gap at or above the threshold, a backwards timestamp jump, or an unknown sample rate is HARD_REVERIFY. The epoch log records gap samples and milliseconds. The old no-duration API remains a hard-classification wrapper for existing callers. All epoch behavior remains gated by `ISAC_RECONF=1`.
- [IMPLEMENTED, NOT VALIDATED on live traffic] This operator decision supersedes the current `SPEC_CURRENT.md` §4.4 continuity row, which says every stream gap is HARD_REVERIFY. Routine UHD overflows should not break the stable-cell target of zero HARD events. Reconfiguration cannot normally complete unnoticed within one frame; MIB, SIB1 and P-RNTI sources still catch rare edge cases.
- [OFFLINE VERIFIED] `nr_passive_cfg_epoch_test.cc` covers a 9 ms SOFT gap, 10 and 50 ms HARD_REVERIFY gaps, an environment override, and the adapted continuity-cause test.

## Validation

- [OFFLINE VERIFIED] Test-first compile failed on the missing duration API: `CCACHE_DIR=/tmp/rr-r8c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_passive_cfg_epoch` (`/tmp/rr-r8c-red.log`). Every build/test invocation was preceded by the orchestration HOLD/BUSY gate.
- [OFFLINE VERIFIED] Focused target build passed: `CCACHE_DIR=/tmp/rr-r8c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_passive_cfg_epoch` (`/tmp/rr-r8c-target-build.log`). The four requested tests passed: `nice -n 19 cmake_targets/ran_build/build/test_nr_passive_cfg_epoch --gtest_filter='CfgEpoch.ShortGapIsSoft:CfgEpoch.LongGapIsHardReverify:CfgEpoch.GapThresholdFromEnv:CfgEpoch.ContinuityLossLoggedAsContinuity' --gtest_brief=1` (4/4).
- [OFFLINE VERIFIED] Required target-list build passed: `CCACHE_DIR=/tmp/rr-r8c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` (`/tmp/rr-r8c-full-build.log`).
- [OFFLINE VERIFIED] One full `nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure`: 126/135 passed (`/tmp/rr-r8c-ctest.log`). The nine failures match the documented ARM intermittent set (`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`) and sandbox-only set (`time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`). The epoch test passed.
- [OFFLINE VERIFIED] `git diff --check` and the sens6 frozen-file gate passed before commit.

## DEFERRED MEASUREMENTS

[PLANNED] Live beds are prohibited in this shared-machine slot. In an orchestrator-approved quiet slot, compare stable-cell flag-off/on behavior and continuity event rates:

```bash
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r8c-off bash tests/passive_rx/dgx/rfsim_regress.sh 2
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r8c-on bash tests/passive_rx/dgx/rfsim_regress.sh 2
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build bash tests/passive_rx/dgx/rfsim_arm.sh /tmp/rr-r8c-stable-3600 3600
rg 'CONFIG_EPOCH|RXDISCONT' /tmp/rr-r8c-stable-3600
```

## Open issues

- [PLANNED] No live UHD overflow duration distribution or stable-cell HARD event rate was measured in this task.
