# P-FUSE (Phase 1 Task 12): chest + MMSE demod + EVM fused, noise-floor-aware SNR — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One GPU unit that does the DL channel estimate, the multi-layer MMSE demod, LLR generation and the EVM sums of a PDSCH grant, in FP32 where the gate allows, and that emits a noise-floor-aware SNR plus DM-RS based nvar/alpha; chest+demod p99 <= 150 us at 273 PRB x 4 L (to be confirmed by the bench of Task 1, the 150 us is an estimate).

**Architecture:** Task 1 benches today's chest and demod (no code change except a gated bench test). Tasks 2-3 are pure, GPU-free units (SNR floor table shared by python and C, DM-RS SNR definition). Tasks 4-6 add the fused FP32 kernel family in a new `.cu` file behind `ISAC_PDSCH_GPU_FUSE=1` (default off, old exact path stays as reference and fallback), fed by per-grant inputs (S/L, DM-RS mask, CSI-RS rate-match RE mask) so that T8-TIM-DEAD fixes A/B (TDRA row, rate-match set) change only the inputs, never the kernel. Task 7 is the replay A/B gate. The decision on where the fusion boundary goes is taken from the Task 1 bench, not guessed.

**Tech Stack:** CUDA C++ (sm_121 GB10 DIRECT; sm_89 RTX 4070 DISCRETE via `nr_gpu_model()`), C, gtest (`test_nr_gpu_pass`), Python 3 stdlib + unittest (numpy only in the table generator), replay `tests/passive_rx/p99_replay.sh`.

**Spec / sources (csd/int16 worktree `/home/nicola/NICOLA/wt/rr-csd-int16`, read-only for planning):** `docs/superpowers/plans/2026-10-16-phase1-dl-g1-g3.md` (Task 12 contract), `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` (1.1 row 4, item E-SNR, sections 3, 5, 8), reports `DLCRC-ROOTCAUSE-20261016.md`, `SCORECARD-BASELINE-20261016.md`, `T8-TIM-DEAD-20261016.md`, `MMSE3GPU-20261014.md`, `GPUFUSE-20261011.md`; code `tests/passive_rx/goal_metrics.py` (`TRUE_TO_EST`, `true_snr_db`), `tests/passive_rx/evm_snr_bias_table.py`, `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_fp32_gate.h`, `nr_pdsch_gpu_chest.h`, `nr_pdsch_gpu_demod.cu/.h`, `nr_pdsch_gpu_demod_host.c`, `nr_gpu_pass_graph.cu`, `nr_gpu_pass_dl.c`, `NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1965-2330`, tests `NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` + `nr_gpu_pass_fixture.h`.

## Global Constraints

- Every new receiver behaviour is behind a default-off `ISAC_*` env flag until its replay A/B passes; the CPU path stays as reference and fallback. No per-cell constants (no PCI, frequency or operator in code). Here: `ISAC_PDSCH_GPU_FUSE=1`, default off; with it off, behaviour is byte-identical (pinned by Task 6 test).
- New GPU code: multi-cell ready (cell index in every job), allocation through `nr_gpu_buf_alloc` + `nr_gpu_buf_h2d_async/d2h_async` called unconditionally (DIRECT GB10 / DISCRETE sens6 decided inside), one CUDA context, RT streams at greatest priority; each port also runs once with `ISAC_GPU_FORCE_DISCRETE=1` on the DGX.
- No allocation after the RX fence (`nr_gpu_allocations_allowed`); every fused scratch buffer is allocated at init for the maximum shape (273 PRB x 14 sym x 4 L x Qm 8).
- FP32: the fused kernel is FP32/mixed with a CPU-reference tolerance check; FP64 only where bit-exactness needs it and never on DISCRETE in the 500 us path. The gate is `nr_gpu_pass_fp32_gate.h` (`NR_FP32_GATE_MIN_N`=64 shadowed grants, flips > 1 % trips; LLR sign disagreement > 0.1 % trips) and the `check_no_fp64_sass.sh` script must stay green for the new `.cu`.
- Core placement: latency-critical threads on Cortex-X925 cores 5-9/15-17, helpers on A725 cores 0-4/10-14.
- Commands: every heavy command through `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh <cmd> < /dev/null`; builds `heavy.sh ninja -C <build> -j6 <targets>` (never more than -j6); private MPS: prefix every GPU run with `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps`. Agents cannot run `cmake`: steps marked **[ORCHESTRATOR: cmake]** are done by the orchestrator.
- Never build while `nr-uesoftmodem` runs (`pgrep -x nr-uesoftmodem`). sens6 frozen paths untouched. Python analysis: stdlib only, `nice -n 19 taskset -c 17 timeout 900 python3 ...`, one at a time; run logs are ~90 MB, never load them into context.
- `git add <explicit paths>`, never -A, never stash. Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` + `Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y`. Retry on `.git/index.lock` 5 s apart, max 5. Agents return report text; the orchestrator saves and commits reports.
- Evidence label on every number; never pool V (Vodafone 3680.01 MHz, `CAPSET=b`) and T (TIM 3759.99 MHz, `CAPSET=tim`), hosts or bandwidths. Multi-cell per spec: no cell constants; the SNR floor is a function of Qm only.
- Metric definitions (roadmap): CRC = SNR-qualified `crc_q`; latency = `tot` p99. `snr_l` (legacy decision-directed EVM) keeps being emitted unchanged: the new SNR goes in NEW fields (see Review Focus 1).

## Review Focus

1. Adding a better SNR must not silently change `crc_q`: `goal_metrics.crc_q` is computed from `snr_l` through `true_snr_db`; if the fused path replaced `snr_l`, CAPSET=b/tim `crc_q` would change by definition, not by receiver behaviour. Pinned by `test_legacy_snr_l_unchanged_with_fuse` (Task 6) and the `crc_q unchanged` gate (Task 7). The new fields are `snr_nfa` and `snr_dmrs`.
2. A grant whose estimate sits on the Qm floor (est within 1 dB of the floor, `true_snr_db` = None) must report "unknown", never a number and never qualified. Pinned by `test_floor_is_unknown` (Task 2).
3. A grant with S/L, DM-RS mask or CSI-RS rate-match mask different from the default (special slot S=1 L=5; periodic CSI-RS slot, T8 class A/B) must be demodulated with exactly those REs, and an empty or zero mask must equal today's behaviour. Pinned by `test_fuse_masks_match_exact_path` (Task 5).
4. Gate trip mid-run: when the FP32 gate or the CHECK mismatch disables the fused path, the very next grant must run on the old exact path with no lost grant and no stale device state. Pinned by `test_fuse_disable_falls_back` (Task 6).
5. Multi-layer noise enhancement and low-SNR Nl=4: the floor-table inversion is AWGN + ideal ZF only (+-1.5 dB near the floor, DLCRC-ROOTCAUSE). The DM-RS SNR must stay within its stated tolerance at Nl=4 and QPSK at 3 dB, and the doc must say which of the two SNRs is authoritative. Pinned by `test_dmrs_snr_nl4_low_snr` (Task 3).

---

## File structure

| file | responsibility |
|---|---|
| `tests/passive_rx/evm_snr_bias_table.py` (modify) | add `--emit-header`: writes `nr_snr_floor_table.h` from the same simulation that produced `TRUE_TO_EST` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor.h` (create) | `__host__ __device__` inline `nr_snr_floor_invert(est_db, qm)` + generated table include; mirrors `goal_metrics.true_snr_db` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor_table.h` (create, generated) | the four `TRUE_TO_EST` rows as `static const float` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fuse.h/.cu` (create) | job struct, FP32 fused kernels, launcher `nr_pdsch_gpu_fuse_dev` |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fuse_host.c` (create) | env switch, gate, CHECK shadow, fallback to the exact path |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` (modify) | bench, SNR, FP32 agreement tests (reuses `GpuPassFixture`) |
| `tests/passive_rx/test_noise_floor_snr.py` (create) | python-side table/inversion tests |
| `CMakeLists` of `openair1/PHY/CODING` (modify, **[ORCHESTRATOR: cmake]**) | add the two new sources to `pdsch_gpu` / the host lib; the compile flags of `nr_pdsch_gpu_demod.cu` (`--fmad=false`) are NOT applied to the new FP32 file |

---

### Task 1: Kernel bench of today's chest and demod at 273 PRB x 4 L (step 0 of the contract)

The 150 us budget is an estimate [INFERRED: spec 1.1 row 4, no kernel bench exists]. Known context, not a substitute: chest mean 157/122 us (V/T), demod mean 189/158 us, `[MEASURED, DGX OTA 2026-10-16, V/T, t26aXE]` (spec 1.1); Nl4 > 200 RB demod GPU part 1.0-1.2 ms `[MEASURED, DGX replay tim C1, MMSE3GPU-20261014 sec.4, ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES]` (includes the call's host-visible GPU part, not a pure kernel time). Those two numbers disagree by 5x, so the bench must separate kernel time from host geometry/launch time.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` (append test `GpuBench.ChestDemod273x4`, skipped unless `NR_GPU_BENCH=1`, so ctest is unchanged)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_fixture.h` (add `static nr_gpu_pass_shape_t bench_shape(int nrb, int nl, int qm)`; `warm_shapes()` at :683 stops at 217 PRB, so 273 needs a new shape)
- Report (text returned): `docs/superpowers/reports/csd/PFUSE-BENCH-<date>.md`

**Interfaces:**
- Consumes: `GpuPassFixture::from_shape(shape, seed)`, `.bind()`, `.run_pass(m, fp32flag)`, `nr_pdsch_gpu_chest_dev(job, n, est_dev, nvar_dev, slot, stream)` (nr_pdsch_gpu_chest.h), `nr_pdsch_gpu_demod_pass_dev(inst, jcap, llr_dev, nvar_dev, stream, stream2)` (nr_pdsch_gpu_demod.h:96), `nr_gpu_pass_nrb_class(273)` = 273.
- Produces: stdout lines `BENCH nrb=273 nl=4 qm=<2|4|6|8> fp=<64|32> chest_us p50=.. p99=.. demod_us p50=.. p99=.. evm_us p50=.. p99=.. sum_p99=..` (GPU event times, n >= 200 iterations after 20 warm-ups), plus `BENCH_KERNELS` listing kernel names with their share (from `nsys`, below).

- [ ] **Step 1: Write the bench test** (it fails to compile until `bench_shape` exists)

```cpp
// appended to nr_gpu_pass_test.cc
TEST(GpuBench, ChestDemod273x4) {
  if (!getenv("NR_GPU_BENCH")) GTEST_SKIP() << "set NR_GPU_BENCH=1";
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  for (int fp32 : {0, 1})
    for (int qm : {2, 4, 6, 8}) {
      auto sh = GpuPassFixture::bench_shape(273, 4, qm);
      sh.fp32 |= fp32 ? NR_GPU_PASS_FP32_DEMOD : 0;
      ASSERT_EQ(0, m.pass_warm(&sh, 3, 0, 1));
      GpuPassFixture fx = GpuPassFixture::from_shape(sh, 7);
      ASSERT_TRUE(fx.tb_ok_built);
      fx.bind();
      std::vector<double> chest, demod, evm;
      for (int it = 0; it < 220; it++) {
        auto r = fx.run_pass(m, fp32);   // fills r.t_chest_us, r.t_demod_us, r.t_evm_us from cudaEvent pairs
        ASSERT_EQ(0, r.rc);
        if (it >= 20) { chest.push_back(r.t_chest_us); demod.push_back(r.t_demod_us); evm.push_back(r.t_evm_us); }
      }
      auto q = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v[(size_t)(p * (v.size() - 1))]; };
      printf("BENCH nrb=273 nl=4 qm=%d fp=%d chest_us p50=%.1f p99=%.1f demod_us p50=%.1f p99=%.1f evm_us p50=%.1f p99=%.1f\n",
             qm, fp32 ? 32 : 64, q(chest, .5), q(chest, .99), q(demod, .5), q(demod, .99), q(evm, .5), q(evm, .99));
    }
}
```

The implementer must add the three `cudaEventRecord` pairs around the chest launch, the demod launch (k_gather ... k_tail) and the EVM chain inside `GpuPassFixture::run_pass` (they do not exist yet; `r.t_*` are new members of its result struct, only filled when `NR_GPU_BENCH` is set so other tests are unaffected).

- [ ] **Step 2: Build** (**[ORCHESTRATOR: cmake]** only if the target list changed; it does not here)

Run: `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ninja -C /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build -j6 test_nr_gpu_pass pdsch_gpu < /dev/null`
Expected: builds; `pgrep -x nr-uesoftmodem` must be empty first.

- [ ] **Step 3: Run the bench**

Run:
```
cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build && NR_GPU_BENCH=1 CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./test_nr_gpu_pass --gtest_filter=GpuBench.ChestDemod273x4 < /dev/null 2>&1 | tee /tmp/pfuse_bench.log | grep BENCH
```
Expected: 8 `BENCH` lines (Qm x FP64/FP32). Then once more with `ISAC_GPU_FORCE_DISCRETE=1` prepended to see the DISCRETE copy cost, and once under nsys for the kernel list:
`... heavy.sh nsys profile -o /tmp/pfuse_bench --stats=true ./test_nr_gpu_pass --gtest_filter=GpuBench.* < /dev/null` (env as above, `NR_GPU_BENCH=1`), keep only the top-15 kernel rows by total time.

- [ ] **Step 4: Decide from the numbers and write the report**

The report states, per Qm and FP64/FP32: p50/p99 of chest, demod, EVM, their sum, the top kernels, and one of three verdicts, which fixes the scope of Tasks 4-5:
  - sum_p99 (FP32) <= 150 us already: fusion is a launch/bandwidth saving only; keep Task 5 minimal (the fused kernel is chosen only if it reduces p99 by >= 20 % vs the sum).
  - 150 us < sum_p99 (FP32) <= 300 us: fuse as planned; budget 150 us is a target, report the gap.
  - sum_p99 (FP32) > 300 us: the 150 us budget is infeasible with this algorithm; report the dominating kernel (expected candidate to check: `k_mmse3_prep`/`k_tail<8>` per-RE 4x4 solve, `k_evm1-3`) and return to the orchestrator before Task 4 with a revised budget. Do not continue silently.

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_fixture.h
git commit -m "test(gpu-fuse): chest+demod kernel bench at 273 PRB x 4 L (NR_GPU_BENCH=1, skipped otherwise)"
```

---

### Task 2: Noise-floor table shared by python and C (item E-SNR, metric side)

**Files:**
- Modify: `tests/passive_rx/evm_snr_bias_table.py` (add `--emit-header PATH`)
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor_table.h` (generated, committed)
- Create: `tests/passive_rx/test_noise_floor_snr.py`

**Interfaces:**
- Consumes: `goal_metrics.TRUE_TO_EST`, `goal_metrics.GRID0` (= -10), `goal_metrics.FLOOR_GUARD_DB` (= 1.0), `goal_metrics.true_snr_db(est_db, qm)`.
- Produces: C `static inline float nr_snr_floor_invert(float est_db, int qm)` (`__host__ __device__` under nvcc); returns `NR_SNR_UNKNOWN` (= -99.0f) when `true_snr_db` would return None. Python: `evm_snr_bias_table.emit_header(path)`.

- [ ] **Step 1: Write the failing test**

```python
# tests/passive_rx/test_noise_floor_snr.py
import os, re, subprocess, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(__file__))
import goal_metrics as gm

ROOT = os.path.join(os.path.dirname(__file__), '..', '..')
HDR = os.path.join(ROOT, 'openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor_table.h')

def parse_header(path):
    txt = open(path).read()
    out = {}
    for qm, body in re.findall(r'NR_SNR_FLOOR_QM(\d)\[\d+\] = \{([^}]*)\}', txt):
        out[int(qm)] = [float(x) for x in body.replace('f', '').split(',') if x.strip()]
    return out

class TestFloorTable(unittest.TestCase):
    def test_header_matches_goal_metrics(self):
        self.assertEqual(parse_header(HDR), {q: [float(v) for v in t] for q, t in gm.TRUE_TO_EST.items()})

    def test_floor_is_unknown(self):
        for qm, floor in ((2, 3.9), (4, 9.1), (6, 12.5), (8, 14.4)):
            self.assertIsNone(gm.true_snr_db(floor + 0.5, qm))   # within the 1 dB guard: unknown, never a number

    def test_inversion_roundtrip(self):
        for qm in (2, 4, 6, 8):
            for true in (6.0, 12.0, 20.0):
                est = gm.TRUE_TO_EST[qm][true - gm.GRID0 if False else int(true) - gm.GRID0]
                got = gm.true_snr_db(est, qm)
                if got is not None:
                    self.assertAlmostEqual(got, true, delta=1.5)   # table is non-injective near the floor: 1.5 dB as stated in DLCRC-ROOTCAUSE

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run it, expect FAIL** (header missing)

Run: `nice -n 19 taskset -c 17 timeout 900 python3 /home/nicola/NICOLA/wt/rr-csd-int16/tests/passive_rx/test_noise_floor_snr.py`
Expected: FAIL `FileNotFoundError ... nr_snr_floor_table.h`.

- [ ] **Step 3: Implement**

`emit_header` in `evm_snr_bias_table.py`: reads nothing new, takes `goal_metrics.TRUE_TO_EST` (single source of truth; the generator's own simulation is the origin of that table, so re-simulation is only done with `--resimulate`, which is not required here) and writes
```c
/* generated by tests/passive_rx/evm_snr_bias_table.py --emit-header; do not edit */
#define NR_SNR_FLOOR_N 41
#define NR_SNR_FLOOR_GRID0 (-10)
static const float NR_SNR_FLOOR_QM2[41] = {3.95f, 3.95f, ...};
... QM4, QM6, QM8
```
(the test regex matches `NR_SNR_FLOOR_QM2[41] = {...}`; keep that exact spelling).

`nr_snr_floor.h`:
```c
#ifdef __CUDACC__
#define NR_SNRF_HD __host__ __device__
#else
#define NR_SNRF_HD
#endif
#include "nr_snr_floor_table.h"
#define NR_SNR_UNKNOWN (-99.0f)
#define NR_SNR_FLOOR_GUARD_DB 1.0f
NR_SNRF_HD static inline float nr_snr_floor_invert(float est, int qm)
{
  const float *t = qm == 2 ? NR_SNR_FLOOR_QM2 : qm == 4 ? NR_SNR_FLOOR_QM4 : qm == 6 ? NR_SNR_FLOOR_QM6 : qm == 8 ? NR_SNR_FLOOR_QM8 : 0;
  if (!t || est < t[0] + NR_SNR_FLOOR_GUARD_DB) return NR_SNR_UNKNOWN;
  if (est >= t[NR_SNR_FLOOR_N - 1]) return est;
  for (int i = 0; i < NR_SNR_FLOOR_N - 1; i++)
    if (est <= t[i + 1]) {
      const float d = t[i + 1] - t[i];
      return (float)(NR_SNR_FLOOR_GRID0 + i) + (d > 0.f ? (est - t[i]) / d : 0.f);
    }
  return est;
}
```
(identical control flow to `goal_metrics.true_snr_db`.) Run `python3 evm_snr_bias_table.py --emit-header openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor_table.h`.

- [ ] **Step 4: Add the C-vs-python golden test** in `nr_gpu_pass_test.cc` as `TEST(SnrFloor, MatchesPython)` with 12 (est, qm) pairs generated once by `python3 -c "import goal_metrics as g; ..."` and pasted as literals (include one floor case per Qm expecting `NR_SNR_UNKNOWN`); tolerance 1e-4 dB.

- [ ] **Step 5: Run both**: python test (command above) then `heavy.sh ninja -C <build> -j6 test_nr_gpu_pass` and `ctest -R test_nr_gpu_pass` with the MPS env. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add tests/passive_rx/evm_snr_bias_table.py tests/passive_rx/test_noise_floor_snr.py openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor.h openair1/PHY/NR_UE_TRANSPORT/nr_snr_floor_table.h openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc
git commit -m "feat(gpu-fuse): shared noise-floor SNR inversion table (python == C), floor is unknown"
```

---

### Task 3: DM-RS based SNR definition (nvar / alpha), simulation-pinned

The DM-RS SNR does not depend on Qm and has no floor, so it is the better metric for QPSK/16QAM; the floor-inverted EVM SNR is kept because it is what the contract names and what `goal_metrics` can re-baseline on existing logs.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` (test `GpuPassSnr.DmrsSnrMatchesTruth`, `test_dmrs_snr_nl4_low_snr`)
- Create: `docs/superpowers/reports/csd/PFUSE-SNR-<date>.md` (text; definition and measured error)

**Interfaces:**
- Consumes: chest post outputs `nr_gpu_chest_post_out_t.nvar` and `nvar_ant[4]` (nr_gpu_pass_post.h:32-39: u32, per-RE noise after the chest's divisor), the estimate rows in `est_dev`, `GpuPassFixture::noise_db` (known synthetic SNR).
- Produces: `snr_dmrs_db[l]` per layer `= 10*log10(S_l / N)`, `S_l = mean over DM-RS REs and layers-l of sum_a |h_hat[a][l]|^2`, `N = nvar_a summed over antennas, same scaling as nvar` (the implementer fixes the exact integer scaling from `k_fir` / chest post (:25 comment: `sum_s mean_a(q) * w_s / nrb_total`) so that S and N are in the same units; the scale factor is derived analytically, never fitted per cell).

- [ ] **Step 1: Write the failing test.** Loop over `noise_db` in {3, 6, 10, 15, 20, 25}, shapes Nl in {1, 2, 4} x 48/217 PRB x Qm 2/8 from the fixture; run the pass; read `snr_dmrs_db` (new field in `nr_gpu_pass_result_t`, -99 when invalid); assert `|snr_dmrs_db - noise_db| <= 1.0` for Nl=1 and `<= 2.0` for Nl=2,4 (those tolerances are proposals: the fixture is AWGN with no multi-layer correlation, so tighten if the measurement is better, but never loosen without orchestrator approval); additionally for `noise_db`=3, Nl=4, Qm=2 assert `snr_dmrs_db` is finite (that is `test_dmrs_snr_nl4_low_snr`, the case where the EVM SNR is on the floor).
- [ ] **Step 2: Run, expect FAIL** (`snr_dmrs_db` absent): `... ./test_nr_gpu_pass --gtest_filter=GpuPassSnr.* < /dev/null`.
- [ ] **Step 3: Implement** the reduction as an extra output of the existing chest-post launcher (`nr_gpu_pass_launch_chest_post`), computed on the DM-RS rows already resident; no new kernel launch. Fill `snr_dmrs_db[4]` in the result.
- [ ] **Step 4: Run, expect PASS**; record the error table per (Nl, noise_db) in the report.
- [ ] **Step 5: Commit** (`git add` the two test files and the changed `nr_gpu_pass_post.h` / chest-post `.cu`).

---

### Task 4: Fused job struct with per-grant S/L, DM-RS mask and CSI-RS rate-match RE mask

T8-TIM-DEAD requires that the layout of REs is an INPUT of the kernel: class A needs S=1 L=5 (special slot, DM-RS symbol 2 only, mask 0x4 vs 0x804 for L=13); class B needs the rate-match set (rows 16/17 symbols 8,9,12,13 and NZP resources). Today the host runs the real `nr_dlsch_extract_rbs()` on index-coded inputs (`nr_dlsch_extract_geometry`, nr_pdsch_gpu_demod_host.c) and hands the result to the kernel; the fused job keeps that principle: no re-implementation of the extractor rules, only a compact device-readable form of its result.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fuse.h`
- Test: `tests/nr_gpu_pass_test.cc` (`GpuFuse.JobLayoutRoundTrip`)

**Interfaces:**
- Produces:
```c
#define NR_GPU_FUSE_MAX_NL 4
#define NR_GPU_FUSE_MAX_SYM 14
typedef struct {
  uint8_t  cell;                 /* multi-cell: index into the per-cell tables */
  uint8_t  nl, qm, nbrx;         /* layers, modulation order, RX branches */
  uint16_t rnti;
  uint16_t nrb, rb_start;        /* contiguous grant; segmented grants use seg_* (same rule as the pass graph, <= 16 segments) */
  uint8_t  start_symbol, nb_symbols;   /* S, L from the grant, never a constant */
  uint16_t sym_mask;             /* symbols demodulated (bit s) */
  uint16_t dmrs_sym_mask;        /* DM-RS symbols (0x804 for S1 L13, 0x4 for S1 L5), from dlDmrsSymbPos */
  uint16_t rm_re_mask_id;        /* index into the per-cell device table of CSI-RS/PDCCH/SSB rate-match RE masks; 0xFFFF = none */
  const uint32_t *rm_re_dev;     /* [14][ceil(nrb*12/32)] bitset per symbol, written by the host extractor, device-accessible */
  uint32_t nvar_fixed;           /* chest post normalisation, as nr_gpu_chest_post_par_t */
  /* outputs (device-written, host reads after the fence) */
  float    snr_nfa_db[NR_GPU_FUSE_MAX_NL];   /* floor-inverted EVM SNR; NR_SNR_UNKNOWN on the floor */
  float    snr_dmrs_db[NR_GPU_FUSE_MAX_NL];
  float    evm_pct;
  uint32_t flags;                /* bit0: floor, bit1: fp32 path used, bit2: invalid */
} nr_gpu_fuse_job_t;
typedef int (*nr_gpu_fuse_dev_fn)(const nr_gpu_fuse_job_t *job, nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *est_scratch, void *stream);
```
All fields are plain data; the device mask table is allocated through `nr_gpu_buf_alloc` at init (`N_CELL_MAX` x slot ring), filled by `nr_gpu_buf_h2d_async` unconditionally.

- [ ] **Step 1: Write the failing test** `JobLayoutRoundTrip`: build a job from the fixture for S1 L13 (mask 0x804) and S1 L5 (mask 0x4) with an rm mask covering symbols 8,9,12,13 of 12 REs/RB; assert the device-side copy of `sym_mask`, `dmrs_sym_mask` and the bit count of `rm_re_dev` equal the host values; assert the job is trivially copyable (`static_assert(std::is_trivially_copyable<nr_gpu_fuse_job_t>::value)`).
- [ ] **Step 2: Run, expect compile FAIL; Step 3: implement the header and `nr_gpu_fuse_job_from_exact(const nr_pdsch_gpu_demod_call_t*, nr_gpu_fuse_job_t*)` in `nr_pdsch_gpu_fuse_host.c` (copies from the exact path's already computed geometry); Step 4: run, expect PASS; Step 5: commit** (explicit paths).

---

### Task 5: Fused FP32 kernels (chest -> MMSE demod -> LLR -> EVM sums -> SNR) with accuracy tests first

Fusion boundary comes from Task 1's verdict. Default design (a): (1) the existing chest LS+FIR rows kernel (`k_ls`, `k_fir`) stays, its nvar row output feeds the next kernel directly from device memory (no host hop, the pass graph already does this); (2) ONE kernel `k_fuse_re` replaces `k_gather + k_level + k_mask_pw + k_align + k_mmse3_prep + k_tail`: a block handles a tile of REs of one symbol, reads y/h straight from the rxF grid and the estimate rows (no `y`/`h` gather buffers), applies `dmrs_sym_mask`/`rm_re_mask`, solves the Nl x Nl MMSE in FP32 in registers (cofactor form as `k_tail<MM>`, MM from `nl`), writes LLRs, and accumulates the EVM partial sums and the DM-RS/EVM SNR terms in shared memory with one atomic per block; (3) `k_fuse_out` (one block) folds, inverts the floor table (`nr_snr_floor_invert`) and writes the job outputs. If Task 1 shows chest dominating, do the symmetric change in the chest instead and record that in the report.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fuse.cu`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc`
- **[ORCHESTRATOR: cmake]** add `nr_pdsch_gpu_fuse.cu` to `pdsch_gpu` (the CUDA sources list near `openair1/PHY/CODING/CMakeLists.txt:106`), WITHOUT the `--fmad=false` option set for `nr_pdsch_gpu_demod.cu` (:110); then `heavy.sh cmake` re-run is the orchestrator's.

**Interfaces:**
- Consumes: Task 4 `nr_gpu_fuse_job_t`, `nr_snr_floor_invert`, `nr_pdsch_gpu_chest_dev`, `GpuPassFixture`.
- Produces: `extern "C" int nr_pdsch_gpu_fuse_dev(const nr_gpu_fuse_job_t *job, nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *est_scratch, void *stream);` launch-only, graph-capturable (no sync, no allocation), `-ENOSYS`-style negative code with a reason counter on unsupported shapes (the caller then uses the old path).

**Accuracy contract (stated BEFORE measuring; reference = existing FP64 exact path of the same fixture grant, i.e. `run_pass(m, 0)`):**

| Qm | LLR sign disagreement (all LLRs) | LLR sign disagreement among LLRs with abs(ref) >= 1/8 of the grant's mean abs LLR | CRC |
|---|---|---|---|
| 2 (QPSK) | <= 0.01 % | <= 0.005 % | CRC set identical |
| 4 (16QAM) | <= 0.05 % | <= 0.02 % | identical |
| 6 (64QAM) | <= 0.10 % | <= 0.05 % | identical |
| 8 (256QAM) | <= 0.10 % | <= 0.05 % | identical |

0.10 % is the cap of the existing gate (`nr_gpu_pass_dl.c:47`); lower Qm rows are stricter because their LLR margins are wider. These numbers are proposals fixed here; only the orchestrator loosens them. EVM sums: within 2e-3 relative of the FP64 chain (as `GpuPassFp32.EvmChainMatchesFp64Sums`). `snr_nfa_db` equals `nr_snr_floor_invert(10*log10(ref/err), qm)` computed from the FP64 sums to within 0.1 dB.

- [ ] **Step 1: Write the failing tests** (all FP32-fused vs FP64 exact on the same fixture grant)

```cpp
TEST(GpuFuse, LlrSignAndCrcPerQm) {
  // for nrb in {24, 217, 273}, nl in {1,2,3,4}, qm in {2,4,6,8}, noise_db in {6, 12, 20}, 4 seeds
  //   a = fx.run_pass(m, 0)  (exact FP64)    b = fx.run_fuse(m)   (nr_pdsch_gpu_fuse_dev)
  //   count_sign_agreement(a, b, &same, &total) per qm; strong subset via |a.llr| >= mean/8
  //   EXPECT per-qm disagreement fractions <= the table above; EXPECT a.tb_ok == b.tb_ok for every grant
}
TEST(GpuFuse, MasksMatchExactPath) {   // test_fuse_masks_match_exact_path
  // S1 L5 / dmrs 0x4, and S1 L13 / dmrs 0x804 with a CSI-RS rm mask on symbols 8,9,12,13: LLR count and sign agreement as above;
  // an all-zero mask and an unset rm_re_mask_id must give exactly the default-layout result (bitwise equal to the no-mask call).
}
TEST(GpuFuse, SnrNfaMatchesInvertedEvm) {
  // snr_nfa_db vs nr_snr_floor_invert(EVM from the FP64 sums): within 0.1 dB; floor grants (noise_db 3, qm 8) -> NR_SNR_UNKNOWN
}
```
`GpuPassFixture::run_fuse` is added to the fixture (same bound buffers as `run_pass`, calls `nr_pdsch_gpu_fuse_dev` then the existing LDPC pass tail, so CRC is real).

- [ ] **Step 2: Run, expect FAIL (symbol missing).** `heavy.sh ninja ... test_nr_gpu_pass` fails to link until Step 3.
- [ ] **Step 3: Implement `k_fuse_re` / `k_fuse_out` and `nr_pdsch_gpu_fuse_dev`.** Mandatory properties: grid = (RE tiles, symbols); FP32 only (verify with `tests/.../check_no_fp64_sass.sh` on the new object); no `cudaMalloc` or sync; scratch from the init-time buffer; `cell` is part of the scratch index; MM selection from `nl` identical to `k_tail<MM>` (QAM-FIX/MMSE3 mode per the existing `qmode` argument so the same decision as the exact path is reproduced, which is what makes the LLR comparison meaningful).
- [ ] **Step 4: Run, expect PASS.** `CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps heavy.sh ./test_nr_gpu_pass --gtest_filter=GpuFuse.* < /dev/null`, then the same with `ISAC_GPU_FORCE_DISCRETE=1`. If a Qm row is exceeded, report the measured fraction and stop (the orchestrator decides loosen vs fix); do not edit the table.
- [ ] **Step 5: Re-run the bench (Task 1 command) with the fused path** and add `fuse` rows to `BENCH` lines; the target is chest+fuse sum p99 <= 150 us at Qm 2-8, 273 x 4.
- [ ] **Step 6: Commit** (explicit paths: the `.cu`, the test, the fixture).

---

### Task 6: Host wiring behind `ISAC_PDSCH_GPU_FUSE=1`, FP32 gate, CHECK shadow, fallback, new log fields

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fuse_host.c` (env, gate, CHECK)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c` (`nr_pdsch_gpu_demod_grant`, ~:1118: when the fuse flag is on and the shape is supported, call the fused path; else fall through unchanged), `openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c` (the chest entry near :2198/:2304 skips the separate chest call when the fused job runs it), `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (`evm_fill` ~:449: set new output fields; do NOT touch `layer_snr_db`)
- Modify: the EVMGRANT log line emitter (grep `EVMGRANT` in `nr_pdsch_passive_queue.c`): append ` snr_nfa=<db,db,db,db> snr_dmrs=<db,db,db,db>` after the existing fields (existing parsers use `KV` regex, extra keys are ignored: `goal_metrics.KV`)
- Modify: `tests/passive_rx/goal_metrics.py` (read `snr_nfa` when present; opt-in flag `--snr nfa|legacy`, default `legacy` so that existing numbers and `test_goal_metrics.py` do not change) + `tests/passive_rx/test_goal_metrics.py`

**Interfaces:**
- Consumes: Task 4 job, Task 5 launcher, `nr_fp32_gate_t` / `nr_fp32_gate_note(&g, flip)` (nr_gpu_pass_fp32_gate.h), existing `ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=N` / `..._CHEST_CHECK=N` semantics.
- Produces: env `ISAC_PDSCH_GPU_FUSE` (0/1, default 0), `ISAC_PDSCH_GPU_FUSE_CHECK=N` (every N-th grant also runs the old exact path, compares per-Qm sign agreement and the CRC verdict; N=64 on the replay arms, same style as t26f's `CHECK=64`), counters `FUSE calls/ok/refused[reason]/check_ok/check_bad/gate` printed at teardown in the same line style as `GPU-DEMOD`.

- [ ] **Step 1: Write the failing tests**
  - `test_fuse_off_is_identical` (gtest): with `ISAC_PDSCH_GPU_FUSE` unset, the demod host call path produces outputs byte-identical to before (run the existing `GpuPassStages.DevEqualsOldEntryPoints` plus a new assertion that no `FUSE` counter moved).
  - `test_legacy_snr_l_unchanged_with_fuse` (gtest): with FUSE=1, `layer_snr_db[l]` (legacy EVM, CPU-folded) equals the FUSE=0 value to 1e-3 dB on the fixture.
  - `test_fuse_disable_falls_back` (gtest): inject a CHECK mismatch (hook `nr_gpu_fuse_test_force_flip(n)`); after the gate trips, the next grant returns through the old path, no job is lost, counters show `refused[gate]`.
  - python: `test_goal_metrics.py::test_snr_nfa_optin` — a synthetic EVMGRANT line with `snr_nfa=` is used only with `--snr nfa`; default output unchanged.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement.** Gate rule = `nr_fp32_gate_note` with flip = (CRC verdict differs) and the sign-disagreement test of Task 5's per-Qm table (> the "all LLRs" row ⇒ trip); on trip call the same disable function as `nr_gpu_pass_fp32_shadow` (log reason string). Unsupported shape (PT-RS, DM-RS type 2, probes, >16 segments, etc. = the pass graph's own exclusions, `nr_gpu_pass.h` header comment) returns -1 with a reason counter, old path runs.
- [ ] **Step 4: Build and run:** `heavy.sh ninja -C <build> -j6 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests < /dev/null`, then `heavy.sh ctest -j4 --output-on-failure < /dev/null` (known ARM failures: dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256/64) and the python tests.
- [ ] **Step 5: Commit** (explicit paths of every file above).

---

### Task 7: Replay A/B gate (no radio)

**Files:**
- Create: `tests/passive_rx/dgx/pfuse_ab.sh` (wrapper that runs the four arms sequentially) — or run the commands by hand; report `docs/superpowers/reports/csd/PFUSE-AB-<date>.md` (text)

**Interfaces:** consumes `p99_replay.sh` (`CAPSET=b|tim`, `B=`, `ENVFILE=`, `TMO=`), `p99_stats.py`, `goal_metrics.py`, `PDTIM`/`DECODETIME` stage lines (chest and demod stage us per grant, `ISAC_PDTIM_PCT=1 ISAC_PDTIM_CLASS=1` already in t26f).

- [ ] **Step 1: Arms** (n = 3 each per capture, interleaved A B A B A B, never pooled across V/T; base env = `t26f.env`, which already has `ISAC_PDSCH_GPU_EXACT_CHEST=1 ISAC_PDSCH_GPU_EXACT_DEMOD=1`):
  - A: base. B: base + `ISAC_PDSCH_GPU_FUSE=1 ISAC_PDSCH_GPU_FUSE_CHECK=64`. C (accuracy): base + `ISAC_PDSCH_GPU_FUSE=1 ISAC_PDSCH_GPU_FUSE_CHECK=1` (every grant shadowed).
  Command per arm (build dir `build_t26` or the orchestrator's chosen build; `pgrep -x nr-uesoftmodem` empty first):
  `CAPSET=b CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps /home/nicola/NICOLA/wt/rr-csd-int16/tests/passive_rx/p99_replay.sh pfuse_b_A1 < /dev/null` and `... p99_replay.sh pfuse_b_B1 ISAC_PDSCH_GPU_FUSE=1 ISAC_PDSCH_GPU_FUSE_CHECK=64 < /dev/null`; repeat with `CAPSET=tim`. (p99_replay.sh takes heavy.lock itself; the outer `heavy.sh` is not added, as its header says it takes `heavy.lock` through heavy.sh internally.)
- [ ] **Step 2: Analyse** (stdlib, one at a time, `nice -n 19 taskset -c 17 timeout 900 python3 tests/passive_rx/goal_metrics.py --json <run.log>` and `p99_stats.py`):
  1. `crc_q` (legacy `snr_l` qualification) per arm and capture: B equals A within run-to-run spread (the A1/A2/A3 spread sets the band; report both), and C shows `check_bad = 0` and zero CRC-set differences in the `FUSE` teardown line.
  2. Chest+demod stage p99 (DECODETIME/PDTIM lines) at 273 PRB x 4 L grants, and over all grants: target <= 150 us for the 273 x 4 class. The replays contain few 273 x 4 grants; the report states n for the class; if n < 30 it says "not verified on air-like data" and uses the Task 5 bench number, labelled `[MEASURED, kernel bench]`.
  3. `tot` p99 and drops, to confirm no regression.
  4. New fields: for each Qm, scatter summary of `snr_nfa` vs `snr_dmrs` (median difference, p10/p90), floor-fraction: how many grants are UNKNOWN.
  5. Re-baseline: run `goal_metrics.py --snr nfa` on arm B and report the qualified-failure fraction next to the legacy 90 % (T) / 65 % (V) of DLCRC-ROOTCAUSE (labelled `[MEASURED, DGX replay, ...]`).
- [ ] **Step 3: Run once with `ISAC_GPU_FORCE_DISCRETE=1`** (arm B, `CAPSET=b` only): functional, no timing claim.
- [ ] **Step 4: Report** with evidence labels; acceptance list (all must be satisfied):
  - LLR sign and CRC set: Task 5 per-Qm table met on the test fixture AND `check_bad=0`, zero CRC-set differences on arm C of both captures.
  - Replay A/B: chest+demod p99 <= 150 us at 273 PRB x 4 L (or labelled not verified as above).
  - `CAPSET=b` and `CAPSET=tim` `crc_q` unchanged (within the A-arm spread).
  - Orchestrator-only afterwards: sens6 FP32 timing at the phase gate, OTA A/B interleaved vs t26f, daytime, n >= 3 per cell, V and T separate (spec section 8 gates 3-4). Not part of this plan.
- [ ] **Step 5: Commit** the wrapper (if created) with explicit paths; the report is saved and committed by the orchestrator.

---

## Self-review

- **Contract coverage:** step-0 bench with exact command (Task 1); fused kernel behind `ISAC_PDSCH_GPU_FUSE=1`, default off (Tasks 4-6); FP32 behind the existing gate (Task 5/6, `nr_fp32_gate_note`); `noise_floor_aware_snr` as the inverse of the `true_snr_db` table per Qm (Task 2, emitted as `snr_nfa_db`; the contract name `noise_floor_aware_snr` of `nr_gpu_pdsch_result_t` maps to this field) and DM-RS nvar/alpha (Task 3); per-grant S/L, DM-RS mask, rate-match mask as inputs (Task 4, T8-TIM-DEAD); LLR sign + CRC set with per-Qm tolerance (Task 5); replay p99 and crc_q unchanged for both captures (Task 7).
- **Placeholders:** none intended; the places where the implementer must read code before writing (exact nvar scaling in Task 3, `run_pass` event insertion in Task 1, integration points in Task 6) are marked with the file and line to read, since those depend on code not reproduced here.
- **Type consistency:** `nr_gpu_fuse_job_t`, `nr_pdsch_gpu_fuse_dev`, `nr_snr_floor_invert`, `NR_SNR_UNKNOWN`, `snr_nfa_db`, `snr_dmrs_db` are used with the same names in Tasks 2-7.
- **Not checked:** whether the fixture can build a 273 PRB x 4 L Qm 8 grant within `NR_GPU_PASS_MAX_CB`=32 (a 273 x 4 x 12 sym Qm 8 TB has more than 32 CBs; the bench/accuracy tests may need a reduced MCS or only the chest/demod part with LDPC skipped; Task 1 Step 1 must resolve this and say so); the `ISAC_GPU_PASS` graph is not in t26f (`t26f.env` has `EXACT_DEMOD/CHEST/LDPC=1` but no `ISAC_GPU_PASS`), so the replay A/B exercises the exact-path wiring of Task 6, not the graph; extending the fused unit into `nr_gpu_pass_graph.cu` is a follow-up if `ISAC_GPU_PASS` becomes default.
