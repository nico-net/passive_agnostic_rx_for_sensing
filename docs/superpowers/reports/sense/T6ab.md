# T6a-b — passive PRACH review fixes

Host: DGX aarch64, 2026-10-05. Scope: Task 6a review of `68a121bfe4`; no live RF bed was run.

## Changes and decisions

- [CODE-READ, DGX aarch64] `nr_prach_passive.c` now seeds each FDM noise floor with eight complete occasions without detection, then compares each max-correlation energy with the lower quartile of up to 32 recent maxima plus `ISAC_PRACH_MARGIN_DB10` (default 80 dB×10). It uses no absolute energy cutoff. `nr-ue-ru.c` publishes applied RX gain changes so the floor seeds again after AGC. The source remains default off.
- [CODE-READ, DGX aarch64] The slot hook selects the broadcast occasion, snapshots its needed time-domain interval and metadata into a four-slot preallocated ring, and drops/counts work when full or a producer is already active. The worker starts before UE slot threads. It runs sequence generation, `rx_nr_prach_ru`'s copied-buffer receive path, correlation, pending RAR joins and record export. `nr_prach.c` supplies exact sample bounds and the copied-buffer index variant. `prach_us` in `ISAC_METRICS` is mean worker microseconds per processed time/frequency occasion; `prach_drops` counts queue/producer drops, and `prach_skipped` counts unsupported or unsafe occasions.
- [CODE-READ, DGX aarch64] FDD logs `PRACH passive inactive: FDD not supported` once. Mixed-slot PRACH is skipped and counted if any occupied symbol is DL; long formats conservatively require UL/flexible symbols through their final slot. A long occasion whose start is in the previous frame is logged once and counted when one is scheduled. `ISAC_TD_IGNORE_SIB1=1` explicitly makes PRACH inert even if a cached SIB1 prior exists.
- [CODE-READ, DGX aarch64] Shutdown now joins the UE slot producer, drains and joins the PRACH worker, flushes pending RAR records, then closes the sensing exporter. This removes the producer/worker/pending race.
- [OFFLINE VERIFIED, DGX aarch64] Tests compare FR1 unpaired rows 0, 67, 77, 145 and 189 with [ETSI TS 38.211 v17.4.0 Table 6.3.3.2-3](https://www.etsi.org/deliver/etsi_ts/138200_138299/138211/17.04.00_60/ts_138211v170400p.pdf): SFN x/y, subframe and 30 kHz PRACH-slot position, format, starting symbol, time occasions and duration. The same `PrachOccasionTable` test checks broadcast msg1-FDM separately from the 38.211 time table by counting worker correlator calls for two FD entries. Process-slot tests inject OAI-root synthetic time-domain preambles through the queue and previous-frame stitch, then check preamble zero and delay within one TA unit. Other tests cover an idle floor across gain steps, a preamble above floor, worker isolation/drain, SIB1 ignore, mixed-slot DL filtering and FDD inactivity.

- [CODE-READ, DGX aarch64] Files: `nr_prach_passive.{c,h}` and `nr_prach.c` hold detection/queue/sample-copy logic; `nr-ue-ru.c` and `nr-uesoftmodem.c` carry gain and shutdown hooks; `nr_passive_metrics.{c,h}` and `nr_passive_metrics_json.c` export counters; `nr_prach_passive_test.{cc,bridge.c,bridge.h}` and `nr_passive_metrics_test.cc` provide regression coverage.

## Verification

Every build and test checked `/home/nicola/NICOLA/wt/rr-orchestration/BUSY` and took a shared `/tmp/td_measure.lock`; commands ran with `nice -n 19`, builds with `-j8`, CTest with `-j4`.

- [OFFLINE VERIFIED, DGX aarch64, red] `test_nr_prach_passive` failed to link with the missing floor test seam before implementation (`nr_prach_passive_floor_reset_test`, `nr_prach_passive_floor_test`). The first synthetic slot run also exposed a fixture NCS mismatch; correcting it made preamble and delay assertions pass.
- [OFFLINE VERIFIED, DGX aarch64] Sensing OFF configure/build passed for `nr-uesoftmodem test_nr_prach_passive test_nr_isac_adapter test_nr_passive_metrics`; focused CTest **3/3 passed** on the final source:
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S . -B cmake_targets/ran_build/build -DENABLE_ISAC_SENSING=OFF -DCPM_SOURCE_CACHE=/tmp/sense_s1_cpm
  flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem test_nr_prach_passive test_nr_isac_adapter test_nr_passive_metrics
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -R '^(test_nr_prach_passive|test_nr_isac_adapter|test_nr_passive_metrics)$' --output-on-failure
  ```
- [OFFLINE VERIFIED, DGX aarch64] Sensing ON full target-list build passed:
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S . -B cmake_targets/ran_build/build -DENABLE_ISAC_SENSING=ON -DCPM_SOURCE_CACHE=/tmp/sense_s1_cpm
  flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
  ```
- [OFFLINE VERIFIED, DGX aarch64] ON focused CTest **3/3 passed** on the final source. Shuffle seeds 1, 3 and 5 passed for `test_nr_prach_passive` (13 cases), `test_nr_isac_adapter` (7 cases) and `test_nr_passive_metrics` (10 cases): nine runs, 90 passing cases:
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/build/<binary> --gtest_shuffle --gtest_random_seed=<1|3|5> --gtest_brief=1
  ```
- [OFFLINE VERIFIED, DGX aarch64 sandbox] Full ON CTest completed **142/150 registrations passed** in 1597.95 s, including `test_nr_prach_passive`, `test_nr_passive_metrics` and `nr_td_sim_test`. The eight failures were exactly the documented ARM/sandbox baseline set: `test_nr_pusch_ra0_qam256`, `time_management_tests`, `dft_test`, `test_nr_modulation`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. `test_nr_pusch_ra0_qam64` passed this run. The full run preceded the last PRACH window-bound, worker-start and metrics-test assertions; the final source was then rebuilt ON/OFF and the three affected focused registrations plus shuffle matrix passed. Command and log:
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
  # /tmp/t6ab_ctest_full.log
  ```
- [OFFLINE VERIFIED, DGX aarch64] `git diff --check` and the sens6 frozen-path gate passed immediately before commit.

## DEFERRED MEASUREMENTS

[NOT RUN, DGX aarch64] The lock policy reserves rfsim and live beds for the orchestrator. Run the 4-RX disabled and all-source gates separately on an idle host, with `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset:

```sh
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6ab_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6ab_gate_all GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources all --sensing.cfr_path /tmp/sense_t6ab_gate.cfr --sensing.records_path /tmp/sense_t6ab_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
```

[NOT RUN, OCUDU ZMQ bed] Compare on-air PRACH/RAR/Msg4 with gNB validation logs:

```sh
flock -x /tmp/td_measure.lock env PX_FIRST=1 PX_EXTRA='--sensing.enable 1 --sensing.sources prach --sensing.records_path /tmp/sense_t6ab_ocudu.jsonl' tests/passive_rx/run_ocudu_passive.sh 300 /tmp/sense_t6ab_ocudu
rg '"source":"prach"' /tmp/sense_t6ab_ocudu.jsonl
rg 'RAPROC|RAR' /tmp/sense_t6ab_ocudu/gnb.log
```

## Open limits

[IMPLEMENTED, NOT LIVE VALIDATED] The margin and measured worker cost require live calibration. TS 38.211's current FR1 unpaired long-format rows end within the frame, so the previous-frame-start skip is a defensive path without a reachable table fixture. The previous-frame sample stitch is exercised with a test-only A1 occasion at slot zero. Live UL coverage, 4-RX regression and AGC behavior remain unmeasured. The merged-main Technique D defaults and rank > 1 elimination limitation are unchanged.
