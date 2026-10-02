# R2c — R2b review follow-up

Host: DGX aarch64 (`spark-74c3`). Branch: `rr/reconfig-robustness`.

## Changes and decisions

- [OFFLINE VERIFIED] `nr_pdcch_dci_length_sweep.{c,h}` retains the 30..63 cold range until every stage-1 length has at least 256 trials and a completed round still has no lock. Seen lengths above 63, `ISAC_DCI_LEN_MAX`, and SUSPECT re-lock keep their existing wide-range paths. The 100-bit lock fixture now supplies its seen-length hint explicitly.
- [IMPLEMENTED, NOT VALIDATED on GPU] `nr_pdcch_blind_monitor_rt.c` passes each lookahead lane's active maximum to `lane_batch_add`, matching primary DL/UL prefill and the CPU sweep.
- [OFFLINE VERIFIED] The seen-length order and runtime reconf gate use one `pthread_once`-cached `ISAC_RECONF` parse (`atoi(e) == 1`). `nr_polar_defs.h` asserts a polar entry is busy before returning it to its free list.
- [OFFLINE VERIFIED] `nr_pdcch_dci_length_sweep_test.cc` checks that a cold round remains narrow through stride and budget resumes, then widens only after all 34 stage-1 lengths reach 256 trials.

## Validation

- [OFFLINE VERIFIED] Red test: `CCACHE_DIR=/tmp/rr-r2c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_pdcch_dci_length_sweep` built; `nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_dci_length_sweep --gtest_filter='*ColdRange*:*ColdRound*'` failed 2/2 against the old widening rule.
- [OFFLINE VERIFIED] Target build: `CCACHE_DIR=/tmp/rr-r2c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_monitor` passed. `nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_dci_length_sweep` passed 37/37; `nice -n 19 cmake_targets/ran_build/build/test_nr_pdcch_blind_monitor` passed 206/208, with two existing capture-dependent skips. Logs: `/tmp/rr-r2c-length.log`, `/tmp/rr-r2c-blind.log`.
- [OFFLINE VERIFIED] Required build passed 340/340 steps: `CCACHE_DIR=/tmp/rr-r2c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` (`/tmp/rr-r2c-full-build.log`). `pgrep -x nr-uesoftmodem` found no receiver before builds.
- [OFFLINE VERIFIED] One full `nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure` run passed 122/130 (`/tmp/rr-r2c-ctest.log`). Failures are known ARM `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation` and known sandbox `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. No unlisted failures.
- [OFFLINE VERIFIED] `git diff --check` and the sens6 frozen gate passed before commit. `ISAC_RX_BRANCH_FO` stayed unset. No radio, simulator campaign, or GPU workload ran.

## DEFERRED MEASUREMENTS

[PLANNED] Orchestrator quiet-machine CPU gate; compare first C-RNTI to bank-add and CRC/drop rates against the R2b baseline:

```bash
cd /home/nicola/NICOLA/wt/rr-robust
if pgrep -x nr-uesoftmodem; then exit 2; fi
env -u ISAC_RX_BRANCH_FO -u NR_GPU_FEP ISAC_RECONF=0 NR_GPU_POLAR=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r2c-sa-off bash tests/passive_rx/dgx/rfsim_regress.sh 2
env -u ISAC_RX_BRANCH_FO -u NR_GPU_FEP ISAC_RECONF=1 NR_GPU_POLAR=0 BUILD=/home/nicola/NICOLA/wt/rr-robust/cmake_targets/ran_build/build GATE_CRC_MIN=98.0 GATE_DROP_MAX=1.0 OUT=/tmp/rr-r2c-sa-on bash tests/passive_rx/dgx/rfsim_regress.sh 2
```

[PLANNED] Orchestrator GPU slot for lookahead-batch compatibility:

```bash
cd /home/nicola/NICOLA/wt/rr-robust
if pgrep -x nr-uesoftmodem; then exit 2; fi
cmake -S . -B cmake_targets/ran_build/build_gpu_r2c -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=ON -DOAI_USRP=ON -DENABLE_LDPC_CUDA=ON -DLDPC_CUDA_ARCH=121 -DCMAKE_CUDA_ARCHITECTURES=121 -DCPM_SOURCE_CACHE=/tmp/rr-r1-cpm
CCACHE_DIR=/tmp/rr-r2c-ccache nice -n 19 ninja -C cmake_targets/ran_build/build_gpu_r2c -j8 nr-uesoftmodem polar_gpu nr_polar_sc_cuda_test
nice -n 19 cmake_targets/ran_build/build_gpu_r2c/nr_polar_sc_cuda_test quick
```

## Open issues / limits

- [IMPLEMENTED, NOT VALIDATED on live reconfiguration] The lane fix is compile-verified but has not been exercised by a GPU or live lookahead sweep.
