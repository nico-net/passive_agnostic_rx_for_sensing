# wbgrant: grant-level wideband DM-RS CFR path (sensing mode only)

Branch twomodes/wbgrant from twomodes/design 6abc464153. Design: docs/superpowers/specs/2026-11-06-wideband-grant-cfr-design.md.
Host for every measurement: DGX Spark (aarch64), 4 RX, idle host under flock -x heavy.lock + td_measure.lock.

## What changed
- `nr_sense_wbg_admit/cost/cfr/set_rate/stats` (nr_sense_unit.c): per-cell lock-free token bucket. Asked by both DM-RS tap sites only after the unit gate refused (so no double export); admits grants of >= sense_min_prb PRBs (the wideband target, not the continuity floor) of any RNTI. Out of tokens: a grant wider than any admitted this window may borrow (widest first), otherwise tokens accrue with time (oldest gap first). No per-RNTI ranking.
- `nr_sense_sched_decide`: one per-cell "wideband grant" resource priced in the shared POOL before any unit (offered rate x mean CPU per tap x bucket); f = min(1, pool room / demand). f = 1: no throttle. rt thread measures offered rate and cost per tap from cumulative counters and publishes the rate each decision.
- Sites: `consumer_dmrs_tap` (solver-path grants: CRC-verified or SOLVED layout) and the monitor tap (DCI 1_0 / non-dl_auto 1_1). Never-guess: unsolved layouts and sweep hypotheses never reach it.
- wbg records are booked to the cell totals only (meta.wbg, not printed), never to an RNTI unit; wbg tap CPU goes to its own counter (not the unit, not the unattributed baseline).
- No new env/CLI/-D/.cfg key, no locks, no fixed cores. Everything is behind nr_isac_mode()==SENSING.

## How a pdsch_dmrs record is produced (item 1) [CODE READ]
DM-RS tap only: one slot FEP of the first DM-RS symbol + `nr_pdsch_channel_estimation` + comb-2 export. No LDPC. The grant is decoded anyway in sensing mode (every proven wide grant). Cost per tap at 217 PRB / 4 RX [MEASURED replay, DGX, 4 RX, 0.25x speed, thread CPU incl. FEP + estimate + export + hand-off]: about 290-300 us steady (SENSE_WBG cost_us 285-312; 360-710 us in the first seconds), 104 us on the 106 PRB rfsim bed. The 217-PRB tap does its own FEP (consumer site is not on the E5 shared cache), which is why it is ~3x the code-comment figure of ~100 us [HYPOTHESIS: not decomposed].

## Replay A/B [MEASURED replay, DGX, raw_pci415_12s_b (217 PRB, 4 RX, sensing cfg, FROZEN-style env MPCI_20261025, speed 0.25), base = rr-twomodes build 6abc464153, new = this branch]
12 s capture (10.8 s span with grants). Order b, n, n, b used the first n build, which had two defects fixed afterwards (T_cell threshold, meta.wbg not carried); those arms (wS1/wS2) are not used below. Final arms: bS1 bS2 (base), nS1 nS2 (new).

| arm | wide grants (>=128 PRB) | wideband pdsch_dmrs rec/s | share of wide grants with a >=128 PRB pdsch rs record (slot+rnti) | share with any pdsch rs record | snr_db_p50 p10/p50/p90 (wideband pdsch_dmrs) | CPU cores | scanq full+stale / queued | pdschq full+stale / queued |
|---|---|---|---|---|---|---|---|---|
| base S1 | 482 | 0.18 | 0.4 % | 3.1 % | 5.7 / 8.0 / 8.0 | 2.27 | 0 / 18314 | 0 / 7607 |
| base S2 | 336 | 1.39 | 8.3 % | 23.8 % | 7.3 / 14.1 / 17.8 | 1.84 | 0 / 18223 | 0 / 4103 |
| new S1 | 478 | 7.21 | 18.8 % | 43.3 % | 6.0 / 8.3 / 15.9 | 2.23 | 0 / 18269 | 0 / 7258 |
| new S2 | 474 | 6.93 | 18.1 % | 41.1 % | 5.9 / 8.3 / 15.9 | 2.28 | 0 / 18287 | 0 / 6918 |

- Headline: wideband pdsch_dmrs rec/s 0.18-1.39 (base) to 6.9-7.2 (new), x5 to x40; share of wide grants with a wideband record 0.4-8.3 % to 18-19 %.
- SNR distribution of the wideband records is in the base spread (p50 8.0-14.1 vs 8.3); the p90 widens (15.9 vs 8.0-17.8) because the new records include RNTIs at other SNR.
- CPU: 2.23 / 2.28 (new) vs 2.27 / 1.84 (base): within the 0.4-core spread between base runs; no penalty resolvable with n=2.
- Queue drops: scan and PDSCH queue full and stale are 0 in all four arms.
- Budget: `frac=1.00, rate=-1` in every decision (the budget never bound): offered 116-118 taps over the run, 290 us each = about 3 ms CPU/s at replay speed. The budget-short path is exercised by the unit and rt tests only [SIM VERIFIED], not on air.
- Limit [MEASURED]: only 116-118 of ~478 wide grants reach a tap site. The rest are never offered: grants of RNTIs whose layout is not yet proven in this 12 s capture (solver probe grants have dmrs_tap = false, unsolved dl_auto 1_1 never taps by the never-guess rule). So the remaining gap is the solver's coverage, not the budget. 43 % of wide grants now have some pdsch rs record but only 19 % reach >= 128 valid PRBs: the quality block (unchanged) invalidates PRBs on the rest.
- Replay caveat: the replay runs at 0.25x, so wall-time rates (SENSE_WBG offered_hz) are 4x lower than capture-time rates; the capture is 12 s old data (2026-10), not the daytime 2026-11-06 traffic. Two arms per build; run-to-run spread of the base is large (wide grants 482 vs 336, wideband rec/s 0.18 vs 1.39).

## Traffic mode, same capture [MEASURED replay, 1 arm each]
bT1 (base) vs nT1 (new), traffic cfg: ISAC_METRICS key set identical (336 keys), no SENSE_SCHED / SENSE_WBG lines in either log, no sense_prod_count / sense_cfr_exported counters (all 0 in both). Counters differ by replay timing (pdschq_crc_ok 241 vs 261, traffic_skip 7758 vs 9209, scanq_queued 18285 vs 18313): the base-vs-base spread was not measured for traffic, so these are not shown byte-identical, only structurally identical. Code argument: every new function begins with nr_isac_mode()==SENSING or sits behind nr_sense_unit_gate (always true outside sensing), the scheduler thread does not run, and the nr_isac.cc copy of meta.wbg only copies a false bool [CODE READ].

## Sensing rfsim, s9 bed [SIM VERIFIED, DGX, 106 PRB, 4 RX, 150 s, ue.passive.sensing.rfsim.s9.cfg, rfsim_arm.sh, one arm each]
| arm | ssb | csirs | pdsch_dmrs | pdsch_data | invalid rs (valid_cfr_prbs 0) | violations (last SENSE_SCHED) |
|---|---|---|---|---|---|---|
| base, sense_min_prb 128 (cfg default) | 9534 | 795 | 12338 | 3359 | 0 | 0 |
| new, 128 | 9591 | 800 | 14072 | 3370 | 0 | 0 |
| base, --sensing.sense_min_prb 96 | 9606 | 801 | 14672 | 3384 | 0 | 0 |
| new, 96 | 9603 | 800 | 14807 | 3382 | 1 | 0 |
- At 128 the 106 PRB bed never offers a grant (offered=0): the path is dormant, as intended. At 96 it fires: 682 taps, 682 records, 104 us per tap, frac 1.0.
- All sources valid, 0 violations in all four arms, except one pdsch_dmrs record with valid_cfr_prbs = 0 (snr null, n_re 636, rnti 4660, abs_slot 48890) among 14807 in new-96. I did not establish whether it came from the wbg or the unit path or whether base can produce one (0 of 14672 in base-96; n=1 arm). Flag for review.

## ctest -j4 [SIM VERIFIED, DGX]
397 tests: 19 not passing first run. 16 are the known list (7 Not-Run pusch_uci, qam64/qam256, ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells, dft_test, ldpc_cuda_pool_cpu, modulation). Three were new to me and are handled: test_static_audit_scanner and static_audit_check failed because the new file-static g_wbgp had no row in tools/mpci/mpci_statics.tsv (row added, both pass); test_nr_gpu_pass failed under load and passed on rerun (flaky in earlier reports too). All sense tests pass: unit (+5 wbg), sched (+5 Wbg), sched_rt (+1).

## Concerns
1. Coverage is capped by proven layouts (~25 % of wide grants reach a tap in this capture); more wideband records now need the solver to prove layouts faster, not a bigger budget.
2. The record writer cost (SENSE_RING) of wbg records is not priced; mean (not p99) tap cost is compared with the p99 pool budget. Both are harmless at 3 ms/s but matter if a cell has hundreds of wide grants/s.
3. The tap does its own FEP at the consumer site (~290 us at 217 PRB); sharing the E5 slot cache there would cut it.
4. Unit tests were written after the implementation (no red run); mutation check not done.
5. Measured on a 12 s October capture at 0.25x speed, not on air and not on the daytime traffic; no OTA validation.
6. Traffic/legacy identity is structural plus counter-set equality, not byte equality (replay timing).
7. One invalid record in new-96 rfsim (above).

## Fix round 1 (review findings F1-F12)
Commit: "wbg fix round 1" (on top of b2db1d870f). Design doc rewritten to the implemented behaviour (F11). Tests were written first; they failed to compile against the old API (red), then passed.

| finding | change |
|---|---|
| F1 | `consumer_dmrs_tap(..., crc_ok)`: wbg only after the TB CRC passed and for a C-RNTI (`nr_sense_wbg_consumer_ok`); the SOLVED-layout CRC-fail rescue tap stays unit-only. A wbg record is submitted only with nvar > 0, channel power > 0 and at least one RE (`nr_sense_wbg_export_ok`). |
| F2 | The monitor no longer runs any chest for wbg. Under g_phase2_mu it takes one token (a CAS) and enqueues the job with `wbg_pre` (and `dmrs_tap`); the PDSCH consumer taps after the CRC. Hold time: the added work under the lock is one atomic CAS [CODE READ]; the logs contain no LOCKHOLD line (count 0), so there is no timing line to show. |
| F3 | The wbg is priced on POOL, QUEUE_DL (max semantics, f x demand) and SENSE_RING. |
| F4 | The bucket is never unlimited. Startup/reset rate = 1 tap per SENSE_DECIDE_NS. Published rate = f x offered x 1.25 (INCUMBENT margin), never -1. Unmeasured cost: rate from the cell's pdsch_dmrs unit model (wide message rate, p99 bucket CPU per message), holding no budget; none: 0.25 Hz. |
| F5 | Order: enabled incumbents and units with history (rank order, so a challenger beating an incumbent by the margin still displaces it) -> wbg with pool share = remaining room / cells with offered wide grants -> cold units. CellsShareThePoolInOrder rewritten (fair share, incumbent kept). |
| F6 | `g_wbgp[c]` cleared on cell reset; all counter deltas guarded (a decrease re-baselines, offered_hz 0 for that period). |
| F7 | Cells >= NR_SENSE_CELLS admit nothing (test with cell index 8). |
| F8 | wbg writer cost -> `nr_sense_wbg_ring_cost` (cell resource), priced on SENSE_RING. |
| F9 | `nr_sense_wbg_monitor_ok` includes (!sweep_generation \|\| settled). |
| F10 | Moot: the monitor does no chest; the consumer books `nr_sense_wbg_cost` after every tap that ran. |
| F12 | New tests: site conditions (class, dl_auto, sweep, CRC, export), no double export (`nr_sense_wbg_after_gate`), cell reset, counter decrease, concurrent admit (8 threads x 2000 on one bucket: 4..6 admitted), cell 8, ring cost, queue/ring pricing, unit-model startup, rt test asserts published rate (125 Hz = 1.25 x 100 Hz offered; < 0.1 Hz with a tiny pool) and offered_hz / cost. |

Tests [SIM VERIFIED, DGX]: unit 37 (+8 wbg), sched 62 (+10 Wbg), sched_rt 20 (+3) pass. ctest -j4: 397 tests, exactly the 16 known failures (7 Not-Run pusch_uci, qam64, qam256, ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells, dft_test, ldpc_cuda_pool_cpu, modulation); static_audit and test_nr_gpu_pass pass.

### Replay A/B, raw_pci415_12s_b, 217 PRB, 4 RX, sensing cfg [MEASURED replay, DGX]
Speed 1.0x (real-time ingest; the raw_iq tooling takes ISAC_RAW_IQ_SPEED=1.0), 2 arms per build:

| arm | wide grants | wideband pdsch_dmrs rec/s | wide grants with >=128 PRB rs (slot+rnti) | CPU cores | scanq full+stale / queued | pdschq full+stale / queued |
|---|---|---|---|---|---|---|
| base b1 | 438 | 0.00 | 2.7 % | 2.41 | 0 / 17442 | 0 / 3294 |
| base b2 | 412 | 0.00 | 2.9 % | 2.53 | 0 / 17542 | 0 / 4213 |
| new n1 | 465 | 1.22 | 3.0 % | 2.49 | 0 / 17513 | 0 / 3543 |
| new n2 | 452 | 2.43 | 7.5 % | 2.53 | 0 / 17486 | 0 / 3444 |

SNR p10/p50/p90 of the new wideband records: 7.2/8.4/16.2 and 6.4/8.3/16.6 (base has none at 1.0x). Scan and PDSCH queue full and stale drops: 0 in all four arms, so the new tap does not raise drops at 1.0x. CPU: new 2.49/2.53 vs base 2.41/2.53 (inside the spread).
The gain is small at 1.0x because the run is 12 s long and the first decision (that publishes a measured rate) comes only after ~8 s: until then the startup rate is 1 tap/4 s (+ bucket of 4). In n1/n2 offered 55/72 and admitted 17/33. This is the price of "never unlimited" in a short run; on a long OTA run the startup lasts one 4 s interval.
Tap cost 330-430 us (higher than before at real-time load), writer cost 74-79 us per wbg record.

Speed 0.25x, new build, 2 arms (same capture as round 0; base arms bS1/bS2 from round 0 are unchanged code): f2: 491 wide grants, 3.88 wideband rec/s, 11.2 % of wide grants with a >=128 PRB rs record (23.6 % with any), SNR 7.1/8.9/16.2, CPU 2.14, scan and pdschq drops 0/0. f1: INVALID run (the replay produced only 6 wide grants: the receiver did not hold sync on that run; not used). So at 0.25x: 3.9 rec/s and 11 % vs round 0's 7 rec/s and 19 % before the fixes, against base 0.18-1.39 rec/s and 0.4-8.3 %. The loss against round 0 is the CRC gate (F1) and the finite startup (F4) [MEASURED, n=1 valid arm; not decomposed].

### s9 rfsim, sense_min_prb 96 [SIM VERIFIED, 150 s, 4 RX, one arm each]
base: ssb 9582, csirs 799, pdsch_dmrs 14649, pdsch_data 3381, 0 records with valid_cfr_prbs 0, violations 0. new: ssb 9609, csirs 801, pdsch_dmrs 15342, pdsch_data 3381, 0 records with valid_cfr_prbs 0 (was 1 before F1), violations 0; wbg offered 641, admitted 107, taps 107, 112 us per tap.

### Concerns
1. At 1.0x the 12 s capture is mostly startup; the benefit of the measured-rate regime is only seen at 0.25x (3.9 rec/s). A longer real-time capture or an OTA run is needed to show the steady state.
2. F1 and F4 cost coverage (about 11 % vs 19 % of wide grants at 0.25x, n=1 valid arm).
3. The cold-unit-last ordering changes admission order only when a cold unit would have outranked a warm one; existing sched tests pass unchanged except two rewritten Wbg tests.
4. Hold time under g_phase2_mu is argued by code read (one CAS), not measured: no LOCKHOLD line exists in these logs.
5. Mean tap cost is still compared with p99 budgets; the unit-model startup estimate is rough (p99 bucket CPU divided by messages per bucket).

## Fix round 2 (R1-R6)
Commit c7f83e4ed4 (plus this report). Tests first: they failed to compile against the old API (red), then passed.

| item | change |
|---|---|
| R1 | `consumer_dmrs_tap(..., crc_ok, iq_ok, tok)`: iq_ok = `owned \|\| nr_passive_credit_allowed(producer slot, job slot)` evaluated at tap time; the wbg tap needs `nr_sense_wbg_tap_ok(crc_ok, iq_ok)`. The false "overwritten IQ gives pw=0" comment is gone. The unit path is unchanged (it does not take iq_ok; that is the pre-existing behaviour). |
| R2 | `nr_sense_wbg_refund()` (atomic add on wbg_tok, capped at the depth; counter wbg_refunded) on: enqueue refused (both monitor enqueue sites), job dropped/stale/narrow/evicted (`release_solver` refunds a `wbg_pre` job), CRC fail or probe (end of the decode block), ring credit expired, dmrs_sym invalid, nothing exported (CPU booked with `nr_sense_wbg_wasted`, no tap counted). Identity admitted - refunded == taps: unit test, and 25 - 0 = 25 in the replay log. Not covered: a decode ERROR/UNSUPPORTED status that skips the block, and the `slot_gate` timeout path goes through release_solver (covered). The first leaks a token only until the bucket refills. |
| R3 | A cell >= NR_SENSE_CELLS: admit refuses and counts `overcap`; the scheduler thread logs a one-time LOG_W per cell; metrics (sensing mode only) gain `sense_wbg_offered/admitted/taps/refunded` and `offered_wbg_overcap`. NR_SENSE_CELLS (8) is not raised: documented in nr_sense_unit.h. |
| R4 | The first wbg decision of a cell takes the counters since start as one period (offered rate = offered / SENSE_DECIDE_NS, cost from even one tap). Test: no pdsch_dmrs unit, 4 taps, first decision publishes 12.5 Hz (measured), not 0.25 Hz. The cell-reset test now uses `nr_sense_unit_reset` (what a resync does). |
| R5 | Routed: a grant that needed the chest (SNR gate on) now reaches the wbg path at the second defer-enqueue site, after the gate passed, with `wbg_pre`; the tap still runs on the consumer, outside g_phase2_mu. |
| R6 | `_Static_assert(sizeof(nr_pdsch_passive_job_t) == 1792)` in nr_pdsch_passive_queue.c; 1792 (0x700) measured by compiling the header at base 6abc464153 (rr-twomodes flags) and at this branch: identical, wbg_pre fits in padding. The 12 s capture raw_pci415_12s_b from October loads and replays in the smoke arms below. |

Tests [SIM VERIFIED, DGX]: unit +5, rt +1 (plus the changed cell-reset test); statics audit row for the warning flag. ctest -j4: 397 tests, 17 not passing = the 16 known + test_nr_gpu_pass_geo, which passed on rerun (GPU test, no GPU file touched; flaky under load as test_nr_gpu_pass was).

Smoke replay, raw_pci415_12s_b, 1.0x, one arm per build [MEASURED replay, DGX]: base b1: 458 wide grants, 0 wideband pdsch_dmrs rec/s, 2.4 % of wide grants with a >=128 PRB rs record, CPU 2.43, scanq 0/17481, pdschq 0/3837 (full+stale / queued). New n1: 361 wide grants, 24 wideband records = 2.25 rec/s, 7.2 % with a >=128 PRB record, SNR p10/p50/p90 6.5/8.3/16.6, CPU 2.28, scanq 0/17544, pdschq 0/4230. SENSE_WBG: offered 47, admitted 25, taps 25, records 25, refunded 0, tap 313 us, ring 74 us/record. Metrics line carries sense_wbg_* and offered_wbg_overcap. Zero queue drops in both. The number of wide grants differs between arms (replay variation, as before).

s9 rfsim, sense_min_prb 96, 150 s, 4 RX [SIM VERIFIED, one arm]: pdsch_dmrs 14894, pdsch_data 3382, csirs 797, ssb 9570; 0 records with valid_cfr_prbs=0; violations 0; wbg offered 845, admitted 263, taps 263, 102 us per tap.

Concerns: (1) coverage unchanged by this round, still bounded by the startup and CRC gate; (2) a decode ERROR status leaks one token until the refill; (3) the new iq_ok gate is wbg-only: the existing unit tap still has no ring-lifetime check (pre-existing, not changed to keep sensing-unit behaviour as is); (4) the job-level R1 test is at the helper level (`nr_sense_wbg_tap_ok`), not a full consumer run; (5) test_nr_gpu_pass_geo flaked once.
