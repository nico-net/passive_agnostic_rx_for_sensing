# R3b — length re-lock evidence after spec 69ae438406

Host: DGX aarch64. Branch: `rr/reconfig-robustness`. Scope: R3b only.

## Changes and decisions

- [OFFLINE VERIFIED] `nr_pdcch_blind_monitor_rt.c` feeds both DL and UL length miss checks from `nr_pdcch_blind_rnti_bootstrap_recent()`. The bootstrap table already records C-RNTI accepts from a decoded CORESET#0 common search space, and the DL 1_1 path records accepted C-RNTIs. No SA/NSA mode gate was found in the R3/R4/R5 paths checked; evidence is consumed when present.
- [IMPLEMENTED, NOT VALIDATED on live reconfiguration] A format 1_0 C-RNTI accept on a known CORESET#0 UE search space now refreshes the bootstrap record when that RNTI is already confirmed. This closes the remaining CORESET#0 accept gap without changing decoding or allowing an unverified UE search hypothesis to bootstrap its own identity. The existing freshness query supplies that record to both DL and UL miss checks.
- [OFFLINE VERIFIED] `tests/nr_pdcch_dci_length_sweep_test.cc` adds `Coreset0AcceptCountsWhenAvailable`: fresh CORESET#0 C-RNTI accept records alone make a locked length SUSPECT after three searched misses. The existing `SuspectWithoutCoreset0Evidence` remains green. `CMakeLists.txt` links the existing bootstrap source into that focused test target. No applicable test or source comment used “NSA-like” for the SIB1 evidence column.

## Validation — DGX aarch64

- [OFFLINE VERIFIED] `CCACHE_DIR=/tmp/rr-r3b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_rnti_bootstrap test_nr_pdcch_blind_monitor` passed. A first placement of the new test in the bootstrap target exposed its existing transitive link stubs; moving the cross-module test to the length target resolved that test-only linkage.
- [OFFLINE VERIFIED] `nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_dci_length_sweep --gtest_brief=1` passed 38/38; `nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_blind_rnti_bootstrap --gtest_brief=1` passed 14/14. The blind-monitor binary also exited successfully with its existing capture-dependent skips.
- [OFFLINE VERIFIED] After `pgrep -x nr-uesoftmodem` returned no process, `CCACHE_DIR=/tmp/rr-r3b-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` passed (`/tmp/rr-r3b-build.log`). One full `nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure` passed 122/131 (`/tmp/rr-r3b-ctest.log`). Nine failures match the documented ARM set (`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`) and sandbox socket set (`time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`).
- [OFFLINE VERIFIED] `git diff --check` and the sens6 frozen gate passed before commit. No receiver, simulator, OTA, or GPU workload ran.

## DEFERRED MEASUREMENTS

[PLANNED] Stable SA rfsim gate in the orchestrator's quiet machine slot; this does not inject a length change:

```bash
cd /home/nicola/NICOLA/wt/rr-robust
if pgrep -x nr-uesoftmodem; then exit 2; fi
env -u ISAC_RX_BRANCH_FO ISAC_RECONF=1 NR_GPU_POLAR=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r3b-sa bash tests/passive_rx/dgx/rfsim_regress.sh 2
```

## Open issues

- [PLANNED] A live length change and a SIB1-less capture or fixture are still needed to measure re-lock latency and confirm the optional-evidence behavior over radio. R12/R13 owns those fixtures; this task has no runnable command for them yet.
