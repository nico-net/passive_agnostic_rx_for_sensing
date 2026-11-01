# fix1101/srsq -- why the SRS job queue was full and SRS never detected

Base: cand1101 (ac5c4ec2af). Evidence labels per PROJECT_MEMORY §0.1. Nothing was run on air or in a replay.

## Root cause (three layers)
1. **Pool lease per job, taken before the cheap decision** `[OFFLINE VERIFIED, DGX replay 2026-11-01 logs pair3/c*/rx.log]`.
   `srs_worker` did `nr_pool_site_float_acquire(NR_POOL_HOUSE)` around every `process_srs()`. The same logs print
   `SENSING: CORE_POOL long wait class=4 waited_ms=53..100` (class 4 = HOUSE; 40 such lines, the log is rate-limited at 40). While the worker waits for a
   token the 4-deep queue fills; `srs_us` (thread CPU) stays ~3 s per 100 s while `srs_jobs_dropped` climbs linearly (84 k at the end), i.e. the worker is
   blocked, not busy. Jobs that the idle backoff or the tracker would have skipped in microseconds waited for the lease like real scans.
2. **Drop counter inflated**: `sr.count==SRS_DEPTH` was tested before the "slot has UL symbols" test, and `nr_srs_passive_capture` is called every slot, so every
   DL slot arriving while the queue was full counted as a dropped job (cand1101 count 29-139 k per 100 s vs ~60 k UL-capable slots).
3. **Blind discovery aliased with the occasions**: the 1-in-`interval` backoff (interval up to 64 eligible slots, decays after 4 empty scans) can never put 4
   detections at >= 75 % occasion coverage (tracker rule) on an SRS with period > ~16 eligible slots; every scan costs 24 FFTs + a frequency-domain pregate
   (budget 5 % of a core => ~40 scans/s), so even a stationary SRS was seen at random 1/6 of its occasions.

Submitter: `nr_srs_passive_capture` (nr-ue.c:822, once per slot, every slot) queues one job per slot whose previous slot has UL symbols 8..13 (TDD table):
one job per eligible slot, not per symbol or antenna, no duplicates (the 6 x 4-antenna windows live inside the job). Consumer: one `srs_worker` per cell
(nr_core_plan, polls the queue with a 1 ms nanosleep when empty).

## Changes (nr_srs_passive.c, nr_srs_blind.{c,h})
- `nr_srs_td_comb_gate` (new): time-domain comb detector. Every comb 2/4/8 signal is periodic with N/2, so one lag-N/2 correlation over the central N/4 samples of each
  (antenna, symbol) window, DC removed, sum of normalised |rho|^2 ~ Gamma(K,1) under noise, gate at K + 8 sqrt(K). No FFT, no timing search, no lease.
- `process_srs`: gate on **every** eligible slot when the search is undirected (backoff no longer applied; the occasions of any period are all examined); FFT scan (all
  nine window offsets until found) only when the gate opens; lease taken lazily just before the first FFT, kept across a burst of queued jobs and released when the
  queue is empty (`nr_pool_site_float_yield` between jobs). Predicted occasions transform only the symbols of the tracked resources (2 instead of 6).
- `nr_srs_passive_capture`: mask test before the full-queue test (only eligible slots can be dropped); `SRS_DEPTH` 4 -> 32 (eligible slots <= 0.8/ms => 40 ms of lease wait).
- State is still per cell (same `NR_CELL_STATIC_HEAP` object, no new file-scope statics); no flags, no cell constants.

## Tests (written first; red on cand1101, green now) `[OFFLINE VERIFIED, DGX aarch64 unit]`
`tests/nr_srs_production_path_test.cc` (new target `test_nr_srs_production_path`, cmake_targets/mpci_cloud.cmake): TS 38.211 SRS (group 17, cs 3, 8 RB, 2 symbols,
generated with the production `generate_srs_nr`) written into the 4-antenna sample ring of a 4096-point TDD UE (DDDSU, special slots) at the 0.5 ms slot rate, pushed through
`nr_srs_passive_capture -> queue -> worker -> process_srs -> tracker -> nr_srs_export`; the pool-lease wait is modelled by a 10 ms stall in `nr_pool_site_float_acquire` (--wrap).
| case | cand1101 | now |
|---|---|---|
| comb 2, period 20, 10 ms lease stall (150 periods) | 5 records, 0 confirmed, 2692 dropped | 82 records, 79 confirmed, 0 dropped |
| comb 4 offset 1, period 40, 10 ms stall | 0 records, 2681 dropped | 56 records, 53 confirmed, 0 dropped |
| comb 2, period 20, no stall | 8 records, 0 confirmed (budget/backoff) | 83 records, 80 confirmed |
| noise only, 10 ms stall | 2671 dropped, 149 lease waits | 0 dropped, 0 lease waits |
`nr_srs_blind_test.cc`: TdGate* (600 noise/DC windows stay closed; comb 2/4/8 open on full and special-slot masks; sensitivity: comb-4 wideband power per sample -10.5 dB opens
100/100, -14 dB 39/100, -16.5 dB 0/100 -- for a narrow SRS of b RB in R RB divide by R/b, i.e. an 8-RB SRS of a 217-RB cell needs about +0..+4 dB per RE).

## Measured per-job cost (4 ant, N = 4096, aarch64, `flock -x /tmp/td_measure.lock`) `[OFFLINE VERIFIED, DGX aarch64 unit]`
- Before: undirected scan = 24 FFTs (265 us at one offset) + 0.9 MB memset + frequency-domain pregate (~1.2 ms total per scan job, run 1 in `interval` slots) behind a lease wait.
- After: idle eligible slot = gate only, 15.8 us hot / 50 us in situ (cold buffer, production-path idle test: 60.7 ms per 1200 slots) at 800 eligible slots/s = ~4 % of a core
  (inside the 5 % budget); tracked occasion = 2 symbols x 4 ant = 90 us of FFT; full scan only on gate-open.

## Not done / not verified
- Nothing replayed or on air. The gate's false-open rate on real UL traffic is unknown (PUSCH DM-RS type 1 is comb 2 and opens it like it opened the old pregate; cost then = scans, bounded by the
  5 % budget). Threshold K + 8 sqrt(K) is from the noise statistics, not fitted to any cell `[HYPOTHESIS]` on air.
- FFT sharing with the UL/PUSCH path not done (the PUSCH path transforms only granted slots; the SRS symbols are not computed there).
- Queue memory: 32 jobs x 4 ant x 6 symbols x (N+800) x 4 B = 15 MB per cell at N = 4096.
- Windows without SRS but with 100 % `budget_skip` (5 % cap) still lose occasions: counters below show it.

## Counters to read on air
`srs_jobs_dropped` (now only real queue overflows of eligible slots; expect ~0), `srs_pregate_skip` (= gate-closed slots), `srs_backoff_skip` (= 0 by design), `srs_budget_skip`,
`srs_us`, `periodic_srs_{predicted,hit,miss,confirmed}`, `sense_cfr_exported_srs`; and `CORE_POOL long wait class=4` lines (the lease wait itself is unchanged and now only hits real scans).


## Round 3 (2026-11-01 night): real-capture physics check and live chain on real slots
Tools (read-only on the captures): `tools/srs/srs_physics_check.py`, `srs_gate_stats.py`, `srs_null_stat.py`. Test: `test_nr_srs_real_inject --capture <dir> --slot0 <sample>` (skips without arguments).
Findings `[OFFLINE VERIFIED, DGX aarch64, captures 2026-10-24]`:
- TDD is DDDDDDDSUU with a 5 ms period; the uplink of raw_pci415_day_4s is at the noise floor (in-cell power over empty spectrum -0.1 dB median, p95 +0.3 dB over 1584 last-6 symbols);
  no comb-2/4 RB run >= 4 RB; steady 4.4 dB elevated wideband blocks at symbols 0,1,8,9 of slots 8,9 (non-comb, every period: PUCCH-like).
- The old gate statistic (white-noise Gamma) was 1.3-2.6x its null on this data (coloured noise, narrowband PUCCH/PUSCH): that is why it opened on 24-30 % of the slots. Replaced by a self-normalised statistic
  (reference correlations at lags that carry no comb); on the captures the per-symbol excess (sum-K)/sqrt(K) has median 0.1, p99 4, max 8.8 on pci415 (gate level 9: 0 opens in 4 s), i.e. the null.
  pci923 shows 44 opens in 4 s concentrated on slot 7 symbol 11 (excess ~20) and raw_pci415_12s_b shows 1639 symbols over 9 (excess up to 39, symbol 10): real comb-like UL (or DL tail) content, not SRS-verified.
- Bug found by injecting a TS 38.211 SRS (production generator) into the real slots: (1) the strict RB-local comb detector needs ~+8 dB per RE and reported 217 RB for a 216 RB SRS (illegal width);
  (2) the sequence search used a fixed score floor of 0.65 (needs ~+3 dB per RE on every antenna); (3) its lag-1 phase-slope estimate accumulates ~16 rad of ramp over 1300 REs at 10 dB, so the coherent score
  collapsed to 0.25 where 0.83 is available. Fixed: statistical comb detector over legal m_SRS widths after a gate open, Beta(1,n-1)-derived threshold (1e-6 union), multi-lag unwrapped slope.
- Live chain on real pci415 slots plus injected SRS (one SRS every 10-20 ms, `[OFFLINE VERIFIED]`): exported and confirmed from 0 dB per RE for 96/216 RB comb 2, from +5 dB for 48 RB and 216 RB comb 4;
  exported but not confirmed at -5 dB (216 RB comb 2: 7 records); a 24 RB comb-4 SRS (2.8 % of the band power) is below the time-domain gate at +5 dB. srs_jobs_dropped = 0. The capture alone: 0 gate opens, 16 ms CPU per 0.8 s (pci415).
Limits: the real uplink of these captures contains no detectable SRS at the gate's sensitivity (hence no real SRS record can be expected from them); multi-port SRS and SRS frequency hopping were not injected.

## Round 4: srs_jobs_dropped in the clean replay (597-1382 / 21-35k / 62-131k) `[HYPOTHESIS on air, OFFLINE VERIFIED reproduction]`
The counter has five increment sites; after round 3 the queue-full and lease paths are not the live ones (the gate is quiet, scans take the lease only for FFTs). The remaining ones are the event posts
(`post_event`, PDCCH / PUSCH monitor threads): a try-lock miss or a full 64-entry ring counted as a dropped job. The slot thread's `nr_srs_passive_capture` held `srs_lock` during the whole 470 KB
sample copy (~50-100 us, 800 times/s) so a few % of all posts met the held lock, and the ring was only drained when an uplink slot's job ran. Fix: copy outside the lock (the slot at `head`
is private until published), blocking lock for the microsecond critical sections, events also drained on every 1 ms idle poll of the worker. Test `EventTrafficDoesNotCountAsDroppedJobs`
(two threads posting ~7k events/s during captures): 170 drops (1.3 %) on 23bdc92123, 0 now. The error counters are not split by cause on air; if drops persist, the ring-full case (> 64 events per job interval) is the remaining one.
