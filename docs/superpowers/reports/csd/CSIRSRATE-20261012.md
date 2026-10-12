# CSIRSRATE 2026-10-12: ISAC_CSIRS_DISC_RATELIMIT (branch csd/csirs-ratelimit, base csd/otatest 4cbe1510e2)

Evidence labels (PROJECT_MEMORY 0.1): [unit] = DGX aarch64 gtest; [CODE-READ]; [INF] = inference. NO replay was run: the harness denied the replay launch (see 4). Nothing here is measured on a capture.

## 1. What it does (default off, flag-off path unchanged) [CODE-READ]
Discovery cost is proportional to the ingest duty (`nr_csirs_disc_td_push`: contiguous windows of 640 slots, 1 in 7 after a 2.56 s acquisition burst), because each fed window runs FEP + normalize + sweep/refine. ISAC_CSIRS_DISC_RATELIMIT=1 adds a state machine (`nr_csirs_disc_rate_internal.h`, called from `nr_csirs_disc_process`) that changes that duty:
* SEARCH = existing full-rate behaviour (burst, then 1 window in `div`). Time-to-confirm is therefore untouched until the set is confirmed.
* STABLE when >= 1 resource of this module is confirmed (`nr_csirs_blind_rt_discovery_confirmed`) and the set has been unchanged for max(2 search cycles, 2x the time taken to build the set, 4 occasions of the slowest period). Ingest becomes G windows per div_s. These windows run the unchanged pipeline, so they are the cheap re-check and the slow background search for new resources (confirmed hypotheses are already skipped by the sweep).
* Derived rates, no capture constants: G = ceil(4*maxP/win); m = ceil(ln(miss)/ln(1-pdet)) (miss 0.01, pdet 0.5 -> 7 checks); check cycle = notice/m (notice 60 s default); div_s = max(ceil(cycle/win), G*div), so the duty never exceeds the SEARCH duty. Env: ISAC_CSIRS_DISC_RATELIMIT_{NOTICE_S,MISS,PDET,CYCLES}. pdet=0.5 is an assumed per-check detection probability [INF], not measured.
* Escalation to SEARCH (and re-arm of the acquisition burst): a confirmed resource no longer confirmed; confirmed count changes; a detector footprint outside the footprints learned during SEARCH and at least as strong as the weakest confirmed one; configuration epoch / ring rewind; PDSCH CRC rate drop (3-sigma one-sided over disjoint blocks of >= 200 decodes against the rate between the last change and STABLE; counters from `nr_pdsch_passive_queue_get_stats`, weak). The "correlated with CSI-RS REs" qualifier is not implemented: the CRC trigger is global.
* Compatible with ISAC_CSIRS_DISC_NORM_COALESCE (kernel untouched). Only files owned by this task were edited (nr_csirs_disc.c, nr_csirs_disc_td.[ch], new header, new test, CMakeLists test target).

## 2. Unit [unit, DGX aarch64]
`test_nr_csirs_disc_rate` 4/4 PASS (derivation of checks/G/div_s, never above baseline duty, each escalation trigger, CRC noise does not trip). `test_nr_csirs_disc_td` PASS (3 pass, 3 disabled as before).

## 3. Known risks [CODE-READ]
* "Confirmed" is tested with the exact hypothesis the module admitted; if blind_rt later widens/merges the resource the match fails and the machine reports `confirmed-lost` and stays in SEARCH (safe, no saving). Must be checked on the first replay (log line `RATELIMIT STABLE/SEARCH reason=`).
* On a 12 s capture the confirmed set stabilises late, so little of the replay is in STABLE with the default 60 s notice; use `ISAC_CSIRS_DISC_RATELIMIT_NOTICE_S=8` to exercise the mechanism in a replay (ratio effect, not a default).

## 4. Not done
Replay A/B (b = raw_pci415_12s_b, tile3 = raw_pci923_tim_12s_tile3, off vs on, >= 3 runs/arm, nsys discovery GPU share, PDTIM mean/p90/p99, confirmed set / time-to-confirm, CRC-OK): the replay launch was denied by the harness classifier, so no run was made and nothing is claimed. Runner script used for the attempt: /tmp/crl_run.sh (wraps tests/passive_rx/timacq_replay.sh SPEED=1 ENVFILE=ota/t26y.env, `ISAC_CSIRS_DISC_RATELIMIT=1` for the on arm).

## OTA env delta
None required (default off). To trial after a replay A/B: add `ISAC_CSIRS_DISC_RATELIMIT=1` to ota/t26y.env (single line).
