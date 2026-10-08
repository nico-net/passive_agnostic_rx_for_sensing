# UL-INT — integrated first-PUSCH CPU path (U1–U9)

2026-10-08, `csd/ul-int`, DGX aarch64 sandbox. The first-PUSCH milestone requires a nonzero full PUSCH TB CRC for a C-RNTI whose DCI 1_1 length was trusted by the DL receiver, measured from its first accepted DCI 0_1. No gNB configuration or log is a receiver input.

## Implementation

- **[CODE-READ + IMPLEMENTED]** Merged `csd/ul-b`, `csd/ul-c`, and the evolving `csd/t26b` onto UL-A. The only UL-C conflict was its per-symbol FEP macro: the resolution retains UL-A's bounded IQ snapshot and UL-C's per-symbol timing sweep. The existing CSI-RS contract in `nr_csirs_disc.h` was not changed; existing semantics are untouched.
- **[IMPLEMENTED]** `ISAC_UL_PUSCH_CHAIN=1` is default off and activates the bounded UL worker, DL-locked per-UE state, banked DCI 0_1, U3 ticket catalogue, timing and CP DM-RS discovery. The DL-only full-auto OTA config gets DCI 0_1 scanning and PUSCH decode under this flag. Admission requires a current trusted DL DCI 1_1 ticket and a producer-time U3 hypothesis ticket. The accepted 0_1 is parked for k2, copied into the bounded worker, and fed through the existing gNB PUSCH FEP/chest/one-layer demod/rate-recovery/LDPC/TB-CRC path. The no-UCI decode is tried first; a CSI-request or nonzero DAI is logged as a tentative UCI hint. Existing optional UCI recovery remains separately configured.
- **[IMPLEMENTED]** DM-RS discovery's actual nSCID/DM-RS ID and the data ID used by the decoder now reach U4's TB-confirmed state instead of confirming the original grant's ID. Per-UE data-ID hypotheses rank PCI and DL PDSCH/PDCCH IDs, then walk the 10-bit domain at one candidate per failed grant, revisiting a DL prior every 16 failures. Full nonzero TB CRC remains the only ID/config confirmation. Identity generation, incarnation and epoch guard delayed results.
- **[IMPLEMENTED]** `UL_FIRST_0_1`, `UL_PUSCH_CRC`, and `UL_FIRST_TB_OK` lines report source/target absolute slots, MCS, PRBs, applied TA, tentative UCI hint, IDs, decode status and CPU microseconds. The first accepted DCI 0_1 starts each per-identity clock; a nonzero CRC-valid TB stops it. `UL_CHAIN_DROP`, `PUSCHQ` and `pusch_book` expose admission/queue losses.
- **[IMPLEMENTED + focused synthetic test]** After the first capture runs exposed many unsupported interpretations, U3 now retains up to four equal-length BWP/SRI/CSI-request width splits per geometry, assigning every geometry its first split before adding alternatives; its fixed 64-hypothesis ceiling remains. The chain skips known RV-nonzero grants before the IQ queue because this first decoder has no HARQ history, and counts them as `skipped_rv`. Rejected jobs now state their reason, while `UL_CONFIG_STATS` reports proposals, feedback and proposal time. These two changes are enabled only by the chain flag and were not present in the full 12-second replays above.
- **[CODE-READ, locations]** The producer catalogue is in `nr_pdcch_ul_config_disc.c:175-253`, first-grant admission/counters in `nr_pusch_passive_monitor_rt.c:32-99`, worker feedback and thread CPU timing in `nr_pusch_passive_queue.c:138-208`, timing/DM-RS search and decoder entry in `nr_pusch_passive_decode.c:825-1100`, and the width-split regression in `nr_pdcch_blind_monitor_test.cc:3194`. These paths are relative to `openair1/PHY/NR_UE_TRANSPORT/` (the test is in its `tests/` directory).

## Verification

- **[OFFLINE VERIFIED, build/unit]** CPU receiver and focused UL tests built. Eleven banked DCI/U3/first-PUSCH tests passed, including identity/timer, data-ID sweep and a same-length alternative-width test; `test_nr_pusch_ul_disc` passed. The first-PUSCH tests reject duplicate/stale identity credit and preserve every data-ID sweep position around a prior retry. Banked known-RNTI noise controls reported 0/5,000 false accepts. The wrong-DL-BWP fixture made 71 U3 proposal calls at 17.09 µs mean and 36.80 µs max after the catalogue expansion. These fixtures are not an OTA TB CRC result.
- **[OFFLINE VERIFIED, full build/test gate]** `CCACHE_DIR=/tmp/rr-csd-d-ccache flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ninja -C cmake_targets/ran_build/build -j8 tests` completed 70/70 build steps after the final source changes. `flock -s -w 7200 /tmp/td_measure.lock nice -n 19 ctest --test-dir cmake_targets/ran_build/build -j4 --output-on-failure -E '^nr_td_sim_test$'` completed all 226 listed cases in 911.57 s: 202 passed, 15 were disabled, and nine failed. The nine are precisely the brief's known ARM (`dft_test`, `test_nr_modulation`, `test_nr_pusch_ra0_qam64`, `test_nr_pusch_ra0_qam256`) and sandbox socket (`time_management_tests`, `test_gtp`, `test_vrtsim`, `test_vrtsim_cirdb`, `nr_cuup_functional_test`) failures. `test_nr_pdcch_blind_monitor`, `test_nr_pusch_ul_disc`, the DCI 0_1 layout sweep, and the 911.51 s CSI-RS footprint case passed in this full run. No unexpected test failure was observed.
- **[OFFLINE VERIFIED, replay `raw_pci415_12s_b`, baseline flags]** 1,474,498,624 of 1,474,560,000 samples/channel reached raw-IQ EOF. PBCH and NSA SIB1 decoded. Final PDCCH census: 1,041 occasions, 3,123 candidates, 135 accepts (130 SI, five TC, **zero C-RNTI and zero DCI 0_1**). `pusch_book` parked/claimed 0/0; `PUSCHQ` queued/decoded/CRC-OK 0/0/0 and every UL queue drop counter 0. No `UL_FIRST_0_1`, `UL_PUSCH_CRC`, arrival or DM-RS observation exists. The idle UL worker used 1,491.309 ms CPU over 411,492.314 ms wall time (0.0036 core); per-grant CPU and first-CRC latency are unmeasured. The run used the brief's CPU flags and the UL-A config, before the later T26B tip and its dedicated DL solver flags. Log: `/tmp/ulint_replays/replay_ulint_b_163645/run.log`.
- **[OFFLINE VERIFIED, replay `raw_pci415_12s_20261008`, receiver-derived CORESET fixture]** The CPU replay loaded two CORESET records (`nID=62922` and `10163`) extracted from the earlier T26B GPU receiver log. The joint DL solver independently observed repeated `JOINT_ANON_HIT` records for C-RNTI `0x8f5a`, locked DCI 1_1 length 49, and obtained DL PDSCH CRCs. The UL bank first saw DCI 0_1 length 44 at absolute slot 2306, locked that length at 2506, and logged `UL_FIRST_0_1 rnti=0x8f5a t=2506`. At raw-IQ EOF (1,474,558,461 of 1,474,560,000 samples/channel), **none of 1,050 `UL_PUSCH_CRC` results passed**: 906 unsupported before LDPC and 144 reached LDPC but failed a segment/TB CRC. The last periodic counters showed 1,016 queued, 142 decoded, zero CRC OK, one stale queue drop, no full/epoch/unready/snapshot drops, and nine admission drops lacking a DL lock. Thus U2 admission, U3 tickets, U1 scheduling, U5/U6 candidate search, FEP, demodulation and LDPC entry are exercised; U9 has no TB evidence with which to confirm an interpretation. This is a fixture-assisted downstream test, not a fully blind CPU cold start. Log: `/tmp/ulint_replays/replay_ulint_old_fixture_165008/run.log`.
- **[OFFLINE VERIFIED, per-grant CPU on that replay]** The 906 unsupported grants used a mean 338 µs (median 301 µs, p95 645 µs); the 144 LDPC attempts used a mean 49.3 ms (median 19.2 ms, p95 213 ms, maximum 237 ms). Across all 1,050 results the mean was 7.06 ms. `cpu_us` uses `CLOCK_THREAD_CPUTIME_ID`, so it is thread CPU time around each decoder call; queue wait is excluded. No first-CRC latency exists because no TB passed.
- **[OFFLINE VERIFIED, replay `raw_pci415_12s_b`, receiver-derived CORESET fixture, diagnostic binary]** The two CORESET records also worked on the main recording. Joint DL evidence and length locks yielded 14 C-RNTIs with `UL_FIRST_0_1` records. Raw-IQ EOF consumed 1,474,498,630/1,474,560,000 samples/channel. Of 4,317 logged PUSCH results, **zero TB CRCs passed**: 2,280 were rejected as RV nonzero without HARQ history, 2,031 entered LDPC and failed segment/TB CRC, and six exceeded the bounded UL snapshot. There were 28 accepted timing observations and 29 DM-RS candidate reports, but no U3 TB-confirmed hypothesis. The last periodic queue sample showed 4,469 queued, 1,967 decoded, zero CRC OK, drops of 39 full, 277 stale, one epoch, nine unready and one snapshot; those losses are materially above the requested near-zero gate under this combined DL+UL CPU replay. Its single UL worker used 97,972.688 ms CPU over 468,927.070 ms wall time (0.209 core). Per-grant LDPC attempts used 46.9 ms mean thread CPU (median 21.4 ms, p95 190 ms, maximum 292 ms); unsupported jobs used 370 µs mean. Log: `/tmp/ulint_replays/replay_ulint_main_fixture_165914/run.log`.
- **[OFFLINE VERIFIED + INFERENCE, failing stage]** U2 trusted identity and length, U3 ticket issuance, U1 worker admission, U5/U6 observation, FEP and LDPC entry all occur on the main recording. The first unproved link is **U3's grant interpretation at the correct UL resources**, followed by U7/U8 decoding: no interpretation has passed TB CRC. In particular, a DM-RS-like score of 0.909 for `0x37f3` at absolute slot 19470 used ID 415 and an advance of −8 samples against a nominal 1,600; a DL DCI 1_1 for the same RNTI was also accepted in that slot. This is consistent with a hypothesis landing on DL energy, so that strong pilot score cannot certify a PUSCH. The original U3 catalogue retained only the first equal-length split of BWP, SRI and CSI-request bits; the next revision retains bounded alternatives. Wrong UL TDRA/k2, scrambling ID, UCI position and weak UL energy remain unresolved alternatives until a full TB CRC or an independently validated slot-IQ fixture distinguishes them.
- **[OFFLINE VERIFIED, revised first-five-second replay]** The final binary used the exact main capture/sample count and replay fault injection `ISAC_RAW_IQ_GAP_AT_S=5 ISAC_RAW_IQ_GAP_S=6.9`; the source logged a gap at sample 614,400,112 and skipped the remaining 6.9 s before clean EOF. All 605 PUSCH results occurred before the gap: **zero nonzero TB CRC OK**, 603 segment/TB CRC failures, one bounded-timing reject and one all-zero TB that correctly logged `ok=0` (a decoder CRC pass by construction is not a valid milestone). Eleven DL-found RNTIs logged `UL_FIRST_0_1`. The last pre-gap stats showed 826 U3 proposals, 550 live hypotheses, 311 RV-nonzero grants skipped before queueing, 508 queued, 482 decoded, zero CRC OK, zero queue-full drops, 25 stale drops and two unready drops. U3 proposal time averaged 587 µs with a 2.68 ms maximum; the 603 LDPC failures averaged 38.6 ms thread CPU (median 18.8 ms, p95 190 ms). The worker used 23,921.372 ms CPU over 158,452.394 ms wall time (0.151 core). The width expansion improved catalogue coverage and the RV gate removed queue-full losses in this window, but the required first-PUSCH CRC and near-zero stale-drop gate remain unmet. Log: `/tmp/ulint_replays/replay_ulint_main_width5s_gap_171010/run.log`.

## Current gate and limits

- **[CODE-READ + OFFLINE VERIFIED]** Without a GPU, `nr_pdcch_coreset_map_idsweep_want()` enables only an optional CPU snapshot dump (`ISAC_COREMAP_IDSWEEP`), whose full-ID sweep is offline; it does not publish live dedicated CORESET banks. The baseline replay's zero C-RNTI/0_1 census therefore stops before U2, U3, and any PUSCH grant. U5–U9 cannot be judged on that run. `csd/t26b` independently found DL-locked C-RNTIs and PDSCH CRCs on the older capture with GPU nID discovery; that result is receiver-derived, not gNB truth.
- **[CODE-READ]** The reused one-layer gNB demod combines four RX channel estimates by matched filtering. For rank one under equal white branch noise this is decision-equivalent to the MMSE direction, but explicit noise-regularized four-RX MMSE and numerical parity remain unverified. Transform-precoded, multilayer, PTRS/complex UCI and unsupported RV cases remain outside this first no-UCI CP-OFDM path.
- **[ANALYTICAL + OFFLINE VERIFIED controls]** A reported milestone requires a DL-trusted C-RNTI ticket, CRC-accepted DCI 0_1, current epoch/incarnation, U3 ticket, and a nonzero full TB CRC. All-zero TBs are excluded. The synthetic 0/5,000 known-RNTI noise control constrains the DCI stage only; a quantitative real-waveform false-accept rate still requires wrong-RNTI/wrong-ID replay controls and an observed TB pass.

## Replay and orchestrator commands

Baseline main-capture command actually run (no sudo, no GPU):

```sh
REPLAY_PREFIX='' REPLAY_OUT=/tmp/ulint_replays REPLAY_LAST=/tmp/ulint_replays/last \
BASECFG=/home/nicola/NICOLA/wt/rr-csd-d/tests/passive_rx/dgx/ue.passive.ota.ula.cfg \
CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 \
nice -n 19 /home/nicola/NICOLA/wt/rr-orchestration/csd_replay_user.sh ulint_b \
/home/nicola/NICOLA/wt/rr-csd-d/cmake_targets/ran_build/build 0.25 \
ISAC_CORE_PLAN=0 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_PDCCH_NID_BANK=1 \
ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_DCI11_HARD_REFUTE=1 \
ISAC_DCI11_HARQ_SEQ=1 ISAC_DCI11_STAGE2=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_UL_PUSCH_CHAIN=1
```

The fixture came only from `/home/nicola/NICOLA/ota_runs/replay_t26_k21_154001/run.log`, an earlier receiver GPU nID sweep on the older capture. To reproduce the fixture-assisted full main replay on CPU, create the same receiver-derived file and run:

```sh
cat >/tmp/ulint_older_coresets.txt <<'EOF'
CORESET nid=62922 sym=0 dur=1 grid_rb=0 groups=36 L=0 R=0 shift=0 ties=2
CORESET nid=10163 sym=0 dur=1 grid_rb=0 groups=36 L=0 R=0 shift=0 ties=1
EOF
REPLAY_PREFIX='' REPLAY_OUT=/tmp/ulint_replays REPLAY_LAST=/tmp/ulint_replays/last \
BASECFG=/home/nicola/NICOLA/wt/rr-csd-a/tests/passive_rx/dgx/ue.passive.ota.dldedicated.dgx.cfg \
CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_b CAP_SAMPLES=1474560000 CAP_TICK=208029025 \
nice -n 19 /home/nicola/NICOLA/wt/rr-orchestration/csd_replay_user.sh ulint_main_fixture \
/home/nicola/NICOLA/wt/rr-csd-d/cmake_targets/ran_build/build 0.25 \
ISAC_CORE_PLAN=0 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 ISAC_PDCCH_NID_BANK=1 \
ISAC_PDCCH_DISCOVERED_FILE=/tmp/ulint_older_coresets.txt \
ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 \
ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1 ISAC_PDCCH_KNOWN_PRESCREEN=1 \
ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_DCI11_HARD_REFUTE=1 \
ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 \
ISAC_PDSCH_SOLVER_REUSE=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_UL_PUSCH_CHAIN=1
```

For the older fixture-assisted full replay, substitute `CAP=/home/nicola/NICOLA/ota_runs/raw_pci415_12s_20261008 CAP_TICK=210036277`; sample count and all flags are the same.

**[DEFERRED GPU replay/OTA]** Use the orchestrator's current validated X410/DPDK launcher and DL dedicated config with this exact feature environment (the replay additionally sets `ISAC_CORE_PLAN=0`; no radio command was run here):

```sh
ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_CSIRS_DISC=1 ISAC_CSIRS_DISC_CUMULATIVE=1 \
ISAC_PDCCH_NID_BANK=1 ISAC_PDCCH_NID_GPU=1 ISAC_PDCCH_NID_GPU_DUTY=1 \
ISAC_PDCCH_JOINT=1 ISAC_PDCCH_JOINT_ANON=1 ISAC_PDCCH_JOINT_ANON_ASYNC=1 \
ISAC_PDCCH_DED_EVERY_SLOT=1 ISAC_PDCCH_C0_OCC_GATE=1 ISAC_PDCCH_KNOWN_PRESCREEN=1 \
ISAC_PDCCH_USS_HASH=1 ISAC_PDCCH_DED_BUDGET_US=300 ISAC_DCI11_HARD_REFUTE=1 \
ISAC_DCI11_HARQ_SEQ=1 ISAC_PDSCH_SOLVER=1 ISAC_PDSCH_SOLVER_BUDGET=24 \
ISAC_PDSCH_SOLVER_REUSE=1 ISAC_RECONF=1 ISAC_METRICS=1 ISAC_UL_PUSCH_CHAIN=1
```

If the orchestrator's GPU replay finds a C-RNTI but cannot yet run the integrated binary, please provide its receiver-derived `JOINT_ANON_HIT`, DCI 1_1 length lock, dedicated CORESET record, each accepted raw DCI 0_1 payload/length/bank/absolute source slot, target slot, and CFO. The four-channel raw-IQ capture is already present in this worktree's host paths; those records identify the exact IQ windows for a CPU downstream fixture without gNB inputs.
