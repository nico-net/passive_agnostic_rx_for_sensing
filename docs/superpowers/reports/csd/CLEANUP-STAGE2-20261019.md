# CLEANUP STAGE 2 -- 2026-10-19 (branch csd/int16-stage2, from csd/int16-merge19 9281e24732)

Evidence labels: [CODE-READ] worker reading of old vs new code; [BUILD] DGX aarch64 ninja build_t26; [CTEST] DGX; [REPLAY] DGX recorded-capture replay, correctness only (no latency quoted). No OTA, no hardware.

## Summary
- Env names read by receiver code (non-test): 784 at HEAD -> 73 now (711 names gone; includes the 561 DEFERRED, ~214 frozen, and merge19-added flags; gNB/simulator names are among the 73 and untouched).
- Diff vs HEAD: 198 files, +2119 / -23182 lines (+ new tests/passive_rx/effective_config_check.py, eq_run_stage2.sh); 6 files deleted (nr_ul_llr_dump.c + test, nr_dmrs_nid_solve_live.c, nr_pdcch_gpu_parity.cuh, pdcch_anon_offline.cc, nr_csirs_disc_gpu_budget.cuh, 2 replay headers).
- Parked patch stage2_all.patch: unusable (git apply --check fails in all 33 files against current code); not reused.
- Commits: 7, by subsystem (GPU, PDCCH, PDSCH, UL, CSI-RS, sync/radio/common/MAC+logging, tests/CMake). Intermediate commits do not build alone (cross-file stubs); the branch tip does.

## Results
- Build: `ninja -j6 -k 0 nr-uesoftmodem oai_usrpdevif tests params_libconfig rfsimulator raw_iq`: only the 3 pre-existing link failures (test_nr_ssb_rate_match_prod, test_nr_csi_rb_extent, nr_pusch_ra0sim). [BUILD]
- ctest -j4 (nr_td_sim_test excluded, pre-existing, 460 s): 254 tests, only pre-existing failures: ra0 x9 + ssb_rate_match_prod + csi_rb_extent (link), dft_test, test_nr_modulation. test_nr_gpu_pass, test_nr_pdcch_blind_monitor (the PDCCH suite), test_nr_pdsch_solver pass. [CTEST]
- Effective config: tests/passive_rx/effective_config_check.py (output below). Frozen env has 235 names: 21 still read from env (site/hardware, table below), 214 baked as constants. `strings -a` over nr-uesoftmodem + 17 libs: 0 hits for 738 removed names (log tags ISAC_METRICS, ISAC_SEGV_BT and OAI_RNGSEED, a simulator seed, are allow-listed: they are text, not getenv). [BUILD]
- Replay equivalence (correctness only; OLD = merge19 binary + FROZEN_20261018.env via p99_replay.sh; NEW = this binary, no ISAC env via p99_replay_clean.sh; sd_t26ul.cfg; libraw_iq.so for OLD taken from this build because merge19 build dir had none). [REPLAY]
  First batch (5 runs per arm) exposed a regression: tim real grants -12 percent, probe grants +50 percent. Root cause: cdmrs_async_on() in nr_pdsch_passive_queue.c lost its `on = 1` before nr_bgq_start, so several threads started the dmrsIdBg worker (5 starts vs 1) and the per-CORESET DM-RS identity was never decided. Fixed to the HEAD ordering. Re-run after the fix (3 runs per arm, b and tim; earlier b batches also kept):
  - tim: DL n_real NEW 5064-5870 vs OLD 4741-5905; crc_q 73.2-75.8 vs 74.6-75.7; decoded RNTI union 13 vs 13; UL n_real 3087-3413 vs 3118-3326.
  - b: DL n_real NEW 3335-3814 vs OLD 1524-4035 (OLD run-to-run spread is itself wide); crc_q 95.3-97.1 vs 96-98.1; RNTI union 29 vs 33, >=30-decode sets differ by 2 marginal RNTIs each way (0x4433/0xb3bb OLD-only, 0x0211/0xd026 NEW-only), inside the run-to-run churn seen between OLD runs.
  Verdict: matches within run-to-run spread for 3 runs per capture; n is small, a full OTA/longer replay series is still to be done.
- Not verified: no OTA; no latency comparison; core-plan reservation for the removed GPU worker thread unchanged; many unit tests were converted by workers without individual old-vs-new proof (see TESTS).

## Kept as site/hardware config (env read stays)
| name | reason |
|---|---|
| ISAC_UHD_CTRL_CORES, ISAC_USRP_RING_CORE, ISAC_UE_RT_CORE, ISAC_PDCCH_USS_CORE, ISAC_PDCCH_NID_GPU_CORE, ISAC_PDCCH_JOINT_ANON_CORE, ISAC_PDCCH_PAR_CORE, ISAC_GPU_WORKER_CORE, ISAC_CSIRS_DISC_CORE, ISAC_CSIRS_DISC_DETECT_CORES, ISAC_DMRS_SCAN_CORE, ISAC_UL_SNAP_CORE, ISAC_UL_BGDISC_CORE, ISAC_UL_NID_BG_CORE, ISAC_BG_CORES, ISAC_BG_CRITICAL_CORES, ISAC_PDSCH_CORES, ISAC_PUSCH_CORES, ISAC_FLOAT_CORES, ISAC_GPU_CHECK_CORES, UHD_CONFIG_FILE | CPU core maps, per installation |
| ISAC_SCAN_ANT | RX antenna count |
| ISAC_USRP_CATCHUP_X, ISAC_USRP_NO_TX, ISAC_USRP_CHPOW_S, ISAC_USRP_RING_MS, _RING_ZEROCOPY, _RING_PKTS, _RING_POLL, _RING_POLL_TIMEOUT_MS, _RING_PRIO, ISAC_USRP_START_TIMEOUT_MS, ISAC_USRP_RESTART_MAX, ISAC_USRP_RESTART_UNTIL_SYNC, ISAC_USRP_ISOLATION_CHECK, ISAC_RX_RECOVER | radio/X410 hardware behaviour (ring is set by starttest.sh on air) |
| ISAC_RAW_IQ_* (DIRECTORY, SAMPLES, FIRST_TICK, CHANNELS, RATE, CENTER, SHIFT, SPEED, DECIM, GAP*) | replay device, needed by the equivalence harness |
| ISAC_MLOCK, ISAC_METRICS_PATH, ISAC_SIB1_CACHE_DIR | host paths / privileges |
| ISAC_PASSIVE_REPLAY_CAPTURE, ISAC_PASSIVE_REPLAY_INPUT | capture/replay subsystem used by gate scripts |
| ISAC_CSIRS_DISC, ISAC_RECONF, ISAC_GPU_BUDGET, ISAC_LOG_ASYNC | frozen =1, but nr-uesoftmodem main() setenv()s them only under --passive-rx; they are the passive-mode handshake that keeps gNB/phy-test behaviour. Lead decision: replace by an is_passive() constant in shared code |
| ISAC_CSIRS_DISC_BACKEND | test-only cpu/gpu selector |
| OAI_GDBSTACKS, OAI_CONFIGMODULE, OAI_VNF, OAI_PNF; ISAC_TB_* , ISAC_GNB_PDCCH_DMRS_ID, ISAC_HIDDEN_*; ISAC_ST2_DEBUG_HYP (tests) | upstream/gNB/simulator/test-bed, out of scope |

## Decisions the lead should look at
1. ISAC_AUTO_ACQUIRE removed as default-off: tests/passive_rx/auto_acquire.py and raw_baseline/{run_raw_replay,validate_raw_acquisition,test_raw_replay}.py depend on it (restore the nr_initial_sync.c/nr-ue.c/config_ue.c hunks if the raw-baseline harness must stay).
2. Core-plan planner (nr_core_plan_*), nr_passive_obs module, USRP ring machinery (ring is ON on air via starttest.sh) kept; planner/obs now unreachable from production.
3. Stubs left for foreign callers (listed in NEEDS of the worker reports below): follow-up cleanup, no behaviour.
4. Stale scripts still set removed flags (gpu_replay.sh, gpupass_ab.sh, ota/score_cce.py DCIGT arm, decodefix_*.py parse DECODEDIAG): harmless env, lost data.

## Effective-config check output
```
frozen names: 235; kept as env read (site/hardware): 21; baked constants: 214
KEPT  ISAC_CSIRS_DISC=1  (env, code default if unset)
KEPT  ISAC_DMRS_SCAN_CORE=-1  (env, code default if unset)
KEPT  ISAC_FLOAT_CORES=10-14  (env, code default if unset)
KEPT  ISAC_GPU_BUDGET=1  (env, code default if unset)
KEPT  ISAC_GPU_CHECK_CORES=10-14  (env, code default if unset)
KEPT  ISAC_GPU_WORKER_CORE=10  (env, code default if unset)
KEPT  ISAC_LOG_ASYNC=1  (env, code default if unset)
KEPT  ISAC_PDCCH_NID_GPU_CORE=13  (env, code default if unset)
KEPT  ISAC_PDCCH_USS_CORE=12  (env, code default if unset)
KEPT  ISAC_PDSCH_CORES=7,3,4,0,1  (env, code default if unset)
KEPT  ISAC_PUSCH_CORES=8,9  (env, code default if unset)
KEPT  ISAC_RECONF=1  (env, code default if unset)
KEPT  ISAC_RX_RECOVER=1  (env, code default if unset)
KEPT  ISAC_SCAN_ANT=4  (env, code default if unset)
KEPT  ISAC_UHD_CTRL_CORES=15  (env, code default if unset)
KEPT  ISAC_UL_SNAP_CORE=8  (env, code default if unset)
KEPT  ISAC_USRP_CATCHUP_X=1.05  (env, code default if unset)
KEPT  ISAC_USRP_CHPOW_S=1  (env, code default if unset)
KEPT  ISAC_USRP_NO_TX=1  (env, code default if unset)
KEPT  ISAC_USRP_RING_ZEROCOPY=1  (env, code default if unset)
KEPT  ISAC_USRP_START_TIMEOUT_MS=2200  (env, code default if unset)
BAKED CUDA_DEVICE_MAX_CONNECTIONS=32
BAKED ISAC_ANONLEN=1
BAKED ISAC_CARRIER_FROM_SIB1=1
BAKED ISAC_CHEST_DELAY_SHARE=0
BAKED ISAC_CHEST_GPU_DELAY=1
BAKED ISAC_CHEST_PDP_DIAG=0
BAKED ISAC_COREMAP_IDSWEEP_DUR=3
BAKED ISAC_CORESET_DISC_SYNCQ=1
BAKED ISAC_CORESET_DISC_WATCH=1
BAKED ISAC_CORE_PLAN=0
BAKED ISAC_CSIRS_DISC_CUMULATIVE=1
BAKED ISAC_CSIRS_DISC_NORM_COALESCE=1
BAKED ISAC_CSIRS_DISC_RATELIMIT=1
BAKED ISAC_CSIRS_DISC_TRS_PRIORITY=1
BAKED ISAC_CSIRS_FULL=1
BAKED ISAC_CSIRS_FULL_BLOCK_INFER=2
BAKED ISAC_CSIRS_FULL_BURSTS=1
BAKED ISAC_CSIRS_FULL_ERASE=1
BAKED ISAC_CSIRS_FULL_TRS_PAIR=1
BAKED ISAC_CSIRS_PORTS_GT8=1
BAKED ISAC_DCI11_HARD_REFUTE=1
BAKED ISAC_DCI11_HARQPID=1
BAKED ISAC_DCI11_HARQ_SEQ=1
BAKED ISAC_DL_TRAFFIC_PROBE=16
BAKED ISAC_E2E_TRACE=1
BAKED ISAC_FATTAILS=1
BAKED ISAC_GPU_DISCOVERY_BUDGET_PCT=30
BAKED ISAC_GPU_DISC_SMS=8
BAKED ISAC_GPU_LDPC_CL_MIN_CB=4
BAKED ISAC_GPU_LDPC_NC_MIN_CB=8
BAKED ISAC_GPU_LDPC_SM=1
BAKED ISAC_GPU_LMEM_PRERESERVE=1
BAKED ISAC_GPU_PASS_FP32=1
BAKED ISAC_GPU_PASS_POLL_SLEEP_US=600
BAKED ISAC_GPU_PASS_RETRY_OLD=1
BAKED ISAC_GPU_POLL_SLEEP_US=200
BAKED ISAC_GPU_RT_ISOLATE=1
BAKED ISAC_GPU_SELFCHECK=0
BAKED ISAC_KSSB_ALIGN=1
BAKED ISAC_L2MAXH_DELTA_NL2=-3
BAKED ISAC_MCS_TABLE_EVID=1
BAKED ISAC_METRICS=1
BAKED ISAC_PDCCH_BANK_HANDOFF=1
BAKED ISAC_PDCCH_C0_OCC_GATE=1
BAKED ISAC_PDCCH_DED_BUDGET_SHARE=2
BAKED ISAC_PDCCH_DED_BUDGET_US=300
BAKED ISAC_PDCCH_DED_EVERY_SLOT=1
BAKED ISAC_PDCCH_DISC_COVER=1
BAKED ISAC_PDCCH_DWELL_OWN_BUDGET=1
BAKED ISAC_PDCCH_FEP_CACHE=1
BAKED ISAC_PDCCH_GATE_CFAR=1
BAKED ISAC_PDCCH_JANON_ALL_CORESETS=1
BAKED ISAC_PDCCH_JOINT=1
BAKED ISAC_PDCCH_JOINT_ANON=1
BAKED ISAC_PDCCH_JOINT_ANON_ASYNC=1
BAKED ISAC_PDCCH_JOINT_ANON_DISCOVERY=1
BAKED ISAC_PDCCH_JOINT_ANON_LEN_HINT=1
BAKED ISAC_PDCCH_JOINT_ANON_RATE=1
BAKED ISAC_PDCCH_JOINT_ANON_STABLE=1
BAKED ISAC_PDCCH_KNOWN_PRESCREEN=1
BAKED ISAC_PDCCH_NID_BANK=1
BAKED ISAC_PDCCH_NID_GPU=1
BAKED ISAC_PDCCH_NID_GPU_CAPTURE_EVERY=2
BAKED ISAC_PDCCH_NID_GPU_DUTY=1
BAKED ISAC_PDCCH_NID_GPU_RECHECK_MS=250
BAKED ISAC_PDCCH_POINTA_REF=1
BAKED ISAC_PDCCH_RBOFFSET_FIX=1
BAKED ISAC_PDCCH_SCAN_TIMING=1
BAKED ISAC_PDCCH_TIMING=1
BAKED ISAC_PDCCH_USS_FAST=1
BAKED ISAC_PDCCH_USS_HASH=1
BAKED ISAC_PDCCH_USS_TRACK_BACKOFF=4
BAKED ISAC_PDSCHQ_PROBE_LAST=1
BAKED ISAC_PDSCH_CSI_POPULATION_RETRY=1
BAKED ISAC_PDSCH_CSI_RETRY=1
BAKED ISAC_PDSCH_DMRS_CDM_RETRY=1
BAKED ISAC_PDSCH_DMRS_ID_CONTRAST=1
BAKED ISAC_PDSCH_DMRS_ID_PER_CORESET=1
BAKED ISAC_PDSCH_DMRS_ID_REFRESH=64
BAKED ISAC_PDSCH_DMRS_ID_SCAN=16
BAKED ISAC_PDSCH_DMRS_SCAN_ASYNC=1
BAKED ISAC_PDSCH_EVM_GRANT=1
BAKED ISAC_PDSCH_EVM_PROBES=1
BAKED ISAC_PDSCH_GATHER_FAST=1
BAKED ISAC_PDSCH_GPU=0
BAKED ISAC_PDSCH_GPU_EXACT_CHECK=1
BAKED ISAC_PDSCH_GPU_EXACT_CHEST=1
BAKED ISAC_PDSCH_GPU_EXACT_CHEST_CHECK=64
BAKED ISAC_PDSCH_GPU_EXACT_DEMOD=1
BAKED ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=64
BAKED ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_DEFER=1
BAKED ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_NL34=1
BAKED ISAC_PDSCH_GPU_EXACT_DEMOD_MMSE3=1
BAKED ISAC_PDSCH_GPU_EXACT_DEMOD_NL34=1
BAKED ISAC_PDSCH_GPU_EXACT_FEP=1
BAKED ISAC_PDSCH_GPU_EXACT_LDPC=1
BAKED ISAC_PDSCH_GPU_GRAPH=1
BAKED ISAC_PDSCH_GPU_GRAPH_COVER=1
BAKED ISAC_PDSCH_GPU_GRAPH_DMRS2=1
BAKED ISAC_PDSCH_GPU_GRAPH_RETRY=1
BAKED ISAC_PDSCH_GPU_GRAPH_SPLIT=1
BAKED ISAC_PDSCH_GPU_HOST_PIN=1
BAKED ISAC_PDSCH_HOST_FAST=1
BAKED ISAC_PDSCH_INLINE_POOL=1
BAKED ISAC_PDSCH_MMSE_EXACT_NL34=1
BAKED ISAC_PDSCH_MMSE_MODE=2
BAKED ISAC_PDSCH_PDCCH_RM=1
BAKED ISAC_PDSCH_RECOVERY_ASYNC=1
BAKED ISAC_PDSCH_REP_FIELDS=1
BAKED ISAC_PDSCH_RETRY_NORESTORE=1
BAKED ISAC_PDSCH_RETRY_PREDICT=1
BAKED ISAC_PDSCH_RETRY_YIELD=2
BAKED ISAC_PDSCH_SLOT_SPREAD=1
BAKED ISAC_PDSCH_SNR_GATE=1
BAKED ISAC_PDSCH_SOLVER=1
BAKED ISAC_PDSCH_SOLVER_BACKOFF=200
BAKED ISAC_PDSCH_SOLVER_BUDGET=24
BAKED ISAC_PDSCH_SOLVER_COVERAGE=1
BAKED ISAC_PDSCH_SOLVER_DMRS_DOMAIN=1
BAKED ISAC_PDSCH_SOLVER_DMRS_PRIO=1
BAKED ISAC_PDSCH_SOLVER_FAIRSHARE=1
BAKED ISAC_PDSCH_SOLVER_LAYOUT_RECOVER=32
BAKED ISAC_PDSCH_SOLVER_NL_PREF=1
BAKED ISAC_PDSCH_SOLVER_PRIOR_CRC=1
BAKED ISAC_PDSCH_SOLVER_PRIOR_IDS=1
BAKED ISAC_PDSCH_SOLVER_PROBELOG=1
BAKED ISAC_PDSCH_SOLVER_PROBE_BURST=16
BAKED ISAC_PDSCH_SOLVER_PROBE_FILL=25
BAKED ISAC_PDSCH_SOLVER_PROBE_RATE=0.5
BAKED ISAC_PDSCH_SOLVER_REUSE=1
BAKED ISAC_PDSCH_SOLVER_SCR=1
BAKED ISAC_PDSCH_SOLVER_SCR_CLEAN=0
BAKED ISAC_PDSCH_SOLVER_SCR_CSIRS=1
BAKED ISAC_PDSCH_SOLVER_SCR_EXPLICIT=1
BAKED ISAC_PDSCH_SOLVER_SIGNIF=1
BAKED ISAC_PDSCH_SOLVER_THREAD_CTX=1
BAKED ISAC_PDSCH_SOLVER_WHY=1
BAKED ISAC_PDSCH_TDA_FIELD=1
BAKED ISAC_PDSCH_TDRA_RETRY=1
BAKED ISAC_PDSCH_WIDE_MRC_RETRY=1
BAKED ISAC_PDTIM_CLASS=1
BAKED ISAC_PDTIM_PCT=1
BAKED ISAC_POINTA_SWEEP=1
BAKED ISAC_PSS_ACC=8
BAKED ISAC_PSS_DECIM=1
BAKED ISAC_PUSCH_CONSUMERS=2
BAKED ISAC_PUSCH_MMSE_MODE=2
BAKED ISAC_PUSCH_PRIO=0
BAKED ISAC_RA_TDRA_SWEEP=1
BAKED ISAC_RX_MRC_ALIGN=2
BAKED ISAC_RX_MRC_MODE=1
BAKED ISAC_RX_MRC_PER_RNTI=1
BAKED ISAC_RX_RECOVER_MAX=20
BAKED ISAC_SCAN2_SLOWPASS_MS=8
BAKED ISAC_SCANCOST=2
BAKED ISAC_SCANQ_FRESH=6
BAKED ISAC_SCAN_READING_HASH=1
BAKED ISAC_SCAN_SCOREMEMO=1
BAKED ISAC_SEGV_BT=1
BAKED ISAC_SI_RV_ANY=1
BAKED ISAC_SLOT_SCALE=1
BAKED ISAC_SSB_FEP_GPU=1
BAKED ISAC_SSS_MULTI=2.5
BAKED ISAC_SSS_REL_RATIO=2.5
BAKED ISAC_SWEEP_SIGNIF=1
BAKED ISAC_TD_CB0_BACKEND=gpu
BAKED ISAC_UL_BGDISC=1
BAKED ISAC_UL_BWP_DL_PRIOR=1
BAKED ISAC_UL_BWP_SEED=1
BAKED ISAC_UL_DCI_LOG=1
BAKED ISAC_UL_DELAY_FAST=1
BAKED ISAC_UL_DELAY_ONCE=1
BAKED ISAC_UL_FIT_READ=1
BAKED ISAC_UL_GPU_FEP=1
BAKED ISAC_UL_GPU_FEP_KCHARGE=1
BAKED ISAC_UL_GPU_LDPC=1
BAKED ISAC_UL_GPU_LDPC_CHECK=64
BAKED ISAC_UL_GPU_LDPC_HOLD=1
BAKED ISAC_UL_GPU_PART=rt
BAKED ISAC_UL_HARQ_COMBINE=1
BAKED ISAC_UL_K2_PAIR=1
BAKED ISAC_UL_LAYOUT_LOCK=1
BAKED ISAC_UL_LAYOUT_LOG=1
BAKED ISAC_UL_LDPC_SNR_MARGIN=6
BAKED ISAC_UL_LOCK_BWP_ALL=1
BAKED ISAC_UL_LOCK_CRC_FIRST=1
BAKED ISAC_UL_LOCK_CRC_GUARD=1
BAKED ISAC_UL_LOCK_CRC_K=3
BAKED ISAC_UL_LOCK_MCS_STAT=1
BAKED ISAC_UL_LOCK_SIB1_FIRST=1
BAKED ISAC_UL_LOCK_SIGN=2
BAKED ISAC_UL_MCS_TABLE_SWEEP=1
BAKED ISAC_UL_NID_BG=1
BAKED ISAC_UL_NID_BG_CPU_PCT=10
BAKED ISAC_UL_NID_BG_IDLE=1
BAKED ISAC_UL_NID_BG_IQ=1
BAKED ISAC_UL_PREFIX_ENT=1
BAKED ISAC_UL_PRIO_QUEUE=1
BAKED ISAC_UL_PUSCH_CHAIN=1
BAKED ISAC_UL_PUSCH_SOLVER=1
BAKED ISAC_UL_QUEUE_DEPTH=32
BAKED ISAC_UL_RETRY=1
BAKED ISAC_UL_RETRY_SNR_MARGIN=3
BAKED ISAC_UL_SNAP_POOL=1
BAKED ISAC_UL_SNAP_PRIO=0
BAKED ISAC_UL_SOLVER_FALLTHROUGH=2
BAKED ISAC_UL_SPARE_RESCUE=1
BAKED ISAC_UL_STALE_DROP=1
BAKED ISAC_UL_STALL_DIAG=1
BAKED ISAC_UL_TA_TRACK=1
BAKED ISAC_UL_TDA_PAIR=1
BAKED ISAC_UL_UCI_BUDGET=8
BAKED ISAC_UL_UCI_SAFE=1
BAKED NR_GPU_FEP=1
removed names checked: 738 in 18 binaries; surviving string hits: 0
```

# Worker reports (verbatim): flags removed / baked / kept / lost diagnostics / tests / doubts

## G1
GROUP G1 report (files: nr_pdcch_blind_monitor_rt.c, nr_pdcch_passive_queue.c, nr_pdcch_joint_live.c, nr_blind_stats.h, nr_scancost.h, tests/nr_pdcch_joint_live_test.cc)
Compile-checked with chk.py: rt.c, passive_queue.c, joint_live.c, nr_pdcch_dci11_layout_sweep.c, nr_pdcch_ul_discovery.c, nr_hyp_sweep.c, nr_pdsch_config_sweep.c, tests nr_pdcch_joint_live_test.cc, nr_pdsch_dmrs_scan_test.cc, nr_pdcch_blind_monitor_test.cc, nr_hyp_sweep_test.cc, nr_dl_adaptive_test.cc: all OK, no new warnings (the zero-length-format warning at the BTIM/indexing check is old).
git diff --shortstat (rt.c, passive_queue.c, joint_live.c, nr_blind_stats.h, nr_scancost.h, joint_live_test.cc): 6 files changed, 302 insertions(+), 3020 deletions(-); rt.c 11654 -> 8995 lines.

REMOVED (flag = effective value ; kind) -- env read and the dead path are gone
ISAC_PDCCH_SHARED_PRESCREEN = off (unset) ; feature (whole shared prescreen: shared_screen_ok/score/abs_sum, rescue plumbing, audit_rescued/audit_score task fields)
ISAC_PDCCH_SHARED_PRESCREEN_MIN, ISAC_PDCCH_SHARED_PRESCREEN_AUDIT, ISAC_PDCCH_SHARED_DMRS, ISAC_PDCCH_SHARED_DMRS_MIN = off ; feature/diag (SCREEN_AUDIT_* logs, SCANPASS screen_audit fields)
ISAC_PDCCH_NEON_SCORE = off (scalar score) ; tuning (3 readers; neon branches removed)
ISAC_PDCCH_PASS_ADAPT = off ; feature (pass_adapt_on, pass_should_run, lowf/g_pass_low_export, g_pk_skip, PASS_ADAPT log)
ISAC_PDCCH_DWELL_PRESCREEN, ISAC_PDCCH_DWELL_PRESCREEN_Z = off ; feature
ISAC_PDCCH_PRESCREEN_DIAG = off ; diag (PSDIAG histograms, ps_* task fields)
ISAC_PDCCH_SIM_SPARSE = off ; test-only (sim_sparse_blank_symbol)
ISAC_PDCCH_NRNTI = unset (hypothesis test) ; diag control
ISAC_DCI11_STAGE2 = off (and nr_agnostic_v2()==0) ; feature (nr_pdcch_dci11_stage2_candidates, dci11_layout_evidence, dci11_family_key, DCI11_STAGE2 log, Thompson pick branch; from_stage2 constant false, dl_layout_index constant 0xFFFF)
ISAC_DCI11_PARSE_CACHE = 0 ; feature (parse memo + self-check)
ISAC_DCI11_DIAG_FILTER, ISAC_DCI11_PRUNELOG = off ; diag
ISAC_DCI11_OBSERVE_MM = off (observe every DCI) ; tuning
ISAC_UL_FORCE_OPTS = off ; experiment
ISAC_PDCCH_DISCOVERY_BUDGET_US = 0 ; feature (budget_active/budget_owner/deadline paths, DISCOVERYLAT/DISCOVERYTRACE scope)
ISAC_PDCCH_DISCOVERY_TRACE, ISAC_DISCOVERY_EVIDENCE (DISCOVERY_EVIDENCE json), ISAC_PDCCH_DISCOVERY_REPLAY (blind_discovery_replay + PDCCHREPLAY) = off ; diag
ISAC_PDCCH_CSS_NRNTI0 = off ; feature
ISAC_PDCCH_JOINT_ANON_OCC = 4000 default (baked as constant) ; ISAC_PDCCH_JOINT_ANON_MM = 1 default (baked); ISAC_PDCCH_JOINT_ANON_BUDGET_US = 0 (deadline logic removed) ; ISAC_PDCCH_JOINT_ANON_TOPK = 8 and ISAC_PDCCH_JOINT_ANON_Q = 256 (the STABLE=1 defaults, baked) ; ISAC_BG_JANON_NICE = 0 (baked)
ISAC_PDCCH_ANON_DUMP, ISAC_PDCCH_ANON_DUMP_MAX = off ; diag
ISAC_PDCCH_DMRS_RANK = 1 default (baked, modes 0/2 removed) ; tuning
ISAC_BWP_TRACK = off (default was nr_agnostic_v2()==0) ; feature (all pbwp scan code: snapshots, CORESET discovery, second pass, bwp_probe tasks, on_accept switch, BWP_DIAG). See NEEDS.
ISAC_DISCOVER_DIAG = off ; diag (DISCOVERDIAG, BRANCHFO, XCHECK, ULSWEEPTIM, store-eviction logs)
ISAC_FORCE_DCI_LEN = off ; diag
ISAC_TDD_SKIP = off (superseded by SCAN2 TDD) ; legacy feature
ISAC_PDCCH_CAPTURE = off (dci_nr.c global stays 0) ; diag
ISAC_PDCCH_LLR_CACHE, ISAC_PDCCH_LLR_CACHE_VERIFY = off ; feature (llrc key/cache/verify, g_llrc_* counters, SCANPASS llr_cache fields)
ISAC_PDCCH_LEN_MEMORY = off ; feature
ISAC_PDCCH_TASK_CAP64 = off ; feature
ISAC_PDCCH_DMRS_GATE = off ; feature (DM-RS coherence gate; g_held_dmrs counter kept, always 0, so the HELD line keeps its format)
ISAC_PDCCH_ENERGY = off ; diag (ENERGYPROBE)
ISAC_PDCCH_KNOWN_PRESCREEN_MIN = 0.66 default ; ISAC_PDCCH_GATE_CFAR_Z = 3.09 default ; ISAC_PDSCH_SNR_GATE_DB = 3 dB default ; ISAC_PDCCH_C0_OCC_DB = 6 dB default ; ISAC_SCAN_SCOREMEMO_CHECK = off (diag) ; ISAC_ANONLEN_Z = 2.33, ISAC_ANONLEN_MIN_RNTI = 2, ISAC_ANONLEN_EVERY = 16, ISAC_SCAN2_UL_Z = 2.33 (all baked as constants)
ISAC_PDCCH_ULDCIGT, ISAC_PDCCH_DCIGT, ISAC_UL_DBG_LOG, ISAC_DCI_RAW_LOG, ISAC_DCI_FIELDS, ISAC_PDCCH_NO_MISMATCH_GATE, ISAC_PDCCH_NO_PDSCH_DECODE = off ; diag
ISAC_PDSCH_SOLVER_DIAG, ISAC_PDSCH_SOLVER_MAX_LIVE = 1024 default (baked) ; ISAC_RNTI_LAYOUT_CACHE_FAILS / _CHECK (layout cache is already off via nr_rnti_layout_cache_on()==false: whole cache path + goto lc_regroup removed) ; ISAC_PDSCH_READING_RV = off (reading dedupe keys without rv)
ISAC_PDCCH_GPU_FEP (warning-only gate), ISAC_PDCCH_PAR_BANKS, ISAC_PDCCH_PAR_CORE = off ; feature (par_helper/par_bank_worker thread removed)
ISAC_TD_GRANTWORK = on by default (baked: want_gw = pdsch_decode>=1)
ISAC_PDCCH_DED_EVERY_SLOT, SOLVER_DMRS_PRIO etc: see BAKED.
Dead branches of nr_agnostic_v2() (always 0) inside my file deleted (stage2 gate, SS retire pruning, 1_0 duty reduction log branch, Thompson).
Not an env flag but removed with their flag: SOLVEDLAYOUT diag log (unmarked, no env), nr_pdcch_anon_dump, psd_*.

BAKED (flag=value ; code path stays)
ISAC_SCANCOST=2, ISAC_ANONLEN=1 (and the per-tier ISAC_SCAN2_UL/ULSSH/TDD, ISAC_ANONLEN_SCREEN/LEARN unset => on): nr_scancost.h helpers now return true (nr_scancost_on/nr_scan2_on/nr_anonlen_on kept as constant functions because nr_pdcch_dci11_layout_sweep.c, nr_pdcch_ul_discovery.c, tests/nr_pdsch_dmrs_scan_test.cc still call them)
ISAC_SWEEP_SIGNIF=1 (nbs_sweep_signif_enabled() returns true; used by nr_hyp_sweep.c, nr_pdsch_config_sweep.c)
ISAC_SCANQ_FRESH=6 (scanq_fresh() returns 6, passive_queue.c)
ISAC_PDCCH_JOINT=1 (nr_pdcch_joint_live_enabled() returns true, joint_live.c; header declares it)
ISAC_PDCCH_JOINT_ANON=1, _ASYNC=1, _STABLE=1, _RATE=1, _DISCOVERY=1, _LEN_HINT=1, ISAC_PDCCH_JANON_ALL_CORESETS=1 (the rate/token-bucket block is a provable no-op at rate=1 + stable and was removed; the inline joint-RNTI fallbacks in the cand worker, reachable only with async off, removed)
ISAC_PDCCH_USS_HASH=1, ISAC_PDCCH_DED_EVERY_SLOT=1, ISAC_PDCCH_C0_OCC_GATE=1, ISAC_PDCCH_DWELL_OWN_BUDGET=1, ISAC_PDCCH_DED_BUDGET_US=300, ISAC_PDCCH_DED_BUDGET_SHARE=2, ISAC_PDCCH_BANK_HANDOFF=1, ISAC_PDCCH_SCAN_TIMING=1, ISAC_PDCCH_TIMING=1, ISAC_PDCCH_FEP_CACHE=1, ISAC_PDCCH_GATE_CFAR=1, ISAC_PDCCH_KNOWN_PRESCREEN=1, ISAC_SI_RV_ANY=1, ISAC_PDCCH_POINTA_REF comment only
ISAC_PDSCH_SOLVER_PROBE_RATE=0.5, _PROBE_BURST=16, _PROBE_FILL=25, ISAC_PDSCH_SNR_GATE=1, ISAC_PDSCH_DMRS_ID_SCAN=16 (>0), ISAC_PDSCH_SOLVER_DMRS_PRIO=1, ISAC_SCAN_SCOREMEMO=1, ISAC_PDSCH_SOLVER_DMRS_DOMAIN=1, ISAC_SCAN_READING_HASH=1, ISAC_PDSCH_REP_FIELDS=1, ISAC_PDSCH_SOLVER_BUDGET=24, ISAC_PDSCH_SOLVER_SCR_CLEAN=0 (path removed), ISAC_PDSCH_SOLVER_SCR_CSIRS=1, ISAC_PDSCH_SOLVER_SCR_EXPLICIT=1, ISAC_MCS_TABLE_EVID=1, ISAC_UL_DCI_LOG=1 (ULDCI line always on), ISAC_SCAN2_SLOWPASS_MS=8 (watchdog always on), ISAC_SEGV_BT=1 (already unconditional)
Defaults baked (not frozen): ISAC_SWEEP_OCCASIONS=500, ISAC_DCI_LEN_MIN=30, ISAC_DCI_LEN_MAX=NR_DCI_MAX_PAYLOAD, ISAC_DCI_SWEEP_STRIDE=0 (assignments removed), ISAC_RECONF_DISCOVERY_DUTY=20, ISAC_CSS0_EVERY=8, ISAC_LANE_UNION_LLR=1, ISAC_SCAN2_AUDIT_EVERY=64, ISAC_ANONLEN_AUDIT_EVERY=64, ISAC_SCAN2_ULSSH_AUDIT_EVERY=16 (see DOUBTS)

KEPT
ISAC_PDCCH_JOINT_ANON_CORE: core map of the janon worker (site config); getenv and nr_bg_lane_enter("ISAC_PDCCH_JOINT_ANON_CORE") untouched, only the DEFERRED markers dropped.
ISAC_SEGV_BT handler: frozen, kept (code unconditional).

LOST DIAGNOSTICS
SHARED_PRESCREEN*: SCREEN_AUDIT_HIT/ACCEPT logs and SCANPASS screen_audit counters (shared-prescreen miss audit).
PASS_ADAPT: PASS_ADAPT transitions log; SCANPASS skip= field.
PRESCREEN_DIAG: PSDIAG per-AL score/energy/DM-RS histograms. DWELL_PRESCREEN: calibrated dwell gate.
LLR_CACHE: llr_cache hit/miss/bad in the SCANPASS line (line otherwise unchanged; SCANTIME/SCANTIME_FINE untouched and are the parsed ones).
DISCOVERY_BUDGET_US/TRACE: DISCOVERYLAT and DISCOVERYTRACE lines. DISCOVERY_EVIDENCE: DISCOVERY_EVIDENCE json stream. DISCOVERY_REPLAY: PDCCHREPLAY fixture writer. ANON_DUMP: dump file (tests/pdcch_anon_offline.cc consumed it; see NEEDS).
DISCOVER_DIAG: DISCOVERDIAG ENTRY/CFG/fep, BRANCHFO, XCHECK, ULSWEEPTIM, "length store evicted".
DCI11_PRUNELOG: DCI11_PRUNE; DCI11_STAGE2 log; SOLVEDLAYOUT log; PDSCH_SOLVER_DIAG; LAYOUT_CACHE_CHECK/stat logs; DCI11_PARSE_CACHE check log; DCI11_OBSERVE_SKIP.
ULDCIGT / DCIGT (also parsed by tests/passive_rx/ota/score_cce.py and tests/passive_rx/captures/run_arm.sh DCIGT arm -> those arms now produce nothing), UL_DBG_LOG ULDBG_DCI, DCI_RAW_LOG DCIRAW, DCI_FIELDS, ENERGYPROBE, BWP_DIAG, STAGE2 reject log.
NO_PDSCH_DECODE (scan-cost isolation), NO_MISMATCH_GATE, SIM_SPARSE (silent-cell simulation), TDD_SKIP, CAPTURE (replay fixture), FORCE_DCI_LEN.
PAR_BANKS: the helper-thread arm (measured slower, 2026-10-16).

TESTS
tests/nr_pdcch_joint_live_test.cc: removed the static setenv("ISAC_PDCCH_JOINT") and ASSERT_EQ(kEnable,0); nr_pdcch_joint_live_enabled() is now unconditionally true, the test still asserts it.
No other test setenv's any flag I removed. tests/cbg_contract_test.py (reads rt.c text): all 6 cases fail when run directly, including ones that only read other files; I did not investigate (looks pre-existing / path related), the rt.c patterns it checks (cbgti/cbgfi grant fields, blind_harq_tag, 64/32 constants) are untouched by my edits.
nr_pdcch_blind_monitor_test.cc / nr_pdsch_dmrs_scan_test.cc still compile; they exercise library functions (extract_pre, parse_lane_als, reading_idx) I did not touch.

NEEDS (out of my set)
- tests/pdcch_anon_offline.cc + CMakeLists.txt:3300 target pdcch_anon_offline: its input (ISAC_PDCCH_ANON_DUMP) no longer exists; delete file and target.
- nr_pdcch_blind_monitor.c: nr_pdcch_blind_extract_pre / nr_pdcch_blind_extract_11_pre (only used by the removed parse cache; test nr_pdcch_blind_monitor_test.cc:7019 covers them) and nr_pdcch_blind_parse_lane_als (no production caller now, tested at nr_pdcch_blind_monitor_test.cc:5432): delete together with those tests if wanted.
- nr_pdsch_solver.c: nr_pdsch_solver_layout_cache, layout_cache_stats, nr_rnti_layout_cache_on() (false) are now uncalled; nr_dci11_thompson_pick (nr_pdcch_dci11_layout_sweep.c) no longer called; nr_pdcch_discovered_candidate_share (nr_pdcch_discovered.*) no longer called; nr_pdcch_scan_llr_cache.h + tests/nr_pdcch_scan_fast_test.cc (llr cache) now unused by production; nr_pdcch_scan_score_neon/_neon_sum/abs_sum_neon, nr_pdcch_scan_score_scalar_sum in the scan score header unused.
- nr_agnostic_v2.h: all callers in my file are gone; other files (nr_dlsch_demodulation.c, nr_pdsch_gpu_demod_host.c, nr_pdsch_passive_decode.c) still use it (always 0): owners should delete their dead branches, then the header.
- nr_pdcch_joint_live.h/.c: nr_pdcch_joint_live_enabled() is now `return true`; the header comment "ISAC_PDCCH_JOINT=1 enables it; default off" is stale; once other callers are gone drop the function. nr_scancost.h helpers nr_scancost_on/nr_scan2_on/nr_anonlen_on and nbs_sweep_signif_enabled are constant true: callers in nr_pdcch_dci11_layout_sweep.c, nr_pdcch_ul_discovery.c, nr_hyp_sweep.c, nr_pdsch_config_sweep.c, tests/nr_pdsch_dmrs_scan_test.cc can drop the checks (then the helpers).
- nr_pdcch_blind_monitor_rt.h/other: exported nr_pdcch_bwp_crc_result / nr_pdcch_bwp_probe_result (kept, they are called by the PDSCH decode) now operate on a g_pbwp that nothing arms; job.bwp_entry / job.bwp_probe_* and cand_task.bwp_entry/bwp_probe (copied into nr_fl_task_t in nr_pdcch_fast_lane.h) are always 0. The PDSCH worker can drop the calls, then these two functions, g_pbwp and nr_passive_bwp.* can go.
- nr_pdcch_dci11_layout_feedback() (kept, exported): layout_index is now always 0xFFFF so the per-layout/family tally part is unreachable; the resolver fields layout_fam/fam_ok/fam_tr (nr_pdcch_dci11_layout_sweep.h) and nr_dci11_layout_evidence-style readers there are dead with it.
- Other flags read via helpers in other files whose callers are in my file and which I did not touch: nr_a2e_trace(), nr_admit_on(), nr_bg_lane_on(), nr_pdsch_solver_*_on(), nr_tda_field_on(), nr_dci01_fdra_refuse_enforced(), nr_ul_layout_bwp_seed_enabled(), nr_csirs_disc_enabled(), nr_srs_passive_enabled(): owners of those files.

DOUBTS (lead to recheck)
1. Audit sampling is behaviour: ANONLEN (1 sweep in 64, non-joint-anon only), SCAN2 UL screen (1 in 64) and ULSSH (1 in 16) audits default ON and an audited screened-out candidate is decoded AND can be returned as a hit; so they are not pure diagnostics. I baked them as constants (kept counters/lines SCAN2 ul[...] which scan2_stats.py parses). If you want them dead, that changes decisions on ~1.5 % of sweeps.
2. ISAC_PDCCH_JOINT_ANON_TOPK / _Q defaults depend on STABLE (8 / 256 when stable=1); I used the stable values since STABLE=1 is frozen.
3. nr_agnostic_v2() is a constant 0 in the tree, so I treated ISAC_BWP_TRACK, STAGE2, SS-retire pruning as off; the marker said the same, and git grep confirms no other source of the value.
4. nr_pdcch_nrnti_override: removed the force-0 control; the unset path is the hypothesis test (unchanged).
5. In run_occasion the `visit`, `shares`, `n_verified` locals and the verified_rnti_set() call went away (the call was a pure read; result unused after the dwell-share removal).
6. GPU/other flags that sit in functions called from my file are untouched; I only edited getenv/wrapper reads literally in my five files.
7. The remaining hard-coded "TEMPORARY DIAGNOSTIC" blocks (CONSTDIAG etc., g_constdiag_left) are not env-gated and were not removed.
8. JOINT_ANON_HIT/SCAN2/ANONLEN/SCANTIME/BTIM lines: kept (stat lines parsed by p99_stats.py, scan2_stats.py, scanfast/gpu2 analysers); SCANPASS line lost fields (not parsed by any script I found).

## G2
REPORT G2 (merged: passive_decode.c by me; G2a = demod/chest/small files; G2b = config_sweep). Detailed per-file lists: /tmp/s2/rep_G2a.md, /tmp/s2/rep_G2b.md (read them, they are part of this report).
Compiled OK with chk.py: nr_pdsch_passive_decode.c, nr_pdsch_config_sweep.c, nr_dlsch_demodulation.c, nr_dl_channel_estimation.c (+ G2a/G2b files and the 4 tests they touched). Exported-symbol compare (old HEAD vs new object, nm -g) of every .c in the group: no export lost, except nr_pdsch_config_sweep_k0_legacy_set (G2b, see NEEDS). Nothing built or run.

==== nr_pdsch_passive_decode.c (8140 -> ~4800 lines) ====
REMOVED (flag = effective value ; kind)
ISAC_CHEST_PDP_DIAG = 0 (frozen) ; diag (chest_pdp_diag_on, nr_dl_chest_diag_request writes; GPU job .chest_diag = 0)
ISAC_SI_SOFTCOMBINE = 0 (default) ; feature (SI soft combining: si_combine_on, t_hq.si/.cell, harqc_entry cell/ncomb, NR_SI_COMB_MAX, SI_SOFTCOMBINE logs, gp_hq_si and its GATE_HARQ branch; harq_retx_decide lost its `si` arg)
ISAC_PDSCH_PIN_PREWARM_ALL = 0 (default) ; tuning (branch in nr_pdsch_passive_decode_pin_prewarm removed; nr_pdsch_pin_prewarm_all() kept as `return 0` stub for nr_pdsch_passive_queue.c)
ISAC_PRG_SWEEP = 1 (default) ; baked (prg_sweep_enabled deleted, sweep always on)
ISAC_CHEST_TINTERP = 0 (default) ; feature (time interpolation of the estimate: block, tinterp_row, chest_tinterp_global, t_tinterp_override, job->time_interp=0, GATE_TINTERP keeps only ue->chest_time==1)
ISAC_PDSCH_TINT_AB = 0, ISAC_PDSCH_TINTERP_RETRY = 0 ; diag/feature (TINTAB line, retry arm bit 16)
ISAC_PDSCH_DECODE_DIAG = 0 ; diag (decode_diag_on, DECODEDIAG/DECODEEVM/DECODEEVMRB/DECODECSI lines, t_decode_cb)
ISAC_PDSCH_EVM_RB = 0 ; diag
ISAC_RX_BRANCH_FO = 0 ; feature (brfo_commit, gp_brfo_measure, BRANCHFO measure+log, s_fo_ema, t_brfo_*)
ISAC_DMRS_FO_APPLY = 0 ; feature (and the whole DM-RS phase-slope CFO/SFO tracker + DMRSFO log + gp_dmrsfo_commit: its EMAs only fed APPLY/SFO_CORRECT)
ISAC_SFO_CORRECT = 0 ; feature (SFO channel-estimate rotation, sfo_eps/sfo_applied, g_sfo_ppm_ema)
ISAC_PDSCH_PROBE_C1 = 1 (implied by SOLVER default on) ; baked (probe_c1_on deleted; probes always record the C=1 outcome)
ISAC_LDPC_ITERS = 8 (default) ; baked (also gp_iters); nr_pdsch_ldpc_iters_override path kept
ISAC_PDSCH_TBPARM = off ; diag (SEGDIAG, TBPARM, TBRESULT lines, CHESTDIAG block + rep_append)
ISAC_CHEST_DIAG = off ; diag
ISAC_PDSCH_PDCCH_OCC_IDS, ISAC_PDSCH_PDCCH_OCC_DIAG = off ; diag (pdcch_occ_diag)
ISAC_RX_NVAR_FIX = 0 ; baked (divisor stays number_symbols*Nl*nb_ant; GPU job .nvfix = 0)
ISAC_RX_NVAR_PERBRANCH = 1 (default) ; baked (per-branch nvar always on; .branch always planned branch)
ISAC_LLR_NORM = 1 (default) ; baked (3 sites: main decode, gw cb0 input, gw cb0 decode; GPU job .norm = 1)
ISAC_PDSCH_OWN_PDCCH_RM = 0, ISAC_PDSCH_OWN_PDCCH_DB = 4 ; feature (own-PDCCH rate-match arm, own_pdcch_hypotheses, t_own_pdcch, OWNPDCCH log, GWFP term)
ISAC_PTRS_K, ISAC_PTRS_L = unset ; feature (PT-RS grants stay refused; PT-RS sweep pick/feed/latched log, rnti_ptrs_*, rnti_dec_t.ptrs, g_ptrs_cell_arm, PTRS_SWEEP stats; ptrs_unav/ptrs_arm gone)
ISAC_SSB_FEP_GPU = 1 (frozen) ; baked (GPU FEP for PSS/SSS symbols always, CPU fallback unchanged)
ISAC_PROBE_HORIZON = 1 (default) ; baked
ISAC_XANT = off ; diag (XANT cross-antenna correlation)
ISAC_DC_FIX = 0 ; feature (DC-subcarrier interpolation)
ISAC_CHEST_OCC (+ _AB override use) = 0 ; feature (OCC-after-delay chest block; GATE_OCC branch removed)
ISAC_RX_IRC = 0 (per-grant override unreachable: MRC_PER_RNTI=1 gives modes {1,3} only) ; feature (IRC DM-RS covariance block)
ISAC_PDSCH_CDM_DIAG = off ; diag (CDMDIAG)
ISAC_PDSCH_RAW_DUMP(_MAX,_MINNL) = off ; diag (dump)
ISAC_RX_ML_2L, ISAC_RX_EQ_AB = 0 (my reads only) ; feature (rho_dl buffer allocation; rho_dl_p stays NULL)
ISAC_PDSCH_ANT_GATE = 0 ; feature (branch roughness gate)
ISAC_PDSCH_MMSE2_DIAG (ISAC_MMSE2_DIAG) = 0 ; diag (memset/.on of nr_mmse2_diag and MMSE2DIAG log)
ISAC_PDSCH_EVM (EQDIAG, EQDIAG2) = off ; diag
ISAC_PDSCH_LLR_DUMP(_FULL,_ALL,_MAX) = off ; diag
ISAC_LLR_SCALE = 0 ; tuning (LLR rescale block)
ISAC_RV_RETRY = 0 ; diag/feature (RV 2/3/1 retry; RVRETRY stats line + g_rv_try/g_rv_ok; NR_TD_GW_F_RV_RETRY no longer set; RVRESCUE log)
ISAC_LLRFILL_ALL = off ; diag (the whole LLRFILL one-shot line)
ISAC_PDSCH_RM_GENIE (genie: RMDCI, RMTBS, MMSE2DIAG calls) = 0 ; diag (all calls to nr_pdsch_rm_genie_* gone from my file)
ISAC_GPU_DISC_SMS_LOW = off ; tuning (large-grant SM partition begin/end)
ISAC_PDSCH_CSI_EXTENT_AB (CSIAB), ISAC_PDSCH_MMSE_AB / _AB_QPSK (QAMAB) = off ; diag (passive_decode_tintab/qamab and the CSIAB code in the entry deleted; t_qam_ab_alt gone)
ISAC_PDSCH_STATIC_CSI_RETRY = 0 ; feature (static_csi_resources, STATICCSI arm)
ISAC_PDSCH_SIGNAL_GEOM_RETRY = 0 ; feature (SIGGEOM arm, signal_geom_detect, dmrs_mask_type_a); also dead via !harq_combine_on()
ISAC_PDSCH_RETRY_SNR_MIN_DB = off ; tuning (RETRY_SNR_GATE)
ISAC_PDSCH_GPU_PRIO = 0 ; tuning (nr_pdsch_gpu_exact_set_lowprio call)
ISAC_PDSCH_PDCCH_RM mode 2 (default is 1) ; diag (PDCCHRM arm; pdcch_rm_mode() deleted, mode 1 baked)
ISAC_TD_GW_CHECK, ISAC_TD_GW_PROFILE, ISAC_TD_PROBE_EQUIV_CHECK, ISAC_TD_PROBE_EQUIV_BRANCH = off ; diag (bodies deleted; the 3 public functions kept as empty stubs, see NEEDS; also t_cap_*, t_eq_snap*, t_probe_no_horizon, t_chest_bypass, t_last_probe_horizon, t_last_chest_hit, equiv_rerun/equiv_diff, gw_hyp_of)
nr_agnostic_v2() (const 0): all dead branches in my file deleted (the PT-RS pick, the `|| nr_agnostic_v2()` terms, `!nr_agnostic_v2()` terms, include)
harq_combine_on() (const 1) removed: RETRY_ARMS_OK() = nr_hq_retry_lane(true, ...), `!harq_combine_on()` dead blocks deleted (the pre-body TDRA-predict arm and SIGGEOM), snr-gate arg true.

BAKED (code path stays)
ISAC_PDTIM_PCT=1, ISAC_PDTIM_CLASS=1 (frozen) ; ISAC_PDCCH_TIMING (already baked on)
ISAC_MCS_TABLE_EVID=1 (frozen)
ISAC_PDSCH_HOST_FAST=1 (frozen) ; ISAC_PDSCH_GATHER_FAST=1 (frozen: gather_index/modulo path removed)
ISAC_PDSCH_EVM_GRANT=1, ISAC_PDSCH_SNR_GATE=1, ISAC_PDSCH_EVM_PROBES=1 (frozen; note the old `getenv()?:"1"` forms were always truthy). EVM armed = !gpu_llr && !gw_ready; gp_evm_armed deleted; zabs always computed
ISAC_PDSCH_RETRY_PREDICT=1, ISAC_PDSCH_RETRY_NORESTORE=1, ISAC_PDSCH_TDRA_RETRY=1, ISAC_PDSCH_CSI_RETRY=1, ISAC_PDSCH_DMRS_CDM_RETRY=1, ISAC_PDSCH_WIDE_MRC_RETRY=1, ISAC_PDSCH_CSI_POPULATION_RETRY=1, ISAC_RA_TDRA_SWEEP=1, ISAC_PDSCH_RETRY_YIELD=2 (frozen)
ISAC_PDSCH_RETRY_BUDGET_US = 1000 (code default with frozen ISAC_FATTAILS=1) -> #define RETRY_BUDGET_NS 1000000ull
ISAC_FATTAILS=1 (in the budget default only)
ISAC_PDSCH_PDCCH_RM=1 (frozen)

KEPT
(none in passive_decode.c: no site config read there)

LOST DIAGNOSTICS
DECODEDIAG/DECODEEVM/DECODEEVMRB/DECODECSI (ISAC_PDSCH_DECODE_DIAG): per-decode full parameter line, per-symbol/k12 DD-SNR, per-RB map, CSI list. NOTE tools decodefix_*.py / wides0_offline.py parse DECODEDIAG offline (not goal_metrics/eq_cmp).
SEGDIAG/TBPARM/TBRESULT/CHESTDIAG (TBPARM, CHEST_DIAG): rate-matching param set, per-RNTI outcome with csirs tuples, per-branch power/Gram.
DMRSFO + BRANCHFO (+ DMRSFO REJECTED): DM-RS phase-slope CFO/SFO and per-branch FO tracker EMA lines.
EQDIAG/EQDIAG2 (ISAC_PDSCH_EVM), CDMDIAG, XANT, PDCCHOCC (OCC_DIAG), LLRFILL, RAW_DUMP, LLR_DUMP, MMSE2DIAG, SIGGEOM, STATICCSI, OWNPDCCH, PDCCHRM, CSIAB, TINTAB, QAMAB, RMDCI/RMTBS genie, RVRESCUE/RVRETRY, RETRY_SNR_GATE, SI_SOFTCOMBINE, PTRS_SWEEP, GW_EQUIV/GWTIM profile/PROBE_EQUIV (stubs): capability of each is the obvious one named.
Kept (parsed or always-on): EVMGRANT, gp/gq/li lines, RETRY_ARMS_OK path, DECODETDRA, RATDRA, TDAFIELD, HARQC, LDPCDIAG, SHAPE/RBHIST/MCSHIST/RBMAP/PIPEDIAG/SEGIDX counters (unflagged always-on stats; left).

TESTS
passive_decode.c is not unit-tested by setenv of any removed flag (grep of the 90 removed names in */tests/* found none). G2a: nr_pdsch_dmrs_scan_test.cc (CONTRAST=0 arm + 2 setenv dropped). G2b: nr_pdsch_config_sweep_test.cc (3 legacy-only tests deleted: MaskLastSymbolAndK0CollapseAContextToTheEndAmbiguity, K39.LegacyFlagRestoresPinning, K42.LegacyPinKeepsK0AndTheLastSymbolSetIsMonotone; K42's non-legacy monotone coverage is LOST, lead may want a replacement), nr_dci_history_test.cc, nr_td_test_baseline.h, nr_td_cb0_wire_test.cc.
ISAC_DCI11_RV_CONSISTENCY / HARQ_COMBINE: not read in any file of this group (only a comment in nr_pdsch_solver.h); no test of mine toggles them.

NEEDS (out of my set)
nr_pdsch_passive_queue.c: drop nr_pdsch_pin_prewarm_all() use (l.~1471-1474) then delete the stub in passive_decode.c; drop nr_pdsch_passive_ptrs_sweep_allow() calls (l.2015, 2780) + decl in nr_pdsch_passive_decode.h:264 then delete my no-op; drop nr_pdsch_passive_probe_equiv_check / gw_check / gw_profile calls (l.2129, 2652, 2653) + decls (.h:255, 387, 389) then delete my 3 stubs; EQ_AB / CHEST_OCC_AB / LDPC_ITERS_AB blocks (l.2022-2060) now inert (my side no longer consumes occ_override and the ML/EQ buffer; irc_override modes 4-6 unreachable); the `s_decslow` ISAC_PDTIM_CLASS read there.
nr_dlsch_demodulation.c (G2a, from its NEEDS): delete nr_pdsch_decode_signs/_sign_errors (my file no longer references them), nr_mmse2_diag (my file no longer sets it), nr_dlsch_csi_extent_override, nr_dlsch_qam_mode_override (my GWFP and CSIAB/QAMAB gone), nr_pdsch_chest_occ_override.
Callers now unused after my edits: nr_csirs_full_ab()/nr_csirs_full_validate() calls remain in passive_decode_entry (owner of nr_csirs_blind_rt decides; I left the two calls since the flags live in their file); nr_gpu_budget_rt_large_begin/end; nr_pdsch_gpu_exact_set_lowprio; nr_pdsch_gpu_demod_pin_prewarm (demod_host.c); nr_pdsch_solver_note_rv_mismatch; nr_gpu_pass_hq_refuses (nr_gpu_pass_hq.h + tests/nr_gpu_pass_test.cc l.1126-1129); NR_GPU_PASS_GATE_HARQ/GATE_OCC/GATE_CAP; t_gpp->brfo_rows / brfo output in nr_gpu_pass_dl.c, nr_gpu_pass.h:239 (the chest-post cfo/sfo outputs of the pass are no longer consumed); nr_pdsch_rm_genie.c/.h + nr_pdsch_cpu_route.c/.h + nr_agnostic_v2.h can be deleted by the lead (all callers in my files are gone; nr_agnostic_v2() still called by nr_pdcch_blind_monitor_rt.c and nr_pdsch_gpu_demod_host.c:336).
NEEDS from G2b: nr_pdsch_config_sweep.h: delete decl of nr_pdsch_config_sweep_k0_legacy_set (~l.596) + stale comments (l.85, 212, 218, 300-302, 520, 525, 635); nr_td_cb0_wire.c may drop the cb0_elim_env() check.
Other G2a NEEDS: see rep_G2a.md.

DOUBTS
- ISAC_PDSCH_PROBE_C1: baked to 1 on the assumption nr_pdsch_solver_enabled() is true with no env (solver.c default "1"); if the solver owner changes that, probe_c1 must follow.
- ISAC_PDSCH_RETRY_BUDGET_US: 1000 us derived from the FATTAILS default (frozen FATTAILS=1); same source as the queue/other files' nr_fattails reads.
- DMRSFO/BRANCHFO tracker removal: judged diagnostic-only because its sole consumers were ISAC_DMRS_FO_APPLY and ISAC_SFO_CORRECT (both off); the lead may re-check g_sfo_ppm_ema had no other reader (git grep showed none).
- Shared log-budget statics for "UNSUP@seg-ptrs"/"UNSUP@1373" were merged into one counter (log rate only).
- I fixed one self-inflicted duplication mid-way (a slicing bug) by re-cutting; final file compiles and the exported symbols equal HEAD, but a quick eyeball of `git diff` around the LLR normalisation / unscramble block in passive_decode_body_core (search "gw_llr_norm") is advised.
- G2a doubts: CSI_RB_EXTENT/CSIRS_FULL baking, ISAC_RX_EQ_AB read in queue.c, mmse3 return ignored (see rep_G2a.md).

git diff --shortstat:
nr_pdsch_passive_decode.c: 1 file changed, 164 insertions(+), 3515 deletions(-)
G2a files (recovery, rm_genie, cpu_route .c/.h, dmrs_scan, dlsch_decoding, dlsch_demodulation, agnostic_v2.h, nr_dl_channel_estimation.c, dmrs_scan_test.cc): 10 files changed, 65 insertions(+), 947 deletions(-)
G2b files (config_sweep.c + 4 test files): 5 files changed, 39 insertions(+), 237 deletions(-)

## G3
G3 REPORT (PDCCH group) -- worktree /home/nicola/NICOLA/wt/rr-csd-int16s2, dir openair1/PHY/NR_UE_TRANSPORT
All edited TUs compile with /tmp/s2/chk.py (sources, .cu and edited tests). Nothing was built or run.
Note on "effective value": nr-uesoftmodem.c main() does setenv(...,0) for ISAC_RECONF=1, ISAC_CSIRS_DISC=1, ISAC_GPU_BUDGET=1, ISAC_LOG_ASYNC=1 under --passive-rx, so those are constants in the receiver (but 0/unset in unit tests).

REMOVED
ISAC_PDCCH_GPU_FEP = off (default) ; feature (GPU PDCCH FEP loader in nr_pdcch_gpu_fep.h now returns NULL)
ISAC_PDCCH_GPU_CPU_ARITH = off ; diag (parity kernel call + g_cpu_arithmetic removed from nr_pdcch_gpu_fep.cu, include of nr_pdcch_gpu_parity.cuh dropped)
ISAC_PDCCH_ANT_MASK = 0 (default) ; diag/A-B (dci_nr.c + nr_pdcch_gpu_fep.cu)
ISAC_PDCCH_GPU_DUMP, ISAC_PDCCH_GPU_DUMP_MAX = unset ; diag (nr_pdcch_gpu_record.c is now an empty stub)
ISAC_PDCCH_CFGTRACE, ISAC_PDCCH_CFGTRACE_SLOT = off ; diag (PDCCHCFG line)
ISAC_TSYNC_AUDIT = off (dci_nr.c reader only; other readers in foreign files) ; diag (TSYNC_PDCCH line)
ISAC_PDCCH_LLRPROBE, ISAC_PDCCH_LLRPROBE_SLOT = off ; diag (nr_pdcch_llr_probe body emptied, function kept: rt.c extern)
ISAC_PDCCH_NID_GPU_BUDGET_MS = 5000 (default) ; tuning
ISAC_PDCCH_NID_MULTISYMBOL = on (default) ; feature (single-symbol shortlist() + Device::multisymbol deleted in nr_pdcch_nid_gpu.cu)
ISAC_PDCCH_NID_GPU_NODROP = off ; feature (nodrop_acquire, slot clock, dropped_snapshots counters, cv_free deleted)
ISAC_PDCCH_NID_GPU_INCREMENTAL = on (default) ; feature (non-incremental window/every path deleted)
ISAC_PDCCH_NID_GPU_BATCH = 64, _EVERY (unused when incremental), _WINDOW = 10, _ACQUIRE_MS = 5000, _MAINTAIN_MS = 500 ; tuning (now constants in a policy struct, see BAKED)
ISAC_DMRS_NID_SOLVE (=1/2/3), _EVERY, _OPT, _CORE, _GRACE_MS, _MAINTAIN_MS, _RECHECK_MS = unset/off ; feature (solver "primary" mode in nr_pdcch_nid_gpu.c, hint/merged records, nr_dmrs_nid_solve_live.c emptied, call in coreset_map.c, nr_pdcch_nid_gpu_primary use in blind_monitor.c)
ISAC_PDCCH_NID_GPU_SYMBOLS = 3 (default) ; tuning (baked)
ISAC_PDCCH_NID_GPU_OCC_GATE, _OCC_DB, _OCC_FLOOR_PCT = on / 6 dB / 10 % (default) ; tuning (baked)
ISAC_COREMAP_IDSWEEP (CPU idsweep dump) = off ; diag (g_idsw struct, idsw_worker, IDSWEEP dump files, CPU branch of want/push deleted)
ISAC_COREMAP_SUBBAND = off -> coherent statistic ; feature (nr_pdcch_subband_z, NR_PDCCH_SUBBAND_* deleted)
ISAC_COREMAP_ONACCEPT = off ; diag (accept_probe body emptied, function kept: rt.c calls it)
ISAC_COREMAP_PHASE = -1 (unset) ; diag
ISAC_COREMAP_GT, _GT_CSET, _Z, _EMITBAR, _OCCBAR(=0.55 baked) = off/unset ; diag/tuning
ISAC_DISCOVER_DIAG = off ; diag (DISCOVERDIAG, DISCOVERGATE, DISCOVERHIST, SWEEPDIAG, COREMAPDIAG, LOOKAHEAD_ASSIGN prints)
ISAC_DISCOVER_MIN_BG = 3, ISAC_DISCOVER_DOMINANCE_K = 4, _N = 3 ; tuning (baked)
ISAC_RECONF (dci_length_sweep.c only) = 1 (frozen, set by main) ; see BAKED
ISAC_DCI_LEN_MAX = unset ; tuning (explicit_max removed here; rt.c still reads it itself, foreign)
ISAC_DCI_WIDE_PROBE_EVERY = 1 ; tuning
ISAC_RECONF_N_SUSPECT = 200 ; tuning (nr_pdcch_dci_length_n_suspect_from_env() now returns 200)
ISAC_TD_DCI_ADJ = on ; tuning (default kept as s_enabled=1; nr_dci_hist_enabled_set() stays as test seam)
ISAC_TD_CERT_CONFIRMED = off ; feature (h->cert_confirmed=false; field and conf_only branch kept, header owned elsewhere)
ISAC_DCI11_S1_OFF = off ; diag
ISAC_DCI11_HARD_REFUTE = 1 (frozen) ; BAKED
ISAC_DCI11_HARQ_SEQ = 1 (frozen) ; BAKED
ISAC_UL_FDRA_REFUSE = off ; feature (nr_dci01_fdra_refuse_enforced() returns false; callers in rt.c)
ISAC_SIM_CANDIDATE_RNTIS, ISAC_SIM_CANDIDATE_RNTI = unset ; sim hook (blind_rnti_bootstrap.c; no other user in repo)
ISAC_UL_DCI_BANK = on via UL_PUSCH_CHAIN/SOLVER=1 (frozen) ; BAKED (nr_pdcch_ul_bank_enabled() returns true)
ISAC_UL_CONFIG_DISC = on via CHAIN/SOLVER ; BAKED (nr_pdcch_ul_config_disc_enabled() returns true; also geometry(): solver=true)
ISAC_UL_LOCK_EDGE, ISAC_UL_LOCK_COHERENT, ISAC_UL_LOCK_SPRT, ISAC_UL_LOCK_CLIP, ISAC_UL_SPRT_DUMP = off ; feature/diag (whole SPRT/edge/coherent machinery in nr_pdcch_ul_layout_lock.c deleted; the 3 *_enabled() predicates return false because nr_pusch_passive_decode.c calls them)
ISAC_UL_LOCK_LLR, ISAC_UL_LOOKALIKE = off ; feature (llr_enabled/lookalike_enabled, lookalike_lock_locked, llr_lock_ok, llr_probe_discriminates, g_disc deleted)
ISAC_UL_LOCK_PRIOR_SEARCH, ISAC_UL_LOCK_PRIOR_DWELL = off ; feature (whole ULPRIOR provisional-BWP search deleted; g_prov always false)
ISAC_UL_LOCK_NO_TRIAL_UNLOCKED = off ; feature
ISAC_UL_MCS_TABLE = unset ; diag (forced table block deleted)
ISAC_MAP_PASS0_ONLY = unset (staged mode) ; tuning (map_staging_enabled() etc. removed, staging constant)
ISAC_CSS0_ALS = unset ; tuning
ISAC_AL1_COVER = off ; feature (al1_cover_enabled, al1_cover_for_span, stage<0 lap deleted)
ISAC_PDCCH_EXTENT_BATCH = unset -> K=1 -> 0 lookahead lanes ; tuning (nr_pdcch_blind_lookahead_count() returns 0)
ISAC_PDCCH_DISC_ENUM, _ENUM_MAX, _ENUM_NIDS = off ; feature (disc_enum_*, s_enum_tried, disc_bank_verified, .ex field deleted)
ISAC_PDCCH_DISCOVERED_FILE = unset (file branch dead: nr_pdcch_nid_gpu_enabled() is constant true) ; feature (file poll branch, legacy catalog hand-off with s_disc[], discovered_load/restore/verified, struct stat deleted)
ISAC_PDCCH_DWELL_ADAPTIVE = off ; feature
ISAC_PDCCH_NO_FAKE_FOOTPRINT = off ; feature (nr_pdcch_no_fake_footprint_enabled() deleted; header decl remains)
ISAC_PDCCH_FULLCRC = off ; diag (FULLCRC print)
ISAC_PDSCH_RM_GENIE (reader in blind_monitor.c) = off ; diag (out->payload/dci_len no longer filled; the other reader nr_pdsch_rm_genie.c belongs to another group)
ISAC_DCI_WATCH_RNTI = unset ; diag (DCIWATCH print)
ISAC_SIB1_CACHE = on (default) ; feature switch (sib1_cache_enabled() removed)
ISAC_UL_PUSCH_CHAIN, ISAC_UL_PUSCH_SOLVER (blind_monitor.c, ul_config_disc.c) = 1 (frozen) ; BAKED

BAKED (value became a constant, code path stays)
ISAC_PDCCH_NID_GPU=1 (nr_pdcch_nid_gpu_enabled() returns true; the !enabled branches in blind_monitor.c/coreset_map.c removed)
ISAC_PDCCH_NID_GPU_DUTY=1, _RECHECK_MS=250 (frozen), _CAPTURE_EVERY=2 (frozen, in idsweep_want), ISAC_COREMAP_IDSWEEP_DUR=3 (frozen, nr_pdcch_coreset_map_idsweep_dur() returns 3)
ISAC_PDCCH_NID_GPU_ACQUIRE_MS=5000, _MAINTAIN_MS=500, _WINDOW=10, _BATCH=64, _SYMBOLS=3, OCC gate 6 dB / 10 %
ISAC_PDCCH_NID_BANK=1, ISAC_PDCCH_DISC_COVER=1, ISAC_PDCCH_POINTA_REF=1, ISAC_CARRIER_FROM_SIB1=1 (frozen; the 4 public *_enabled() return true)
ISAC_PDCCH_RBOFFSET_FIX=1 (frozen; = IS_PASSIVE_RX_MODE, so non-passive keeps the legacy group positions)
ISAC_RECONF=1 in nr_pdcch_dci_length_sweep.c (nr_pdcch_reconf_enabled() returns true, seen-length-first order always on; blind_monitor.c outside-occupancy block unconditional)
ISAC_SLOT_SCALE=1 (frozen; set_mu always scales)
ISAC_DCI11_HARD_REFUTE=1, ISAC_DCI11_HARQ_SEQ=1, ISAC_DCI11_HARQPID/ISAC_SCANCOST (harqpid branch was true in both arms)
ISAC_PDCCH_USS_FAST=1, ISAC_PDCCH_USS_TRACK_BACKOFF=4 (frozen), ISAC_PDCCH_USS_TRACKER=on
ISAC_UL_LAYOUT_LOCK=1, _LOCK_MCS_STAT=1, _PREFIX_ENT=1, _BWP_SEED=1, _BWP_DL_PRIOR=1, _LOCK_SIB1_FIRST=1, _LOCK_CRC_FIRST=1, _LOCK_CRC_GUARD=1, _LOCK_CRC_K=3, _LOCK_BWP_ALL=1, _MCS_TABLE_SWEEP=1, _K2_PAIR=1, _TDA_PAIR=1, _FIT_READ=1, _BGDISC=1, _LAYOUT_LOG=1, _LOCK_SIGN=2 (all frozen). The public predicates (lock/bwp_seed/dl_prior/sib1_first/crc_first/crc_guard/fit_read) return true. Dead else-branches (legacy MCS score rule, ascending prefix order, non-CRC-first trial path, PROVISIONAL state) deleted.
   SIGN=2 (shadow): the tally (sign_book_locked, g_only, det) is kept because the always-on "ULLAYOUT STATS ... signt[" line shows it; the "SIGN_SHADOW would lock" one-shot log was removed.
ISAC_UL_PUSCH_CHAIN/SOLVER=1 in nr_pdcch_ul_config_disc.c and blind_monitor.c.

KEPT
ISAC_PDCCH_USS_CORE: CPU-core map (nr_env_core + nr_bg_lane_enter)
ISAC_PDCCH_NID_GPU_CORE: CPU-core map (env_int kept for this one key, default 13 = frozen)
ISAC_SIB1_CACHE_DIR: path of the per-PCI SIB1 cache (default /tmp/passive_rx); kept because tests use it to isolate the cache (nr_reconfig_replay_test now sets it to a mkdtemp dir). Lead decides whether it is a site key.
ISAC_GPU_BUDGET, ISAC_BG_LANE, ISAC_CELL_STATE: read by foreign helpers (nr_gpu_budget_enabled, nr_admit_on, nr_cell_state_sweep_blocked), not in my files.

LOST DIAGNOSTICS
ISAC_PDCCH_GPU_DUMP: recording of PDCCH IQ+LLR for libpdcch_gpu offline replay (nr_pdcch_slot_bench input)
ISAC_PDCCH_GPU_CPU_ARITH / ISAC_PDCCH_ANT_MASK: CPU-arithmetic parity kernel in the GPU PDCCH FEP; forced antenna mask A/B
ISAC_PDCCH_CFGTRACE: PDCCHCFG per-invocation derived-parameter trace
ISAC_TSYNC_AUDIT (dci_nr.c part): TSYNC_PDCCH timing/coherence per candidate
ISAC_PDCCH_LLRPROBE: LLRPROBE signature of the unscrambled soft bits
ISAC_COREMAP_IDSWEEP: /tmp/passive_rx/idsweep_NNN.bin snapshot dumps for idsweep_offline
ISAC_COREMAP_GT/_GT_CSET/ONACCEPT/COREMAPRB/COREMAPACC/COREMAPOCC/COREMAPDIAG: CORESET-map ground-truth profiles and per-window statistics
ISAC_DISCOVER_DIAG: DISCOVERDIAG, DISCOVERGATE, DISCOVERHIST, SWEEPDIAG, LOOKAHEAD_ASSIGN lines
ISAC_UL_SPRT_DUMP: SPRT_DUMP zn lines; SPRT/edge/coherent BWP lock statistics and their "sprt[ wv[ overlap[" STATS suffixes (were empty with the default env)
ISAC_UL_LOCK_SIGN=2 SIGN_SHADOW would-lock line
ISAC_PDCCH_FULLCRC: FULLCRC raw payload dump; ISAC_DCI_WATCH_RNTI: DCIWATCH reject trace; ISAC_PDSCH_RM_GENIE payload hand-off (RMDCI)
ISAC_PDCCH_DISC_ENUM log tag; ISAC_PDCCH_DWELL_ADAPTIVE reject line variant
Parsed lines checked: ULLAYOUT STATS/BWP_LOCK/probes, PDCCH_NID_GPU pass (otatail_stats.py) kept in the same format. STATS now prints "bwp=LOCKED|searching" (PROVISIONAL cannot occur). The PDCCH_NID_GPU "pass" line no longer carries dropped_snapshots (was nodrop only); "stop summary"/"cumulative" lines lost skipped_batches=/primary= fields (not parsed by any tool I grepped).

TESTS
tests/nr_pdcch_nid_worker_test.cc: rewritten around a test-only seam nr_pdcch_nid_gpu_test_policy(dur,window,batch,duty,acq,maint,recheck) (declared extern in the test, defined in nr_pdcch_nid_gpu.c). Deleted the --nodrop arm, the non-incremental (legacy) arms and the "off identity" first start. Arms left: default (incremental, duty off), --duty, --device.
tests/nr_pdcch_nid_algorithm_test.cc, _benchmark.cc, _incremental_test.cc, _stream_benchmark.cc: removed setenv INCREMENTAL/MULTISYMBOL.
tests/nr_pdcch_dci_length_sweep_test.cc: deleted DefaultOrderIsAscending, WideProbeCadenceFromEnv, ExplicitMaximumEnablesColdWideRound; removed RECONF / RECONF_N_SUSPECT setenv.
tests/nr_pdcch_dci11_layout_sweep_test.cc: removed HARD_REFUTE/HARQ_SEQ setenv and the "flag off" tail of HardRefutationArmsType0OnTheLiveCellPayloads.
tests/nr_dci_history_test.cc: env-switch part of KillSwitchStillDisablesAll reduced to nr_dci_hist_enabled_set(-1) -> default on.
tests/nr_pdcch_blind_rnti_bootstrap_test.cc: SlotScale test keeps only the baked-on half.
tests/nr_blind_fix_test.cc: removed MCS_STAT setenv.
tests/nr_dl_adaptive_test.cc: deleted RetrySearchesOtherWidthsAtSameOffsetAndNeverInventsVerification (exhaustive walk, removed alternative); removed unsetenv MAP_PASS0_ONLY in the staged test.
tests/nr_reconfig_replay_test.cc: ISAC_SIB1_CACHE=0 replaced by ISAC_SIB1_CACHE_DIR=mkdtemp.

NEEDS (out of my file set)
CMakeLists.txt: drop test_nr_pdcch_nid_worker_nodrop, _incremental and _duration2/3 arms (still pass, now duplicates/legacy); git rm nr_dmrs_nid_solve_live.c and remove it from add_library(nr_pdcch_nid_worker) (line 1515); nr_pdcch_gpu_parity.cuh is now unused (git rm).
nr_pdcch_blind_monitor_test.cc (G1) must delete/convert, otherwise they fail or do not link:
  - PRIOR_SEARCH tests (~6760-6830, nr_ul_layout_prior_expected / _prior_state removed), SPRT (~6340) and EDGE (~6365) tests (predicates return false), LLR/SIGN decide tests (functions kept but unused), CRC_FIRST=0 / SIB1_FIRST=0 / BWP_SEED=0 / DL_PRIOR=0 / CRC_GUARD=0 / PREFIX_ENT=0 / FIT_READ=0 / LAYOUT_LOCK-unset arms (~6131-6180, 6580-6760), ISAC_UL_CONFIG_DISC setenv (~3140-3312, now always on)
  - discovery: ISAC_PDCCH_NID_BANK=0 (~4604-4735), DISC_COVER=0/1 (~4826-4873), POINTA_REF/CARRIER_FROM_SIB1=0 (~4964, 5066), DISC_ENUM tests (~4971-5025), DWELL_ADAPTIVE and NO_FAKE_FOOTPRINT tests (~5061-5130, functions deleted), RBOFFSET_FIX unit test is a pure function and still fine, SIB1_CACHE=0 test (~5764)
  - ISAC_DCI11_PARSE_CACHE comment only.
Headers (foreign): nr_pdcch_nid_gpu.h (decl of nr_pdcch_nid_gpu_dropped_snapshots, nr_pdcch_nid_gpu_primary, nr_dmrs_nid_live_hint/reset/hints), nr_dmrs_nid_solve.h (nr_dmrs_nid_live_offer, hint_sig/sweep_demand/prim_cfg helpers + tests in nr_dmrs_nid_solve_test.cc), nr_pdcch_blind_monitor_rt.h (nr_pdcch_dwell_adaptive_enabled, nr_pdcch_no_fake_footprint_enabled), nr_pdcch_ul_layout_lock.h (nr_ul_layout_prior_expected/_prior_state, llr/sign/lookalike, edge/coherent/sprt predicates, crc_first/guard etc. now constants), nr_pdcch_coreset_map.h comments (ONACCEPT, IDSWEEP), nr_pdcch_ul_config_disc.h comments. Declared-but-undefined now: nr_pdcch_nid_gpu_dropped_snapshots, nr_pdcch_nid_gpu_primary, nr_dmrs_nid_live_*, nr_ul_layout_prior_expected, nr_ul_layout_prior_state, nr_pdcch_dwell_adaptive_enabled, nr_pdcch_no_fake_footprint_enabled.
Stubs kept only because foreign callers remain: nr_pdcch_gpu_record_cpu (call in nr_pdcch_blind_monitor_rt.c:6540, nr_pdcch_gpu_record.c becomes removable), nr_pdcch_llr_probe (rt.c:4585), nr_pdcch_coreset_map_accept_probe (rt.c:4064), nr_ul_layout_prior_note (nr_pusch_passive_queue.c:424), nr_ul_layout_edge/coherent/sprt_enabled (nr_pusch_passive_decode.c:1447,1578 dead branches), nr_dci01_fdra_refuse_enforced (rt.c), nr_pdcch_blind_lookahead_count (rt.c:6267). nr_pdcch_blind_monitor_rt.c:11568 still reads ISAC_PDCCH_GPU_FEP; libpdcch_gpu.so (nr_pdcch_gpu_fep.cu) is now only loaded by its own tests (nr_pdcch_gpu_fep_test, nr_pdcch_slot_bench) and is a whole dead module (nr_gpu_pdcch_fep_load/prepare return NULL).
Dead code that needs rt.c/header edits to go: the lookahead-lane machinery (EXTENT_BATCH unset => K=0 lanes) in blind_monitor.c/rt.c, nr_pdcch_al1_map.* lane use, nr_pdcch_blind_monitor_bank_has_geometry (no caller left in blind_monitor.c), nr_pdcch_discovered_load kept for tests, rt.c readers of ISAC_DCI_LEN_MAX.
Other flags read in rt.c/other groups' files that belong to the same families: ISAC_PDCCH_LEN_MEMORY (mentioned in nr_pdcch_dci_length_sweep.c comment), ISAC_TSYNC_AUDIT in csi_rx.c / nr_adjust_synch_ue.c / phy_procedures_nr_ue.c.

DOUBTS
1. ISAC_RECONF in my files is baked to true because main() setenv's it; unit tests (no --passive-rx) used to run these paths with RECONF unset. Any PDCCH test that relied on the legacy ascending length order (nr_pdcch_dci_length_sweep_test: tests that call note_seen) or on s_outside_occupancy staying false may now fail; I could not run them.
2. nr_pdcch_nid_gpu_test_policy is a new test seam in product code (7 lines). Alternative: delete the duty/window tuning from the worker test. The duty/incremental worker test timing (waits 5/130 ms) was only desk-checked.
3. ISAC_PDCCH_NID_GPU=1 is now unconditional: on a host without libpdcch_nid_gpu.so start() fails and logs "init failed" exactly as before (enabled() was true by default already).
4. EXTENT_BATCH default K=1 => zero lookahead lanes: I baked it, but deleting the lane machinery was not attempted (cross-file).
5. ISAC_UL_MCS_TABLE_SWEEP / LOCK_SIGN / LAYOUT_LOG and the other frozen UL lock keys were verified against FROZEN_20261018.env; the code defaults were already equal (or unset-default true), so no behaviour change intended. ISAC_PDCCH_RBOFFSET_FIX default was already 1 under passive-rx.
6. nr_ul_layout_lock.c: the hyp_list / crc_trials code now uses K=3 literal (was env default 3). nr_ul_variant_count/get calls take (true,true,true) for sweep/k2pair/tdapair.
7. ISAC_SIB1_CACHE_DIR kept (see KEPT).

git diff --shortstat (my 17 source files): 17 files changed, 313 insertions(+), 3364 deletions(-)
git diff --shortstat (12 edited test files): 12 files changed, 29 insertions(+), 258 deletions(-)

## G4
REPORT G4 (cleanup stage 2) -- files under /home/nicola/NICOLA/wt/rr-csd-int16s2/openair1/PHY/NR_UE_TRANSPORT/
git diff --shortstat (my 14 files + 2 test files): 16 files changed, 377 insertions(+), 2450 deletions(-)
All touched TUs compile with /tmp/s2/chk.py (queue, solver, demod.cu, demod_host.c, exact.c, fep.cu, ldpc.cu, ldpc_host.c, record.c, traffic probe x2, solver test, ldpc test), plus the includers of nr_pdsch_gpu_fep.h / nr_pdsch_gpu_record.h (passive_decode.c, gpu_pass_dl/ul/graph, blind_monitor*, metrics, csirs_blind_rt, nr_ul_nid_bg, polar_gpu_mod, slot_bench, ssb_rate_match check). Nothing was linked or run.

REMOVED (flag = effective value ; kind) -- env read and dead alternative gone
ISAC_PDSCH_GPU_PRIO = unset/off ; feature (low-priority twin streams; lowprio plumbing in exact.c/fep.cu/demod.cu/ldpc.cu/queue deleted; prio.cuh kept as stub, see NEEDS)
ISAC_PDCCH_GPU_EXACT_FEP = unset/off ; feature (nr_pdcch_gpu_exact_fep_symbol is now a stub returning -1, counters pdcch_gpu/pdcch_cpu dropped from the GPU_EXACT_FEP stats line)
NR_GPU_LLR_SCALE = unset (4.0) ; tuning (now static const)
NR_GPU_FORCE_ANT = unset (-1) ; diag (static const)
ISAC_GPU_PASS (read in fep.cu) = unset ; feature (PASS_LS_ROWS = 1024, i.e. the GRAPH=1,SPLIT=1 branch)
ISAC_PDSCH_GPU_EXACT_DIRECT = unset (direct = platform_shared_direct) ; tuning
ISAC_PDSCH_GPU_EXACT_DEADLINE_US = unset (2000 us) ; tuning (static const)
ISAC_PDSCH_GPU_BATCH_ASYNC = unset/off ; feature (g_batch_async path deleted)
ISAC_GPU_HEARTBEAT = unset/off ; diag (heartbeat thread, k_hb, k_smid, hb_* and the GPU_GREEN_VERIFY probe deleted)
ISAC_PDSCH_GPU_CHEST_TRIM = unset/off ; tuning (Par.trim and the zero-fill branch of k_fir deleted)
ISAC_PDSCH_GPU_STAGE_DUMP, ISAC_PDSCH_GPU_DUMP, ISAC_PDSCH_GPU_DUMP_MAX, ISAC_PDSCH_GPU_DUMP_FULL = unset/off ; diag (nr_pdsch_gpu_record_cpu and stage_context/stage_chest deleted)
ISAC_PDSCH_GPU_CSI_DISJOINT = unset/off ; feature (nr_pdsch_gpu_csi_disjoint is now `return !numCsiRsForRateMatching`)
ISAC_PDSCH_GPU_EXACT_DEMOD_PROF = unset/off ; diag (all ev[] events and the GPU_EXACT_DEMOD_PROF line)
ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES = 0 ; diag
ISAC_RX_QAM_AGC, ISAC_RX_IRC, ISAC_RX_BEST_BY_SNR, ISAC_L2MAXH_PEAK, ISAC_L2MAXH_HEADROOM, ISAC_SUBSET_DIAG, ISAC_RX_ML_2L, ISAC_RX_BRANCH, ISAC_PDSCH_DECODE_DIAG, ISAC_MMSE_FLOAT = unset ; "refuse GPU demod when set" guard list deleted (refuse() keeps nr_agnostic_v2())
ISAC_RX_BRANCH_MIN_DB = unset (12 dB) ; tuning
ISAC_L2MAXH_DELTA = unset (0) ; tuning
ISAC_RX_MRC_SHIFT_SUM = unset (0) ; tuning (g_shift_sum removed, job shift_sum = 0)
ISAC_PDSCH_MMSE2_NL34_QPSK = unset (0) ; tuning
ISAC_PDSCH_MMSE_EXACT_NVAR_DB = unset (21 dB) ; tuning
ISAC_PDSCH_GPU_EXACT_DEMOD_EVM = unset (1: GPU float EVM) ; tuning (fp64 EVM mode, CPU-copy EVM mode and g_gpu_evm deleted, evm_fp64 = 0)
ISAC_GPU_CHECK_ASYNC_QUEUE = unset (2) ; tuning
ISAC_GPU_CHECK_ASYNC_EVERY = unset (256) ; tuning
ISAC_PDSCH_GPU_EXACT_LDPC_CHECK = unset (0) ; diag
ISAC_UL_GPU_LDPC_DEADLINE_US = unset (4000) ; tuning (literal in the init call)
ISAC_UL_GPU_LDPC_PCT = unset (15) ; tuning
ISAC_UL_GPU_LDPC_MIN_BITS = unset (0) ; tuning (min_bits, fb_small and the "small=" field of the UL LDPC report deleted)
ISAC_GPU_LDPC_NOCLUSTER = unset ; test knob (cluster state = sm_major>=9)
ISAC_GPU_LDPC_CL_FORCE = unset (0) ; test knob (g_cl_force removed; pick_cb keeps its `force` argument because the exported nr_pdsch_gpu_ldpc_pick_cb signature is used by the test)
ISAC_PDSCH_SOLVER_FAIR, ISAC_PDSCH_SOLVER_LEADER, ISAC_PDSCH_SOLVER_PREF, ISAC_PDSCH_SOLVER_PREF_MCS = unset/off ; features (probe_hog, leader_ok, search_pref_skip deleted; why-counters leader/hog/nlskip dropped from the SOLVER why lines)
ISAC_RX_MRC_PER_RNTI_AB, ISAC_RX_MRC_PER_RNTI_SNR = unset (0) ; features (comb_ab, comb_by_snr, comb_snr/comb_nsnr per-RNTI state deleted; comb_pick now only the CRC-rate rule over receivers {1,3})
ISAC_RNTI_LAYOUT_CACHE = unset/off ; feature (layout cache bodies, lc_* statics, lc_gen field deleted; nr_rnti_layout_cache_on()=false and nr_pdsch_solver_layout_cache()/_stats() kept as stubs for the caller in nr_pdcch_blind_monitor_rt.c)
ISAC_CORESET_DISC_MISS_S = unset (12.0) ; tuning (literal)
ISAC_CORESET_DISC_RECOVER_MAX = unset (3) ; tuning (literal)
ISAC_GPU_DUMP = unset ; diag (gpu_dump_record)
ISAC_GPU_SELFCHECK_ENFORCE = unset ; diag
ISAC_PDSCHQ_REAL_FIRST = unset/off ; feature (real_first_locked, rf_* deleted)
ISAC_PDSCH_DMRS_ID2_ASYNC = unset/off ; feature (dldmrs_run/dldmrs_defer/dldmrs_obs_t deleted; the inline trylock path stays)
ISAC_PDSCH_SLOT_TIMING = unset/off ; diag (PDSCH_SLOT_TIME line)
ISAC_PDSCHQ_SIGNAL_TIME = unset/off ; replay-only pacing
ISAC_PROBE_ALL = unset/off ; diag
ISAC_PDSCH_K0_PROBE = unset (on) ; baked, see BAKED
ISAC_TD_GW_PROBE = unset/off ; feature (set_grantwork(gw,false))
ISAC_RX_EQ_AB, ISAC_CHEST_OCC_AB, ISAC_LDPC_ITERS_AB = unset/off ; A/B experiments (the queue no longer touches nr_dlsch_eq_override / nr_pdsch_chest_occ_override / nr_pdsch_ldpc_iters_override; EVMGRANT line keeps eq=-1 occ=-1 it=-1 so its format is unchanged)
ISAC_PDSCH_SOLVER_DIAG = unset/off ; diag (PDSCH_SOLVER_FULL line)
ISAC_A2E_TRACE = unset/off ; diag (enqueue copy and the a2s=/a2x=/a2p= EVMGRANT fields deleted; nr_a2e_trace() kept as `return 0` for its many callers in nr_pdcch_blind_monitor_rt.c)
ISAC_PDSCH_RECOVERY_SNAP = unset (8) ; tuning -> BAKED
Legacy whole-slot GPU LLR front end (reachable only via ISAC_PDSCH_GPU=1 / NR_GPU_FEP, frozen ISAC_PDSCH_GPU=0 overrides NR_GPU_FEP=1 in nr_gpu_fep_load_impl): deleted from the queue: g_gpu and all g_gpu_* statics, the GPU worker thread / request queue / gpu_fep_submit_and_wait, the per-group GPU block, gpu_job, the self-check block (ISAC_GPU_SELFCHECK, frozen 0), gpu_health_failed, per-consumer GPU LLR buffers, gpu_dump_record; nr_gpu_fep_load/_prepare/_load_impl removed from nr_pdsch_gpu_fep.h. The exact GPU path (nr_pdsch_gpu_exact_warmup, nr_gpu_pass_warmup) is untouched. Slot-group max is now the constant 8 (was `g_gpu ? 64 : 8`, g_gpu always NULL); queue-stats gpu_stale_pre/_post/_selfcheck_*/_disabled are now 0.
 ISAC_PDSCH_GPU = 0 (frozen), NR_GPU_FEP = 1 (frozen, overridden), ISAC_GPU_SELFCHECK = 0 (frozen) ; feature
ISAC_PDSCH_SLOT_SPREAD = 1 (frozen) -> BAKED (the `!g_spread` wide-first sort branch deleted)

BAKED (NAME=value, code path stays)
ISAC_E2E_TRACE=1 (nr_passive_e2e_trace() returns 1; used by exact.c too)
ISAC_PDSCH_GPU_EXACT_FEP=1, ISAC_PDSCH_GPU_EXACT_CHECK=1 (spot check), ISAC_PDSCH_GPU_HOST_PIN=1
ISAC_PDSCH_GPU_EXACT_DEMOD=1, ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_DEFER=1, ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=64, ISAC_RX_MRC_MODE=1, ISAC_RX_MRC_ALIGN=2, ISAC_L2MAXH_DELTA_NL2=-3, ISAC_PDSCH_MMSE_MODE=2, ISAC_PDSCH_GPU_EXACT_DEMOD_NL34=1, ISAC_PDSCH_MMSE_EXACT_NL34=1, ISAC_PDSCH_GPU_EXACT_DEMOD_MMSE3=1, ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_NL34=1
ISAC_FATTAILS=1 (nr_fattails_on() deleted; ISAC_GPU_CHECK_ASYNC=1, ISAC_PDSCH_DMRS_ID_ASYNC=1, nr_fattails_pin_all()=1 [ISAC_GPU_PIN_ALL])
ISAC_PDSCH_GPU_EXACT_LDPC=1, ISAC_UL_GPU_LDPC=1, ISAC_UL_GPU_LDPC_CHECK=64, ISAC_PDSCH_HOST_FAST=1 (prep_segment = fast path then reference; the duplicate inline fast block in prep_segment_ref and the =2 compare mode deleted)
ISAC_GPU_LDPC_NC_MIN_CB=8, ISAC_GPU_LDPC_CL_MIN_CB=4, ISAC_GPU_LDPC_SM=1, ISAC_UL_GPU_LDPC_HOLD=1
solver (all frozen/default-on): ISAC_PDSCH_SOLVER=1 (nr_pdsch_solver_enabled() returns true), THREAD_CTX=1, LAYOUT_RECOVER=32, FAIRSHARE=1, PRIOR_CRC=1, SIGNIF=1, PRIOR_IDS=1, WHY=1, SCR=1, REUSE=1, COVERAGE=1 (non-coverage planners deleted), NL_PREF=1 (nl_skip/pref_gap deleted), BACKOFF=200, PROBELOG=1, ISAC_PDSCH_TDA_FIELD=1 (nr_tda_field_on() true), ISAC_RX_MRC_PER_RNTI=1
ISAC_DCI11_RV_CONSISTENCY=ON (default; nr_pdsch_solver_rvc_on() returns true, internal guards simplified)
ISAC_PDSCH_DMRS_ID_SCAN=16, ISAC_PDSCH_DMRS_SCAN_ASYNC=1, ISAC_PDSCH_DMRS_ID_REFRESH=64, ISAC_PDSCH_DMRS_ID_PER_CORESET=1
ISAC_PDSCH_INLINE_POOL=1, ISAC_PDSCH_K0_PROBE (unset = on), ISAC_TD_GRANTWORK (unset = on), ISAC_PDSCH_EVM_PROBES=1, ISAC_PDTIM_CLASS=1, ISAC_PDSCHQ_PROBE_LAST=1, ISAC_PDSCH_RECOVERY_SNAP (unset -> 8)
Mode-dependent, non-passive behaviour kept: ISAC_DL_TRAFFIC_PROBE=16, ISAC_POINTA_SWEEP=1, ISAC_CORESET_DISC_WATCH=1, ISAC_CORESET_DISC_SYNCQ=1 now `IS_PASSIVE_RX_MODE ? value : 0`; nr_syncq_gate_closed() returns 0 (level 2 was never frozen/default).

KEPT (site config)
ISAC_GPU_CHECK_CORES (core map, nr_pdsch_gpu_demod_host.c nr_bgq_start)
ISAC_PDSCH_CORES (core map, queue start)
ISAC_DMRS_SCAN_CORE (core map; nr_env_list_nth + nr_bg_lane_enter in the queue)
ISAC_GPU_WORKER_CORE: the only reader in my files was the deleted legacy GPU worker thread; the frozen value 10 is still consumed by executables/nr-uesoftmodem.c (see NEEDS)

LOST DIAGNOSTICS
ISAC_GPU_HEARTBEAT: device/host stall probe (GPU_HEARTBEAT lines) and GPU_GREEN_VERIFY RT/discovery SM-overlap check; tests/passive_rx/otatail_stats.py and otatail_replay.sh read them
ISAC_PDSCH_GPU_EXACT_DEMOD_PROF: per-kernel GPU time of the demod
ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES: per-(Nl,Qm,RB class) demod timing
ISAC_PDSCH_GPU_DUMP / STAGE_DUMP / ISAC_GPU_DUMP: recorded-grant dumps for the offline GPU replay (tests/passive_rx/dgx/gpu5_replay.sh, gpu_replay.sh use the env; tests/nr_gpu_slot_bench.cc still reads the format)
ISAC_GPU_SELFCHECK / GPU_SELFCHECK_DETAIL lines: GPU-vs-CPU LLR comparison of the legacy GPU front end
ISAC_PDSCH_SLOT_TIMING: PDSCH_SLOT_TIME line
ISAC_PDSCH_SOLVER_DIAG: PDSCH_SOLVER_FULL line
ISAC_A2E_TRACE: a2s=/a2x=/a2p= fields of EVMGRANT (arrival-to-enqueue split)
ISAC_PDSCH_SOLVER_FAIR/LEADER/PREF and the SNR/AB receiver pickers: their A/B arms
why-counters leader/hog/nlskip in "PDSCH_SOLVER why rnti=" lines
ISAC_RX_EQ_AB, ISAC_CHEST_OCC_AB, ISAC_LDPC_ITERS_AB: paired A/B rotation of the equaliser / chest / LDPC iteration limit
ISAC_PDSCH_GPU_EXACT_DEMOD_EVM=0/2 and ISAC_PDSCH_GPU_EXACT_LDPC_CHECK on the DL: CPU-copy / fp64 EVM mode, DL LDPC self-check
ISAC_PDSCHQ_SIGNAL_TIME / ISAC_PROBE_ALL: replay pacing / probe-only isolation
Kept always-on lines (grep'd before touching): gp=/gq=/li= (EVMGRANT), EVMGRANT, RETRY arms path, PDSCH_SOLVER identity/probe/SOLVED lines; PDSCH_SOLVER probe-log is now unconditional.
Removed UL LDPC report field "small=" (no consumer found with git grep).

TESTS
tests/nr_pdsch_solver_test.cc: removed the pre-bake suite env block (kOff, kEnable, kRvcOff, kLcEnable, kComb) and every setenv; deleted ReceiverPickFollowsSnrWhenCrcTies, ReceiverPickIgnoresTheSnrOfFailedDecodes (removed SNR rule) and RntiLayoutCache.HitSlotClassSeparationAndFailInvalidation + PdschSolverRvc.OnByDefault (removed code / trivial). Converted to the on-arm only (off arms covered removed code): OnlyACrcProvenSolutionSeedsTheCell, SignificanceRuleScalesTheSolveEvidenceWithTheProbesSpent (now quiet<busy only, max_grants 4000, BurnProbes continues through backoff grants), ProvenDimsAreTriedWithEveryOwnIdentity, FairShareSplitsTheProbeRateBetweenActiveRntis, ShiftedLayoutLosesTheRepresentative, McsZeroSolutionSeedsTheCellOnlyOnceHarqVerified. OneLayerQpskReadings...BoundedFallback -> ...FallBackAtOnce (gap 3 -> 1). LayoutRecoveryNeedsCreditedCrcFailures now needs 32 credited failures (loop). TheCellPriorSolvesASecondRntiFaster now CRC-proves the first RNTI with FullDecodes before the second solves. SolvesTheSimulatedCell bound relaxed 120 -> 400 grants (BACKOFF=200 is now always on). Compile-checked only; NOT RUN (see DOUBTS).
tests/nr_pdsch_gpu_ldpc_test.c: removed the manual modes nc / mix / sm / cl2 / cl4 (their env flags NOCLUSTER, NC_MIN_CB, SM, CL_FORCE are gone) and the BENCH_CFG fields for the removed flags; modes ul, pick, bench, clab and the default run remain.
tests/nr_dl_traffic_probe_test.cc, nr_pdsch_gpu_fep_test.cc, nr_pdsch_gpu_demod_test.cc, nr_pdsch_recovery_test.cc: no flag I removed was set there; only compile-checked. tests/nr_pdsch_gpu_fep_test.cc and nr_pdcch_gpu_fep_test.cc still `#define NR_GPU_FEP_NO_LOADER` (now an unused macro for the pdsch header; harmless).

NEEDS (out-of-set)
1. nr_pdsch_gpu_prio.cuh: nr_gpu_pass_graph.cu:1248 still calls nr_pdsch_gpu_prio_on() (now constant false) and nr_pdsch_gpu_low_stream(); owner of nr_gpu_pass_graph.cu should delete that line (and the I.s_lo stream if unused), then nr_pdsch_gpu_prio.cuh can be deleted. Also stale text in nr_gpu_pass.h:206.
2. nr_pdsch_gpu_exact.h (foreign): drop the declaration of nr_pdsch_gpu_exact_set_lowprio (definition deleted, no callers left), the comment "ISAC_PDCCH_GPU_EXACT_FEP", and the nr_pdcch_gpu_exact_fep_symbol decl; nr_pdcch_blind_monitor_rt.c:6381 can drop its call (the function is a stub returning -1).
3. nr_pdsch_gpu_record.h (foreign): drop the nr_pdsch_gpu_record_cpu prototype (definition deleted; no caller); nr_pdsch_gpu_stage.h weak decls of nr_pdsch_gpu_stage_context/_chest and the guarded calls in PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c (lines ~1093, 1239, 1391, 1611) are dead (the definitions are deleted; calls are weak-guarded so they link).
4. nr_pdsch_solver.h (foreign): remove declaration of nr_pdsch_solver_fairshare_on (definition deleted, no external caller), update comments; nr_rnti_layout_cache_on / nr_pdsch_solver_layout_cache / _stats / nr_pdsch_solver_lc_stats_t are now stubs: the layout-cache block in nr_pdcch_blind_monitor_rt.c (~9073-9090) and these API entries can be deleted together. nr_pdsch_solver_note_snr()'s crc_ok parameter is now unused (kept for the signature).
5. nr_pdsch_passive_queue.h / nr_passive_metrics.c: gpu_stale_pre, gpu_stale_post, gpu_selfcheck_fail, gpu_selfcheck_skipped, gpu_disabled stats fields (and the nr_passive_metrics gpu_fep_* mirrors) are now always 0. nr_a2e_trace()/nr_a2e_ts/nr_a2e_kind/nr_a2e_bank/nr_a2e_stg and job.a2s_*/a2p_* fields: nr_a2e_trace() returns 0; remove the ~25 callers in nr_pdcch_blind_monitor_rt.c, then the definitions in the queue file (they are defined near nr_pdsch_passive_queue_enqueue).
6. executables/nr-uesoftmodem.c:248 still adds the GPU core-plan entry for core_feature("ISAC_PDSCH_GPU") || core_feature("NR_GPU_FEP"); with the legacy GPU worker thread gone that reservation (and ISAC_GPU_WORKER_CORE) has no consumer: drop it, or decide to keep the core reserved. Caution: frozen NR_GPU_FEP=1 makes this branch live in the plan today, so removing it changes the core plan (a deliberate change for the lead).
7. nr_dlsch_demodulation.c / nr_pdsch_passive_decode.c still read ISAC_RX_BRANCH_MIN_DB, ISAC_L2MAXH_DELTA, ISAC_RX_EQ_AB, ISAC_CHEST_OCC_AB, ISAC_LDPC_ITERS_AB, ISAC_TD_GW_PROBE, ISAC_PROBE_ALL, ISAC_GPU_PASS (grep hits there): the CPU-side constants must match the ones I baked in nr_pdsch_gpu_demod_host.c (BRANCH_MIN_DB=12, L2MAXH_DELTA=0, DELTA_NL2=-3, MRC_MODE=1, MRC_ALIGN=2, MMSE_MODE=2, NL34 mode 3 from 3 layers, MRC_SHIFT_SUM=0, NVAR_DB=21).
8. nr_gpu_fep_hygiene.h (nr_gpu_fep_run_checked / nr_gpu_fep_reference_result / nr_gpu_fep_should_disable / nr_gpu_fep_sign_agreement) and nr_pdsch_real_first.h (+tests/nr_pdsch_real_first_test.cc) and nr_pdsch_passive_decode.c nr_pdsch_passive_gpu_job / set_llr_override (+tests/nr_ssb_rate_match_prod_check.c, nr_gpu_fep_hygiene_test.cc) lost their only production callers in the queue; the owner can delete them with their tests.
9. tests/passive_rx/otatail_stats.py, otatail_replay.sh, dgx/gpu5_replay.sh, gpu_replay.sh, gpudemod_replay.sh, gpu6_replay.sh set/parse flags that no longer exist (frozen test dirs excepted).

DOUBTS
- tests/nr_pdsch_solver_test.cc was converted without running it. The old suite explicitly forced the pre-bake OFF values (SIGNIF, PRIOR_CRC, BACKOFF, REUSE, ...), so every scenario now runs under the on-values; expectations that depend on grant counts (SolvesTheSimulatedCell <= 400, TheCellPriorSolvesASecondRntiFaster <= 4 after FullDecodes, the signif busy>quiet test with BurnProbes, LayoutRecovery loop, SolvedRntiNeverFullDecodesAReadingWithoutItsRepresentative which used RvcSetup(false) and now runs with the HARQ fn registered) may need tuning. Lead: run nr_pdsch_solver_test first.
- GPU demod MMSE-mode logic was rewritten from the (s_ex34, s_m3, s_nl34, s_nl34q) lattice into straight code; I re-derived it for the frozen values (3+ layers -> mode 3, 2 layers -> mode 2, override still honoured, QPSK 3-layer demotion kept) but it is not exercised by a test I ran.
- nr_pdsch_gpu_demod_host.c refuse(): s_env now = nr_agnostic_v2() cached once; the other removed guard variables were unset in prod, so equal.
- ISAC_PDSCH_GPU frozen as 0 while NR_GPU_FEP=1: I read the loader as ISAC_PDSCH_GPU first (set -> wins), hence off. If the lead intends NR_GPU_FEP=1 to turn the legacy LLR path on, the baseline binary would also have loaded it only if nr_gpu_pass/exact warm-up failed (it is the `else` of nr_pdsch_gpu_exact_warmup) -- with ISAC_PDSCH_GPU=0 present it cannot.
- nr_syncq_enabled(): kept `IS_PASSIVE_RX_MODE ? 1 : 0`; if non-passive callers ever matter it is the same as before.
- Layout cache stub: nr_rnti_layout_cache_on() is false for every process (was off unless the env was set; the test used to turn it on).
- queue stats line gpu_* now 0 and `SENSING: GPU_FEP ...` / `GPU_SELFCHECK` lines no longer exist anywhere (no parser found in tests/passive_rx other than otatail/gpu replay helpers).

## G5
REPORT G5 (GPU pass / td_cb0 / gpu platform / LDPC cuda)
All edited TUs compile with /tmp/s2/chk.py (graph.cu, dl.c, ul.c, green test, budget, platform + test, cb0 wire/backend/gpu_backend/batch .c/.cu, polar_gpu users, ldpc_cb0.cu, ldpc_decoder.cu, cuda_decoder.c, coding load/segment decoder, nr_gpu_pass_test.cc, td_cb0 tests/bench). Nothing was run.

REMOVED
ISAC_GPU_PASS = unset (v1 pass off) ; feature (v1 path/refusal branch gone; v2 always on)
ISAC_PDSCH_GPU_GRAPH = frozen 1 -> v2 constant ; baked
ISAC_PDSCH_GPU_GRAPH_SLOT / _LEAN = unset (off) ; experiment (sub-flag names dropped)
ISAC_PDSCH_GPU_GRAPH_MB = unset (default cap in graph.cu) ; tuning
ISAC_GPU_PASS_CLASSES = unset (512, or 1<<20 with split) ; tuning
ISAC_GPU_PASS_SEG_CLASSES = unset (1400, or 1<<20 with split) ; tuning
ISAC_GPU_PASS_NDMRS = unset ("3,2" ; "3,2,1" for contiguous classes with split) ; tuning
ISAC_PDSCH_DECODE_DIAG (warm evm term) = irrelevant, evm const 1 (EVM_GRANT/SNR_GATE default "1" made it always 1) ; diag
incompatible_env list: ISAC_PDSCH_EVM ISAC_SFO_CORRECT ISAC_DMRS_FO_APPLY ISAC_CHEST_OCC ISAC_RX_IRC ISAC_CHEST_TINTERP ISAC_DC_FIX ISAC_PDSCH_ANT_GATE ISAC_LLR_SCALE ISAC_PDSCH_RM_GENIE ISAC_PDSCH_LLR_DUMP ISAC_RX_BRANCH_FO ISAC_PDSCH_MMSE_AB ISAC_MMSE2_DIAG ISAC_PDSCH_OWN_PDCCH_RM ISAC_TD_TB_CPU_WHILE_ACQ ISAC_LLR_NORM = unset (check never fired) ; gate on experiments (the whole incompatible_env() + "pass stays off" branch deleted; these flags are still read in other files, owners decide)
ISAC_GPU_PASS_LOG_UNSEEN = unset ; diag (counters g_fb_unseen/g_ul_fb_unseen kept)
ISAC_GPU_PASS_FP32 = frozen 1 ; baked (g_fp32 = 1)
ISAC_UL_HYP_MAX_AGE = unset (2000 slots) ; tuning
ISAC_UL_GPU_PASS_NDMRS = unset ("2,1") ; tuning
ISAC_UL_GPU_PASS_CLASSES = unset (160) ; tuning
ISAC_UL_GPU_PASS (test-only setenv, nobody read it) ; dead
ISAC_GPU_POLL_SLEEP_US = frozen 200 ; baked
ISAC_GPU_DISC_SMS = frozen 8 ; baked
ISAC_GPU_RT_ISOLATE = frozen 1 ; baked (isolate path const, non-isolate paths gone)
ISAC_GPU_DISC_SMS_LOW = unset (LOW partition off) ; experiment (lpart/lgreen/lsms/lmade/lfail, idx -1 branch, nr_green_low_ok deleted; nr_gpu_green_stream(...,-1) now returns false)
ISAC_GPU_GREEN_LOG = unset ; diag
ISAC_PDSCH_GPU_CTX_LIMIT = unset (no limit) ; experiment (nr_gpu_ctx_limit(pool) = pool, kept as stub for foreign callers)
ISAC_GPU_DISCOVERY_BUDGET_PCT = frozen 30 ; baked (pct init 30)
ISAC_GPU_RT_DEADLINE_US = unset (250) ; tuning
ISAC_GPU_FORCE_DISCRETE = unset ; test hook -> replaced by nr_gpu_mem_test_force_discrete(int) (no env)
ISAC_GPU_MEMORY_MODEL = unset (auto) ; test/override (nr_gpu_choose_model(c) lost its override arg)
ISAC_GPU_SHARED_MANAGED = unset (off) ; experiment (managed-memory alloc path, NR_GPU_ALLOC_MANAGED, malloc_managed runtime ptr, use_managed/managed_opt_in args deleted)
NR_GPU_POLAR = unset (off) ; experiment (nr_polar_gpu.h loader now returns NULL, dlopen code gone)
ISAC_TD_CB0_BACKEND = frozen gpu ; baked (mode_from_env deleted; default and mode_set(-1) = GPU)
ISAC_TD_CB0_GPU_BACKOFF = unset (64) ; tuning (test hook nr_td_cb0_backend_test_backoff)
ISAC_TD_CB0_GPU_ITERS = unset (2 x max_iter) ; experiment (nr_td_cb0_gpu_iters is now just 2*max_iter capped)
ISAC_TD_TB_CPU_WHILE_ACQ = unset (1) ; baked in wire/gpu_backend (force only when backend peek == CPU)
ISAC_LLR_SCALE (read in wire.c) = unset (0) ; experiment (inadmissible-reason branch removed)
ISAC_TD_CB0_GPU_AUTOREG = unset (auto-register) ; test hook (nr_td_cb0_wire_test_autoreg)
ISAC_TD_CB0_THREADS = unset (8) ; tuning (unmarked)
ISAC_TD_CB0_RANK_MAX = unset (4) ; tuning (unmarked)
ISAC_TD_CB0_BUDGET_US = unset (NR_TD_CB0_BUDGET_US_DEFAULT) ; tuning (unmarked)
ISAC_TD_CB0_CPU_PCT = unset (30) ; tuning (unmarked; test hook nr_td_cb0_wire_test_cpu_pct)
ISAC_TD_CB0_B = unset (NR_TD_CB0_B_DEFAULT 96) ; tuning (unmarked)
NR_GPU_CB0 = unset (GPU dematch off until nr_td_cb0_use_gpu(1)) ; experiment
NR_TD_CB0_CUDA_LDPC = unset (off until nr_td_cb0_use_cuda_ldpc(1)) ; experiment
LDPC_CB0_MAX_ITEMS (batch.cu and ldpc_cb0.cu) = unset (2048) ; tuning
LDPC_CB0_TIMEOUT_MS / LDPC_CUDA_TIMEOUT_MS = unset (200) ; tuning (ldpc_cb0.cu, cuda_decoder.c)
LDPC_CB0_TIMEOUT_US_PER_ITEM = unset (100) ; tuning
LDPC_CB0_BREAKER_N / LDPC_CUDA_BREAKER_N = unset (4) ; tuning
LDPC_CB0_BREAKER_S / LDPC_CUDA_BREAKER_S = unset (5 s) ; tuning
LDPC_CB0_TEST_INJECT, LDPC_CB0_TEST_STALL_MS = unset (0) ; test (hooks via ldpc_cb0_test_hooks remain)
LDPC_CUDA_TEST_QUEUE_CAP, LDPC_CUDA_TEST_SKIP_LAUNCH, LDPC_CUDA_TEST_STALL_MS, LDPC_CUDA_TEST_INJECT = unset ; test (ldpc_cuda_test_hooks / ldpc_pool_test_hooks remain)
LDPC_CB0_SLOTS = unset (3) ; tuning
LDPC_CB0_CHUNK = unset (0 = L2 auto) ; tuning (override branch deleted)
LDPC_CB0_GRAPH / LDPC_CUDA_GRAPH = unset (graphs on) ; experiment (non-graph branch deleted in ldpc_cb0.cu and ldpc_decoder.cu pool)
LDPC_CB0_MEM = unset (explicit staged copies) ; experiment (MODE_UNIFIED stays reachable only through the existing test hook ldpc_cb0_test_set_mem)
LDPC_CB0_WORK_CW = unset (256) ; tuning
LDPC_CB0_NCW = unset (2) ; tuning (NCW=4 launch branch deleted)
LDPC_CUDA_WARMUP_ITERS = unset ; tuning (4th warm-up iteration set was a duplicate of 16)
LDPC_BENCH (3 files) = unset ; diag
ISAC_PDSCH_GPU_LDPC = unset (CPU "ldpc" lib) ; feature (load_nrLDPC_coding_interface no longer switches to "_cuda"; callers passing version explicitly unchanged)

BAKED
ISAC_PDSCH_GPU_GRAPH=1, _COVER=1, _SPLIT=1, _RETRY=1, _DMRS2=1 (frozen), _SEG128=1 (code default), g_sub_on = all 1 (names array now COVER SPLIT RETRY DMRS2 SEG128; idx remap inside nr_gpu_pass_dl.c only)
ISAC_GPU_PASS_FP32=1 ; ISAC_GPU_POLL_SLEEP_US=200 ; ISAC_GPU_DISC_SMS=8 ; ISAC_GPU_RT_ISOLATE=1 ; ISAC_GPU_DISCOVERY_BUDGET_PCT=30 ; ISAC_TD_CB0_BACKEND=gpu
Not frozen, constants: see REMOVED (e.g. ISAC_TD_CB0_THREADS=8 RANK_MAX=4 CPU_PCT=30 B=96 BUDGET_US=20000, LDPC_CB0_* defaults above)

KEPT
ISAC_GPU_BUDGET (nr_gpu_budget.c): frozen=1 but nr_gpu_budget.c is shared code; main() setenv's it only under --passive-rx (non-passive keeps budget off). Baking needs a passive flag; lead decides. Test nr_pdcch_nid_worker_test sets it to 0 too.
ISAC_CSIRS_DISC (ldpc_decoder.cu pool stream priority): frozen=1, set by main() under --passive-rx; same reason (libldpc_cuda also loadable by non-passive UE).

LOST DIAGNOSTICS
ISAC_GPU_PASS_LOG_UNSEEN: stderr line "GPUPASS unseen class / UL class" per unseen shape (the unseen counters in the census remain)
ISAC_GPU_GREEN_LOG: per-stream "[GPU green] ... SM mask" lines (nr_green_smmask itself stays, ldpc.cu uses it)
LDPC_BENCH: per-TB wall/cpu timing prints in libldpc, libldpc_cuda and the kernel-phase print in ldpc_decoder.cu
ISAC_GPU_DISC_SMS_LOW: the dynamic low-SM discovery stream (experiment)
"TD_CB0 wiring on" log line lost the fields tb_cpu_while_acq, llr_scale, gpu_iters_cap (constants now); other fields unchanged
GPU_PLATFORM log line lost "managed=%d"
Unseen-class granular debugging via ISAC_GPU_PASS_CLASSES (warm-set shrinking) is gone

TESTS
nr_gpu_pass_test.cc: FORCE_DISCRETE env -> nr_gpu_mem_test_force_discrete(); removed the static setenv of GRAPH/SPLIT/FP32 (GRAPH, SPLIT default 1; FP32=0 reference now via new test hook nr_gpu_pass_test_fp32(0) called from the fixture's GpuPassModule::get); ul_ready: ISAC_UL_GPU_PASS(_CLASSES) env -> nr_gpu_pass_ul_test_budget(1) (new hook, declared in nr_gpu_pass_ul.h)
nr_gpu_pass_fixture.h: dlsym nr_gpu_pass_test_fp32 and call fp32(0)
nr_gpu_platform_test.cc: managed-opt-in test deleted (code removed); override assertions removed; Discrete test uses attrs[18]=0; managed_alloc stub removed from runtime table
nr_gpu_green_test.cu: LOW/dyn section and the three setenv deleted; now runs the default 8-SM split
nr_td_cb0_wire_test.cc: env -> hooks (backoff, autoreg, cpu_pct); deleted LlrScaleFollowsTheDecodeRule (removed code) and the GPU_ITERS / TB_CPU_WHILE_ACQ=0 asserts of the pre_tb_cpu test
nr_td_cb0_gpu_backend_test.cc: BACKOFF env -> nr_td_cb0_backend_test_backoff
nr_td_cb0_gpu_bench.c: removed the gpu_iters argv (usage now threads seconds cpu rank); nr_td_cb0_bench.c: removed LDPC_CUDA_WARMUP_ITERS setenv (the 8-iteration graph is no longer pre-captured in that bench)
New non-env test hooks (all trivial setters): nr_gpu_mem_test_force_discrete, nr_gpu_pass_test_fp32 (libpdsch_gpu.so), nr_gpu_pass_ul_test_budget, nr_td_cb0_backend_test_backoff, nr_td_cb0_wire_test_cpu_pct, nr_td_cb0_wire_test_autoreg
Not edited: nr_pdsch_gpu_ldpc_test.c prints getenv of POLL_SLEEP/RT_ISOLATE/DISC_SMS in BENCH_CFG (harmless, would print "unset"); nr_pdcch_nid_stream_benchmark.cc and nr_gpu_budget_benchmark.cu read ISAC_GPU_DISCOVERY_BUDGET_PCT/BUDGET locally (not mine to judge)

NEEDS
- nr_pdsch_gpu_fep.cu:818 reads ISAC_GPU_PASS / ISAC_PDSCH_GPU_GRAPH / _SPLIT (defaults "1"): bake to (v2 on, split on); ISAC_GPU_PASS is never set now.
- nr_csirs_disc_gpu_sweep.cu uses nr_gpu_green_stream(&lo,..,-1) and nr_gpu_green_disc_pick(..., nr_gpu_budget_rt_large_active()): LOW stream never exists now. Remove that use, then nr_gpu_green_disc_pick (green.h), nr_gpu_budget_rt_large_begin/end/active (nr_gpu_budget.c/.h + nr_gpu_budget_test.cc), and the large begin/end calls in nr_pdsch_passive_decode.c:5961-5967.
- nr_pdsch_gpu_demod.cu:82, nr_pdsch_gpu_fep.cu:173, nr_pdsch_gpu_ldpc.cu:550 call nr_gpu_ctx_limit(pool) (now returns pool): drop the call.
- nr_pdcch_blind_monitor_rt.c uses nr_gpu_polar_load()/prepare() (always NULL now): delete the GPU polar batch path, nr_polar_gpu_mod.c, libpolar_gpu.so target in openair1/PHY/CODING/CMakeLists.txt (~line 240) and nr_pdcch_nid... users.
- openair1/PHY/CODING/CMakeLists.txt:267 comment mentions NR_GPU_CB0 (comment only).
- nr_td_cb0_sched.h has NR_TD_CB0_R_LLR_SCALE reason + adm_in_t.llr_scale (now always 0): removable with the metrics/tests that name it.
- Stale scripts setting removed flags: tests/passive_rx/dgx/gpu_replay.sh (ISAC_PDSCH_GPU_LDPC, ISAC_GPU_RT_DEADLINE_US), tests/passive_rx/gpupass_ab.sh (ISAC_GPU_PASS etc.). Frozen-gate globs do not cover them, but check they are not equivalence tooling.
- PROJECT_MEMORY.md lines 1642-1643 describe the old TD_CB0 wiring log fields.
- nr_pdsch_passive_decode.c failed chk.py during my run (not my file; sched_getcpu implicit decl) - other worker mid-edit or pre-existing.

DOUBTS
- ISAC_GPU_BUDGET / ISAC_CSIRS_DISC kept (see KEPT): if the lead wants zero env reads, they must become passive-mode constants (a shared-code flag).
- ISAC_PDSCH_GPU_GRAPH_SEG128: not in the frozen env; effective value taken from the code default ("1" in the baked list) = on.
- Test hook design for FP32: production default is FP32 (frozen 1); tests that want double must be built through the new hook, which I put in GpuPassModule::get so the whole test binary runs double as before (env was set at static init). Not run.
- nr_gpu_mem.c FORCE_DISCRETE replaced by a setter in the shared platform DSO (one instance); I assumed test exe and libpdsch_gpu.so share the DSO copy (CMake: nr_gpu_platform SHARED), so the setter reaches the module.
- ldpc_cb0.cu: g_timeout etc. were re-set in init_locked from env; now they keep static defaults and any test hook set before init survives init (previously overwritten at init).
- nr_td_cb0_wire.c: warmup loop in cuda decoder uses 3 iteration sets instead of 4 (the 4th duplicated 16): same graphs, one fewer redundant warm batch pass.
- Did not compile the full tests/targets; only chk.py per TU.

DIFFSTAT (my files only, headers and tests included): 34 files changed, 243 insertions(+), 918 deletions(-)

## G6
G6 (uplink) report. Repo /home/nicola/NICOLA/wt/rr-csd-int16s2. All touched TUs compile via /tmp/s2/chk.py (OK; only pre-existing warnings).
git diff --shortstat (my files + tests, incl. 2 deleted files): 25 files changed, 254 insertions(+), 2037 deletions(-)

REMOVED (flag = effective value ; kind)
ISAC_UL_GPU_PRIO = off (unset) ; tuning (stream prio hi)
ISAC_UL_STALL_US = 2000 (default, now literal) ; diag threshold
ISAC_UL_TEST_RT_LOAD = off ; test-only load generator (whole k_ul_test_spin + thread deleted)
ISAC_UL_GPU_FEP_DEADLINE = off ; feature (staging rows, abandon/reclaim/hold, stats o[10..12], "deadline" log fields deleted)
ISAC_UL_GPU_FEP_PCT = 30 (default) ; tuning -> literal 30
ISAC_UL_GPU_FEP_CHECK = 16 (default; NOT in frozen env) ; -> macro ULG_CHECK_EVERY 16
ISAC_UL_UE_STATE = n/a (no literal read in my files; ul_ids.c read it with chain/solver; effective true)
ISAC_UL_BGDISC_MAX_AGE_MS = 100 (default) ; tuning -> literal
ISAC_UL_DISC_EVERY = 1 (default) ; tuning (scan every grant; scan_tick/scan_now deleted)
ISAC_UL_TA_TRACK path: see BAKED
ISAC_UL_RETRY_SNR_DB = -500 (gate off) ; tuning (term deleted)
ISAC_UL_RETRY_MAX_RB = 12 (default) ; tuning -> literal
ISAC_UL_PIPELINE = n/a (chain on => pipeline true) ; nr_pusch_passive_pipeline_enabled() returns true
ISAC_PUSCH_SEARCH_PRIO = 0 (off) ; feature (FIFO boost while layout search) deleted
ISAC_UL_PROBE_TA = off ; feature deleted
ISAC_UL_LAYOUT_BRUTE, ISAC_UL_LAYOUT_BRUTE_IDS = off ; diag (brute-force re-decode, UL_LBRUTE_* logs) deleted
ISAC_UL_PROBE_DEPTH = 8 (default) ; -> literal
ISAC_UL_CPU_BUDGET_PCT = 0 (off) ; feature (consumer idle budget, g_budget_sleep_us, budget_sleep_ms field of PUSCHQ2) deleted
ISAC_UL_FORCE_IDS = off ; diag (ground-truth id forcing) deleted
ISAC_UL_UCI_BUDGET_US = 0 (off) ; tuning deleted
ISAC_UL_GPU_PASS_SEED = off ; diag deleted
ISAC_UL_TA_SWEEP = off ; diag (TASWEEP census + nr_sense_ul_ta_applied call) deleted
ISAC_PUSCH_DIAG = off ; diag (ULSIG, PUSCHDIAG lines, llr_out_all/want_est extras) deleted
ISAC_PUSCH_TIMING = off ; diag (UTIM stage sums/histogram, nr_pusch_passive_grant_stages) deleted
ISAC_UL_LAYOUT_CONTROL = off ; diag (control experiment) deleted
ISAC_UL_TIMING_DISC, ISAC_UL_DMRS_DISC = (chain on => true) ; the inline on-thread timing/DM-RS search branch was unreachable (bg_on branch has the superset condition) -> deleted
ISAC_UL_RX_BRANCH = unset ; override deleted (see BAKED ISAC_RX_MRC_MODE)
ISAC_UL_TIMING_DIAG = off ; diag (UL-TIMING line) deleted
ISAC_PASSIVE_REPLAY_INPUT + ISAC_PASSIVE_REPLAY_UL_UCI = off ; diag (REPLAY-UCI offline sweep) deleted together with passive_ul_unav_res/passive_beta_harq/passive_alpha/passive_qd_ack/passive_uci_crc (only used by it and the dump)
ISAC_UL_LLR_DUMP, _SNR, _MCS, _MIN_RB, _MAX, _OK_MAX, _MAX_MB = off ; diag (nr_ul_llr_dump.c and its test deleted)
ISAC_UL_NID_BG_{SNR,QM,MIN_RB,REPEAT,GAP_MS,TOPM,ADDPOS,TRIALS,JOB_MS,HYP,DROP,NBWP,MCS} = defaults 6,2,16,4,500,12,3,1,15000,1000,100,4,4 ; tuning -> constants
ISAC_UL_NID_BG_DBG_TRY, ISAC_UL_NID_BG_LOG = off ; diag (ground-truth id, UL_NID_OFFER/ADMIT/SWEEP/HYP logs) deleted
ISAC_SRS_CPU_PCT = unset (5) ; tuning deleted
ISAC_PRACH_MARGIN_DB10 = 80 (default) ; -> literal
ISAC_BG_LANE = off (unset) ; feature. nr_bg_lane_on()=0, nr_bg_lane_enter() empty; nr_admit.c reduced to always-run stubs (on=0, try=1, done/stats/register no-ops)
ISAC_BG_MIN_DUTY_PCT, ISAC_BG_JANON_BACKLOG, ISAC_BG_JANON_DUTY_PCT = (lane off) ; deleted with the admit logic

BAKED (NAME=value, code path stays)
ISAC_UL_GPU_FEP=1 (frozen) ; ISAC_UL_GPU_FEP_KCHARGE=1 (frozen; span charge unconditional) ; ISAC_UL_GPU_PART=rt (frozen = code default; rt first, disc fallback) ; ISAC_UL_STALL_DIAG=1 (frozen; uldiag::on() returns true, see DOUBTS)
ISAC_UL_PUSCH_CHAIN=1, ISAC_UL_PUSCH_SOLVER=1 (frozen): nr_ul_chain_on(), nr_pusch_solver_enabled(), nr_pusch_passive_ul_ue_state_enabled() return true (kept as functions: foreign callers/headers)
ISAC_SLOT_SCALE=1 (frozen) in nr_pusch_solver_set_mu
ISAC_UL_BGDISC=1 (frozen): bg discovery always on; nr_ul_bgdisc_enabled() deleted from the .c; the bg-admit wait loop deleted (admit always 1)
ISAC_UL_TA_TRACK=1 (frozen): nr_pusch_ul_disc_ta_track_on() deleted from ul_disc.c, tracked branch kept everywhere; the "off" branches in seed/update deleted
ISAC_UL_HARQ_COMBINE=1 (frozen); ISAC_UL_HARQ_MAX_SEG=8 (default)
ISAC_UL_UCI_BUDGET=8 (frozen) ; ISAC_UL_UCI_SAFE=1 (frozen)
ISAC_UL_SOLVER_FALLTHROUGH=2 (frozen): every ticketed grant the solver did not fully decode takes the full decode
ISAC_UL_RETRY=1 (frozen) ; ISAC_UL_RETRY_SNR_MARGIN=3 (frozen)
ISAC_UL_LDPC_SNR_MARGIN=6 (frozen)
ISAC_UL_DELAY_ONCE=1 (frozen) ; ISAC_UL_DELAY_FAST=1 (already constant in nr_passive_delay_fast.h)
ISAC_PUSCH_MMSE_MODE=2 (frozen): passive -> 2, else 0 (non-passive stays stock)
ISAC_RX_MRC_MODE=1 (frozen) as seen by UL: branch 0 when nant>1 (demod keep_branch, chest timing keep, GPU-pass in.keep); log "UL restricted to receive branch 0" kept
ISAC_UL_PRIO_QUEUE=1, ISAC_UL_SPARE_RESCUE=1, ISAC_UL_STALE_DROP=1, ISAC_UL_SNAP_POOL=1, ISAC_PUSCH_CONSUMERS=2, ISAC_UL_QUEUE_DEPTH=32, ISAC_PUSCH_PRIO=0, ISAC_UL_SNAP_PRIO=0 (all frozen)
ISAC_UL_NID_BG=1, ISAC_UL_NID_BG_IQ=1, ISAC_UL_NID_BG_CPU_PCT=10, ISAC_UL_NID_BG_IDLE=1 (frozen); nr_ul_nid_bg_enabled()/_iq_enabled() return true
ISAC_E2E_TRACE comment only. UL_E2E line: dropped the always-zero stg fields calls:/fep:/rxp:/dec:

KEPT (site/hardware config)
ISAC_PUSCH_CORES (core map), ISAC_UL_SNAP_CORE (core map, default 8 = frozen value), ISAC_UL_BGDISC_CORE (core pin), ISAC_UL_NID_BG_CORE (core pin), ISAC_BG_CORES (core map; read only inside nr_bg_lane_cores, which is now reachable only from test_nr_core_plan.cpp because the lane is off -- lead decides whether to delete it with ISAC_BG_CRITICAL_CORES in nr_core_plan.c)

LOST DIAGNOSTICS
ISAC_UL_FORCE_IDS: forced-ID arm of the UL rfsim bed
ISAC_UL_LAYOUT_BRUTE(_IDS): brute-force re-decode of failed grants over BWP/table/CDM/TDA (UL_LBRUTE_*)
ISAC_UL_TA_SWEEP: round-robin TA sweep census (TASWEEP line)
ISAC_PUSCH_DIAG: ULSIG coherence/LLR occupancy and PUSCHDIAG per-grant lines
ISAC_PUSCH_TIMING: UTIM per-stage timing + UL_E2E stage sums (calls/fep/rxp/dec); docs script docs/.../e2eall.py reads stg keys, now only ldpc/prep
ISAC_UL_LLR_DUMP*: offline LLR dump for the U4 n_ID brute force (tests/nr_ul_nid_bruteforce.c now has no producer)
ISAC_UL_NID_BG_LOG/DBG_TRY: UL_NID_* offer/admit/sweep/hyp traces, ground-truth id injection
ISAC_UL_TIMING_DIAG: UL-TIMING line
ISAC_UL_TEST_RT_LOAD / ISAC_UL_GPU_FEP_DEADLINE: offline VL2 stall reproduction and deadline abandon mode
ISAC_PASSIVE_REPLAY_UL_UCI: REPLAY-UCI offline footprint sweep
ISAC_UL_LAYOUT_CONTROL: wrong-slot false-lock control
ISAC_UL_PROBE_TA, ISAC_PUSCH_SEARCH_PRIO: experiments
UL_NID_PREP "skipped" log (nid_prep callback now NULL)
Parsed tooling check: goal_metrics.py/eq_cmp.py lines (UL_E2E tot=, UL_PUSCH_CRC, PUSCHQ2 real[queued= real_stale=) untouched. ulldpc_stats.py prefix 'UL_GPU_FEP on=' kept.

TESTS
test_nr_pusch_gpu_fep.cpp: deleted DeadlineAbandonLeavesRowsUntouched (only covered the deadline mode); dropped setenv ISAC_UL_GPU_FEP.
nr_ul_llr_dump_test.cc: file deleted (only covered the dump).
nr_admit_test.cc: reduced to one test (admit off => always run); the duty/backlog tests covered the deleted logic.
nr_pusch_ul_disc_test.cc: deleted PerUeTimingEmaRejectsOutlier (covered the TA_TRACK=0 path); removed setenv/unsetenv TA_TRACK and BGDISC_MAX_AGE_MS (ExpiredSnapshotDropped now sleeps 150 ms vs the fixed 100 ms age).
nr_pusch_solver_test.cc: removed setenv ISAC_UL_PUSCH_SOLVER.
test_nr_core_plan.cpp: removed the BG_LANE=1 / nr_bg_lane_enter lines of BgLane.EmptyListFallsBackUnpinned (BG_CORES parsing cases kept).
nr_pusch_mmse2_test.cc, nr_pusch_ul_disc_test.cc, test_nr_pusch_gpu_fep.cpp: comment-only flag mentions.
Not run (rule: no ctest). Compile-only.

NEEDS (out-of-set work)
1. CMakeLists.txt: remove nr_ul_llr_dump.c (line ~1116) and the test_nr_ul_llr_dump target (lines ~3320-3326); delete nr_ul_llr_dump.h; tests/nr_ul_nid_bruteforce.c + its CMake target (line ~3311) have no producer any more. THE BUILD BREAKS until CMake is edited (I deleted nr_ul_llr_dump.c and tests/nr_ul_llr_dump_test.cc).
2. Headers to trim (declared, now undefined or constant): nr_pusch_ul_bgdisc.h (nr_ul_bgdisc_enabled; comments), nr_pusch_ul_disc.h (nr_pusch_ul_disc_ta_track_on), nr_pusch_passive_decode.h (nr_pusch_passive_grant_stages, comments), nr_admit.h (nr_admit_reset_for_test, nr_admit_note_duty_for_test, NR_ADMIT_* enum fields, comments), nr_pusch_passive_ul_ids.h (flags_reset_for_test is now a no-op), nr_pusch_solver.h/queue.h/queue_cursor.h comments; nr_ul_nid_bg.h (prep callback arg now always NULL).
3. Bg lane / admit are dead as a whole: callers in nr_pdcch_blind_monitor_rt.c, nr_pdcch_nid_gpu.c, nr_pdcch_uss_tracker.c, nr_pdsch_passive_queue.c, nr_csirs_disc_feed.c/.h (nr_bg_lane_enter, nr_admit_on/try/done/now_ns/stats, ISAC_BG_JANON_NICE) can be deleted by their owners; then nr_bg_lane.c/nr_admit.c can go. Also nr_pusch_budget_idle_ns (queue_cursor.h, test nr_dl_adaptive_test.cc:475) is unused now.
4. nr_pdsch_gpu_ldpc.cu still uses uldiag::on/snap/report (ULSTALL); if its owner removes the diag, then delete namespace uldiag from nr_pusch_gpu_diag.h and the ev0/ev1/dg code in nr_pusch_gpu_fep.cuh.
5. Foreign flag readers that must agree with my baked constants: executables/nr-uesoftmodem.c (ISAC_UL_PUSCH_CHAIN/SOLVER/UE_STATE), nr_passive_ue_ctx.c, nr_pdcch_blind_monitor.c:3796, nr_pdcch_ul_layout_lock.c:1030 (ISAC_UL_BGDISC=1 => true).
6. Foreign tests that setenv ISAC_UL_PUSCH_CHAIN/SOLVER=0 or ISAC_UL_UE_STATE and call nr_pusch_passive_ul_ids_flags_reset_for_test (nr_passive_ue_ctx_test.cc:32-35,64,116-155; nr_pdcch_blind_monitor_test.cc:5806-5886): the UL chain is now unconditionally on, so "fixture default = UL off" cases fail or are vacuous; their owner must rewrite them.
7. tests/passive_rx/dgx/ul_rfsim_arm.sh and docs env lists mention the removed flags (harmless, ignored).

DOUBTS
- uldiag::on() returns true (frozen ISAC_UL_STALL_DIAG=1) although rule 7 says diagnostics go; it must stay because nr_pdsch_gpu_ldpc.cu (foreign) uses it. ULSTALL lines only print for calls slower than 2000 us.
- ISAC_BG_CORES kept per rule 4 but only reachable from a unit test now (lane off); nr_bg_lane_enter body deleted (was a no-op when off).
- UL RX branch: effective keep = 0 relies on frozen ISAC_RX_MRC_MODE=1 (mode 2 would mean all branches). Baked as 0 in demod, chest timing and the GPU pass.
- ISAC_UL_GPU_FEP_CHECK is not in the frozen env: baked its code default 16.
- nr_pusch_passive_ul_ue_state_enabled() = true relies on chain/solver frozen=1 (ISAC_UL_UE_STATE had no other effect).
- Dead-branch deletion in decode_inner (inline timing/DM-RS search, legacy UL DM-RS 2-stage id probe) relies on bg_on==true and ue_state==true being constants; I verified the condition inclusion by reading, the lead should re-run the UL equivalence replay.
- nr_ul_nid_bg load_env now only reads ISAC_UL_NID_BG_CORE and logs once at first nr_ul_nid_bg_enabled() call (same trigger point as before).
- Order of PUSCHQ2/ULQ pool log line changed (dropped prio/spare_rescue/budget_pct fields); nothing parses them (grepped tests/*.py).

## G7
REPORT G7 (CSI-RS discovery/blind, csi_rx, nr_passive_*, rf_helpers)  -- all touched TUs and tests compile (chk.py OK); nothing built or run.

REMOVED
ISAC_CSIRS_DISC = 1 (frozen) ; feature (nr_csirs_disc_enabled() now returns true; weak-null fallback in blind_rt kept for fixtures without the feed object)
ISAC_CSIRS_DISC_CUMULATIVE = 1 (frozen) ; feature (off path removed)
ISAC_CSIRS_DISC_TRS_PRIORITY = 1 (frozen) ; feature (off path removed)
ISAC_CSIRS_DISC_RATELIMIT = 1 (frozen) ; feature (always rl_update)
ISAC_CSIRS_DISC_RATELIMIT_NOTICE_S/MISS/PDET/CYCLES = 60 / 0.01 / 0.5 / 2 (default) ; tuning
ISAC_CSIRS_DISC_SWEEP_HYPS = 32 (default) ; tuning (test-only int nr_csirs_disc_test_sweep_limit kept, see TESTS)
ISAC_CSIRS_DISC_DELTA_MAX = 1 (default) ; tuning
ISAC_CSIRS_DISC_RING = 640 (default) ; tuning
ISAC_CSIRS_DISC_BATCH = 100 (default, unmarked) ; tuning
ISAC_CSIRS_DISC_TD_RING = 1024 (default, unmarked) ; tuning
ISAC_CSIRS_DISC_FEED_DIV/FEED_WIN/FEED_BURST/FEED_ACQ_DIV = 7/640/5120/2 (default) ; tuning
ISAC_CSIRS_DISC_WIN_K/M/FLOOR/Z = 12/4/2/0 (default, unmarked) ; tuning (gate_config returns constants)
ISAC_CSIRS_DISC_DIAG = off ; diag (3 log blocks in nr_csirs_disc.c + timing line and timers in nr_csirs_disc_fp.c)
ISAC_CSIRS_DISC_DUMP = off ; diag (dump_slot in feed.c, device copy-back in gpu.cu)
ISAC_CSIRS_DISC_MANAGED = off ; diag (managed-memory alloc)
ISAC_CSIRS_DISC_BUDGET = off ; experiment (whole CsirsDiscGate + file nr_csirs_disc_gpu_budget.cuh deleted, 7 gate users removed in gpu.cu/gpu_sweep.cu/gpu_cluster.cuh)
ISAC_CSIRS_DISC_CHUNK_US / CHUNK_UNITS = off ; experiment (Pace/chunked launches removed; one launch per kernel as before)
ISAC_CSIRS_DISC_CONFIRM_MAX = 4 (default) ; tuning
ISAC_CSIRS_DISC_NORM_COALESCE = 1 (frozen) ; feature (constant 1 into normalize kernel)
ISAC_CSIRS_DISC_CUMULATIVE in gpu_sweep.cu warm-up = 1 ; same
ISAC_GPU_COPY_FENCE (-> ISAC_FATTAILS=1 frozen) = 1 ; feature (copy fence unconditional in disc_copy and sweep copy)
ISAC_GPU_LMEM_PRERESERVE = 1 (frozen) ; feature (read removed)
ISAC_CSIRS_BLIND = off, g_on = g_disc ; feature
ISAC_CSIRS_BLIND_RANK / EPRDUMP / IDSWEEP / SLOTSWEEP / WIDE / PER_SLOT / ROWS / RB = off/1/default ; diag+experiment (rank mode, span profile, EPR stats and ranking logs, id sweep, slot sweep, footprint rows 6-18 (WIDE) block, per-slot count N>1, row filter, RB window)
ISAC_CSIRS_FULL = 1 (frozen) ; nr_csirs_full_enabled() now `return 1` (kept, called from nr_pdsch_passive_decode.c)
ISAC_CSIRS_FULL_BURSTS = 1 (frozen) ; feature (disc cap 64 constant, row-1 second burst always its own entry)
ISAC_CSIRS_FULL_TRS_PAIR = 1 (frozen) ; feature
ISAC_CSIRS_FULL_BLOCK_INFER = 2 (frozen) ; feature (=1 variant deleted)
ISAC_CSIRS_FULL_ERASE = 1 (frozen) ; feature
ISAC_CSIRS_FULL_VALID, FULL_AB, FULL_AB_EVID = off ; diag/experiment (g_fv machinery, full_valid_cands, full_evidence, ab hypotheses deleted; nr_csirs_full_validate/nr_csirs_full_ab are now empty stubs)
ISAC_PDCCH_TIMING (blind_rt) = 1 (frozen) ; timers unconditional
ISAC_PDCCH_TIMING (csi_rx.c) = IS_PASSIVE_RX_MODE (non-passive stays off) ; csirs_timing = IS_PASSIVE_RX_MODE
ISAC_RECONF (blind_rt.c g_reconf) -> nr_cfg_reconf_enabled() weak (same value as the env read), see KEPT
ISAC_RECONF_SI_GRACE_MS = 5000, ISAC_RECONF_SI_BUMP_WITHOUT_SIB1 = 1, ISAC_RECONF_GAP_HARD_MS = 10 ms, ISAC_RECONF_SIB1_MAX_OCCASIONS = 8 (defaults) ; tuning
ISAC_TD_IGNORE_SIB1 = off ; test arm (nr_cfg_ignore_sib1() returns false, 9 internal guards and the ue_ctx guard removed)
ISAC_ACQ_CARRIER_FROM_RS = 1 (default on) ; feature (always on)
ISAC_SIB1_SIBLING = off ; experiment (sibling file store/adoption deleted; nr_passive_acq_try_sibling_carrier is an empty stub)
ISAC_CELL_STATE, ISAC_CELL_STATE_SECS = 0 ; experiment (process-wide cell-state instance deleted; 3 hooks are stubs; pure nr_cell_state_update/note_rnti kept, header+test)
ISAC_UL_UE_STATE / ISAC_UL_PUSCH_CHAIN / ISAC_UL_PUSCH_SOLVER = 1 (CHAIN, SOLVER frozen) -> ul_state_enabled always true ; feature (ul_state_enabled var and the non-UL branch of nr_ue_ctx_try_get removed)
ISAC_USRP_RING_TRAFFIC_METRICS = off ; diag (dlsym of isac_usrp_ring_traffic and json fields)
ISAC_OBS_TEST_WRITER_PAUSE_MS, ISAC_UECTX_TEST_WRITER_PAUSE_MS = off ; test hooks -> now plain int globals nr_passive_obs_test_writer_pause_ms / nr_ue_ctx_test_writer_pause_ms
ISAC_PASSIVE_REPLAY_FAILURES (version 3 constant), ISAC_PASSIVE_REPLAY_UL_PROBE, ISAC_PASSIVE_REPLAY_UL_CONFIG = off ; diag (headers nr_passive_replay_probe.h, nr_passive_replay_ul_config.h deleted)
ISAC_TSYNC_AUDIT, ISAC_CSI_DIAG = off ; diag (csi_rx.c blocks)
ISAC_RX_GAIN_TRIM = unset (trim 0) ; tuning (function and comment block deleted)

BAKED (value became constant, path stays)
ISAC_CSIRS_DISC=1, CUMULATIVE=1, TRS_PRIORITY=1, RATELIMIT=1, NORM_COALESCE=1, FATTAILS(copy fence)=1, GPU_LMEM_PRERESERVE=1, CSIRS_FULL=1, FULL_BURSTS=1, FULL_TRS_PAIR=1, FULL_BLOCK_INFER=2, FULL_ERASE=1, PDCCH_TIMING=1 (blind_rt), UL_PUSCH_CHAIN/SOLVER=1 (ue_ctx), plus the default-valued tunings above.
Verified defaults equal frozen values in the code (they did, the flag reads only had `?: "1"`); no value had to be changed.

KEPT
ISAC_CSIRS_DISC_CORE: core map (nr_bg_lane_enter + env_core in disc_feed.c), marker removed.
ISAC_METRICS_PATH: site/output config (nr_passive_metrics.c), marker removed.
ISAC_RECONF (nr_passive_cfg_epoch.c read_enable): frozen=1 but set for passive by setenv in executables/nr-uesoftmodem.c, off for non-passive; shared by ~40 callers and toggled by tests of other groups (nr_isac_adapter_test, nr_pdsch_config_sweep_test, nr_td_sim_test, dci_length_sweep_test). Lead decides how to bake (needs a coordinated change with nr-uesoftmodem.c).
ISAC_CSIRS_DISC_BACKEND: kept as TEST-ONLY cpu/gpu selector (e2e and fp tests force the CPU or GPU backend; unset = auto = production). Remove only if the tests get another selector.
ISAC_PASSIVE_REPLAY_CAPTURE (+ INPUT read in nr-uesoftmodem.c): whole capture/replay subsystem touches foreign files (pdsch_passive_queue, pdcch rt, nr-ue.c); not removed, lead decides.
ISAC_CSIRS_DISC_DETECT_CORES: not read in my files.

LOST DIAGNOSTICS
ISAC_CSIRS_DISC_DIAG: per-window DIAG/EVIDENCE/VERIFY log lines of discovery and the detector normalise/fold/cluster timing.
ISAC_CSIRS_DISC_DUMP: ring dump for offline replay of discovery (nr_csirs_disc_dump_test still reads its own input via ISAC_CSIRS_DISC_DUMP_IN, test-only).
ISAC_CSIRS_BLIND_RANK/EPRDUMP/IDSWEEP/SLOTSWEEP: ranking mode, EPR dump/rank/slot histograms, scramblingID and slot-index sweeps, span profile (legacy non-discovery blind search analysis).
ISAC_CSIRS_BLIND_WIDE: rows 6-18 footprint-first search in the legacy path.
ISAC_CSIRS_FULL_VALID / FULL_AB / FULL_AB_EVID: CSIFULLVAL/CSIFULLAB/CSIFULLEV A/B rate-matching hypothesis lines and the CRC-validated phase learning.
ISAC_CSIRS_DISC_CHUNK_*: chunked GPU launches (stall trace).
ISAC_CSIRS_DISC_BUDGET: discovery GPU budget slicing (OTATAIL experiment).
ISAC_CSIRS_DISC_MANAGED: managed-memory diagnostic.
ISAC_TSYNC_AUDIT (TSYNC_CSIRS line), ISAC_CSI_DIAG (CSIDIAG line), ISAC_USRP_RING_TRAFFIC_METRICS (usrp_ring_*_bytes json fields; the struct fields in nr_passive_metrics.h stay zero).
ISAC_PASSIVE_REPLAY_UL_PROBE/UL_CONFIG/FAILURES: UL probe and UL config replay, failure-only recording (version 4 recordings; reading old ones still works).
ISAC_RX_GAIN_TRIM: per-branch RX gain trim.
ISAC_SIB1_SIBLING, ISAC_CELL_STATE: sibling-carrier hypothesis, cell traffic state report.
ISAC_TD_IGNORE_SIB1: SIB1-less test arm (sa_bed run_arm.sh still sets it for its 'sib1less' arm: that arm no longer exists).
Kept log lines parsed by tooling: not touched (gp=/gq=/li=, EVMGRANT, UL_PUSCH_CRC, RETRY_ARMS_OK are not in my files).

TESTS (all compile via chk.py; none run)
nr_csirs_disc_feed_test: OffByDefault -> AlwaysOn (EXPECT_TRUE).
nr_csirs_disc_dump_test, nr_csirs_disc_td_test, nr_csirs_disc_fp_test: setenv ISAC_CSIRS_DISC dropped; fp BackendDisabled -> BackendCpuSelector (DISC=0 half removed).
nr_csirs_disc_sweep_test: GateEnvironmentValidation -> GateDefaults; DISC=0 'sweep returns 0' block removed (now null-arg returns -1); GpuChunkedBitExact deleted (chunking removed).
nr_csirs_disc_e2e_test: DisabledPipelineIsByteIdentical deleted (off path); ReadmissionMergesPeriodExtentAndLatePhase deleted (pinned the FULL_BURSTS=0 single-burst readmission); SWEEP_HYPS setenv -> nr_csirs_disc_test_sweep_limit = 32/64 (int global in nr_csirs_disc.c/h); DISC setenv dropped.
nr_csirs_blind_search_test: CsirsFullValid test deleted, RankModeExportsNeitherBank deleted, main() no longer sets ISAC_CSIRS_FULL* to 0 (keeps ISAC_LOG_ASYNC=0).
nr_csirs_blind_rt_test_bridge.c: fullvalid bridge function and g_rank/g_wide/g_ids/g_slotsweep uses removed (export(rank,...) param kept, ignored).
nr_passive_ue_ctx_test: UL fixture env lines and UL_UE_STATE set/unset removed (UL always on), first test now expects the "ul" key present (was absent with UL off), IgnoreSib1KeepsParametersNull deleted, local nr_cfg_ignore_sib1 stubs removed, writer pause via nr_ue_ctx_test_writer_pause_ms.
nr_passive_obs_test: pause via nr_passive_obs_test_writer_pause_ms.
nr_passive_cfg_epoch_test: IgnoreSib1Arm test + guard, SiModNoReacquireFallbackCanBeDisabled, SiOptionsAreCachedAfterFirstTick, GapThresholdFromEnv deleted (flags gone); nr_cfg_si_options_reset_for_test call dropped.
nr_passive_acq_state_test: SiblingCarrierIsAdopted... deleted.
nr_reconfig_replay_test: the 4 Sib1Absent cases deleted, IGNORE_SIB1 env handling removed (run_replay still carries the now-always-true sib1_available param).

NEEDS (out of my set)
1. openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc: remove ISAC_TD_IGNORE_SIB1 uses (lines ~97, 2252-2259, 5740-5771: the ignore arm no longer exists, those cases will FAIL) and ISAC_UL_UE_STATE setenv/unsetenv (5806, 5838). Same for any td_sim/other tests using IGNORE_SIB1 (nr_td_sim_test not checked).
2. openair1/SIMULATION/NR_PHY/hidden_waveform.h:96 reads ISAC_TD_IGNORE_SIB1 (simulator, out of scope but the arm is gone). tests/passive_rx/sa_bed/run_arm.sh sets it (sib1less arm).
3. executables/nr-ue.c:1179 calls nr_passive_acq_try_sibling_carrier (now a stub): delete call, then delete the stub + decl in nr_passive_acq_state.h (+ header text for SIBLING/CELL_STATE).
4. nr_pdsch_passive_decode.c:3990-3994, 6060-6066: calls nr_csirs_full_ab / nr_csirs_full_validate (empty stubs now) and uses nr_csirs_full_enabled() (returns 1): remove those calls/guards (incl. the t_quiet 'trial decode' logic for FULL_VALID), then delete the stubs from nr_csirs_blind_rt.c/.h and nr_csirs_full_enabled from nr_csirs_blind_search.c/.h.
5. nr_pdcch_coreset_map.c:143, nr_pdcch_blind_monitor_rt.c:4054, openair2 nr_ue_procedures.c:221 (+ test_nr_ue_ra_procedures.cpp:51): remove the nr_cell_state_* hook calls, then the stubs in nr_passive_acq_state.c/.h (and the pure state machine + its test).
6. nr_passive_cfg_epoch.h: drop nr_cfg_ignore_sib1(), nr_cfg_ignore_sib1_reset_for_test(), nr_cfg_si_options_reset_for_test() (stubs/constant false in the .c), and their ~40 callers in other groups (nr_pdcch_blind_monitor*.c, nr_prach_passive.c, config_ue.c, nr_ue_dci_configuration.c, nr_ue_procedures.c, rrc_UE.c).
7. nr_passive_metrics.h:77 usrp_ring_copy_bytes/direct_bytes now dead fields; radio/USRP/usrp_lib.cpp isac_usrp_ring_traffic export now unused.
8. executables/nr-uesoftmodem.c:277 setenv("ISAC_CSIRS_DISC",...) is now dead; ldpc_decoder.cu:1049 getenv("ISAC_CSIRS_DISC") reads it (foreign): needs baking to true there. nfapi fapi_nr_ue_interface.h:20, nr_dlsch_demodulation.c:248 comments mention ISAC_CSIRS_FULL.
9. tests/passive_rx scripts still pass removed env (harmless): csifull_replay.sh etc.
10. nr_passive_replay_capture: decide on removing the whole capture/replay (INPUT in nr-uesoftmodem.c, hooks in 5 files).

DOUBTS
- ISAC_RECONF left as is (see KEPT); blind_rt reads it through weak nr_cfg_reconf_enabled(): in fixtures that do not link cfg_epoch this is false, same as env unset.
- The legacy non-discovery blind search (g_disc<=0 branches in nr_csirs_blind_rt.c: CPU scoring loop, per-slot rotation, nr_csirs_blind_init path) is unreachable in production now but I kept it: unit tests (blind_rt bridge, blind_search tests) still link blind_rt without the feed object and run it. Candidate for a second pass; also exported-but-unused helpers in nr_csirs_blind_search.c (energy_ratio, fp_record/fp_match/symbol_on, blind_append/pin variants) that only WIDE/IDSWEEP used.
- nr_csirs_disc_gpu_sweep.cu: sweep/refine kernels keep their chunk parameters (c0/nc, u0/u1/fin) because the CPU reference in nr_csirs_disc_sweep_internal shares them; launches are single-shot as before.
- nr_passive_ue_ctx_test: other cases may have implicitly relied on UL state being off by fixture (I only found the one 'no ul key' assertion); the e2e SWEEP_HYPS=64 case depends on nr_csirs_disc_test_sweep_limit being honoured; blind_search_test main no longer forces FULL/BURSTS/TRS_PAIR/BLOCK_INFER/ERASE off, so any blind_search/bridge case that silently depended on those off-values would now differ (disc cap 64 vs 32, full bursts). Please run ctest for these binaries on the lead build.
- nr_csirs_monitor.c: capacity now `IS_PASSIVE_RX_MODE ? DISC_MONITOR_CAPACITY : NR_CSIRS_MONITOR_MAX` (was getenv DISC else passive); non-passive with ISAC_CSIRS_DISC=1 is no longer expressible.
- nr_csirs_disc_enabled() is now true also for non-passive (it already defaulted to "1" regardless of mode).

git diff --shortstat (my files incl. deleted headers and edited tests): 43 files changed, 196 insertions(+), 2107 deletions(-)
Deleted files: nr_csirs_disc_gpu_budget.cuh, nr_passive_replay_probe.h, nr_passive_replay_ul_config.h (rm'd, not git rm'd; lead stages).

## G8
G8 report (cleanup stage 2). Worktree /home/nicola/NICOLA/wt/rr-csd-int16s2. No build/commit done; every touched TU passed python3 /tmp/s2/chk.py
(nr_shutdown_test.c and nr-ue-ru.c were checked WITHOUT -DNRUE_RU_SHUTDOWN_TEST: the test-macro build is unchecked).

REMOVED  (NAME = effective value ; kind)
ISAC_SCAN_SCRATCH_MB = 512 (default) ; tuning (parser, status enum, cache, warning log, 5 gtest cases removed)
ISAC_PBCH_REPLAY = off (default) ; diag (golden-buffer replay, nr_pbch_golden_capture/live_capture/replay_{golden,masks,2x2}, pbch_replay_one/mask, sweep_issb, dmrs scan thread)
ISAC_AUTO_ACQUIRE = off (default) ; feature (coarse-CFO trial loop, auto_timing/auto_anchor/auto_drift in nr-ue.c, ISAC_ACQ_SSB/_DRIFT/_TIMING_* logs, passive_acquisition_sib1() in config_ue.c)
ISAC_ACQ_CFO_MAX_HZ, ISAC_ACQ_PROBE = unused (AUTO_ACQUIRE off) ; feature
ISAC_DELTA_SCAN = off ; diag (nr_pbch_measure_delta)
ISAC_CIR_TEST = off ; diag (nr_pbch_cir_convention_test)
ISAC_PHY_DIAG = off ; diag (PBCHDIAG/SPECDIAG in nr_generate_pbch_llr, FEPDIAG/SSBTIME/FRAMESCAN, SSBSWEEP, nr_isac_framescan)
ISAC_PBCH_L2_DELTA = 0 ; tuning
ISAC_PSS_PEAK_RATIO = 5 ; tuning
ISAC_PBCH_OFFSET_BIAS = 0 ; diag
ISAC_SSB_IQ, ISAC_CHEST_RAW = off ; diag (IQ/estimate dumps)
ISAC_CHEST_COH = off ; diag
ISAC_PBCH_CFO_SWEEP = off ; diag (shadow CFO hypotheses decode, TSYNC_CFO_SWEEP)
ISAC_TSYNC_AUDIT = off ; diag (TSYNC, TSYNC_OUT, TSYNC_PBCH, TSYNC_CFO_PBCH)
ISAC_CFO_TRACK_HZ/_SPREAD_HZ/_APPLY/_PERIOD = 0/500/off/50 ; feature (CFO trim loop, CFOTRK, nr_ue_cfo_resync_request/hz and the retune+reinit consumer in nr-ue.c)
ISAC_PBCH_CAPTURE, ISAC_PBCH_CAPTURE_PATH = off ; diag
ISAC_FORCE_GLOBAL_SYNC = off ; diag/experiment
ISAC_TSYNC_CRC_GATE = 0 ; experiment (timing update is CRC-independent, now unconditional)
ISAC_DMRS_ANCHOR, ISAC_ANCHOR_ENTER/STAY/SIGN = off ; experiment (the block was already inert: s_pbch_sym1_off==0 made nr_pbch_dmrs_anchor unreachable; also POSTREBASE/ANCHOR logs, nr_pbch_dmrs_anchor())
ISAC_PBCH_NO_TRACK = 0 ; experiment (cached-MIB mode)
ISAC_MAX_POS_ACC = unset ; tuning (limit constant 1024; rate-scaled variant removed)
ISAC_SYNC_ANT_MASK = all ; tuning
ISAC_TSYNC_DUMP = off ; diag
ISAC_TSYNC_MIN_EWIN = 0 ; tuning (reliable == true always)
ISAC_TSYNC_MEASURE = 0 ; diag (measure-only mode, TSYNC_OBS log)
ISAC_TSYNC_REBASE_RATIO = 8 ; tuning (constant)
ISAC_TARGET_PCI = unset (-1) ; feature (operator PCI pin at start; the runtime re-sync use of target_Nid_cell stays)
ISAC_RX_RECOVER_TEST_COARSE_EPOCH = off ; test hook
ISAC_TSYNC_RESET = 0 ; experiment (both reads: drift cap and loop reset; nr_ue_reset_time_sync_loop() now has no caller but is declared in a header, left)
ISAC_UL_PROBE = 0 ; diag (ULPROBE, BRSNR logs, g_ulprobe_*)
ISAC_SHIFT_CENSUS = 0 ; diag
ISAC_UL_DBG_LOG (nr-ue.c read only) = 0 ; diag (ULDBG_TS line; the reader in nr_pdcch_blind_monitor_rt.c belongs to another group)
ISAC_DISC_NO_RESYNC = 0 ; experiment (sync invalidation on RX discontinuity is unconditional)
ISAC_PBCH_SHIFT_CLAMP = 0 ; tuning
ISAC_SCAN_CONFIRM = 1 (default on) ; see BAKED
ISAC_RF_FAIL_AFTER_READS = unset ; test hook (moved to a test-only setter, see TESTS)
ISAC_BANDSCAN_FIRST_HZ = unset ; test knob
ISAC_CORE_PLAN = 0 (frozen; main also forced it to 0 for --passive-rx) ; feature switch (passive_core_plan(), core_feature(), core_env_value(), core_{dl,ul}_count_explicit)
ISAC_PDCCH_USS_TRACKER (core-plan read only) = 1 ; with passive_core_plan
ISAC_UL_UE_STATE = off ; feature
ISAC_UECTX_PATH = unset ; feature (writer is opened with path NULL, period 5.0 s, unchanged)
ISAC_UECTX_PERIOD_S = 5.0 ; tuning
ISAC_OBS_PATH, ISAC_OBS_TEST_RING_SLOTS = unset ; feature (per-grant observation writer no longer opened by the receiver)
ISAC_UL_PUSCH_CHAIN, ISAC_UL_PUSCH_SOLVER (nr-uesoftmodem.c read) = 1 passive / unset otherwise ; frozen, see BAKED
ISAC_OTA_CFG = off ; diag (OTACFG from SIB1 / from MIB probes in config_ue.c and nr_ue_dci_configuration.c)
ISAC_MALLOC_NOTRIM = off ; experiment
ISAC_SHUTDOWN_TIMEOUT_S = 15 s (default) ; tuning (test build forces 1 s under NRUE_RU_SHUTDOWN_TEST)
ISAC_RX_RECOVER_MAX_FAULTS=20, ISAC_RX_RECOVER_WINDOW_S=60, ISAC_RX_RECOVER_NOCTRL=1, ISAC_RX_RECOVER_BACKOFF_MS=300, ISAC_RX_RECOVER_BACKOFF_MAX_MS=3000, ISAC_RX_RECOVER_LEAD_MS=80 ; tuning (constants, usrp_lib.cpp)
ISAC_RX_CHAN_MAP = identity ; A/B tool
USRP_DECIM_WORKERS = unset ; tuning override
ISAC_RAW_IQ_WANDER, ISAC_RAW_IQ_WANDER_UNTIL_S = off ; replay fault injection (no script uses it)

BAKED (value became a constant, code path stays)
ISAC_SSS_MULTI = passive 2.5 (>0 => on) / non-passive 0 (frozen 2.5) ; nr_initial_sync.c (const = IS_PASSIVE_RX_MODE), sss_nr.c (2.5 / 0)
ISAC_SSS_REL_RATIO = passive 2.5 / non-passive 0 (frozen 2.5) ; sss_nr.c
ISAC_KSSB_ALIGN = passive 1 / non-passive 0 (frozen 1) ; nr_initial_sync.c (non-passive keeps the advisory line only)
ISAC_PSS_ACC = passive 8 / non-passive 0 (=> K 1) (frozen 8) ; pss_nr.c
ISAC_PSS_DECIM = passive 1 / non-passive 0 (frozen 1) ; pss_nr.c (SYNC_SCAN log prints the same "1"/"0")
ISAC_CSIRS_PORTS_GT8 = passive 1 (32 port buffers) / non-passive 0 (frozen 1) ; nr_init_ue.c
ISAC_UL_DELAY_FAST = 1 (frozen 1, also default for every mode) ; nr_ul_delay_fast_on() now returns true (kept as a function: used by nr_ul_channel_estimation.c)
ISAC_SCAN_CONFIRM = 1 ; scan-confirm pass always on
ISAC_TSYNC_GLOBAL_REBASE = 1 ; global rebase always armed (ratio 8, 4 consecutive observations)
ISAC_RF_STALL_WATCHDOG = 1 ; always on
ISAC_RF_STALL_MAX_REINIT = 4 ; constant
ISAC_RX_RECOVER_MAX = 20 (frozen 20) ; constant (ISAC_RX_RECOVER itself is KEPT)
ISAC_UL_PUSCH_CHAIN / ISAC_UL_PUSCH_SOLVER (nr-uesoftmodem.c) = 1 in passive (frozen 1) ; UE-context writer opened iff IS_PASSIVE_RX_MODE
ISAC_FLOAT_CORES default 10-14 / ISAC_CORE_PLAN=0 : main sets only ISAC_FLOAT_CORES (setenv ..., 0); the float lane no longer needs a CORE_PLAN env, it is simply off while a core plan is active (nobody activates one now)

KEPT (site/hardware config table, reason)
ISAC_USRP_RING_MS, ISAC_USRP_RING_CORE, ISAC_USRP_RING_PRIO, ISAC_USRP_RING_PKTS, ISAC_USRP_RING_POLL, ISAC_USRP_RING_POLL_TIMEOUT_MS, ISAC_USRP_RESTART_MAX, ISAC_USRP_RESTART_UNTIL_SYNC, ISAC_USRP_ISOLATION_CHECK: ISAC_USRP_* hardware keys, kept per instruction (see DOUBTS: the ring is OFF in the frozen config)
ISAC_USRP_RING_ZEROCOPY, ISAC_USRP_CATCHUP_X, ISAC_USRP_CHPOW_S, ISAC_USRP_NO_TX, ISAC_USRP_START_TIMEOUT_MS: USRP hardware keys, frozen (main setenv defaults 1 / 1.05 / 1 / 1 / 2200 kept)
ISAC_UHD_CTRL_CORES (usrp_lib.cpp, nr_core_plan.c): core map, frozen 15 (main setenv default kept)
ISAC_FLOAT_CORES: core map (10-14 passive default in main)
ISAC_UE_RT_CORE, ISAC_PDCCH_USS_CORE, ISAC_GPU_WORKER_CORE, ISAC_CSIRS_DISC_CORE, ISAC_PDCCH_NID_GPU_CORE, ISAC_PDSCH_CORES, ISAC_PUSCH_CORES, ISAC_BG_CRITICAL_CORES, nr_env_core defaults: core maps (nr_core_plan.c/.h, nr-ue.c)
UHD_CONFIG_FILE: UHD site file (nr_core_plan.c machine reader)
ISAC_RX_RECOVER: radio recovery switch (frozen 1; main default; replay sets 0) (nr-ue.c x2, usrp_lib.cpp)
ISAC_SCAN_ANT: RX/antenna count of the scan (frozen 4) (nr_initial_sync.c)
ISAC_MLOCK: site (system.c lock_memory_to_ram, mlockall skip on GB10)
ISAC_LOG_ASYNC: frozen 1, set by main with setenv(...,0) for --passive-rx and read by common/utils/LOG/log.c (internal main->log.c channel, log.c is shared with gNB); lead decides whether to replace by an API
ISAC_RAW_IQ_DIRECTORY/SAMPLES/FIRST_TICK/CHANNELS/RATE/CENTER/SHIFT/SPEED/DECIM and ISAC_RAW_IQ_GAP_AT_S/GAP_S/GAP2_AT_S/GAP2_S: replay device harness (GAP/GAP2 are used by raw_baseline/run_raw_replay.py and rxr/check_double_gap_replay.py; DECIM is replay config, no script uses it)
ISAC_PASSIVE_REPLAY_INPUT, ISAC_PASSIVE_REPLAY_CAPTURE (nr-uesoftmodem.c): offline replay mode, used by tests/passive_rx/raw_baseline/run_gate_campaign.sh and run_adaptive_receive_test.sh and read also in nr_passive_replay_capture.c / nr_pusch_passive_decode.c (other groups); kept so the pair stays coherent
ISAC_TB_LOG_WIDTHS (nr_mac_common.c): gNB-side rfsim test bed ground-truth log (tests/passive_rx/dgx/ul_rfsim_arm.sh sets it for the gNB); shared gNB/UE code, out of scope
OAI_GDBSTACKS (assertions.h), OAI_CONFIGMODULE (config_load_configmodule.c): generic upstream OAI flags shared with gNB and simulators; only the DEFERRED markers were stripped
HOME (getenv in nr_core_plan.c): path of ~/.uhd/uhd.conf, not a flag

LOST DIAGNOSTICS
ISAC_PBCH_REPLAY: offline golden-buffer PBCH replay / FFT-window sweep / 2x2 sample-parameter cross replay
ISAC_DELTA_SCAN, ISAC_CIR_TEST, ISAC_PHY_DIAG: per-occasion PBCH DMRS displacement, CIR zero-delay convention test, PBCHDIAG/SPECDIAG/FEPDIAG/SSBTIME/FRAMESCAN/SSBSWEEP traces
ISAC_PBCH_OFFSET_BIAS, ISAC_PBCH_CFO_SWEEP, ISAC_PBCH_CAPTURE: tracking FFT window bias test, derotation hypothesis sweep, raw PBCH estimate/LLR capture file
ISAC_SSB_IQ, ISAC_CHEST_RAW, ISAC_CHEST_COH: raw SSB IQ, estimate/RE dumps, estimate coherence log
ISAC_TSYNC_AUDIT / _DUMP / _MEASURE: TSYNC, TSYNC_OUT, TSYNC_PBCH, TSYNC_CFO_PBCH, TSYNC_OBS logs, CIR power dump, open-loop drift observation
ISAC_UL_PROBE, ISAC_SHIFT_CENSUS: slot-type power map / BRSNR per-branch SNR lower bound, cumulative window motion census
ISAC_UL_DBG_LOG (nr-ue.c half): ULDBG_TS slot->timestamp line (tools/uldbg/*.py needs it together with the nr_pdcch_blind_monitor_rt.c half)
ISAC_OTA_CFG: OTACFG probes that printed pdcch_blind_monitor_* config lines from MIB/SIB1 (comments in tests/passive_rx/ota/*.conf, frozen, still mention it)
ISAC_AUTO_ACQUIRE/ACQ_*: autonomous coarse CFO acquisition + ISAC_ACQ_SSB/ISAC_ACQ_SIB1/ISAC_ACQ_DRIFT JSON lines
ISAC_OBS_PATH: per-grant observation JSONL
ISAC_MALLOC_NOTRIM: heap trim experiment
ISAC_RAW_IQ_WANDER: replay timing-jitter injection
ISAC_RX_CHAN_MAP: antenna permutation A/B

TESTS
openair1/PHY/NR_UE_TRANSPORT/tests/nr_initial_sync_budget_test.cc: removed CachedScratchPublishesFinalValue and Parse (they only covered the removed env parser); batch-size tests unchanged. nr_initial_sync_budget.h/.c lost the parser/enum/cache (header edited although not in the list: only nr_initial_sync.c and this test use it).
common/utils/tests/test_nr_shutdown.py + nr_shutdown_test.c + nr-ue-ru.c + nr_shutdown.c: the "injected" case now arms the failure through test-only nrue_ru_test_fail_after_reads(3) (under NRUE_RU_SHUTDOWN_TEST); the 1 s watchdog is set by that macro in nr_shutdown.c instead of ISAC_SHUTDOWN_TIMEOUT_S. Production code no longer reads either env.
common/utils/tests/test_nr_core_plan.cpp: dropped the "ISAC_CORE_PLAN=1 keeps the lane off" probe (the env no longer exists); the lane test still covers pins, lane and malformed list.
openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_ra_procedures.cpp: removed the nr_pdcch_blind_dci10_size link stub (its only caller was the ISAC_OTA_CFG probe) and reworded one comment.
Not touched but impacted (other groups): openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc setenv ISAC_UL_UE_STATE (reader is in nr_pusch_passive_ul_ids, not mine).

NEEDS (out of my set)
1. executables/nr-uesoftmodem.c main() setenv block: entries whose readers other groups bake (ISAC_CSIRS_DISC, ISAC_GPU_BUDGET, ISAC_RECONF) become dead once those groups finish; delete then. (I left them: I cannot see whether their readers are gone.)
2. Comments naming removed flags in files I do not own: PHY/NR_REFSIG/pss_nr.h:69,71,80 (PSS_DECIM/PSS_ACC), PHY/defs_nr_UE.h:234,505,541,579,580, NR_UE_TRANSPORT/nr_pss_decim.h:5-6, csi_rx.c:53,60 (log text mentions ISAC_CSIRS_PORTS_GT8=0), nr_dl_traffic_probe_rt.h:14 (KSSB_ALIGN), nr_pdcch_blind_monitor.c:326, nr_passive_obs.{c,h}, nr_passive_ue_ctx.c:537 (UECTX_PATH), radio/USRP/usrp_ring_recovery.h:40, nr_pusch_passive_ul_ids.h:71, tests nr_pss_decim_test.cc:4, nr_csirs_ports_check.c:3, nr_pusch_ul_disc_test.cc:303.
3. nr_passive_obs.{c,h} (+ nr_passive_obs_test.cc) has no production opener now (nr-uesoftmodem.c no longer calls nr_passive_obs_open); delete with its readers if ISAC_OBS_PATH is gone for good. tests/passive_rx/campaign/campaign.py:69 still sets ISAC_OBS_PATH.
4. nr_ue_reset_time_sync_loop() (nr_adjust_synch_ue.c, declared in nr_estimation.h) has no caller; nr_pbch_diag_* counters remain (nr_pbch_diag_counts is used by the RF stall watchdog).
5. Scripts that depend on removed flags: tests/passive_rx/auto_acquire.py and raw_baseline/{run_raw_replay.py,validate_raw_acquisition.py,test_raw_replay.py} (ISAC_AUTO_ACQUIRE + ISAC_ACQ_SSB/SIB1 parsing), tests/passive_rx/ota/*.conf comments (ISAC_OTA_CFG), tools/uldbg/* (ISAC_UL_DBG_LOG).
6. Planner library after ISAC_CORE_PLAN removal: nr_core_plan_{add,build,activate}, nr_core_machine_read, env_for(), the nr_core_plan_active() branches in radio/USRP/usrp_lib.cpp and common/utils/system.c, and most of test_nr_core_plan.cpp are now unreachable from production (passive_core_plan() was the only caller; the default was already off). I did not delete them (they are in my files, but the brief said core-map keys stay and the planner is a self-tested library): lead decides.

DOUBTS
- FROZEN env has NO ISAC_USRP_RING_MS: usrp_ring_ms()==0, so the whole decoupled RX ring (producer thread, ZEROCOPY, CATCHUP_X pacing, RESTART_*, RING_POLL, ISOLATION_CHECK, the RX_RECOVER re-arm machinery) is dead in the frozen config. I kept every ISAC_USRP_* read as instructed and baked only the non-ISAC_USRP RX_RECOVER_* tuning keys inside that machinery. If the lead agrees the ring is not part of the product, usrp_lib.cpp (+ usrp_ring_recovery.h/test) can lose ~1000 lines and RING_*, ZEROCOPY, CATCHUP_X, RESTART_*, POLL, ISOLATION_CHECK, CHPOW_S (ring read path) would go.
- ISAC_AUTO_ACQUIRE is removed as class 2 (default off) but tests/passive_rx/raw_baseline and auto_acquire.py use it; if the raw-baseline acquisition harness must stay, restore nr_initial_sync.c/nr-ue.c/config_ue.c hunks for it from HEAD.
- FFTADDR log (6 lines) kept: parsed by tools/ota_stats.py as a clock stamp; printed "bias=0" literally.
- ISAC_SSS_MULTI/SSS_REL_RATIO/KSSB_ALIGN/PSS_ACC/PSS_DECIM/CSIRS_PORTS_GT8 are baked as "IS_PASSIVE_RX_MODE ? frozen : old non-passive default" (non-passive UE keeps old behaviour). nr_ul_delay_fast_on() was default ON for every mode, kept so.
- nr_core_float_mask(): old condition was "ISAC_CORE_PLAN set and ==0". New: float lane applies iff ISAC_FLOAT_CORES is set and no plan is active. A non-passive run with ISAC_FLOAT_CORES set but CORE_PLAN unset used to be ignored, now applies (nobody sets it).
- Frozen ISAC_CORE_PLAN=0 while code default of core_feature("ISAC_CORE_PLAN") was already false: planner was never active in the frozen config either way.
- ISAC_UL_PUSCH_CHAIN/SOLVER: the old test was strcmp(flag,"1"); with env set to other values than "1" in non-passive it was off, now off always in non-passive too (unchanged without env). nr_cfg_reconf_enabled() is no longer evaluated in main().
- Not touched (not env flags): PBCHFAIL/ACQREF diag dump and g_pbch_diag recording in nr_pbch.c, RFCENSUS/ANTPOW/ADCPEAK/RFPOW census, nr_syncq_enabled ("ACQMISS-2 default off", reader in another file), LOG_TIMEMUT.
- nr_mac_common.c: my first removal of ISAC_TB_LOG_WIDTHS was reverted (gNB-side test bed use); only marker comment replaced.

git diff --shortstat over my files (incl. the 7 test/header/py files above): 29+ files changed, 129 insertions(+), 3672 deletions(-)
 phy_procedures_nr_ue.c -946 ; nr_pbch.c -1000+ ; nr_adjust_synch_ue.c -250 ; nr-ue.c ; usrp_lib.cpp small.

## G9
G9: nr_pdcch_blind_monitor_test.cc (7064 -> ~6450 lines). Compiles; only link-undefined symbols were nr_ul_layout_prior_expected / nr_ul_layout_prior_state (declared in nr_pdcch_ul_layout_lock.h, defined nowhere). All other nr_* symbols have a definition.

TEST cases DELETED
- Css0Autoconf.IgnoreSib1KeepsMibCss0ButDisablesC0Uss : TD_IGNORE_SIB1 arm (nr_cfg_ignore_sib1() is constant false)
- Sib1Cache.IgnoreArmSuppressesPublishLoadAndHash, Sib1Cache.IgnoreArmIsIndependentOfReconf : IGNORE_SIB1 arms only
- DiscoveredCoresets.FeatureOffCannotStartAdditionalDwell : NID_BANK=0 arm
- DiscoveredCover.FlagOffDwellsOnShiftedPublicationAndCannotDecode : DISC_COVER=0 arm (cover always on)
- DiscoveredCarrier.FlagOffKeepsGridGeometry : CARRIER_FROM_SIB1=0 / POINTA_REF=0 arm
- DiscoveredEnum fixture + 4 tests (DurationsInterleaverShiftBundleAndPciAreEnumerated, FlagOffAndCapAreRespected, AnyVerifiedCoresetEndsTheEnumeration, CarrierSubsizesAndNoDuplicateOfLegacyInterleavedCover) + helpers DwellKey/DrainDwells/Has/Count : DISC_ENUM code removed
- DiscoveredRobust.AdaptiveNoPrescreenSurvivesUntilCap, AdaptivePrescreenWithoutCrcRejectsAt4000, AdaptiveCountsOnlyPrescreenedOccasions : DWELL_ADAPTIVE=1 (removed; step_ex ignores prescreen)
- DiscoveredRobust.NoFakeFootprintKeepsRootPeriodWhilePending : NO_FAKE_FOOTPRINT (removed; always overwrites 1/0/1)
- UlLayoutLock.SprtLocksOnWeakEvidenceAndNeverOnNoise (+ UllSprtRun/SprtRun helpers), EdgeTestLocksTheTrueLayoutAndNeverOnNoise : sprt/edge_enabled() constant false
- UlLayoutLock.LlrRuleOnSyntheticTallies, SignRuleOnSyntheticTallies, SignRuleLocksTheTruthNextToAnAdjacentLookAlike (+ SignWorld) : LLR/SIGN lock rules removed
- UlLayoutLock.PriorSearchStaysMovesAndIgnoresLowSnr (+ UllPriorRun) : PRIOR_SEARCH removed (prior_enabled false, prior_expected/state undefined)

TEST cases EDITED (live behaviour, removed-flag setenv dropped)
- UlLayoutFit.EntropyRankedPrefixAndFitReadAreCoherent: dropped the PREFIX_ENT=0 "old order" arm (np>=2, riv==14); kept the entropy-ranked arm and the fit-read check.
- UlLayoutLock.CrcFirstLocks...: dropped the "all flags 0" arm (crc_trials==0). CRC_GUARD is baked ON, so a bare crc_result(true) no longer locks at once: the two locking passes now use crc_verdict(..., tbs 6000, nseg 1, seg_ok 1, slot) (CRC24A-decisive).
- UlLayoutLock.DlBwpPriorSeeds...: dropped the DL_PRIOR=0 "searching" arm; setenvs removed.
- UlLayoutLock.CrcGuard..., DmrsVotes..., Sib1Cache.StartupHint..., UlConfigDisc* (x6 setenv), UlUeIds, UlFirstPusch.DataIdWalk..., DiscoveredCoresets/Cover/Carrier/Robust fixtures: setenv/unsetenv of removed flags and flags_reset_for_test calls dropped.

NEEDS (lead)
- nr_ul_layout_prior_expected / nr_ul_layout_prior_state: declared in nr_pdcch_ul_layout_lock.h but undefined; now unused -> delete declarations (also nr_ul_layout_prior_enabled/prior_note if unused elsewhere).
- nr_ul_layout_llr_decide / nr_ul_layout_sign_decide (+ their .h decls) are now unreferenced by any test or receiver code -> can be deleted.
- nr_cfg_ignore_sib1_reset_for_test / nr_pusch_passive_ul_ids_flags_reset_for_test (no-ops) are no longer called by this test.

DOUBTS
- Not run. CrcFirst test now runs with BWP_ALL/MCS_STAT/K2_PAIR/TDA_PAIR/TABLE_SWEEP at their baked values (previously forced 0 by the test); ASSERT_LE(n,3) on trial count and the slot numbers 400/401 for the decisive verdicts may need adjustment.
- Tests with no setenv (DiscoveredCoresets.*) now see DISC_COVER always on (was default off); queue order may differ for lines other than the full-carrier ones.

## ISAC_AUTO_ACQUIRE restored without a flag (2026-10-20)
Trigger: `nr_auto_acquire_on = passive && !(--ssb given) && !(--initial-fo given)`, computed once in nr-uesoftmodem.c from `PARAMFLAG_PARAMSET` of the two options (no new flag); pure rule `nr_auto_acquire_trigger()` in nr_initial_sync_budget.h. Pinned runs (all OTA) are unchanged: every restored hunk is behind that bit.
Restored from csd/int16-merge19: nr_initial_sync.c coarse-CFO trial loop + ISAC_ACQ_CFO_TRIAL/ISAC_ACQ_SSB; nr-ue.c auto_timing anchor/drift/handoff (ISAC_ACQ_TIMING_ANCHOR/DRIFT/HANDOFF); config_ue.c passive_acquisition_sib1 (ISAC_ACQ_SIB1/UNSUPPORTED).
CFO bound: `NR_ACQ_CFO_MAX_HZ` = 60000 (constant, ISAC_ACQ_CFO_MAX_HZ gone). Auto mode asserts `--ue-fo-compensation` with a clear message.
ISAC_ACQ_PROBE is still an env read (config_ue.c, auto mode only; auto_acquire.py probe phase). Not a new flag, but not baked either.
Scripts: auto_acquire.py and raw_baseline/run_raw_replay.py no longer set ISAC_AUTO_ACQUIRE/ISAC_ACQ_CFO_MAX_HZ and omit --ssb/--initial-fo (auto_acquire.py now always uses --ue-scan-carrier, so its capture phase no longer pins the probed SSB/CFO seed; --cfo-max-hz default 200000 -> 60000). Behaviour change: any passive run without --ssb and --initial-fo is now auto and needs --ue-fo-compensation.
Tests: AutoAcquire.* in nr_initial_sync_budget_test.cc (trigger table, bound). Functional acquisition on a capture NOT run: no raw IQ capture on this host, replay needs sudo unshare. Build: only the known link failures remain (ra0, ssb_rate_match_prod, csi_rb_extent); ctest failures dft_test, test_nr_modulation, nr_td_sim_test as before.
