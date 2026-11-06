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

## Design
1. `nr_sense_wbg_admit(prbs, t)` (nr_sense_unit.c), asked by both sites only when the unit gate refused: admits a grant of >= T_cell PRBs (`nr_sense_cell_floor`,
   sense_min_prb = 128 until the scheduler steps it down) in SENSING mode, whichever RNTI. Grants whose unit is enabled never reach it (no double export); the
   per-RNTI units and their skip/rate accounting are unchanged. Fields must be proven: site 1 only runs for CRC-verified or SOLVED layouts; site 2 only for DCI 1_0 or a
   non-dl_auto 1_1; a family accept without verified fields never gets here.
2. Per-cell lock-free token bucket (no mutex; CAS on one word). Unlimited until the scheduler's first decision. When tokens run out a grant wider than any admitted since
   the last decision may borrow (down to -2 tokens): widest first; otherwise tokens accrue with time, so the grant after the longest gap gets the next token: oldest gap first.
   No per-RNTI ranking.
3. Budget: the scheduler (`nr_sense_sched_decide`) prices one per-cell "wideband grant" resource in the shared POOL before any unit is admitted:
   demand = offered wide-grant rate x mean CPU per tap x bucket; admitted fraction f = min(1, pool room / demand); the rate f x offered is published to the bucket every
   decision. Its CPU is booked by `nr_sense_wbg_cost` into its own counters, never into an RNTI unit or the unattributed baseline (no double count).
4. Records: `nr_sense_rs_meta_t.wbg` (not printed). The writer books a wbg record to the cell totals (wb_valid, gap_max) via `nr_sense_wbg_cfr`, not to an RNTI unit, so the
   units' valid ratio, age and eligibility are untouched.
5. Traffic and legacy mode: every new path starts with `nr_isac_mode() == SENSING` (the unit gate returns true there, so `wbg` stays false); the scheduler thread does not run.
   No new env/CLI/-D/.cfg key, no constants per cell, no locks on RX/scan threads, no fixed cores.

## Known limits
- The mean (not p99) per-tap cost is compared with the p99 pool budget.
- The record writer's cost for wbg records (SENSE_RING) is not priced.
- T_cell below sense_min_prb makes wbg records of [T_cell, 128) PRBs continuity records, as for units.
