# T6a — passive PRACH detection and records

Host: DGX aarch64, 2026-10-04. `S1.md`, `S1b.md`, `S2.md`, the Sensing API plan, `CODEX_BRIEF.md`, and `nr_sense_api.h` were read. `S2b.md` is absent from this branch's report tree.

## Changes and decisions

- [CODE-READ, DGX aarch64] `nr_prach_passive.{c,h}` uses the SIB1 broadcast RACH configuration and OAI's 38.211 occasion lookup to build `prach_item_t` and call `rx_nr_prach_ru` plus `rx_nr_prach` on the passive UE's samples. The UE slot hook is in `nr-ue.c`; a missing SIB1 RACH config leaves the path inert and produces a single diagnostic log. Only TDD UL or mixed slots are examined. The PRACH source is enabled by `--sensing.sources prach` or `all` and costs one atomic source check when disabled.
- [CODE-READ, DGX aarch64] The detector uses preallocated sequence, frequency and frame-boundary sample buffers. It examines each broadcast time/frequency occasion, emits one peak above its energy threshold, and retains detections for up to 160 slots to associate an overheard RAR. `nr_pdsch_passive_queue.c` passes only fully validated RAR PDUs; every RAPID and TA in the PDU is collected. The join checks RA-RNTI, RAPID and occasion age, including SFN/HFN wrap. Long-format RA-RNTI uses the subframe index. A matching RAPID yields `rar_match:true`; another RAPID on the same RA-RNTI yields `false`; an absent RAR yields `null`. Shutdown drains pending records before closing the exporter.
- [CODE-READ, DGX aarch64] The small `format_record()` function builds one schema-2 `rs` JSON line per detection and calls `nr_sense_push_record`. It carries `source:prach`, `preamble`, energy in the gNB detector's dB×10 units, `delay_samples`, frame/slot/FDM/symbol occasion, absolute slot, epoch fields captured with the detection, RAR relation/RAPID/TA, and the common D4 record keys. Unknown CFR fields and unavailable RF timestamp are `null`. This local formatter is the single replacement point for Task 3's `nr_sense_emit_rs` at merge.
- [CODE-READ, DGX aarch64] `nr_prach.c` is linked into `PHY_NR_PASSIVE_UL` with its gNB scheduler-only entry point excluded. CMake links `nr_sense_api` explicitly so the sensing-OFF receiver still resolves the PRACH record call. The `prach` source uses bit 16 outside the CFR enum to avoid collision with Task 6b source growth. The repository's `set_default_nta_offset()` handles FR1 and FR2 timing.
- [CODE-READ, DGX aarch64] Tests cover occasion lookup, synthetic sequence peak and delay, RAR fixture/RAPID matching and slot wrap, long-format RA slot mapping, schema-2 record fields and `true`/`false`/`null` RAR values. Changes stay local to PRACH, the UE/RAR hooks, source gating, CMake and tests; no frozen sens6 file or `PROJECT_MEMORY.md` was edited.

## Verification

Every build and test checked the orchestration `BUSY` marker and took a shared `/tmp/td_measure.lock`. No receiver, rfsim bed, OTA run or benchmark was launched.

- [OFFLINE VERIFIED, DGX aarch64, red] The record-format and RAR slot-wrap tests were added before their implementations; `test_nr_prach_passive` failed to link with the expected missing test-seam symbols. The long-format RA slot test also failed to link before implementation.
- [OFFLINE VERIFIED, DGX aarch64] ON build of the full CLAUDE.md target list passed after the final PRACH changes:
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
  ```
- [OFFLINE VERIFIED, DGX aarch64] OFF configure/build passed for `nr-uesoftmodem`, `test_nr_prach_passive` and `test_nr_isac_adapter`; the two focused CTest registrations passed. The initial OFF link identified the missing `nr_sense_api` dependency, which was fixed before the successful build. The build directory was then restored to ON.
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S . -B cmake_targets/ran_build/build -DENABLE_ISAC_SENSING=OFF -DCPM_SOURCE_CACHE=/tmp/sense_s1_cpm
  flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem test_nr_prach_passive test_nr_isac_adapter
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 -R '^(test_nr_prach_passive|test_nr_isac_adapter)$' --output-on-failure
  ```
- [OFFLINE VERIFIED, DGX aarch64] ON shuffle seeds 1, 3 and 5 passed for `test_nr_prach_passive` (4 cases) and `test_nr_isac_adapter` (7 cases): six binary runs, 33 passing cases total.
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/build/<binary> --gtest_shuffle --gtest_random_seed=<1|3|5> --gtest_brief=1
  ```
- [OFFLINE VERIFIED, DGX aarch64 sandbox] Full ON CTest completed: **141/150 registrations passed**, including `test_nr_prach_passive`, `test_nr_isac_adapter`, and `nr_td_sim_test` (1628.88 s). The nine failures are exactly S2's documented ARM/sandbox baseline set: `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `time_management_tests`, `dft_test`, `test_nr_modulation`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. No new failing registration. CTest returned 8 for those failures. Command and log:
  ```sh
  flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
  # /tmp/sense_t6a_ctest_final.log
  ```
- [CODE-READ, DGX aarch64] `git diff --check` and the sens6 frozen-path gate passed during implementation and will be repeated immediately before commit.

## DEFERRED MEASUREMENTS

[NOT RUN, DGX aarch64] `CODEX_BRIEF.md` reserves rfsim and live beds for the orchestrator. Run the disabled and all-source 4-RX gates separately on an idle DGX host, with `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset:

```sh
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6a_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6a_gate_all GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources all --sensing.cfr_path /tmp/sense_t6a_gate.cfr --sensing.records_path /tmp/sense_t6a_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
```

[NOT RUN, OCUDU ZMQ bed] The existing launcher starts the passive receiver before srsUE attaches when `PX_FIRST=1`, allowing PRACH/RAR/Msg4 to be observed. Its external OCUDU and srsUE binaries and network setup must be available. From this worktree:

```sh
flock -x /tmp/td_measure.lock env PX_FIRST=1 PX_EXTRA='--sensing.enable 1 --sensing.sources prach --sensing.records_path /tmp/sense_t6a_ocudu.jsonl' tests/passive_rx/run_ocudu_passive.sh 300 /tmp/sense_t6a_ocudu
rg '"source":"prach"' /tmp/sense_t6a_ocudu.jsonl
rg 'RAPROC|RAR' /tmp/sense_t6a_ocudu/gnb.log
```

[NOT RUN, sens6 SA bed] The required on-air PRACH/RAR comparison with an attached UE awaits the operator's R13 SA bring-up and runbook, which is absent from this branch. Compare each exported preamble/occasion/delay and `rar_match`/TA with the gNB validation log. No sens6 or OCUDU ground-truth claim is made here.

## Open limits

[IMPLEMENTED, NOT LIVE VALIDATED] The energy threshold and 160-slot RAR wait require live calibration. The current module skips a long PRACH occasion that begins in the previous radio frame; the detector inherits OAI's supported PRACH formats and sample rates. Heavy concurrent slot processing may skip an occasion because the correlator uses one preallocated workspace. PRACH emits records only, with no CFR. K28 live UL coverage, CPU overhead, regression gates and rank > 1 acceleration behavior remain unmeasured in T6a.
