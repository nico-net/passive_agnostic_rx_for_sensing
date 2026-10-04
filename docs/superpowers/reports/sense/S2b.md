# S2b — S2 review fixes

## Changes and decisions

- [CODE-READ, DGX aarch64] `nr_isac.cc` activates its configured source mask after an exporter open with no new sink. The core opens in a sinkless mode without allocating rings or starting a writer; each submitted CFR frame increments `cfr_no_sink`. One warning names the absent `records_path`/`cfr_path`. Legacy `out_path`, `report_path`, and `report_endpoint` remain inert.
- [CODE-READ, DGX aarch64] Source parsing follows `05ac78beb9`: the seven original tokens select their individual old enum bits, including `pdsch_dmrs` without the blind bit. Empty or unknown `sources` falls back to the legacy single `source`; its default and unknown value select CSI-RS. `all` and `csirs` are not old tokens. Explicit empty `sources` invokes the same fallback. `subslot_symbols`, `subslot_min_re`, and `subslot_min_snr_db` again pass through to the existing receiver reconstruction call; retired algorithm keys still warn once.
- [CODE-READ, DGX aarch64] `nr_sense_push_cfr_u32` validates and packs receiver indices directly into a reserved frame slot under the existing ring mutex. The 138,600-byte static TLS conversion arrays are gone; no producer allocation was added. A wide index beyond its wire range and an antenna count beyond eight increment the existing `cfr_rejected`. The core allocates CFR slots for the actual configured RX antenna count. `[sensing] cfr_antennas=1` stores only the first antenna; the default stores all RX antennas.
- [CODE-READ, DGX aarch64] Default CFR ring depth is 32. With four RX and default `cfr_max_re=46,200`, its upper bound is `32 × (48 + 3×46,200 + 3 + 8×4×46,200) + 1024×4096 + 4×(32+1024) = 55,944,160 bytes` (53.35 MiB). Both `cfr_ring_frames` and `cfr_max_re` remain overrides. Sinkless mode allocates neither ring.
- [CODE-READ, DGX aarch64] Removed the deleted dashboard launch from `auto_acquire.py` and the dead detection fusion block in `run_passive_rx.sh`; corrected the merged CMake comment. Receiver CFR creation, CFO/SFO/DM-RS corrections, data-aided reconstruction, and disabled sensing gates were otherwise left in place.

## Verification

- [OFFLINE VERIFIED, DGX aarch64, red] `EnableWithoutSinkKeepsSourcesActive`, `SourceTokensMatchLegacy`, and the updated `SingleAntennaWrappersAndTruncation` were introduced before implementation. The original adapter failed the DM-RS bit expectation; log: `/tmp/sense_s2b_red.log`.
- [OFFLINE VERIFIED, DGX aarch64] Focused adapter suite: 10/10 cases passed after implementation (`flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/build/test_nr_isac_adapter --gtest_brief=1`); log: `/tmp/sense_s2b_adapter.log`.
- [OFFLINE VERIFIED, DGX aarch64] ON full target list built (`nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests`); logs `/tmp/sense_s2b_on_build.log` and `/tmp/sense_s2b_on_build_final.log`. OFF full target list built in `build_off`; logs `/tmp/sense_s2b_off_build.log` and `/tmp/sense_s2b_off_build_final.log`. Each Ninja command used `flock -s -w 7200 /tmp/td_measure.lock env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp nice -n 19 ninja -C cmake_targets/ran_build/<build> -j8 <targets>` after the BUSY check.
- [OFFLINE VERIFIED, DGX aarch64] Focused CTest: ON **4/4**, OFF **4/4** registrations passed (`test_nr_isac_adapter`, `test_nr_sense_api`, `test_nr_isac_ssb_source`, `test_nr_passive_cfg_epoch`). Logs: `/tmp/sense_s2b_targeted_on.log`, `/tmp/sense_s2b_targeted_off.log`. Command per mode: `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/<build> -j4 -R '^(test_nr_isac_adapter|test_nr_sense_api|test_nr_isac_ssb_source|test_nr_passive_cfg_epoch)$' --output-on-failure`.
- [OFFLINE VERIFIED, DGX aarch64] ON and OFF shuffled runs each passed **18/18 binary invocations, 855 cases passed, 48 documented skips**. Binaries: `test_nr_isac_adapter`, `test_nr_sense_api`, `test_nr_isac_ssb_source`, `test_nr_passive_cfg_epoch`, `test_dlsch_fixed_point`, `test_nr_pdcch_blind_monitor`; seeds **1/3/5**. Each invocation used `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/<build>/<binary> --gtest_shuffle --gtest_random_seed=<seed> --gtest_brief=1`; logs `/tmp/sense_s2b_<build>_<binary>_seed<seed>.log`.
- [OFFLINE VERIFIED, DGX aarch64] `bash -n tests/passive_rx/run_passive_rx.sh` and `python3 -m py_compile tests/passive_rx/auto_acquire.py` passed.
- [OFFLINE VERIFIED, DGX aarch64 sandbox] Full ON CTest: **140/149 passed**, with only the nine documented baseline failures: `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation` (ARM), and `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test` (sandbox sockets). `nr_td_sim_test` passed in 1602.15 s. Log: `/tmp/sense_s2b_ctest_on.log`. Command: `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure`. CTest exited 8 for those baseline failures.
- [OFFLINE VERIFIED, DGX aarch64] Deleted-algorithm source grep returned zero matches; `git diff --check` passed; sens6 frozen gate passed before commit. No rfsim, OTA, simulator campaign or GPU benchmark was run.

## DEFERRED MEASUREMENTS

- [NOT RUN, DGX aarch64] The brief reserves the 4-RX rfsim gate for the orchestrator. Run separately on an idle host with `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset:

  ```sh
  flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s2b_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
  flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s2b_gate_on GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources csi_rs,pdsch_dmrs,pdsch_data,pdsch_dmrs_blind,pusch_dmrs,pusch_data,ssb --sensing.cfr_path /tmp/sense_s2b_gate.cfr --sensing.records_path /tmp/sense_s2b_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
  flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s2b_gate_ant1 GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources csi_rs,pdsch_dmrs,pdsch_data,pdsch_dmrs_blind,pusch_dmrs,pusch_data,ssb --sensing.cfr_antennas 1 --sensing.cfr_path /tmp/sense_s2b_gate_ant1.cfr --sensing.records_path /tmp/sense_s2b_gate_ant1.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
  ```

## Open limits

- [IMPLEMENTED, NOT VALIDATED on live traffic] Task 5 must measure CFR overhead with all four antennas and with `cfr_antennas=1`. K45 scan-queue capacity and rank > 1 acceleration remain outside S2b.
