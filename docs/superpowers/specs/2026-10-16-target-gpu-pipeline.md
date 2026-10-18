# Target GPU pipeline: 500 us budget, multi-cell-ready GPU API, Phase 1 port order (2026-10-16)

Phase 0 Task 6. Design only: no code, no build, no OTA. Inputs: roadmap `docs/superpowers/plans/2026-10-16-rx-goals-roadmap.md` and the Phase 0 reports in `docs/superpowers/reports/csd/` (GPU-INVENTORY, SCORECARD-BASELINE, DLCRC-ROOTCAUSE, ROOTCAUSE, ALLON-OTA, AB-SPREAD-PIN, PDCCH-PARBANKS-OTA, LDPC-PERCB, MPCI-OTA, ULCAUSE; all `-20261016`). Code cites are `file:line` on csd/int16 and are `[CODE-READ]`. Numbers keep the label of their source report; V = Vodafone 3680.01 MHz, T = TIM 3759.99 MHz, never pooled.

**Task 4 (GPU stall trace) is NOT done.** Everything that depends on it is marked `TO BE FIXED BY TASK 4` with a provisional value and the reason. They are: the maximum length X of a background kernel (4.2), the priority rules (4.3), and the claim that the RT/disc SM split is sufficient (4.1). Phase 1 plan must re-read these three after Task 4.

**2026-10-17 update:** section 10 ("DL GPU data path v2") replaces rows 4-7 of table 1.1 and ranks 8 and 10 of section 7 for the DL per-grant path. Plan: `docs/superpowers/plans/2026-10-17-dl-gpu-datapath.md`.

Rules that shape the design (roadmap): G3 is done together with G1/G2 (every G1/G2 fix that touches a CPU stage is the GPU port of that stage, CPU kept as reference and fallback); X925 cores for latency threads, A725 for helpers; new GPU interfaces are multi-cell ready; sens6 (RTX 4070, discrete, sm_89, weak FP64) works through a runtime switch `nr_gpu_model()`.

---

## 1. Per-slot timeline target (Task 6 Step 1)

Clock: t=0 is "slot fully read" (the `tot` of `goal_metrics.py` starts here and includes PDCCH arrival-to-enqueue `a2e`; see the `e2e=a2e,qw,pre,dec,first,bodies,tot,lag` field in EVMGRANT). Every row is a **p99 allocation**, not a p50. Worst grant class used for the DL table: 273 PRB, 4 layers, 32 CBs (`NR_GPU_LDPC_MAX_JOBS` = 32 per launch, nr_pdsch_gpu_ldpc.h:17), code-rate class R23 (R13 is the stretch case, see the gap column). Today's numbers: `[MEASURED, DGX OTA 2026-10-16, V/T, t26aXE, n=3]` from ALLON/ROOTCAUSE unless stated.

### 1.1 DL: slot read to last grant decoded

| # | stage | where | stream / prio | target start-end (us) | today (V / T) | gap and means |
|---|---|---|---|---|---|---|
| 0 | whole-slot FEP, streamed per symbol group as the ring fills (grant independent: FFT of 4 antennas x 14 symbols) | GPU | RT, highest | ends <= +30 | FEP per grant mean 89/76 us, p99 <= 576/512 us (PDTIM) | removes FEP from the critical path. New: today FEP is triggered per grant (nr_pdsch_gpu_fep.cu:1233-1286). Needs per-symbol write pointer from the ring (`usrpRing`, usrp_lib.cpp:1131/1510): TO VERIFY, not read. |
| 1 | PDCCH: FEP of CORESET symbols (started when they arrive, about -390 us for symbols 0-2), LLR, polar, CRC, all CORESETs x candidates x lengths x cells in one batch | GPU | RT, highest | -250 to +60 | `a2e` p50 0.62-0.88 ms, p99 2.8-4.5 ms; scan wait p99 up to 2.45 ms, run p50 0.4-0.55 / p99 2.0-2.65 ms; bank pass 158-182 us x 724k (V) | the single biggest gap: 0.6 ms floor (ROOTCAUSE 4.4) becomes <= 60 us after t=0. Means: B3 (section 2), early start, no CPU scan queue. |
| 2 | DCI accept, RNTI track, TDRA/CSI-RS table lookup, job build, enqueue to a lane | CPU X925 | - | +60 to +90 | part of `a2e`, not split | keep CPU (control, 30 us). Split needed in the log (`a2e` = scan + accept) for Phase 1 measurement. |
| 3 | dispatch: normal lane and heavy lane (>= 8 CBs or > 150 PRB or Nl >= 3), no slot-group serialisation | CPU X925 | - | +90 to +100 | `qwait` p99 V 6.0-7.1 ms / T 1.3-2.6 ms (top p99 component, 44-63 % of the V p99 set); `pre` p99 1.2-1.8 ms (widest-first slot group, nr_pdsch_passive_queue.c ~1460-1729) | queue policy (G1/G2 item Q). |
| 4 | chest + MMSE demod + EVM + SNR, fused per job | GPU | RT, highest | +100 to +250 | chest mean 157/122 us + demod 189/158 us (cpu 113+125 / 94+112); chest fallback to CPU at > 2048 rows removed by chunking (refused=0, ALLON) | 150 us for 273 PRB x 4L is an **estimate** [INFERRED]; no kernel bench exists. Phase 1 step 0 benches it. Means: fuse chest+demod+EVM in one graph, FP32 where the CPU check allows (nr_gpu_pass_fp32_gate.h). |
| 5 | rate-dematch + deinterleave on GPU, written straight into the LDPC input layout | GPU | RT, highest | +250 to +280 | CPU `prep_segment` 18.9 us/CB R13, 0.32-0.38 ms at CB16+ on air, serial (nr_pdsch_gpu_ldpc_host.c:115, :173) | port (item L). Reuse `cb0_dematch_kernel` (nr_td_cb0_batch.cu) with DL rate-match variants; removes the CPU prep and its H2D. |
| 6 | LDPC decode with in-kernel CRC, non-cluster kernel family for C >= 4 | GPU | RT, highest | +280 to +480 | CB16+ GPU wait p50 385-430 us (NC_MIN_CB=8), first body p50 2.0-2.8 ms; bench 32 CB: R23 nc 158 us / cluster 651 us; R13 nc 441 / cluster 1230 us (t26f env) [MEASURED bench, LDPC-PERCB] | R23 fits (158 < 200). **R13 32 CB does not fit (441 us, gap 240 us)**: means = cluster size 2 or 4 variant (not measured, LDPC-PERCB "Not done"), in-kernel CRC early exit (today the host checks CRC on snapshots, nr_pdsch_gpu_ldpc_host.c:187-195), it_max 8 kept unless the CPU-reference check allows less. Per-CB latency floor of nc is 347 us at C=1 (R13): a launch with C<4 stays on the cluster kernel. |
| 7 | TB bits + CRC + EVM back to the host | DIRECT: mapped, 0 copy; DISCRETE: one D2H | RT | +480 to +500 | CPU CRC/hand-off "small" (not measured) | 20 us reserved. |

Sum of allocations = 500 us with **zero margin**. Margin must come from three levers that move work before t=0: row 0 (done), row 1 early start (done), and chest on the DM-RS symbols of same-slot (k0=0) grants once the DCI is known. The third lever needs the DCI earlier than t=0, which needs the PDCCH batch to finish about 150 us before slot end (Phase 1 step after B3). Not counted in the table. [INFERRED: target, not measured]

Gap summary (sum of today's p99 components vs 500 us): today `tot` p99 = V 11.5 ms, T 8.1 ms (ALLON). The p99 grants are 40-94 % in slots with a >= 8 CB grant (ROOTCAUSE 1). Order of closing: qwait + pre (4-7 ms) by lanes; a2e (0.6-4 ms) by B3; large-body 2-6 ms by nc + dematch GPU + chest chunking (nc and chunking already merged on csd/int16).

### 1.2 UL: slot read to last grant decoded (known-ID UEs; the first decode of a new UE is reported separately)

| # | stage | where | prio | target (us) | today | gap and means |
|---|---|---|---|---|---|---|
| 0 | IQ snapshot of the UL slot (retained per slot, ring of N slots, C2 buffer) | host ring, mapped | - | 0 | snapshot pool 36 (core 0 A725) | keep; becomes the input of the background ID search (item U2). |
| 1 | UL FEP | GPU | RT partition | 0 to +60 | 93 us (V, 168k calls), 89 us (T) | fits. |
| 2 | UL grant -> layout (locked BWP table), DM-RS + chest + equalise + demap, fused | GPU | RT | +60 to +200 | solver/equalise on CPU: UL_E2E cpu p50/p99 V 1005/21737 us, T 1364/20516 us (n=73789/57846) | port (item U-GPU, stage 2 of G2). CPU threads 0,1 are A725 today. |
| 3 | LDPC (nc), HARQ soft combine of the stored LLRs (C3) | GPU | RT | +200 to +400 | 198/193 us per call; `fb err` ~10 % of UL TBs (section 6, item F) | HARQ buffer lives on the device, keyed (cell, rnti, pid). |
| 4 | CRC, result | mapped / D2H | RT | +400 to +450 | small | 50 us margin. |
| bg | 1024 data-ID search over retained IQ, strong-DM-RS UEs only | GPU | lowest, disc partition | off path, X-bounded | CPU ID sweep 1 of 1024 per grant, hit rate attempts/1024 [INFERRED, ULCAUSE] | item U2. |

UL `tot` p50/p99 today: V 6075/47663, T 2882/41295 us (SCORECARD, arm X). UL CRC is ~0 so the UL "gap" is a correctness gap first (ULCAUSE ranked causes), a latency gap second.

### 1.3 Core placement (G3)
`[MEASURED, AB-SPREAD-PIN]`: X925 = cores 5-9, 15-19; A725 = 0-4, 10-14; pinning the PDCCH scan to A725 made `a2e` p50 +0.4-1.1 ms. Target map after Phase 1 (DGX):

| class | cores | threads |
|---|---|---|
| X925 radio | 5, 15, 16, 18, 19 | UEthread (nr-ue.c:2524), uhd_ctrl, usrpRing, dpdk-io x2 (unchanged, KEEP-CPU) |
| X925 latency | 6, 7, 8, 9, 17 | one PDCCH submit/accept thread per cell group, 2 PDSCH dispatch/collect threads, 1 PUSCH dispatch/collect thread, 1 spare. Today PDSCH consumers 3,4 and PUSCH 0,1 are on A725 (GPU-INVENTORY thread table). |
| A725 helpers | 0-4, 10-14 | NID worker, USS tracker, csirsDisc host, dmrsIdBg, log, stats, janon until ported. `gpuFepWorker` (core 10) idle: removed. |
Goal for G3: report "cores of CPU work per cell" (today ~7 over ~17 pinned, radio ~3.3, cell ~3.7) and "dedicated latency cores".

---

## 2. PDCCH on GPU (B3) and the short Phase-2 lock (B5) (Step 2)

### 2.1 What is reused
- `nr_pdcch_gpu_exact_fep_symbol` (nr_pdsch_gpu_exact.c:170-175, caller nr_pdcch_blind_monitor_rt.c:6065) and `nr_pdcch_gpu_fep.cu` (k_gold / k_llr / k_ls_pdcch; stream at greatest priority in the RT green context, :205-215). Off today (`pdcch_gpu=0`, GPU-INVENTORY section 0) because it is called per symbol from the scan thread.
- `npg_decode_vec` (nr_polar_gpu_mod.c:75): batch polar decode, n_vec distinct unscrambled vectors, `vidx[]` selects the vector per item. The CPU sweep already builds exactly this grid: "ONE GPU BATCH PER SWEEP", 45 candidates x (NR_DCI_MAX_PAYLOAD+1) lengths = 1530 items (nr_pdcch_blind_monitor_rt.c:3836-3845, `SWEEP_BATCH_MAX_CAND 45`). That batch is the unit B3 submits for the whole slot.
- NID sweep kernels (`nr_pdcch_nid_gpu.cu`, ~20 kernels, disc partition, least priority :576) stay in the background class (4.2).

### 2.2 Batch layout (one submission per slot per cell group)
```
job (per CORESET x search space x aggregation level x candidate x cell):
  cell, coreset_id, ss_id, L, cce_start, n_rnti_hyp, dmrs_id(s), dci_len_set -> items
slot header: { cell[], slot, sym_ready_mask, ring offset per cell }
```
One kernel chain per slot: FEP of CORESET symbols (cell-batched) -> gold/descramble once per (candidate, dmrs_id) (CPU already hoists this, :3830) -> LLR -> `npg_decode_vec` over all (candidate, length) -> CRC-RNTI match against a device-resident RNTI set (C-RNTI hypotheses and fixed RNTIs) -> compact hit list. Output `nr_gpu_dci_hit_t[]` {cell, coreset, ss, cce, L, len, rnti, payload}. Joint-anon (RNTI-less) search is the same chain with the CRC check replaced by "CRC-mask recovery" and runs in the same submission as a second item class (item J), removing the 70 % unpinned `pdcchJanon` thread (V drops 282k of 539k submissions, GPU-INVENTORY 2a).

### 2.3 What stays CPU
DCI field decode and validation, RNTI tracking and aging, USS hash tracker, TDRA table learning (G1 item T-DRA), grant build and enqueue. These are 30 us per slot (table 1.1 row 2) and are control logic with branching.

### 2.4 Phase-2 lock split (B5)
Today the lock path (confirm a PCI/CORESET/RNTI hypothesis, then switch to cheap tracking) runs on the CPU scan threads. Split:
1. **GPU phase 1 (always on)**: the full hypothesis grid above; hits are counted per hypothesis in a device histogram (no host involvement).
2. **Lock decision (CPU, 50 us budget)**: a hypothesis locks when N consecutive slots hit with CRC-RNTI match; N and thresholds are the existing ones, not retuned.
3. **GPU phase 2 (locked)**: a reduced grid (one hypothesis per UE per CORESET) is submitted by the same entry point with `n_items` small, so the lock path is the same kernel with fewer items; "short" means the lock holds the GPU stream only for that reduced chain (<= 1 slot), never for the full grid, and never takes the discovery stream.

### 2.5 Multi-cell
`cell` is in every job and hit. N cells are one submission: the kernels index `cell` into per-cell tables (frame params, ring pointer, DM-RS ids, RNTI sets). No per-cell stream, no per-cell CUDA context (MPCI-OTA: a second context multiplies the body 4-9x and the channelizer overruns; one context is a hard requirement for G4).

---

## 3. Memory model (Step 3)

One runtime switch: `nr_gpu_model()` (openair1/PHY/TOOLS/nr_gpu_mem.c:8-13, `NR_GPU_MODEL_DIRECT` / `NR_GPU_MODEL_DISCRETE`, env `ISAC_GPU_FORCE_DISCRETE=1` for tests). One buffer type `nr_gpu_buf_t {dev, host, bytes, model}` (nr_gpu_mem.h:10). DIRECT: `cudaHostAlloc` mapped + device pointer, `dev` and `host` are the same memory, `h2d_async` / `d2h_async` are no-ops (nr_gpu_mem.c:21-27 DIRECT alloc, :37-46 copies). DISCRETE: pinned host + `cudaMalloc`, explicit async copies on the job's stream. Allocation is pre-fence only (`nr_gpu_allocations_allowed`, nr_gpu_mem.c:19; header :11): every pipeline buffer is allocated at init for `N_CELL_MAX` cells and a slot ring, never per slot.

Rule for new code: **allocate through `nr_gpu_buf_alloc`, move through `nr_gpu_buf_h2d_async` / `d2h_async`, and call them unconditionally**. The DIRECT/DISCRETE difference is then inside the helper, not an `if` in the caller (the existing explicit branches in nr_pdsch_gpu_fep.cu:1233-1286 and nr_gpu_pass_graph.cu:330-1118 move behind it).

| buffer | size per cell | owner (alloc) | lifetime | DIRECT (GB10) | DISCRETE (sens6) |
|---|---|---|---|---|---|
| IQ ring (slot-indexed, 4 ant) | the existing host ring | radio (usrp_lib) | ring period | kernels read the host ring in place via device pointer | one H2D per slot, only the symbol groups needed, from the slot's pinned ring slice into a device slot ring (as nr_pdsch_gpu_fep.cu:1281-1286 does today) |
| freq-domain slot grid (post FEP) | 4 ant x 14 sym x N_fft complex | pipeline init | one slot, ring of 3 | device-only scratch (no host mapping needed) | device-only |
| PDCCH job table / hit list | KB | pipeline init, `nr_gpu_buf_t` | one slot | mapped, host reads hits after the fence | host table H2D (<= 64 KB), hit list D2H (<= 8 KB) |
| RNTI sets, DM-RS ids, TDRA tables | KB, per cell | CPU control, written at lock | until changed | mapped, written in place, version counter | H2D on change only (rare) |
| per-grant LLR / equaliser scratch | up to 273 PRB x 12 sym x 4 L x Qm | pipeline init, per consumer slot | one job | device-only scratch | device-only |
| LDPC input (dematched LLR) | NR_GPU_LDPC_DEV_IN_BYTES = 32 x stride (nr_pdsch_gpu_ldpc.h:40) | pipeline init | one launch | device-only (written by the dematch kernel, no host copy) | device-only |
| LDPC output (snapshots, CRC flags, `res[]`) | NR_GPU_LDPC_DEV_BITS_BYTES (header :38-41) | pipeline init | one launch | mapped: host reads in place | one D2H of TB bits + flags |
| HARQ soft buffers (UL C3, DL later) | per (cell, rnti, pid) | pipeline init, slab | until ACK/new data | device-only | device-only |
| UL IQ retention (C2) | N slots x UL PRB x 14 sym x 4 ant | pipeline init | seconds | mapped host ring (already in host RAM) | device ring, D2H not needed; bounded by VRAM (RTX 4070 12 GB): budget set at init |
| csirs_disc working set | existing | today `cudaMallocManaged` (GPU-INVENTORY section 3) | - | keep | **must move to `nr_gpu_buf_alloc`** (no discrete path today) |

Discrete caveats (sens6) [CODE-READ unless stated]:
- FP64: the DL pass graph has an FP32 gate (`G.fp32`, nr_gpu_pass_dl.c:314, nr_gpu_pass_fp32_gate.h). New kernels default to the FP32 or mixed path with the CPU-reference tolerance check; FP64 only where the bit-exact reference needs it, and never in the 500 us path on DISCRETE.
- LDPC kernel family: `decide_nc()` picks the non-cluster kernels when compute capability < 9 (nr_pdsch_gpu_ldpc.cu:562, `major < 9`). sm_89 therefore always runs `_nc`: the NC fix below is also the sens6 path and is already the tested one (ctest `test_nr_pdsch_gpu_ldpc_nc`).
- Demod, LDPC and PDCCH FEP rely on mapped/pinned host memory (GPU-INVENTORY section 3): discrete behaviour on sens6 is UNVERIFIED; each port includes a `ISAC_GPU_FORCE_DISCRETE=1` run on the DGX (functional) and a sens6 run at the phase gate (timing).
- Green contexts and SM partitions: sm_89 support of the partition calls used in `nr_gpu_green.h` is UNVERIFIED; fallback exists (priority-only streams, e.g. nr_pdsch_gpu_ldpc.cu:955-975 attempt 1 and nr_pdcch_gpu_fep.cu:211-212).

---

## 4. Streams, priorities, partitions (Step 4)

### 4.1 Structure
One process, one CUDA context (`nr_gpu_platform`). Two SM partitions through green contexts (nr_gpu_green.h:54 `nr_gpu_green_stream(&s, priority, rt, idx)`): RT partition (40 of 48 SMs with `ISAC_GPU_RT_ISOLATE=1 ISAC_GPU_DISC_SMS=8`, `[MEASURED env t26a*, GPU-INVENTORY section 0]`) and disc partition (8 SMs). Today each translation unit builds its own streams over the same split (GPU-INVENTORY section 3); target: **one stream factory** in the platform layer, `nr_gpu_stream_get(class, idx)`, so priorities are decided in one place. TO BE FIXED BY TASK 4: whether 40/8 is the right split; LDPC-PERCB shows 40 SMs give 3 concurrent 8-SM clusters (nc: 40 single-SM CBs), which is the reason nc is the default for C >= 4, but the stall trace may show the disc partition starving the RT one through memory bandwidth rather than SMs.

### 4.2 Classes and the background kernel bound X
| class | users | partition | priority |
|---|---|---|---|
| RT-DL | slot FEP, PDCCH batch, chest/demod/dematch/LDPC (normal lane) | RT | highest |
| RT-DL-heavy | the same chain for heavy-lane grants | RT | highest, own stream pool (never behind small grants on one stream) |
| RT-UL | UL FEP, solve, LDPC, HARQ | RT | highest (UL has its own pool, nr_pdsch_gpu_ldpc.cu:955-975 selects `nr_pusch_gpu_ul_stream`) |
| BG | NID sweep, CSI-RS discovery, DM-RS ID hypotheses, UL data-ID search, sensing | disc | lowest |

**X (maximum length of one background kernel launch): provisional X = 200 us. TO BE FIXED BY TASK 4.** Reason for the provisional value: a non-preemptible block wave on a shared SM adds its full duration to a waiting RT block; 200 us is 40 % of the whole budget and 5x the largest per-stage allocation that has headroom (row 3), so anything longer is chopped. Evidence that today violates it: NID kernels max ~1.9 ms and stage-1 594-691 ms host-side (GPU-INVENTORY 2a), i.e. at least a 10x chop is needed; the 0.1-1.9 s stalls Task 4 looks for are not explained by that and are the actual X driver. Rule once Task 4 gives the number: every BG kernel launches in chunks with an explicit grid bound so one chunk <= X, with the chunk loop on the host (async, no sync) so RT work interleaves. Enforced by a CI check (test that reads `nsys` kernel durations, extending `tests/passive_rx/sysan_nsys.py`).

### 4.3 Priority rules
Provisional, TO BE FIXED BY TASK 4 (they assume RT green partition isolation really holds):
1. RT classes use the greatest stream priority, BG the least; no BG stream may be created without the `bg` flag in the factory.
2. The heavy lane and normal lane have the same priority (a heavy grant is never made slower to favour a small one: the p99 set is the heavy grants, ROOTCAUSE 1); isolation is by separate streams and separate consumers, not by priority.
3. PDCCH batch and slot FEP are launched first in the slot; they are the only stages that may pre-empt a running chest/demod/LDPC in the same partition, and only through stream priority.
4. DISCRETE: the H2D copies of the IQ ring use a dedicated copy stream at the highest priority, so a BG copy never queues in front of them.
5. Budget guard (existing): `nr_gpu_budget_rt_begin/end/large_begin/large_end` and `nr_gpu_budget_discovery_begin/delay_us` (nr_gpu_budget.h:17-39) keep BG launches out of the window while RT work is in flight; reused, not replaced. The UL pool already uses it (`nr_gpu_budget_rt_active()`, nr_pdsch_gpu_ldpc.cu:1007).

---

## 5. Multi-cell-ready GPU API (the interfaces)

Common conventions for all calls (explain once):
- `pipe`: opaque `nr_gpu_pipe_t*` from `nr_gpu_pipe_init(const nr_gpu_pipe_cfg_t*)`, one per process. `cfg` carries `n_cells_max`, per-cell frame params, ring geometry, SM split, `nr_gpu_model()` result; allocation of every buffer in section 3 happens here (pre-fence).
- `cell`: uint8, index into the per-cell tables; present in every job and every result.
- Batching: a call takes `n` jobs that may belong to different cells and different slots; the implementation groups by kernel shape, never by cell.
- Submission is async and returns a `nr_gpu_fence_t` (an event on the job's stream). `collect` waits with `cudaStreamQuery` plus the existing poll helper `nr_gpu_poll_relax` (as nr_pdsch_gpu_ldpc.cu:691-697), not `cudaStreamSynchronize` (blocking-sync schedule sleeps on the interrupt; comment at :691-693).
- All calls return 0 or a negative code with a **reason** (`NR_GPU_ERR_*`), and increment a per-reason counter; no bare `-1` (section 6, item F).
- Allocation model by `nr_gpu_model()` is the section 3 table; the caller does nothing model-specific.

```c
/* ---- PDCCH (item B3) ---- */
typedef struct { uint8_t cell; uint8_t coreset; uint8_t ss; uint8_t L; uint16_t cce0; uint16_t dci_len_mask_idx; uint16_t dmrs_id; uint8_t flags; } nr_gpu_pdcch_item_t;
/* One submission = all items of one slot for n_cells cells. Stream: RT-DL highest. */
int nr_gpu_pdcch_submit(nr_gpu_pipe_t *pipe, const nr_gpu_slot_ref_t *slots /*per cell: slot, sym_ready_mask*/, int n_cells,
                        const nr_gpu_pdcch_item_t *items, int n_items, nr_gpu_fence_t *out);
int nr_gpu_pdcch_collect(nr_gpu_fence_t f, nr_gpu_dci_hit_t *hits /*{cell,coreset,ss,cce,L,len,rnti,payload}*/, int max_hits, int *n_hits);

/* ---- slot FEP (grant independent) ---- */
int nr_gpu_slot_fep_submit(nr_gpu_pipe_t *pipe, const nr_gpu_slot_ref_t *slots, int n_cells, uint16_t sym_from, uint16_t sym_to, nr_gpu_fence_t *out);

/* ---- PDSCH chain: chest + demod + dematch + LDPC + CRC (items L, Q) ---- */
typedef struct { uint8_t cell; uint8_t heavy; /* lane */ uint16_t rnti; nr_gpu_pdsch_pdu_t pdu; /* PRB, S/L, mcs, tbl, Nl, DM-RS, rv, rate-match mask id */ } nr_gpu_pdsch_job_t;
int nr_gpu_pdsch_submit(nr_gpu_pipe_t *pipe, const nr_gpu_pdsch_job_t *jobs, int n, nr_gpu_fence_t *out);   /* lane picks the stream pool */
int nr_gpu_pdsch_collect(nr_gpu_fence_t f, nr_gpu_pdsch_result_t *res /*{cell,rnti,crc,tb bits,evm,snr_l[4],noise_floor_aware_snr,cb_ok mask}*/, int n);

/* ---- PUSCH (items U-GPU, U2, C3) ---- */
int nr_gpu_pusch_submit(nr_gpu_pipe_t *pipe, const nr_gpu_pusch_job_t *jobs /*cell, rnti, pid, ndi, layout, dmrs_id, data_id, harq slot*/, int n, nr_gpu_fence_t *out);
int nr_gpu_pusch_collect(nr_gpu_fence_t f, nr_gpu_pusch_result_t *res, int n);
/* IQ retention + background data-ID search (BG class, chunked <= X) */
int nr_gpu_iqstore_put(nr_gpu_pipe_t *pipe, uint8_t cell, uint32_t slot_abs, uint16_t rnti);          /* pins a ring slice */
int nr_gpu_dataid_search_submit(nr_gpu_pipe_t *pipe, const nr_gpu_dataid_job_t *jobs /*cell, rnti, retained slice id, dmrs id set*/, int n, nr_gpu_fence_t *out);
int nr_gpu_dataid_search_collect(nr_gpu_fence_t f, nr_gpu_dataid_hit_t *hits, int max, int *n); /* {cell,rnti,data_id,crc_ok} */

/* ---- DM-RS scan / ID hypotheses (BG) ---- */
int nr_gpu_dmrs_scan_submit(nr_gpu_pipe_t *pipe, const nr_gpu_dmrs_job_t *jobs, int n, nr_gpu_fence_t *out);
```
Per-interface summary:

| interface | cell index | batching across cells | allocation (DIRECT / DISCRETE) | stream, priority |
|---|---|---|---|---|
| `nr_gpu_pdcch_*` | in item and hit | one launch chain for all cells' items of a slot | job table + hit list mapped / H2D+D2H small | RT, highest |
| `nr_gpu_slot_fep_submit` | per `slots[]` entry | one kernel over cells x symbols x antennas | ring read in place / H2D of the needed symbol groups | RT, highest |
| `nr_gpu_pdsch_*` | in job and result | jobs of all cells grouped by (Nl, Qm, CB-count class); LDPC launches pooled up to 32 CBs across jobs of the same code class | device scratch both; result mapped / D2H | RT, highest; normal and heavy lane = separate stream pools |
| `nr_gpu_pusch_*` | in job | same grouping | HARQ slab device-only | RT-UL, highest |
| `nr_gpu_iqstore_*`, `dataid_search_*`, `dmrs_scan_*` | in job | batched over cells and UEs | host ring / device ring | BG, lowest, chunk <= X |

Existing entry points these wrap, not rewrite: `nr_pdsch_gpu_ldpc_run` (nr_pdsch_gpu_ldpc.cu:658; takes no stream, owns a ctx pool), the device-resident `nr_pdsch_gpu_ldpc_dev` (:769, takes `stream` and `nr_gpu_buf_t`), UL `ul_exec` (:848). The `_dev` form is the model for the new calls: input and output are `nr_gpu_buf_t`, the stream is an argument.

---

## 6. G1/G2 fix list (work items, stage touched, GPU port it rides on)

Rule (roadmap G3-together): an item that touches a CPU stage is implemented as that stage's GPU port (CPU kept as reference + fallback). "Rides on" names the port in section 7 whose kernel the change lives in. Items that touch only control/scheduling stay CPU (KEEP-CPU per GPU-INVENTORY section 4) and say so.

| id | work item | evidence (cause) | stage touched | rides on | notes |
|---|---|---|---|---|---|
| T-DRA | DL TDRA S/L ambiguity on V: learn the per-RNTI time-domain table (resolve S/L at the first success, then fix it per RNTI) | DLCRC 4: V ceiling 70-75 % at any margin; DECODETDRA recovers V 18320/62655 (S=1/L=13) and 8005/27073 (S=0/L=14); wrong S/L changes G and TBS (nr_pdsch_passive_decode.c:1690-1742); retry at :6393-6530 | PDSCH job build (CPU control) + dematch/demod symbol range (GPU) | **P-DEMATCH**: S/L enters the GPU job (`pdu`), and the per-RNTI table is read by the job builder in row 2. Retry stays as CPU/GPU fallback. | ~24k of 71k V qualified failures (34 %). First confirm with the replay in DLCRC 6 item 2 (`raw_pci415_12s_b`, `ISAC_PDSCH_TDRA_RETRY=0` vs `1`, `ISAC_PDSCH_DECODE_DIAG=1`). |
| T-DEAD | T periodic dead bins: periodic CSI-RS/TRS (20 ms) not rate-matched around | DLCRC 3: slots 8/18 of even frames CRC 0 % (n=896) vs odd 100 %; slots 3/13 nsym=11 0 %; 7 % of T qualified losses; G count uses confirmed CSI-RS resources only (nr_pdsch_passive_decode.c:2858-2880); static resources PCI 415 only (:6017-6040) | rate-match RE mask (G, demod RE selection) | **P-DEMATCH** + demod RE mask: the mask is a per-cell device table `rate-match mask id` in the job; periodic entries carry (period, offset). CSI-RS discovery (GPU sweep) feeds it. | Confirm: replay `raw_pci923_tim_12s_tile3` with `ISAC_CSIRS_FULL=1 ISAC_CSIRS_FULL_AB=1`. [INFERRED cause until then]. |
| E-SNR | EVM-SNR estimator bias: noise-floor-aware SNR, also fed to the solver | DLCRC 1: decision-directed EVM reads 3.9/9.1/12.5/14.4 dB for pure noise (QPSK/16/64/256QAM); 90 % (T) and 65 % (V) of qualified failures are not truly qualified; solver fed the same value (nr_pdsch_passive_queue.c:2233 `nr_pdsch_solver_note_snr`); estimator at nr_dlsch_demodulation.c:2166-2300, `layer_snr_db` nr_pdsch_passive_decode.c:472 | EVM/SNR (today CPU, GPU kernels evm1-3 in nr_pdsch_gpu_demod.cu) | **P-FUSE** (chest+demod+EVM): the GPU kernel emits `noise_floor_aware_snr` = inverse of the simulated floor curve per Qm, plus DM-RS based `nvar`/`alpha`; `goal_metrics.py` qualification switches to it. | Metric first (no port needed to re-baseline the scorecard); the same formula goes into the fused kernel. Re-baseline before G1(b) is called valid (SCORECARD Step 5). |
| Q | Queue policy: heavy-grant lane, small grants first | ROOTCAUSE 4: qwait + pre are the top p99 components (V 44-63 %, T 18-74 %); slot group decodes widest first (nr_pdsch_passive_queue.c ~1460-1729) | scheduling (CPU) | none (KEEP-CPU policy); the lane id is the `heavy` field of `nr_gpu_pdsch_job_t` so it maps to a stream pool | lane threshold: >= 8 CBs, or > 150 PRB, or Nl >= 3 (ROOTCAUSE 4); small first within a slot; heavy lane has its own consumers. |
| L | LDPC prep on GPU (rate-dematch + deinterleave) | `prep_segment` 18.9 us/CB serial, 0.32-0.38 ms at CB16+ (LDPC-PERCB, ALLON "CPU LDPC prep unchanged") | PDSCH LDPC prep (CPU) | **P-DEMATCH** | `cb0_dematch_kernel` exists (nr_td_cb0_batch.cu); needs DL variants (rv, `tbslbrm`, filler F, two E sizes `E`/`E2` with `first_rE2`, nr_pdsch_gpu_ldpc_host.c:115-135). CPU `prep_segment` is the bit-exact reference. |
| NC | NC kernel threshold: `ISAC_GPU_LDPC_NC_MIN_CB` 8 -> 4, and use nc in the UL pool | LDPC-PERCB: crossover C ~3-4 (nc slower at C=1: 347 vs 114 us); CB16+ wait halved at 8 (ALLON) | LDPC launch (`decide_nc`, nr_pdsch_gpu_ldpc.cu:550-569) | **P-NC** (config + UL `ul_exec` -> `ldpc_launch`) | warm both families in `warm_common` (lazy-load stalls). Default must not depend on env for sens6 (always nc, cc < 9). |
| F | GPU LDPC CPU fallbacks: DL 1.4 % (T, 1501 of 106000 TBs; V 2257), UL ~10 % (V 18560, T 11798) [MEASURED, GPU-INVENTORY 2b]. Find why `G->run` fails | DL path: `G->run(...) != 0` returns 1 and counts `fb_err` (nr_pdsch_gpu_ldpc_host.c:178-181). `nr_pdsch_gpu_ldpc_run` returns -1 on several different conditions with no reason: not initialised or n out of range (:660-661), `it_max` out of range / `K & 7` / `K>>3 > KB_MAX` / **graph miss `graph(BG,Z,R)`** (:664-667), stream error (:696-704), `nsnap` outside [0, 16] (the -1 sentinel the kernel did not overwrite, :730-733). UL path (`ul_exec`, :848-905; caller :1000-1040) has four more: 100 ms window **cap** (`n_cap`, :1002), RT active with no partition (`n_rt`, :1007), no free ctx (`n_busy`, :1012), **4 ms deadline** (`n_deadline`, `deadline_us = 4000`, :806 / :872), `n_err`. | LDPC launch layer (both) | **P-NC** (diagnosis ships with it) | Hypotheses, none proven [INFERRED]: DL = graph miss for a (BG,Z,R) not pre-built, or the sentinel left by a timeout; UL ~10 % = `n_cap`/`n_busy`/`n_deadline` when the UL pool shares the RT partition with DL. Work item: reason enum + per-reason counters in `fb_err` (DL) and a printed breakdown of `n_cap/n_rt/n_busy/n_deadline/n_err` (UL) in the stats line; one OTA-free replay; then remove the top reason. Each fallback is a CPU LDPC decode on the latency path. |
| B3 | PDCCH fully on GPU | ROOTCAUSE 4.4: `a2e` p50 ~0.6 ms is a floor; AB-SPREAD/PDCCH-PARBANKS: CPU scan slower with more threads (bank pass 158-182 -> 219-272 us), core-class sensitive; pdcchJanon 70 % | PDCCH scan (CPU) | **P-PDCCH** | section 2. PAR_BANKS=1 and the third scan thread are abandoned (PDCCH-PARBANKS-OTA verdict). |
| B5 | short Phase-2 lock | section 2.4 | PDCCH lock logic | **P-PDCCH** | |
| J | joint-anon on the same chain | GPU-INVENTORY 2a: V done 256k / DROPPED 282k; T 187k / 8.7k | pdcchJanon (CPU, unpinned 70 %) | **P-JANON** (after P-PDCCH) | same LLR chain, CRC replaced by mask recovery. |
| C1 | UL BWP look-alike lock (216@1 vs DL 217@0) | ULCAUSE: RIV placement of every non-probe grant shifted after the lock (`nr_pdcch_ul_layout_lock.c`) | UL layout (CPU control) | none (control) but the locked table is a device table read by **P-ULSOLVE** | verification: brute-force replay of ULCAUSE "separating test". |
| C4 | UL probe/real log split (`probe=`, `tbl=` in `UL_PUSCH_CRC`) | ULCAUSE: 61,337 of 77,347 status=1 lines are 3-PRB probes; logs cannot separate wrong-ID from low SNR; `goal_metrics.py` already reads optional `probe=`/`tbl=` | log only | none | prerequisite of every UL measurement; first UL item. |
| C2 | UL IQ retention + 1024 data-ID search on GPU | ULCAUSE: V ID-limited (attempts with snr_db >= 10: 43 / 17 RNTIs, different data_id each), hit rate ~attempts/1024; right pair decoded 23/23 at -6.7 dB (ab_G1_1_234131) | UL ID search (CPU, nr_scrambling_id_sweep, `nr_pusch_ul_bgdisc.c`) | **P-ULID** | 1024 independent descramble+LDPC-CRC tests on one retained slice: batch = 1024 items, BG class, chunked <= X (4.2). CPU sweep is the reference. T is SNR-limited (best ~7 dB): not fixed by this item, reported as such. |
| C3 | UL HARQ soft combining on GPU | UL retransmissions (rv > 0) are decoded alone today; no combining | UL LDPC input | **P-ULSOLVE** | device HARQ slab keyed (cell, rnti, pid), cleared on NDI toggle. |
| DM | DM-RS scan on GPU | dmrsScan core 2: V 186 full scans mean 95 ms, T 810 scans 70.7 ms; GPU only does the check (3534 checks 87 us) | DM-RS ID / TDRA solver | **P-DMRS** | `k_unscr`/`k_pick` in nr_gpu_pass_graph.cu are the starting kernels. |
| CORE | Core placement: latency threads on X925 5-9/15-17, helpers on A725 | AB-SPREAD-PIN: scan on A725 +0.4-1.1 ms `a2e`; PDSCH consumers 3,4 and PUSCH 0,1 on A725 today | thread pinning, no code port | none (config) | table 1.3. Interleaved A/B. |

Items that must be done in this order for the metric to mean anything: C4, E-SNR (re-baseline), then everything else.

---

## 7. Phase 1 port order, ranked by p99 / CRC gain per week (Step 5)

Effort is the author's estimate [INFERRED]; gain is from the cited report. "Replay" = recorded-capture A/B on the DGX, no radio (`raw_pci415_12s_b` for V, `raw_pci923_tim_12s_tile3` for T, never pooled). "OTA" = interleaved A/B against the current default (t26f), n >= 3 per cell, daytime, only by the orchestrator and when the radio is back. Accuracy test = CPU reference check on recorded slots, bit-exact unless a tolerance is stated.

| rank | port / item | effort | gain | accuracy test (CPU reference) | validation |
|---|---|---|---|---|---|
| 1 | CORE placement | 0.2 wk | removes core-class sensitivity of `a2e`, frees A725 for helpers | none (no code) | OTA A/B only (cannot replay thread timing) |
| 2 | **P-NC** + diagnosis F | 0.5 wk | LDPC wait at CB16+ x0.5 (ALLON), removes 1.4 % DL / ~10 % UL CPU fallbacks (CPU LDPC on the latency path); nc is the only sm_89 path | `test_nr_pdsch_gpu_ldpc` and `_nc` bit-identical (ctest), plus UL nc vs cluster | replay A/B (CRC set identical, fb counters by reason); OTA A/B |
| 3 | C4 + **E-SNR** (metric) | 0.5 wk | no latency; makes G1(b) valid (today 65-90 % of "qualified failures" are not qualified) | simulated AWGN curve vs formula (DLCRC 1 sim) per Qm | replay: re-run SCORECARD on the existing 15 runs; no OTA needed |
| 4 | **Q** queue policy: heavy lane, small first | 0.5 wk | qwait + pre = top p99 component (V 44-63 % of the p99 set; 4-7 ms) | decoded-TB set identical to baseline on replay (CRC bits unchanged); drops < 1 % | replay A/B p99 `qwait`/`pre`; OTA A/B |
| 5 | **T-DRA** (inside P-DEMATCH job build; CPU first, GPU range after) | 1 wk | V CRC: up to 34 % of qualified losses (~24k of 71k) => crc_q +10 to +20 pp [INFERRED] | per-slot CRC with retry=0/1 on `raw_pci415_12s_b`: failing set in slots 1-5/11-15 must equal the retry-recovered set | replay A/B; OTA A/B |
| 6 | **T-DEAD** (periodic CSI-RS/TRS mask) | 0.5-1 wk | T: 7 % of qualified losses (2426), CRC 92.4 -> 97.3 % over qualified grants | replay with `ISAC_CSIRS_FULL` on/off, CRC in even-frame slots 8/18 and 3/13 | replay A/B; OTA A/B |
| 7 | UL **C1** lock + **C2/P-ULID** | 0.5 wk + 2.5 wk | UL CRC from ~0 on V (the only G2 path); T stays SNR-limited, reported | brute-force data_id 0..1023 on retained IQ of a strong-DM-RS V grant: GPU hit set == CPU sweep hit set | replay (needs IQ retention capture) then OTA A/B; UL track (cloud x86 rfsim for the CPU parts) |
| 8 | **P-DEMATCH** (GPU rate-dematch) | 1.5 wk | -0.3 to -0.4 ms per CB16+ grant; enables T-DRA/T-DEAD masks on device | bit-exact vs `prep_segment` LLR output, all (BG, Z, rv, E/E2, F) classes of the recorded captures | replay A/B; OTA A/B |
| 9 | **P-PDCCH** (B3 + B5) | 3 wk | `a2e` 0.6-0.9 ms p50 / 3-4.5 ms p99 -> <= 60 us after t=0; frees ~0.6 core; a prerequisite for G1(a) at all (ROOTCAUSE 4.4) | DCI hit set (cell, rnti, cce, payload) identical to CPU scan on the recorded slots; false-alarm rate unchanged | replay A/B (hit set + latency); OTA A/B; sens6 gate |
| 10 | **P-FUSE** (chest+demod+EVM fused, FP32 path) | 1.5 wk | chest+demod 280-346 us mean -> 150 us p99 (estimate); completes the 500 us sum | LLR sign agreement and CRC set vs the existing GPU exact path (`ISAC_PDSCH_GPU_EXACT_*_CHECK`), tolerance stated per Qm; FP32 gate | replay A/B; OTA A/B; sens6 FP32 timing |
| 11 | **P-ULSOLVE** (UL solver/equalise + C3 HARQ) | 2.5 wk | UL cpu p50 1.0-1.4 ms -> GPU; removes UL threads 0,1; soft combining raises CRC on retx | CPU `nr_pusch_passive_decode` LLR/CRC agreement on recorded UL slots | replay; OTA A/B |
| 12 | **P-JANON** | 1 wk | pdcchJanon 70 % core gone; V stops dropping 52 % of submissions | hit set == CPU janon | replay; OTA A/B |
| 13 | **P-DMRS** (DM-RS scan / ID hypotheses) | 1.5 wk | dmrsScan 25 % + dmrsIdBg x3 (~6 % each); scans 70-95 ms each | hypothesis ranking identical on recorded captures | replay |
| 14 | cluster-2/4 LDPC variant (R13, 273 PRB, 4L stretch case) | 1 wk | closes the R13 32-CB gap (441 us -> <= 200 us target) [INFERRED] | bit-exact vs `_nc` | bench (`test_nr_pdsch_gpu_ldpc 2 bench`), then OTA |

Ordering logic: ranks 1-4 are small and remove the largest p99 drivers or make the metric valid; 5-6 are the CRC losses (G1(b)); 7 is the only route to G2; 8-11 are the large ports that make the 500 us sum possible (G1(a)) and are also the G3 core savings; 12-14 are G3 remainder or stretch. P-PDCCH (rank 9) is first among the long items because the 0.6 ms `a2e` floor blocks G1(a) regardless of the decode.

## 8. Gates per port
1. CPU reference kept; GPU path behind the same env switch style used today, default off until its replay A/B passes.
2. Accuracy test on recorded slots (column above) before any OTA.
3. DGX replay A/B; then `ISAC_GPU_FORCE_DISCRETE=1` functional run on the DGX; then sens6 build + ctest + replay at the phase gate (sens6 Task-9 style; sens6 Task 9 is still pending for Stage 1).
4. OTA A/B interleaved with t26f, n >= 3 per cell, daytime, V and T separate, only by the orchestrator. Positive result -> Haiku updates PROJECT_MEMORY.md; merge to csd/otatest only then.
5. Stall regression (after Task 4): no kernel in a BG stream longer than X, no RT kernel start gap above the Task 4 threshold.

## 9. Not checked / open
- Task 4 outputs: X, priority rules, 40/8 SM split (marked above).
- Per-symbol ring write pointer availability for the early-start of rows 0-1 (usrp_lib.cpp, not read).
- Chest+demod 150 us and the P-FUSE speedup are estimates; no kernel bench of those two.
- CB-count x code-rate histogram of on-air grants (decides how often R13 32 CB occurs); the table uses R23 as primary class and R13 as stretch.
- Discrete behaviour (sens6) of demod, LDPC, PDCCH FEP, green-context partition calls on sm_89: unverified.
- The "fb err" reasons (item F) are hypotheses from reading the return paths; no counter by reason exists yet.
- V per-thread CPU map; UL `ulBgDisc`, `dmrs_nid_solve` costs (GPU-INVENTORY "Not checked").
- Effort estimates are the author's.

---

## 10. DL GPU data path v2 (2026-10-17)

Status: design, not implemented. Plan: `docs/superpowers/plans/2026-10-17-dl-gpu-datapath.md`. Branch `csd/int16-gpupath` (base csd/int16 ac5b25a608).
This section supersedes rows 4-7 of table 1.1 and ranks 8 and 10 of section 7 for the DL per-grant path. Sections 2 (PDCCH), 3 (memory model rules), 4 (streams) and 5 (multi-cell API) still hold; where 10 is more specific, 10 wins.

User decisions (2026-10-17), binding:
- The whole DL per-grant data path runs on the GPU as one chain with no host round trip between stages: FEP, channel estimation, demod, descrambling, LLR stats, rate de-matching, LDPC, CRC.
- The CPU keeps control only: DCI parsing, layout solver, RNTI tracker, launch, readback of CRC and soft bits. PDCCH blind decode stays on the CPU.
- Mechanism: pre-instantiated CUDA graphs per grant shape (or graph update with node parameters). A persistent kernel only if graphs leave a measured gap.
- Every step is bit-exact with today's path in unit tests and builds/runs on sm_121 and sm_89 (`CMAKE_CUDA_ARCHITECTURES "89;121"`). No GB10-only feature without an sm_89 fallback.
- Goals: DL p99 < 3 ms on air at daytime load on both cells (TIM PCI 923, Vodafone PCI 415), crc_q >= 80 %, drops < 1 %. Then fewer CPU cores at equal performance (10 % tolerance). Then 2 cells in one process (200 MHz wideband, GPU channelizer). The design must not block batching across cells.

### 10.0 Starting point: what exists and what it showed

Two GPU DL paths exist today [CODE-READ]:
1. **Stage path** (production, `t26gE`): `nr_pdsch_gpu_exact_fep_ranges` -> wait -> `nr_pdsch_gpu_chest_grant` -> wait -> host math -> `nr_pdsch_gpu_demod_grant` -> wait -> host LLR work -> `nr_pdsch_gpu_ldpc_decode` -> wait -> host CRC. Four host waits per body, plus host compute between them (table 10.2).
2. **GPUPASS v1** (`ISAC_GPU_PASS=1`, default off; `nr_gpu_pass_graph.cu`, `nr_gpu_pass.h`): FEP -> chest -> chest post -> demod (+EVM branch) -> descramble -> LLR norm -> rate dematch -> LDPC -> CRC -> pick -> CRC as ONE captured graph per shape class and instance, one host wait. It is bit-exact (GpuPassDl.GraphEqualsStagePath; 0 shadow mismatches on replay and on air).

What v1 showed:
- [MEASURED, DGX replay b/tim, GPUPASS-8B-20261016, n=6] first-body p50 for <= 96 PRB is not better than the stage path (528-616 vs 560-576 us). Graph launch-to-done sits in the 256-512 us bucket at p50; host prep 64 us, post 16 us.
- [MEASURED, DGX OTA T night 2026-10-16, GPUPASS-OTA, n=2] the >5 ms count is 2-9x the t26f value; ~41 % of passes end in a CRC fail. After a failing pass the first body is not repeated (CONT_FAIL copies the pass LLRs, nr_pdsch_passive_decode.c:5168-5177), but every retry arm is a full body again (FEP + chest + demod + LDPC) and every sampled shadow / need_cpu / zero-TB / rc != 0 case runs the whole stage path on top (OLD_FIRST, :5325-5338) [CODE-READ].
- [MEASURED, GPUPASS-20261016] 308 + 1400 segmented classes x 4 instances, 5792 MB of device memory, 8.6 s of warm-up before RX.
- Retry bodies re-enter the pass and re-run FEP + chest + demod + LDPC [CODE-READ, GPUPASS-8B "Code reading"]; `ISAC_GPU_PASS_RETRY_OLD=1` only sends them back to the stage path.

New measurement for this section [MEASURED, DGX replay <cap>, t26gE + ISAC_GPU_PASS=1 ISAC_GPU_PASS_RETRY_OLD=1 ISAC_GPU_PASS_POLL_SLEEP_US=600, binary rr-csd-int16gc build_t26 (csd/int16-gpuc 4576048508, same DL code as ac5b25a608), nsys -t cuda,nvtx --cuda-graph-trace=node, n=1 per capture; dirs `rr-csd-int16gpu/replays/replay_gpv_{b_141223,tim_141619}`, scripts passk.py / e2e.py in the session scratchpad, not committed]:

| v1 pass body, real grants (us) | b p50 | b p99 | tim p50 | tim p99 |
|---|---|---|---|---|
| body wall | 597 | 1639 | 630 | 1388 |
| host before graph launch (body start -> cudaGraphLaunch) | 108 | 337 | 65 | 252 |
| launch -> first kernel | 16 | 97 | 15 | 77 |
| graph span (first kernel start -> last kernel end) | 409 | 965 | 459 | 852 |
| of which GPU busy / idle inside the graph | 363 / 43 | 858 / 266 | 421 / 34 | 734 / 174 |
| host after the graph (commit, copies) | 53 | 619 | 54 | 704 |
| kernels per graph | 27 | 28 | 27 | 28 |

Kernel time per pass body, mean us (b / tim): k_ldpc4_t 76 / 63, k_fep_exact 56 / 53, k_delay_exact 43 / 34, k_tail 38 / 99, EVM chain (k_evm1-3, fold, out) 77 / 171, k_rdm 19 / 24, k_chest_post 18 / 16, k_crc 16 / 14, k_align 19 / -, k_gather 12 / 16.

Where the v1 tot tail comes from (goal_metrics tot >= p99, same runs): b n=45, tim n=51 grants; tail means b a2e 1029, qw 230, pre 503, dec 1911 us; tim a2e 635, qw 133, pre 304, dec 2110 us (bodies = 1: retries run in the async recovery worker, outside tot). **29 of 45 (b) and 42 of 51 (tim) tail grants were decoded by the stage path, not by the graph** (their EVMGRANT stage timers are > 200 us; the pass bypasses them), although only 12-13 % of all grants took the stage path. Gates that sent bodies to the stage path in these runs (GPUPASS census): probe 3249 / 2989, nomem 2471 / 945 (recovery-worker threads without prewarmed buffers), seg > 16: 389 / 250, unseen class 0 / 195, gw 99 / 32, share 36 / 35, old_first 49 / 255.
goal_metrics tot p50 / p99: b 1328 / 3029, tim 1075 / 2486 (t26gE without the pass, SCAN-TAIL2 n=8 means: b 1235 / 3500, tim 978 / 2289 [MEASURED, DGX replay]).

Reading:
1. Inside a graph the host gaps are gone: idle 34-43 us p50. The graph's span is the kernels' own time.
2. v1's kernel time per grant is higher than the stage path's own-kernel busy (363-421 vs 247-315 us p50) because each grant transforms all 14 symbols, the EVM chain is in line, and chest post, rate dematch and CRC moved to the GPU.
3. v1's tail is the grants it does not cover. Coverage, not the graph, decides p99.
4. Host before the launch (65-108 us p50, up to 337 p99) is still there: geometry, job build and parameter blocks.

Summary: the graph mechanism works (no gaps inside it). v1 lost on (a) coverage: the grants it gates off (probes, recovery-worker bodies, > 16 segments, unseen classes, shadows) are most of its tail; (b) per-grant work it added (all 14 symbols transformed per grant and per retry body; every retry a full body); (c) one graph per full-shape product, which caps the class set, memory (5.7-6.8 GB) and warm-up (8.6 s). v2 keeps v1's kernels and its bit-exact harness and changes the structure: three sub-graphs per grant plus one slot graph, the instance held across retries, and coverage of every body kind.

### 10.1 Where the stage-path time goes today

[MEASURED, DGX replay, nsys + NVTX body ranges, t26hE (= t26gE + RNTI_LAYOUT_CACHE + UL_LOOKALIKE, DL data path identical), csd/int16-gpuc build, n=1 trace per capture; traces `rr-csd-int16gc/replays/replay_ns_{b_111100,tim_111154}`, script in this session's scratchpad]. "busy" = union of the body's own kernels; "gaps" = wall time between them (host compute, host waits, launch latency).

| per real body (us) | b p50 | b p99 | tim p50 | tim p99 |
|---|---|---|---|---|
| wall | 564 | 2019 | 589 | 2043 |
| own kernels busy | 247 | 849 | 315 | 1016 |
| gaps between own kernels | 172 | 1125 | 159 | 903 |
| kernels per body | 18 | 19 | 18 | 19 |

| per decode call incl. retries (us) | b p50 | b p99 | tim p50 | tim p99 |
|---|---|---|---|---|
| wall | 603 | 4233 | 592 | 2720 |
| own kernels busy | 257 | 1825 | 317 | 1217 |
| bodies per call in the slowest 1 % | 7.9 (mean) | | 2.6 (mean) | |

Host launch API cost: 13.5 `cudaLaunchKernel` + 0.8 `cudaLaunchKernelExC` + 1.6 `cudaMemcpyAsync` per real body = 97 us of host API time per body [MEASURED, same b trace; nsys inflates API cost].

Kernel time per tail body (slowest 1 % of real bodies, mean us): b: k_delay_exact 125, k_ldpc4_t 115, k_tail 67, k_fep_exact 65, k_evm1 61, k_fir 59, k_mmse3_prep 29, k_ls 28. tim: k_delay_exact 230, k_evm1 212, k_tail 187, k_ldpc4_t 185, k_fir 113, k_fep_exact 80, k_ls 59. [MEASURED, same traces]

Combined with GPU-CONTENTION-20261017 (host time between stages 71 % / 36 % of tail-body wall for b / tim; own kernels 17 / 26 %; contention 12-38 %) and LARGE-GRANT-TAIL / HOST-GAP-20261017 (chest end -> demod launch 287-300 us p50 before GATHER_FAST, 113 us after; demod end -> LDPC launch 284-397 us p50; LLR stats + unscramble 62 us mean, 145 at CB >= 16; prep_segment 18-98 us p50 with HOST_FAST, 430 us p90 at CB >= 16): the floor reachable by removing every gap is the busy column, i.e. -55 to -60 % of body wall at p99, before any kernel gets faster.

### 10.2 Stage list and where each host step goes

Source: code map of passive_decode_body_core on csd/int16-gpupath [CODE-READ, this session]; D = nr_pdsch_passive_decode.c, DH = nr_pdsch_gpu_demod_host.c, LH = nr_pdsch_gpu_ldpc_host.c, G = nr_gpu_pass_graph.cu, P = nr_gpu_pass_post.cuh. Cost labels from LARGE-GRANT-TAIL / HOST-GAP-20261017 [MEASURED, DGX replay].

| # | stage / host step today (t26gE) | file:line | today | v2: where it runs | kernel (v2) |
|---|---|---|---|---|---|
| 0 | slot FEP | D:3290, wait nr_pdsch_gpu_fep.cu:196 | GPU per grant, host wait | sub-graph S, once per (cell, slot), launched by the dispatcher at slot read, before any DCI | k_fep_exact (exists) |
| 1 | chest LS, delay, FIR, seg compose | D:3532/3586, wait nr_pdsch_gpu_fep.cu:1488 | GPU, host wait; 78-204 us | C | k_ls, k_delay_exact, k_fir, k_seg_compose (exist) |
| 2 | nvar sum, normalisation, per-branch substitution | D:3540-3603, 3987-4053 | host, small | C | k_chest_post block 1 (exists, P:215) |
| 3 | DM-RS CFO/SFO measurement | D:4080-4328 | host O(N) per row | C (measurement); the EMA and log stay host after readback (`gp_dmrsfo_commit`, D:2434) | k_chest_post block 0 (exists, P:292) |
| 4 | BRANCHFO per-antenna H1 conj(H0) | D:4329-4374 | host; also host in v1 (D:2464 on D2H est rows) | C | k_brfo (new, reduction over nrx x N, result in the result block) |
| 5 | chest cache store / lookup | D:3455-3505, 3802-3830 | host memcpy | removed: the instance keeps est on the device for the whole call (10.7) | - |
| 6 | RE gather (data order, segments, ring wrap) | D:4742-4790 | host 39-47 us p50 with GATHER_FAST (220 before) | D: composed into the demod RX index map (v1 way, nr_gpu_pass_seg_remap_geo) | k_gather (exists) |
| 7 | demod job build incl. nr_dlsch_extract_geometry | DH:312, 560-646 | host per symbol | host, BEFORE the C launch (it depends on the DCI only), written into the parameter block | - (control) |
| 8 | MRC align, level, MMSE3 prep, tail | demod.cu, wait :1687 | GPU, host wait | D | k_align, k_level, k_mmse3_prep, k_tail (exist) |
| 9 | EVM chain + fold | DH:471-538, 671-685; deferred wait demod.cu:1591 | GPU + host fold | D, forked branch joined at the end of L (off the LLR critical path) | k_evm1-3, k_evm_fold, k_evm_out (exist); host fold of the sums stays (bookkeeping) |
| 10 | Qm oracle | D:4929-4937 | host over <= 4096 equalised samples | host after readback (reads qm_row in place / D2H 16 KB) | - |
| 11 | LLR stats (absum, zero, sat, clip8; sign stats after descramble) | D:5179-5235, 5246-5270 | host; with unscramble 62 us mean, 145 at CB >= 16 | D | k_llr_stats (new; v1 skips these fields) |
| 12 | unscramble | D:5237-5244 | host NEON (HOST_FAST) | D | k_unscr (exists) |
| 13 | LLR norm (K38 shift over CB0 span) | D:5296-5312 | host O(G) | D | k_norm_sum, k_norm_apply (exist) |
| 14 | segmentation, TB params (C, K, Z, E, E2, R) | D:1814-1914 | host | host, before the C launch (DCI only) | - (control) |
| 15 | prep_segment (deinterleave, rate dematch, int8 pack) | LH:194, 251 | host 18-98 us p50, 430 p90 at CB >= 16 with HOST_FAST | L | k_rdm (exists, bit-exact, P-DEMATCH) |
| 16 | LDPC | LH:257, wait nr_pdsch_gpu_ldpc.cu:788 | GPU per 32-CB batch, host wait | L | k_ldpc4_t / nc family (exist) |
| 17 | snapshot CRC pick, TB CRC, reassembly | LH:271-281, D:2015-2042 | host | L | k_crc, k_pick, k_crc (exist) + k_tb_pack (new: reassembly into the result block) |
| 18 | commit (MAC hand-off, EVMGRANT, solver feedback, HARQ book, logs) | D:5694-5712, 1519, 5571+ | host | host after the one wait | - (control) |

Host waits per body: 4-5 today (FEP, chest, demod, [EVM], LDPC per 32-CB batch) -> 1 in v2 (end of L).

### 10.3 Graph structure, shapes and keys

One grant decode = a **chain of three sub-graphs** launched back to back on one stream, plus a slot sub-graph shared by all grants of a (cell, slot):

| sub-graph | content | key (graph structure only) | launched |
|---|---|---|---|
| **S** slot FEP | k_fep_exact over the 4 antennas x the symbols any grant of the slot needs (default 14) into the device slot grid | (cell, nsym_mask class: 1 value today) | once per (cell, slot), by the PDSCH dispatcher when the slot is read (before any DCI of that slot is known); every grant, retry and probe of the slot waits on its event with `cudaStreamWaitEvent` (no host wait). If the grid ring no longer holds the slot (> 3 slots old), the body re-runs S on its own instance |
| **C** chest | k_ls, k_delay_exact, k_fir, [k_seg_compose], k_chest_post | (nrb_class, nl, ndmrs, nseg_class) | first body; retries only when the DM-RS hypothesis (CDM group, port set) changes |
| **D** demod + LLR | k_gather, k_level, [k_align], [k_mmse3_prep], k_tail, EVM fork/join (k_evm1-3, k_evm_fold, k_evm_out), k_unscr, k_llr_stats (new), k_norm_sum, k_norm_apply | (nrb_class, nl, qmode, align, evm) | first body; retries that change the RE set, S/L, branch or MRC |
| **L** decode | k_rdm, LDPC (nc or cluster family, chosen per key), k_crc (snapshots), k_pick, k_crc (TB), k_tb_pack | (ncb_class, ldpc family) | every body (a retry that only changes rv/TB parameters relaunches only L) |

Why three and not one: v1's single key is the product of all fields (308 + 1400 classes, 5.8 GB). With sub-graphs the count is the SUM. Expected warm set [ESTIMATE from v1's class census: nrb_class 8 x nl 4 x ndmrs <= 4 x nseg 6 for C, 8 x 4 x qmode 3 x align 2 x evm 2 for D, 6 x 2 for L]: theoretical <= 768 + 384 + 12; observed classes on the two captures are expected at <= 150 C + <= 100 D + 12 L, i.e. ~260 exec graphs per instance. Instantiation cost [ESTIMATE from v1: 8.6 s for 1708 x 4 graphs = ~1.3 ms per exec graph]: ~260 x 4 x 1.3 ms = 1.4 s pre-fence. Unseen key at run time = old stage path for that grant (counted), and an off-path warm of that key on a helper thread before the next fence-free use is NOT allowed (allocation after the fence is forbidden); unseen keys are logged for the next warm set.

Graph parameters: as in v1, every kernel reads its job from a device-accessible parameter block written by the host before launch (`PassBlk`-style per instance and per sub-graph). No `cudaGraphExecKernelNodeSetParams`, no `cudaGraphExecUpdate` on the hot path. Grid sizes come from the class; kernels early-exit beyond the job's real size (v1 behaviour).

Not used: conditional graph nodes (CUDA 12.4+ feature, not needed: the host decides the entry point before launch) and device-side graph launch.

### 10.4 Memory layout and ownership

Same rules as section 3 (`nr_gpu_buf_alloc`, pre-fence only, `h2d_async`/`d2h_async` called unconditionally). New per-instance state that makes retries cheap: an instance is held for the whole decode call (first body + retries), not per body.

| buffer | owner | lifetime | DIRECT (GB10) | DISCRETE (sm_89) |
|---|---|---|---|---|
| IQ ring | radio | ring | read in place by S | H2D of the slot's sample span on the RT copy stream, once per (cell, slot) |
| slot grid rxF [cell][3 slots][4 ant][14][N] c16 | pipeline init | 3 slots | device scratch (no host mapping); host mirror written only on request (CSI/sensing consumers) | device; D2H only on request |
| est, nvar rows, chest post out | instance | decode call | device | device |
| geometry maps geo_y/geo_h, pilots, scrambling words, rdm/crc tables (parameter blocks) | instance | one launch | mapped, host writes, kernels read | pinned host + one H2D per launch (the 4 copies v1 makes in `enqueue`) |
| LLR int16 [G] | instance | decode call | device | device; D2H only if a CPU consumer asks (shadow check, CPU retry fallback) |
| LDPC in int8, snapshots | instance | one launch | device | device |
| result block (tb_ok, seg_ok[], TB bytes, EVM/SNR sums, nvar, LLR stats, post out, need_cpu) | instance | one launch | mapped, read in place after the event | one D2H (<= 40 KB) |

Removed vs v1 on DISCRETE: v1's unconditional D2H of `est` and `rxF` (4 x 14 x 4096 x 4 B = 917 KB each per pass) and of the whole LLR buffer [CODE-READ nr_gpu_pass_graph.cu enqueue()]: they become request-only.
Instances: `NR_GPU_PASS_RING` = 4 today; v2 needs one per PDSCH consumer (5 in t26gE: cores 7,8,9,3,4) plus one for the async recovery worker: 6. Device memory per instance [ESTIMATE]: rxF is shared (slot grid), so an instance is est 4 x 4 x 14 x 4096 x 4 B = 3.7 MB + LLR 2 x 1.1 MB + LDPC in/snapshots ~1 MB + scratch ~4 MB = ~10 MB; 6 instances ~60 MB plus graphs. Budget cap for sens6: 1 GB total (8 GB card in the 2026-09-30 snapshot, 12 GB if it is the 4070; open question 10.12).

### 10.5 Readback contract

- The host waits once per launch (end of L) with `cudaEventQuery` + `nr_gpu_poll_relax_pass` (existing), never `cudaStreamSynchronize`.
- What the host reads after the event: the result block (above). DIRECT: in place, after the event (the event orders the device writes). DISCRETE: the D2H of the result block is the last node of L, so the same event covers it.
- What the host does after the read: commit (TB to MAC, EVMGRANT fields, solver feedback, HARQ bookkeeping) — control only. The CPU never re-computes a stage the GPU ran.
- Soft bits (LLR) and the slot grid are read only when a consumer flags it before launch: shadow check (every ISAC_GPU_PASS_CHECK-th grant), a CPU fallback retry, CSI/sensing consumers of rxdataF. On DIRECT they are readable in place after the event at no cost (mapped); on DISCRETE a flagged D2H node is appended.
- `need_cpu` (zero syndrome with failing CRC, OAI would iterate further): the only case where the CPU LDPC decoder runs a block again; counted. [MEASURED v1: need_cpu = 0 in all replay and OTA runs.]

### 10.6 Error and fallback path

| condition | when detected | action | counter |
|---|---|---|---|
| unseen key, no free instance, unsupported variant (gate list of nr_gpu_pass_dl.h: segments > 16, DM-RS type 2, PT-RS, HARQ combining, Z % 4, C > 32, ...) | before launch | stage path for the whole call | `fb_unseen/fb_busy/fb_unsup` + gate id |
| CUDA error at launch | launch | stage path for this grant; instance marked bad until a self-test passes | `fb_err` |
| graph not done after the RT deadline (`nr_gpu_rt_deadline_us`) | wait | keep waiting (the GPU work cannot be cancelled); count | `late` |
| CRC fail | result | retries go through the v2 entry points (10.7), never the stage path | `tb_fail` |
| need_cpu | result | CPU LDPC on the device LLRs (read in place / D2H), counted | `need_cpu` |
| shadow mismatch (every N-th grant also runs the stage path and compares LLR hash, TB, seg_ok, EVM fields) | after commit | log `GPUGRAPH MISMATCH`, disable v2 for the run, stage path from then on | `shadow_bad` |

Debugging: `ISAC_PDSCH_GPU_GRAPH_NOGRAPH=1` runs the same kernels stream-ordered without capture (the sub-graph builders are plain functions that enqueue on a stream; capture wraps them), so compute-sanitizer and nsys see ordinary kernels; NVTX ranges per sub-graph launch.

### 10.7 Probes, retries and trial decodes on the same graphs

Every body of a call keeps the instance (est, LLR, nvar on the device). The body declares which inputs changed; the host launches from the first affected sub-graph:

[CODE-READ: arms in passive_decode_retry_impl, D:6402-6760; async worker nr_pdsch_passive_queue.c:2594-2708]

| body | what changes | v2 entry | what is reused on the device |
|---|---|---|---|
| first body | - | S (shared) wait + C + D + L | - |
| TDRA S toggle (arm & 2), second TDA | start symbol S, so data REs and G | D + L | slot grid (S covers all 14 symbols), est |
| CSI retry, CSI population (cv 1, 2), static CSI, PDCCH RM | RE mask, G | D + L | slot grid, est |
| DM-RS CDM (arm & 4) | n_dmrs_cdm_groups, so beta_dmrs | C + D + L | slot grid |
| wide MRC (arm & 8) | demod combining | D + L | slot grid, est |
| TINTERP (arm & 16, off in t26gE) | chest interpolation | C + D + L | slot grid |
| RV retry (off) | LDPC parameters on the same LLRs | L | slot grid, est, LLR |
| async recovery worker (RECOVERY_ASYNC=1) | the arms above, later, on its own IQ snapshot | its own instance; S is re-run only when the slot grid ring no longer holds the slot (3-slot ring, 1.5 ms) | est and LLR are NOT shared with the consumer's instance (released at the end of the first call) |
| solver layout probe | layout hypothesis, CB0 only (`nb_segments_to_decode` = 1, horizon to CB0) | C + D + L on the low-priority twin stream, ncb_class 1 | slot grid |

The decode call holds one instance from the first body to the last retry, so est / LLR stay valid. The arms are not changed (same order, same stop rule), so the decoded set equals today's by construction; the unit test compares every arm's LLR and TB against the stage path.

Solver probes (layout hypotheses) are bodies with their own instance and the low-priority twin stream (ISAC_PDSCH_GPU_PRIO, nr_pdsch_gpu_prio.cuh); they launch C+D+L with the probe's ncb class (often 1: CB0 only), so a probe and a real decode use the same graphs. The slot FEP S is shared, so a probe never re-runs FEP.

### 10.8 Per-grant GPU time

Per-grant GPU time of the v2 chain (critical path, single grant, no contention). Base: v1 graph kernels [MEASURED above, b / tim]; v2 deltas [ESTIMATE].

| item | p50 b / tim (us) | p99 b / tim (us) | source |
|---|---|---|---|
| v1 graph span | 409 / 459 | 965 / 852 | [MEASURED, 10.0] |
| - slot FEP moved to S (launched at slot read, before the grant is queued; a2e p50 380-584 us leaves it ample time) | -56 / -53 | -102 / -66 (tail mean) | [MEASURED kernel time, 10.0] |
| + k_llr_stats, k_brfo, k_tb_pack (one pass over G / rows) | +10 / +10 | +25 / +25 | [ESTIMATE: memory-bound passes of the size of k_unscr] |
| + 3 sub-graph launches instead of 1 (dependent launches on one stream) | +10 / +10 | +20 / +20 | [ESTIMATE] |
| **v2 chain C+D+L** | **~370 / ~425** | **~910 / ~830** | [ESTIMATE] |
| host before launch (job build, geometry, parameter blocks) | 65-108 -> 60 | 252-337 -> 150 | [MEASURED v1] -> [ESTIMATE: TB params and geometry no longer wait on the chest] |
| host after (commit) | 53 -> 40 | 619-704 -> 100 | [MEASURED v1] -> [ESTIMATE: no LLR copy-out / cont_fail memcpy, no old_first rerun] |
| **v2 body wall** | **~470 / ~525** | **~1150 / ~1080** | [ESTIMATE] |
| today's body wall (stage path, t26hE) | 564 / 589 | 2019 / 2043 | [MEASURED, 10.1] |

Retry body (arm that enters at D): ~ v2 chain minus C (k_ls + k_delay_exact + k_fir + k_chest_post: 43+14+18+~7 = ~80 us b mean) -> ~290 / ~340 us p50 [ESTIMATE]. Today a retry body costs a full body.

From grant to tot p99. Today the tot tail is a2e + qw + pre + dec; in the tail mean, dec (= the first body; retries run in the async worker) is 47-50 % (b) and 60-70 % (tim) of tot [MEASURED, 10.0 and t26hE traces: b dec 1690-1911 of 3597-3674 us; tim 2110-2481 of 3184-3497 us]. pre and qw are mostly earlier grants' decodes (P99-BOTTLENECK: pre of later slot-group members correlates 0.99 with earlier members' dec), so they shrink with dec. a2e (PDCCH, stays on the CPU) does not.

| cell / source | today tot p99 | model | v2 tot p99 [ESTIMATE] |
|---|---|---|---|
| b replay | 3.0-3.5 ms (pass / t26gE) | tail: a2e 1.0 + (qw + pre 0.8) x 0.55 + dec 1.9 -> 1.15 | ~2.3 ms |
| tim replay | 2.3-2.5 ms | a2e 0.6 + (qw + pre 0.45) x 0.55 + dec 2.1 -> 1.1 | ~1.7 ms |
| T on air, daytime (GHC / DAYTIME, t26gE) | 2.9-4.0 ms | same ratio as tim replay (x 0.68) | ~2.0-2.7 ms: **meets < 3 ms** |
| V on air, daytime | 4.2-7.7 ms | same ratio as b replay (x 0.75) | ~3.2-5.8 ms: **does not meet < 3 ms by this plan alone** |

p50: today 1.0-1.6 ms on air (dec p50 ~0.55-0.64 ms in replay). With the v2 body p50 ~0.47-0.53 ms, p50 drops by ~0.1 ms [ESTIMATE]; p50 is dominated by a2e (0.38-0.58 ms replay p50), which this plan does not touch.

What closes V: (1) the 12-13 % of grants v1 left on the stage path are 64-82 % of its tail [MEASURED 10.0]; v2 must cover probe, nomem (recovery worker), seg > 16 and unseen classes (Tasks 4, 6, 7), otherwise the estimate above is not reached; (2) on air V has more CB >= 16 grants than replay (GPU-CONTENTION caveat), where k_ldpc4_t is 115-219 us per tail body and k_tail<double> scales with Qm (P-FUSE: 363-666 us at 273 x 4): kernel work, not host gaps, then dominates and needs the LDPC cluster-2/4 (rank 14) or FP32 demod decision (open question); (3) a2e p99 on V (not measured on air for t26gE). The plan's final gate reports these three separately so the next step is chosen by measurement.

### 10.9 Multi-cell batching hook

- `cell` is a field of every parameter block (slot grid index, delay table, frame params); single-cell code passes 0 and kernels bounds-check it.
- S is keyed by (cell, slot); one launch can take a cell list (grid.z = cell) once the 200 MHz channelizer exists: same kernel, more blocks.
- L's parameter block is an array of CB jobs, each with its own (instance, LLR offset): the next step after this plan is a cross-grant L launch (jobs of several grants, of either cell, with the same LDPC family) without a new graph key. Nothing in v2 assumes one grant per L launch except the `ncb_class` sizing, which becomes "CBs in the batch".
- One process, one CUDA context, the existing RT green partition; no per-cell stream or context (MPCI-OTA: a second context costs 4-9x).

### 10.10 sm_89 portability

| feature | GB10 sm_121 | sm_89 | plan |
|---|---|---|---|
| CUDA graphs: capture, instantiate, launch | yes | yes (all since CUDA 10) | same code |
| conditional nodes, device graph launch | not used | not used | - |
| mapped zero-copy (DIRECT) | yes | no (DISCRETE) | `nr_gpu_model()`; `ISAC_GPU_FORCE_DISCRETE=1` test on the DGX for every task |
| green contexts / SM partition | used (RT 40 / disc 8 SMs) | UNVERIFIED | priority streams fallback (exists, nr_gpu_green.h) |
| LDPC cluster kernels | yes (cc >= 9) | no: `decide_nc()` forces nc | the L key carries the family; sm_89 warms only nc |
| FP64 demod (bit-exact k_tail<double>) | 2x slower than FP32 [MEASURED P-FUSE bench, 273x4] | FP64 at 1/64 rate on consumer Ada [vendor spec] | bit-exact requires FP64: risk 10.11-R5; sens6 timing measured at the phase gate |
| CUDA 13 / driver 580 | yes | driver 580 on sens6 (snapshot) | build with `CMAKE_CUDA_ARCHITECTURES "89;121"`, ctest `test_nr_gpu_pass` in DIRECT and forced DISCRETE on the DGX; sens6 run by the orchestrator |

### 10.11 Risks

| id | risk | evidence | mitigation |
|---|---|---|---|
| R1 | graph instantiation cost / warm-up | v1: 8.6 s, 5.8 GB for 1708 classes x 4 [MEASURED] | sub-graph keys (sum, not product); warm set from the observed class census; budget check at init (refuse v2 above the cap, stage path stays) |
| R2 | shape explosion (segmented grants, new nrb classes, multi-cell) | v1 needed 1400 segmented classes | nseg only in C's key; unseen = stage path + logged key; class census tool in Task 1 |
| R3 | graphs on sm_89 | graph API is arch-independent; green contexts unverified | no conditional nodes; priority-stream fallback; forced-DISCRETE test every task |
| R4 | debugging inside graphs | captured kernels are opaque to printf ordering and hard to bisect | NOGRAPH mode (10.6), per-sub-graph NVTX, shadow check |
| R5 | FP64 bit-exactness vs sm_89 speed | k_tail<8,double> 431 us/call at 273x4 on GB10 [MEASURED P-FUSE] | out of scope here; the FP32 instantiation exists (NR_GPU_PASS_FP32_DEMOD) and is a separate, non-bit-exact decision (open question) |
| R6 | LDPC CRC equivalence | rare GPU-vs-CPU LDPC disagreement pre-exists (1 in ~10000 TBs, HOST-GAP orchestrator check); OAI's stop rule (first passing snapshot) is emulated by k_pick | the unit test compares against the stage path's GPU LDPC (same kernel), not the CPU decoder; CPU-decoder agreement stays a separate counter; need_cpu path kept |
| R7 | instance held across retries starves other consumers | 4 instances today, 5 consumers + recovery worker | 6 instances; busy -> stage path (counted); Task 7 replay gate checks fb_busy < 1 % |
| R8 | the gain does not show at p99 on air (as with v1) | v1 OTA: >5 ms tail 2-9x | per-task replay gates on both captures with tot p99 and first-body p99 by CB class; on-air A/B vs t26gE only at the end |

### 10.12 Open questions (user / orchestrator)
1. V on air: the estimate (10.8) puts V at ~3.2-5.8 ms after this plan. Is a follow-up on kernel time (LDPC cluster-2/4, or the non-bit-exact FP32 demod instantiation that exists today) acceptable, given the bit-exact rule? Without one of them, V p99 < 3 ms depends on how many CB >= 16 grants the cell carries.
2. sens6 GPU: the 2026-09-30 snapshot says RTX 4060 Ti 8 GB, the user's belief is RTX 4070 12 GB. The v2 device budget is capped at 1 GB; v1 needed 5.7-6.8 GB and would not fit next to the other GPU users on 8 GB.
3. FP64 on sm_89: bit-exact demod keeps k_tail<double>. Consumer Ada runs FP64 at 1/64 of FP32. Its time on sens6 is unknown until the sens6 gate. Accept "bit-exact on sm_89 even if slower", or allow FP32 there?
4. a2e (PDCCH on the CPU by decision) is 0.6-1.0 ms of the tail mean in replay. After v2 it becomes the largest single tail component on b. Is the PDCCH-on-CPU decision final for the p99 goal?
5. Async recovery: v2 keeps the arms and order unchanged. Running independent arms concurrently (D+L for several arms at once) would cut recovery latency but costs GPU time. Out of scope unless asked.
