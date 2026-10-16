# P-PDCCH (B3 + B5) and P-JANON (item J) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the whole blind-PDCCH candidate search (CORESET FEP result -> LLR -> CCE gather -> unscramble -> polar -> CRC/RNTI test -> compact hit list) onto one GPU submission per slot, add the joint-anonymous (RNTI-less) search as a second item class of that same submission so the `pdcchJanon` thread can be removed, and shorten the Phase-2 lock, with the CPU scan kept as reference and fallback.

**Architecture:** Staged so the early deliverable is evidence, not kernels. Part I dumps recorded scan inputs, builds a CPU oracle and a GPU-vs-oracle parity harness (first arm = the GPU polar path that exists today). Part II builds the device decode stage (`nr_gpu_pdcch_submit/collect`, spec section 5) fed by CPU-made soft bits and integrates it behind `ISAC_PDCCH_GPU_SCAN=1` (this alone removes the polar/CRC cost from the CPU). Part III ports the LLR/demap stages bit-exactly so the CPU stops producing e_rx at all (the a2e target needs this). Part IV is the lock split and gate. Part V is P-JANON on the same chain.

**Tech Stack:** C / CUDA (OAI nr-uesoftmodem fork, DGX GB10 DIRECT, sens6 RTX 4070 DISCRETE via `ISAC_GPU_FORCE_DISCRETE=1`), `libpolar_sc_cuda` (bit-exact SC polar), existing replay harness `tests/passive_rx/p99_replay.sh`, stdlib Python 3 analysis.

**Spec:** `docs/superpowers/specs/2026-10-16-target-gpu-pipeline.md` (sections 1.1 rows 1-2, 2.1-2.5, 3, 5, 6 items B3/B5/J, 7 ranks 9 and 12, 8). Contracts: `docs/superpowers/plans/2026-10-16-phase1-dl-g1-g3.md` Part C Tasks 11 and 13. Evidence: `docs/superpowers/reports/csd/PDCCH-PARBANKS-OTA-20261016.md`, `GPU-INVENTORY-20261016.md` (2a), `T8-TIM-DEAD-20261016.md` (section 2), `GPU4.md`.

## Global Constraints
- Worktree `/home/nicola/NICOLA/wt/rr-csd-int16`, branch `csd/int16` (orchestrator may cut `csd/pdcch-gpu` from it). Builds ONLY as `cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 <targets> < /dev/null`. Agents cannot run cmake: every new target / `add_test` / new source in a CMakeLists is marked **[ORCHESTRATOR: re-run cmake]**; the agent stops at that step and reports it, the orchestrator re-runs cmake, the agent continues.
- Tests and CUDA programs: `heavy.sh ./<test> < /dev/null` from the build dir, with `mkdir -p /tmp/nicola_nomps; export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps`.
- Never build while one of OUR receivers runs: `for p in $(pgrep -x nr-uesoftmodem); do sudo -n readlink /proc/$p/exe; done | grep -q ^/home/nicola/` must be false. Never touch other users' processes. Bounded waits only.
- Agents never run OTA, uhd_*, X410, DPDK or firmware tools (radio unplugged since 2026-10-16 16:10). All validation is unit / replay / rfsim. OTA A/B is the orchestrator's, daytime, n >= 3 per cell, interleaved, V and T separate.
- Replays one at a time: `cd /home/nicola/NICOLA/wt/rr-csd-int16 && ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env CAPSET=b|tim TMO=90 tests/passive_rx/p99_replay.sh <name> <ENV=VAL ...> < /dev/null` (it takes heavy.lock itself). `CAPSET=b` = Vodafone `raw_pci415_12s_b` (V), `CAPSET=tim` = `raw_pci923_tim_12s_tile3` (T). Never pool V and T. Label every number `[MEASURED, DGX replay <cap>, <arm>, n=..]`.
- Every new behaviour behind a default-off `ISAC_*` flag (`ISAC_PDCCH_GPU_SCAN=1`, `ISAC_PDCCH_GPU_JANON=1`); the CPU path stays reference and fallback. **No per-cell constants** (no PCI, frequency, operator, CORESET geometry literal in code or tests; geometry comes from the item/class tables).
- New GPU code: `cell` index in every item and every hit; buffers via `nr_gpu_buf_alloc` pre-fence + `nr_gpu_buf_h2d_async` / `nr_gpu_buf_d2h_async` called UNCONDITIONALLY (`openair1/PHY/TOOLS/nr_gpu_mem.h`); one CUDA context (module dlopen'd like `libpolar_gpu.so`, shared cudart via `nr_gpu_module.c`); RT stream at greatest priority; completion by `cudaStreamQuery` + `nr_gpu_poll_relax_us` (`nr_gpu_poll.h`), never `cudaStreamSynchronize`; every return code carries a reason (`NR_GPU_ERR_*`) and a per-reason counter.
- Each port runs once with `ISAC_GPU_FORCE_DISCRETE=1` on the DGX (functional, not timing).
- Core placement: latency threads on X925 5-9/15-17, helpers A725 0-4/10-14.
- `ISAC_RX_BRANCH_FO` unset. sens6-frozen paths never edited; new configs under tests/passive_rx/ use `.cfg`. Stop receivers with SIGINT only.
- `git add <explicit paths>`, never `-A`, never stash. Commit trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y`. Retry a commit on `.git/index.lock` 5 s apart, max 5.
- Run logs are ~90 MB: never load them into an agent's context (grep / python only). Python: stdlib, `nice -n 19 taskset -c 17 timeout 900 python3 ...`, one at a time.
- gNB logs/configs are ground truth only, never receiver input. Agents return report text; the orchestrator saves and commits reports.

## Review Focus
1. **DCI TDRA field aliasing (T8 section 2):** the GPU path must hand the CPU the COMPLETE raw payload bits (all `len` bits, up to `NR_DCI_MAX_PAYLOAD`=140, never truncated to 64, never interpreted on the device) so the CPU DCI 1_1 resolver can still read bits 16-17 (TDA). Pinned by `test_payload_bits_preserved_47_and_140` (Task 5).
2. **False alarms:** the device prefilter is a SUPERSET of the CPU admission (range + fixed SI/P RNTIs), the CPU class admission still runs on every GPU hit; the GPU hit count per 1000 noise-only items must not exceed the oracle's. Pinned by `test_noise_only_false_alarms_not_higher` (Task 5).
3. **Hit-buffer overflow must never be silent:** a slot with more hits than the compact list holds sets an overflow counter and forces that occasion back to the CPU scan. Pinned by `test_hit_overflow_forces_cpu_fallback` (Task 5).
4. **Stale / reordered completion:** a collect for slot N must never return slot N-1's hits when the ring of 3 wraps (same class of bug as the PDSCH queue epoch). Pinned by `test_collect_slot_tag_matches_submit` (Task 5).
5. **janon superset vs identical:** parity is judged on the recorded post-top-K janon items (deterministic); the live CPU janon is rate/queue/drop dependent and is NOT an oracle. A GPU janon that finds more hits than the CPU on the same item is a FAIL until each extra hit is shown to be a true DCI (re-encode check). Pinned by `test_janon_hit_set_equals_oracle_and_extra_hits_reencode` (Task 12).

## Findings that shape the plan (evidence labels)
- F1 [CODE-READ csd/int16]: the sweep grid is built in `sweep_gpu_prefill()` (`nr_pdcch_blind_monitor_rt.c:3861`): `n_cand <= SWEEP_BATCH_MAX_CAND(45)` x `n_len <= NR_DCI_MAX_PAYLOAD+1(141)` items, one `nr_pdcch_unscrambling` per candidate (not per length), `api->decode_vec(vec, VSTRIDE=16*108, n_cand, vidx, lens, als, n, crcs, pls, oks)`. Results are consumed as a cache of `(crc, payload)` per `(cand, len)`; the CPU applies its own admission (`lane_batch_get` :3798, `...length_scorer_inner` :4040-4140). So the GPU hit contract is `(crc, payload)` and admission stays CPU.
- F2 [CODE-READ]: `npg_decode_vec` (`nr_polar_gpu_mod.c:75`) is a HOST-pointer, mutex-serialised, synchronous API (H2D, kernel, D2H, host `finish()` = info extraction + CRC24C, `nr_polar_sc_cuda_host.c:103`). It is not usable inside a device chain. The SC kernel itself is bit-exact vs `polar_decoder_int16` in steady state (`nr_polar_sc_cuda.h` header comment, `nr_polar_sc_cuda_test.cc`). Today's GPU polar is only used by the sweep / lane batch; the tracked-RNTI hot path (`nr_pdcch_blind_cand_worker_body`, :4290) decodes on the CPU.
- F3 [MEASURED GB10, `GPU4.md`, 2026-10-09]: the existing PDCCH GPU FEP/LLR (`nr_pdcch_gpu_fep.cu`) has LLR sign agreement 88.283 % (`b`) and 77.195 % (`20261008`) vs the CPU; "No GPU LLR is consumed by polar search"; CPU produces 40,612/76,745 zero LLRs vs GPU 8,626/13,893 (CPU branch quality gate, fixed-point compensation, symbol P90 autoscale and clipping not ported). The `nr_pdcch_gpu_fep.h` header states multi-antenna combining there is plain power-weighted MRC. => "hit set identical" through a GPU LLR stage requires a bit-exact port of `nr_rx_pdcch_symbol` (`dci_nr.c:420-1060`). This is the long pole of Part III.
- F4 [CODE-READ]: `nr_pdcch_gpu_record.c` (`ISAC_PDCCH_GPU_DUMP`, `_MAX`) already records, per CORESET, the CPU-FFT'd IQ `[nant][dur][N]` and the CPU LLR (pre-demap, per RE) with `nr_pdcch_gpu_record_t` (magic `0x47435032`). `nr_pdcch_slot_bench` replays it. It does NOT record candidates (post-demap e_rx), items, or hits.
- F5 [CODE-READ]: janon = `janon_submit` (:2952, rate/top-K/hash sampling, bounded queue, drop counted) + `janon_worker` (:2851): `nr_pdcch_unscrambling(e, 0, L*108, nid)`, then per length `nr_pdcch_blind_decode_raw_11` (n_RNTI=0 polar: recovered CRC value IS the RNTI = "mask recovery", range test, FI bit) and on failure `nr_pdcch_joint_live_decode_11` (prescreen + OSD order 0 GF(2) solve, `nr_pdcch_joint_solve.c:188`, accepted if `mismatched_bits <= joint_anon_mm_max(L)`). `ISAC_PDCCH_JOINT=1` and `ISAC_PDCCH_JOINT_ANON_ASYNC=1` are both in the default env `t26aXE.env`, so the joint solve is on the live path. [MEASURED DGX OTA 2026-10-16, V] 282k of 539k submissions dropped (52 %); T 8.7k of 196k (GPU-INVENTORY 2a).
- F6 [CODE-READ]: the "Phase-2 lock" in code is a MUTEX around the occasion tail (`nr_pdcch_blind_phase2.c`, `g_phase2_mu`; accepts, dedupe, RNTI persistence, CFR/PDSCH submit); "a discovery-capable config (autodiscover) runs entirely under the phase-2 lock" (`nr_pdcch_blind_monitor_rt.c:5256`); `BTIM_P2WAIT` (:2445) already measures the wait. Spec 2.4 describes it as a hypothesis-lock state machine (N consecutive hits). These are two different things; Task 10 handles both explicitly.
- F7 [CODE-READ]: `nr_pdcch_joint_prescreen` (`nr_pdcch_joint_solve.c:364`) sums in double; `cmp_wi` (:179) orders reliabilities with `qsort` (ties unspecified). Both matter for bit-exact device ports (Task 13).
- F8 [CODE-READ]: `nr_gpu_pipe_t`, `nr_gpu_fence_t`, `nr_gpu_dci_hit_t`, `nr_gpu_slot_ref_t` exist only in the spec (grep over csd/int16 finds them nowhere else). This plan defines the PDCCH-relevant subset in a new header and does not build the general pipe.
- F9 [MEASURED OTA 2026-10-16, PDCCH-PARBANKS-OTA]: more CPU scan threads made the scan slower (bank pass 158-182 -> 219-272 us); PAR_BANKS and the 3rd scan thread are abandoned. Nothing in this plan adds CPU scan threads.

## File Structure
Create:
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_record.h` / `.c` — record format v2 (inputs of the decode stage + janon items) and writer.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_oracle.h` / `.c` — CPU oracle: dump -> expected hit list (uses `nr_pdcch_unscrambling`, `polar_decoder_int16`, joint solve). CPU-only.
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_record_test.cc`, `nr_pdcch_scan_oracle_test.cc` — CPU tests (linked like `test_nr_pdcch_blind_monitor`, root `CMakeLists.txt:2516`).
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_gpu.h` — the public C API (spec section 5 subset) + hit/class/item structs.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_gpu.cu` — module `libpdcch_scan_gpu.so` (unscramble, decode glue, finish, prefilter, compaction; later LLR, demap, janon kernels).
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_gpu_client.c` / `.h` — dlopen loader + host-side glue used by `nr_pdcch_blind_monitor_rt.c` (keeps the 10.9k-line file from growing).
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_parity.cc` — GPU-vs-oracle parity tool + ctest (CUDA).
- `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_gpu_test.cc` — synthetic-DCI unit tests (CUDA).
- `tests/passive_rx/pdcch_scan_stats.py` + `tests/passive_rx/test_pdcch_scan_stats.py` — replay counter extraction (a2e, scan wait/run, janon, shadow mismatches).
Modify:
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` — dump seams, janon worker body extraction, GPU cache hookup, lock narrowing. Minimal edits, logic moved into the new files where possible.
- `openair1/PHY/CODING/nrPolar_tools/cuda/nr_polar_sc_cuda.{cu,h}`, `nr_polar_sc_cuda_host.c` — device-resident finish entry `npc_decode_dev`.
- `openair1/PHY/CODING/CMakeLists.txt` (module + tests inside `if(ENABLE_LDPC_CUDA)`, add `pdcch_scan_gpu` to the `foreach(module ...)` list at ~:278), root `CMakeLists.txt` (CPU tests next to :2516, new .c sources next to :1027) — **[ORCHESTRATOR: re-run cmake after each CMake edit]**.
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_phase2.c/.h` (Task 10).
- `docs/superpowers/reports/csd/` — one report per gate (orchestrator saves).

Order and dependencies: 1 -> 2 -> 3 (EARLY DELIVERABLE, no new kernels) -> 4 -> 5 -> 6 (stage A shippable) ; 7 -> 8 -> 9 (stage B) ; 10 ; 11 (P-PDCCH gate). P-JANON: 12 needs only Task 5; 13 needs Task 12; 14 needs 6 + 13. Tasks 12-13 can run in parallel with Part III.

---

# Part I: dump, oracle, parity harness (EARLY DELIVERABLE)

### Task 1: Scan-input record format and dump hooks

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_record.h`, `nr_pdcch_scan_record.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_record_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (three seams, see Step 4)
- Modify: root `CMakeLists.txt` (source list near :1027 and test near :2516) **[ORCHESTRATOR: re-run cmake]**

**Interfaces:**
- Produces (used by Tasks 2, 3, 6, 12): the record API below. A record = everything the decode stage needs for one occasion (kind OCC) or one janon queue item (kind JANON), but not the live CPU results (the oracle recomputes them; live hits are traced separately for a cross-check).

- [ ] **Step 1: Write the header** `nr_pdcch_scan_record.h`

```c
#ifndef NR_PDCCH_SCAN_RECORD_H
#define NR_PDCCH_SCAN_RECORD_H
#include <stdint.h>
#include <stdio.h>
#include "nr_dci_bits.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NR_PDCCH_SCAN_REC_MAGIC 0x53434E32u /* 'SCN2' */
#define NR_PDCCH_SCAN_MAX_CAND 45           /* == SWEEP_BATCH_MAX_CAND */
#define NR_PDCCH_SCAN_MAX_CLASS 8
#define NR_PDCCH_SCAN_MAX_LIST 8
enum { NR_SCAN_KIND_OCC = 1, NR_SCAN_KIND_JANON = 2 };
enum { NR_SCAN_FI_ANY = 0, NR_SCAN_FI_DL = 1 /* payload bit len-1 set */, NR_SCAN_FI_UL = 2 /* clear */ };
enum { NR_SCAN_LENS_RANGE = 0, NR_SCAN_LENS_LIST = 1 };
/* One decode class = one (scrambling, admission, length set) applied to a subset of the candidates. */
typedef struct {
  uint16_t scr_rnti;          /* data-scrambling RNTI used by nr_pdcch_unscrambling (0 = n_RNTI 0) */
  uint16_t rnti_min, rnti_max;
  uint8_t fi_req;             /* NR_SCAN_FI_* */
  uint8_t fixed_ok;           /* 1: also pass crc 0xFFFF (SI) and 0xFFFE (P), as nr_pdcch_blind_decode_10 does */
  uint8_t lens_mode;          /* NR_SCAN_LENS_* */
  uint8_t n_list;             /* LIST: number of explicit lengths */
  uint16_t len_lo, len_hi;    /* RANGE: inclusive */
  uint16_t list[NR_PDCCH_SCAN_MAX_LIST];
  uint64_t cand_mask;         /* bit c set: candidate c belongs to this class */
} nr_pdcch_scan_class_t;
typedef struct {
  uint32_t magic, bytes, version, kind;
  uint32_t abs_slot;
  int32_t frame, slot;
  uint8_t cell, n_cand, n_cls, pad0;
  uint16_t dmrs_id;           /* pdcch-DMRS-ScramblingID (nID) */
  uint16_t pad1;
  uint64_t geom;              /* opaque occasion/CORESET key as the monitor uses it */
  /* payload: n_cls x nr_pdcch_scan_class_t; n_cand x {uint8 L; uint8 pad; uint16 cce}; n_cand x int16 e_rx[L*108] (RAW, not unscrambled) */
} nr_pdcch_scan_rec_hdr_t;
typedef struct { uint8_t L; uint16_t cce; const int16_t *e_rx; } nr_pdcch_scan_cand_t;
/* In-memory view of one record (pointers into a caller buffer). */
typedef struct {
  nr_pdcch_scan_rec_hdr_t h;
  nr_pdcch_scan_class_t cls[NR_PDCCH_SCAN_MAX_CLASS];
  nr_pdcch_scan_cand_t cand[NR_PDCCH_SCAN_MAX_CAND];
} nr_pdcch_scan_rec_t;

/* Writer: appends one record to the file named by ISAC_PDCCH_SCAN_DUMP (up to ISAC_PDCCH_SCAN_DUMP_MAX records,
 * default 4096, kind filter ISAC_PDCCH_SCAN_DUMP_KIND=occ|janon|all default all). Thread-safe, never blocks the scan for
 * more than the fwrite; returns 1 if written. No-op (returns 0) when the env is unset. */
int nr_pdcch_scan_record_write(const nr_pdcch_scan_rec_t *r);
/* Reader: reads the next record from f into buf (cap bytes); fills *r with pointers into buf. Returns 1, 0 at EOF, -1 on corrupt. */
int nr_pdcch_scan_record_read(FILE *f, uint8_t *buf, size_t cap, nr_pdcch_scan_rec_t *r);
/* Serialise to a caller buffer (used by the writer and the test). Returns bytes or -1 if cap too small. */
long nr_pdcch_scan_record_pack(const nr_pdcch_scan_rec_t *r, uint8_t *out, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write the failing test** `tests/nr_pdcch_scan_record_test.cc`

```cpp
#include "nr_pdcch_scan_record.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
int main()
{
  static int16_t e0[2 * 108], e1[8 * 108];
  for (int i = 0; i < 2 * 108; i++) e0[i] = (int16_t)(i - 100);
  for (int i = 0; i < 8 * 108; i++) e1[i] = (int16_t)(7 * i - 3000);
  nr_pdcch_scan_rec_t r;
  memset(&r, 0, sizeof r);
  r.h = {NR_PDCCH_SCAN_REC_MAGIC, 0, 2, NR_SCAN_KIND_OCC, 4242, 17, 3, 1, 2, 2, 0, 0, 0x1234, 0, 0xABCDEF};
  r.cls[0] = {};
  r.cls[0].scr_rnti = 0; r.cls[0].rnti_min = 1; r.cls[0].rnti_max = 0xFFEF; r.cls[0].fi_req = NR_SCAN_FI_DL;
  r.cls[0].lens_mode = NR_SCAN_LENS_RANGE; r.cls[0].len_lo = 12; r.cls[0].len_hi = 60; r.cls[0].cand_mask = 0x3;
  r.cls[1] = {};
  r.cls[1].scr_rnti = 0x4601; r.cls[1].rnti_min = r.cls[1].rnti_max = 0x4601; r.cls[1].fi_req = NR_SCAN_FI_UL;
  r.cls[1].lens_mode = NR_SCAN_LENS_LIST; r.cls[1].n_list = 2; r.cls[1].list[0] = 39; r.cls[1].list[1] = 45; r.cls[1].cand_mask = 0x2;
  r.cand[0] = {2, 6, e0};
  r.cand[1] = {8, 16, e1};
  std::vector<uint8_t> buf(1 << 16);
  const long n = nr_pdcch_scan_record_pack(&r, buf.data(), buf.size());
  assert(n > 0);
  FILE *f = tmpfile();
  assert(fwrite(buf.data(), 1, (size_t)n, f) == (size_t)n);
  rewind(f);
  std::vector<uint8_t> rb(1 << 16);
  nr_pdcch_scan_rec_t q;
  assert(nr_pdcch_scan_record_read(f, rb.data(), rb.size(), &q) == 1);
  assert(q.h.cell == 2 && q.h.n_cand == 2 && q.h.n_cls == 2 && q.h.dmrs_id == 0x1234 && q.h.geom == 0xABCDEF);
  assert(q.cls[1].list[1] == 45 && q.cls[0].len_hi == 60 && q.cls[0].cand_mask == 3);
  assert(q.cand[1].L == 8 && q.cand[1].cce == 16 && memcmp(q.cand[1].e_rx, e1, sizeof e1) == 0);
  assert(memcmp(q.cand[0].e_rx, e0, sizeof e0) == 0);
  assert(nr_pdcch_scan_record_read(f, rb.data(), rb.size(), &q) == 0);
  /* corrupt magic must be refused, not mis-parsed */
  buf[0] ^= 0xFF;
  rewind(f); ftruncate(fileno(f), 0); fwrite(buf.data(), 1, (size_t)n, f); rewind(f);
  assert(nr_pdcch_scan_record_read(f, rb.data(), rb.size(), &q) == -1);
  puts("nr_pdcch_scan_record_test PASS");
  return 0;
}
```
(add `#include <unistd.h>` for `ftruncate`.)

- [ ] **Step 3: Implement** `nr_pdcch_scan_record.c` (`pack` writes header with `bytes`, then classes, then `{L,pad,cce}` x n_cand, then each `e_rx` of `L*108` int16; `read` validates magic, `bytes <= cap`, `n_cand <= 45`, `n_cls <= 8`, each `L ∈ {1,2,4,8,16}`; `write` takes a static mutex, lazily reads the three env vars once, counts records). Register the test in root `CMakeLists.txt` next to `test_nr_pdcch_blind_monitor` (:2516) as a plain executable with `add_test(NAME test_nr_pdcch_scan_record COMMAND test_nr_pdcch_scan_record)` and add `nr_pdcch_scan_record.c` to the source list near :1027. **[ORCHESTRATOR: re-run cmake]**

- [ ] **Step 4: Dump seams in `nr_pdcch_blind_monitor_rt.c`.** Each seam builds an `nr_pdcch_scan_rec_t` from data already in scope and calls `nr_pdcch_scan_record_write`; guard with a cached `static int on = -1` read of `getenv("ISAC_PDCCH_SCAN_DUMP")` so the off path costs one predictable branch. Read 40 lines around each before editing.
  - Seam OCC-sweep: end of `sweep_gpu_prefill()` (:3861-3930) — class `{scr_rnti=ctx->scrambling_rnti, rnti_min/max=ctx->rnti_min/max, fi_req=NR_SCAN_FI_DL, lens_mode=LIST from lengths[], cand_mask=all}`, candidates from `ctx->cand[c]` (`cd->L`, `cd->cce`, `cd->e_rx`), `dmrs_id=ctx->dmrs_scrambling_id`.
  - Seam OCC-tracked: top of `nr_pdcch_blind_cand_worker_body()` (:4300) — one class `{scr_rnti=t->scrambling_rnti, rnti range, fi_req by t->ul_scan / format, lens_mode=LIST {t->dci_length}}`, one candidate (`t->e_rx`, `t->L`, `t->cce`). (Tracked items are recorded one per record; the parity tool merges records with the same `(abs_slot, geom)`.)
  - Seam JANON: in `janon_submit()` right after the `it->c[k]` copy loop (:3040-3057) — `kind=NR_SCAN_KIND_JANON`, class `{scr_rnti=0, rnti_min=it->rnti_min, rnti_max=it->rnti_max, fi_req=NR_SCAN_FI_DL, fixed_ok=0, lens_mode = it->nlen ? LIST(it->lens) : RANGE(dci_len_min(), min(dci_len_max(),64)), cand_mask=(1<<m)-1}`, `dmrs_id=nid`.
  Do not change any control flow.

- [ ] **Step 5: Build and run the unit test**

Run: `cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26 && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 test_nr_pdcch_scan_record nr-uesoftmodem < /dev/null && /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./test_nr_pdcch_scan_record < /dev/null`
Expected: `nr_pdcch_scan_record_test PASS`; `nr-uesoftmodem` links.

- [ ] **Step 6: Commit**
```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_record.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_record.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_record_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c CMakeLists.txt
git commit -m "feat(pdcch-gpu): scan-input record format v2 and dump seams (ISAC_PDCCH_SCAN_DUMP)" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y"
```

### Task 2: CPU oracle (dump -> expected hit list) and janon worker-body extraction

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_oracle.h`, `nr_pdcch_scan_oracle.c`, `tests/nr_pdcch_scan_oracle_test.cc`
- Modify: `nr_pdcch_blind_monitor_rt.c` (janon worker body -> `janon_solve_item`)
- Modify: root `CMakeLists.txt` **[ORCHESTRATOR: re-run cmake]**

**Interfaces:**
- Consumes: `nr_pdcch_scan_rec_t` (Task 1); `nr_pdcch_unscrambling` (`dci_nr.c:1146`), `polar_decoder_int16` as called in `blind_polar_decode` (`nr_pdcch_blind_monitor.c:4197`: `polar_decoder_int16((int16_t*)llr, dci_estimation, 1, NR_POLAR_DCI_MESSAGE_TYPE, dci_length, aggregation_level)`), `nr_pdcch_blind_decode_raw_11`, `nr_pdcch_joint_live_decode_11`.
- Produces (Tasks 3, 5, 6, 12, 13, 14):
```c
typedef struct { uint8_t cell, cls, cand, L; uint16_t cce, len; uint32_t crc; uint16_t rnti; uint8_t via_joint; uint16_t mismatched_bits; nr_dci_bits_t payload; } nr_pdcch_scan_hit_t;
/* DECODE-STAGE oracle (kind OCC): every (cand in cls.cand_mask, len in lens(cls)) with len+24 <= L*108:
 * unscramble, polar_decoder_int16, keep if (crc>>16)==0 && ((crc in [rnti_min,rnti_max]) || (fixed_ok && (crc==0xFFFF||crc==0xFFFE)))
 * && FI test. This is the SUPERSET prefilter of F1; CPU class admission is NOT applied here. Returns number of hits written. */
int nr_pdcch_scan_oracle_occ(const nr_pdcch_scan_rec_t *r, nr_pdcch_scan_hit_t *out, int max);
/* JANON oracle (kind JANON): exactly the loop body of janon_worker, synchronous, no queue, no drop, no admission gate. */
int nr_pdcch_scan_oracle_janon(const nr_pdcch_scan_rec_t *r, nr_pdcch_scan_hit_t *out, int max);
```

- [ ] **Step 1: Extract the janon worker body.** In `nr_pdcch_blind_monitor_rt.c` move the `for (int c = 0; c < it.n; c++) {...}` block of `janon_worker` (:2889-2933) into `static int janon_solve_item(const janon_item_t *it, janon_hit_t *out, int max, int *via_joint_cnt)` returning the number of hits; the worker then calls it and does the `g_janon_hits` / overflow bookkeeping. Behaviour-preserving (same unscramble, same `raw_11` then joint fallback, same `joint_anon_mm_max`, same log lines). The oracle in this task calls the same public functions (`nr_pdcch_blind_decode_raw_11`, `nr_pdcch_joint_live_decode_11`, `joint_anon_mm_max` made non-static as `nr_pdcch_joint_anon_mm_max`) rather than the static one, so the oracle does not depend on the 10k-line file.

- [ ] **Step 2: Write the failing test** `tests/nr_pdcch_scan_oracle_test.cc`: build one synthetic record with `polar_encoder_fast` (as `joint_encode_cb`, `nr_pdcch_joint_live.c:11-15`) — encode a 47-bit DL DCI with CRC mask rnti 0x4601, AL 4, scrambled with the Gold sequence of `c_init=((0x4601<<16)+nid)%2^31` using `nr_pdcch_unscrambling` applied to a +/-127 hard LLR vector (scrambling is its own inverse on the sign), embed it as candidate 1 of 3 (the other two are fixed-seed noise), class 0 = RANGE 20..60 with `scr_rnti=0x4601`, then assert:
```cpp
nr_pdcch_scan_hit_t h[64];
int n = nr_pdcch_scan_oracle_occ(&rec, h, 64);
bool found = false;
for (int i = 0; i < n; i++) if (h[i].cand == 1 && h[i].len == 47 && h[i].rnti == 0x4601) {
  found = true; assert(h[i].L == 4 && h[i].payload.w[0] == expected_payload_w0);
}
assert(found);
/* determinism */ nr_pdcch_scan_hit_t h2[64]; assert(nr_pdcch_scan_oracle_occ(&rec, h2, 64) == n && !memcmp(h, h2, n * sizeof h[0]));
/* length gate */ rec.cls[0].len_hi = 46; n = nr_pdcch_scan_oracle_occ(&rec, h, 64); for (i..) assert(h[i].len != 47);
```
plus a janon record with `scr_rnti=0`, same DCI scrambled with n_RNTI=0 and the RNTI only in the CRC mask: `nr_pdcch_scan_oracle_janon` returns `rnti==0x4601`, `via_joint==0`.

- [ ] **Step 3: Run it to verify it fails** (`undefined reference nr_pdcch_scan_oracle_occ`). Build target `test_nr_pdcch_scan_oracle`, link like `test_nr_pdcch_blind_monitor` (root `CMakeLists.txt:2516-2530`: copy its `target_sources` / `target_link_libraries`). **[ORCHESTRATOR: re-run cmake]**

- [ ] **Step 4: Implement the oracle.** For the OCC kind, for each class and each candidate in `cand_mask`: `nr_pdcch_unscrambling((c16_t*)e_rx, cls.scr_rnti, L*108, dmrs_id, z)` once per `(cand, class)`, then for each length `polar_decoder_int16`; prefilter exactly as documented above; fill `nr_pdcch_scan_hit_t`. FI test: `nr_dci_bits_field(&payload, len, 0, 1) != 0` for DL, `== 0` for UL. Janon kind: call the shared decode used by `janon_solve_item`.

- [ ] **Step 5: Run, verify pass; commit**

Run: `heavy.sh nice -n 19 ninja -j6 test_nr_pdcch_scan_oracle nr-uesoftmodem < /dev/null && heavy.sh ./test_nr_pdcch_scan_oracle < /dev/null` -> PASS. Also run the existing `test_nr_pdcch_blind_monitor` (197 pass + 2 skips expected per PROJECT_MEMORY) to prove the extraction changed nothing.
```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_oracle.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_scan_oracle.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_oracle_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c CMakeLists.txt
git commit -m "feat(pdcch-gpu): CPU oracle for recorded scan inputs; janon worker body extracted (behaviour-preserving)" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y"
```

### Task 3: Record the two captures, GPU parity harness with today's GPU polar as the first arm

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_parity.cc`
- Modify: `openair1/PHY/CODING/CMakeLists.txt` (inside `if(ENABLE_LDPC_CUDA)`) **[ORCHESTRATOR: re-run cmake]**
- Create: `tests/passive_rx/pdcch_scan_stats.py`, `tests/passive_rx/test_pdcch_scan_stats.py` (used from Task 6 on)

**Interfaces:**
- Consumes: Task 1 reader, Task 2 oracle (the parity tool links BOTH the oracle, whose CPU libs it needs, and `dlopen`s the GPU module, so it is built like `test_nr_pdcch_scan_oracle` plus `${CMAKE_DL_LIBS}`; it does not link CUDA).
- Produces: CLI `nr_pdcch_scan_parity <dump.bin> [--arm npg|dev|chain] [--cells N] [--max-records M]` printing, per kind: `records`, `items`, `oracle_hits`, `gpu_hits`, `missing` (oracle-only), `extra` (gpu-only), `false_alarm_rate = extra_nonDCI / items`, and exit code 0 only if `missing==0 && extra==0`. A hit key is `(cell, cand-position (L, cce), len, crc, payload)`. Arms: `npg` = the existing `libpolar_gpu.so` `decode_vec` (host finish); `dev` (Task 5) and `chain` (Task 9) are added by those tasks.

- [ ] **Step 1: Capture the dumps (orchestrator-visible, one replay at a time).** Needs Task 1 built into `nr-uesoftmodem`.
```
cd /home/nicola/NICOLA/wt/rr-csd-int16
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env CAPSET=b TMO=90 tests/passive_rx/p99_replay.sh scan_dump_b ISAC_PDCCH_SCAN_DUMP=/tmp/nicola_scan_b.bin ISAC_PDCCH_SCAN_DUMP_MAX=6000 < /dev/null
ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env CAPSET=tim TMO=90 tests/passive_rx/p99_replay.sh scan_dump_tim ISAC_PDCCH_SCAN_DUMP=/tmp/nicola_scan_tim.bin ISAC_PDCCH_SCAN_DUMP_MAX=6000 < /dev/null
```
Expected: both files non-empty; record count by kind printed by the parity tool in Step 4. Note the dump perturbs timing: dump replays are NEVER used for latency numbers. Keep the files out of git (size). Also capture janon items: same run (kind all).

- [ ] **Step 2: Write the failing test.** Skeleton of `nr_pdcch_scan_parity.cc` main with the key comparison (write the full file; the comparison core):
```cpp
struct Key { uint8_t cell; uint8_t L; uint16_t cce, len; uint32_t crc; uint64_t w[NR_DCI_WORDS]; bool operator<(const Key&) const; };
static std::set<Key> keys(const nr_pdcch_scan_hit_t *h, int n) { /* cell,L,cce,len,crc,payload words */ }
int compare(const char *tag, const std::set<Key>& want, const std::set<Key>& got, long items) {
  int missing = 0, extra = 0;
  for (auto& k : want) if (!got.count(k)) missing++;
  for (auto& k : got) if (!want.count(k)) extra++;
  printf("%s items=%ld oracle=%zu gpu=%zu missing=%d extra=%d\n", tag, items, want.size(), got.size(), missing, extra);
  return missing + extra;
}
```
`--arm npg`: for each OCC record, replicate `sweep_gpu_prefill` (unscramble per (cand,class) with the oracle's unscramble, `nr_gpu_polar_api_v2()->decode_vec`, prefilter) via `dlopen("libpolar_gpu.so")` + `dlsym("nr_gpu_polar_api_v2")` (signature in `nr_polar_gpu.h`); group by record, batch up to 45 cands x 141 lens.

- [ ] **Step 3: Run it to verify it fails to build** (CMake target missing) -> add target `nr_pdcch_scan_parity` (+ `add_test(NAME nr_pdcch_scan_parity_synth COMMAND nr_pdcch_scan_parity --synthetic ...)` only after Task 5). **[ORCHESTRATOR: re-run cmake]**

- [ ] **Step 4: Run on the recorded dumps and record the baseline**
```
cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./nr_pdcch_scan_parity /tmp/nicola_scan_b.bin --arm npg < /dev/null
/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./nr_pdcch_scan_parity /tmp/nicola_scan_tim.bin --arm npg < /dev/null
```
Expected: `missing=0 extra=0` for kind OCC on both (GPU SC is documented bit-exact). Any non-zero is a finding, not something to "fix by tolerance": report the first 5 differing keys (cell, L, cce, len, crc, payload hex). Also run `ISAC_GPU_FORCE_DISCRETE=1` once for the `npg` arm.

- [ ] **Step 5: Write `pdcch_scan_stats.py` with a failing unittest first.** It extracts from a replay `run.log` (never loading it whole): the `a2e` p50/p99 from the `e2e=a2e,qw,pre,dec,first,bodies,tot,lag` EVMGRANT field (`ISAC_E2E_TRACE`; confirm the exact field order by `grep -m3 -o 'e2e=[0-9,-]*' run.log` first), the `SCAN_E2E` wait/run percentiles and bank-pass numbers (`ISAC_PDCCH_SCAN_TIMING=1` is in `t26aXE.env`), `JOINT_ANON_ASYNC items submitted=.. done=.. dropped=..`, `g_pdcch_gpu_equal/mismatch/error` and the new Task 6/9 shadow lines. Unit test with a 20-line fixture log string asserting the percentile values (nearest-rank p99). Run: `nice -n 19 taskset -c 17 timeout 900 python3 tests/passive_rx/test_pdcch_scan_stats.py`.

- [ ] **Step 6: Commit; report baseline** (record counts, `oracle_hits` per cap, parity of `npg` arm, number of JANON records and fraction of janon oracle hits with `via_joint=1` — this fraction decides Task 13's priority).
```bash
git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_scan_parity.cc openair1/PHY/CODING/CMakeLists.txt tests/passive_rx/pdcch_scan_stats.py tests/passive_rx/test_pdcch_scan_stats.py
git commit -m "test(pdcch-gpu): GPU-vs-CPU-oracle parity harness on recorded scan inputs; baseline of the npg arm" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>" -m "Claude-Session: https://claude.ai/code/session_01TAoCwYXxAmX1GFz7dihf4y"
```

---

# Part II: device decode stage (stage A)

### Task 4: Device-resident polar finish (info extraction + CRC24C) in the SC library

**Files:**
- Modify: `openair1/PHY/CODING/nrPolar_tools/cuda/nr_polar_sc_cuda.cu`, `nr_polar_sc_cuda.h`, `nr_polar_sc_cuda_host.c`, `nr_polar_sc_cuda_test.cc`

**Interfaces:**
- Consumes: `npc_register`, `finish()` (`nr_polar_sc_cuda_host.c:103`) as the reference.
- Produces (Task 5):
```c
/* Everything on the device. dvec: n_vec vectors of vstride int16 (already unscrambled), pid[i]/vidx[i] per item on the device,
 * outputs on the device: crc[i] (24-bit, = what polar_decoder_int16 returns), payload[i] (nr_dci_bits_t). Async on `stream`
 * (cudaStream_t as void*). Returns 0 or -(NPC_ERR_*) with a reason. n <= NPC_MAX_ITEMS_DEV (4096). */
int npc_decode_dev(const int16_t *dvec, int vstride, int n_vec, const int *dvidx, const int *dpid, int n,
                   uint32_t *dcrc, nr_dci_bits_t *dpayload, void *stream);
```

- [ ] **Step 1: Failing test** in `nr_polar_sc_cuda_test.cc`: new mode `dev` that, for every registered param (all 704 `(len, AL)` combinations the test already enumerates), generates 200 encoded+noisy vectors (the existing generator), runs `npc_decode_batch_vec` (host finish) and `npc_decode_dev` into `cudaMalloc`'d buffers (this test file is CUDA, so raw cudart is allowed here), and requires `crc` and the three payload words identical for every item. Add `add_test(NAME nr_polar_sc_cuda_test_dev COMMAND nr_polar_sc_cuda_test dev)` **[ORCHESTRATOR: re-run cmake]**. Run -> fails (`npc_decode_dev` undefined).
- [ ] **Step 2: Implement `k_finish`**: one thread per item after the SC kernel (same stream): gather info bits from `u` using the registered `info` pattern into `B[0..2]` exactly like `finish()` (:103-146: `payloadBits`, `crcParityBits`, the three length regimes `<=32`, `<=64`, `>64`), compute CRC24C with a 256-entry device table (copy of `crc24c`), `crc ^ rxcrc`. The u-vector stays in a device scratch of `n * NPC_MAX_N` bytes allocated at `npc_warmup` time (pre-fence). `npc_decode_batch_vec` stays untouched as the reference.
- [ ] **Step 3: Build/run** `heavy.sh nice -n 19 ninja -j6 nr_polar_sc_cuda_test polar_gpu < /dev/null && heavy.sh ./nr_polar_sc_cuda_test dev < /dev/null` -> `BIT-EXACTNESS: PASS` for dev mode. Also run once `ISAC_GPU_FORCE_DISCRETE=1` (test uses raw cudart so it is mode-independent; the flag matters from Task 5).
- [ ] **Step 4: Bench note** (no gate): print kernel + finish time for n = 1530 and n = 4096 at AL 1/4/8; add to the Task 5 report. This is the first real number against the 60 us after-t=0 allocation of spec table 1.1 row 1; it is [INFERRED] until then.
- [ ] **Step 5: Commit** (`git add` the four files).

### Task 5: `nr_gpu_pdcch_submit_cand / collect` module: unscramble, decode, prefilter, compact hits, multi-cell

**Files:**
- Create: `nr_pdcch_scan_gpu.h`, `nr_pdcch_scan_gpu.cu`, `tests/nr_pdcch_scan_gpu_test.cc`
- Modify: `openair1/PHY/CODING/CMakeLists.txt` (module `pdcch_scan_gpu`, test, add to `foreach(module ...)`, `add_dependencies(nr-uesoftmodem pdcch_scan_gpu)`) **[ORCHESTRATOR: re-run cmake]**; extend `nr_pdcch_scan_parity.cc` with `--arm dev` and `--synthetic`.

**Interfaces:**
- Consumes: `npc_decode_dev` (Task 4), `nr_gpu_buf_*`, `nr_gpu_poll_relax_us`.
- Produces (Tasks 6, 9, 12, 14) — `nr_pdcch_scan_gpu.h`:
```c
#define NR_GPU_N_CELL_MAX 4
#define NR_GPU_PDCCH_ERR_NOT_INIT   (-1)
#define NR_GPU_PDCCH_ERR_RANGE      (-2)  /* n_items / n_cand / class index out of range */
#define NR_GPU_PDCCH_ERR_BUSY       (-3)  /* ring of in-flight slots full */
#define NR_GPU_PDCCH_ERR_CUDA       (-4)
#define NR_GPU_PDCCH_ERR_NO_PARAM   (-5)  /* (len, AL) not registered */
typedef struct nr_gpu_pipe nr_gpu_pipe_t;           /* opaque; PDCCH subset of the spec pipe (F8) */
typedef struct { uint64_t seq; int slot_tag; int idx; } nr_gpu_fence_t;
typedef struct { uint32_t abs_slot; uint16_t sym_ready_mask; } nr_gpu_slot_ref_t;
/* Spec section 5 item. dci_len_mask_idx indexes the per-cell CLASS table below (documented deviation: it carries the
 * scrambling RNTI, RNTI range, FI rule and length set, which the spec text left implicit). */
typedef struct { uint8_t cell, coreset, ss, L; uint16_t cce0, dci_len_mask_idx, dmrs_id; uint8_t flags, pad; } nr_gpu_pdcch_item_t;
enum { NR_GPU_PDCCH_F_JANON = 1, NR_GPU_PDCCH_F_HAVE_LLR = 2 /* e_rx supplied by host (stage A) */ };
typedef struct { uint16_t scr_rnti, rnti_min, rnti_max; uint8_t fi_req, fixed_ok, lens_mode, n_list; uint16_t len_lo, len_hi, list[8]; } nr_gpu_pdcch_class_t; /* same fields as nr_pdcch_scan_class_t without cand_mask */
typedef struct { uint8_t cell, coreset, ss, L; uint16_t cce, len; uint8_t kind, via_joint; uint16_t rnti, mismatched_bits; uint32_t crc; uint32_t abs_slot; nr_dci_bits_t payload; } nr_gpu_dci_hit_t;
typedef struct { int n_cells; int max_items; int max_hits; /* default 1024: 24+bytes hit => ok */ int ring; /* default 3 */ } nr_gpu_pdcch_cfg_t;
nr_gpu_pipe_t *nr_gpu_pdcch_pipe_init(const nr_gpu_pdcch_cfg_t *cfg);              /* pre-fence allocation of EVERY buffer */
int nr_gpu_pdcch_set_classes(nr_gpu_pipe_t *p, int cell, const nr_gpu_pdcch_class_t *c, int n); /* H2D on change only */
/* Stage A: soft bits supplied by the host: e_rx[i] = L*108 raw int16 (NOT unscrambled) for item i. */
int nr_gpu_pdcch_submit_cand(nr_gpu_pipe_t *p, const nr_gpu_slot_ref_t *slots, int n_cells, const nr_gpu_pdcch_item_t *items,
                             const int16_t *const *e_rx, int n_items, nr_gpu_fence_t *out);
int nr_gpu_pdcch_collect(nr_gpu_fence_t f, nr_gpu_dci_hit_t *hits, int max_hits, int *n_hits, int *overflow);
/* Spec-name entry (no host LLR): added in Task 9 as nr_gpu_pdcch_submit(); same collect. */
void nr_gpu_pdcch_counters(const nr_gpu_pipe_t *p, uint64_t *submitted, uint64_t *collected, uint64_t *overflow, uint64_t err[8]);
```
The loader returns the table `{init, set_classes, submit_cand, collect, counters}` via `nr_gpu_pdcch_scan_api()` exported from the `.so`, exactly like `nr_gpu_pdcch_fep_api`.

- [ ] **Step 1: Write the failing tests** `tests/nr_pdcch_scan_gpu_test.cc` (dlopen the module like `nr_pdcch_gpu_fep_test.cc`). Shared helper `make_dci(len, rnti, nid, L, seed)` = `polar_encoder_fast` + Gold scrambling (use the test's own bit-serial Gold from `nr_pdcch_gpu_fep_test.cc:gold_bits`, independent of the module) -> int16 raw soft bits of magnitude 127 (hard LLR) + optional Gaussian noise from a fixed-seed LCG. Tests:
  - `test_payload_bits_preserved_47_and_140`: DCI 1_1-like 47-bit payload whose bits 16-17 (MSB-first positions, `nr_dci_bits_field(&pl, 47, 16, 2)`) equal `01` (T8 slot-3 case) and a 140-bit payload with a random pattern in word 2; submit via `submit_cand`; the hit's `payload` equals the input bit-for-bit (all 3 words) and `nr_dci_bits_field(&hit.payload, 47, 16, 2) == 1`. (Review Focus 1.)
  - `test_hit_set_equals_oracle_synthetic`: 3 cells x 20 occasions x (1 DCI + 4 noise-only candidates), all lengths 12..64 in RANGE mode, compare the hit key set with `nr_pdcch_scan_oracle_occ` (linked into this test like the parity tool). Different `nid`/`scr_rnti` per cell; every hit carries the right `cell`. (Multi-cell.)
  - `test_noise_only_false_alarms_not_higher`: 2000 noise-only candidates (L 1,2,4,8), `rnti_min=1, rnti_max=0xFFEF`; `gpu_hits <= oracle_hits` and in fact equal keys. (Review Focus 2.)
  - `test_hit_overflow_forces_cpu_fallback`: `max_hits=4`, an input engineered to produce > 4 hits (replicate one valid DCI over 10 candidates); `collect` reports `overflow==1`, `n_hits==4`, and the counter `overflow` increments. (Review Focus 3.)
  - `test_collect_slot_tag_matches_submit`: submit slots 100, 101, 102, 103 without collecting 100 (ring=3) -> the 4th returns `NR_GPU_PDCCH_ERR_BUSY` (reason counter incremented); collect 100 returns `hits[i].abs_slot==100` only. (Review Focus 4.)
  - `test_error_reasons`: n_items = 0 / > max_items -> `ERR_RANGE`; unregistered `(len,AL)` -> `ERR_NO_PARAM`; each increments its counter slot.
  - `test_discrete_equals_direct`: run `test_hit_set_equals_oracle_synthetic` body twice, once with `ISAC_GPU_FORCE_DISCRETE=1` set via `setenv` BEFORE `dlopen`, same keys.
- [ ] **Step 2: Run -> fail** (module missing). Add the CMake module/test; **[ORCHESTRATOR: re-run cmake]**.
- [ ] **Step 3: Implement the module.** Kernels (one stream, greatest priority as in `nr_pdcch_gpu_fep.cu:205-215`, `NrGpuRtScope` as at :283):
  1. `k_unscramble`: per item, `c_init = ((scr_rnti<<16)+dmrs_id) % 2^31`, Gold words generated with the jump images of `nr_pdsch_gpu_fep.cu` (GPU4: 31-bit recurrence jump) — one unscramble per `(cand, class)` not per length (F1). Output vec rows in a device scratch `[max_cand][16*108]`. Bit-exact with `nr_pdcch_unscrambling`: `z = (bit ? -x : x)` with the SAME int16 negation (note `-(-32768)` wraps in the CPU code `z2[i] = -ptr[i]` as int16 — replicate, do not saturate).
  2. Build the length list on device from the class (RANGE/LIST), skip `len + 24 > L*108` (same test as `blind_polar_decode`, `:4182`), produce `dvidx/dpid` arrays (the pids come from a host table `(len, AL) -> pid` registered at `set_classes` time via `npc_register`; unregistered -> `ERR_NO_PARAM`, counted).
  3. `npc_decode_dev` (Task 4).
  4. `k_prefilter_compact`: `(crc>>16)==0 && (crc in [rnti_min,rnti_max] || fixed_ok&&(crc==0xFFFF||crc==0xFFFE)) && FI` -> `atomicAdd` slot in the hit buffer; `n_hits_dev` written; hits beyond `max_hits` increment `overflow_dev`.
  Memory: all buffers via `nr_gpu_buf_alloc` at `pipe_init`; `submit` does `nr_gpu_buf_h2d_async` for the item table and the supplied e_rx (staged contiguous), `collect` waits with `cudaStreamQuery` + `nr_gpu_poll_relax_us`, then `nr_gpu_buf_d2h_async` of the hit list only (<= `max_hits*sizeof(hit)`, a few KB; the spec's 8 KB bound is met with the default 1024 hits only if sizeof(hit) <= 8 bytes — it is not: set the DISCRETE D2H to `n_hits` entries after a first 4-byte count copy; DIRECT reads in place).
  Ring of 3 in-flight submissions with `seq` and `slot_tag` per ring entry (Review Focus 4).
- [ ] **Step 4: Build/run**
```
cd /home/nicola/NICOLA/wt/rr-csd-int16/cmake_targets/ran_build/build_t26
export CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps
/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh nice -n 19 ninja -j6 pdcch_scan_gpu nr_pdcch_scan_gpu_test nr_pdcch_scan_parity < /dev/null
/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./nr_pdcch_scan_gpu_test < /dev/null
ISAC_GPU_FORCE_DISCRETE=1 /home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./nr_pdcch_scan_gpu_test < /dev/null
/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./nr_pdcch_scan_parity /tmp/nicola_scan_b.bin --arm dev < /dev/null
/home/nicola/NICOLA/wt/rr-orchestration/heavy.sh ./nr_pdcch_scan_parity /tmp/nicola_scan_tim.bin --arm dev < /dev/null
bash /home/nicola/NICOLA/wt/rr-csd-int16/openair1/PHY/NR_UE_TRANSPORT/tests/check_no_fp64_sass.sh libpdcch_scan_gpu.so < /dev/null
```
Expected: all tests PASS; both recorded dumps `missing=0 extra=0`; no FP64 SASS (this stage has none; the check is repeated in Task 12/13 where a scalar FP64 compare is added — update the script's allow-list there, with the reason, never silently).
- [ ] **Step 5: Commit** (`git add` the new/changed files by path; CMake included).

### Task 6: Integrate stage A behind `ISAC_PDCCH_GPU_SCAN=1` (CPU soft bits in, GPU decode out) with shadow mode

**Files:**
- Create: `nr_pdcch_scan_gpu_client.h`, `nr_pdcch_scan_gpu_client.c`
- Modify: `nr_pdcch_blind_monitor_rt.c` (cache hookup beside `lane_batch_get` and the `g_sweep_cache` consumers at :3798, :4075, :4110; tracked path at :4290-4340), root `CMakeLists.txt` (client .c) **[ORCHESTRATOR: re-run cmake]**

**Interfaces:**
- Consumes: Task 5 API.
- Produces: env `ISAC_PDCCH_GPU_SCAN` = `0` (default, CPU), `1` (GPU decode, CPU fallback per item), `2` (SHADOW: CPU result is used, GPU result computed and compared; mismatches counted and logged `SENSING: PDCCH_GPU_SHADOW mismatch ...`). Client API:
```c
/* per-occasion batch builder used by the sweep and tracked paths */
void nr_pdcch_gpu_client_begin(int cell, uint32_t abs_slot);
int  nr_pdcch_gpu_client_add(uint64_t geom, uint8_t L, uint16_t cce, const int16_t *e_rx, const nr_gpu_pdcch_class_t *cls /*interned*/);
int  nr_pdcch_gpu_client_flush(void);   /* submit+collect; returns n_hits or <0 (caller falls back to CPU for the occasion) */
bool nr_pdcch_gpu_client_get(uint64_t geom, uint8_t L, uint16_t cce, uint16_t len, uint32_t scr_rnti, uint32_t *crc, nr_dci_bits_t *payload);
```
`get` returns `true` + `(crc,payload)` ONLY for a prefilter hit; for an item the GPU judged non-hit it returns `false` and the caller must NOT re-decode on the CPU when `flush` succeeded for that item (otherwise no CPU saved); the "decided no-hit" state is tracked with a per-occasion bitmap (`nr_pdcch_gpu_client_decided(...)`). Any `flush` error => no item is marked decided => unchanged CPU path (the same safety contract the existing sweep cache gives: "this can only be faster, never wrong").

- [ ] **Step 1: Failing test** (CPU-only, in `nr_pdcch_scan_oracle_test.cc`): fake the module with a table of function pointers returning a canned hit list; assert (a) `decided`/`get` semantics above incl. a flush error leaves everything undecided, (b) shadow mode counts a deliberately corrupted canned hit as one mismatch and still returns the CPU result.
- [ ] **Step 2: Implement the client and the hookup.** At each CPU decode site: if mode 1 and the item is decided -> use the cached `(crc, payload)` and skip `polar_decoder_int16`; if mode 2 -> run CPU, then compare. The class admission code (`dci10_default_class_mask`, SI/P/RA, `nr_pdcch_blind_extract_11`, TDRA etc.) is NOT touched: it keeps consuming `(crc, payload)`.
- [ ] **Step 3: Replay A/B (stage A hit-set gate).** One at a time, per cap:
```
ENVFILE=/home/nicola/NICOLA/wt/rr-orchestration/ota/t26aXE.env CAPSET=b TMO=90 tests/passive_rx/p99_replay.sh gpuscan2_b ISAC_PDCCH_GPU_SCAN=2 < /dev/null
ENVFILE=... CAPSET=tim ... p99_replay.sh gpuscan2_tim ISAC_PDCCH_GPU_SCAN=2 < /dev/null
ENVFILE=... CAPSET=b ... p99_replay.sh gpuscan1_b ISAC_PDCCH_GPU_SCAN=1 < /dev/null     (and tim)
```
Expected: `PDCCH_GPU_SHADOW mismatches=0` over >= 100k compared items per cap; mode 1: DL grants count, `crc_q` (`goal_metrics.py`) and RNTI set equal to the mode-0 replay of the same cap within replay-to-replay spread (take 2 mode-0 replays first to measure that spread; it is [MEASURED] not assumed). Report scan run p50/p99 for 0 vs 1 (this is the CPU saved; a2e is still bounded by CPU LLR production).
- [ ] **Step 4: Forced-discrete functional run:** `ISAC_GPU_FORCE_DISCRETE=1` with `ISAC_PDCCH_GPU_SCAN=2` on CAPSET=b: shadow mismatches 0.
- [ ] **Step 5: Commit.** Stage A is shippable here, default off.

---

# Part III: LLR, demap, FEP on the GPU (stage B)

### Task 7: Bit-exact GPU LLR: stage taps on the CPU, then chest / gate / MRC / autoscale port

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/dci_nr.c` (an optional tap struct, no behaviour change), `nr_pdcch_gpu_fep.cu` or a new `nr_pdcch_llr_gpu.cu` section of the new module, `tests/nr_pdcch_slot_bench.cc` (reuse as the reference reader), new test `tests/nr_pdcch_llr_exact_test.cc`

**Interfaces:**
- Consumes: the existing dumps of F4 (`ISAC_PDCCH_GPU_DUMP`, IQ + CPU LLR), `nr_rx_pdcch_symbol` (`dci_nr.c:420-1060`).
- Produces (Task 8, 9): device function set `k_pdcch_chest`, `k_pdcch_gate_mrc`, `k_pdcch_llr_scale` producing the CPU's `llr[]` (c16 pairs per RE, symbol-major, DM-RS REs excluded) bit-exactly.

- [ ] **Step 1: Baseline, no code.** Capture dumps `ISAC_PDCCH_GPU_DUMP=/tmp/nicola_gpu_dump_{b,tim}.bin ISAC_PDCCH_GPU_DUMP_MAX=512` (non-timing replays), run `nr_pdcch_slot_bench dump.bin --gpu` and record, per cap, the exact-equal LLR fraction (int16 equality, NOT sign agreement). Expected from F3: far below 100 %. This number is the starting point; label `[MEASURED, DGX replay <cap>, existing libpdcch_gpu.so]`.
- [ ] **Step 2: Failing test** `nr_pdcch_llr_exact_test.cc`: for every record of the dump require `memcmp(gpu_llr, cpu_llr) == 0`; print the first differing RE and which antenna/branch-gate decision applies there. Fails now.
- [ ] **Step 3: CPU stage tap.** Add `typedef struct { c16_t *chest; int16_t *gate_mask_and_shift; c16_t *post_mrc; } nr_pdcch_stage_tap_t; extern __thread nr_pdcch_stage_tap_t *t_pdcch_tap;` in `dci_nr.c`, filled at the end of (a) the pilot LS + delay/FIR smoothing block (:480-657), (b) the BRANCH QUALITY GATE + MRC block (:743-806), (c) the autoscale/clip stage (:879-968). Null by default; the tap writes only when set. The dump hook sets it for dumped slots only (extend `nr_pdcch_gpu_record_cpu` record version to 2 with the three intermediates). This makes the port bisectable: chest, then gate+MRC, then LLR.
- [ ] **Step 4: Port stage by stage, each with an exact-equality assertion against its tap**: (i) fixed-point pilot products, delay de-rotation, FIR smoothing; (ii) per-branch roughness gate (`4x the smoothest` rule, comment at :743) and MRC (`nr_pdcch_detection_mrc`, :394, with `ISAC_RX_MRC_MODE=1` and `ISAC_SCAN_ANT` honoured: both come from the item/cell table, never hard-coded); (iii) `ISAC_PDCCH_*` autoscale (symbol P90), clipping, fixed-point compensation (read the actual env flags the scan sets in `t26aXE.env`: `ISAC_PDCCH_GATE_CFAR=1` etc.; each env-dependent behaviour used in the replay env must be reproduced or the test must state which it excludes). The device code uses integer arithmetic only where the CPU does; `--fmad=false` for the module as `pdcch_nid_gpu` does (CMake :`target_compile_options(pdcch_nid_gpu ... --fmad=false)`).
- [ ] **Step 5: Gate G-LLR (decision point, orchestrator reads it).** Run the exact test on both dumps. PASS = 100 % int16-equal LLR on >= 99.9 % of records and every unequal record explained (e.g. a branch-FO slot the exact FEP already refuses: `exact_run` falls back when `cont_fo_comp`/branch FO is set, `nr_pdsch_gpu_exact.c:~200`). If after the three ports the equality stays < 100 %, STOP and report: the identical-hit-set contract for the full-GPU chain cannot be met by this LLR; stage A (Task 6) remains the deliverable and the exact residual (which stage, magnitude) goes in the report. Do not loosen the test to a sign-agreement threshold.
- [ ] **Step 6: Commit per port stage** (three commits, explicit paths).

### Task 8: GPU CCE gather: demap + deinterleave + candidate rows

**Files:** the new module (`k_cce_gather`), `tests/nr_pdcch_scan_gpu_test.cc` (new test), reference `nr_pdcch_demapping_deinterleaving` (`dci_nr.c:63`)

**Interfaces:**
- Consumes: Task 7 LLR (device, per RE), CORESET geometry from a per-cell/per-coreset table `nr_gpu_pdcch_coreset_t {n_rb, duration, reg_bundle_size, interleaver_R, n_shift, rb_offset, cset_start, bwp_start, dmrs_ref, start_symbol, dmrs_scrambling_id, type}` uploaded by `set_coresets(cell, coreset_id, ...)` on change only.
- Produces: `e_rx` for each item `(coreset, L, cce0)` in device scratch, laid out as the CPU's `pdcch_e_rx` (`L*108` int16 pairs per candidate), feeding `k_unscramble` (Task 5) directly.

- [ ] **Step 1: Failing test** `test_cce_gather_equals_cpu`: synthesize random int16 LLR `[n_rb*duration*9]` for >= 12 geometry cases covering interleaved (`R in {2,3,6}`, bundle 2/3/6, with `n_shift`), non-interleaved, duration 1/2/3, `n_rb` 6..96 (list them as a table in the test; geometry values are test inputs, not tied to any cell); for every `(L, cce0)` in the search space compare device e_rx to the CPU `nr_pdcch_demapping_deinterleaving` output (linked test, root CMake target like the oracle test). Fails (kernel absent).
- [ ] **Step 2: Implement `k_cce_gather`** as a direct index-map port: compute `f_bundle_j` exactly as the CPU loop (`c, r` interleaver walk with `n_shift`), then CCE->REG-bundle->RE address; one thread per output element.
- [ ] **Step 3: Run test (direct and `ISAC_GPU_FORCE_DISCRETE=1`), PASS. Commit.**

### Task 9: Full chain `nr_gpu_pdcch_submit` from IQ, a2e measurement, replay latency gate

**Files:** the new module (`nr_gpu_pdcch_submit` + `nr_gpu_pdcch_set_coresets`), `nr_pdcch_scan_gpu_client.c`, `nr_pdcch_blind_monitor_rt.c` (producer-side submit call; consumer-side collect), `tests/nr_pdcch_scan_parity.cc` (`--arm chain` from dumped IQ+items)

**Interfaces:**
- Consumes: Tasks 5, 7, 8; FEP output: use the already-GPU `nr_pdcch_gpu_exact_fep_symbol` result (`nr_pdsch_gpu_exact.c:175`, `ISAC_PDCCH_GPU_EXACT_FEP=1`, already exact vs CPU FEP, self-checked) as the rxdataF the chain reads (device pointer exposed by a new getter `nr_pdsch_gpu_exact_fep_dev_ptr(slot, symbol)`; on DISCRETE the module copies once). The spec's early start (FEP per symbol group while the ring fills) is NOT in this plan: it needs the per-symbol ring write pointer, which the spec itself lists as "TO VERIFY, not read" (spec 9). Task 9 Step 5 reads `usrp_lib.cpp:1131/1510` and the raw_iq device and reports whether it is available; implementing it is a follow-up.
- Produces: entry
```c
int nr_gpu_pdcch_submit(nr_gpu_pipe_t *p, const nr_gpu_slot_ref_t *slots, int n_cells, const nr_gpu_pdcch_item_t *items, int n_items, nr_gpu_fence_t *out);
```
exactly the spec-5 signature; `items` carry `(cell, coreset, ss, L, cce0, class idx, dmrs_id)`, LLR is produced on device (flag `HAVE_LLR` clear).

- [ ] **Step 1: Failing parity arm.** Extend dumps with record version 2 of F4 (IQ) + Task 1 records for the same occasion (join key `abs_slot, geom`); `--arm chain` replays IQ -> chain -> hits and compares with the oracle on the SAME occasion built from the CPU e_rx. Expected before the chain exists: arm absent -> fail.
- [ ] **Step 2: Implement `nr_gpu_pdcch_submit`** = FEP-read + `k_pdcch_chest/gate_mrc/llr_scale` + `k_cce_gather` + the Task 5 kernels, one stream, ONE launch chain per slot for all cells (cell index into per-cell tables: frame params, ring pointer, DM-RS ids, class sets).
- [ ] **Step 3: Parity.** `nr_pdcch_scan_parity <dump> --arm chain` on both caps and with `ISAC_GPU_FORCE_DISCRETE=1`: `missing=0 extra=0` on tracked + sweep items. Multi-cell: `--cells 2` duplicates each record as cell 0 and cell 1 (cell 1 with `dmrs_id ^ 1` and the oracle recomputed for it) and checks cell tagging.
- [ ] **Step 4: Production integration.** `ISAC_PDCCH_GPU_SCAN=1` + `ISAC_PDCCH_GPU_EXACT_FEP=1`: the scan consumer no longer builds `pdcch_e_rx` / LLR for the GPU-covered occasions; it calls `collect` and feeds `(crc, payload)` to the same admission code as Task 6. A failed or overflowed collect falls back to the full CPU occasion. Occasions with branch FO / `cont_fo_comp` (exact FEP refuses) fall back, counted by reason.
- [ ] **Step 5: Latency gate (REPLAY, not dump, nothing else running).** For each cap, n = 3 alternating CPU/GPU replays:
```
... p99_replay.sh a_cpu_b  < /dev/null
... p99_replay.sh a_gpu_b ISAC_PDCCH_GPU_SCAN=1 ISAC_PDCCH_GPU_EXACT_FEP=1 < /dev/null
python3 tests/passive_rx/pdcch_scan_stats.py <replay dir>/run.log
```
Acceptance (contract): `a2e` p50 <= 100 us and p99 <= 300 us, scan wait/run reported, `crc_q` and drops (full+stale+narrow < 1 %) not worse. If a2e misses, the stats script's stage split (chain submit-to-collect vs CPU accept) says which side; report [MEASURED], do not tune a constant.
- [ ] **Step 6: Read-only check for early start** (see Interfaces) and write the finding into the report. Commit.

---

# Part IV: lock split and gate

### Task 10: Short Phase-2 lock (B5)

**Files:** `nr_pdcch_blind_phase2.c/.h`, `nr_pdcch_blind_monitor_rt.c` (:4953-5035, :5179-5260), new `tests/nr_pdcch_phase2_hold_test.cc`, `tests/nr_pdcch_scan_oracle_test.cc` (lock-state test)

**Interfaces:**
- Consumes: Task 9 hit list; existing hypothesis-lock logic and thresholds N (do NOT retune; spec 2.4).
- Produces: (a) `phase2_hold_ns` histogram counters (p50/p99/max) around `nr_pdcch_blind_phase2_lock()/unlock()` pairs, logged with the existing scan summaries; (b) under `ISAC_PDCCH_GPU_SCAN=1` the GPU hypothesis grid runs OUTSIDE the lock and the lock is held only for accept/dedupe/persistence/enqueue; (c) the hypothesis-lock decision isolated as a pure function:
```c
/* One slot of hypothesis tracking. Pure: no globals, no I/O. Same thresholds as today's code. */
typedef struct { uint8_t consecutive; uint8_t locked; } nr_pdcch_hyp_state_t;
int nr_pdcch_hyp_step(nr_pdcch_hyp_state_t *s, int hit_with_crc_rnti_match, int n_required, int *newly_locked);
```
  and a reduced phase-2 grid (one hypothesis per tracked UE per CORESET) submitted through the SAME `nr_gpu_pdcch_submit` with a small `n_items`.

- [ ] **Step 1: Measure first.** Add the hold-time counters (behaviour-neutral) and run one CPU-mode replay per cap; report `p2hold` p50/p99 and `BTIM_P2WAIT` p99 [MEASURED]. This decides how much the split can gain; if the CPU-mode hold p99 is already < 50 us (the spec's CPU budget) report that and do only (c).
- [ ] **Step 2: Failing test (equivalence of the lock decision).** Locate today's lock logic (`grep -n "VERIFY_OCC\|consecutive\|nr_pdcch_blind_rnti_persistence_check" nr_pdcch_blind_*.c`), extract `nr_pdcch_hyp_step` with the existing `N`, and write a test that feeds it a recorded per-slot hit sequence (from the Task 1 dumps: `crc==rnti` per slot for one tracked RNTI) and asserts the lock slot equals the slot at which the legacy code locks (obtained by running the legacy code path on the same sequence in the test: keep the legacy function callable as `nr_pdcch_hyp_step_legacy` until the test passes, then delete it in the same commit). Fails first.
- [ ] **Step 3: Narrow the lock.** Move the sweep/tracked GPU submit+collect before `nr_pdcch_blind_phase2_lock()` in `run_discovered_occasion` and the generic occasion tail; assert with the existing `Phase2Concurrent*` gtests (2 tests in `test_nr_pdcch_blind_monitor`; run the whole binary) and a new test that two threads each holding a fake collect do not serialise (hold time of the fake < 1 ms with 2 threads in parallel).
- [ ] **Step 4: Reduced phase-2 grid.** After lock, submit only items of locked hypotheses (`n_items` small); test: grid size after lock == number of locked hypotheses (synthetic), the device histogram (`hits per hypothesis`, phase 1) increments identically to a CPU count over the same trace.
- [ ] **Step 5: Replay both caps** `ISAC_PDCCH_GPU_SCAN=1`: `p2hold` p99 down vs Step 1, `Phase2Concurrent*` pass, TSAN-clean is NOT claimed (aarch64 residuals, K30). Commit.

### Task 11: P-PDCCH gate (replay both caps, discrete, multi-cell, report)

**Files:** none new except `docs/superpowers/reports/csd/PPDCCH-GATE-<date>.md` (orchestrator saves the agent's text).

- [ ] **Step 1: Re-run everything from a clean build** (`ninja -j6 nr-uesoftmodem pdcch_scan_gpu nr_pdcch_scan_gpu_test nr_pdcch_scan_parity test_nr_pdcch_blind_monitor test_nr_pdcch_scan_record test_nr_pdcch_scan_oracle nr_polar_sc_cuda_test`, then `ctest -R "pdcch|polar" -j2`) and record pass counts. Known ARM failures list is in CLAUDE.md; none is PDCCH.
- [ ] **Step 2: Contract table in the report**, each row with its label:
  - DCI hit set (cell, rnti, cce, L, payload) identical to the CPU scan on recorded slots, CAPSET=b and CAPSET=tim: parity tool (`--arm dev` and `--arm chain`) output and shadow-mode `mismatches` count, false-alarm count (`extra` and the noise-only test) not higher. `[MEASURED, DGX replay <cap>, ...]`.
  - replay a2e p50 <= 100 us, p99 <= 300 us (Task 9 Step 5, n=3 per cap, interleaved with the CPU arm).
  - forced-discrete functional run: parity + shadow mismatches 0 with `ISAC_GPU_FORCE_DISCRETE=1`.
  - multi-cell: synthetic 2-cell test (Task 5/9) pass; one CUDA context (`nr_gpu_pdcch_counters`, `cuCtxGetCurrent` count in the test equals 1 after loading the module alongside `libpolar_gpu.so`).
  - Phase-2 hold p99 before/after.
  - DCI 1_1 TDRA field: the T8 payload test (Review Focus 1) pass; and on the `tim` replay the count of slot-class-3 DCIs whose bits 16-17 equal `01` seen by the CPU resolver is the same with GPU scan on and off (use the existing RMDCI diagnostic, `ISAC_PDSCH_RM_GENIE=1`, off-path otherwise).
- [ ] **Step 3: sens6 note.** sens6 build + ctest + replay is the phase gate (spec 8 gate 3), run by the orchestrator; this plan only guarantees the DISCRETE code path functions on the DGX.
- [ ] **Step 4: Report text returned to the orchestrator; stop.** Positive result -> Haiku updates PROJECT_MEMORY (orchestrator).

---

# Part V: P-JANON (Task 13 of the phase plan)

### Task 12: Janon item class on the chain: polar mask-recovery path

**Files:** the new module (`k_janon_*`), `nr_pdcch_scan_gpu.h` (flag already defined), `tests/nr_pdcch_scan_gpu_test.cc`, `tests/nr_pdcch_scan_parity.cc` (`--kind janon`), `tests/check_no_fp64_sass.sh` (allow-list entry for the single scalar prescreen compare, with the reason as a comment)

**Interfaces:**
- Consumes: Task 5 module, Task 2 janon oracle (`nr_pdcch_scan_oracle_janon`), `nr_pdcch_joint_prescreen` (`nr_pdcch_joint_solve.c:364`) as the reference.
- Produces: items with `flags & NR_GPU_PDCCH_F_JANON`: class `scr_rnti=0`, `rnti_min/max` of the CORESET's allowed C-RNTI range, `fi_req=DL`, RANGE `dci_len_min()..min(dci_len_max(),64)` or the LEN_HINT list; hit `kind=1`, `rnti` = recovered CRC value (mask recovery), `via_joint=0`. Device prescreen `k_joint_prescreen(e, E) -> bool` bit-for-bit equal to `nr_pdcch_joint_prescreen`: sums `a=Σ|x|`, `b=Σx²` in int64 (exact; E<=1728 and |x|<=32768 so no overflow), final test `r >= 2/π + 3.09*0.339/sqrt(E)` evaluated in double with the per-E threshold precomputed on the host (single scalar FP64 compare per candidate; document in the allow-list; the per-item cost is negligible).

- [ ] **Step 1: Failing tests** in `nr_pdcch_scan_gpu_test.cc`:
  - `test_janon_hit_set_equals_oracle_and_extra_hits_reencode`: the synthetic janon record of Task 2 (47-bit DL DCI, rnti 0x4601, n_RNTI=0 scrambling, AL 4) + 3 noise candidates + one candidate whose DCI is 1 length-hint off; hit set (cell, L, cce, len, rnti, payload) equals the oracle; any hit not in the oracle is re-encoded with `polar_encoder_fast` and must reproduce the input bits within `mismatched_bits` (else FAIL).
  - `test_prescreen_equals_cpu`: 5000 random int16 vectors for E in {108,216,432,864,1728} incl. all-zero and constant vectors (`b==0` -> false): device bool == `nr_pdcch_joint_prescreen`.
  - `test_janon_no_drop_burst`: submit 64 janon submissions of 8 candidates each without waiting (ring wraps with collect interleaved by a helper thread): `err[BUSY]` increments are retried by the producer; final `submitted == collected`, 0 lost.
  - `test_janon_does_not_use_class_rnti`: the same candidate in an OCC class with `scr_rnti=0x4601` and in a janon class with `scr_rnti=0` produce different keys (no scrambling-RNTI leakage between classes in one launch).
- [ ] **Step 2: Run -> fail; implement `k_janon_*`** as additional items of the Task 5 launch (same unscramble/decode/finish/prefilter kernels; the janon-specific piece is the class table row and the prescreen). Rerun with `ISAC_GPU_FORCE_DISCRETE=1`.
- [ ] **Step 3: Parity on recorded janon items:** `nr_pdcch_scan_parity /tmp/nicola_scan_b.bin --kind janon --arm dev` and tim: report `oracle_hits`, `via_joint` share (from Task 3), `missing`, `extra`. Expected at this task: `missing` equals exactly the `via_joint=1` hits of the oracle (the joint solve lands in Task 13) and `extra=0`.
- [ ] **Step 4: Commit.**

### Task 13: GPU joint solve (OSD order 0, GF(2)) bit-exact

**Files:** the new module (`k_joint_solve`), `tests/nr_pdcch_scan_gpu_test.cc`, reference `nr_pdcch_joint_solve.c:188-362`, model builder `nr_pdcch_joint_live.c:model_get`

**Interfaces:**
- Consumes: the host-built model matrices M, baseline b (per `(A, L, nid, pre)`; built once on the host via `nr_pdcch_joint_model_new_ex` with the real encoder callback, F5) — expose a read-only accessor `nr_pdcch_joint_model_export(const model*, uint64_t *rows, int *K, int *E, uint8_t *b)` added to `nr_pdcch_joint_solve.c`; uploaded to a device model cache by `(A, L, nid)` at `set_classes`/first use, pre-fence capacity `JOINT_CACHE_N` (read its value from `nr_pdcch_joint_live.c`); a model that failed to build (NULL on the host) is never retried and its items report `NO_PARAM`.
- Produces: `via_joint=1`, `mismatched_bits` set, accepted iff `mismatched_bits <= nr_pdcch_joint_anon_mm_max(L)` and the RNTI/FI admission of `nr_pdcch_joint_live_decode` (:116-121); `dci_length > 64` skipped exactly as the CPU does (`NR_PDCCH_JOINT_MAX_A`).

- [ ] **Step 1: Failing test** `test_joint_solve_equals_cpu`: for >= 400 vectors over `(A in {27,39,47,55,64}, L in {1,2,4,8})` generated as encoded DCI + scrambling with n_RNTI=0 + Gaussian noise at three SNRs (passes / marginal / fails; fixed seeds), compare per vector `(accepted, rnti, payload, mismatched_bits)` of the CPU `nr_pdcch_joint_live_decode_11` with the device result. Must also cover ties: construct vectors with equal `|llr|` runs (repeated values) so the reliability ordering tie-break is exercised.
- [ ] **Step 2: Determine the reference ordering before writing the kernel.** `cmp_wi` (:179) uses `qsort` on `wi_t{w,i}` descending by `w` only; glibc `qsort` is merge-sort based (stable in practice but unspecified). Write a 10-line CPU test that checks whether equal-weight elements keep ascending index order for the sizes used (E=108..1728); if yes the device uses a stable key `(w desc, i asc)`; if not, report it and make the CPU side deterministic first (change `cmp_wi` to tie-break on `i`) in the SAME commit with the CPU test updated, since an unstable reference cannot be matched bit-for-bit.
- [ ] **Step 3: Implement `k_joint_solve`**: one thread block per (candidate, length): bit-packed 128-bit row vectors (`v128`, :14-22), partial pivoting over the K most reliable linearly independent observations, back-substitution, mismatch count over all E positions. All integer / bit ops (no FP) except the reliability weight which is the int16 `|llr|` (integers).
- [ ] **Step 4: Run test; PASS required with zero differing vectors.** Then the Task 12 recorded-parity command again: `missing=0 extra=0` for janon items on both caps (hit set identical, including `via_joint` hits). If the device cannot be made bit-exact within one agent session, STOP and report exactly which vectors differ and why; the fallback (Task 14 Step 5) keeps one CPU worker for the joint class only, and the contract "pdcchJanon thread removable" is then reported NOT MET with the measured share of janon hits that need it.
- [ ] **Step 5: Commit.**

### Task 14: Switch janon to the GPU chain, remove the thread, gate

**Files:** `nr_pdcch_blind_monitor_rt.c` (`janon_submit` :2952, `janon_start` :3065, `janon_worker`; hit consumer `janon_feed` :3086-3123, :6835), `nr_pdcch_scan_gpu_client.c`

**Interfaces:**
- Consumes: Tasks 6, 9, 12, 13.
- Produces: env `ISAC_PDCCH_GPU_JANON=1` (requires `ISAC_PDCCH_GPU_SCAN=1`): `janon_submit()` appends its candidates (all prescreen-passing ones, not only top-K, not rate-sampled: the limits existed only to protect the CPU worker) to the SAME slot submission as janon items; collected janon hits are written into `g_janon_hits` through the unchanged `janon_hit_t` (so the next occasion's `janon_feed` is untouched); `janon_start()` does not create the thread when the flag is on and the module is healthy. Health = last N submissions free of `ERR_CUDA`; on a persistent GPU error the flag degrades to the CPU worker (thread started lazily, counted once) so a GPU fault never silently disables the anonymous search.

- [ ] **Step 1: Failing test** (CPU, fake module as in Task 6): with the flag on and a fake healthy module, `janon_submit` produces no `pdcchJanon` thread (assert via a test hook `nr_pdcch_janon_thread_started()`), a canned hit reaches `janon_feed` exactly once with the same `(geom, len, rnti, hash, L, cce)`; with a fake `ERR_CUDA` x N the worker thread starts and the counter `janon_gpu_degraded` is 1.
- [ ] **Step 2: Implement.** Keep `g_janon_sub/drop/done/found` counters meaningful: `sub` = GPU janon items submitted, `drop` = items the ring refused (must be 0), `done` = collected, `found` = hits.
- [ ] **Step 3: Replay gate, both caps, n=3, alternating CPU janon / GPU janon** (`ISAC_PDCCH_GPU_SCAN=1 ISAC_PDCCH_GPU_JANON=1`): 
  - hit set: record with Task 1 (`kind janon`) in a separate non-timing replay per arm and compare `nr_pdcch_scan_parity --kind janon`; AND compare the live outcome: set of `(rnti, len)` accepted by the anonymous length-sweep evidence (`JOINT_ANON_HIT`, `ANONLEN`, lock logs) GPU arm superset of CPU arm, every extra one re-encode-checked (Review Focus 5).
  - `JOINT_ANON_ASYNC items ... dropped=` is 0 in the GPU arm (CPU arm baseline V ~52 % per F5, label it as the earlier OTA number, not replay).
  - `pdcchJanon` absent from `ps -L` / the thread map during the GPU arm (`top -H -b -n1 -p <pid>` once at 30 s; thread names are set by `pthread_setname_np`); freed core reported.
  - forced-discrete functional run.
- [ ] **Step 4: Report** (orchestrator saves). 
- [ ] **Step 5: If Task 13 was reported not met:** ship `ISAC_PDCCH_GPU_JANON=1` with the CPU worker restricted to the joint class: GPU polar-janon handles `via_joint=0`; the CPU worker only receives candidates the GPU marked "prescreen pass, polar fail". Report the measured CPU load of that residual worker and mark the "thread removable" contract NOT MET.

---

## Self-Review (done)
- Spec/contract coverage: Task 11 contract -> Tasks 3, 5, 6, 8-11 (items per CORESET x SS x L x candidate x cell, one submission per slot, reuse of FEP / `npg` kernels / sweep grid, CPU keeps DCI decode/RNTI tracking/USS hash/TDRA/grant build, lock split 2.4, env flag). Task 13 contract -> Tasks 12-14 (second item class in the same submission, CRC replaced by mask recovery, thread removed). Acceptance -> Task 11 Step 2 table + Task 14 Step 3.
- Placeholders: none left as TODO; the two places that depend on code the plan author could not exhaust (the exact seam lines in Task 1 Step 4, the lock logic in Task 10 Step 2) give the grep and the acceptance test.
- Type consistency: `nr_gpu_pdcch_class_t` fields = `nr_pdcch_scan_class_t` minus `cand_mask`; `nr_gpu_dci_hit_t` carries `via_joint` / `mismatched_bits` used in Tasks 12-14; `nr_gpu_pdcch_submit_cand` (Task 5) vs `nr_gpu_pdcch_submit` (Task 9) are distinct names on purpose.
- Review Focus: each of the five has a named test (Tasks 5 and 12).
