# SPS — bounded blind SRS CPU cost

Host: DGX aarch64. Branch: `sense/perf`, base `827a5e31ca`.

[MEASURED, DGX aarch64 rfsim 106 PRB 4 RX 420 s @827a5e31ca, one run per arm; supplied by the operator] `api_off` used 893 receiver CPU-s and all nine sources used 1264 (+41%). In the all-source arm, `sense_prod_us_srs` accumulated 351 s. That bed has no uplink, so blind SRS searches cannot succeed there. The four cited DL source-only arms were each about +1%; they do not explain the all-source overrun.

## Changes and decisions

- [CODE-READ, DGX aarch64] `nr_srs_passive.c` captures only the final six symbols marked UL in a known TDD table. The worker applies a comb-energy gate against the local off-comb floor before sequence search at each timing offset. Empty scans back off through 1/2, 1/4, …, 1/64 of eligible UL slots; a comb candidate, confirmed observation, RRC hint or DCI SRS request resets that backoff. Eligible-slot counting avoids a TDD-period alias that could starve search forever. On an allowed scan, the nine timing offsets remain available to detect delayed SRS.
- [CODE-READ, DGX aarch64] The worker accounts thread CPU in a rolling one-second window and stops work when the allowance is exhausted. `ISAC_SRS_CPU_PCT` sets the percentage of one core (integer 1–100; default 5). Checks occur before each timing offset and sequence candidate. A single in-flight FFT or candidate can overshoot the allowance; later work waits for the sample to leave the window. `srs_budget_skip`, `srs_backoff_skip`, `srs_pregate_skip`, and `srs_idle_resets` are cumulative `ISAC_METRICS` counters. `srs_us` remains the worker CPU measurement; `sense_prod_us_srs` remains the producer timing diagnostic.
- [CODE-READ, DGX aarch64] After a resource is confirmed, only its inferred occasions and geometry are searched; its saved group and cyclic shift are tried first. Four missed occasions return it to blind search; stale and illegal recurrence gaps cannot keep tracking mode active. Hints remain optional and SRS identity/attribution rules are unchanged.
- [CODE-READ, DGX aarch64] Files: `nr_srs_blind.{c,h}`, `nr_srs_blind_internal.h`, `nr_srs_passive.{c,h}`, `nr_passive_metrics.{c,h}`, `nr_passive_metrics_json.c`, and SRS/metrics tests under `openair1/PHY/NR_UE_TRANSPORT/tests/`.

## Validation

- [OFFLINE VERIFIED, DGX aarch64] Test-first red: the new SRS policy tests failed to compile before their APIs existed. Both final ON and OFF target-list builds passed with `CCACHE_DISABLE=1`, `nice -n 19`, `ninja -j8` and a per-command shared `/tmp/td_measure.lock`; logs `/tmp/sps_build_final_build.log` and `/tmp/sps_build_off_final_build.log`.
- [OFFLINE VERIFIED, DGX aarch64] Final focused CTest: eight registrations passed in each mode (`/tmp/sps_build_final_focused.log`, `/tmp/sps_build_off_final_focused.log`). Shuffle seeds 1/3/5 passed for eight focused binaries in each mode, 48/48 invocations; logs `/tmp/sps_final_<mode>_<binary>_<seed>.log`. The SRS binary has 31 passing cases, including the three requested named regressions. Each test command held its own shared lock and checked the orchestration BUSY file.
- [OFFLINE VERIFIED, DGX aarch64] Final full ON CTest excluding the separately registered long simulation completed 143/152 registrations in 236.77 s; its nine failures were the known ARM cases `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation` and sandbox cases `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. Log: `/tmp/sps_ctest_final_fast.log`.
- [OFFLINE VERIFIED, DGX aarch64] The separate long `nr_td_sim_test` passed 1/1 in 1597.29 s (`/tmp/sps_ctest_td_sim.log`). Together the two commands cover all 153 CTest registrations: 144 passed and nine known failures. The last change to confirmed-resource hint selection was rebuilt and passed the final focused and shuffled SRS suites in both ON and OFF modes; it does not link into the long simulator.

Exact build/test commands, run only after the BUSY file was absent and under a separate shared lock each time:

```sh
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build_off -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -E '^nr_td_sim_test$' --output-on-failure
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -R '^nr_td_sim_test$' --output-on-failure
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -R '^(test_nr_srs_blind|test_nr_passive_metrics)$' --output-on-failure
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/build/test_nr_srs_blind --gtest_shuffle --gtest_random_seed=1 --gtest_brief=1
```

The last command was repeated with seeds 3/5 and in `build_off`; the focused CTest and other shuffled binaries were likewise repeated in each mode.

## DEFERRED MEASUREMENTS — orchestrator

[NOT RUN, DGX aarch64] The lock policy reserves rfsim timing and the exclusive lock for the orchestrator. On the idle host, after integration, run the paired 420 s off and all-source arms below. Compare receiver user+system CPU in `base_r1/rx/time.txt`; require gate PASS, no new CFR/record drops, and all-source CPU ≤ 1.10 × the paired off arm. Subtract the first and last `ISAC_METRICS` samples for `sense_prod_us_srs`, `srs_us`, and the new skip/reset counters. Check the CFR/record links with `read_sense.py`.

```sh
flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO -u ISAC_RECONF OUT=/tmp/sense_sps_api_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 1
flock -x -w 7200 /tmp/td_measure.lock env -u ISAC_RX_BRANCH_FO -u ISAC_RECONF OUT=/tmp/sense_sps_api_all GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources csi_rs,pdsch_dmrs,pdsch_dmrs_blind,pdsch_data,pusch_dmrs,pusch_data,ssb,prach,srs --sensing.records_path /tmp/sense_sps_api_all.jsonl --sensing.cfr_path /tmp/sense_sps_api_all.cfr --sensing.cfr_antennas 4 --sensing.cfr_ring_frames 32 --sensing.cfr_max_re 46200 --sensing.rec_ring_slots 1024' tests/passive_rx/dgx/rfsim_regress.sh 1
python3 tools/sense_api/read_sense.py /tmp/sense_sps_api_all.jsonl /tmp/sense_sps_api_all.cfr
```

## Open limits

[IMPLEMENTED, NOT LIVE VALIDATED] The +10% all-source CPU target awaits the orchestrator's re-measure. Backoff and the 5% CPU allowance can delay first detection when SRS begins on an otherwise idle cell. The one-second CPU allowance is checked between operations, so a single operation can exceed it briefly. The existing unsupported SRS sequence-hopping v=1 case and live UL validation dependency remain.
