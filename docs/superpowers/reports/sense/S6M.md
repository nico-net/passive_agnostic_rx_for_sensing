# S6M — PRACH/SRS integration into sense/api

[CODE-READ, DGX aarch64] Base: `c5edc3734a`. Merged `sense/t6a-prach` at `757d078538` in `89c0349227`, then `sense/t6b-srs` at `f24b75b014` in `b34c36de32`, both with `--no-ff`. One following integration commit contains the semantic fixes, regressions and this report. No push, other-worktree edit, operator-plan edit or frozen-file edit was performed.

## Conflict resolutions

[CODE-READ, DGX aarch64] PRACH merge, all four conflicted files:

- `CMakeLists.txt`: retained T3's records/observation objects, public exporter dependency and epoch library; added the passive-UL exporter dependency.
- `executables/nr-ue-ru.c`: retained both includes and both gain-change actions: T3 session emission and PRACH floor reset.
- `executables/nr-uesoftmodem.c`: retained startup session emission and added PRACH startup. Kept T6a-b's shutdown ordering: join slot producers, drain PRACH, close exporter.
- `nr_isac.cc`: retained S2b token parsing (`csi_rs`, separate ordinary/blind DM-RS bits; no `csirs`/`all` aliases). PRACH's explicit token and initial worker gate were retained; the bit-16 workaround was removed in the integration fix.

[CODE-READ, DGX aarch64] SRS merge, all seven conflicted files:

- `CMakeLists.txt` (three conflicts): retain both `NR_PASSIVE_PRACH_RX_ONLY` and `NR_SRS_PASSIVE_EXTRACT_ONLY`; retain T3 exporter objects/dependencies plus `NR_SRS_EXPORT_ON`; retain both gating and SRS test registrations.
- `executables/nr-ue.c`: run both PRACH capture and SRS capture after the existing PUSCH hook.
- `executables/nr-uesoftmodem.c` (three conflicts): retain both worker headers, startup session and both worker starts. Reject the early SRS/exporter shutdown block; drain SRS after producers join and before exporter close, alongside PRACH.
- `nr_pusch_data_aided.{c,h}`: the two branches' added argument describes the same actual FFT-window advance. Use one argument for S34 applied-TA records and T6b-b export-only phase correction; its final name is `ta_applied_samples`.
- `nr_pusch_passive_decode.c` (three conflicts): retain the SRS-evidence OR gate alongside the T4 source gate; retain the T3/S34 metadata-aware submission and append SRS evidence posting; pass the S34 actual applied advance to data-aided export.
- `nr_sense_api.h`: retain T3's unique appended PRACH and SRS IDs and S2b's `cfr_max_ant`, adding T6b-b's index/UL phase-reference helpers and documentation.

## Semantic decisions

[CODE-READ, DGX aarch64] There is one `nr_sense_src_t`: SSB=0, CSI-RS=1, PDSCH DM-RS=2, PDSCH data=3, PUSCH DM-RS=4, PUSCH data=5, PRACH=6, SRS=7. Thus the six established CFR v1 IDs are unchanged. The legacy receiver enum keeps its seven existing ordinals and appends PRACH=7/SRS=8. Mask bits always use that receiver enum; one checked mapping translates to wire IDs. PRACH remains record-only.

[CODE-READ, DGX aarch64] Both new sources require exact explicit `sources` tokens, `prach` and `srs`; SRS startup now uses the same adapter source mask as PRACH. Missing/default/unknown `sources` retains S2b's legacy `source` fallback (default CSI-RS). **`all` remains an unrecognized legacy token**, so `all` alone selects that fallback and enables neither new worker. `all,srs,prach` enables the two explicit new tokens, not the seven legacy tokens. To enable every source use `csi_rs,pdsch_dmrs,pdsch_data,pdsch_dmrs_blind,pusch_dmrs,pusch_data,ssb,prach,srs`. This deliberately preserves S2b rather than reviving the older Task-2 plan's `all` alias.

[CODE-READ, DGX aarch64] PRACH and SRS now construct `nr_sense_rs_meta_t` and call `nr_sense_emit_rs`; neither owns a JSON formatter. Common D4 keys and T3 optional names remain. PRACH additionally retains `delay_samples`, `occasion`, `rar_match`, `rar_rapid`, `rar_ta`; its existing `delay` is the same sample value, and `energy` retains the detector’s dB×10 units. SRS retains CRB-relative `comb_offset` (also exposed by T3's `offset`), `n_symbols`, `slot_offset`, `confirmed`, `timing_cs_ambiguous`, floating `delay_ambiguity_samples`, and full uint64 `track_id`/`srs_track_id`. Unknown fields serialize as null. PRACH carries no frame link; SRS retains truncation and dropped-frame null linking. Both carry captured epoch/identity/dedicated epoch and monotonic timestamp; SRS now snapshots dedicated epoch at IQ queue admission. Neither worker samples current configuration at record emission. RF sample timestamps remain null when unavailable.

[CODE-READ, DGX aarch64] All CFR sources use **absolute subcarrier indices relative to CRB0/Point A**, never FFT bins or allocation-relative indices. The shared RS emitter advertises `k_abs_convention:"crb0_point_a"`. CSI-RS's per-RB estimate is located at `12*CRB`; PDSCH DM-RS/data and PUSCH data include the BWP/CRB origin; sparse PDSCH data uses each actual PRB; PUSCH DM-RS converts the FFT bin by subtracting `first_carrier_offset` modulo FFT size; SRS maps its allocated CRBs directly. Common `nr_sense_crb_re` arithmetic is used by the RB-based producers. **SSB correction:** estimator index zero is the actual first SSB RE at `fp->ssb_start_subcarrier`, so the axis is `ssb_start_subcarrier+i`, with neither k_SSB subtraction/RB rounding nor FFT wrapping. k_SSB normalization and the S34 `start_rb` metadata remain. The old axis tests that asserted rounding/wrapping were replaced with physical-coordinate assertions.

[CODE-READ, DGX aarch64] PUSCH `ta_applied_samples` remains the signed, actual FFT advance selected by the sweep and first-pass residual refinement, positive for an advanced window. Grant/RS `ta_est_samples` remains null as in S34. SRS attribution receives a separate arrival estimate, `ta_applied_samples - final_residual_delay`, explicitly named `ta_est_samples` in its internal input. SRS's own `ta_est_samples` is a signed measured representative (negative and zero values remain numeric) with the reviewed CS/delay ambiguity flags. T6b-b's export-only phase correction uses the actual applied advance for both PUSCH taps; decoder buffers are unchanged. UL CFR remains referenced to the unadvanced DL-frame FFT window after RX symbol/CP rotation.

[CODE-READ, DGX aarch64] Cleanly merged behavior retained: PRACH's robust per-FDM floor, AGC reset, fixed worker queue, TDD/SIB1 guards and RAR joins; SRS supported-length guard, booked-grant veto, four-sighting confirmation/revocation, RX rotations, alias-aware attribution, geometry cache, explicit-source default-off behavior and metrics. S2b sinkless accounting/antenna limits/subslot settings and T3/T4/S34 grants, gates and port-count semantics remain. No receiver convergence defaults, rank > 1 CB0 restrictions, K45 queue budget, deferred C/P/E levers or `ISAC_RECONF` default were changed; `ISAC_RX_BRANCH_FO` stayed unset.

## Validation

[OFFLINE VERIFIED, DGX aarch64, red] `SenseRecords.SrsSignedTimingAndCrbOffset` ran against the merged pre-fix records object and failed both expected assertions (negative timing and `comb_offset`), then passed after integration. Logs: `/tmp/s6m_red_build.log`, `/tmp/s6m_red.log`. The first focused run also found a test-only config-lifecycle error in the new multi-config source-token regression; adding the same `end_configmodule` cleanup used by the existing token regression fixed it.

[OFFLINE VERIFIED, DGX aarch64] New/extended checks cover explicit new source tokens without changing legacy fallback; SRS signed timing, 64-bit track IDs, dedicated epoch, truncation/linking and probation fields; PRACH common formatting/RAR fields; captured PRACH/SRS provenance across an epoch bump. `CfrKabsConventionConsistentAcrossSources` uses the real generated-SRS estimator and production SSB-axis helper with the shared DL/UL coordinate helpers, crossing FFT bin zero and retaining a non-RB-aligned SSB offset. The existing T6b-b four-antenna rotated/noisy PUSCH/SRS fixture and attribution tests remain.

[OFFLINE VERIFIED, DGX aarch64] Full target-list ON and OFF builds passed. Logs: `/tmp/s6m_on_build.log`, `/tmp/s6m_on_final_build.log`, `/tmp/s6m_off_build.log`. Final focused CTest passed **13/13 in each mode**; logs `/tmp/s6m_on_focus.log`, `/tmp/s6m_off_focus.log`. ON shuffled runs passed **39/39 invocations, 1,146 cases, 48 documented skips**; summary `/tmp/s6m_build_shuffle_summary.log`.

[OFFLINE VERIFIED, DGX aarch64] OFF shuffled runs passed **39/39 invocations, 1,146 cases, 48 documented skips**; summary `/tmp/s6m_build_off_shuffle_summary.log`. All **78/78** requested shuffled invocations passed across the two modes.

[OFFLINE VERIFIED, DGX aarch64 sandbox] Full ON CTest passed **144/153 registrations** in **1,597.03 seconds**. `nr_td_sim_test` passed in **1,596.98 seconds** and `test_nr_pdsch_config_sweep` passed in **237.59 seconds**. CTest exited 8 for exactly the nine documented baseline exceptions: four ARM failures (`test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`) and five sandbox socket failures (`time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`). No additional failing registration occurred. Log: `/tmp/s6m_on_ctest.log`. All production and test source changes were built before this full run; only this report was edited afterward.

[OFFLINE VERIFIED, DGX aarch64] `git diff --check`, the staged whitespace check and the sens6 frozen-path gate passed before the integration commit. The frozen gate was also run before each merge commit.

[CODE-READ, DGX aarch64] Every build/test command first waited for the orchestrator BUSY file to disappear and separately held the shared measurement lock. No exclusive lock, live receiver, standalone simulator campaign or GPU benchmark was run. The full CTest registration `nr_td_sim_test` is included as requested. Exact commands from the worktree root:

```sh
# Before EACH build/test invocation below:
while [ -e /home/nicola/NICOLA/wt/rr-orchestration/BUSY ]; do sleep 30; done
# Both existing Ninja/RelWithDebInfo caches: ENABLE_TESTS=ON, ENABLE_LDPC_CUDA=OFF;
# build has ENABLE_ISAC_SENSING=ON; build_off has ENABLE_ISAC_SENSING=OFF.
flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build_off -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
# Run for MODE=build and MODE=build_off:
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/MODE -j4 -R '^(test_nr_(sense_.*|srs_.*|prach_.*|isac_.*|passive_obs|passive_metrics|passive_cfg_epoch|pdcch_blind_monitor|pdcch_blind_rnti_bootstrap)|test_nr_ue_mib_blind_handoff)$' --output-on-failure
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
# Each binary below, SEED=1,3,5, MODE=build,build_off:
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/MODE/BINARY --gtest_shuffle --gtest_random_seed=SEED --gtest_brief=1
```

[CODE-READ, DGX aarch64] Shuffle binary list: `test_nr_isac_ssb_source`, `test_nr_isac_adapter`, `test_nr_sense_api`, `test_nr_sense_records`, `test_nr_sense_gating`, `test_nr_srs_blind`, `test_nr_prach_passive`, `test_nr_passive_obs`, `test_nr_passive_metrics`, `test_nr_passive_cfg_epoch`, `test_nr_pdcch_blind_monitor`, `test_nr_pdcch_blind_rnti_bootstrap`, `openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff`. Individual logs: `/tmp/s6m_<build|build_off>_<binary-basename>_seed<1|3|5>.log`.

## DEFERRED MEASUREMENTS and limits

[NOT RUN, DGX aarch64] The brief reserves live/rfsim runs for the orchestrator. Run separately on an idle host, with `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset:

```sh
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s6m_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_s6m_gate_on GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources csi_rs,pdsch_dmrs,pdsch_data,pdsch_dmrs_blind,pusch_dmrs,pusch_data,ssb,prach,srs --sensing.cfr_path /tmp/sense_s6m_gate.cfr --sensing.records_path /tmp/sense_s6m_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
```

[NOT RUN, sens6/OCUDU SA bed] After operator bring-up of the external core/gNB/UE and the broker prerequisites, capture before UE attachment:

```sh
flock -x /tmp/td_measure.lock env PX=1 PX_FIRST=1 NO_UE=0 UL_PING=0.2 PX_EXTRA='--sensing.enable 1 --sensing.sources prach,srs,pusch_dmrs,pusch_data --sensing.cfr_path /tmp/sense_s6m_live.cfr --sensing.records_path /tmp/sense_s6m_live.jsonl' tests/passive_rx/run_ocudu_passive.sh 420 /tmp/sense_s6m_ocudu
```

[NOT RUN, live validation] That broker harness is one RF channel, not a four-RX measurement. The separate four-channel 106-PRB/3750-MHz/30-kHz SA observer command from T6b-b, after an SRS-capable gNB and attached UE are already running with four-channel UL delivery, is:

```sh
flock -x /tmp/td_measure.lock timeout --signal=INT 420s cmake_targets/ran_build/build/nr-uesoftmodem -O tests/passive_rx/ue.passive.conf --rfsim --passive-rx --rfsimulator.serveraddr 127.0.0.1 -C 3750000000 -r 106 --numerology 1 --band 78 --ssb 516 --ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources prach,srs,pusch_dmrs,pusch_data --sensing.cfr_path /tmp/sense_s6m_sa4.cfr --sensing.records_path /tmp/sense_s6m_sa4.jsonl
```

[IMPLEMENTED, NOT VALIDATED on live traffic] K28 remains open: the DGX PHY-test bed supplies no PUSCH/SRS. PRACH floor calibration, SRS false-alarm/detection probability, multi-UE attribution, CPU/drop overhead and slow-consumer behavior on real traffic still require the above beds. Preserve T6b-b's limitations (v=1 hopping/multiport unsupported; delay/CS aliases can remain indistinguishable; late/dropped grants cannot retroactively veto), and T6a-b's unsupported-occasion skips. gNB logs/configuration are validation ground truth only. Task 5's reader can consume the single coordinate rule and schema-2 records; implementing that reader is outside S6M.
