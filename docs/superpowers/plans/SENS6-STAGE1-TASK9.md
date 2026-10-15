# sens6 (x86_64, RTX 4070) — Stage 1 Task 9: build, tests, replay

Deferred by the user on 2026-10-15 (sens6 not reachable from the DGX). Run it on sens6 after pulling this branch.
Plan: `docs/superpowers/plans/2026-10-15-stage1-gpu-decode-pass-dl-ul.md`, Task 9. Evidence label for every result: `[MEASURED, sens6 x86_64 RTX 4070 …]`.

## Get the code
```bash
git fetch origin
git checkout csd/gpupass        # or: git worktree add ../rr-csd-gpupass origin/csd/gpupass
git log --oneline -1            # note the commit in every result
```

## Configure and build (cloud-x86 variant + CUDA sm_89)
```bash
cmake -S . -B cmake_targets/ran_build/build_s6 -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=OFF \
  -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
cd cmake_targets/ran_build/build_s6
ninja nr-uesoftmodem rfsimulator params_libconfig nr-softmodem raw_iq pdsch_gpu tests
```
If a root CUDA MPS daemon runs on sens6, prefix every CUDA program/ctest with
`CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/$USER_nomps` (mkdir it first).

## Tests
```bash
ctest -j4 --output-on-failure 2>&1 | tail -40
./test_nr_gpu_pass                      # expect: model DISCRETE, FP32 auto on sm_89 (once Task 5 is in)
ctest -R "test_nr_pdsch_gpu_ldpc|test_nr_pdsch_gpu_ldpc_nc|test_nr_pusch_gpu_ldpc|test_nr_llr_norm"
```
Expected: `nr_gpu_model()` = DISCRETE; GPU LDPC uses the non-cluster kernels (`nr_pdsch_gpu_ldpc_stats` family 2); no new failures beyond the ARM-only known list (which may pass on x86). Record any failure verbatim.

## Replay (needs the captures copied read-only from the DGX)
Copy `ota_runs/raw_pci415_12s_b` and `ota_runs/raw_pci923_tim_12s_tile3` (read-only), then from the repo root:
```bash
tests/passive_rx/gpupass_ab.sh b 6      # exists once Stage 1 Task 8 is merged
tests/passive_rx/gpupass_ab.sh tim 6
```
Pass criteria as Task 8 (bad_* = 0, CRC-OK ON ≥ OFF − 1 %, first pass p99 ≤ 500 µs for ≤ 96 PRB), compared against the sens6 OFF arm; also record H2D/D2H bytes and time per pass from the `GPUPASS` stats line. No OTA from sens6 (user, 2026-10-15).

## Report
Append a "sens6" section to `docs/superpowers/reports/csd/GPUPASS-<date>.md` with host facts (CPU, cores, CUDA/driver, `nvidia-smi -q` summary), the ctest result and the replay tables.
