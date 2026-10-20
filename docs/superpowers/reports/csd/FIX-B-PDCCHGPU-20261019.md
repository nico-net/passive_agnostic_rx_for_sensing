# FIX-B' -- CPU-only cut of slot arrival -> grant enqueued (file name kept: FIX-B-PDCCHGPU-20261019)

Branch csd/int16-pdcchgpu (from freeze-20261018 310a86d899). The GPU PDCCH design (Fix B) was replaced by B' on the coordinator's decision:
CPU only, no extra core, no new flag. Evidence labels: [READ FROM CODE], [BENCH unit, DGX host, loaded] (shared machine, other agents running:
microbench numbers carry that noise), [MEASURED on air 2026-10-18] (from RESULT.md, prior session). NOT measured on air by this work: nothing here
was run on a radio or replayed.

## 1. What was changed

| # | change | files | bit-exact? |
|---|---|---|---|
| 1 | Post-accept bookkeeping runs AFTER the PDSCH job is enqueued (queue `nr_post_accept_t`, GCC cleanup at the end of each Phase-2 iteration, so every `continue`/`break` still runs it): rnti_seen log line (the clock is read at the old place, only formatting moves), TechA-window + cell-state RNTI notes (2 mutexes), on-accept DM-RS probe, stage-0 note, search-space registry note, AL1-union note, USS-hash note | nr_pdcch_post_accept.h (new), nr_pdcch_blind_monitor_rt.c | same decoded grants, solver learning, discovery; only the order of notes that nothing in the iteration reads changes |
| 2 | DCI 1_1 resolver observe pass (runs per accepted DL DCI inside accept->enqueue, before the layout choice): visit only live layouts (8 flags per load) instead of testing 5k-27k flags in 7 loops; plausible+impossible computed in one pass | nr_pdcch_dci11_layout_sweep.{h,c} | state hash identical to the unmodified code (golden test) |
| 3 | Solver reading pass: same alive-only scan and one memset for the group ids | nr_pdcch_blind_monitor_rt.c | readings, ids and order identical (test replicates the pass, old vs new) |
| 4 | Scan queue: signal after the unlock (was under the lock: the woken consumer's first act is to take the lock the producer still holds) ; PDSCH queue flush: same for the broadcast | nr_pdcch_passive_queue.c, nr_pdsch_passive_queue.c (3-line hand-off, no decode code) | n/a |
| 5 | Evidence stamps (ISAC_E2E_TRACE, already in the frozen env): SCAN_E2E now also prints pre_us (arrival -> enqueue) and q_us (enqueue -> consumer running), 10 us bins; with ISAC_A2E_TRACE a POSTACCEPT line (mean/max us of the deferred bookkeeping) | nr_pdcch_passive_queue.{c,h}, rt.c | diagnostic only |

### Inputs the job (or its consumer) reads -- these stay BEFORE the enqueue
- BC9 DCI history entry (`bc9_dci_accept`): the PDSCH consumer looks up THIS DCI in it (`nr_dci_hist_at(rnti, dci_abs_slot)`, nr_pdsch_passive_queue.c) as soon as it dequeues; deferring it would race.
- RNTI bootstrap / persistence / dedupe / mismatch gates, `ue->dci_thres`, `nr_ue_ctx_on_dci_accept`, autodiscover observe (the "unverified" drop reads it), layout observe + pin selection (choose the layout), `nr_pdsch_solver_note_dci`, the solver planning and every field of `nr_pdsch_passive_job_t`.
- The discovery-token observe stays where it is (that path never enqueues).
- DCI 1_1 solver block: it BUILDS the jobs (readings decide which variants exist, their layout, rate matching and solver token), so it cannot move after the enqueue (same conclusion as DL-A2E-20261018 item 3).

## 2. Scan-queue wait: mechanism
[MEASURED on air 2026-10-18, RESULT.md] arr->occ p50/p90/p99 = 61/162/385 us; occ->pass p99 272 us.
[READ FROM CODE] The "arrival" stamp is taken when `nrue_ru_read` returns (nr-ue.c:1758). Before the scan consumer can run: RT thread census loops and TX bookkeeping, NotifiedFIFO push to a worker, `UE_dl_processing` (MAC slot indication, PBCH, PDCCH), `nr_pdcch_blind_monitor_process` occasion gate, then `nr_pdcch_passive_queue_enqueue` -> `pthread_cond_signal` -> consumer. The queue itself is a mutex + condvar with a 20 ms timed wait: a futex wake, no polling, no extra lock in the way (the only avoidable bounce was signalling under the lock; fixed).
[BENCH unit, DGX host, loaded] idle consumer woken once per 500 us: enqueue -> consumer running p50 98-104 us (p99 120-500 us) whether the signal is under the lock or after it (differences are inside the run-to-run noise: 96.6 vs 101.1 us in run 1). The same consumer kept awake by a bounded sched_yield spin: p50 4.4 us (run 1) -- one run out of two (the other run was disturbed by the host). So the dominant term is the CPU leaving an idle state, not the lock or the condvar: [READ FROM CODE, sysfs] this Grace host has cpuidle LPI-0..3 with exit latencies 0 / 42 / 231 / 433 us and residencies 0 / 1930 / 2542 / 2542 us; a consumer core that idles ~250 us per slot is exactly in the 40-230 us exit range.
The remedy is therefore a SYSTEM setting, not code (needs root, so not done here): hold `/dev/cpu_dma_latency` at 0 for the receiver's lifetime (the radio window wrapper can keep a small root helper open), or `echo 1 > /sys/devices/system/cpu/cpuN/cpuidle/state{2,3}/disable` (and state1) on the scan consumer cores. A code-side spin would cost ~0.4-0.6 core per consumer (idle part of the slot) and was not added: it contradicts the "fewest cores" goal and the orchestrator's no-polling hint. It can be added later as a bounded spin if the system setting is not acceptable.
Next on-air run: SCAN_E2E pre_us / q_us (with ISAC_E2E_TRACE=1) splits the 60 us into producer side (RT chain + worker hand-off) and queue/wake.
The +240-390 us tail is not a wake-up: [MEASURED] occ->pass and pass+ulsweep are long on the same slots, i.e. a consumer still busy with the previous occasion (also the Phase-2 lock wait p1end->p2 p99 136-209 us). Shortening the pass (below) is what shortens it.

## 3. Cost of the parts (unit bench, same host)
| part | before | after | label |
|---|---|---|---|
| `nr_dci11_resolver_observe_rnti`, per accepted DL DCI (n_hyp 5458, 350 alive, 4-RX irrelevant) | 19.0 us | 12.7-14.2 us | [BENCH unit] |
| same, n_hyp 10916, 225 alive | 25.9 us | 11.7 us | [BENCH unit] |
| solver reading pass (n_hyp 5458, 350 alive, 226 parses/DCI) | 33.5 us | 23.6 us | [BENCH unit, test replicate of the monitor loop] |
The remaining reading-pass cost is the ~226 payload parses (~100 ns each); on air the layout set is larger (DL-A2E: 1250-1480 parses per DCI on capture b), so the O(n_hyp) part removed here is a fixed ~10 us and the parse cost scales with the live set. `ISAC_DCI11_PARSE_CACHE` (bit-exact, 0 mismatches in 10 M parses, DL-A2E) would take ~15-20 % of the pass; it is not in the frozen env and its A/B was inconclusive (tim p99 +100 us, one outlier run), so it is NOT switched on here.
[MEASURED on air 2026-10-18] accept->enq p50 122/106 us: accept->solver 26 us (this includes the resolver observe, now ~7-12 us cheaper), solver readings 91 us (b) / 28 us (tim), readings->enq 5 us. Expected effect of this work on air, from the bench: -10 to -25 us on accept->enq at the median; NOT measured on air.
PDCCH pass (fep/demap/prepass/decode, 200-225 us): no cut made. The existing stamps (BTIM + a2p) give the split per occasion, not per grant; no unit bench can reproduce the FEP/LLR on a real slot without a dump. Known from earlier reports: polar decode ~1.8 us per AL2 candidate on one core (SCAN2-20261014), ~49 % of scan-thread time is the anonymous DL length sweep -- needed for discovery, so left alone.

## 4. Tests
- New (in test_nr_pdcch_blind_monitor): PostAccept.JobEnqueuedBeforeBookkeepingIdenticalContent (job first on fall-through, early return, break-like exit; job content equal in both orders), PostAccept.FifoOnceAndFullQueueRunsAtOnce, AliveNext.MatchesLinearScan (70 lengths x 40 densities x every start, unaligned), Dci11ObserveGolden.StateHashUnchanged (3 BWPs/sequences; golden produced by the UNMODIFIED nr_pdcch_dci11_layout_sweep.c at 310a86d899 with the same header-only harness tests/nr_dci11_observe_golden.h), Dci11ObserveGolden.ReadingPassAliveScanIsIdenticalAndFaster, two BENCH tests. ctest name: test_nr_pdcch_post_accept.
- Full suite: see section 6.

## 5. NOT done / limits
- No on-air or replay measurement (forbidden here). The on-air effect of 1-4 is estimated from the bench only.
- The post-accept deferral is a code-structure change in an 11 k-line function: the unit test proves the mechanism (order, once, all exits), not the integration inside run_occasion_one; reviewed by reading every deferred call for readers inside the iteration (list above).
- The GPU PDCCH lane (Fix B as first specified) was not built: see section 7.
- Idle-state exit (the 60 us) needs the root-level setting above.
- Not touched: PDSCH/PUSCH decode, retry, instance pools (csd/int16-fixad), the nID discovery lane.

## 6. Build and ctest
ninja -j6 nr-uesoftmodem oai_usrpdevif tests through heavy.sh on the DGX (aarch64, build_t26): nr-uesoftmodem and oai_usrpdevif built; only the 3 known link failures (test_nr_ssb_rate_match_prod, test_nr_csi_rb_extent, nr_pusch_ra0sim) remain.
ctest -j4: 253 run, 14 failed, all on the known list: 9 test_nr_pusch_ra0_* (Not Run, link), test_nr_ssb_rate_match_prod, test_nr_csi_rb_extent (Not Run, link), dft_test, test_nr_modulation, nr_td_sim_test (471 s timeout). test_nr_pdcch_blind_monitor passed in full (298 gtest cases: the existing suite plus the new ones above), test_nr_pdcch_post_accept, test_nr_dci11_pin passed. [BENCH/ctest unit, DGX]

## 7. Why the GPU PDCCH (first Fix B) was not built [analysis kept from the first pass]

- SCAN2-20261014.md s5 [MEASURED, DGX]: SC polar on GPU, batch of 1/8/32/128 items = 319/261/284/326 us total
  (kernel floor 230-285 us: serial SC tree, one warp per item) vs CPU 1.8 us per AL2 decode. GPU wins only at >= 500 items/call.
  Live: 50-100 decodes per pass (~0.2 ms CPU) vs 0.65-1.2 ms per GPU round trip, plus PDSCH contention.
- csd/int16-ppdcch 9354a94692 [MEASURED]: "stage A bit-exact (3.3M items) but mode 1 slower than CPU on tim; not merged".
- nr_pdcch_gpu_fep.h STATUS: GPU LLR sign parity vs CPU int16 chest FAILS (branch gate, fixed-point compensation, autoscale
  not ported). Bit-identical GPU chest/LLR is itself an unsolved sub-project.
So a one-submission GPU pass (FEP+chest+demap+polar+CRC) cannot reach <= 0.15 ms: kernel floor for polar alone is ~0.25 ms.

