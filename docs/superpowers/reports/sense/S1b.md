# S1b — sensing API review fixes

Host: DGX aarch64, 2026-10-04. Evidence labels follow `PROJECT_MEMORY.md` §0.1.

## Changes

- [CODE-READ] `nr_sense_api.c` now retries `EINTR` and completes short writes, alternates between queued CFR frames and records, assigns IDs before the ring-full check, and counts invalid CFR pushes separately from ring drops. ZMQ send failures and records with embedded newlines are counted. An endpoint on a build without ZMQ issues one `LOG_W` when logging is initialized.
- [CODE-READ] `nr_sense_api.h` documents that a frame ID gap means dropped frames and adds `cfr_rejected` and `zmq_drops` statistics plus deterministic writer test hooks.
- [OFFLINE VERIFIED, DGX aarch64] `nr_sense_api_test.cc` adds `FrameStreamSurvivesPartialWrites` using a FIFO, `InvalidPushCounted`, `RecordsNotStarvedUnderCfrLoad`, `DroppedFramesLeaveIdGaps`, and embedded-newline coverage. `SlowWriterNeverBlocksProducer` keeps its exact drop-count check and removes its 50 ms host-timing assertion.
- [CODE-READ] `CMakeLists.txt` links the API with the logging targets for the no-ZMQ warning path.

## Tests

- [OFFLINE VERIFIED, DGX aarch64] The regression tests were written before the implementation. The first red-build attempt stopped at ccache's read-only directory before a test binary was produced; the failing assertions were therefore not captured as a test run.
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests` — pass. `CCACHE_DISABLE=1` was needed because this sandbox reports its ccache directory read-only.
- [OFFLINE VERIFIED, DGX aarch64] `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ./test_nr_sense_api` — 12/12 pass. The same binary with `--gtest_shuffle --gtest_random_seed=1`, `3`, `5` and `--gtest_brief=1` — 12/12 pass for each seed.
- [OFFLINE VERIFIED, DGX aarch64 sandbox] Full `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest -j4 --output-on-failure` was interrupted after 148/149 registrations: 139 passed and nine failed in the documented baseline set (`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `time_management_tests`, `dft_test`, `test_nr_modulation`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`). The unfinished `nr_td_sim_test` was then run as `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest -R '^nr_td_sim_test$' --output-on-failure` and passed in 1597.38 s. Combined coverage: **140/149 passed, nine documented baseline failures**. The sensing API registration passed in the full run.
- [CODE-READ] `git diff --check` and the sens6 frozen-path diff check — pass.

## DEFERRED MEASUREMENTS

[NOT RUN, per shared-machine policy] Orchestrator 4-RX rfsim gates from this worktree:

```sh
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s1b_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s1b_gate_all GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources all --sensing.cfr_path /tmp/sense_s1b_gate.cfr --sensing.records_path /tmp/sense_s1b_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
```

## Open issues

- [NOT RUN] Live traffic, GPU, and the two 4-RX gates remain for the orchestrator. The merged-main convergence defaults and `ISAC_RECONF` default remain unchanged; this API work does not address rank > 1 elimination grants, K45 scan-queue drops, or the ~3 CB0 candidates per grant.
