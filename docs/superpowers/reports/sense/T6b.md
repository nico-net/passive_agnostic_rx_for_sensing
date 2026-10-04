# T6b — blind SRS receiver

Host: **DGX aarch64**. Worktree: `sense-t6b`. Base: `87cd42950f` (S1b/S2).

## Changes

- [CODE-READ, DGX aarch64] `nr_srs_blind.{c,h}` and its private header implement bounded comb-energy detection over the last six eligible symbols, contiguous PRB spans, comb 2/4/8 and offsets, 1/2/4-symbol resources, group 0–29 and cyclic-shift search using `generate_srs_nr`, optional hint-first search, and 32 anonymous resource histories. Three observations are required before reporting a legal recurrence period; missed observations are handled by the GCD of gaps. Resource histories are receiver configuration observations, not target trackers.
- [CODE-READ, DGX aarch64] `nr_srs_estimate.c` reuses `generate_srs_nr` → `nr_get_srs_signal` → `nr_srs_ls_channel_estimation` → `nr_srs_channel_interpolation` through a minimal fake-gNB context. Comb 8 uses bounded linear interpolation of the same gNB LS estimates because upstream only has comb-2/4 interpolation filters. Comb-2/4 leading REs before the first pilot use its LS value; upstream interpolation leaves these REs zero for nonzero comb offsets. Four antennas remain separate, with carrier-relative `k_abs` and absolute OFDM symbol indices. The detected PRB start goes in the synthesized PDU's BWP start, avoiding the upstream 8-bit `k_0_p`/`n_shift` placement limit.
- [CODE-READ, DGX aarch64] `nr_srs_passive.{c,h}` adds a preallocated four-slot raw-IQ queue and one consumer. Startup is outside RX threads. Producers use try-lock/drop, fixed copies and capture-time metadata; no producer allocation, blocking wait or file I/O. The consumer uses the existing CFO compensation and DFT functions and searches nine FFT-window placements from −400 to +400 samples, in 100-sample steps, around broadcast/common N_TA_offset. It searches separate PRB resources at different delays and suppresses duplicate exports across windows. It captures the preceding slot so trailing search windows are already available. Known DL symbols are excluded; an unknown TDD pattern does not prevent blind search. FDD is inert.
- [CODE-READ, DGX aarch64] `nr_srs_export.{c,h}` pushes antenna-major float CFR with `NR_SENSE_SRC_SRS`, appended immediately before `NR_SENSE_SRC_COUNT`. One static `emit_srs_record` formats schema-2 JSON locally, ready to replace with Task 3's emitter. It contains the D4 timing/epoch/UL keys and comb, comb_offset, n_symbols, start_rb, nb_rb, cs, group, period, slot_offset, track_id, srs_track_id, attribution and nullable rnti. Full rings produce a null frame link; truncated records describe the stored RE count. Unknown RF timestamp and dedicated epoch are null.
- [CODE-READ, DGX aarch64] `nr_srs_attribution.c` accepts a recent nonzero DCI request from a known RNTI, or combined PUSCH timing and normalized CFR similarity (at least four matching carrier REs). A unique request is eligible for 32 slots; PUSCH evidence expires after 256 slots. Multiple matching RNTIs or conflicting evidence remain anonymous. The method records evidence, not proof of UE identity or knowledge of the encrypted SRS scheduling configuration.
- [CODE-READ, DGX aarch64] Small runtime hooks retain DCI 1_1's previously discarded SRS-request field, post requests from admitted DCI 0_1/1_1 of confirmed RNTIs, post existing PUSCH channel/timing evidence, and harvest optional unciphered RRCSetup SRS group/CS hints. Harvested hints never supply an RNTI or bypass the blind correlation. The worker clears learned state and hints on a captured epoch/identity change. The bootstrap confirmation lookup is on the consumer because it takes a mutex; observation producers only post bounded events.
- [CODE-READ, DGX aarch64] `srs_rx.c` wraps the noise-bin lookup at the FFT boundary and excludes only its gNB scheduling function in `PHY_NR_PASSIVE_UL` (`NR_SRS_PASSIVE_EXTRACT_ONLY`). The extraction function is the real gNB implementation. `CMakeLists.txt` links the SRS sources into that library and adds `test_nr_srs_blind`. OFF builds compile receiver SRS startup as a no-op.

## Commits

1. `9215da6ff9` — `feat(sense): blind SRS detection, CFR export and attribution` (detector/search).
2. `856bdea20b` — same plan subject (estimation/export and receiver queue).
3. Attribution, final integration fixes and this report — same plan subject, current report commit.

## Decisions and interpretation limits

- [CODE-READ, DGX aarch64] Read the complete plan (D1–D6, constraints, review focus and all tasks), brief, rules, S1/S1b/S2 and API header. S2b was absent from this branch; its report was read from `sense-api` without modifying that worktree. No other worker branch was merged or rebased here. The build marker eventually appeared with `rc=1`; the missing baseline build work was completed in this worktree.
- [ANALYTICAL] CS and arrival delay both introduce linear phase on a comb. A single observation cannot uniquely distinguish all aliases over an unrestricted timing range. Search chooses the equivalent CS with the smallest residual delay, and records explicitly say `timing_cs_ambiguous:true`. `ta_est_samples` is the **positive advance** of the observed UL arrival relative to the DL-frame FFT window: common advance minus the selected window displacement minus residual channel delay. It is not the UE's gNB-commanded TA. PUSCH evidence uses the same convention, including its first-pass refinement and final residual.
- [CODE-READ, DGX aarch64] Group hopping depends on more than `n_ID mod 30` in the existing generator. No full hopping ID is claimed. Every symbol must validate before a joint multi-symbol estimate is exported; otherwise the worker searches symbols independently. Per-occasion group changes can be observed without knowing ciphered RRC configuration, but a stable hopping-resource identity/period is not guaranteed.
- [PARTIAL, DGX aarch64] The synthesized estimator PDU has one SRS transmit port. Superposed/multi-port resources, frequency hopping within a jointly estimated burst and sequence-hopping v=1 are not identified as such. Frequency-selective channels that fail the normalized correlation threshold remain unexported. Live detection probability, false-alarm rate and timing ambiguity resolution are unmeasured.
- [CODE-READ, DGX aarch64] SRS source selection is local to startup: `sources` containing `srs` or `all`, with the export API enabled. This permits SRS-only operation despite the legacy adapter's old source enum. Explicit legacy source lists without `srs` do not start the worker. `cfr_antennas=1` exports the first antenna; otherwise all configured RX antennas are exported. FFT size, carrier width, antenna count or sample-rate changes require a restart; mismatched captures are dropped safely. Queue/evidence drops are reported in `SENSING: SRS queue dropped=...` at shutdown; accepted CFR/record drops use the existing API counters.
- [CODE-READ, DGX aarch64] Merge points are limited: the passive-UL CMake source list/test entry; the new source enum; the local JSON emitter; two executable lifecycle/capture hooks; small DCI/PUSCH hooks. Task 3 can supply its captured RF timestamp/dedicated epoch and replace the static emitter. Task 4's PUSCH gate must retain the additional SRS-evidence condition. Do not assign two source-enum names the same ordinal when combining PRACH and SRS.

## Validation

[OFFLINE VERIFIED, DGX aarch64] All builds and tests below separately held a shared `/tmp/td_measure.lock` and checked that the orchestrator `BUSY` file was absent. No receiver, rfsim bed, simulator campaign, GPU benchmark or exclusive lock was run.

- [OFFLINE VERIFIED, DGX aarch64, red → green] Tests were written before their implementation. While the requested `.prebuilt` marker was absent, isolated C detector and recurrence checks were compiled into `/tmp` with `cc` under the shared lock: both failed against the initial stubs, then passed. Logs: `/tmp/sense_t6b_comb_red.log`, `/tmp/sense_t6b_recurrence_red.log`. Ninja was not invoked before the marker appeared. The first generated-SRS suite run after provisioning passed 7/7; no claim is made that all seven generated tests were executed red before implementation.
- [OFFLINE VERIFIED, DGX aarch64, red → green] Adding the LS fixture before wiring the estimator produced undefined `nr_srs_estimate` (`/tmp/sense_t6b_red2.log`). The export fixture then failed because the record kept the input RE count on truncation; after correction all 9 cases passed (`/tmp/sense_t6b_test2_red.log`, `/tmp/sense_t6b_test2.log`).
- [OFFLINE VERIFIED, DGX aarch64, red → green] Attribution fixtures first failed to link (`/tmp/sense_t6b_red3.log`), then passed. `HoppingSymbolsDoNotExportWrongReference` subsequently failed against first-symbol-only verification (12/13 passed), then passed after all-symbol verification and runtime splitting (`/tmp/sense_t6b_hopping_red.log`).
- [OFFLINE VERIFIED, DGX aarch64, red → green] Checking every allocated RE with the largest comb offset exposed zero CFR values before the first pilot. After bounded edge extension, all 13 cases passed (`/tmp/sense_t6b_edge_red.log`, `/tmp/sense_t6b_edge_green.log`).
- [OFFLINE VERIFIED, DGX aarch64] Final SRS suite: **13/13 passed**, including comb/offset/symbol enumeration, noise/downlink rejection, group/CS recovery, recurrence with missed slots, optional/wrong hints, disjoint spans, invalid geometry, non-SRS comb rejection, four distinct complex antenna channels within ±0.5 dB, binary/JSON linking, capture-time epoch, full-ring null link, truncation, attribution ambiguity/expiry and epoch-reset isolation. Log: `/tmp/sense_t6b_edge_green.log`.
- [OFFLINE VERIFIED, DGX aarch64] Focused CTest: **4/4 registrations passed** (`test_nr_pdcch_blind_monitor`, `test_nr_isac_adapter`, `test_nr_srs_blind`, `test_nr_sense_api`), log `/tmp/sense_t6b_focused.log`.
- [OFFLINE VERIFIED, DGX aarch64] Required ON target list built successfully, with an incremental refresh after the final fixes. Logs: `/tmp/sense_t6b_full_build.log`, `/tmp/sense_t6b_final_build.log`, `/tmp/sense_t6b_final_build2.log`, `/tmp/sense_t6b_producer_build.log`, `/tmp/sense_t6b_edge_targets.log`.
- [OFFLINE VERIFIED, DGX aarch64] ON shuffles: **21/21 invocations passed**, seeds 1/3/5 on seven affected gtest binaries; **804 cases passed, 48 documented skips**. Summary: `/tmp/sense_t6b_shuffle_results.log`. All three SRS seeds were refreshed after the final edge correction and passed.
- [OFFLINE VERIFIED, DGX aarch64] OFF `nr-uesoftmodem tests` build passed, including a final incremental refresh. Focused CTest passed **4/4 registrations**, including SRS, API, adapter and blind monitor. Logs: `/tmp/sense_t6b_off_build.log`, `/tmp/sense_t6b_off_refresh.log`, `/tmp/sense_t6b_off_focused.log`.
- [OFFLINE VERIFIED, DGX aarch64] OFF shuffles: **21/21 invocations passed**, the same seven binaries and seeds 1/3/5; **804 cases passed, 48 documented skips**. Logs: `/tmp/sense_t6b_off_<binary>_seed<seed>.log`, summary `/tmp/sense_t6b_off_shuffle_results.log`.
- [OFFLINE VERIFIED, DGX aarch64] Full ON CTest ran once: **141/150 passed, 9 failed**, 1661.18 seconds. `nr_td_sim_test` passed in 1638.50 seconds. The four documented ARM failures were `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`, `dft_test`, `test_nr_modulation`; the five documented sandbox failures were `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. These are the baseline exceptions named in `CODEX_BRIEF.md`; no additional failure occurred. Log: `/tmp/sense_t6b_ctest.log`. The full run started before the final edge/producer fixes; affected targets were rebuilt and focused/shuffled tests were refreshed afterward.

Exact build/test commands (run from the indicated build directory unless `-C`/`-B` is shown):

```sh
# Cache relocation was necessary because the shared CPM cache is read-only.
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S . -B cmake_targets/ran_build/build -U 'CPM_*' -DCPM_SOURCE_CACHE=/tmp/sense_s1_cpm
flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 test_nr_srs_blind
flock -s -w 7200 /tmp/td_measure.lock env CCACHE_DISABLE=1 nice -n 19 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ./test_nr_srs_blind --gtest_brief=1
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest -R '^(test_nr_srs_blind|test_nr_sense_api|test_nr_isac_adapter|test_nr_pdcch_blind_monitor)$' --output-on-failure
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest -j4 --output-on-failure
# Per binary and per seed 1, 3, 5:
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ./BINARY --gtest_shuffle --gtest_random_seed=SEED --gtest_brief=1
# OFF mode (from worktree root):
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake -S . -B cmake_targets/ran_build/build_off -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCPM_SOURCE_CACHE=/tmp/sense_s1_cpm -DENABLE_TESTS=ON -DENABLE_ISAC_SENSING=OFF -DENABLE_LDPC_CUDA=OFF -DOAI_USRP=ON -DOAI_SIMU=ON -DOAI_RF_EMULATOR=ON -DOAI_VRTSIM=ON -DT_TRACER=ON -DAUTO_DOWNLOAD_ASN1C=OFF -DASN1C_EXEC=/opt/asn1c/bin/asn1c
flock -s -w 7200 /tmp/td_measure.lock env CCACHE_READONLY=1 CCACHE_TEMPDIR=/tmp nice -n 19 ninja -C cmake_targets/ran_build/build_off -j8 nr-uesoftmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build_off -j4 -R '^(test_nr_srs_blind|test_nr_sense_api|test_nr_isac_adapter|test_nr_pdcch_blind_monitor)$' --output-on-failure
# Repeat the per-binary shuffle command from build_off for seeds 1, 3, 5.
```

Shuffle targets (all gtest binaries linking the changed receiver/export libraries): `test_nr_isac_ssb_source`, `test_nr_pdcch_blind_monitor`, `test_nr_pdcch_blind_rnti_bootstrap`, `test_nr_isac_adapter`, `test_nr_srs_blind`, `test_nr_sense_api`, `openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff`. Logs: `/tmp/sense_t6b_<binary>_seed<seed>.log` and `/tmp/sense_t6b_shuffle_results.log`.

## DEFERRED MEASUREMENTS

[NOT RUN, DGX aarch64] The brief reserves live/rfsim runs for the orchestrator. Run each arm separately on an idle host, from this worktree, with `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset:

```sh
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6b_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6b_gate_on GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources all --sensing.cfr_path /tmp/sense_t6b_gate.cfr --sensing.records_path /tmp/sense_t6b_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
```

[NOT RUN, sens6] On the attached-UE SA/OCUDU host, after bringing up its core and SRS-capable gNB/UE, the existing broker harness can start the observer before attachment:

```sh
flock -x /tmp/td_measure.lock env PX=1 PX_FIRST=1 NO_UE=0 UL_PING=0.2 PX_EXTRA='--sensing.enable 1 --sensing.sources srs,pusch_dmrs,pusch_data --sensing.cfr_path /tmp/sense_t6b_live.cfr --sensing.records_path /tmp/sense_t6b_live.jsonl' tests/passive_rx/run_ocudu_passive.sh 420 /tmp/sense_t6b_ocudu
```

[CODE-READ, sens6 harness] This existing ZMQ broker is one RF channel, so that command checks attachment/SRS occurrence and attribution only; it is **not** four-RX validation. Its ZMQ plugin and external gNB/UE/core are prerequisites. For the repository's existing 106-PRB / 3750-MHz SA rfsim cell, with the SRS-capable gNB and attached UE already brought up by the operator, the four-RX observer command is:

```sh
flock -x /tmp/td_measure.lock timeout --signal=INT 420s cmake_targets/ran_build/build/nr-uesoftmodem -O tests/passive_rx/ue.passive.conf --rfsim --passive-rx --rfsimulator.serveraddr 127.0.0.1 -C 3750000000 -r 106 --numerology 1 --band 78 --ssb 516 --ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources srs,pusch_dmrs,pusch_data --sensing.cfr_path /tmp/sense_t6b_sa4.cfr --sensing.records_path /tmp/sense_t6b_sa4.jsonl
```

[NOT RUN, sens6] This command assumes that named SA cell and four-channel UL delivery; check the bed's RF/sample routing before interpreting missing SRS. OTA carrier/device arguments are operator-specific and are not fabricated here. gNB scheduling/configuration/logs are validation ground truth only, never receiver input. Verify comb/offset/group/CS, measured recurrence, per-symbol mapping, stable static-channel CFR, correct/anonymous attribution, and every non-null frame link against the binary stream. Measure CPU cost and queue/API drops, including a slow output consumer and `cfr_antennas=1` versus all four antennas. The PHY-test regression has no SRS/PUSCH and cannot establish this coverage or close K28.

## Interactions and remaining work

[CODE-READ, DGX aarch64] Merged-main convergence defaults, rank > 1 CB0 restrictions, K45 DCI scan queue capacity, deferred levers C/P/E and `ISAC_RECONF` default-off are unchanged. `ISAC_RX_BRANCH_FO` was never enabled. Additional SRS CPU/drop-rate behavior needs the deferred measurement; no performance claim is made. `PROJECT_MEMORY.md` and frozen sens6 files are untouched. The frozen-path gate and `git diff --check` are repeated before each commit.
