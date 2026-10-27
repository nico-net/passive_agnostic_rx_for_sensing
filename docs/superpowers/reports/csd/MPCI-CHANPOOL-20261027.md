# MPCI-CHANPOOL (2026-10-27): the CPU channelizer leases its workers from the core pool

Branch `mpci/c-chanpool` (base mpci-main @ 1a78e0f001). NO FLAGS; N=1 (no channelizer, no feed) unchanged.

## What was done
1. **CHAN class in `nr_core_pool`** (`NR_POOL_CHAN`, last index, most urgent: served before SCAN/DECODE/RETRY/DISC/HOUSE, never promoted, never yields).
   - Bound: at most `chan_cap(N) = max(1, N-2)` leases; decode/scan reserve keep the rest.
   - Demand: `nr_pool_set_chan_demand(p, tokens)` (tokens = threads from `iq_chan_threads_needed` on the measured work per ms of signal). Reserved
     `r = min(demand, N-2)` (N > 2). Every other class works on `N-r` tokens: DL N'-1, scan reserve, low N'-2, per-cell DL share `max(1,N'-1-(A-1))`, and
     aged (promoted) waiters may not take the reserved tokens CHAN is not holding. Demand 0: every formula and the stats line are the pre-CHAN ones.
   - A CHAN lease charges no cell (no usage, no activity, no per-cell stats); its demand is taken off N for the per-cell fair share.
   - Core choice: the `r` reserved tokens are the fastest ones (pool `cpu_capacity`, then lowest index); CHAN is placed only on them, every other class
     only off them (fallback to any free token). On a hybrid host (GB10) the channelizer therefore sits on the fast cores, derived from topology.
   - Sticky reserved core: a CHAN worker keeps its core between blocks (release does not go home), because a migrating `sched_setaffinity` costs about
     110 us per call on the DGX [Bench, DGX GB10, shared host, n=5000, `lat` micro-bench: pin acquire p50 110 us / release p50 115 us, no-pin 0.1 us], i.e.
     ~220 us of a 267 us block period. A lease of a reserved token is mutex + counters only.
   - Site wrappers: `nr_pool_site_chan_acquire(blocking)`, `_release`, `_set_demand`, `_max`.
2. **iq_chan worker leases** (hook interface, `iq_chan.h`; no link on the pool): `iq_chan_set_lease()`, `iq_chan_lease_acquire/release/set_demand/max_tokens`.
   CPU backend: worker 0 (the caller of `run`) takes a blocking CHAN lease before it publishes a block and keeps it until all claimed jobs are done (the
   wait is for leased workers, not for input); helpers take a non-blocking lease only for a block that still has unclaimed jobs, denied = skip the block;
   all leases are released before the wait for the next block. With hooks, idle helpers yield 100 times instead of 4000 (the wait runs on the home lane, not a core of its own).
   `iq_chan_start`: ceiling for k is the pool's CHAN bound (not a cpu list), benches run under the same leases, the final k is registered as demand
   (`demand(1)` before the 1-thread bench, `demand(ceil_k)` before the k-thread benches, `demand(k)` at the end); `iq_chan_stop` clears it.
   Stats: `lease_blocks`, `lease_denied`, `lease_wait_ns`, `lease_wait_max_ns` (MPCI_FEED line gets ` chan_lease=<n>/denied=<n>/wait_max_us=<n>` only when leases ran).
3. **Feed reader on the layout's I/O role** (`nr_core_layout`): `nr_cl_build_nf(.., feed, ..)` / `nr_core_layout_init_nf` plan ONE MORE real-time reader
   after the cells' readers (same ranking, same W; pool/lane/housekeeping built on the reduced W); `L->feed`, `nr_core_layout_feed_cpu()`; CORE_PLAN line
   gets ` feed=<cpu>` only when requested. `nr_cl_build_n` == `nr_cl_build_nf(.., false, ..)` byte for byte (test). The receiver requests it when
   `radio && g_ncells > 1`; without a free CPU feed = -1 and the reader floats on the home lane (best effort, plan still hostable).
4. **Receiver glue** (`executables/nr-uesoftmodem.c`): lease hooks installed in `mpci_feed_start`, reader on `nr_core_layout_feed_cpu()`, no cpus passed to the
   channelizer (the old "first pool CPU = reader, next pool CPUs = channelizer" is gone), init call `nr_core_layout_init_nf`.

## iq_chan.c lines touched (CPU worker part only; backend registry/selection untouched)
New lease block before the CPU backend (31-62); `worker_main` (117-134); `cpu_run` (251, 257-258); `iq_chan_start` (859-882: demand/ceiling/bench cpus);
`iq_chan_stop` (891); `iq_chan_stats` (903-904). `iq_chan.h`: lease API (83-104), 4 stats fields (139-140).

## Tests [OFFLINE VERIFIED, DGX GB10 aarch64 unit]
- `test_nr_core_pool_chan` (14 tests, new): pure bounds; CHAN first against SCAN/DECODE/RETRY/DISC/HOUSE; never promoted; no yield; charges no cell;
  stats line unchanged without CHAN; bounded share (decode and scan keep theirs); reserved tokens stay free under a flood of every class; aged low
  waiters cannot take them; per-cell DL share accounts the demand (N=10, 2 cells: 8 -> 6 -> 0); helper try vs blocking wait at the cap; fast cores first
  on the GB10 pool fixture; reserved tokens = the fastest and nobody else is placed there; worker keeps its core (pin on, acquire+release median < 20 us); site wrappers.
- `test_nr_core_layout` (+5, 35 pass): no-feed == historical byte for byte; GB10 2 cells + feed distinct from rt/ring/uhd/pool/lane, same capacity bucket, W-1,
  line has feed=; 4-cpu and sens6 fixtures.
- `test_iq_chan` (+`test_lease`): fake pool (semaphore, cap 2): bit-exact equivalence and live add/remove/retune with leases at k=1 and k=4, never more than the cap
  held, every lease released, nothing held while the channelizer idles (200 samples), demand registered = threads and cleared on stop. Existing tests unchanged: ALL OK.
- `test_nr_core_pool` (35), `test_nr_core_pool_float` (5): pass.

## Bench [Bench, DGX GB10, shared host, n=1 run per row, 15-20 s, flock -x, nice 10] (logic evidence only)
Model (`bench_nr_core_pool ... chan=3 chan_demand chan_msps=245.76 chan_per_thread_msps=133.5 cells=2`): 245.76 MS/s x 4 RX x 2 cells, per-thread backend rate
133.5 MS/s per channel (`test_iq_chan --bench`, 2 cells decim 4/8, k=1, 0.543x real time) -> k = 3 (margin 1.25); 266.7 us block period, 491 us of single-thread
work per block (1.84 cores offered); 2 cells with the bench decode/scan/disc mix at scale s. Late block = last worker done > 1 period after arrival.

| pool N | scale | arm | c0 DL wait p99 us | c1 DL wait p99 us | DL miss (c0/c1) | CHAN lease wait p50/p99/max us | CHAN late blocks |
|---|---|---|---|---|---|---|---|
| 10 | 0.5 | no channelizer | 411 | 422 | 27, 38 /24k | - | - |
| 10 | 0.5 | chan=3 + demand | 463 | 486 | 12, 15 /24k | 52 / 57 / 5872 | 387/225004 (0.17 %) |
| 8 (taskset 4-12) | 0.5 | no channelizer | 287 | 335 | 78, 44 /24k | - | - |
| 8 (taskset 4-12) | 0.5 | chan=3 + demand | 1024 | 992 | 116, 119 /24k | 53 / 4794 / 29318 | 3722/225000 (1.65 %) |
| 8 | 0.25 | chan=3 + demand | 349 | - | 0 /9k | 53 / 55 / 704 | 69/168750 (0.04 %) |
| 8 | 0.1 | chan=3 + demand | 164 | - | 13 /3.6k | 53 / 5049 / 28941 | 2812/168750 (1.67 %) |
| 6 | 0.5 | chan=3 + demand | decode starved | | 18000 /18k | 55 / 77 / 4308 | 1112/225004 (0.49 %) |
| 6 | 0.5 | chan=3, NO demand | 2608 | 2824 | 1441, 1669 | all late (workers migrate per block) | 100 % |

Reading: with the demand registered the channelizer runs at ~220 us per 267 us block and its lease wait is the timer wake-up (p50 52 us); without the registered
reservation (leases migrate the thread every block, ~220 us of pin cost) it cannot keep real time: the receiver therefore always registers the demand before the
first block. A pool of 6 cannot host 2 cells at 0.5x of the bench mix plus a 3-token channelizer (3 tokens left for decode: starved, expected); N >= 8 hosts it, N=10 costs
DL wait p99 +50 us. The CHAN tail at N=8 (p99 5 ms in two of three runs, 0.04 % in the third and independent of the load: scale 0.1 bad, 0.25 good) is NOT explained:
[HYPOTHESIS] host noise from other sessions on the shared host / start-up transient; not re-measured. Real-radio, real-ring numbers are for the orchestrator.

## Interface notes for G5 (GPU channelizer)
- Backends need no pool knowledge. Around the CPU-side steps of a GPU backend (launch, staging copies, ring writes; NOT the wait for the device) call
  `h = iq_chan_lease_acquire(1)` ... `iq_chan_lease_release(h)`; non-blocking (`0`) for optional helper threads (`IQ_CHAN_LEASE_DENIED` = skip).
  `IQ_CHAN_LEASE_NONE` (no hooks) = proceed unleased, release is a no-op.
- Demand: `iq_chan_start` registers k only for the CPU backend; a GPU backend calls `iq_chan_lease_set_demand(n)` with the number of CPU workers it needs
  (typically 1) and `0` at stop (`iq_chan_stop` also sends 0). Reserved tokens are the fastest cores and fixed; a sticky worker keeps its core.
- Hold a lease only for a processing step; never across `cudaStreamSynchronize`-style waits or the wait for the next block.
- `iq_chan_stats_t.lease_*` are process-wide counters.

## Open items
- Demand is set at start (k from the start-up bench) and not recomputed when cells pause/resume or the capture span changes (reserved tokens then idle).
- RINGS-mode (shm_iq device, several cells, radio=true) also plans a feed reader CPU it does not use (the layout is built before the device name is known).
- GPU backend (G5): demand/lease calls to be added in its CPU parts (interface above).
- nr-uesoftmodem link and ctest: see the final message.
