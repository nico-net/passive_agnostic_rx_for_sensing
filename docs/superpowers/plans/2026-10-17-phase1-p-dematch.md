# P-DEMATCH (GPU rate-dematch + deinterleave into the LDPC input) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the serial CPU `prep_segment` (deinterleave + rate-dematch + int8 pack, 0.32-0.38 ms at CB16+) of the DL exact-GPU-LDPC path by one GPU kernel that writes the LDPC input in `NR_GPU_LDPC_DEV_IN` layout, chained on one stream into the existing device LDPC launch, behind `ISAC_PDSCH_GPU_DEMATCH=1` (default off), bit-exact with the CPU, cell index in the job.

**Architecture:** The dematch kernel already exists: `k_rdm` (`nr_gpu_pass_post.cuh:146`, launcher `nr_gpu_pass_launch_rdm`, same `libpdsch_gpu.so`), bit-exact vs `prep_segment` on 1000 random seeds (`GpuPassPost.RateDematchMatchesCpu`). It is only wired into the GPUPASS graph (off in t26f). This plan adds (1) a header `nr_gpu_dm.h` with the job geometry (S/L, DM-RS REs, rate-matched REs -> G -> E/E2/first_rE2) usable on host and device, (2) a module entry `nr_pdsch_gpu_dm_run` in `nr_pdsch_gpu_ldpc.cu` = stage LLR -> `k_rdm` -> `dev_impl` -> collect, on a private pass-instance context, (3) the host switch in `gpu_tb` (`nr_pdsch_gpu_ldpc_host.c`) with a per-class shadow check against `prep_segment`, (4) replay acceptance. CPU path stays the reference and the fallback for every refusal.

**Tech Stack:** C/CUDA (OAI nr-uesoftmodem fork, `libpdsch_gpu.so`), gtest (`test_nr_gpu_pass`), Python stdlib analysis (`stage_split.py`, `goal_metrics.py`).

**Spec:** `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` (section 1.1 row 5, section 3 memory model, section 5 multi-cell API, section 7 rank 8, section 8 gates). Parent plan: `docs/superpowers/plans/2026-10-16-phase1-dl-g1-g3.md` Task 10 (contract is binding; the deviations found while reading code are listed under "Contract deltas"). Evidence: `docs/superpowers/reports/csd/T5-LDPC-REASONS-20261016.md`, `T8-TIM-DEAD-20261016.md`.

## Global Constraints
- Worktree `/home/nicola/NICOLA/wt/rr-csd-int16`, branch `csd/int16`, build dir `cmake_targets/ran_build/build_t26`. Builds only `/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 -C cmake_targets/ran_build/build_t26 <targets> < /dev/null`. Agents cannot run cmake: Task 2 Step 9 adds a ctest target, the orchestrator re-runs cmake there (say so in the report and stop that step).
- Never build while one of OUR receivers runs: `for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe; done | grep -q ^/home/nicola/` must be false. Never touch other users' processes. Bounded waits only.
- Agents never run OTA, uhd_*, X410, DPDK or firmware tools. All validation here is gtest, replay, rfsim. OTA A/B is the orchestrator's (daytime, n >= 3 per cell).
- CUDA programs and ctest: `mkdir -p /tmp/nicola_nomps; export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps`. Test binaries run through `heavy.sh` with `< /dev/null`, cwd = build dir (they `dlopen("./libpdsch_gpu.so")`).
- Replays one at a time via `tests/passive_rx/p99_replay.sh` (it takes heavy.lock itself through heavy.sh; call it directly with `< /dev/null`, do not nest another lock), `ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env`, `CAPSET=b` (Vodafone `raw_pci415_12s_b`) or `CAPSET=tim` (`raw_pci923_tim_12s_tile3`), `TMO=90`. Label `[MEASURED, DGX replay <cap>, <arm>, n=..]`; never pool b with tim, never mix with OTA.
- Default-off env flag `ISAC_PDSCH_GPU_DEMATCH=1`; CPU `prep_segment` stays reference and fallback. No per-cell constants (no PCI, frequency, operator, bandwidth in code or tests).
- New GPU code: cell index in every job, allocation only through `nr_gpu_buf_alloc` (pre-fence, in init), `nr_gpu_buf_h2d_async/d2h_async` called unconditionally (DIRECT no-op, DISCRETE copy), one CUDA context, greatest-priority streams (the pass-instance streams of `nr_pdsch_gpu_ldpc_init` already are), one `ISAC_GPU_FORCE_DISCRETE=1` functional run on the DGX.
- ISAC_RX_BRANCH_FO stays unset. sens6-frozen paths never edited; new configs under `tests/passive_rx/` use `.cfg`. Stop receivers with SIGINT only.
- `git add <explicit paths>`, never -A, never stash. Trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y`. Retry a commit on `.git/index.lock` 5 s apart, max 5.
- Evidence label on every claim about current behaviour (`[CODE-READ file:line]`, `[MEASURED ...]`, `[INFERRED]`).

## Contract deltas (read before coding; found by reading the code, all [CODE-READ])
1. **Kernel: reuse `k_rdm`, not `cb0_dematch_kernel`.** `cb0_dematch_kernel` (`nr_td_cb0_batch.cu:44-62`) takes one E per item and host-precomputed `k0/Ncb/Foffset` items (`nr_td_cb0_batch.h` item has no E2/first_rE2), applies a K38 `llr_shift`, and lives in `libtd_cb0_gpu.so` (a different DSO and context than `nr_pdsch_gpu_ldpc_dev`). `k_rdm` (`nr_gpu_pass_post.cuh:146-186`) already takes `rv, tbslbrm, F, K, Kprime, C, E` per segment (E2 is per-segment E), writes `Kc*Z` int8 at `i * NR_GPU_LDPC_DEV_IN_STRIDE`, and is in the same `libpdsch_gpu.so` as `nr_pdsch_gpu_ldpc_dev`. Bit-exactness vs `prep_segment` is already unit-tested (`nr_gpu_pass_test.cc:762` `RateDematchMatchesCpu`, with rv 0-3, LBRM, repetition, F, Z in {2,7,11,...,384}). So the work is wiring + geometry + safety, not a new kernel. The parent plan's "extend cb0_dematch_kernel to DL rv" is not needed.
2. **T-DRA S/L and T-DEAD mask reach the dematch only through G.** `G = ((12*L - dmrs_re) * nprb - unav_res) * Qm * Nl` (`nr_tbs_tools.c:15-20`; passive call `nr_pdsch_passive_decode.c:3004` with `unav_res = ptrs_unav + csi_unav + ssb_unav`); E/E2/first_rE2 are functions of G, C, Qm, Nl (`nr_tbs_tools.c:22-37`, loop at `nr_pdsch_passive_decode.c:1869-1878`). The per-RE mask changes which REs the demod emits (P-FUSE's concern), not the dematch. So "carried on device" = the job holds `nsym (=L), dmrs_re, unav_res` and the device-callable `nr_gpu_dm_geom()` derives G/E/E2/first_rE2; the host refuses the GPU path if that disagrees with the CPU's G (guards a T-DRA/T-DEAD change that updates G on one side only). T-DRA/T-DEAD themselves are not implemented yet (T8 report: contracts only); this task makes them free for the dematch.
3. **`nr_pdsch_gpu_ldpc_dev` owns ONE context** (`dev_impl(NR_GPU_PASS_RING, ...)`, `nr_pdsch_gpu_ldpc.cu:~785`): concurrent DL threads (cores 7,8,9,3,4) would serialise on it. We use the per-instance contexts `g_ctx[CTX+i]`, i < `NR_GPU_PASS_RING` (=4), which are idle when GPUPASS is off; DM refuses to init when `ISAC_GPU_PASS` or `ISAC_UL_GPU_PASS` is set. No free instance -> `NR_GPU_LDPC_E_BUSY` -> CPU prep (counted, never blocks).
4. **The TB parameter struct has no cell field** (`nrLDPC_coding_interface.h:49-91`). Task 3 adds `dm_cell` (+ geometry fields) set by the passive receiver. Value is 0 until multi-cell lands; every layer carries it and bounds-checks it against `NR_GPU_DM_CELL_MAX`.
5. **`tb->d` is not written by the GPU dematch.** `t_cap_req` (GrantWork CB0 equivalence check, `nr_pdsch_passive_decode.c:1944`) reads `TB_parameters.d` after the decode; so the receiver sets `dm_allow = !t_cap_req` per TB. The CPU fallback recomputes `d` itself because `d_to_be_cleared` is required (`decode_impl`, host.c ~234).
6. **Worktree precondition.** `git status` at plan time shows uncommitted edits (the T5 follow-up: K%8 snapshot padding) in `nr_pdsch_gpu_ldpc.cu/.h`, `nr_pdsch_gpu_ldpc_host.c`, `nr_pdsch_gpu_ldpc_test.c`. Do not start until those are committed (ask the orchestrator); anchor all edits on the text quoted here, line numbers are approximate.

## Review Focus
1. A segment the CPU refuses or that `k_rdm` zero-fills (C=0, Foffset>Ncb, E%Qm!=0, bad Qm, Kprime out of range) must behave exactly like `rm_bad` today: block fails, no abort, no decode attempt. Pinned by `GpuDm.SegBadMatchesKernelZeroFill` (Task 1) and the unchanged `rm_bad[k]` branch in Task 3.
2. A stale or wrong geometry (G recomputed on one side with a different L or rate-matched RE count, i.e. exactly what T-DRA/T-DEAD will change) must refuse the GPU dematch (CPU prep), never decode with a wrong E. Pinned by `GpuDm.GeomTracksSlAndMask` (Task 1) and counter `dm_geom` (Task 3).
3. A TB with C > 32 (several launches) and a layout probe (`nb_segments_to_decode` = 1 of a bigger C) must stage the right LLR window and keep absolute offsets. Pinned by `GpuDm.WindowedChunksMatchCpu` (Task 2).
4. Concurrent DL threads: a busy/full context pool must fall back to the CPU, never wait, never corrupt another thread's buffers. Pinned by `GpuDm.ConcurrentThreadsNeverCorrupt` (Task 2).
5. DISCRETE (sens6, `ISAC_GPU_FORCE_DISCRETE=1`): staging offsets, snapshot tail bytes of K%8!=0 blocks, and result copy-back must be identical to DIRECT. Pinned by running all `GpuDm.*` in the discrete ctest (Task 2 Step 9) and the forced-discrete replay (Task 4).

## File map
- Create `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_dm.h` - job geometry, segment fill/refusal predicates, module entry typedefs (host+device inline).
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu` - `nr_pdsch_gpu_dm_init/run/stats`.
- Modify `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h` - TB fields `dm_*`.
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` - fill the TB fields.
- Modify `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c` - DM branch, CHECK, class table, stats.
- Modify `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` - `GpuDm.*` tests (no new target; cpu_prep, Dev<>, PostSyms already there).
- Modify `openair1/PHY/CODING/CMakeLists.txt` - one `add_test` for the discrete run (orchestrator cmake re-run).

---

### Task 1: Geometry header (S/L, DM-RS REs, rate-matched REs -> G/E/E2) and segment predicates

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_dm.h`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc` (append at end of file)

**Interfaces:**
- Produces (all `static inline`, `NRDM_HD` = `__host__ __device__` under nvcc, empty in C):
  - `typedef struct { uint16_t nprb, nsym; uint32_t dmrs_re, unav_res; uint8_t Qm, Nl; } nr_gpu_dm_geom_in_t;` (`nsym` = PDSCH L = `number_symbols`; `dmrs_re` = `nb_re_dmrs * dmrs_len` per PRB; `unav_res` = rate-matched REs of the whole grant)
  - `typedef struct { uint32_t G; int E, E2, first_rE2; } nr_gpu_dm_geom_t;`
  - `nr_gpu_dm_geom_t nr_gpu_dm_geom(const nr_gpu_dm_geom_in_t *g, int C);` (E = -1, G = 0 when Qm or Nl is 0, as `nr_get_E`)
  - `int nr_gpu_dm_seg_bad(const nr_gpu_rdm_seg_t *s);` (1 = the segment is refused / zero-filled; mirrors `k_rdm`'s `bad`)
  - `typedef struct { uint8_t BG, Qm, rv; uint32_t Z, K, F, Kprime, C, tbslbrm; int E, E2, first_rE2; } nr_gpu_dm_tb_t;`
  - `void nr_gpu_dm_seg_fill(const nr_gpu_dm_tb_t *t, uint32_t r, nr_gpu_rdm_seg_t *s);` (llr_off exactly as `prep_segment`: `r*E` for `r < first_rE2`, else `first_rE2*E + (r-first_rE2)*E2`)
  - `#define NR_GPU_DM_CELL_MAX 4`, `#define NR_GPU_DM_LLR_MAX (273u*12u*14u*4u*8u)` (largest G: 273 PRB, 14 sym, 4 layers, 256QAM)
  - `typedef int (*nr_gpu_dm_run_fn)(nr_gpu_ldpc_job_t *jobs, const nr_gpu_rdm_seg_t *segs, int n, uint8_t cell, const int16_t *llr, uint32_t llr_n, int8_t *chk);` and `typedef int (*nr_gpu_dm_init_fn)(void); typedef void (*nr_gpu_dm_stats_fn)(uint64_t *o);` (implemented in Task 2)

- [ ] **Step 1: Write the failing tests** (append to `nr_gpu_pass_test.cc`; add near the other `extern "C"` declarations: `uint32_t nr_get_G(uint16_t, uint16_t, uint8_t, uint16_t, uint32_t, uint8_t, uint8_t); int nr_get_E(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t);` and `#include "nr_gpu_dm.h"` after `nr_gpu_pass_post.h`; both symbols are already linked into this test, [MEASURED nm of build_t26/test_nr_gpu_pass: `T nr_get_E`, `T nr_get_G`])

```cpp
/* ===================== P-DEMATCH: geometry and segment predicates (CPU only, no device needed) ===================== */
TEST(GpuDm, GeomMatchesCpu) {
  std::mt19937 g(77);
  for (int it = 0; it < 20000; it++) {
    nr_gpu_dm_geom_in_t in = {};
    in.nprb = 1 + g() % 273; in.nsym = 1 + g() % 14;
    in.dmrs_re = (g() % 3) * (g() % 13);                       /* nb_re_dmrs * dmrs_len per PRB */
    in.unav_res = g() % 4 ? 0 : g() % (in.nprb * 12);          /* a few rate-matched REs */
    const uint8_t qms[] = {2, 4, 6, 8};
    in.Qm = qms[g() % 4]; in.Nl = 1 + g() % 4;
    if ((12u * in.nsym) <= in.dmrs_re) continue;               /* not a real grant */
    const int C = 1 + g() % 150;
    const uint32_t G = nr_get_G(in.nprb, in.nsym, in.dmrs_re > 255 ? 255 : in.dmrs_re, 1, in.unav_res, in.Qm, in.Nl);
    if (in.dmrs_re > 255) continue;
    const nr_gpu_dm_geom_t o = nr_gpu_dm_geom(&in, C);
    ASSERT_EQ(G, o.G) << it;
    ASSERT_EQ(nr_get_E(G, C, in.Qm, in.Nl, 0), o.E) << it;
    int e2 = o.E, fr = C;                                      /* the loop of passive_ldpc_decode_core */
    for (int r = 1; r < C; r++) { int e = nr_get_E(G, C, in.Qm, in.Nl, r); if (e != o.E) { e2 = e; fr = r; break; } }
    ASSERT_EQ(e2, o.E2) << it; ASSERT_EQ(fr, o.first_rE2) << it;
  }
  nr_gpu_dm_geom_in_t z = {}; z.nprb = 10; z.nsym = 12;        /* Qm = 0 or Nl = 0: nr_get_E returns -1 */
  EXPECT_EQ(-1, nr_gpu_dm_geom(&z, 3).E);
}

/* T-DRA changes L (special slot S1 L5), T-DEAD changes the rate-matched REs: G, E and the E/E2 split must follow, and
 * the dematch must see a different E for the same C (this is what the host check refuses when only one side moved). */
TEST(GpuDm, GeomTracksSlAndMask) {
  nr_gpu_dm_geom_in_t base = {}; base.nprb = 52; base.nsym = 13; base.dmrs_re = 12 * 2; base.Qm = 6; base.Nl = 2;
  nr_gpu_dm_geom_in_t sl = base; sl.nsym = 5;                  /* row 1: L = 5 */
  nr_gpu_dm_geom_in_t mk = base; mk.unav_res = 4 * 12 * 52 / 6; /* a periodic CSI-RS-type mask */
  const nr_gpu_dm_geom_t a = nr_gpu_dm_geom(&base, 4), b = nr_gpu_dm_geom(&sl, 4), c = nr_gpu_dm_geom(&mk, 4);
  EXPECT_GT(a.G, b.G); EXPECT_GT(a.G, c.G);
  EXPECT_NE(a.E, b.E); EXPECT_NE(a.E, c.E);
  EXPECT_EQ(nr_get_G(52, 5, 12, 2, 0, 6, 2), b.G);
  EXPECT_EQ(nr_get_G(52, 13, 12, 2, mk.unav_res, 6, 2), c.G);
}

TEST(GpuDm, SegFillOffsetsMatchPrepSegment) {
  nr_gpu_dm_tb_t t = {}; t.BG = 1; t.Qm = 6; t.rv = 2; t.Z = 384; t.K = 22 * 384; t.F = 0; t.Kprime = t.K; t.C = 5;
  t.E = 6000; t.E2 = 6012; t.first_rE2 = 2; t.tbslbrm = 0;
  nr_gpu_rdm_seg_t s;
  for (uint32_t r = 0; r < t.C; r++) {
    nr_gpu_dm_seg_fill(&t, r, &s);
    const uint32_t off = r < (uint32_t)t.first_rE2 ? r * t.E : t.first_rE2 * t.E + (r - t.first_rE2) * t.E2; /* prep_segment */
    EXPECT_EQ(off, s.llr_off); EXPECT_EQ(r < (uint32_t)t.first_rE2 ? 6000u : 6012u, s.E);
    EXPECT_EQ(2u, s.rv); EXPECT_EQ(5u, s.C); EXPECT_EQ(384u, s.Z);
  }
}

TEST(GpuDm, SegBadMatchesKernelZeroFill) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.rdm);
  nr_gpu_rdm_seg_t good = {0, 3000, 6, 1, 64, 22 * 64, 0, 22 * 64, 1000000, 1, 0};
  nr_gpu_rdm_seg_t bad[6]; for (auto &b : bad) b = good;
  bad[0].C = 0; bad[1].Qm = 3; bad[2].E = 3001; bad[3].rv = 4; bad[4].Kprime = 0; bad[5].F = 66 * 64; /* Foffset wraps past Ncb */
  EXPECT_EQ(0, nr_gpu_dm_seg_bad(&good));
  Dev<int16_t> dllr; Dev<nr_gpu_rdm_seg_t> dsegs; Dev<int8_t> dout;
  std::vector<int16_t> llr(4000, 5); dllr.up(llr.data(), llr.size() * 2);
  for (auto &b : bad) {
    ASSERT_EQ(1, nr_gpu_dm_seg_bad(&b));
    dsegs.up(&b, sizeof b); dout.up(std::vector<int8_t>(NR_GPU_LDPC_DEV_IN_STRIDE, 55).data(), NR_GPU_LDPC_DEV_IN_STRIDE);
    ASSERT_EQ(0, S.rdm(dllr.p, dsegs.p, 1, dout.p, nullptr));
    std::vector<int8_t> got(68 * 64); dout.down(got.data(), got.size());
    for (int8_t v : got) ASSERT_EQ(0, v);
  }
}
```

- [ ] **Step 2: Run, expect FAIL to compile** (`nr_gpu_dm.h` missing). `cd /home/nicola/NICOLA/wt/rr-csd-int16 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 -C cmake_targets/ran_build/build_t26 test_nr_gpu_pass < /dev/null 2>&1 | tail -20` -> `fatal error: nr_gpu_dm.h: No such file`.

- [ ] **Step 3: Create `openair1/PHY/NR_UE_TRANSPORT/nr_gpu_dm.h`**

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* P-DEMATCH (ISAC_PDSCH_GPU_DEMATCH=1, default off): GPU rate dematching + deinterleaving written straight into the LDPC input
 * (NR_GPU_LDPC_DEV_IN layout). The kernel is k_rdm (nr_gpu_pass_post.cuh); this header is the job description:
 *  - geometry: G = ((12*L - dmrs_re) * nprb - unav_res) * Qm * Nl as nr_get_G (nr_tbs_tools.c), E/E2/first_rE2 as nr_get_E. A new
 *    start/length (T-DRA) or a rate-match mask (T-DEAD) reaches the dematch ONLY through L and unav_res, so the job carries those;
 *  - per-segment fill and the refusal predicate (mirrors k_rdm's `bad`: such a segment is written as zeros and the host treats it
 *    as prep_segment's -1);
 *  - the module entry points of libpdsch_gpu.so (nr_pdsch_gpu_ldpc.cu). */
#ifndef NR_GPU_DM_H
#define NR_GPU_DM_H
#include <stdint.h>
#include "nr_gpu_pass_post.h"
#include "nr_pdsch_gpu_ldpc.h"
#ifdef __CUDACC__
#define NRDM_HD __host__ __device__
#else
#define NRDM_HD
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define NR_GPU_DM_CELL_MAX 4 /* cells (PCIs) served by one process; the job carries the index, the contexts are shared */
#define NR_GPU_DM_LLR_MAX (273u * 12u * 14u * 4u * 8u) /* LLRs of the largest grant: 273 PRB, 14 symbols, 4 layers, 256QAM */

typedef struct {
  uint16_t nprb, nsym; /* nsym = PDSCH length L (number_symbols) */
  uint32_t dmrs_re;    /* nb_re_dmrs * dmrs_len, per PRB */
  uint32_t unav_res;   /* REs removed by rate matching over the whole grant (CSI-RS, ZP, SSB, PTRS) */
  uint8_t Qm, Nl;
} nr_gpu_dm_geom_in_t;
typedef struct { uint32_t G; int E, E2, first_rE2; } nr_gpu_dm_geom_t;

static inline NRDM_HD int nr_gpu_dm_E(uint32_t G, int C, int Qm, int Nl, int r)
{
  const uint32_t q = (uint32_t)(Nl * Qm);
  return (r <= C - (int)((G / q) % (uint32_t)C) - 1) ? (int)(q * (G / (q * (uint32_t)C))) : (int)(q * (G / (q * (uint32_t)C) + 1));
}
static inline NRDM_HD nr_gpu_dm_geom_t nr_gpu_dm_geom(const nr_gpu_dm_geom_in_t *g, int C)
{
  nr_gpu_dm_geom_t o;
  o.G = 0; o.E = o.E2 = -1; o.first_rE2 = C;
  if (g->Qm == 0 || g->Nl == 0 || C < 1) return o;
  o.G = (12u * g->nsym - g->dmrs_re) * g->nprb * g->Qm * g->Nl;
  o.G -= g->unav_res * g->Qm * g->Nl;
  o.E = o.E2 = nr_gpu_dm_E(o.G, C, g->Qm, g->Nl, 0);
  for (int r = 1; r < C; r++) {
    const int e = nr_gpu_dm_E(o.G, C, g->Qm, g->Nl, r);
    if (e != o.E) { o.E2 = e; o.first_rE2 = r; break; }
  }
  return o;
}

/* 1 = refused / zero-filled by k_rdm (same conditions, same order as the kernel) */
static inline NRDM_HD int nr_gpu_dm_seg_bad(const nr_gpu_rdm_seg_t *s)
{
  const uint32_t Z = s->Z, N = s->BG == 1 ? 66 * Z : 50 * Z;
  if (s->C == 0 || (s->BG != 1 && s->BG != 2) || s->rv > 3 || (s->Qm != 2 && s->Qm != 4 && s->Qm != 6 && s->Qm != 8)) return 1;
  uint32_t Ncb = N;
  if (s->Tbslbrm != 0) { const uint32_t nref = 3 * s->Tbslbrm / (2 * s->C); Ncb = nref < N ? nref : N; }
  const uint32_t Foffset = s->K - s->F - 2 * Z;
  return Foffset > Ncb || s->E % s->Qm != 0 || s->Kprime < 2 * Z || s->Kprime > s->K;
}

typedef struct { uint8_t BG, Qm, rv; uint32_t Z, K, F, Kprime, C, tbslbrm; int E, E2, first_rE2; } nr_gpu_dm_tb_t;
static inline NRDM_HD void nr_gpu_dm_seg_fill(const nr_gpu_dm_tb_t *t, uint32_t r, nr_gpu_rdm_seg_t *s)
{
  const int second = r >= (uint32_t)t->first_rE2;
  s->E = (uint32_t)(second ? t->E2 : t->E);
  s->llr_off = second ? (uint32_t)t->first_rE2 * (uint32_t)t->E + (r - (uint32_t)t->first_rE2) * (uint32_t)t->E2 : r * (uint32_t)t->E;
  s->Qm = t->Qm; s->BG = t->BG; s->Z = t->Z; s->K = t->K; s->F = t->F; s->Kprime = t->Kprime; s->Tbslbrm = t->tbslbrm; s->C = t->C; s->rv = t->rv;
}

/* libpdsch_gpu.so (nr_pdsch_gpu_ldpc.cu). run: stage llr[lo..hi) -> k_rdm -> nr_pdsch_gpu_ldpc_dev on one stream, wait, fill jobs[k].nsnap /
 * syn0 / snap exactly as nr_pdsch_gpu_ldpc_run. jobs[k].llr is ignored. chk (nullable): n * NR_GPU_LDPC_DEV_IN_STRIDE bytes receive Kc*Z dematched
 * bytes per block (CHECK / tests). Returns 0 or an NR_GPU_LDPC_E_* (the caller runs the CPU prep + run); never blocks on a busy pool. */
typedef int (*nr_gpu_dm_init_fn)(void);
typedef int (*nr_gpu_dm_run_fn)(nr_gpu_ldpc_job_t *jobs, const nr_gpu_rdm_seg_t *segs, int n, uint8_t cell, const int16_t *llr, uint32_t llr_n, int8_t *chk);
/* o[0] calls, o[1] code blocks, o[2] busy, o[3] cap, o[4] errors, o[5 .. 5 + NR_GPU_DM_CELL_MAX) calls per cell */
typedef void (*nr_gpu_dm_stats_fn)(uint64_t *o);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 4: Build and run Task 1 tests**
`... ninja -j6 -C cmake_targets/ran_build/build_t26 test_nr_gpu_pass < /dev/null` then
`cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26 && mkdir -p /tmp/nicola_nomps && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh env CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps ./test_nr_gpu_pass --gtest_filter='GpuDm.*' < /dev/null`
Expected: 4 tests PASS (`GeomMatchesCpu`, `GeomTracksSlAndMask`, `SegFillOffsetsMatchPrepSegment`, `SegBadMatchesKernelZeroFill`). If `GeomMatchesCpu` fails on a `dmrs_re` case, the test's `nb_re_dmrs`/`dmrs_len` split (255 cap) is the cause, not the header: keep `dmrs_re <= 255`.

- [ ] **Step 5: Commit**
```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_gpu_dm.h openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc
git commit -m "feat(dematch): P-DEMATCH geometry (S/L, rate-matched REs -> G/E/E2) and segment predicates, tests"
```

---

### Task 2: Module entry `nr_pdsch_gpu_dm_run` (stage -> k_rdm -> device LDPC -> collect) with bit-exact and concurrency tests

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu` (append after the `nr_pdsch_gpu_ldpc_pass_dev` definition, before the `ULLDPC` comment block)
- Modify: `openair1/PHY/CODING/CMakeLists.txt` (one `add_test`, Step 9)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc`

**Interfaces:**
- Consumes: Task 1 header; `dev_impl(int inst, const nr_gpu_ldpc_job_t*, int n, const nr_gpu_buf_t *llr_dev, nr_gpu_buf_t *bits_dev, void *stream)` (static, same TU); `nr_gpu_pass_launch_rdm(const int16_t *llr, const nr_gpu_rdm_seg_t*, int, int8_t *ldpc_in, void *stream)`; `g_ctx[CTX + i].s`; `NJ`, `CTX`, `g_ok`, `nr_gpu_poll_relax`; `nr_gpu_buf_*`.
- Produces: `extern "C" int nr_pdsch_gpu_dm_init(void)`; `extern "C" int nr_pdsch_gpu_dm_run(...)` (signature = `nr_gpu_dm_run_fn`); `extern "C" void nr_pdsch_gpu_dm_stats(uint64_t *o)`.

- [ ] **Step 1: Write the failing tests** (append to `nr_gpu_pass_test.cc`, after the Task 1 tests; they use the existing `cpu_prep`, `Dev`, `have_gpu`, `pass_model()`; add `int nr_get_R_ldpc_decoder(int, int, int, int, int *, int);` to the `extern "C"` block)

```cpp
/* ===================== P-DEMATCH: module entry vs the host path (prep_segment + nr_pdsch_gpu_ldpc_run) ===================== */
struct DmSyms {
  nr_gpu_dm_init_fn init = nullptr; nr_gpu_dm_run_fn run = nullptr; nr_gpu_dm_stats_fn stats = nullptr;
  int (*ldpc_init)(void) = nullptr; int (*ldpc_run)(nr_gpu_ldpc_job_t *, int) = nullptr;
  bool ok = false;
  static DmSyms &get() {
    static DmSyms s; static bool tried = false;
    if (!tried) {
      tried = true;
      void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
      if (dl) {
        s.init = (nr_gpu_dm_init_fn)dlsym(dl, "nr_pdsch_gpu_dm_init"); s.run = (nr_gpu_dm_run_fn)dlsym(dl, "nr_pdsch_gpu_dm_run");
        s.stats = (nr_gpu_dm_stats_fn)dlsym(dl, "nr_pdsch_gpu_dm_stats");
        s.ldpc_init = (int (*)(void))dlsym(dl, "nr_pdsch_gpu_ldpc_init"); s.ldpc_run = (int (*)(nr_gpu_ldpc_job_t *, int))dlsym(dl, "nr_pdsch_gpu_ldpc_run");
        s.ok = s.init && s.run && s.stats && s.ldpc_init && s.ldpc_run && s.ldpc_init() == 0 && s.init() == 0;
      }
    }
    return s;
  }
};

/* one random TB geometry (any BG / Z / rv / F / LBRM, E != E2 in half the cases) -> up to nmax segments + the LLR array. All derived from
 * nr_gpu_dm_geom, i.e. the same arithmetic the receiver uses; nothing here is a per-cell constant. */
struct DmCase {
  std::vector<nr_gpu_rdm_seg_t> segs; std::vector<nr_gpu_ldpc_job_t> jobs; std::vector<int16_t> llr; uint32_t C = 0; int first_rE2 = 0;
  static DmCase random(int seed, int nmax, int first_seg = 0) {
    static const int zs[] = {2, 7, 11, 16, 22, 36, 64, 96, 128, 176, 208, 256, 320, 384};
    std::mt19937 g(seed * 2654435761u + 3);
    DmCase c; nr_gpu_dm_tb_t t = {};
    t.BG = 1 + g() % 2; t.Z = zs[g() % 14]; const int Kb = t.BG == 1 ? 22 : 10; t.K = Kb * t.Z;
    t.F = g() % 3 == 0 ? 0 : g() % (t.K / 4 + 1); t.Kprime = t.K - t.F; t.rv = g() % 4;
    const uint8_t qms[] = {2, 4, 6, 8}; t.Qm = qms[g() % 4]; const int Nl = 1 + g() % 4;
    nr_gpu_dm_geom_in_t gi = {}; gi.nprb = 1 + g() % 60; gi.nsym = 2 + g() % 13; gi.dmrs_re = 12; gi.unav_res = g() % 3 ? 0 : g() % 200; gi.Qm = t.Qm; gi.Nl = Nl;
    t.C = 1 + g() % 48; const nr_gpu_dm_geom_t go = nr_gpu_dm_geom(&gi, t.C);
    t.E = go.E; t.E2 = go.E2; t.first_rE2 = go.first_rE2;
    const uint32_t N = (t.BG == 1 ? 66 : 50) * t.Z;
    t.tbslbrm = g() % 3 ? 0 : (uint32_t)((uint64_t)(N / 2 + g() % (N / 2)) * 2 * t.C / 3 + 1);   /* LBRM binds in a third of the cases */
    c.C = t.C; c.first_rE2 = t.first_rE2; c.llr.resize(go.G);
    for (auto &v : c.llr) v = (int16_t)((int)(g() % 6001) - 3000);
    const uint32_t last = std::min<uint32_t>(t.C, first_seg + nmax);
    for (uint32_t r = first_seg; r < last; r++) {
      nr_gpu_rdm_seg_t s = {}; nr_gpu_dm_seg_fill(&t, r, &s); c.segs.push_back(s);
      int llrLen = 0; nr_gpu_ldpc_job_t j = {};
      j.BG = t.BG; j.Z = t.Z; j.K = t.K; j.it_max = 8; j.R = (uint8_t)nr_get_R_ldpc_decoder(t.rv, s.E, t.BG, t.Z, &llrLen, 0);
      c.jobs.push_back(j);
    }
    return c;
  }
};

static int dm_vs_host(DmSyms &S, const DmCase &c, uint8_t cell, int *n_bad_rm) {
  const int n = (int)c.segs.size();
  auto gjobs = c.jobs; std::vector<int8_t> chk((size_t)n * NR_GPU_LDPC_DEV_IN_STRIDE, 99);
  const int rc = S.run(gjobs.data(), c.segs.data(), n, cell, c.llr.data(), (uint32_t)c.llr.size(), chk.data());
  if (rc != 0) return rc;
  /* host reference: prep_segment's bytes through nr_pdsch_gpu_ldpc_run */
  auto hjobs = c.jobs; std::vector<std::vector<int8_t>> l(n, std::vector<int8_t>(68 * 384 + 64, 0));
  for (int k = 0; k < n; k++) {
    const int Kc = c.segs[k].BG == 2 ? 52 : 68;
    const int prc = cpu_prep(c.segs[k], c.llr.data(), l[k].data());
    if (prc != 0 || nr_gpu_dm_seg_bad(&c.segs[k])) { std::fill(l[k].begin(), l[k].end(), 0); ++*n_bad_rm; }
    EXPECT_EQ(0, memcmp(l[k].data(), &chk[(size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE], (size_t)Kc * c.segs[k].Z)) << "LLR seg " << k << " BG" << (int)c.segs[k].BG << " Z" << c.segs[k].Z << " rv" << c.segs[k].rv << " E" << c.segs[k].E << " F" << c.segs[k].F;
    hjobs[k].llr = l[k].data();
  }
  EXPECT_EQ(0, S.ldpc_run(hjobs.data(), n));
  for (int k = 0; k < n; k++) {
    EXPECT_EQ(hjobs[k].nsnap, gjobs[k].nsnap); EXPECT_EQ(hjobs[k].syn0, gjobs[k].syn0);
    for (int s = 0; s < hjobs[k].nsnap; s++) EXPECT_EQ(0, memcmp(hjobs[k].snap[s], gjobs[k].snap[s], (hjobs[k].K + 7) >> 3)) << "snap " << k << "/" << s;
  }
  return 0;
}

TEST(GpuDm, RunMatchesHostPathEveryClass) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  DmSyms &S = DmSyms::get(); if (!S.ok) GTEST_SKIP() << "no libpdsch_gpu.so DM entry / LDPC init";
  int n_bad = 0, n_done = 0, n_k8 = 0;
  for (int seed = 0; seed < 400; seed++) {
    const DmCase c = DmCase::random(seed, 8);
    if (c.segs.empty()) continue;
    for (auto &s : c.segs) n_k8 += (s.K & 7) != 0;
    ASSERT_EQ(0, dm_vs_host(S, c, seed % NR_GPU_DM_CELL_MAX, &n_bad)) << seed;   /* every cell index gives the same result */
    ++n_done;
    if (::testing::Test::HasFailure()) break;
  }
  printf("dematch classes: %d TBs, %d refused segments, %d K%%8!=0 blocks\n", n_done, n_bad, n_k8);
  EXPECT_GT(n_done, 300); EXPECT_GT(n_k8, 20);
}

/* C > 32 (two launches) and a layout probe (one block of a bigger C) keep absolute offsets */
TEST(GpuDm, WindowedChunksMatchCpu) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  DmSyms &S = DmSyms::get(); if (!S.ok) GTEST_SKIP();
  int n_bad = 0, checked = 0;
  for (int seed = 1000; seed < 1400 && checked < 25; seed++) {
    DmCase probe = DmCase::random(seed, 1, 0);
    if (probe.C < 34) continue;
    for (int first : {0, 32, (int)probe.C - 1}) {                         /* the later windows start deep inside the LLR array */
      const DmCase w = DmCase::random(seed, 32, first);
      ASSERT_EQ(0, dm_vs_host(S, w, 0, &n_bad)) << seed << " first " << first;
    }
    ++checked;
  }
  EXPECT_GE(checked, 10);
}

TEST(GpuDm, RefusalsAreReasonsNotCrashes) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  DmSyms &S = DmSyms::get(); if (!S.ok) GTEST_SKIP();
  DmCase c = DmCase::random(5, 2); int nb = 0;
  ASSERT_EQ(0, dm_vs_host(S, c, 0, &nb));
  auto j = c.jobs;
  EXPECT_EQ(NR_GPU_LDPC_E_RANGE, S.run(j.data(), c.segs.data(), (int)c.segs.size(), NR_GPU_DM_CELL_MAX, c.llr.data(), (uint32_t)c.llr.size(), nullptr)); /* cell out of range */
  EXPECT_EQ(NR_GPU_LDPC_E_RANGE, S.run(j.data(), c.segs.data(), (int)c.segs.size(), 0, c.llr.data(), 3, nullptr));                                  /* window past the LLR array */
  EXPECT_EQ(NR_GPU_LDPC_E_RANGE, S.run(j.data(), c.segs.data(), 0, 0, c.llr.data(), (uint32_t)c.llr.size(), nullptr));                              /* n = 0 */
  uint64_t o[5 + NR_GPU_DM_CELL_MAX] = {0}; S.stats(o);
  EXPECT_GT(o[0], 0u); EXPECT_GT(o[5], 0u);                                                                                                          /* per-cell counter moves */
}

TEST(GpuDm, ConcurrentThreadsNeverCorrupt) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  DmSyms &S = DmSyms::get(); if (!S.ok) GTEST_SKIP();
  std::atomic<int> bad{0}, done{0}, busy{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 8; t++)                                            /* 8 threads on 4 contexts: some calls must see E_BUSY */
    th.emplace_back([&, t] {
      for (int i = 0; i < 60; i++) {
        const DmCase c = DmCase::random(t * 977 + i, 6);
        if (c.segs.empty()) continue;
        auto j = c.jobs; std::vector<int8_t> chk((size_t)c.segs.size() * NR_GPU_LDPC_DEV_IN_STRIDE, 7);
        const int rc = S.run(j.data(), c.segs.data(), (int)c.segs.size(), t % NR_GPU_DM_CELL_MAX, c.llr.data(), (uint32_t)c.llr.size(), chk.data());
        if (rc == NR_GPU_LDPC_E_BUSY) { ++busy; continue; }
        if (rc != 0) { ++bad; continue; }
        for (size_t k = 0; k < c.segs.size(); k++) {
          const int Kc = c.segs[k].BG == 2 ? 52 : 68; std::vector<int8_t> ref(Kc * c.segs[k].Z + 16, 0);
          if (cpu_prep(c.segs[k], c.llr.data(), ref.data()) != 0 || nr_gpu_dm_seg_bad(&c.segs[k])) std::fill(ref.begin(), ref.end(), 0);
          if (memcmp(ref.data(), &chk[k * NR_GPU_LDPC_DEV_IN_STRIDE], (size_t)Kc * c.segs[k].Z)) ++bad;
        }
        ++done;
      }
    });
  for (auto &x : th) x.join();
  printf("dematch concurrency: done %d busy %d\n", done.load(), busy.load());
  EXPECT_EQ(0, bad.load()); EXPECT_GT(done.load(), 100);
}
```
(The concurrency test calls `cpu_prep` from several threads: it only uses local vectors and the OAI CPU functions, which are re-entrant here; if a data race report appears, serialise the CPU reference with a `std::mutex`, not the GPU call.)

- [ ] **Step 2: Run to verify it fails** - build `test_nr_gpu_pass` (compiles) and run `--gtest_filter='GpuDm.*'` (same command as Task 1 Step 4). Expected: the new tests SKIP with "no libpdsch_gpu.so DM entry" (dlsym returns null). A SKIP is not a pass: Step 5 must show them PASSED.

- [ ] **Step 3: Implement in `nr_pdsch_gpu_ldpc.cu`.** Add `#include "nr_gpu_dm.h"` with the other includes at the top. Insert after the `nr_pdsch_gpu_ldpc_pass_dev` function (the block ending `return inst >= NR_GPU_PASS_RING ? -1 : dev_impl(inst, jobs, n, llr_dev, bits_dev, stream);\n}`):

```cpp
/* ---------------------------------------------------------------------------------------------------------------------
 * P-DEMATCH: ISAC_PDSCH_GPU_DEMATCH=1 (host switch in nr_pdsch_gpu_ldpc_host.c). One call = LLR window staged -> k_rdm writes the LDPC input
 * (device scratch, NR_GPU_LDPC_DEV_IN layout) -> dev_impl decodes -> results collected. Contexts: the per-instance contexts g_ctx[CTX + i],
 * i < NR_GPU_PASS_RING, which GPUPASS (ISAC_GPU_PASS / ISAC_UL_GPU_PASS) owns when on: DM then does not initialise. A busy pool is
 * NR_GPU_LDPC_E_BUSY (the caller runs the CPU prep), never a wait. Buffers: nr_gpu_buf_alloc at init (pre-fence), copies through
 * nr_gpu_buf_h2d_async / d2h_async unconditionally (DIRECT: no-ops, DISCRETE: copies on the context's stream). */
namespace nrdm {
struct DmCtx { nr_gpu_buf_t llr, segs, in, bits; };
static DmCtx g_dm[NR_GPU_PASS_RING];
static std::atomic<unsigned> g_dm_busy{0};
static std::atomic<uint64_t> g_dm_calls{0}, g_dm_cbs{0}, g_dm_nbusy{0}, g_dm_ncap{0}, g_dm_nerr{0}, g_dm_cell[NR_GPU_DM_CELL_MAX];
static int g_dm_ok;
} // namespace nrdm

extern "C" int nr_pdsch_gpu_dm_init(void)
{
  using namespace nrdm;
  static std::once_flag once;
  std::call_once(once, [] {
    if (!g_ok)
      return;
    const char *p = getenv("ISAC_GPU_PASS"), *u = getenv("ISAC_UL_GPU_PASS");
    if ((p && atoi(p)) || (u && atoi(u)))
      return; /* the pass instances own these contexts */
    for (int i = 0; i < NR_GPU_PASS_RING; i++) {
      DmCtx &d = g_dm[i];
      if (nr_gpu_buf_alloc(&d.llr, (size_t)NR_GPU_DM_LLR_MAX * sizeof(int16_t)) || nr_gpu_buf_alloc(&d.segs, NJ * sizeof(nr_gpu_rdm_seg_t))
          || nr_gpu_buf_alloc(&d.in, NR_GPU_LDPC_DEV_IN_BYTES) || nr_gpu_buf_alloc(&d.bits, NR_GPU_LDPC_DEV_BITS_BYTES))
        return;
    }
    g_dm_ok = 1;
  });
  return g_dm_ok ? 0 : -1;
}

extern "C" int nr_pdsch_gpu_dm_run(nr_gpu_ldpc_job_t *jobs, const nr_gpu_rdm_seg_t *segs, int n, uint8_t cell, const int16_t *llr, uint32_t llr_n, int8_t *chk)
{
  using namespace nrdm;
  if (!g_dm_ok)
    return NR_GPU_LDPC_E_INIT;
  if (n < 1 || n > NJ || !jobs || !segs || !llr || cell >= NR_GPU_DM_CELL_MAX)
    return NR_GPU_LDPC_E_RANGE;
  uint32_t lo = 0xffffffffu, hi = 0;
  for (int k = 0; k < n; k++) {
    lo = segs[k].llr_off < lo ? segs[k].llr_off : lo;
    hi = segs[k].llr_off + segs[k].E > hi ? segs[k].llr_off + segs[k].E : hi;
  }
  if (hi > llr_n || lo >= hi)
    return NR_GPU_LDPC_E_RANGE;
  if (hi - lo > NR_GPU_DM_LLR_MAX) {
    g_dm_ncap.fetch_add(1);
    return NR_GPU_LDPC_E_CAP;
  }
  int inst = -1;
  for (unsigned m = g_dm_busy.load(); inst < 0;) { /* claim an instance without waiting */
    int free_i = -1;
    for (int i = 0; i < NR_GPU_PASS_RING; i++)
      if (!(m >> i & 1u)) { free_i = i; break; }
    if (free_i < 0) {
      g_dm_nbusy.fetch_add(1);
      return NR_GPU_LDPC_E_BUSY;
    }
    if (g_dm_busy.compare_exchange_weak(m, m | 1u << free_i))
      inst = free_i;
  }
  struct Release { int i; ~Release() { g_dm_busy.fetch_and(~(1u << i)); } } release{inst};
  DmCtx &d = g_dm[inst];
  const cudaStream_t s = g_ctx[CTX + inst].s;
  const size_t lbytes = (size_t)(hi - lo) * sizeof(int16_t);
  memcpy(d.llr.host, llr + lo, lbytes); /* shortcut: one host copy of the window; upgrade to demod-writes-into-staging if it eats the gain (Task 5 gate) */
  nr_gpu_rdm_seg_t *hs = (nr_gpu_rdm_seg_t *)d.segs.host;
  for (int k = 0; k < n; k++) {
    hs[k] = segs[k];
    hs[k].llr_off -= lo;
  }
  if (nr_gpu_buf_h2d_async(&d.llr, 0, lbytes, s) || nr_gpu_buf_h2d_async(&d.segs, 0, (size_t)n * sizeof(nr_gpu_rdm_seg_t), s)
      || nr_gpu_pass_launch_rdm((const int16_t *)d.llr.dev, (const nr_gpu_rdm_seg_t *)d.segs.dev, n, (int8_t *)d.in.dev, s) != 0) {
    g_dm_nerr.fetch_add(1);
    return NR_GPU_LDPC_E_STREAM;
  }
  if (dev_impl(inst, jobs, n, &d.in, &d.bits, s) != 0) { /* graph miss / bad job: nothing was launched on the LDPC side */
    cudaStreamSynchronize(s); /* the rdm launch above is in flight and uses this context's buffers */
    g_dm_nerr.fetch_add(1);
    return NR_GPU_LDPC_E_GRAPH_MISS;
  }
  const size_t snap_bytes = (size_t)n * NR_GPU_LDPC_MAX_SNAP * NR_GPU_LDPC_KB_MAX;
  if (nr_gpu_buf_d2h_async(&d.bits, 0, snap_bytes, s) || nr_gpu_buf_d2h_async(&d.bits, NR_GPU_LDPC_DEV_RES_OFF, 2 * NJ * sizeof(int), s)
      || (chk && nr_gpu_buf_d2h_async(&d.in, 0, (size_t)n * NR_GPU_LDPC_DEV_IN_STRIDE, s))) { /* shortcut: copies unused snapshot rows on DISCRETE; trim by nsnap if sens6 timing asks */
    cudaStreamSynchronize(s);
    g_dm_nerr.fetch_add(1);
    return NR_GPU_LDPC_E_STREAM;
  }
  uint64_t pt = 0;
  const uint64_t ss0 = nr_ss_now();
  cudaError_t e;
  while ((e = cudaStreamQuery(s)) == cudaErrorNotReady)
    nr_gpu_poll_relax(&pt);
  nr_ss_add(NRSS_LDPC, ss0);
  if (e != cudaSuccess) {
    fprintf(stderr, "nr_pdsch_gpu_dm_run: %s\n", cudaGetErrorString(e));
    g_dm_nerr.fetch_add(1);
    return NR_GPU_LDPC_E_STREAM;
  }
  const int *res = (const int *)((const char *)d.bits.host + NR_GPU_LDPC_DEV_RES_OFF);
  int rc = 0;
  for (int k = 0; k < n; k++) {
    nr_gpu_ldpc_job_t &j = jobs[k];
    j.nsnap = res[2 * k];
    j.syn0 = res[2 * k + 1];
    if (j.nsnap < 0 || j.nsnap > NR_GPU_LDPC_MAX_SNAP) {
      rc = NR_GPU_LDPC_E_SENTINEL;
      continue;
    }
    for (int q = 0; q < j.nsnap; q++)
      memcpy(j.snap[q], (const uint8_t *)d.bits.host + ((size_t)k * NR_GPU_LDPC_MAX_SNAP + q) * NR_GPU_LDPC_KB_MAX, (j.K + 7) >> 3);
    if (chk)
      memcpy(chk + (size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE, (const int8_t *)d.in.host + (size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE, (size_t)(j.BG == 1 ? 68 : 52) * j.Z);
  }
  g_dm_calls.fetch_add(1);
  g_dm_cbs.fetch_add(n);
  g_dm_cell[cell].fetch_add(1);
  return rc;
}

extern "C" void nr_pdsch_gpu_dm_stats(uint64_t *o)
{
  using namespace nrdm;
  o[0] = g_dm_calls.load(); o[1] = g_dm_cbs.load(); o[2] = g_dm_nbusy.load(); o[3] = g_dm_ncap.load(); o[4] = g_dm_nerr.load();
  for (int c = 0; c < NR_GPU_DM_CELL_MAX; c++)
    o[5 + c] = g_dm_cell[c].load();
}
```
Needed, if not already visible in this TU: `#include "nr_gpu_pass_post.h"` is pulled by `nr_gpu_dm.h`; `nr_ss_now/nr_ss_add/NRSS_LDPC` are already used by `nr_pdsch_gpu_ldpc_run` in this file; `nr_gpu_poll_relax` likewise. `dev_impl` is `static` at file scope above: keep the new code in the same file after it. The snapshot-pad (K%8) T5 change must already be committed (Contract delta 6).

- [ ] **Step 4: Build** `... ninja -j6 -C cmake_targets/ran_build/build_t26 pdsch_gpu test_nr_gpu_pass < /dev/null 2>&1 | tail -30`. Fix compile errors only in the new block.

- [ ] **Step 5: Run and verify PASS**
`cd .../build_t26 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh env CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps ./test_nr_gpu_pass --gtest_filter='GpuDm.*' < /dev/null`
Expected: all `GpuDm.*` PASSED (not SKIPPED), printed `dematch classes: >300 TBs ... K%8!=0 blocks >20`, `dematch concurrency: done >100 busy >=0`. If `RunMatchesHostPathEveryClass` shows a LLR mismatch, print the class (the EXPECT message has BG/Z/rv/E/F); do not weaken the memcmp. A graph-miss for an `R` the init did not build is not a mismatch: `dm_vs_host` returns `E_GRAPH_MISS` and the ASSERT shows it; fix the generator (R from `nr_get_R_ldpc_decoder` can be 13/23/89 or 15/13/23 only).

- [ ] **Step 6: Regression of everything that shares the module**
`ctest` equivalents, one binary at a time through heavy.sh in the build dir: `./test_nr_gpu_pass` (all), `./test_nr_pdsch_gpu_ldpc 2`, `./test_nr_pdsch_gpu_ldpc 2 nc`, `./test_nr_pdsch_gpu_ldpc 2 mix`, `./test_nr_pdsch_gpu_ldpc 2 ul`. Expected: same results as before the change (`mismatches=0`; the known ARM failures listed in CLAUDE.md are not in this set).

- [ ] **Step 7: No allocation after the fence** - add to `nr_gpu_pass_test.cc`:
```cpp
TEST(GpuDm, NoAllocAfterFence) {
  if (!have_gpu()) GTEST_SKIP();
  pass_model();
  DmSyms &S = DmSyms::get(); if (!S.ok) GTEST_SKIP();
  nr_gpu_rx_started();                                   /* seals the gate (nr_gpu_platform.c:208) */
  int nb = 0; const DmCase c = DmCase::random(9, 4);
  EXPECT_EQ(0, dm_vs_host(S, c, 0, &nb));                /* the run path allocates nothing */
}
```
(`nr_gpu_rx_started` is process-wide and irreversible: this test must be the LAST `GpuDm` test in the file and `GpuPassFence.*` tests precede it in file order; if other suites after it need allocation, instead run it only under `--gtest_filter` in its own ctest invocation. Check how `GpuPassFence.NoAllocAfterFence` at `nr_gpu_pass_test.cc:1677` handles the same constraint and copy its approach.) Run and expect PASS.

- [ ] **Step 8: Commit**
```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_test.cc
git commit -m "feat(dematch): nr_pdsch_gpu_dm_run = stage + k_rdm + device LDPC on a pass-instance context; bit-exact, windowed, concurrent tests"
```

- [ ] **Step 9: Discrete ctest (ORCHESTRATOR re-runs cmake after this edit)** - in `openair1/PHY/CODING/CMakeLists.txt`, directly after the `test_nr_gpu_pass_discrete` `set_tests_properties` line add:
```cmake
  # P-DEMATCH under the DISCRETE memory model (own process: the DM contexts are allocated once per process)
  add_test(NAME test_nr_gpu_dm_discrete COMMAND test_nr_gpu_pass --gtest_filter=GpuDm.* WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_nr_gpu_dm_discrete PROPERTIES ENVIRONMENT "NR_GPU_PASS_DISCRETE=1;CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps" SKIP_RETURN_CODE 77)
```
Commit it (`git add openair1/PHY/CODING/CMakeLists.txt`), then STOP this step and tell the orchestrator: "new ctest target test_nr_gpu_dm_discrete: please re-run cmake in build_t26". After the re-run: `cd build_t26 && heavy.sh ctest -R 'test_nr_gpu_dm_discrete|test_nr_gpu_pass$' --output-on-failure < /dev/null`. Expected: PASSED for both (the discrete one proves staging offsets, tail bytes and copy-back under the DISCRETE model; `pass_model()` maps `NR_GPU_PASS_DISCRETE=1` to `ISAC_GPU_FORCE_DISCRETE=1`).

---

### Task 3: Host integration (TB fields, passive receiver, `gpu_tb` branch, shadow check, class table, stats)

**Files:**
- Modify: `openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h` (TB struct)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c`
- Test: build + `test_nr_pdsch_gpu_ldpc`/`test_nr_gpu_pass` regressions here; behaviour proof is Task 4/5 (the host code is `static` and needs the whole receiver).

**Interfaces:**
- Consumes: Task 1 header, Task 2 entry (dlsym names `nr_pdsch_gpu_dm_init`, `nr_pdsch_gpu_dm_run`, `nr_pdsch_gpu_dm_stats`).
- Produces: env `ISAC_PDSCH_GPU_DEMATCH=1` (feature), `ISAC_PDSCH_GPU_DEMATCH_CHECK=N` (N=1: every DM TB also runs `prep_segment` and compares byte for byte; N>1: every Nth); log lines `GPU_DEMATCH ...` and `DMCLASS ...` (format below) consumed by Task 5.

- [ ] **Step 1: TB fields.** In `nrLDPC_TB_decoding_parameters_s` (`nrLDPC_coding_interface.h`), after `short *llr;` add:
```c
  /// P-DEMATCH (GPU rate dematching, ISAC_PDSCH_GPU_DEMATCH): the geometry G was built from (PDSCH length L, DM-RS REs per PRB x symbols,
  /// rate-matched REs of the grant) and the cell index. dm_nsym == 0 = unset: the GPU dematch is not used. dm_allow = the caller does not
  /// read tb->d after the decode (the GPU dematch does not write it).
  uint16_t dm_nsym;
  uint32_t dm_dmrs_re, dm_unav_res;
  uint8_t dm_cell;
  bool dm_allow;
```
(The struct is zero-initialised by every producer: `nrLDPC_TB_decoding_parameters_t TB_parameters = {0};` [CODE-READ nr_pdsch_passive_decode.c:1795]; other producers are unaffected.)

- [ ] **Step 2: Passive receiver.** In `nr_pdsch_passive_decode.c`:
  - next to `static __thread bool t_cap_req ...` (line ~1633) add `static __thread uint32_t t_dm_nsym, t_dm_dmrs_re, t_dm_unav; static __thread uint8_t t_dm_cell;`
  - directly after the G computation at ~line 3004-3007 (`const uint32_t G = nr_get_G(freq_alloc->num_rbs, dlsch_config->number_symbols, nb_re_dmrs, dmrs_len, ptrs_unav + csi_unav + ssb_unav, cw->qamModOrder, cw->Nl);`) add:
```c
  t_dm_nsym = (uint32_t)dlsch_config->number_symbols;  /* P-DEMATCH: what G was built from (T-DRA changes L, T-DEAD changes unav) */
  t_dm_dmrs_re = (uint32_t)nb_re_dmrs * dmrs_len;
  t_dm_unav = ptrs_unav + csi_unav + ssb_unav;
  t_dm_cell = 0; /* single cell today; the multi-cell receiver sets the cell of the grant here */
```
  - in `passive_ldpc_decode_core` after `TB_parameters.llr = llr;` add:
```c
  TB_parameters.dm_nsym = (uint16_t)t_dm_nsym; TB_parameters.dm_dmrs_re = t_dm_dmrs_re; TB_parameters.dm_unav_res = t_dm_unav;
  TB_parameters.dm_cell = t_dm_cell; TB_parameters.dm_allow = !t_cap_req;
```
  If the compiler reports `nb_re_dmrs`/`dmrs_len`/`ptrs_unav`/`csi_unav`/`ssb_unav` unavailable at the insertion point, use the exact expressions of the `nr_get_G` call that follows `const uint32_t G` (they are in scope there by construction).

- [ ] **Step 3: Host switch in `nr_pdsch_gpu_ldpc_host.c`.**
  a) includes: after `#include "nr_pdsch_gpu_ldpc_host.h"` add `#include "nr_gpu_dm.h"`.
  b) `gl_t`: add fields
```c
  nr_gpu_dm_run_fn dm_run; nr_gpu_dm_stats_fn dm_stats; _Atomic int dm_on; int dm_check;
  _Atomic uint64_t dm_tbs, dm_cbs, dm_geom, dm_chk_ok, dm_chk_bad, dm_fb[NR_GPU_LDPC_NERR];
```
  c) in `load()`, after the `LOG_A(PHY, "SENSING: GPU_EXACT_LDPC enabled ...` line and before `atomic_store(&G->on, 1);` add:
```c
  {
    const char *d = getenv("ISAC_PDSCH_GPU_DEMATCH");
    if (d && atoi(d) == 1) {
      nr_gpu_dm_init_fn dinit = (nr_gpu_dm_init_fn)dlsym(h, "nr_pdsch_gpu_dm_init");
      G->dm_run = (nr_gpu_dm_run_fn)dlsym(h, "nr_pdsch_gpu_dm_run");
      G->dm_stats = (nr_gpu_dm_stats_fn)dlsym(h, "nr_pdsch_gpu_dm_stats");
      const char *ck = getenv("ISAC_PDSCH_GPU_DEMATCH_CHECK");
      G->dm_check = ck ? atoi(ck) : 0;
      if (dinit && G->dm_run && G->dm_stats && dinit() == 0) {
        atomic_store(&G->dm_on, 1);
        LOG_A(PHY, "SENSING: GPU_DEMATCH enabled: k_rdm rate dematch + deinterleave on the GPU into the LDPC input, check=%d\n", G->dm_check);
      } else {
        LOG_W(PHY, "SENSING: GPU_DEMATCH requested but unavailable (module entry missing, GPUPASS on, or init failed) -- CPU prep_segment\n");
      }
    }
  }
```
  d) class table + helpers, placed before `gpu_tb` (after `tb_ndec`):
```c
/* ---- P-DEMATCH ---- */
typedef struct { uint8_t BG, rv, Qm, lbrm; uint16_t Z; uint32_t E, E2, F, K; _Atomic uint64_t ok, bad; } dmcls_t;
#define DMCLS_MAX 256
static dmcls_t dm_cls[DMCLS_MAX];
static _Atomic int dm_ncls;
static pthread_mutex_t dm_cls_lock = PTHREAD_MUTEX_INITIALIZER;
static dmcls_t *dm_class(const nrLDPC_TB_decoding_parameters_t *tb)
{
  const uint8_t lbrm = tb->tbslbrm && 3 * tb->tbslbrm / (2 * tb->C) < (uint32_t)(tb->BG == 1 ? 66 : 50) * tb->Z;
  const int n = atomic_load(&dm_ncls);
  for (int i = 0; i < n; i++) {
    dmcls_t *c = &dm_cls[i];
    if (c->BG == tb->BG && c->rv == tb->rv_index && c->Qm == tb->Qm && c->lbrm == lbrm && c->Z == tb->Z && c->E == (uint32_t)tb->E && c->E2 == (uint32_t)tb->E2 && c->F == tb->F && c->K == tb->K)
      return c;
  }
  pthread_mutex_lock(&dm_cls_lock);
  dmcls_t *r = NULL;
  const int m = atomic_load(&dm_ncls);
  for (int i = 0; i < m && !r; i++) {
    dmcls_t *c = &dm_cls[i];
    if (c->BG == tb->BG && c->rv == tb->rv_index && c->Qm == tb->Qm && c->lbrm == lbrm && c->Z == tb->Z && c->E == (uint32_t)tb->E && c->E2 == (uint32_t)tb->E2 && c->F == tb->F && c->K == tb->K)
      r = c;
  }
  if (!r && m < DMCLS_MAX) {
    r = &dm_cls[m];
    r->BG = tb->BG; r->rv = tb->rv_index; r->Qm = tb->Qm; r->lbrm = lbrm; r->Z = (uint16_t)tb->Z; r->E = tb->E; r->E2 = tb->E2; r->F = tb->F; r->K = tb->K;
    atomic_store(&dm_ncls, m + 1);
    LOG_A(PHY, "SENSING: DMCLASS BG=%u Z=%u rv=%u Qm=%u E=%u E2=%u F=%u K=%u lbrm=%u C=%u\n", r->BG, r->Z, r->rv, r->Qm, r->E, r->E2, r->F, r->K, r->lbrm, tb->C);
  }
  pthread_mutex_unlock(&dm_cls_lock);
  return r;
}

/* geometry the receiver built G from -> must reproduce the E / E2 / first_rE2 the TB carries. 1 = ok. A mismatch means one side changed L or the
 * rate-matched REs without the other (T-DRA / T-DEAD): the CPU prep runs and the counter moves, the GPU never decodes with a wrong E. */
static int dm_geom_ok(const nrLDPC_TB_decoding_parameters_t *tb)
{
  if (tb->dm_nsym == 0 || tb->Qm == 0 || tb->nb_layers == 0 || tb->dm_cell >= NR_GPU_DM_CELL_MAX)
    return 0;
  const nr_gpu_dm_geom_in_t gi = {.nprb = tb->nb_rb, .nsym = tb->dm_nsym, .dmrs_re = tb->dm_dmrs_re, .unav_res = tb->dm_unav_res, .Qm = tb->Qm, .Nl = tb->nb_layers};
  const nr_gpu_dm_geom_t g = nr_gpu_dm_geom(&gi, (int)tb->C);
  return g.G == tb->G && g.E == tb->E && g.E2 == tb->E2 && g.first_rE2 == tb->first_rE2;
}

/* One launch worth (n <= NR_GPU_LDPC_MAX_JOBS blocks from r0) through the GPU dematch. 0 = jobs[] hold the decode results (same fields as
 * G->run), rm_bad[] set; != 0 = nothing done, the caller runs prep_segment + G->run (and this TB stays on the CPU prep from then on). */
static int dm_chunk(gl_t *G, nrLDPC_TB_decoding_parameters_t *tb, tls_t *t, int r0, int n, int *rm_bad, uint64_t *gpu_ns)
{
  nr_gpu_rdm_seg_t segs[NR_GPU_LDPC_MAX_JOBS];
  nr_gpu_dm_tb_t d = {.BG = tb->BG, .Qm = tb->Qm, .rv = tb->rv_index, .Z = tb->Z, .K = tb->K, .F = tb->F, .Kprime = (uint32_t)lenWithCrc(tb->C, tb->A),
                      .C = tb->C, .tbslbrm = tb->tbslbrm, .E = tb->E, .E2 = tb->E2, .first_rE2 = tb->first_rE2};
  for (int k = 0; k < n; k++) {
    nr_gpu_dm_seg_fill(&d, (uint32_t)(r0 + k), &segs[k]);
    rm_bad[k] = nr_gpu_dm_seg_bad(&segs[k]);
    const uint8_t R = (uint32_t)(r0 + k) < (uint32_t)tb->first_rE2 ? tb->R : tb->R2;
    t->jobs[k] = (nr_gpu_ldpc_job_t){.BG = tb->BG, .R = R, .Z = (uint16_t)tb->Z, .K = (uint16_t)tb->K, .it_max = tb->max_ldpc_iterations, .llr = NULL};
  }
  const uint64_t n_dm = atomic_fetch_add_explicit(&G->dm_tbs, r0 == 0, memory_order_relaxed) + 1;
  const int check = G->dm_check == 1 || (G->dm_check > 1 && n_dm % (uint64_t)G->dm_check == 0);
  int8_t *chk = NULL;
  if (check) {
    if (!t->chk)
      t->chk = malloc((size_t)NR_GPU_LDPC_MAX_JOBS * NR_GPU_LDPC_DEV_IN_STRIDE);
    chk = t->chk;
    if (!chk)
      return 1;
  }
  const uint64_t g0 = now_ns();
  const int rc = G->dm_run(t->jobs, segs, n, tb->dm_cell, tb->llr, tb->G, chk);
  if (rc != 0) {
    atomic_fetch_add_explicit(&G->dm_fb[rc < 0 && -rc < NR_GPU_LDPC_NERR ? -rc : 0], 1, memory_order_relaxed);
    return rc;
  }
  *gpu_ns += now_ns() - g0;
  atomic_fetch_add_explicit(&G->dm_cbs, n, memory_order_relaxed);
  if (check) { /* shadow: prep_segment's bytes vs the GPU's, every block; the first difference switches P-DEMATCH off */
    dmcls_t *cl = dm_class(tb);
    for (int k = 0; k < n; k++) {
      int E;
      uint8_t R;
      int8_t *l = t->l + (size_t)k * (68 * 384 + 16);
      const int bad = prep_segment(tb, r0 + k, t, l, &E, &R) != 0;
      const size_t len = (size_t)(tb->BG == 2 ? 52 : 68) * tb->Z;
      const int same = bad ? rm_bad[k] : (!rm_bad[k] && memcmp(l, chk + (size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE, len) == 0);
      if (cl)
        atomic_fetch_add_explicit(same ? &cl->ok : &cl->bad, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(same ? &G->dm_chk_ok : &G->dm_chk_bad, 1, memory_order_relaxed);
      if (!same) {
        size_t q = 0;
        while (q < len && !bad && l[q] == chk[(size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE + q])
          q++;
        LOG_E(PHY, "SENSING: GPU_DEMATCH MISMATCH cb %d (BG %u Z %u rv %u Qm %u E %d F %u K %u lbrm %u) byte %zu cpu %d gpu %d cpu_bad %d gpu_bad %d -- P-DEMATCH disabled\n",
              r0 + k, tb->BG, tb->Z, tb->rv_index, tb->Qm, E, tb->F, tb->K, tb->tbslbrm, q, bad ? 0 : l[q < len ? q : 0],
              (int)chk[(size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE + (q < len ? q : 0)], bad, rm_bad[k]);
        atomic_store(&G->dm_on, 0);
      }
    }
  }
  return 0;
}
```
  e) `tls_t`: add `int8_t *chk;` (after `uint8_t *c_chk;`).
  f) `gpu_tb`: before the `for (uint32_t r0 ...` loop add
```c
  bool use_dm = !G->ul && atomic_load_explicit(&G->dm_on, memory_order_relaxed) == 1 && tb->dm_allow && tb->llr;
  if (use_dm && !dm_geom_ok(tb)) {
    atomic_fetch_add_explicit(&G->dm_geom, 1, memory_order_relaxed);
    use_dm = false;
  }
```
  and replace, inside the loop, from `int rm_bad[NR_GPU_LDPC_MAX_JOBS] = {0};` through the `*gpu_ns += now_ns() - g0;` line (both inclusive of the prep `for`, the `nr_ss_add(NRSS_PREP, ss_prep);`, `g0`, `rc = G->run`, the `if (rc != 0) {...return 1;}` block) with:
```c
    int rm_bad[NR_GPU_LDPC_MAX_JOBS] = {0};
    const uint64_t ss_prep = nr_ss_now();
    int dm_rc = 1; /* 1 = not tried */
    if (use_dm) {
      dm_rc = dm_chunk(G, tb, t, (int)r0, n, rm_bad, gpu_ns);
      if (dm_rc != 0)
        use_dm = false; /* this TB goes on with the CPU prep (earlier chunks are final, they only wrote jobs/ok) */
    }
    if (dm_rc != 0) {
      for (int k = 0; k < n; k++) {
        int E;
        uint8_t R;
        int8_t *l = t->l + (size_t)k * (68 * 384 + 16);
        rm_bad[k] = prep_segment(tb, (int)r0 + k, t, l, &E, &R) != 0;
        t->jobs[k] = (nr_gpu_ldpc_job_t){.BG = tb->BG, .R = R, .Z = (uint16_t)tb->Z, .K = (uint16_t)tb->K,
                                         .it_max = tb->max_ldpc_iterations, .llr = l};
      }
    }
    nr_ss_add(NRSS_PREP, ss_prep);
    if (dm_rc != 0) {
      const uint64_t g0 = now_ns();
      const int rc = G->run(t->jobs, n);
      if (rc != 0) {
        atomic_fetch_add_explicit(&G->fb_err, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&G->fb_reason[rc < 0 && -rc < NR_GPU_LDPC_NERR ? -rc : 0], 1, memory_order_relaxed);
        return 1;
      }
      *gpu_ns += now_ns() - g0;
    }
```
  The verdict loop after it (`for (int k = 0; k < n; k++) { /* OAI's sequential verdicts ...`) is unchanged: it reads `rm_bad[k]`, `t->jobs[k].nsnap/syn0/snap`, identical for both paths. Keep the `static` ordering: `dm_chunk` must come after `prep_segment` and `tls_t` and `now_ns`, before `gpu_tb`.
  g) stats: in `stats()`, after the existing `LOG_A(...)`, add
```c
  if (!G->ul && G->dm_stats && atomic_load(&G->dm_on) >= 0) {
    uint64_t o[5 + NR_GPU_DM_CELL_MAX] = {0};
    G->dm_stats(o);
    LOG_A(PHY, "SENSING: GPU_DEMATCH on=%d tbs=%lu cbs=%lu busy=%lu cap=%lu err=%lu geom_refused=%lu check_ok=%lu check_bad=%lu classes=%d fb[init,range,param,graph,stream,sentinel,cap,rt,busy]=[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu] cells=[%lu,%lu,%lu,%lu]\n",
          atomic_load(&G->dm_on), (unsigned long)atomic_load(&G->dm_tbs), (unsigned long)o[1], (unsigned long)o[2], (unsigned long)o[3], (unsigned long)o[4],
          (unsigned long)atomic_load(&G->dm_geom), (unsigned long)atomic_load(&G->dm_chk_ok), (unsigned long)atomic_load(&G->dm_chk_bad), atomic_load(&dm_ncls),
          (unsigned long)atomic_load(&G->dm_fb[1]), (unsigned long)atomic_load(&G->dm_fb[2]), (unsigned long)atomic_load(&G->dm_fb[3]), (unsigned long)atomic_load(&G->dm_fb[4]),
          (unsigned long)atomic_load(&G->dm_fb[5]), (unsigned long)atomic_load(&G->dm_fb[6]), (unsigned long)atomic_load(&G->dm_fb[7]), (unsigned long)atomic_load(&G->dm_fb[8]),
          (unsigned long)atomic_load(&G->dm_fb[9]), (unsigned long)o[5], (unsigned long)o[6], (unsigned long)o[7], (unsigned long)o[8]);
    if (G->dm_check) {
      const int nc = atomic_load(&dm_ncls);
      for (int i = 0; i < nc; i++)
        LOG_A(PHY, "SENSING: DMCLASSN BG=%u Z=%u rv=%u Qm=%u E=%u E2=%u F=%u K=%u lbrm=%u n=%lu ok=%lu bad=%lu\n", dm_cls[i].BG, dm_cls[i].Z, dm_cls[i].rv, dm_cls[i].Qm,
              dm_cls[i].E, dm_cls[i].E2, dm_cls[i].F, dm_cls[i].K, dm_cls[i].lbrm, (unsigned long)(atomic_load(&dm_cls[i].ok) + atomic_load(&dm_cls[i].bad)), (unsigned long)atomic_load(&dm_cls[i].ok),
              (unsigned long)atomic_load(&dm_cls[i].bad));
    }
  }
```
  (`n` printed = ok + bad.)
  h) The stats line fires only at `n == 1 || n == 100 || n % 2000 == 0` (existing `stats()` guard): with CHECK on, also let it fire every 500 TBs: change the guard to `if (!(n == 1 || n == 100 || n % 2000 == 0 || (G->dm_check && n % 500 == 0)))`.

- [ ] **Step 4: Build** `... ninja -j6 -C cmake_targets/ran_build/build_t26 pdsch_gpu nr-uesoftmodem test_nr_gpu_pass test_nr_pdsch_gpu_ldpc < /dev/null 2>&1 | tail -30` (no OUR receiver running). Expected: builds. Then run the regressions of Task 2 Step 6 once more (the interface struct changed, everything recompiles).

- [ ] **Step 5: Flag-off proof (no behaviour change)** - the receiver must be byte-identical with the flag unset: run one replay (Task 5 command, `CAPSET=b`, arm OFF, no new env) and check `grep -c 'GPU_DEMATCH' run.log` = 0 and `GPU_EXACT_LDPC ... check_bad=0` as before. (This is Task 5's OFF arm; reuse its run instead of a separate one.)

- [ ] **Step 6: Commit**
```bash
git add openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c
git commit -m "feat(dematch): ISAC_PDSCH_GPU_DEMATCH host switch, geometry guard, shadow check per (BG,Z,rv,E/E2,F) class, stats (default off)"
```

---

### Task 4: Forced-discrete functional gate (DGX)

**Files:** none (run only).

- [ ] **Step 1:** Orchestrator has re-run cmake (Task 2 Step 9) and `test_nr_gpu_dm_discrete` is registered. Run `cd build_t26 && heavy.sh ctest -R 'test_nr_gpu_dm_discrete|test_nr_gpu_pass_discrete|test_nr_gpu_pass$|test_nr_pdsch_gpu_ldpc' --output-on-failure < /dev/null`. Expected: all pass or skip(77) with a stated reason; `GpuDm.*` PASSED (not skipped) in both models.
- [ ] **Step 2: Forced-discrete live replay.**
```bash
cd /home/nicola/NICOLA/wt/rr-csd-int16 && CAPSET=b ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env TMO=90 tests/passive_rx/p99_replay.sh dm_b_DISC_1 ISAC_PDSCH_GPU_DEMATCH=1 ISAC_PDSCH_GPU_DEMATCH_CHECK=1 ISAC_GPU_FORCE_DISCRETE=1 < /dev/null
```
Expected in `$(cat replays/last)/run.log`: a `GPU_DEMATCH enabled` line; the last `GPU_DEMATCH on=1 ... check_bad=0` with `check_ok > 0`; no `MISMATCH`; no `GPU_DEMATCH requested but unavailable`. If it prints unavailable under FORCE_DISCRETE, the DM init failed (allocation) - report with the log line. Functional only: no timing is quoted from this run.

---

### Task 5: Replay acceptance (bit-exact on both captures) and A/B

**Files:** none (run + report text). Reports are returned as text; the orchestrator saves them.

- [ ] **Step 1: Bit-exact on every class of the captures (CHECK arm).** One replay per capture, n=1 each, flag on, check every TB:
```bash
cd /home/nicola/NICOLA/wt/rr-csd-int16
for cap in b tim; do CAPSET=$cap ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env TMO=90 tests/passive_rx/p99_replay.sh dm_${cap}_CHK_1 ISAC_PDSCH_GPU_DEMATCH=1 ISAC_PDSCH_GPU_DEMATCH_CHECK=1 < /dev/null; done
```
Per run `R=$(cat replays/last)` after each: `grep -c 'DMCLASS ' $R/run.log` (distinct classes seen), `grep 'DMCLASSN' $R/run.log | tail -$(grep -c 'DMCLASS ' $R/run.log)` (last print per class, all `bad=0`), `grep 'GPU_DEMATCH on=' $R/run.log | tail -1` (`check_bad=0`, `geom_refused` small, `busy` and fb counts), `grep -c MISMATCH $R/run.log` = 0. Acceptance (contract): `check_bad=0` and every DMCLASSN row `bad=0` for BOTH captures; the class list includes the tiny BG2 K%8!=0 blocks (T5: Z in {7,11,15}) [MEASURED T5]. A class with `n` small is still bit-exact-covered; list the classes (BG,Z,rv,E/E2,F) in the report per capture, never merged. Any `bad>0`: stop, report the MISMATCH line, do not proceed to Step 2.
`geom_refused` must be small (a few %): if large, the thread-local geometry of Task 3 Step 2 is stale on some call path; report the share and the paths (retry arms re-enter the body with a different `dlsch_config`) instead of silently accepting a low DM coverage.

- [ ] **Step 2: A/B performance, interleaved, flag off vs on, no CHECK.** Arms OFF and ON, n=2 per capture, order OFF1 ON1 OFF2 ON2 per capture (host noise is run-to-run; replay is real-time paced):
```bash
cd /home/nicola/NICOLA/wt/rr-csd-int16
for cap in b tim; do for i in 1 2; do
  for arm in OFF ON; do
    X=""; [ $arm = ON ] && X="ISAC_PDSCH_GPU_DEMATCH=1"
    CAPSET=$cap ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env TMO=90 tests/passive_rx/p99_replay.sh dm_${cap}_${arm}_$i $X < /dev/null || echo "FAILED dm_${cap}_${arm}_$i"
  done; done; done
```
(`t26aXE.env` already has `ISAC_E2E_TRACE=1 ISAC_PDCCH_TIMING=1 ISAC_PDTIM_PCT=1`, so `stg=` and `e2e=` are logged in every arm [CODE-READ t26aXE.env].) Analyse each run one at a time, never loading logs into context:
```bash
nice -n 19 taskset -c 17 timeout 900 python3 tests/passive_rx/stage_split.py replays/replay_dm_b_OFF_1_*/run.log | grep -E "^==|cb >=16" 
nice -n 19 taskset -c 17 timeout 900 python3 tests/passive_rx/goal_metrics.py replays/replay_dm_b_OFF_1_*/run.log
```
Report per capture and arm: CB16+ `first` p50/p90 and `prep` p50/p90 and `ldpc` w/c/g (from the `cb >=16` row of `stage_split.py`), `crc_q` and `lat` from `goal_metrics.py`, the `GPU_DEMATCH on=` line of the ON runs (`busy`, `cap`, `geom_refused`, `err`).
Acceptance (contract): ON vs OFF, per capture, CB16+ first-body p50 lower by >= 0.3 ms in the mean of the two runs and in each run's direction; `crc_q` unchanged (|delta| within the OFF run-to-run spread); no `check_bad` (CHECK off, so none printed: also grep `MISMATCH` = 0). Label `[MEASURED, DGX replay <cap>, t26aXE, ISAC_PDSCH_GPU_DEMATCH 0/1, n=2 each]`. Expected effect: `prep` for CB16+ falls from ~0.3 ms to the staging copy + launch [INFERRED]; `ldpc` wall may rise slightly (it now includes the dematch kernel and the staging H2D).

- [ ] **Step 3: Decision gate.** If ON gains < 0.3 ms at CB16+ p50 but `prep` dropped by more than that, the staging `memcpy` in `nr_pdsch_gpu_dm_run` (one host copy of the LLR window) is the remaining cost: report the measured window size (E x C bytes) and recommend the follow-up (demod writes its LLRs straight into the DM staging buffer; needs the demod owner), do NOT start it in this task. If `busy` > 1 % of calls, the 4-instance pool is too small for the 5 PDSCH threads: report, and the follow-up is a per-thread context, not a longer wait.

- [ ] **Step 4: Report (returned as text).** Sections: what changed (files), unit tests (pass counts), CHECK table per capture (class list, ok/bad), A/B table, forced-discrete result, counters (`busy/cap/err/geom_refused`), not-checked list (sens6, OTA, multi-cell with real second cell, 400 MHz), and the exact commit hashes. The orchestrator saves it as `docs/superpowers/reports/csd/PDEMATCH-<date>.md` and decides the OTA A/B and any default flip (default stays off in this plan).

---

## Self-review
- Contract coverage: kernel into DEV_IN layout (Task 2, via k_rdm; Contract delta 1); feeds `_dev` with stream + `nr_gpu_buf_t` (Task 2); rv/tbslbrm/F/E/E2/first_rE2 (Task 1 fill + existing kernel test); cell index in the job (Task 2 `cell`, Task 3 `dm_cell`, per-cell counters, range test); env flag default off (Task 3c); S/L and mask on device (Task 1 geometry, delta 2); bit-exact on all classes of both captures (Task 5 Step 1, plus 400 random classes in Task 2); replay A/B p50 -0.3 ms + crc_q (Task 5 Step 2); FORCE_DISCRETE functional (Task 2 Step 9, Task 4).
- Type consistency: `nr_gpu_dm_run_fn` = `nr_pdsch_gpu_dm_run` (7 params, same order in header, .cu, host call `G->dm_run(t->jobs, segs, n, tb->dm_cell, tb->llr, tb->G, chk)`, tests `S.run(jobs, segs, n, cell, llr, llr_n, chk)`); `nr_gpu_dm_geom_in_t` fields `nprb,nsym,dmrs_re,unav_res,Qm,Nl` used identically in Tasks 1 and 3; stats array layout `o[0..4]` + `o[5..8]` per cell in header, .cu, host, test.
- No placeholders except the one explicit conditional (Task 2 Step 7 fence-test placement), which states the action.
