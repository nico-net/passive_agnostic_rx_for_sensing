# SENSE_SCHED gating fix (F1-F4), branch twomodes/schedfix

Base: twomodes/design 4e6315766b. Requirements: docs/superpowers/reports/twomodes/SENSE-SCHED-GATING-20261106.md.
Host: DGX GB10 aarch64. Evidence labels: [OFFLINE] unit tests, [SIM VERIFIED, DGX aarch64] rfsim 106 PRB 4 RX, [CODE-READ]. Nothing here is OTA.

## Diff summary (nr_sense_sched.c/.h, nr_sense_sched_rt.c; scheduler runs only in mode=sensing, legacy/traffic untouched)
- F1: new state `quiet` (decisions enabled in a row without CFR record growth) and `cfr_seen`. A unit with quiet >= SENSE_WIN_DECISIONS (4 decisions = 16 s, existing constant) AND an unknown/stale ratio (`nr_sense_unit_ratio_known` false) becomes ineligible; the existing shed (SENSE_CONFIRM) and SENSE_REPROBE_BASE..MAX back-off apply unchanged. Growth of n_cfr, a gen change or disabling resets the counter. A source producing >= 1 record per 16 s never trips it; a rarer one is re-probed on the back-off cadence and keeps what it exports.
- F2: unknown-ratio prior = mean known valid ratio of the same source in the same cell (floor SENSE_ELIG_VALID_MIN), 1.0 if the source has no measured unit. SRS prbs = the cell floor (T_cell), not est_usable_prbs/sense_min_prb.
- F3: a unit with streak > 0 that is not enabled (admitted last decision, unconfirmed) ranks with the INCUMBENT factor and does not count as cold for the one-cold-per-decision limit. A unit in hold-off also does not take the cold slot (it cannot be enabled; it still admits/counts streak exactly as before, so violation hold-off timing is unchanged).
- F4: `binding=` is only the resource that rejected a candidate, "none" otherwise (out->binding = NR_RES_N; the JSON metric already maps >=6 to "none"). New out->top (tightest resource), n_elig, cold_deferred; SENSE_SCHED line now prints `binding= top= elig= cold_deferred=`.
- PRACH not made a unit (follow-up, see below). No new env/CLI/-D/cfg keys, no per-cell constants.

## Tests [OFFLINE]
test_nr_sense_sched: 49 pass (42 old + 7 new, SchedGate.*): F1 zero-yield SRS shed and re-probed with back-off; F1 rare source re-probed and keeps records (stays on 30 decisions with a record every 3rd); F2 new PUSCH RNTI priced at the floor so a never-probed PDSCH wins; F2 SRS uses cell floor; F3 pending unit confirmed although a better cold unit appears; high-yield SRS (ratio 0.9) still beats low-yield PDSCH (ratio 0.4); F4 counters; replay of 923 S1 decisions 1-10 (inputs from SENSE_SCHED/SENSE_UNITS of sp_ota_20261105_225342/sp923S1: SRS wb ~598 no CFR, PDSCH wb as logged, one fresh PUSCH RNTI per decision at its logged value). 
Red/green: with the old nr_sense_sched.c and the new header, 7 of the new tests failed (replay: PDSCH admitted at decision 3, never enabled); with the fix PDSCH admitted at decision 3 and enabled at 4 (<= 3 decisions). Two old tests encoded old behaviour and were adjusted: Sched.NewUnitWithoutCfrIsNotDisabled (now 3 decisions; a full window without records is F1).
Replay caveat: PDSCH rows beyond the 8 logged per cell and the PUSCH rows are reconstructed from the logged admitted values (wb = val / (0.2*128*2)), not logged raw; the replay shows the mechanism, not exact OTA numbers.

## ctest [SIM VERIFIED, DGX aarch64]
Build: `ninja -k 0 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` rc 0, 0 FAILED; liboai_device.so symlink made. ctest -j4: 397 tests, 16 not passing, rerun-failed: test_nr_gpu_pass passed on rerun (flaky). Remaining 15: test_nr_pusch_uci_{mapcheck,ack_rm,csi,ack_csi,csi2_oai,small,narrow} Not Run (binary nr_pusch_ucisim is not in the `tests` target), test_nr_pusch_ra0_qam256, dft_test, test_nr_modulation (known ARM list), test_nr_pusch_ul_disc, nr_td_sim_test, test_nr_ssb_rate_match_prod, test_nr_gpu_pass_cells (on the S9 baseline list in the sensing-mode progress notes), test_ldpc_cuda_pool_cpu (SEGFAULT; LDPC CUDA, not touched by this diff, not run at base: unverified). All sched/unit/rt/metrics tests pass.

## rfsim A/B [SIM VERIFIED, DGX aarch64, 106 PRB cell, 4 RX, 150 s, ue.passive.sensing.rfsim.s9.cfg (the S9 bed), under flock -x heavy.lock + td_measure.lock, no receiver otherwise running]
Base binary = /home/nicola/NICOLA/wt/rr-twomodes build (twomodes/design f6e91f107e sources; confirmed old SENSE_SCHED format and build string in records). Fix = this tree 7b241c89.
| arm | ssb | csirs | pdsch_dmrs | pdsch_data | wideband records @96 | violations | first ON (dmrs/data decision) |
| base  | 9552 | 796 | 14036 | 3385 | 18217 | 0 | 11 / 13 |
| fix   | 9558 | 797 | 12359 | 3380 | 16536 | 0 | 14 / 12 |
| base2 | 9540 | 795 | 12338 | 3373 | (not scored) | 1 | 14 / 12 |
| fix2  | 9567 | 797 | 12365 | 3379 | (not scored) | 0 | 14 / 12 |
- Every source still valid (valid_cfr_prbs > 0 for all rs records of all four arms; scorer valid_rate 1.0 per link). No new violations (fix 0/0; base 0/1).
- PDSCH export: fix is within the base's own run-to-run spread. Base run 1 got dmrs ON at decision 11 (14036); base2 and both fix runs had pdsch_data win the cold slot first and dmrs ON at 14 (about 12.3k). fix2 == base2 within 0.2 %. The one "lower" number (fix vs base run 1) is startup ordering seen in the base itself; n=2 per arm, not a significance test.
- Cause of the ordering: with F2 an unprobed pdsch_data and pdsch_dmrs both start at prior 1.0 (equal value); pdsch_data then takes the cold slot and is later shed by the DA budget (binding=da). Base ordered dmrs first only when dmrs' ratio happened to be known. Not changed (ruling: never-probed = 1.0).
- The bed has no SRS/PUSCH traffic, so it is a regression check, not a reproduction of the 923/415 starvation. SENSE_SCHED now shows e.g. `binding=none top=da elig=2 cold_deferred=0`.
- Arm output: /home/nicola/NICOLA/wt/rr-schedfix/.superpowers/ab/{base,fix,base2,fix2} (records gzipped, CFR .bin deleted).

## Concerns / follow-ups
1. A test_nr_sense_sched build (one small target) was compiled while another agent's nr-uesoftmodem (rr-dmrsgate) was running; the full build and ctest started only after pgrep was empty for 60 s. That agent's timing run could have seen ~1 min of one compile.
2. Cold ordering when two sources are both unprobed (dmrs vs data): ties decide who gets the first cold slot (see rfsim). A cost-aware tie-break (cheaper source first) would be a possible follow-up; not done.
3. F1 uses "unknown ratio" as the zero-yield test (so a unit with fresh valid CFRs is never demoted); a unit exporting records with ratio < 0.2 stays on the existing ratio path.
4. PRACH as a scheduler unit: NOT done (as ruled). Follow-up: add NR_UNIT_PRACH (value = valid PRACH records/s, same F1/F2 rule), so zero-export PRACH is shed and releases its HOUSE lease.
5. Not tested on OTA or on sens6 captures; the report's check "rare SRS on sens6 captures" is open.
6. test_ldpc_cuda_pool_cpu SEGFAULT and ctest "Not Run" set were not compared against a base build.

---
# Fix round 1 (review: CHANGES REQUIRED), commit 734c5b23a3

## Changes
1. F2 prior now persistent: `pr_sum/pr_w[cell][src]` in nr_sense_sched_state_t. Each decision both decay by exp(-1/SENSE_WIN_DECISIONS) (time constant 4 decisions, no new constant), then every unit with a known ratio adds (ratio, 1), and every F1 zero-yield shed adds (0, 1) once (at the decision quiet reaches SENSE_WIN_DECISIONS). prior = mean, floored at SENSE_ELIG_VALID_MIN; a source never observed in the cell = 1.0. Cleared in nr_sense_sched_step on a cell reset (and by the memset at init). The old "known units now" prior is gone, so rotating RNTIs whose rows go stale or never produce a record no longer reset the prior to 1.0. The mean is not forgotten when there is no new evidence (only recent observations outweigh it).
2. Held-off units are skipped in admission: no want, no use, no n_elig, no streak. Side effect (intended by the ruling): a unit re-enters one decision later after its hold-off (violation hold-off test now expects D+4; Sched.StaleRatio... waits one more step; the DA test checks that DA was reported at the deciding step).
3. `top` is always computed (tightest use/budget over cells and resources); NR_RES_N ("none") when every budget is 0.
4. Tautological assertions replaced by values: F2 PDSCH admitted val 4000 = 10 x 1.0 x 200 PRB x boost 2; F4 top == pool, and top == none with all caps 0.
5. Known limit, rare sources: with the back-off capped at 64 decisions (256 s) and a probe window of about 6 decisions, a source emitting less than one record per 16 s is observed on ~6 % of the time at the cap (about 24 s of every ~280 s). Its records are exported in those windows and kept; between windows they are lost. Accepted for now.

## Tests [OFFLINE]
test_nr_sense_sched 52/52 (was 49): new F2RotatingPuschRntiDeadRowsAndSheds (stale dead rows, rotating RNTIs, zero-record row shed by F1; before the shed a new PUSCH is priced at 1.0 (40 x 128 x 2 = 10240), after it at the floor 2048 and PDSCH 3200 is first and enabled), F2RecoveringSourceGetsItsPriorBack (prior floor after sheds, > 0.7 after real records at ratio 0.9), HeldOffColdUnitsDoNotReserveBudgetNorOutrankProductive. The 923 S1 replay now also carries three stale dead PUSCH RNTIs per decision and still admits at decision 3 and enables at 4.

## ctest [SIM VERIFIED, DGX aarch64] rebuilt, `ninja -k 0 ...` rc 0, `flock -s`, no receiver
397 tests, 16 not passing, rerun-failed does not change them: test_nr_pusch_uci_* x7 (Not Run, nr_pusch_ucisim not in `tests`), test_nr_pusch_ra0_qam64 and qam256, dft_test, test_nr_modulation (known ARM), test_nr_pusch_ul_disc, nr_td_sim_test, test_nr_ssb_rate_match_prod, test_nr_gpu_pass_cells (S9 baseline list), test_ldpc_cuda_pool_cpu (SEGFAULT). test_nr_gpu_pass passed this time.
test_ldpc_cuda_pool_cpu on the base build (rr-twomodes, twomodes/design): SEGFAULT as well, so it predates this branch.

## rfsim A/B round 1 [SIM VERIFIED, DGX aarch64, 106 PRB, 4 RX, 150 s, s9 bed, both locks held, order base, fix, fix, base]
| arm | pdsch_dmrs | pdsch_data | csirs | ssb | non-ssb invalid rs | violations | first ON dmrs/data |
| base_1 | 14074 | 3381 | 800 | 9600 | 0 | 0 | 11 / 13 |
| fix_2  | 11226 | 2253 | 796 | 9558 | 0 | 0 | 16 / 14 |
| fix_3  | 12317 | 3382 | 796 | 9549 | 0 | 0 | 14 / 12 |
| base_4 | 12296 | 3377 | 792 | 9513 | 0 | 0 | 14 / 12 |
Every source valid, no violations. fix_3 == base_4 (12317 vs 12296, identical enable sequence). base_1 started early (dmrs ON at 11) like earlier base run 1; fix_2 started late (cell admitted two decisions later, identical behaviour afterwards). The differences between arms are the run start offset (2-5 decisions, seen in the base itself); with n=2 per arm and the previous round's 4 runs (fix 12359/12365, base 14036/12338) PDSCH export is not lower in a way beyond that spread. Honest limit: the bed has no SRS/PUSCH, and the dmrs-vs-data tie at prior 1.0 still gives the cold slot to pdsch_data first in both arms of the later runs.
Output: .superpowers/ab_r1/{base_1,fix_2,fix_3,base_4}.

---
# Fix round 2, commit 4e25cb45a6

## 1. Prior stuck at the floor (fixed)
After the decay each decision: pr_sum = pr_sum*dec + (1-dec)*1.0 and pr_w = pr_w*dec + (1-dec) (dec = exp(-1/SENSE_WIN_DECISIONS)), for every [cell][src]. The prior therefore relaxes toward 1.0 with tau = 4 decisions; pr_w is never 0, so the old "w > 0 else 1.0" branch and the near-denormal jump are gone. A source whose mean is 0 recovers as 1 - e^(-k/4) (0.78 at k=6).
Test F2FloorPriorRelaxesAndALateRealSrsGetsEnabled [OFFLINE]: prior state at the floor (mean 0), productive PDSCH incumbent (val 3200, pool holds one unit), a real SRS (wb 20) appears: priced 1024 at first, ENABLED AT DECISION 8 (asserted <= 12), PDSCH shed.
Consequence to know: in steady state with isolated sheds (one zero-yield unit shed every ~8 decisions) the prior sits around 0.5, not at the floor. The floor holds right after several sheds in the same window (rotating test now sheds four RNTIs together, new RNTI priced 2048 = floor at the next decision, PDSCH 3200 ranks first and is enabled within 3). Rotating-RNTI and recovering-source tests were adapted accordingly (the recovering one now probes one decision after the sheds: prior < 0.3, and > 0.7 after real records).

## 2. fix_2 pdsch_data 2253 (investigated)
Timeline of fix_2 (SENSE_UNITS + SENSE_SCHED, decisions 12-34), no PDSCH row ever carried ratio < 1.00 once ON:
- pdsch_dmrs: ON from decision 16 to the end, never off, never shed.
- pdsch_data: ON at 14-15, off 16-23 (shed at decision 15 by the DA budget, `binding=da`, da99 = 71 ms against a budget of 0.6 x 100 = 60 ms; hold-off 4; re-probed), ON again at 24-25, shed again, next probe would be 34-35 = after the end of the 150 s run.
- F1 never fired: F1 needs enabled >= 4 decisions with an unknown ratio; data was ON at most 2 decisions at a time and its ratio was known (1.00), dmrs ratio 1.00 with n_cfr growing. The sheds/hold-offs of pdsch_data are the pre-existing DA-budget re-probe cycle (period about 10 decisions), identical in the base: base_2 data ON at 15-16, 24, 32-33 (2817 records), base_4 in round 1 at 12-13, 21-22, 32-33 (3377).
- So the 33 % loss is window quantisation: each probe window exports about 1127 pdsch_data records; fix_2 started its first window 2 decisions later (cell sync offset), which pushed the 3rd window past the end of the run (2 windows = 2253, 3 = 3380). Not a regression, not an F1 or counting bug (n_cfr per row advances; the row/slot is the same in all arms).
- Consequence for the pass condition: "no PDSCH unit is ever shed or held off on the bed" cannot hold even at base, because pdsch_data (DA 71 ms) does not fit the DA budget (60 ms) on this bed (S7 limitation, noted in the S9 report). What holds: no shed or hold-off caused by F1 or by the prior, pdsch_dmrs never shed in any arm.

## A/B round 2 [SIM VERIFIED, DGX aarch64, 106 PRB, 4 RX, 150 s, s9 bed, both locks, order fix, base, fix, fix; run after fix (1)]
| arm | pdsch_dmrs | pdsch_data | csirs | non-ssb invalid | violations | dmrs ON from | data ON windows (decision) |
| fix_1  | 12289 | 3379 | 794 | 0 | 0 | 14 | 12-13, 21-22, 32-33 |
| base_2 | 12339 | 2817 | 798 | 0 | 1 | 14 | 12-13, 21, 31-32 (one 1-decision window: 2817) |
| fix_3  | 12327 | 3380 | 797 | 0 | 0 | 14 | 12-13, 21-22, 32-33 |
| fix_4  | 14017 | 3381 | 797 | 0 | 0 | 11 | 11-12(ish), 21-22, 32-33 |
Base spread over all 6 base runs so far: pdsch_data 2817-3385, pdsch_dmrs 12296-14074. Fix arms this round: data 3379-3381 (inside), dmrs 12289-14017 (inside). Every fix arm: all sources valid, 0 violations, dmrs never shed.
Arms in .superpowers/ab_r2/{fix_1,base_2,fix_3,fix_4}.

## Tests / ctest
test_nr_sense_sched 53/53 [OFFLINE]. ctest -j4 after rebuild (pgrep empty, flock -s): 397 tests, 16 not passing on the first pass (the usual list plus test_iq_chan, which passed on rerun-failed); second list: 15 = 7 uci Not Run, qam256, ul_disc, nr_td_sim_test, ssb_rate_match_prod, gpu_pass_cells, dft_test, ldpc_cuda_pool_cpu (SEGFAULT also at base), modulation. No sched-related failure.
