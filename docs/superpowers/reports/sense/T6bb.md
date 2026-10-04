# T6b-b — blind SRS review fixes

Host: **DGX aarch64**. Branch: `sense/t6b-srs`. Review baseline: `19eb518dbe`.

## Changes and decisions

- [CODE-READ, DGX aarch64] `nr_srs_make_info` checks the sequence length against **every entry of `ul_allocated_re`**, before either search or estimation can call `generate_srs_nr`. The guard applies to comb 2/4/8. Rejected search candidates increment `nr_srs_unsupported_lengths`; the regression checks the seven reported comb-8 widths without invoking the generator and verifies the count.
- [CODE-READ, DGX aarch64] Booking a PUSCH grant posts its target absolute slot, DM-RS symbol mask and CRB span into the bounded SRS event queue. The worker retains 256 grant exclusions, including after the PUSCH grant book consumes the grant. Any intersecting SRS hypothesis is vetoed before sequence search. The span includes `bwp_start`. Type-0 bounding spans conservatively exclude holes too. This uses receiver grants, never gNB configuration/logs.
- [CODE-READ, DGX aarch64] Provisional SRS observations still export CFR and records, with **`confirmed:false`**. Confirmation requires **three consecutive equal legal gaps (four sightings)** of the same resource; duplicate sightings in a slot do not count. A changed/missed gap immediately returns the observation to probation. There is no GCD inference. Thus aperiodic observations remain provisional, even if attributed. This intentionally favors specificity over confirmation through missing observations.
- [CODE-READ, DGX aarch64] SRS FFT processing now calls the same `apply_nr_rotation_symbol_RX` as PUSCH, including CP time-shift and UL symbol rotation. Rotation inputs travel with captured IQ. Both UL producers export **CRB0/point-A subcarrier indices**, documented in `nr_sense_api.h`; PUSCH DM-RS formerly exported FFT bins, despite a contrary pre-existing comment. SRS `comb_offset` is serialized relative to CRB0; its internal generator offset remains allocation-relative.
- [CODE-READ, DGX aarch64] `nr_sense_ul_to_dl` removes each producer's actual FFT-window advance from its **exported CFR copy**, so SRS, PUSCH DM-RS and PUSCH data refer to the unadvanced DL-frame FFT window after RX symbol/CP rotation. PUSCH supplies `ul_sample_offset - est_delay_pre`, including its refinement. Decoder channel buffers and decoding behavior are untouched. SRS exports direct LS values on pilot REs and retains existing interpolation elsewhere.
- [ANALYTICAL; CODE-READ, DGX aarch64] Blind CS/arrival-time ambiguity is `N / (K_TC * n_cs)` samples, retained as a floating-point `delay_ambiguity_samples` field. `ta_est_samples` remains positive advance relative to the DL frame, not commanded UE TA. A unique attributable PUSCH observation selects the closest delay alias and corresponding CS; the worker regenerates the CFR with that CS. `timing_cs_ambiguous` becomes false only when such evidence resolves the alias. Otherwise the record retains the ambiguity and the selected representative.
- [CODE-READ, DGX aarch64] A DCI SRS request now requires recent PUSCH timing from the same RNTI, matching within two samples modulo the ambiguity. Multiple eligible request/CFR claimants or conflicting evidence remain anonymous. CFR attribution compares pilot phase after testing the PUSCH-supported CS alias; it can resolve an alias without a DCI request. Requests expire after 32 slots, PUSCH evidence after 256. Runtime attribution still requires receiver-confirmed RNTIs.
- [CODE-READ, DGX aarch64] Search evaluates each group once at CS zero, derives the representative CS from the phase slope, and retains CS counts **8/12/6**. A valid RRC hint returns after one scored reference; a wrong hint falls back. Scores combine antenna evidence by energy instead of taking the best antenna. A conservative threshold increases with the bounded group/geometry/window hypothesis count; it is explicitly a heuristic, not a calibrated false-alarm probability.
- [CODE-READ, DGX aarch64] A 64-entry geometry cache, keyed by comb, offset, symbol range and RB span, retains failures for eight slots and successes for the current slot. Repeated FFT placements cannot repeatedly search a cached candidate. Nine FFT placements remain available for distinct detectable geometries. Capture copies only eligible symbols in the last-six-symbol window (with the existing timing margins), with fixed storage and try-lock/drop behavior. No producer allocation or file I/O was added.
- [CODE-READ, DGX aarch64] SRS startup requires an explicit `srs` token: missing/default `sources`, `all`, and similarly named tokens do not enable it; `all,srs` does. Sensing/export gates remain default off. `ISAC_METRICS` now includes cumulative worker CPU microseconds `srs_us` and queue/evidence-drop count `srs_jobs_dropped` (zero before startup). Startup logs the unsupported v=1 sequence-hopping case once per worker start. Captured `N_RB_UL`/carrier-offset changes refresh the search grid and clear history without allocating on capture; FFT/sample-rate/antenna changes still require restart.
- [CODE-READ, DGX aarch64] The reviewed `u = n_ID mod 30`, last-six-symbol search, enum append, isolated formatter and default-off behavior remain. Merged-main convergence defaults, rank > 1 CB0 restrictions, K45 and deferred levers are untouched. No other worktree, operator plan, frozen configuration or `PROJECT_MEMORY.md` was edited.

## Task 3/4 merge notes

[CODE-READ, DGX aarch64] Preserve the small PUSCH changes in `nr_pusch_passive_decode.c`: CRB index conversion, export-only phase-reference conversion, actual FFT advance passed to data-aided export, and the existing additional SRS-evidence gate. `nr_isac_pusch_data_aided_submit` has one additional internal argument, `fft_advance`; its only caller is updated. Merge `nr_sense_ul.h`, the API coordinate documentation, the two metrics fields/getter and the grant-book notification. Keep the SRS enum appended uniquely when combining PRACH. Task 3 may replace the local formatter while retaining `confirmed`, ambiguity and CRB-relative comb-offset fields.

[CODE-READ, DGX aarch64] Read the orchestration lock policy, `PLAN_SENSE.md` D4/D5/D6 and Task 6b, repository rules/memory and original `T6b.md`. The explicit T6b-b review requirements supersede the original permissive attribution, GCD recurrence and implicit `all` source selection.

## Validation

[OFFLINE VERIFIED, DGX aarch64] Each build/test command checked the orchestration BUSY file and separately held a shared `/tmp/td_measure.lock`, at nice 19; builds used at most `-j8`, CTest at most `-j4`. Both caches have `ENABLE_TESTS=ON`, `ENABLE_LDPC_CUDA=OFF`; the ON/OFF caches have the corresponding `ENABLE_ISAC_SENSING` values. No receiver, rfsim bed, standalone simulator campaign, GPU benchmark, or exclusive lock was used.

- [OFFLINE VERIFIED, DGX aarch64, red → green] Four baseline assertion failures were observed before implementation: `Comb8UnsupportedLengthsRejected`, `SingleSightingNotConfirmed`, `OtherUesPeriodicSrsNotClaimed`, `TddGapsNotDeclaredPeriodic`. Log: `/tmp/t6bb_red.log`. Existing recurrence/attribution tests were updated to the stricter contracts.
- [OFFLINE VERIFIED, DGX aarch64, red → green] Grant-veto/cache/hint/source tests initially failed to link their new APIs (`/tmp/t6bb_veto_cpu_red.log`); grid-refresh regression likewise (`/tmp/t6bb_refresh_red.log`). Metrics assertions failed before serialization was added (`/tmp/t6bb_metrics_red.log`). The TP regression uses the separate PUSCH low-PAPR generator, first proves that the waveform passes the SRS sequence test, then proves the booked grant vetoes it in the matching slot and does not veto another slot.
- [OFFLINE VERIFIED, DGX aarch64] The realistic fixture uses four distinct complex antenna gains, nonzero delays 7 and 39 samples, deterministic time-domain noise, OFDM IDFT/DFT, CP time-shift and slot-dependent symbol rotation. SRS uses `generate_srs_nr`; PUSCH uses the separate Gold/QPSK DM-RS generator and LS/slope measurement with the receiver's actual RX rotation. The CFR comparison also uses different SRS/PUSCH FFT advances (+3/-7 samples). This is a PHY unit fixture, not a complete PUSCH TB decode or live receiver run. QPSK pilot amplitude is backed off to avoid fixed-point OFDM saturation; unequal transmit amplitudes are normalized by their own references.
- [OFFLINE VERIFIED, DGX aarch64, red → green] Different FFT advances failed the CFR comparison before phase-reference conversion (`/tmp/t6bb_window_red.log`). PUSCH-supported attribution without a trigger failed on an unresolved CS alias before alias-aware CFR comparison (`/tmp/t6bb_alias_red.log`). A baseline-behavior mutation removing RX rotation, restoring FFT-bin indices and allocation-relative serialized comb offsets fails all three relevant tests (`/tmp/t6bb_rotation_index_offset_red.log`); final code was restored before validation. Early fixture debugging also corrected a bit-array allocation and initialized the receiver sine lookup table; those fixture failures are not claimed as production regressions.
- [OFFLINE VERIFIED, DGX aarch64] Both ON and OFF full target-list builds passed (`/tmp/t6bb_on_build.log`, `/tmp/t6bb_off_build.log`). Focused ON and OFF CTest each passed **5/5 registrations** (`/tmp/t6bb_focused.log`, `/tmp/t6bb_off_focused.log`). SRS has **24 passing cases**. Shuffles passed **48/48 invocations** (24 per mode), **1,740 passed cases and 96 documented skips** (870/48 per mode), seeds 1/3/5 across the eight listed binaries (`/tmp/t6bb_shuffle_summary.log`). Full ON CTest passed **141/150 registrations**, with exactly the nine documented baseline exceptions, in **1600.32 seconds** (`/tmp/t6bb_on_ctest.log`). `nr_td_sim_test` passed in 1599.39 seconds. The four ARM exceptions were `dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`; the five sandbox exceptions were `time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`. No additional failure occurred. The full run used the final production code; no code changed afterward.

Exact commands (from this worktree):

```sh
# Before EACH command below, wait while BUSY exists; each flock is released on command exit:
while [ -e /home/nicola/NICOLA/wt/rr-orchestration/BUSY ]; do sleep 60; done
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 env CCACHE_DISABLE=1 ninja -C cmake_targets/ran_build/build_off -j8 nr-uesoftmodem oai_usrpdevif rfsimulator params_libconfig nr-softmodem tests
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure
# ON used -j4; OFF used -j2 while two long full-suite registrations were running:
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/BUILD -j2 -R '^(test_nr_srs_blind|test_nr_sense_api|test_nr_isac_adapter|test_nr_pdcch_blind_monitor|test_nr_passive_metrics)$' --output-on-failure
# Each named binary, each seed 1/3/5, both build directories:
flock -s -w 7200 /tmp/td_measure.lock nice -n 19 cmake_targets/ran_build/BUILD/BINARY --gtest_shuffle --gtest_random_seed=SEED --gtest_brief=1
```

[OFFLINE VERIFIED, DGX aarch64] Shuffle binary list: `test_nr_isac_ssb_source`, `test_nr_pdcch_blind_monitor`, `test_nr_pdcch_blind_rnti_bootstrap`, `test_nr_isac_adapter`, `test_nr_srs_blind`, `test_nr_sense_api`, `test_nr_passive_metrics`, `openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff`. Logs use `/tmp/t6bb_{on,off}_<binary>_seed<seed>.log` and `/tmp/t6bb_shuffle_summary.log`.

## DEFERRED MEASUREMENTS

[NOT RUN, DGX aarch64] The lock policy reserves live/rfsim measurements for the orchestrator. On an idle host, from this worktree, with `ISAC_RX_BRANCH_FO` and `ISAC_RECONF` unset:

```sh
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6bb_gate_off GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 0' tests/passive_rx/dgx/rfsim_regress.sh 2
flock -x /tmp/td_measure.lock env OUT=/tmp/sense_t6bb_gate_on GATE_SECS=420 RXEXTRA='--ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources all,srs --sensing.cfr_path /tmp/sense_t6bb_gate.cfr --sensing.records_path /tmp/sense_t6bb_gate.jsonl' tests/passive_rx/dgx/rfsim_regress.sh 2
```

[NOT RUN, sens6] With the attached-UE SA/OCUDU prerequisites from T6b in place:

```sh
flock -x /tmp/td_measure.lock env PX=1 PX_FIRST=1 NO_UE=0 UL_PING=0.2 PX_EXTRA='--sensing.enable 1 --sensing.sources srs,pusch_dmrs,pusch_data --sensing.cfr_path /tmp/sense_t6bb_live.cfr --sensing.records_path /tmp/sense_t6bb_live.jsonl' tests/passive_rx/run_ocudu_passive.sh 420 /tmp/sense_t6bb_ocudu
```

[NOT RUN, live validation] That broker harness has one RF channel and cannot establish four-RX performance. For the separately provisioned 106-PRB / 3750-MHz / 30-kHz SA cell, with its SRS-capable gNB and UE already running and four-channel UL delivery configured:

```sh
flock -x /tmp/td_measure.lock timeout --signal=INT 420s cmake_targets/ran_build/build/nr-uesoftmodem -O tests/passive_rx/ue.passive.conf --rfsim --passive-rx --rfsimulator.serveraddr 127.0.0.1 -C 3750000000 -r 106 --numerology 1 --band 78 --ssb 516 --ue-nb-ant-rx 4 --sensing.enable 1 --sensing.sources srs,pusch_dmrs,pusch_data --sensing.cfr_path /tmp/sense_t6bb_sa4.cfr --sensing.records_path /tmp/sense_t6bb_sa4.jsonl
```

[NOT RUN, live validation] Check the bed's RF/sample routing before interpreting missing SRS. Validate transform-precoded pos2 PUSCH with SRS disabled at the transmitter, then simultaneous periodic SRS from two UEs with different delays. Check no confirmed DM-RS-as-SRS records, no wrong RNTI attribution, matched-subcarrier CFR phase, confirmation/revocation, and metrics under a slow export consumer. gNB configuration/logs are validation ground truth only. The DGX phy-test gate alone cannot close UL/K28 coverage.

## Limits and remaining measurements

[ANALYTICAL; NOT RUN, live] A single comb cannot identify unrestricted delay and CS independently. Attribution here is evidence, not proof of identity; two UEs with coincident aliased delays and similar channels can remain intrinsically indistinguishable. v=1 hopping and multiport separation remain unsupported. Unobserved, late or dropped receiver grants cannot veto observations processed before the evidence arrives; recurrence alone cannot distinguish a perfectly periodic unknown PUSCH DM-RS mimic. Eight-slot negative caching can delay discovery of a newly appearing resource with the same geometry. Four regular sightings are deliberately stricter than the former missed-slot GCD policy. The multiple-hypothesis threshold, detection probability, real false-alarm rate, CPU time and queue drops require the deferred measurements; no live performance claim is made.

[OFFLINE VERIFIED, DGX aarch64] Frozen sens6 paths match `sens6-frozen-2026-09-30`; `git diff --check` and the staged whitespace check passed before commit.
