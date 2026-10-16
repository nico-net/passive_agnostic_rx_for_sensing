# Root cause of DL p99 (single PCI) — per-grant stage timing + causal OTA arms, 2026-10-16 11:48-13:20, daytime

Binary csd/int16 a25172d592 (stg= per-grant stage split, ISAC_E2E_TRACE=1). Label: [MEASURED, DGX spark-74c3 + X410, daytime OTA 2026-10-16, per run].
Runs: s* (11:48-12:13: t26aE 5 consumers vs t26a1E 1 consumer, n=2 per cell), q* (12:15-12:55: t26aE n=3 per cell; t26aQE INVALID: ISAC_PDCCH_NID_GPU=0 stops USS DCI discovery, ~1000 grants/run),
u* (12:52-13:20: t26aUE = UL FEP/LDPC off the GPU, n=3 per cell). Scripts: tests/passive_rx/stage_split.py; orchestrator scratch ldpcg.py / p99src.py (bucket and p99 attribution).

## 1. What the grants above p99 wait for (largest single component per grant, % of the p99 set)
| run | tot p99 us | qwait | pre (same-slot serial) | ldpc GPU | CPU chest fallback (>10 ms) | body unaccounted | a2e |
|---|---|---|---|---|---|---|---|
| qTaE1/2/3 | 12789/10221/10435 | 47/53/32 | 4/1/2 | 9/14/26 | 25/14/20 | 3/5/4 | 1/7/3 |
| qVaE1/2/3 | 12665/10991/15430 | 44/38/64 | 18/9/12 | 3/8/3 | 7/8/2 | 17/17/11 | 1/5/0 |
| sTaE1/2 (5 cons) | 13293/12145 | 32/51 | 8/3 | 30/32 | 13/0 | 5/7 | 2/0 |
| sTa1E1/2 (1 cons) | 14803/15144 | 55/73 | 11/7 | 13/8 | 16/9 | 0/1 | 0/0 |
| sVaE1/2 (5 cons) | 11231/13912 | 54/26 | 11/26 | 5/6 | 6/2 | 10/25 | 2/1 |
| sVa1E1/2 (1 cons) | 15872/23340 | 59/7 | 27/52 | 6/30 | 4/0 | 0/2 | 0/0 |
| uTaUE1/2/3 (UL off GPU) | 8461/7345/11207 | 43/27/78 | 10/18/5 | 12/15/4 | 11/8/5 | 2/3/2 | 11/19/1 |
| uVaUE1/2/3 (UL off GPU) | 10659/13231/17101 | 51/31/37 | 12/13/11 | 5/12/21 | 9/12/13 | 8/16/5 | 2/1/0 |
40-94 % of p99 grants belong to slots with >= 8-CB grants.

## 2. Why bodies of large grants are long (per code-block bucket, medians; q*aE runs)
- V, cb 16-23: first body 2.7-4.4 ms; LDPC wall 1.4-1.8 ms = GPU wait 0.8-1.1 ms + CPU prep 0.26-0.33 ms; demod 0.2-0.26; chest 0.15. cb 24+: first 3.0-4.3 ms, LDPC 1.4-2.0 ms (GPU wait 0.8-1.2).
- The GPU wait matches the bench cost of the 8-SM cluster LDPC kernel in the 40-SM RT partition (LDPC-PERCB-20261016.md: C=16 646 us, C=24 937 us, C=32 1230 us): an intrinsic kernel-geometry cost, not contention.
- Within the same bucket p99 is 3-5x p50 (LDPC GPU wait p99 2.2-5.6 ms); CPU chest fallback (GPU chest rows > 2048) puts chest p99 at 25-66 ms in every cb >= 16 bucket.

## 3. Causal tests
- 1 consumer instead of 5 (s*a1E): bodies of the same size are NOT faster (V cb 24+ LDPC 2.4-4.6 ms vs 1.8-1.9 ms with 5) -> consumers do not slow each other.
- UL off the GPU (u*): no change per bucket (V cb 16-23 LDPC GPU wait 761-926 vs 787-1109 us) -> UL GPU work is not the competitor.
- Quiet GPU (NID sweep off too): invalid (traffic lost), but small grants there: LDPC GPU wait p50 31 vs 75 us, first p99 ~1.0 vs 1.7-4.3 ms [confounded: 50-70x fewer grants].

## 4. Root cause (chain)
1. Large grants (>= 8 CBs, > 150 PRB or Nl >= 3) have a 2-6 ms first body, caused by: cluster LDPC kernel geometry (3 CBs at a time; ~1 ms GPU wait at 16-32 CBs), serial CPU LDPC prep (0.2-0.55 ms median, 1-4 ms p99), and the GPU chest row cap (CPU fallback 25-66 ms).
2. The receiver then makes every other grant wait for those bodies: (a) slot-group serialisation — one consumer decodes all grants of a slot serially, WIDEST FIRST (nr_pdsch_passive_queue.c ~1500-1600, 1729), so small grants of the same slot always wait behind the heavy one ("pre", 5-52 % of p99 grants); (b) FIFO queue: heavy bodies occupy consumers in bursts and the queue waits (26-78 % of p99 grants) up to the 9 ms stale limit.
3. Ruled out as p99 drivers: consumer-to-consumer contention, UL GPU work, solver probes, page faults, poll granularity. Not settled: the share of PDCCH NID GPU sweep in GPU-wait tails (cannot be switched off without losing DCIs).
4. Floor: PDCCH arrival->enqueue a2e p50 ~0.6 ms, so end-to-end < 0.5 ms is not reachable with the current PDCCH scan even with an instant decode.

## 5. Fixes in order of p99 grants removed (not implemented)
1. Slot-group: decode small grants first / spread members over idle consumers (removes "pre"); heavy grants on their own lane so they cannot block the queue (qwait).
2. Heavy body: ISAC_GPU_LDPC_NC_MIN_CB=8 (848b05ba03, OTA-safe), parallel LDPC prep, GPU chest without the 2048-row cap (no CPU fallback).
3. Then PDCCH scan latency (a2e) for the sub-ms target.
