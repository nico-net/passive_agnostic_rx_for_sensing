# SENS6-PORTABILITY-2026-10-24: portable build, 3-tier GPU platform and sens6 runs

Integration base: `sens6/integ` @ `1d497f7b3a` (merge of `sens6/discrete-gpu`). Hosts: sens6 only (x86_64, Intel Core Ultra 5 235, 14.8 GB RAM, RTX 4060 Ti cc 8.9, 8188 MiB, CUDA 12.4, gcc 15.2). No DGX run and no OTA run in this report.

Labels (PROJECT_MEMORY §0.1): `[MEASURED, sens6 …]`, `[CODE-READ, git]` (commit text or code read, not run), `[NOT MEASURED]`. Nothing here is pooled with the DGX GB10 numbers.

Source of the run numbers: the scratchpad files `sim_results3.md` (`@4a94af8c0b`, 106 and 273 PRB), `t9_results.md` (`@e300e1e63e`, ctest and T9), and `docs/superpowers/reports/csd/SENS6-SIM-2026-10-23.md`. The mixed-slot solver-gate rows were re-scored today from `ms_b2_r1/base_r1` and `ms_b2_r2/base_r1` with `tests/passive_rx/dgx/score_rx_solver.py`.

## 1. Branches and commits

| branch | head at this writing | role | merged into `sens6/integ` |
|---|---|---|---|
| `sens6/discrete-gpu` | `e300e1e63e` | 3-tier GPU platform, T0-T8, T6b, GPU ISA twins | `1d497f7b3a` |
| `sens6/build-portable` | `5931623290` | portable build fixes | `157fca8cfe` |
| `sens6/mixedslot` | `9afc38e09c` | b2-fix (mixed-slot learner, solver BWP cap, QPSK rotation) | `9afc38e09c` |

Key commits `[CODE-READ, git]`:

| commit | content |
|---|---|
| `9aef537ffb` | DFT-alignment crash fix: 64-B aligned DFT outputs in DL traffic probe and SRS FEP; probe started once |
| `d89db002eb` | T1+T2: CUDA 12.3..13.x compat shims (`nr_gpu_compat.h`); nvcc-aware arch list (drops unbuildable archs with a WARNING) |
| `c386fb8767` | T3+T4: SHARED / DISCRETE / CPU tiers per device at probe; `NR_GPU_FORCE_MODEL`; DIRECT-only guards |
| `7aebf86bae` | M1-M3: probe in a private context; timeout keeps SHARED; fault -> DISCRETE/CPU |
| `8b731049d4` | L1a/L1b: CUDA 12.3 green-context placeholder; explicit-version driver lookup below 12.5 |
| `3d9aefd99e` | arch filter handed on and warned once; fp64 SASS check on built archs; llr_norm test skips off DIRECT |
| `6fd421121b` | T8: `_discrete` / `_cpu` ctest tiers; CPU-tier skips; gcc 15 / x86 test fixes |
| `09bdf2d3f7` | T7: green-context discovery SMs = `max(1, SMs/6)` from the device |
| `8de80ae9ef` | T5: GPUPASS DISCRETE parity (rank>1 DM-RS SNR rows, LDPC counters, qm row, ordered delay-table H2D) |
| `6797027096` | T6: GPUPASS graph memory cap from free memory at probe |
| `237e4d25d9` | T6b: device-sized shared memory budget; per-module shares and floors; csirs segment scratch mode (merged `189d46c797`) |
| `5b165facd3` | simde_mm_average: AVX2 path uses the same 4 wrapping int32 lanes as SSE/NEON (merged `d936f6e6a8`) |
| `67d5695d9f` | test: 32-byte aligned LLR scratch in the UL pass CPU reference |
| `da99b7e89f`, `8d5a91f868` | x86 split-radix DFT twin, rotation twins, host-CPU references; ISA profile chosen at init by self-test, fail closed (merged `e300e1e63e`) |
| `5931623290` | portable build: `_GNU_SOURCE` guards, `nr-uesoftmodem-optional-modules`, `nr_pusch_ra0sim` link, gcc 15 gtest includes (merged `157fca8cfe`) |
| `da253bfd6a`, `a6b95559de` | b2-fix: mixed-slot (S,L) learner per slot phase; solver BWP cap and QPSK rotation (merged `9afc38e09c`) |

## 2. Build (sens6)

Configure: `cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=OFF -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON <src>`. Default CUDA arch list used (no `-DLDPC_CUDA_ARCH=89` needed) `[MEASURED, sens6 build, @e300e1e63e, T9]`.

| step | result | label |
|---|---|---|
| `ninja -k 0` at `@4a94af8c0b` | only failure: `pdsch_gpu` `nr_gpu_pass_graph.cu.o` (fixed by the T1 shims); `nr_pusch_ra0sim` link failure (fixed by `5931623290`) | `[MEASURED, sens6 build]` |
| `ninja -k 0` at `@e300e1e63e` (T9 build) | only failure: `nr_pusch_ra0sim` (known link, before `5931623290`); `nr-uesoftmodem` and `nr-softmodem` link | `[MEASURED, sens6 build]` |
| `ctest -j1` at `@e300e1e63e` | 280 run, 268 pass, 12 fail (9x `test_nr_pusch_ra0_*` with no target at that HEAD, `nr_td_sim_test` (395 s), `test_nr_ssb_rate_match_prod`, `test_vrtsim_cirdb`); 5 skipped (CPU variants); 15 disabled (1339 s) | `[MEASURED, sens6 ctest]` |

Known sens6 ctest failures after the merge: `nr_td_sim_test`, `test_nr_ssb_rate_match_prod`, `test_vrtsim_cirdb`. The `test_nr_pusch_ra0_*` entries are a build gap that `5931623290` closes; not re-run at `1d497f7b3a` in this report `[NOT MEASURED]`.

## 3. GPU tier on sens6

| item | value | label |
|---|---|---|
| tier line | `GPU_PLATFORM model=discrete tier=discrete reason=hmm_pcie` (RTX 4060 Ti cc 8.9, total 8182 MB) | `[MEASURED, sens6 rfsim, @e300e1e63e]` |
| ISA twins | `GPU_ISA_TWIN dft_twin=avx2 rot_twin=madd` (self-test OK) | `[MEASURED, sens6 rfsim, @e300e1e63e]` |
| memory budget | `GPU_MEM tier=discrete total=7803 free_at_probe=7658 budget_mb=5741 allocated_mb=501 pinned_host_mb=82 graph_cap_mb=1561` | `[MEASURED, sens6 rfsim, @e300e1e63e]` |
| GPUPASS | enabled, 704 shape classes (+2014 segmented) x 7 instances; census at kill: launches 0 (no grant reached the GPU) | `[MEASURED, sens6 rfsim, @e300e1e63e]` |
| before the T3-T8 merge | `model=discrete`, DISCRETE backend not implemented, all DL decode on CPU | `[MEASURED, sens6 rfsim, @595c5dcf33 / @4a94af8c0b]` |

## 4. Runs on sens6

### 4.1 Gate 106 PRB, 4 RX, 420 s, two arms (`@4a94af8c0b`)

`[MEASURED, sens6 rfsim, 106 PRB 4 RX]`. Old scorer (`score_rx.py`) cannot pass since `0fb22ecbd0` (no `Technique D CONVERGED` lines). Solver scorer (`score_rx_solver.py`, proposed, not adopted) below.

| criterion | limit | arm 1 | arm 2 | result |
|---|---|---|---|---|
| n_solved | >= 1 | 1 | 1 | PASS |
| solve_s | <= 10 | 1.206 | 1.28 | PASS |
| post_solve_crc | >= 99.8 | 86.55 | 86.59 | FAIL |
| post_solve_tr | >= 5000 | 44724 | 42733 | PASS |
| crc_floor | >= 93.4 | 85.39 | 85.44 | FAIL |
| drop_full | <= 2.5 % | 0.0126 | 0.0057 | PASS |
| old scorer `n_contexts` | >= 2 | 0 | 0 | FAIL |

Verdict: FAIL on both arms. RNTI 0x1234 decoded 85 % of PDSCH queue entries; the ~13.5 % failures are in the low-quality grants (CRC of qualified grants 100 %) `[INFERRED from the scorer split]`.

### 4.2 Mixed-slot b2-fix, 106 PRB, 4 RX, 420 s, two runs (`sens6/mixedslot` @ `9afc38e09c`)

`[MEASURED, sens6 rfsim, 106 PRB 4 RX, branch sens6/mixedslot]`, re-scored with `score_rx_solver.py`.

| criterion | run 1 | run 2 | limit |
|---|---|---|---|
| n_solved | 1 | 1 | >= 1 |
| solve_s | 1.046 | 0.886 | <= 10 |
| post_solve_crc | 99.96 | 99.96 | >= 99.8 |
| post_solve_tr | 43775 | 43813 | >= 5000 |
| crc_floor | 98.80 | 98.94 | >= 93.4 |
| drop_full | 0.2881 | 0.0087 | <= 2.5 |

Solver gate: PASS, PASS. Old scorer: FAIL (no contexts). One UE only. No pre-fix baseline run exists in this report `[NOT MEASURED]`.

### 4.3 273 PRB, 4 RX, 180 s (`@4a94af8c0b`)

| arm | result | label |
|---|---|---|
| rank 1, auto conf | synced; 0 of 4221 PDSCH decoded; wide-only rule fired at 76.5 s, never solved | `[MEASURED, sens6 rfsim]` |
| rank 4, pinned conf | 3315/3315 decoded; CRC 100.0 %; p50/p99/p999 33097 / 34135 / 38132 us (n_real 3452) | `[MEASURED, sens6 rfsim]` |
| rank 4, auto conf | never synced; not a valid bed | `[MEASURED, sens6 rfsim]` |

### 4.4 T9 (273 PRB rank 4, 4 RX, GPU versus CPU arms) at `@e300e1e63e`

Not completed. Every arm (gpu_1, cpu_1, gpu_2, cpu_2, diagnostic gpu_3) was killed by the kernel OOM killer (exit 137) after 14 s (GPU) or 24 s (CPU), before any grant was decoded.

| item | value | label |
|---|---|---|
| receiver anon RSS at kill | 8.6 GB, about 6.4 GB swapped; host swap exhausted | `[MEASURED, sens6 rfsim]` |
| reference, same bed at `@4a94af8c0b` | 180 s, RSS max 7.2 GB, swap used 7.9 GB of 8 GB | `[MEASURED, sens6 rfsim]` |
| growth over reference | about 1.2-1.4 GB, cause not isolated; the CPU arm also dies, so it is not GPU-specific | `[MEASURED]` / `[INFERRED]` |
| pass criteria (shadow_bad, check_bad, CRC within 1 %, device memory < 50 %) | NOT EVALUABLE | |

### 4.5 Scheduling and host

Core pool: `core 3 not usable ... dropped`, then `5 cores: 6 7 8 9 4` in every run; no thread pinned (no CAP_SYS_NICE); UEthread_0 at 96-98 % of one CPU in every run `[MEASURED, sens6 rfsim]`. RAM at the end of the 273 PRB rank-4 run: 326 MB free, 976 MB swap used `[MEASURED, sens6 rfsim, @4a94af8c0b]`.

## 5. Not measured, not claimed

- Mixed-slot CRC before the b2-fix on sens6 (no baseline run).
- The "mixed-slot CRC 0 % to 99.7 %" figure and the "4.2 GB local-memory reserve" figure given in the session brief: neither appears in the run logs, commits or reports read for this report, so neither is quoted.
- 1 RX at 106 PRB on the new build; any OTA run; any DGX run of the `e300e1e63e` / `1d497f7b3a` builds; the T10 DGX regression.

## 6. Open work and T10 checks

Open: host-RAM budget and on-demand buffers (T9 blocker); DCI 1_1 general resolver; 273-PRB rank-1 solver; Q4 staged DISCRETE variants of the standalone chest, demod and UL FEP entries; core plan from topology; GPU/CPU per-path speed choice; GPU FEP for all FFT sizes.

T10 (DGX regression of the GB10 path, plan §3) must check: identical build before and after merge; `ctest -j1` plus all `_discrete` and `_cpu` variants; 106-PRB 4-RX gate with CLAUDE.md thresholds (scored with the old scorer, which cannot pass, and separately with the proposed solver scorer, labelled as such); one 273-PRB rank-4 4-RX arm with `GPUPASS enabled ... DIRECT`, census and CRC within the run-to-run spread of the pre-merge arm; `GPU_PLATFORM ... tier=shared reason=integrated probe=ok`; optional OTA A/B (user decision).
