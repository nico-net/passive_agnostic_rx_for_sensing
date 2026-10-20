# GPU on-air timeline, 2026-10-16

Sonnet agent (stall plan Task 4 step 2); saved by the orchestrator. Traces: ota_runs/ab_nV_1_210903 (V), ab_nT_1_211307 (T), env t26aXE, nsys -t cuda --delay 40 -d 90, n=1 per cell, evening load.

**Summary**
1. Both on-air nsys traces are representative at p50 (V 0.95x, T 1.05x of the plain-run median); p99 is 1.3-2x higher under nsys.
2. Stalls >= 50 ms on air still come from CSI-RS discovery (csirsDisc) kernels: 40 per 90 s (V, 33 clusters, up to 651 ms) and 17 per 90 s (T, 13 clusters, up to 204 ms), ~3.5-4x the replay rate. Blockers: csirsDisc sweep, segment, refine_kernel, finish (not NID).
3. Ranking: (a) hardware-queue head-of-line blocking strongly supported; (c) copy-engine blocking not separable from (a); (b) cudaMalloc/cudaFree: no such call in either trace.
4. Chunking bounds kernel length, not the shared queue; the Task 3 spec update is proposed in section 6.
5. n=1 per cell; V and T never pooled.

## 1. nsys overhead check (DL tot over real grants, us)
| run | n_real | p50 | p99 | p99.9 | drops % |
|---|---|---|---|---|---|
| V nsys | 30077 | 1885 | 28155 | 53160 | 4.8 |
| V plain cVX1 | 26423 | 1435 | 13866 | 45063 | 0.4 |
| V plain cVX2 | 13552 | 1994 | 15800 | 41202 | 1.7 |
| V plain cVX3 | 27660 | 2115 | 17492 | 56471 | 4.2 |
| T nsys | 24153 | 1316 | 8807 | 22775 | 0.5 |
| T plain cTX1 | 26671 | 1259 | 5087 | 9267 | 0.1 |
| T plain cTX2 | 31595 | 1209 | 4893 | 14451 | 0.1 |
| T plain cTX3 | 49843 | 1260 | 6650 | 13467 | 0.4 |
Plain median p50 V 1994, T 1259; nsys passes the 1.2x rule. Stall counts are as measured under nsys.

## 2. GPU busy share per launching thread class (window 89.9 s)
| class | V % | T % |
|---|---|---|
| csirsDisc | 54.7 | 64.3 |
| passivePdsch | 34.7 (busy 19.1) | 20.8 (busy 12.8) |
| NID (nr-uesoftmodem) | 15.4 | 20.7 |
| pdschRecov | 14.0 | 1.1 |
| passivePusch | 11.8 | 4.9 |
| GPU busy union | 75.7 | 78.0 |
Replay union was 59.6 (V) / 49.9 (T). Launch->start p50/p99 us: passivePdsch V 7.6/4155 (replay 1.7 ms p99), T 1.1/697. PDSCH burst wall p50/p99 V 471/10840 us (kernel-busy 277/7034), T 502/4839 (259/3177).

## 3. Longest kernel per class (max / p99 ms, V | T)
refine_kernel 688/420 | 14.4; sweep 292/1.0 | 235/1.75; segment 107/103 | 117/114; finish 71 | 51; lattice_screen 56/38 | 68/63; normalize 34 | 42; energies 5.6 | 6.3; NID sweep 16.5/6.6 | 14.1/6.8. Kernels >= 50 ms V/T: refine 15/0, sweep 73/35, segment 68/125, finish 12/3, lattice 2/5. Only a minority of long kernels stall a victim (V: 7/18 refine, 15/99 sweep, 10/209 segment >= 10 ms). T on air blocks on sweep/segment (replay T was refine).

## 4. Stalls >= 50 ms
V 40 (48 incl. pdschRecov; 33 clusters; 51/165/651 ms min/median/max); T 17 (13 clusters; 55/69/204 ms). Unexplained launch->start >= 10/20/100/200 ms: V 163/68/34/15, T 44/24/3/1. Victims: Pdsch0-4, pdschRecov, Pusch0/1 (V); mostly Pdsch0-4 (T). Stalled kernels V: k_evm_zero 14, k_ldpc4_t 11, k_fep_exact 7, k_ul_fep 4. The stalled kernel starts within -0.1..+1 ms of the end of a background kernel in 88 % (V) / 82 % (T) of cases (last BG end before start). Blocking streams: V s1063 (csirsDisc, green ctx 7) 39/48; stalled streams are in RT green contexts 2/3/4 (different SM partitions).

## 5. Answers
- csirsDisc still the cause on air: yes [MEASURED, V and T, n=1 each].
- (a) HoL: during the 652 ms V stall 3143 other RT kernels started (1094 in the stalled consumer's own green context); the same PDSCH streams recur as victims behind the same background stream -> fixed stream-to-hardware-queue mapping (CUDA_DEVICE_MAX_CONNECTIONS default 8, 24+ RT + 3 BG streams) [INFERRED; the map is not readable from nsys].
- (c) Copies: RT DtoH copies stall in the same events (V 74 >= 50 ms, T 28) but share the stalled stream; csirsDisc copies stay fast; only 2/48 (V) and 0/17 (T) stalled kernels had a same-stream memcpy during the wait. Co-symptom. ISAC_GPU_COPY_FENCE=1 untested.
- (b) No cudaMalloc/cudaFree/cudaMallocAsync/cudaMallocHost in either trace; driver cuMem* not exported. The CODE-READ fold_gpu candidate is not supported on air.

## 6. Task 3 spec update proposal (not yet applied)
1. Chunking must cover sweep, segment, refine_kernel, finish and lattice_screen; target ~1-2 ms per chunk. NID kernels up to 16.5 ms: cap them if the target is below 10 ms.
2. Queue depth: enqueue chunk k+1 only after chunk k-1 completed (at most two queued).
3. HoL test arm: CUDA_DEVICE_MAX_CONNECTIONS=32 with and without chunking, judged on on-air stall count per 90 s (baseline V 40, T 17) and p99.9, not on replay.
4. Success metric: RT kernels with unexplained launch->start >= 10 ms (baseline V 163, T 44).
5. Drop cudaMalloc/cudaFree as a candidate; copy fence optional.

## Not checked
n=1 per cell, one 90 s window; nsys p99 overhead 1.3-2x; queue map inferred; no on-air test of MAX_CONNECTIONS or chunking; UL not analysed; mempool events not exported. Agent scratch SQL scripts not in the repo.
