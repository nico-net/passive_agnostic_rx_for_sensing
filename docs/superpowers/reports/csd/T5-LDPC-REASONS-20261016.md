# T5: GPU LDPC fallback reasons + NC_MIN_CB=4 (Phase 1 Task 5, items NC, F), 2026-10-16

Sonnet agent; saved by the orchestrator. Commits 7f460c305b (reason codes, per-reason counters, tests), 57726e3c7c (log the first 8 param refusals per process). Defaults unchanged; logging only.
Labels: [MEASURED, DGX replay <cap>, t26aXE, TMO=90, n=2 per arm]; b = raw_pci415_12s_b (V), tim = raw_pci923_tim_12s_tile3 (T). Replay only, not pooled with OTA.

## Verdict
- DL GPU LDPC fallbacks have ONE cause: PARAM refusals of tiny BG2 code blocks (R=15, Z in {7,11,15}, K=10*Z = 70/110/150, K % 8 = 6) hitting the `(j.K & 7)` check: the kernel snapshot is byte-packed (K>>3), so they cannot come back bit-exact and go to the CPU. All other reasons (init, range, graph, stream, sentinel, other) are 0 in every run. Cheap on CPU; the run-to-run spread of the OTA counter is how many tiny TBs are granted.
- Not a config gap: Step 6 stops. Follow-up (needed by the all-GPU pipeline): pad the snapshot to (K+7)>>3 bytes and mask the tail bits, keeping CRC and bit-exactness.
- NC_MIN_CB=4: no visible effect in replay (b slower at CB 4-15, tim equal); the OTA weak positive is not reproduced here.

## Changes
enum nr_gpu_ldpc_err in nr_pdsch_gpu_ldpc.h; nr_pdsch_gpu_ldpc_run returns one value per path (all non-zero, callers' != 0 unchanged); host counts fb_reason[]; stats line prints `fallback_err[init,range,param,graph,stream,sentinel,cap,rt,busy,deadline]=[...] other=N`. UL path unchanged (already splits busy/over_cap/rt_defer/late/deadline/err in nr_pusch_gpu_ldpc_report). Tests: GRAPH_MISS (BG1 R15), RANGE (n=0), PARAM (K%8!=0) pass; `test_nr_pdsch_gpu_ldpc 2|nc|ul|mix` mismatches=0.

## Replay table
| cap | arm | run | DL tbs | DL err (all param) | UL tbs | UL over_cap/busy/deadline/err | UL cpu_refused | UL run_fail |
|---|---|---|---|---|---|---|---|---|
| b | NC8 | 1 | 10000 | 30 | 610 | 0/0/0/0 | 0 | 0 |
| b | NC8 | 2 | 12000 | 94 | 1495 | 0/2/1/0 | 0 | 3 |
| b | NC4 | 1 | 14000 | 137 | 1332 | 0/0/0/0 | 3 | 0 |
| b | NC4 | 2 | 14000 | 125 | 1761 | 0/0/0/0 | 3 | 0 |
| tim | NC8 | 1 | 8000 | 182 | 1493 | 107/0/0/0 | 326 | 107 |
| tim | NC8 | 2 | 8000 | 103 | 467 | 0/0/0/0 | 125 | 0 |
| tim | NC4 | 1 | 8000 | 124 | 2723 | 6/0/0/0 | 632 | 6 |
| tim | NC4 | 2 | 8000 | 155 | 38 | 0/0/0/0 | 32 | 0 |
DL err 0.3-2 % of TBs. tbs = last (rate-limited) stats print.

LDPC wall (EVMGRANT stg=ldpc, probe=0, includes queueing), us, p50/p99 runs 1, 2:
| cap | arm | CB 4-15 | CB 16+ (n 14-16 b, 55-70 tim) |
|---|---|---|---|
| b | NC8 | 431/1327, 442/2182 | 877/1565, 949/1204 |
| b | NC4 | 527/2395, 539/1211 | 956/3940, 825/3171 |
| tim | NC8 | 549/1168, 539/1190 | 769/2934, 770/3039 |
| tim | NC4 | 568/1306, 558/1425 | 738/3056, 762/1998 |
crc_q unchanged: b NC8 94.5/95.7, NC4 95.3/95.4; tim NC8 72.6/69.4, NC4 73.3/69.8. DL tot p99 dominated by single-run outliers (busy replay host); no ranking.

## UL
UL over_cap 0-107 (7 % of tbs in tim NC8 r1), busy 2, deadline 1, err 0. cpu_refused is the larger term (~20-80 % of tbs on tim, ~0 on b): host-side refusal, cause not split. Cap/deadline belong to the UL track.

## Notes
First replay batch used a stale binary; discarded (replays/old_t5_invalid/). Replay dirs replays/replay_t5_* (untracked). Analysis script in the session scratchpad (t5an.py).
