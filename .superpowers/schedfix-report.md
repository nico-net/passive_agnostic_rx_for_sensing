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
