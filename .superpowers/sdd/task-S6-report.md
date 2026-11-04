# Task S6 report (sensing scheduler decision, pure)

Status: DONE. Branch sense/s1fix. Files: nr_sense_sched.{h,c}, tests/nr_sense_sched_test.cc (openair1/PHY/NR_UE_TRANSPORT, next to the S5 unit test), CMakeLists.txt (nr_sense_sched.c in nr_sense_api; test_nr_sense_sched), S5 view field est_usable_prbs (nr_sense_unit.{h,c}, 1 line each + 1 test assertion).
Tests (cloud/DGX aarch64 build dir of this worktree): test_nr_sense_sched 19/19; ctest -j4 -R "sense|isac" 10/10; 24 .so kept; frozen check exit 0.
Red first: 18 of 19 failed on the stub (NarrowCarrier passes on a stub by construction). Mutations: no incumbent boost -> HysteresisHolds fails; CONFIRM->1 -> Streak, Hysteresis, MaxTwo, Violation, SlotsReleased fail; no enable cap -> MaxTwoEnables fails.

Design choices (please review)
- Hysteresis: enabled units rank x1/(1-HYST) (=1.25). Equivalent to "disable only if v < (1-HYST) x marginal admitted"; units that fail on resources (lost headroom) or are ineligible are disabled after the streak.
- Streak: per unit, counts decisions where wanted != enabled; changes at SENSE_CONFIRM. Enables capped at 2 per decision (pending ones keep their streak) and an enable must also fit next to the units that stay on. Disables unlimited.
- Violations: delta of the cumulative counter (re-baselined on gen change/new row); the lowest-value enabled unit of the violator's group is disabled now, streak reset. Group = QUEUE_DL for PDSCH_DATA, QUEUE_UL for SRS (per cell), POOL (all cells) for DM-RS. One removal per group per decision.
- Cold unit (not enabled, cpu_p99 = wait_p99 = 0): demand = largest p99 of the same source in its cell; max one cold unit wanted per decision.
- Queues: charged the MAX of admitted waits (waits do not add); others sum. cap_ns <= 0 = resource absent (unconstrained).
- cpu_max_ns is NOT used: the p99 is the stated demand in the brief (a single burst bucket is deliberately ignored); burst protection is the violation path.
- Value uses the view's new est_usable_prbs (latest, not a mean); SRS has none, so min_prb is used. Age ref = last_valid_wb_ns, else first_ns.

Concerns for S7
- valid_ratio is 0 until the first CFR record: a new enabled unit with no CFR yet is ineligible and will be disabled after 2 decisions. S7 should give new units a grace or S5 should expose n_cfr.
- GPU and RING demand are cpu_p99 proxies (no per-unit GPU/ring time in the view). Caps are per 50 ms bucket in ns; S7 must set them (and cap 0 for absent GPU).
- late/skips counters are not used by the decision (only violations).
- Value is computed per decision from the window rates; no calibration on DEIB/OTA (headroom 0.40).

## Fix round 1
- I1: S5 view gains `n_cfr` (CFR records since row creation: a single atomic already exists, a window count would need per-bucket storage). S6: n_cfr == 0 passes the ratio test (value uses the floor 0.2 as ratio); once n_cfr > 0 the floor applies. Tests NewUnitWithoutCfrIsNotDisabled (+ below-floor unit disabled), unit test for n_cfr. Mutation (drop the n_cfr==0 pass) fails it.
- I2: eligibility = wb_rate_hz > 0 (+ratio rule); prbs = max(est_usable_prbs, min_prb, 1). nr_sense_unit_eligible lost its min_prb arg. Test AlternatingGrantSizesDoNotFlap. NarrowCarrier/IneligiblePci now use wb_rate_hz = 0 (what S5 produces).
- (a) unit and PCI age restart when they become eligible again (state: elig_since, pci_elig_since); first sight is unrestricted. Test AgeRestartsAfterIneligible (mutation fails). The boost stays 1, not 0, when ineligible (value is 0 anyway).
- (b) unit x PCI boost capped at SENSE_AGE_CAP total. Test CombinedBoostCappedAt2x (mutation fails).
- (c) violation-removed unit: SENSE_VIOL_HOLDOFF = 2 decisions; earliest re-enable is D+3 (streak counts meanwhile). Test ViolationHoldoffTwoDecisions (mutation fails).
- (d) cap <= 0/NaN: absent only for GPU; others admit nothing using that resource, out->bad_cap counts them. Test BadCapIsInvalidExceptGpu. S7 must set all caps (queue/ring too).
- M5 min_prb < 1 clamped via prbs >= 1; M6 value_last_admitted is now the minimum admitted value.
- Documented, not changed: queue max-aggregation only rejects a unit whose own wait exceeds the budget (aggregate load is bounded by POOL); cold unit with no same-source history costs 0 (one per decision); p99 ignores units active in <1 % of buckets; cand_t C[2048] ~250 KB on the stack (S7 thread needs >= 1 MB stack).
Tests: test_nr_sense_sched 25/25, test_nr_sense_unit 20/20, ctest -R "sense|isac" all pass, 24 .so, frozen exit 0.

## Fix round 2 (N1: lock-out of a unit disabled for a low ratio)
- S5: view gains `last_cfr_ns` (latest CFR record). The ratio EWMA was already initialised by the first record (n_cfr == 0 branch); now pinned by test RatioInitialisedByFirstRecord (first invalid record 0.1, then valid records pull it above 0.9).
- S6: nr_sense_unit_ratio_known(v, now): n_cfr > 0 and last CFR not older than SENSE_WIN_DECISIONS x SENSE_DECIDE_NS (16 s). Otherwise the ratio is unknown again (passes, value uses the 0.2 floor). nr_sense_unit_eligible now takes now_ns.
- Tests: StaleRatioBecomesUnknownAndUnitIsRetried (off for low ratio, still off before 16 s, eligible after, re-enabled only after a streak of 2, then bad CFRs again: off after a streak and not back within 3 decisions); UnitProducingInvalidCfrsStaysOff. Mutation (staleness removed) fails the first.
- Tests: sched 27/27, unit 21/21, ctest -R "sense|isac" all pass, 24 .so, frozen exit 0.
- Residual: a retried unit that is still bad is on for >= 2 decisions (streak) before being judged again; cycle period is >= 16 s window + streaks.
