# PERPATH-FEP-20261024: per-path GPU/CPU choice and GPU FEP at every power-of-two FFT size (sens6)

Branch `sens6/perpath` (worktree `agn-wt/s6-perpath`), base `sens6/integ` @ `1d497f7b3a`. Commits: `45dc971d9e` (selection layer),
`35be6a6349` (FEP sizes), `3a1696d9be` (wiring), `d282de15dc` (test alignment fix). Host: sens6 only (x86_64, RTX 4060 Ti cc 8.9, CUDA 12.4,
gcc 15). No DGX run and no OTA run. Labels follow PROJECT_MEMORY §0.1. Every measurement here is `[MEASURED, sens6 …]`, tier DISCRETE, and is never pooled with GB10 numbers.

## 1. What changed

### (a) Per-path choice (`openair1/PHY/TOOLS/nr_gpu_path.{c,h}`, in `libnr_gpu_platform`)
- **Paths:** `fep`, `chest`, `demod`, `ldpc_dl`, `ldpc_ul`, `pass_dl`, `pass_ul`, `ul_fep`.
- **Rules:**
  - SHARED tier: GPU, with no measurement.
  - DISCRETE tier: the path is measured, then decided.
  - CPU tier: CPU.
  - Fail closed to CPU on `bench_fail`, `gpu_error`, `trial_incomplete` (32 × n picks without a decision), or `direct_only` (no DISCRETE variant).
- **Logging:** one line per path, `SENSING: GPU_PATH path=<p> tier=<t> choice=gpu|cpu reason=<r> …`. A path that is in an online trial logs `choice=trial` at start and one final line when the trial ends.
- **Environment:**
  - `NR_GPU_PATH_SELECT=auto|bench|off`. `off` restores the old behaviour. `bench` also measures on SHARED.
  - `NR_GPU_PATH_<NAME>=gpu|cpu|auto`.
  - `NR_GPU_PATH_TRIAL` (default 8 samples per side).
  - `NR_GPU_PATH_MARGIN` (default 1.0: the GPU wins only if strictly faster).
- **How each path is measured:**
  - **FEP:** a timed self-benchmark at warm-up. One synthetic slot (14 symbols, all antennas) runs through the GPU staged entry and through `nr_slot_fep_ant`. It does 2 warm-up runs and 8 timed runs and takes the medians. The CPU time is per antenna, because the decoder runs one FEP task per antenna (this is the CPU's best case). Every GPU output is compared bit for bit with the CPU output; a mismatch is `bench_fail`.
    - The FEP choice is now separate from the module gate (`g_fep_on`). A CPU choice does not switch off the chest/delay entries or the graph-pass warm-up.
  - **LDPC DL/UL:** a paired online trial. The first TBs run in the existing check mode, where both decoders decode the same TB and are timed. A GPU refusal costs GPU time + CPU time. Hard errors (init, stream, sentinel) give `gpu_error`. The decoded result is the CPU one throughout, so it is unchanged.
  - **GPUPASS DL/UL:** an alternating online trial on eligible grants. The sample is the decode wall time per 1000 LLRs; layout probes are excluded. There is a new pass gate, `path`.
  - **Standalone chest/demod/UL FEP:** these entries read caller memory in place, so they are SHARED-only and log `direct_only` (CPU) on DISCRETE.
- **DGX GB10 invariant:**
  - On SHARED, `nr_gpu_path_policy_begin` returns GPU before any benchmark or trial. `pick()` always returns 1 and samples are ignored.
  - The NEON FEP kernel keeps the compile-time N = 4096 (`Nn = N` when the profile is NEON).
  - This is tested by `Path.SharedTierGb10Unchanged` on a mocked GB10 device `[OFFLINE VERIFIED, sens6 unit test]`. It was **not run on the DGX** `[NOT MEASURED]`.

### (b) GPU FEP at all FFT sizes
- **Before** (`[CODE-READ]`): `nr_pdsch_gpu_exact_warmup` and `allocate_init` accepted only N = 4096, so 106/162 PRB (2048) and 51 PRB (1024) used the CPU FEP.
- **Now:**
  - `k_fep_exact` takes the FFT size for the x86 split-radix twin.
  - Tables are built per size.
  - A per-size self-test against the host `dft(N)` runs at init.
  - New export `nr_pdsch_gpu_exact_ready()`.
- **Sizes covered:** every power of two from 256 to 4096. The receiver's cells without 3/4 sampling use 512, 1024, 2048 and 4096.
- **Not covered:** the 3/4-sampling sizes 768, 1536 and 3072. On the CPU these are radix-3 over a strided split radix, and there is no GPU twin. They are refused and use the CPU FEP.
- **Still N = 4096 only:** delay, chest, graph-pass FEP and UL FEP, which have guards. On aarch64 (NEON) only 4096 runs, so the DGX is unchanged.

## 2. Tests

| item | result | label |
|---|---|---|
| `test_nr_gpu_path` (new) | 14/14 pass (policy table, fail-closed verdicts, median, GB10 SHARED invariant on a mocked device, DISCRETE bench and online trial, catch-up pick, trial_incomplete, gpu_error, env overrides, concurrent samples) | `[OFFLINE VERIFIED, sens6]` |
| `test_nr_pdsch_gpu_exact_size_{512,1024,2048,4096}` (new) | bit-identical to the CPU FEP (`dft` + `apply_nr_rotation_symbol_RX`): 1120 (antenna, symbol) rows per size, 0 differ; amplitudes 40..12000, slots 1-4; per-size twin self-test "bit-identical" | `[OFFLINE VERIFIED, sens6 x86 discrete GPU, @d282de15dc]` |
| `test_nr_pdsch_gpu_exact_size_{768,1536,3072}` (new) | refused as designed: `ready=0` and the FEP call returns -1 | `[OFFLINE VERIFIED, sens6]` |
| full `ctest -j1` (4 GB capped scope), 303 tests | 280 passed, 3 failed (known sens6 failures: `nr_td_sim_test`, which hung as a zombie and stalled ctest, so the run was resumed from test 140; `test_nr_ssb_rate_match_prod`; `test_vrtsim_cirdb`), 5 skipped (`test_nr_pusch_gpu_ldpc_cpu`, `test_nr_gpu_green`, `test_nr_gpu_pass_deq_direct`, `nr_polar_sc_cuda_test_cpu`, `test_nr_pusch_gpu_fep`), 15 disabled | `[MEASURED, sens6 ctest, @3a1696d9be + d282de15dc]` |

Two failures in the first pass were resolved on re-run:
- `test_nr_td_cb0_batch` and `_discrete` failed once and passed when re-run alone. The same 4 sub-tests also fail on a binary built at `1d497f7b3a` (`s6-coreplan/build_cp`) when run at the same time, so this is GPU contention and not this branch.
- `test_nr_pdsch_gpu_exact_size_512` and `_4096` aborted under ctest on the `oai_dfts.c` 64-byte alignment assert. That was a test bug, fixed in `d282de15dc`; all 7 size tests pass after the fix.

## 3. rfsim

**273 PRB rank 4, 4 RX (B2 bed: `gnb.sa.rfsim.100mhz.rank4.conf`, `-m 25 -n 1 -M 273 -l 4`, `ue.passive.pin49r4.100mhz.conf`, `-C 3750000000 -r 273 --ssb 1478`), @`3a1696d9be`, started 17:47.**

The receiver was **OOM-killed after ~14 s**. It had been started without a capped cgroup scope, and the kill stopped the tmux scope and the user's session. It is the same OOM as T9, and the cause is not this branch: the GrantWork LLR pool at 273 PRB, which is being fixed on `sens6/ondemand`. Since then every run and build uses `systemd-run --user --scope -p MemoryMax=… choom -n 1000`.

Lines logged before the kill `[MEASURED, sens6 rfsim 273 PRB 4 RX rank 4, DISCRETE]`:
- `GPU_PATH path=fep tier=discrete choice=cpu reason=slower mode=bench gpu_us=491.74 cpu_us=79.25 n_gpu=8 n_cpu=8 N=4096 nb_rx=4 sym=14 cpu=per_antenna`
  - The GPU staged FEP of one 4-RX slot (H2D window, kernels, D2H, host copy) took 492 µs, against 79 µs per antenna on the CPU (about 317 µs for all 4 antennas serially). The GPU loses even against the serial CPU time.
- `GPU_PATH path=chest … choice=cpu reason=direct_only`; `path=demod … reason=direct_only`; `path=ldpc_dl … choice=trial mode=online`.
- `GPUPASS enabled: 704 shape classes (+2014 segmented) x 7 instances … DISCRETE`.
- No grant was decoded before the kill, so the LDPC and pass trials have **no verdict**, and there is **no CRC** to compare. T9 itself has no CRC number either (every arm was OOM-killed). The only 273-PRB rank-4 reference is §4.3 B2 of SENS6-PORTABILITY (CPU decode, `@4a94af8c0b`): 3315/3315, CRC 100 %.

**106 PRB 4-RX verification arm: NOT RUN.** The new rule says start only if MemAvailable ≥ 8 GB. On the idle sens6 host MemAvailable peaks at about 7.2-7.7 GB, because 4 × 1 GiB hugepages are reserved (`HugePages_Total=4`, all free), so the rule cannot be met `[MEASURED, sens6, 19:00]`. Command for when the rule allows it (under the lock and a 7G scope):

```
flock -x /tmp/td_measure.lock systemd-run --user --scope --quiet -p MemoryMax=7G -p MemorySwapMax=1G choom -n 1000 -- \
  env BUILD=<s6-perpath>/build OUT=<dir> tests/passive_rx/dgx/rfsim_regress.sh 1
```
Then score it with `score_rx_solver.py`. At 106 PRB, N = 2048, so the run would also exercise (b): the FEP bench at N = 2048. GPUPASS stays off at 106 PRB (N = 4096 only).

## 4. Not done / open
- No live verdict yet for the LDPC DL/UL and GPUPASS DL/UL online trials `[IMPLEMENTED, NOT VALIDATED]`. Their logic is unit-tested only.
- The 3/4-sampling FFT sizes (768/1536/3072) have no GPU twin `[PLANNED]`. A radix-3 x86 twin (`radix_3_fft_c16_scaled_strided` + `dft_split_radix_pure_simd_strided`) would be needed. The receiver's launchers do not use `-E` (`[CODE-READ]`: no `-E` in the dgx/autofront launchers).
- The NEON twin is still 4096-only, so on the DGX the GPU FEP is unchanged at the other sizes.
- The DGX regression of this branch (GB10 SHARED unchanged) has not been run `[NOT MEASURED]`.
- **Merge notes:**
  - `sens6/ondemand` adds `nr_host_membudget.c` to every target that compiles `nr_gpu_platform.c`. After merging it, add `nr_host_membudget.c` to `test_nr_gpu_path`'s sources.
  - `nr_gpu_pass_dl.c` gets one added gate name (`"path"`).
  - `nr_gpu_path.c` is attached to `libnr_gpu_platform` with `target_sources`, so it does not touch the `add_library` line.

## 5. For tonight's X410 OTA
- **On the DGX (GB10 SHARED):** by design every path logs `choice=gpu reason=shared`, with no benchmark or trial, and the FEP kernels are unchanged. This is shown by code reading and the mocked unit test, not by a DGX run.
- **On sens6 (DISCRETE):** the build would choose the CPU FEP at 4096, because the GPU is 6× slower per antenna. It would run untested online trials for LDPC and the pass.
- **Recommendation:**
  - Integrate for tonight only with `NR_GPU_PATH_SELECT=off` (previous behaviour), or not at all, until one capped rfsim arm shows the trial verdicts and CRC.
  - The FEP-size work (b) is safe to merge: it is offline bit-exact, and the host keeps the CPU FEP whenever `ready()` = 0.
