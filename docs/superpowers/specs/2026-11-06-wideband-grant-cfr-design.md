# Wideband grant-level DM-RS CFR (sensing mode only)

Status: implemented on twomodes/wbgrant (base 6abc464153). Evidence labels per PROJECT_MEMORY section 0.1.

## Problem [MEASURED OTA daytime 2026-11-06, SINGLEPCI-OTA-20261106-DAY.md 5.1]
415 (217 PRB) carries 40-81 wide DL grants/s over 70-80 RNTIs but only 1.4-3.1 % yield a wideband pdsch_dmrs record. A pdsch_dmrs record is
gated per (cell, RNTI, source) unit; the scheduler enables about 3 units at a time (one cold admission per 4 s decision).

## How a pdsch_dmrs record is produced today [CODE READ]
- No LDPC is needed. The record is a DM-RS tap: one slot FEP of the first DM-RS symbol, `nr_pdsch_channel_estimation`, comb-2 export, `nr_isac_submit_cfr_multi_meta`.
- Two tap sites, both behind the per-RNTI unit gate `nr_sense_unit_gate(_deferred)(NR_UNIT_PDSCH_DMRS, rnti, prbs)`:
  1. `consumer_dmrs_tap` (nr_pdsch_passive_queue.c): grants of the solver path (solved/probing DCI 1_1 layouts), on a PDSCH consumer thread, after the TB CRC verified
     the identity, or after a CRC failure on a SOLVED layout. This is where every wide grant of a solved RNTI passes.
  2. the monitor tap (nr_pdcch_blind_monitor_rt.c) for DCI 1_0 and non-autodiscovered 1_1 layouts, on the scan consumer.
- Cost per tap: the code comments quote ~100-136 us at wide allocations [CODE COMMENT, DGX 2026-11-01]; this branch measures it per cell (SENSE_WBG cost_us) [see report].
- The grant itself is already decoded (sensing decodes every proven wide grant), so the tap is the only extra work.

## Design (as implemented after fix round 1)
1. Scope: a DL grant of >= sense_min_prb PRBs (the wideband target; not the continuity floor T_cell), C-RNTI, proven fields, whose unit is disabled. Grants of enabled units never reach the path (no double export).
   - Consumer site (`consumer_dmrs_tap`, solver-path jobs and monitor-site jobs flagged `wbg_pre`): only after the TB CRC passed. A CRC-failed grant's DCI was never verified (chance DCI = random allocation), so the SOLVED-layout CRC-fail rescue tap stays unit-only.
   - Monitor site (DCI 1_0, non-dl_auto 1_1, sweep settled): the monitor runs NO chest (it holds the per-cell g_phase2_mu); it only takes a token (one CAS) and enqueues the job with `wbg_pre`; the PDSCH consumer taps it after the CRC.
   - A record is exported only with nvar > 0, channel power > 0 and at least one RE.
2. `nr_sense_wbg_admit`: per-cell lock-free token bucket (depth 4), refilled at the rate the scheduler published, never unlimited: one tap per decision interval before the first decision / after a cell reset. A grant takes one token; with none left only a grant wider than the widest admitted since the last decision may borrow (down to -2 tokens); everything else waits for the refill (time). It is a rate limiter with a widest-grant exception, not a ranking of grants. Cells the scheduler does not decide for (index >= NR_SENSE_CELLS) admit nothing.
3. Budget (`nr_sense_sched_decide`), admission order per decision: (a) enabled incumbents and units with history, in rank order (a challenger that beats an incumbent by the margin still displaces it); (b) the wideband-grant resource, per cell, priced on POOL (offered rate x CPU per tap), QUEUE_DL (the tap runs on the PDSCH consumer, so the same time delays the next decode job) and SENSE_RING (writer cost per wbg record); its pool share is the room left by (a) divided by the cells with offered wide grants; (c) cold units. Published rate = f x offered x 1.25 (the incumbent margin), finite. Not measured yet: rate from the cell's pdsch_dmrs unit model (wide rate x p99 bucket CPU per message) holding no budget, else one tap per decision interval.
4. Accounting: tap CPU -> `nr_sense_wbg_cost`, writer cost -> `nr_sense_wbg_ring_cost`, record -> `nr_sense_wbg_cfr` (cell totals only). Never booked to an RNTI unit or to the unattributed baseline. Scheduler counter deltas are guarded against a decrease (cell reset), `g_wbgp[c]` is cleared on a cell reset.
5. Traffic and legacy: every new function starts with nr_isac_mode()==SENSING or sits behind the unit gate (true outside sensing); no new env/CLI/-D/.cfg key, no locks, no fixed cores.

## Known limits
- The mean (not p99) per-tap cost is compared with the p99 budgets.
- Records of [T_cell, 128) PRBs cannot come from this path (threshold is sense_min_prb).
