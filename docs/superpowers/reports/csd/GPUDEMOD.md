# GPU-DEMOD — bit-exact PDSCH channel estimation and demodulation on the GB10 (2026-10-10)

Branch `csd/gpu-demod` (worktree `rr-csd-gd`, base `csd/otatest` 0c57ede27e, `csd/otatest` 3663f0e31d with WIDE-S0
merged in at 9211afd453). Host: DGX Spark GB10 (sm_121, unified memory). Every build, bench and replay went through
`heavy.sh`. No radio, OTA or hardware tools were used. All numbers are offline: replays of `raw_pci415_12s_b` ("b")
and `raw_pci415_12s_20261008` ("old") at 0.25x with `ota/t26m.env` minus the radio-only knobs
(`tests/passive_rx/dgx/gpudemod_replay.sh`, summaries by `gpudemod_stats.py`), and a standalone bench of the GPU
demod module on a synthetic grant.

## Verdict

* **Parity: met.** Every stage moved to the GPU reproduces the CPU outputs bit for bit, proved inside the receiver by
  CHECK flags that recompute every grant on the CPU and compare.
  - Demod (LLRs, `rxdataF_comp`, mags, `dl_valid_re`, `log2_maxh`, used branch): **0 mismatches** in every CHECK
    run, one per build: 10 runs on b (10,000-24,000 grants each, 14,000 on the final build) and 5 on old (1,000-2,002
    each, 2,000 on the final build). LLRs equal ⇒ LDPC input equal ⇒ CRC identical per grant by construction.
  - Chest (every estimate row, all 4,096 entries, and every noise value): **0 mismatches** in 7 runs on b (6,000-
    26,000 grants each, 14,000 on the final build) and 4 on old (1,000-2,000 each), including PRG/VRB-segmented grants
    with up to 2,048 rows since `9b80863850`.
  - With `ISAC_PDSCH_PDCCH_RM=1` (t26m) on, the GPU path consumes WIDE-S0's combined rate-matching mask unchanged:
    the RE geometry comes from the real `nr_dlsch_extract_rbs()` (see Method). `[MEASURED, replay]`
  - The ISAC_PDSCH_EVM_GRANT accumulator (a diagnostic; the decoder never reads it) is computed on the GPU in float
    with double sums: within 1.3e-5 relative of the CPU's (`evm_close` = every checked grant). The CPU-exact copy of
    the block stays selectable (`ISAC_PDSCH_GPU_EXACT_DEMOD_EVM=0`, or `=2` for double on the GPU: 2e-14).
* **Timing: target not met.** Per decode on b (PDTIM means, final build): **2,425 µs (CPU) → 873 µs**; demod
  1,217 → 347 µs, chest 907 → 163 µs. FEP (~100 µs, already GPU) and LDPC (~230 µs, CPU) are now half of the rest.
  The 0.5 ms target would need LDPC off the CPU and another ~2x on the GPU kernels (see the floor below).
* **LDPC on the GPU: profiled, left on the CPU** (orchestrator item, last section).

## 1. Per-stage timing

PDTIM per-decode means (wall µs, replay cores 10-14 shared by 5 consumers). CPU arm = the current OTA front end
(GPU6 exact FEP + GPU delay) with CPU chest and demod; GPU arm adds `ISAC_PDSCH_GPU_EXACT_DEMOD=1
ISAC_PDSCH_GPU_EXACT_CHEST=1`. Replays are not reproducible run to run (GPU6 §2): the decoded grant mix moves, so
read each row as one sample. `[MEASURED]`

| Capture / arm | FEP | chest | demod | LDPC | sum | decodes |
|---|---:|---:|---:|---:|---:|---:|
| b, CPU (`cpu4`) | 82 | 907 | 1,217 | 220 | 2,425 | 10,800 |
| b, GPU first commit (`dem1`, demod only) | 85 | 936 | 377 | 231 | 1,629 | 5,000 |
| b, GPU final (`ldpccpu`) | 122 | **163** | **347** | 241 | **873** | 22,600 |
| old, CPU (`cpu4`) | 76 | 1,057 | 1,596 | 223 | 2,952 | 4,000 |
| old, CPU (`cpu5`) | 68 | 676 | 625 | 166 | 1,536 | 1,400 |
| old, GPU (`tim9`) | 104 | 348 | 252 | 225 | 929 | 1,800 |
| old, GPU final (`final`) | 187 | 366 | 287 | 257 | 1,097 | 3,800 |

The old capture decodes few grants per run and its runs swing (two CPU runs: 2,952 and 1,536 µs); the final GPU run
there met a busy host (FEP, untouched, read 187 µs). b is the representative capture.

GPU calls on b, final build: demod mean 249 µs per grant (GPU 237), chest 162 µs per grant (pilots on the CPU +
LS + exact delay + FIR + noise). PDTIM demod (347) also carries the 3.4 % of grants the GPU path refuses (Nl ≥ 3,
which run the CPU at ~1.3 ms).

Slot span (`PDSCH_SLOT_TIME`, first-to-last consumer of the slot, queueing included), b:

| arm | p50 | p90 | p99 | 1-job slots p50 / p99 |
|---|---:|---:|---:|---:|
| CPU (`cpu4`) | 5,444 | 20,786 | 65,793 | 4,483 / 68,366 |
| GPU final | 2,579 | 10,533 | 34,790 | 2,433 / 34,341 |

**Standalone floor** (bench, synthetic 12-symbol grant, 4 RX, 16QAM, mode 3 + ALIGN=2, GPU EVM on; p50 per grant,
no other GPU users) `[MEASURED, GB10]`:

| grant | first version | final |
|---|---:|---:|
| Nl=1, 273 PRB | 469 µs | **142 µs** (82 without EVM) |
| Nl=2, 273 PRB | 612 µs | **194 µs** (140 without EVM) |
| Nl=1, 50 PRB | 235 µs | 104 µs |
| Nl=2, 50 PRB | — | 107 µs |

Kernel split, Nl=1 273 PRB: gather 10, level 12, mask/pw 12, ALIGN 39, EVM 72 (concurrent stream), compensation 8,
kappa 8, LLR 7. Nl=2: MMSE mode 2 98, EVM 81. The GB10 runs FP64 at a small fraction of FP32 (microbench: 37 GFLOP/s
of FP64 div+add), and the bit-exact ALIGN and MMSE are FP64 by construction; small kernels cost ~10 µs each.
In the receiver a grant takes ~1.7x the bench (5 consumers and the FEP/chest/discovery kernels share the GPU).

## 2. Method (GPU6's): port the CPU arithmetic, prove it in the receiver

* **Geometry from the CPU, not re-implemented.** `nr_dlsch_extract_geometry()` (8 lines in
  `nr_dlsch_demodulation.c`) runs the real `nr_dlsch_extract_rbs()` on index-coded inputs (`rx[s*N+k] = {k,1}`,
  `est[s*N+j] = {j,s+1}`) for one antenna and layer. Its output is the gather map (erased REs come back `{0,0}`).
  Every DM-RS / CSI-RS / CSI-erase / SSB / own-PDCCH / scheduling-PDCCH rule of the CPU extractor is therefore the
  GPU's input, including WIDE-S0's `ISAC_PDSCH_PDCCH_RM` mask after the merge.
* **Integer stages** are scalar models of the SIMDe/NEON code (`nr_pdsch_gpu_demod.cuh`, shared host/device):
  `madd` wrap, `srai`, `packs`, `mulhrs` (vrshrn), wrapping `sign`, saturating adds in antenna order, the
  USE_128BIT LLR loops with their scalar tails. `test_nr_pdsch_gpu_demod` checks them against the CPU functions:
  `DEMOD_MODEL_MISMATCHES total=0`. `[MEASURED, host]`
* **Float stages follow GCC's code**, read from the disassembly of `nr_dlsch_demodulation.c.o` (GCC 13, -O2,
  fp-contract=fast): ALIGN uses `t = fmadd(uar, urr, uai*uri)`, `cr += t` in RE order, `mag = fsqrt(fmadd(cr, cr,
  ci*ci))`, rotation `fnmsub`/`fmadd`, `lround` then a wrapping int16 cast. MMSE mode 2 ports libgcc 13's
  `__divdc3` from its aarch64 object code, GCC's complex-multiply lowering, and `cabs` of integer components as the
  correctly rounded sqrt of the exact sum (glibc's `cabs` agrees on 2e7 random integer pairs). The module is built
  with `--fmad=false` so nvcc fuses nothing itself.
* **Decision logic** (`nrd_decide`): per-RNTI mode 0-3, mode-3 live mask with `pow(10,-min_db/10)` computed by the
  host libm, `log2_maxh` incl. the median for Nl ≥ 2, `ISAC_L2MAXH_DELTA`, the published branch.
* **Chest**: the estimator's own pilot calls on the CPU, then LS (`chest_ls_type1/2`), GPU6's bit-exact delay
  transform (now one launch for all rows), the 16-tap FIR as `c16multaddVectRealComplex` (mulhrs, doubled with
  saturation, saturating accumulate in pilot order), the inverse delay, and the per-antenna noise; segmented rows are
  assembled and the noise width-weighted exactly as `nr_pdsch_passive_decode.c` does.
* **CHECK flags**: `ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=1` runs the CPU symbol loop as well (N > 1: every N-th grant),
  compares, disables the GPU path on the first difference; the CPU's results are the ones used.
  `ISAC_PDSCH_GPU_EXACT_CHEST_CHECK` does the same for the estimator.

## 3. What runs where

GPU (`libpdsch_gpu.so`): RB extraction (gather), channel level / median / branch decision / output shift, mode-3
masking, ALIGN, `nr_channel_compensation` (incl. the CRC-validated wide-MRC recovery arm), MMSE mode 2 for Nl = 2,
kappa sums, LLRs QPSK/16/64/256QAM, layer demapping into the decoder buffer, the EVM accumulator; the channel
estimator of every grant (one batch per grant). Device-resident: extraction buffers, decisions, EVM partials, chest
LS rows; the GPU reads `rxdataF` and the estimate rows and writes the LLRs/estimates in place (GB10 unified memory).

CPU: geometry (index-coded extractor), pilot sequences, the EVM fold (14 symbols of partial sums), LDPC.

Refused (CPU path runs, counted in the log): PT-RS, Nl ≥ 3, Nl = 2 without MMSE mode 2, ML / EQ_AB, IRC, QAM AGC,
forced branches (subset scans), `ISAC_AGNOSTIC_V2`, BEST_BY_SNR, L2MAXH peak/headroom, decode diagnostics, SFO
correction; chest: PDP diagnostic, delay share, DFT window, GPU5 stage dumps, single-branch estimation, > 2,048 rows.

Not done: batching several grants of a slot into one launch (each consumer submits its grant on its own stream; up
to 5 run concurrently), a CUDA graph per grant, Nl ≥ 3.

## 4. LDPC on the GPU: profile, left on the CPU

`ISAC_PDSCH_GPU_LDPC=1 LDPC_BENCH=1` on b (same build, GPU demod + chest on) `[MEASURED, replay]`:

| | value |
|---|---:|
| PDTIM LDPC per decode | **925 µs** GPU vs **241 µs** CPU (next run, same flags otherwise) |
| code blocks per TB | 1.5-1.7 |
| host prep per TB | 28-30 µs |
| GPU wait per TB | 822-868 µs |
| TBs per GPU batch | 1.01 |
| CPU time per TB (process) | 1,694-1,808 µs (poll + worker) |

Code read (`nrLDPC_cuda/`): the decoder is already batched (a pool worker gathers the code blocks of queued TBs and
launches one CUDA graph), with device-side early termination (converged code blocks return at kernel entry), but
the graph always walks 2 x `max_ldpc_iterations` = 16 flooding iterations (64 kernel nodes) plus the copies, and in
the receiver it never batches more than one TB (1.01) because consumers submit one grant at a time. The wait is
therefore launch/latency-bound (≥ 16 iteration nodes at ~2-3 µs each even when converged) plus queueing behind the
demod/chest/FEP kernels of the other consumers. The algorithm (int8 flooding min-sum, damping 3/4, 16 iterations)
is not OAI's CPU layered decoder, so CRC outcomes cannot be identical to the CPU per grant; the orchestrator's gate
("CRC identical on paired grants") would need a bit-exact port of the CPU decoder, which was not attempted tonight.
LDPC stays on the CPU.

## 5. OTA env for the orchestrator

Binary: branch `csd/gpu-demod` at the head commit of this report (includes `csd/otatest` 3663f0e31d). Config, cores
and X410 arguments as for t26m.

* **Arm A (current):** `ota/t26m.env`.
* **Arm B (GPU chest + demod):** `ota/t26m.env` followed by

```text
ISAC_PDSCH_GPU=0 ISAC_PDSCH_GPU_EXACT_DEMOD=1 ISAC_PDSCH_GPU_EXACT_CHEST=1 ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=64 ISAC_PDSCH_GPU_EXACT_CHEST_CHECK=64
```

(t26m already carries `ISAC_PDSCH_GPU_EXACT_FEP=1 ISAC_CHEST_GPU_DELAY=1`, which the chest path needs.) The CHECK=64
spot checks cost one CPU recompute every 64th grant; set them to 0 for the timing-only arm. Leave unset:
`ISAC_PDSCH_GPU_LDPC`, `ISAC_PDSCH_GPU_EXACT_DEMOD_EVM` (default 1 = GPU float EVM), `ISAC_PDSCH_GPU_EXACT_DEMOD_PROF`.

Expected log lines in arm B: `GPU_EXACT_DEMOD enabled ... check=64`, `GPU_EXACT_CHEST enabled ... check=64`,
periodic `GPU_EXACT_DEMOD calls=... check_bad=0 ... evm_far=0` and `GPU_EXACT_CHEST calls=... check_bad=0`, no
`MISMATCH`. PDTIM `chest` and `demod` should drop to roughly 0.2 and 0.35 ms. `[IMPLEMENTED, NOT VALIDATED]` over the
air.

## 6. Commits, files, hooks

* `43d5aec944` perf(gpu): PDSCH demodulation on GPU, bit-exact
* `8d81f3ff0d` perf(gpu): PDSCH channel estimation on GPU, bit-exact
* `9211afd453` merge `csd/otatest` (WIDE-S0) into `csd/gpu-demod`
* `b3798d2d3c` perf(gpu): demod + chest kernels (EVM float/concurrent, ALIGN 2-phase, device state, delay 1 launch, wide MRC)
* `9b80863850` perf(gpu): segmented PDSCH chest on GPU (2,048 rows)
* `dc910e2def` perf(gpu): ALIGN terms and EVM sums in parallel
* `32d28ca479` perf(gpu): MMSE mode 2 Gauss-Jordan unrolled

New files: `nr_pdsch_gpu_demod.{cu,cuh,h}`, `nr_pdsch_gpu_demod_host.{c,h}`, `nr_pdsch_gpu_chest.h`,
`tests/nr_pdsch_gpu_demod_test.cc`, `tests/passive_rx/dgx/gpudemod_replay.sh`, `gpudemod_stats.py`. GPU files changed:
`nr_pdsch_gpu_fep.cu` (chest entry), `nr_pdsch_gpu_exact_fep.cuh` (DelayIO.base), `nr_pdsch_gpu_exact.c` (warm-up,
chest entry), `NR_UE_ESTIMATION/nr_dl_channel_estimation.c` (`nr_pdsch_gpu_chest_grant`).

Hooks in shared files (few lines each, geometry taken from the CPU so WIDE-S0's fixes carry over):
- `nr_dlsch_demodulation.c`: `nr_dlsch_extract_geometry()` wrapper (+17 lines, calls the static extractor).
- `nr_pdsch_passive_decode.c` (+26/-2 vs `csd/otatest`): include (1 line); symbol loop:
  `gdem = nr_pdsch_gpu_demod_grant(...)` + loop condition (6 lines); non-segmented chest loop: `gch` call + per-call
  branch (4 lines); segmented chest loop: `gseg` call, the loop's locals, and a guarded nvar copy (15 lines).
- `nr_ssb_rate_match.c`: none.

Build/test: `ninja nr-uesoftmodem pdsch_gpu test_nr_pdsch_gpu_demod`; `test_nr_pdsch_gpu_demod` passes
(0 mismatches). Full ctest not rerun in this round (only the new test and the replays). sens6 gate clean at every
commit.

```sh
O=/home/nicola/NICOLA/wt/rr-orchestration; F="ISAC_PDSCH_GPU_EXACT_DEMOD=1 ISAC_PDSCH_GPU_EXACT_CHEST=1"
bash tests/passive_rx/dgx/gpudemod_replay.sh b chk $F ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=1 ISAC_PDSCH_GPU_EXACT_CHEST_CHECK=1
bash tests/passive_rx/dgx/gpudemod_replay.sh b tim $F            # timing
bash tests/passive_rx/dgx/gpudemod_replay.sh b cpu               # CPU arm
python3 tests/passive_rx/dgx/gpudemod_stats.py replays/replay_gd_<name>_b_*/run.log
# GD_ENV=t26l selects another env file; ISAC_PDSCH_GPU_EXACT_DEMOD_PROF=1 prints per-kernel GPU time
```

## Deferred

* OTA A/B (orchestrator), env in §5.
* LDPC: a bit-exact GPU port of the CPU layered decoder (needed for per-grant CRC identity); the existing flooding
  decoder is 3.8x slower in the receiver.
* Nl ≥ 3 (stock fixed-point MMSE), PT-RS; batching a slot's grants into one launch / CUDA graphs; full ctest run.
