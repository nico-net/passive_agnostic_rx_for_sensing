# Target GPU pipeline: 500 us budget, multi-cell-ready GPU API, Phase 1 port order (2026-10-16)

Phase 0 Task 6. Design only: no code, no build, no OTA. Inputs: roadmap `docs/superpowers/plans/2026-10-16-rx-goals-roadmap.md` and the Phase 0 reports in `docs/superpowers/reports/csd/` (GPU-INVENTORY, SCORECARD-BASELINE, DLCRC-ROOTCAUSE, ROOTCAUSE, ALLON-OTA, AB-SPREAD-PIN, PDCCH-PARBANKS-OTA, LDPC-PERCB, MPCI-OTA, ULCAUSE; all `-20261016`). Code cites are `file:line` on csd/int16 and are `[CODE-READ]`. Numbers keep the label of their source report; V = Vodafone 3680.01 MHz, T = TIM 3759.99 MHz, never pooled.

**Task 4 (GPU stall trace) is NOT done.** Everything that depends on it is marked `TO BE FIXED BY TASK 4` with a provisional value and the reason. They are: the maximum length X of a background kernel (4.2), the priority rules (4.3), and the claim that the RT/disc SM split is sufficient (4.1). Phase 1 plan must re-read these three after Task 4.

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
