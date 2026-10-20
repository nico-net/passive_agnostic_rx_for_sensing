# CLEANUP 2026-10-18 -- inventory of every environment variable the receiver reads

Branch csd/int16-clean (from csd/int16-int @ 82e542e2d0), config t26yE = `rr-orchestration/ota/t26yE.env` (225 ISAC_* flags + `CUDA_DEVICE_MAX_CONNECTIONS` + `NR_GPU_FEP`, 227 variables).
Method: [CODE-READ] `git grep` of every `getenv` and of every wrapper that takes a variable name (env_int, ulq_env, core_feature, feature, env_on, setting, pss_env_int, usrp_env_int, envl, disc_env_int, nr_env_list_nth, nr_bg_lane_enter, array-driven readers), receiver code only (openair1, common, executables, radio, openair2; unit-test sources excluded).

Classes
- A: set in t26yE to a value that differs from the code default (the default was changed to the t26yE value in the bake-in commit).
- B: set in t26yE to the value that already was the code default (no edit).
- SITE-ENV: not read by our code (CUDA runtime variable); stays in t27.env.
- C: not in t26yE (section 3).

## 1. Launch-script settings (not edited, listed for completeness)
- `rr-orchestration/ota/starttest.sh` prepends: `ISAC_CSIRS_DISC=1` (DISCON), `ISAC_CSIRS_DISC_CORE=11`, `ISAC_CSIRS_DISC_DETECT_CORES=12,13,14`, `ISAC_UHD_CTRL_CORES=2,3,4`, `ISAC_RX_MRC_MODE=1`, `ISAC_USRP_RING_MS=500`, `ISAC_USRP_RING_CORE=16`, `ISAC_SCAN_ANT=1`, `ISAC_UE_RT_CORE=5`, `ISAC_PDCCH_USS_CORE=7`; the env file given on the command line comes after and overrides (so t26yE wins for `CSIRS_DISC`, `UHD_CTRL_CORES=15`, `RX_MRC_MODE`, `SCAN_ANT=4`, `PDCCH_USS_CORE=12`). Still passed by the script and NOT baked: `ISAC_CSIRS_DISC_CORE=11`, `ISAC_CSIRS_DISC_DETECT_CORES=12,13,14`, `ISAC_USRP_RING_MS=500`, `ISAC_USRP_RING_CORE=16`, `ISAC_UE_RT_CORE=5`.
- `rr-orchestration/ota/ota.sh` passes its arguments as `env` assignments and sets no ISAC_* of its own.
- `tests/passive_rx/p99_replay.sh` (replay harness) prepends the same keys plus `ISAC_MLOCK=0`, `ISAC_RAW_IQ_*`, `ISAC_RX_MRC_MODE=1 ISAC_SCAN_ANT=1 ISAC_PDCCH_USS_CORE=7 ISAC_CSIRS_DISC=1`, and strips `ISAC_USRP_*`, `ISAC_UHD_*`, `ISAC_RX_RECOVER*` from the env file. The new `p99_replay_clean.sh` drops the four behaviour keys and the env file.

## 2. The 227 t26yE variables
| variable | t26yE | class | note |
|---|---|---|---|
| CUDA_DEVICE_MAX_CONNECTIONS | 32 | SITE-ENV |  |
| ISAC_ANONLEN | 1 | A |  |
| ISAC_CARRIER_FROM_SIB1 | 1 | A |  |
| ISAC_CHEST_DELAY_SHARE | 0 | B | default 0 |
| ISAC_CHEST_GPU_DELAY | 1 | A |  |
| ISAC_CHEST_PDP_DIAG | 0 | A |  |
| ISAC_COREMAP_IDSWEEP_DUR | 3 | A | coreset_map.c default was 3 only if NID_GPU enabled else 1; now 3 always |
| ISAC_CORESET_DISC_SYNCQ | 1 | A |  |
| ISAC_CORESET_DISC_WATCH | 1 | A |  |
| ISAC_CORE_PLAN | 0 | A | unset now = 0; test_nr_core_plan unsetenv expects plan on |
| ISAC_CSIRS_DISC | 1 | A | 7 readers + core_feature in nr-uesoftmodem.c; many tests setenv/unsetenv it |
| ISAC_CSIRS_DISC_CUMULATIVE | 1 | A |  |
| ISAC_CSIRS_DISC_NORM_COALESCE | 1 | A |  |
| ISAC_CSIRS_DISC_RATELIMIT | 1 | A |  |
| ISAC_CSIRS_DISC_TRS_PRIORITY | 1 | A |  |
| ISAC_CSIRS_FULL | 1 | A |  |
| ISAC_CSIRS_FULL_BLOCK_INFER | 2 | A |  |
| ISAC_CSIRS_FULL_BURSTS | 1 | A |  |
| ISAC_CSIRS_FULL_ERASE | 1 | A |  |
| ISAC_CSIRS_FULL_TRS_PAIR | 1 | A |  |
| ISAC_CSIRS_PORTS_GT8 | 1 | A |  |
| ISAC_DCI11_HARD_REFUTE | 1 | A |  |
| ISAC_DCI11_HARQPID | 1 | A | presence-only: replaced by constant |
| ISAC_DCI11_HARQ_SEQ | 1 | A |  |
| ISAC_DL_TRAFFIC_PROBE | 16 | A |  |
| ISAC_DMRS_SCAN_CORE | -1 | B |  |
| ISAC_E2E_TRACE | 1 | A |  |
| ISAC_FATTAILS | 1 | A | umbrella fallback baked in nr_fattails_on, retry_budget_us, COPY_FENCE fallbacks (csirs_disc_gpu*.cu): all unset FATTAILS flags now on |
| ISAC_FLOAT_CORES | 10-14 | A |  |
| ISAC_GPU_BUDGET | 1 | A |  |
| ISAC_GPU_CHECK_CORES | 10-14 | A |  |
| ISAC_GPU_DISCOVERY_BUDGET_PCT | 30 | B | default 30 |
| ISAC_GPU_DISC_SMS | 8 | A |  |
| ISAC_GPU_LDPC_CL_MIN_CB | 4 | A |  |
| ISAC_GPU_LDPC_NC_MIN_CB | 8 | A |  |
| ISAC_GPU_LDPC_SM | 1 | A |  |
| ISAC_GPU_LMEM_PRERESERVE | 1 | A |  |
| ISAC_GPU_POLL_SLEEP_US | 200 | A |  |
| ISAC_GPU_RT_ISOLATE | 1 | A |  |
| ISAC_GPU_SELFCHECK | 0 | A |  |
| ISAC_GPU_WORKER_CORE | 10 | A |  |
| ISAC_KSSB_ALIGN | 1 | A |  |
| ISAC_L2MAXH_DELTA_NL2 | -3 | A |  |
| ISAC_LOG_ASYNC | 1 | A |  |
| ISAC_MCS_TABLE_EVID | 1 | A |  |
| ISAC_METRICS | 1 | B | no C reader (only ISAC_METRICS_PATH); script-grep only |
| ISAC_PDCCH_BANK_HANDOFF | 1 | A |  |
| ISAC_PDCCH_C0_OCC_GATE | 1 | A |  |
| ISAC_PDCCH_DED_BUDGET_SHARE | 2 | A |  |
| ISAC_PDCCH_DED_BUDGET_US | 300 | A |  |
| ISAC_PDCCH_DED_EVERY_SLOT | 1 | A |  |
| ISAC_PDCCH_DISC_COVER | 1 | A |  |
| ISAC_PDCCH_DWELL_OWN_BUDGET | 1 | A |  |
| ISAC_PDCCH_FEP_CACHE | 1 | A |  |
| ISAC_PDCCH_GATE_CFAR | 1 | A |  |
| ISAC_PDCCH_JANON_ALL_CORESETS | 1 | A |  |
| ISAC_PDCCH_JOINT | 1 | A |  |
| ISAC_PDCCH_JOINT_ANON | 1 | A |  |
| ISAC_PDCCH_JOINT_ANON_ASYNC | 1 | A |  |
| ISAC_PDCCH_JOINT_ANON_DISCOVERY | 1 | A |  |
| ISAC_PDCCH_JOINT_ANON_LEN_HINT | 1 | A |  |
| ISAC_PDCCH_JOINT_ANON_RATE | 1 | A | atof default 0.0 -> 1 |
| ISAC_PDCCH_JOINT_ANON_STABLE | 1 | A |  |
| ISAC_PDCCH_KNOWN_PRESCREEN | 1 | A |  |
| ISAC_PDCCH_NID_BANK | 1 | A |  |
| ISAC_PDCCH_NID_GPU | 1 | A | also core_feature in nr-uesoftmodem plan path now true |
| ISAC_PDCCH_NID_GPU_CAPTURE_EVERY | 2 | A |  |
| ISAC_PDCCH_NID_GPU_CORE | 13 | A |  |
| ISAC_PDCCH_NID_GPU_DUTY | 1 | A |  |
| ISAC_PDCCH_NID_GPU_RECHECK_MS | 250 | A |  |
| ISAC_PDCCH_POINTA_REF | 1 | A |  |
| ISAC_PDCCH_RBOFFSET_FIX | 1 | A |  |
| ISAC_PDCCH_SCAN_TIMING | 1 | A | presence-only: replaced by constant |
| ISAC_PDCCH_TIMING | 1 | A | presence-only: replaced by constant |
| ISAC_PDCCH_USS_CORE | 12 | A |  |
| ISAC_PDCCH_USS_FAST | 1 | A |  |
| ISAC_PDCCH_USS_HASH | 1 | A |  |
| ISAC_PDCCH_USS_TRACK_BACKOFF | 4 | A |  |
| ISAC_PDSCHQ_PROBE_LAST | 1 | A |  |
| ISAC_PDSCH_CORES | 7,3,4,0,1 | A |  |
| ISAC_PDSCH_CSI_POPULATION_RETRY | 1 | A |  |
| ISAC_PDSCH_CSI_RETRY | 1 | A |  |
| ISAC_PDSCH_DMRS_CDM_RETRY | 1 | A |  |
| ISAC_PDSCH_DMRS_ID_CONTRAST | 1 | A |  |
| ISAC_PDSCH_DMRS_ID_PER_CORESET | 1 | A |  |
| ISAC_PDSCH_DMRS_ID_REFRESH | 64 | A |  |
| ISAC_PDSCH_DMRS_ID_SCAN | 16 | A |  |
| ISAC_PDSCH_DMRS_SCAN_ASYNC | 1 | A |  |
| ISAC_PDSCH_EVM_GRANT | 1 | A | presence-only checks: now always true; SNR_GATE OR-sites |
| ISAC_PDSCH_EVM_PROBES | 1 | A | presence-only checks: now always true; SNR_GATE OR-sites |
| ISAC_PDSCH_GATHER_FAST | 1 | A |  |
| ISAC_PDSCH_GPU | 0 | B | fep.h: PDSCH_GPU=0 overrides NR_GPU_FEP=1; unset both = off too. nr-uesoftmodem.c:242 ORs NR_GPU_FEP into the GPU core-plan role only (inactive when CORE_PLAN=0 baked by fork C) |
| ISAC_PDSCH_GPU_EXACT_CHECK | 1 | B | default already 1 |
| ISAC_PDSCH_GPU_EXACT_CHEST | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_CHEST_CHECK | 64 | A |  |
| ISAC_PDSCH_GPU_EXACT_DEMOD | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK | 64 | A |  |
| ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_DEFER | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_NL34 | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_DEMOD_MMSE3 | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_DEMOD_NL34 | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_FEP | 1 | A |  |
| ISAC_PDSCH_GPU_EXACT_LDPC | 1 | A |  |
| ISAC_PDSCH_GPU_HOST_PIN | 1 | A |  |
| ISAC_PDSCH_HOST_FAST | 1 | A |  |
| ISAC_PDSCH_INLINE_POOL | 1 | A |  |
| ISAC_PDSCH_MMSE_EXACT_NL34 | 1 | A |  |
| ISAC_PDSCH_MMSE_MODE | 2 | A |  |
| ISAC_PDSCH_PDCCH_RM | 1 | A |  |
| ISAC_PDSCH_RECOVERY_ASYNC | 1 | A |  |
| ISAC_PDSCH_REP_FIELDS | 1 | A |  |
| ISAC_PDSCH_RETRY_NORESTORE | 1 | A |  |
| ISAC_PDSCH_RETRY_PREDICT | 1 | A |  |
| ISAC_PDSCH_RETRY_YIELD | 2 | A |  |
| ISAC_PDSCH_SLOT_SPREAD | 1 | A |  |
| ISAC_PDSCH_SNR_GATE | 1 | A | value read wrapped in blind_monitor_rt.c; OR-ed presence sites wrapped too |
| ISAC_PDSCH_SOLVER | 1 | A |  |
| ISAC_PDSCH_SOLVER_BACKOFF | 200 | A |  |
| ISAC_PDSCH_SOLVER_BUDGET | 24 | A |  |
| ISAC_PDSCH_SOLVER_COVERAGE | 1 | A |  |
| ISAC_PDSCH_SOLVER_DMRS_DOMAIN | 1 | A |  |
| ISAC_PDSCH_SOLVER_DMRS_PRIO | 1 | A |  |
| ISAC_PDSCH_SOLVER_FAIRSHARE | 1 | A |  |
| ISAC_PDSCH_SOLVER_LAYOUT_RECOVER | 32 | A |  |
| ISAC_PDSCH_SOLVER_NL_PREF | 1 | A |  |
| ISAC_PDSCH_SOLVER_PRIOR_CRC | 1 | A |  |
| ISAC_PDSCH_SOLVER_PRIOR_IDS | 1 | A |  |
| ISAC_PDSCH_SOLVER_PROBELOG | 1 | A |  |
| ISAC_PDSCH_SOLVER_PROBE_BURST | 16 | A |  |
| ISAC_PDSCH_SOLVER_PROBE_FILL | 25 | A |  |
| ISAC_PDSCH_SOLVER_PROBE_RATE | 0.5 | A |  |
| ISAC_PDSCH_SOLVER_REUSE | 1 | A |  |
| ISAC_PDSCH_SOLVER_SCR | 1 | A |  |
| ISAC_PDSCH_SOLVER_SCR_CLEAN | 0 | B | default 0 |
| ISAC_PDSCH_SOLVER_SCR_CSIRS | 1 | A |  |
| ISAC_PDSCH_SOLVER_SCR_EXPLICIT | 1 | A |  |
| ISAC_PDSCH_SOLVER_SIGNIF | 1 | A |  |
| ISAC_PDSCH_SOLVER_THREAD_CTX | 1 | A |  |
| ISAC_PDSCH_SOLVER_WHY | 1 | A |  |
| ISAC_PDSCH_TDA_FIELD | 1 | A |  |
| ISAC_PDSCH_TDRA_RETRY | 1 | A |  |
| ISAC_PDSCH_WIDE_MRC_RETRY | 1 | A |  |
| ISAC_PDTIM_CLASS | 1 | A |  |
| ISAC_PDTIM_PCT | 1 | A |  |
| ISAC_POINTA_SWEEP | 1 | A |  |
| ISAC_PSS_ACC | 8 | A |  |
| ISAC_PSS_DECIM | 1 | A | pss_env_int got a def param; nr_initial_sync log default also 1 |
| ISAC_PUSCH_CONSUMERS | 2 | A |  |
| ISAC_PUSCH_CORES | 8,9 | A |  |
| ISAC_PUSCH_MMSE_MODE | 2 | A |  |
| ISAC_PUSCH_PRIO | 0 | A |  |
| ISAC_RA_TDRA_SWEEP | 1 | A |  |
| ISAC_RECONF | 1 | A | hidden_waveform.h (sim generator) left untouched; CMake sets ISAC_RECONF=1 for 6 tests |
| ISAC_RX_MRC_ALIGN | 2 | A |  |
| ISAC_RX_MRC_MODE | 1 | A | UL sites (nr_ulsch_demodulation.c, nr_ul_channel_estimation.c, nr_pusch_passive_decode.c) pass the raw string; unset vs 1 gives keep=0 either way, left untouched |
| ISAC_RX_MRC_PER_RNTI | 1 | A |  |
| ISAC_RX_RECOVER | 1 | A | nr-ue.c reads it in the main exe: on replay NEW now has rx_recover ON (OLD replay stripped it) -> pass ISAC_RX_RECOVER=0 in NEW replay launch to stay equivalent |
| ISAC_RX_RECOVER_MAX | 20 | B |  |
| ISAC_SCAN2_SLOWPASS_MS | 8 | A |  |
| ISAC_SCANCOST | 2 | A |  |
| ISAC_SCANQ_FRESH | 6 | A |  |
| ISAC_SCAN_ANT | 4 | B | default = all RX ants; value 4 clamps to nb_antennas_rx, identical at 4 RX |
| ISAC_SCAN_READING_HASH | 1 | A |  |
| ISAC_SCAN_SCOREMEMO | 1 | A |  |
| ISAC_SEGV_BT | 1 | A | constructor in blind_monitor_rt lib now always installs SEGV handler (also in test binaries) |
| ISAC_SI_RV_ANY | 1 | A |  |
| ISAC_SLOT_SCALE | 1 | A |  |
| ISAC_SSB_FEP_GPU | 1 | A |  |
| ISAC_SSS_MULTI | 2.5 | A |  |
| ISAC_SSS_REL_RATIO | 2.5 | A |  |
| ISAC_SWEEP_SIGNIF | 1 | A |  |
| ISAC_TD_CB0_BACKEND | gpu | A |  |
| ISAC_UHD_CTRL_CORES | 15 | A | baked 15 via nr_env_core; replay: only is_critical/recovery readers see it (core 15 already in --thread-pool); usrp_lib not loaded on raw_iq; plan-mode readers (nr_core_plan.c:304, nr-uesoftmodem.c:205) kept raw getenv |
| ISAC_UL_BGDISC | 1 | A |  |
| ISAC_UL_BWP_DL_PRIOR | 1 | A |  |
| ISAC_UL_BWP_SEED | 1 | A |  |
| ISAC_UL_DCI_LOG | 1 | A |  |
| ISAC_UL_DELAY_FAST | 1 | A |  |
| ISAC_UL_DELAY_ONCE | 1 | A |  |
| ISAC_UL_FIT_READ | 1 | A |  |
| ISAC_UL_GPU_FEP | 1 | A |  |
| ISAC_UL_GPU_FEP_KCHARGE | 1 | A |  |
| ISAC_UL_GPU_LDPC | 1 | A |  |
| ISAC_UL_GPU_LDPC_CHECK | 64 | B |  |
| ISAC_UL_GPU_LDPC_HOLD | 1 | A |  |
| ISAC_UL_GPU_PART | rt | A |  |
| ISAC_UL_HARQ_COMBINE | 1 | A |  |
| ISAC_UL_K2_PAIR | 1 | A |  |
| ISAC_UL_LAYOUT_LOCK | 1 | A |  |
| ISAC_UL_LAYOUT_LOG | 1 | A |  |
| ISAC_UL_LDPC_SNR_MARGIN | 6 | A |  |
| ISAC_UL_LOCK_BWP_ALL | 1 | A |  |
| ISAC_UL_LOCK_CRC_FIRST | 1 | A |  |
| ISAC_UL_LOCK_CRC_GUARD | 1 | A |  |
| ISAC_UL_LOCK_CRC_K | 3 | B |  |
| ISAC_UL_LOCK_MCS_STAT | 1 | A |  |
| ISAC_UL_LOCK_SIB1_FIRST | 1 | A |  |
| ISAC_UL_LOCK_SIGN | 2 | A |  |
| ISAC_UL_MCS_TABLE_SWEEP | 1 | A |  |
| ISAC_UL_NID_BG | 1 | A |  |
| ISAC_UL_NID_BG_CPU_PCT | 10 | A |  |
| ISAC_UL_NID_BG_IDLE | 1 | A |  |
| ISAC_UL_NID_BG_IQ | 1 | A |  |
| ISAC_UL_PREFIX_ENT | 1 | A |  |
| ISAC_UL_PRIO_QUEUE | 1 | A |  |
| ISAC_UL_PUSCH_CHAIN | 1 | A |  |
| ISAC_UL_PUSCH_SOLVER | 1 | A |  |
| ISAC_UL_QUEUE_DEPTH | 32 | A |  |
| ISAC_UL_RETRY | 1 | A |  |
| ISAC_UL_RETRY_SNR_MARGIN | 3 | A |  |
| ISAC_UL_SNAP_CORE | 8 | A |  |
| ISAC_UL_SNAP_POOL | 1 | A |  |
| ISAC_UL_SNAP_PRIO | 0 | A |  |
| ISAC_UL_SOLVER_FALLTHROUGH | 2 | A |  |
| ISAC_UL_SPARE_RESCUE | 1 | A |  |
| ISAC_UL_STALE_DROP | 1 | A |  |
| ISAC_UL_STALL_DIAG | 1 | A |  |
| ISAC_UL_TA_TRACK | 1 | A |  |
| ISAC_UL_TDA_PAIR | 1 | A |  |
| ISAC_UL_UCI_BUDGET | 8 | A |  |
| ISAC_UL_UCI_SAFE | 1 | A |  |
| ISAC_USRP_CATCHUP_X | 1.05 | A |  |
| ISAC_USRP_CHPOW_S | 1 | A |  |
| ISAC_USRP_NO_TX | 1 | A |  |
| ISAC_USRP_RING_ZEROCOPY | 1 | A |  |
| ISAC_USRP_START_TIMEOUT_MS | 2200 | A |  |
| NR_GPU_FEP | 1 | B | effective B: see ISAC_PDSCH_GPU |

## 3. Class C: read by the receiver, not in t26yE
Columns: variable, number of read lines (HEAD), first reader, rough kind (diag = only adds logging/measurement by its name; other = tunable or experiment gate). The kind is a name heuristic, [CODE-READ], not a verified audit; stage 2 (deleting dead experiment paths) works through this list subsystem by subsystem and the final report lists what was deleted and what was left.
| variable | read lines | first reader | kind |
|---|---|---|---|
| ISAC_A2E_TRACE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:3044 | diag |
| ISAC_ACQ_CARRIER_FROM_RS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:265 | other |
| ISAC_ACQ_CFO_MAX_HZ | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:669 | other |
| ISAC_ACQ_PROBE | 1 | openair2/LAYER2/NR_MAC_UE/config_ue.c:2399 | other |
| ISAC_AGNOSTIC_V2 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_agnostic_v2.h:12 | other |
| ISAC_AL1_COVER | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1757 | other |
| ISAC_ANCHOR_ENTER | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1965 | other |
| ISAC_ANCHOR_SIGN | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1972 | other |
| ISAC_ANCHOR_STAY | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1967 | other |
| ISAC_ANONLEN_AUDIT_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6881 | other |
| ISAC_ANONLEN_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6929 | other |
| ISAC_ANONLEN_MIN_RNTI | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6928 | other |
| ISAC_ANONLEN_Z | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6879 | other |
| ISAC_AUTO_ACQUIRE | 3 | executables/nr-ue.c:1109 | other |
| ISAC_BANDSCAN_FIRST_HZ | 1 | executables/nr-ue-ru.c:158 | other |
| ISAC_BG_CORES | 1 | common/utils/nr_bg_lane.c:21 | other |
| ISAC_BG_JANON_BACKLOG | 1 | openair1/PHY/TOOLS/nr_admit.c:33 | diag |
| ISAC_BG_JANON_DUTY_PCT | 1 | openair1/PHY/TOOLS/nr_admit.c:33 | other |
| ISAC_BG_JANON_NICE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2974 | other |
| ISAC_BG_LANE | 2 | common/utils/nr_bg_lane.c:16 | other |
| ISAC_BG_MIN_DUTY_PCT | 1 | openair1/PHY/TOOLS/nr_admit.c:29 | other |
| ISAC_BWP_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6331 | diag |
| ISAC_BWP_TRACK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4371 | other |
| ISAC_CELL_STATE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:480 | diag |
| ISAC_CELL_STATE_SECS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:500 | diag |
| ISAC_CFO_TRACK_APPLY | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1816 | other |
| ISAC_CFO_TRACK_HZ | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1810 | other |
| ISAC_CFO_TRACK_PERIOD | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1817 | other |
| ISAC_CFO_TRACK_SPREAD_HZ | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1812 | other |
| ISAC_CHEST_COH | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1541 | other |
| ISAC_CHEST_DFT_APPLY | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1125 | other |
| ISAC_CHEST_DFT_WIN | 5 | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:76 | other |
| ISAC_CHEST_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4465 | diag |
| ISAC_CHEST_GPU_DELAY_CHECK | 3 | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1619 | other |
| ISAC_CHEST_OCC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3736 | other |
| ISAC_CHEST_OCC_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1990 | other |
| ISAC_CHEST_RAW | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1518 | other |
| ISAC_CHEST_STAGE_TIMING | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:64 | diag |
| ISAC_CHEST_TINTERP | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1103 | other |
| ISAC_CIR_TEST | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1121 | other |
| ISAC_COREMAP_EMITBAR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:843 | other |
| ISAC_COREMAP_GT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:674 | other |
| ISAC_COREMAP_GT_CSET | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:691 | other |
| ISAC_COREMAP_IDSWEEP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:443 | other |
| ISAC_COREMAP_OCCBAR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:863 | other |
| ISAC_COREMAP_ONACCEPT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:131 | other |
| ISAC_COREMAP_PHASE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:606 | other |
| ISAC_COREMAP_SUBBAND | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:81 | other |
| ISAC_COREMAP_Z | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:799 | other |
| ISAC_CORESET_DISC_MISS_S | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe_rt.c:246 | other |
| ISAC_CORESET_DISC_RECOVER_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe_rt.c:247 | other |
| ISAC_CSIRS_BLIND | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:729 | other |
| ISAC_CSIRS_BLIND_EPRDUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:865 | diag |
| ISAC_CSIRS_BLIND_IDSWEEP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1449 | other |
| ISAC_CSIRS_BLIND_PER_SLOT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1001 | other |
| ISAC_CSIRS_BLIND_RANK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:793 | other |
| ISAC_CSIRS_BLIND_RB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c:325 | other |
| ISAC_CSIRS_BLIND_ROWS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c:276 | other |
| ISAC_CSIRS_BLIND_SLOTSWEEP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1489 | other |
| ISAC_CSIRS_BLIND_WIDE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1192 | other |
| ISAC_CSIRS_DISC_BACKEND | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_backend.c:224 | other |
| ISAC_CSIRS_DISC_BUDGET | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_budget.cuh:29 | other |
| ISAC_CSIRS_DISC_CHUNK_UNITS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu:221 | other |
| ISAC_CSIRS_DISC_CHUNK_US | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu:223 | other |
| ISAC_CSIRS_DISC_CONFIRM_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:946 | other |
| ISAC_CSIRS_DISC_CORE | 3 | executables/nr-uesoftmodem.c:243 | other |
| ISAC_CSIRS_DISC_DELTA_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:153 | other |
| ISAC_CSIRS_DISC_DIAG | 4 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:481 | diag |
| ISAC_CSIRS_DISC_DUMP | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c:307 | diag |
| ISAC_CSIRS_DISC_FEED_ACQ_DIV | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:198 | other |
| ISAC_CSIRS_DISC_FEED_BURST | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:198 | other |
| ISAC_CSIRS_DISC_FEED_DIV | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:49 | other |
| ISAC_CSIRS_DISC_FEED_WIN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:49 | other |
| ISAC_CSIRS_DISC_MANAGED | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:235 | other |
| ISAC_CSIRS_DISC_RATELIMIT_CYCLES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:244 | other |
| ISAC_CSIRS_DISC_RATELIMIT_MISS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:242 | other |
| ISAC_CSIRS_DISC_RATELIMIT_NOTICE_S | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:241 | other |
| ISAC_CSIRS_DISC_RATELIMIT_PDET | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:243 | other |
| ISAC_CSIRS_DISC_RING | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:208 | other |
| ISAC_CSIRS_DISC_SWEEP_HYPS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:143 | other |
| ISAC_CSIRS_FULL_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:2069 | other |
| ISAC_CSIRS_FULL_AB_EVID | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:2023 | other |
| ISAC_CSIRS_FULL_VALID | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1898 | other |
| ISAC_CSI_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/csi_rx.c:1420 | diag |
| ISAC_CSS0_ALS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:396 | other |
| ISAC_CSS0_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5615 | other |
| ISAC_DCI11_DIAG_FILTER | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1620 | diag |
| ISAC_DCI11_OBSERVE_MM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9104 | other |
| ISAC_DCI11_PARSE_CACHE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1446 | other |
| ISAC_DCI11_PRUNELOG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1976 | diag |
| ISAC_DCI11_RV_CONSISTENCY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:398 | other |
| ISAC_DCI11_S1_OFF | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.c:774 | other |
| ISAC_DCI11_STAGE2 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1324 | other |
| ISAC_DCI_FIELDS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10645 | other |
| ISAC_DCI_LEN_MAX | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2320 | other |
| ISAC_DCI_LEN_MIN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2310 | other |
| ISAC_DCI_RAW_LOG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9086 | diag |
| ISAC_DCI_SWEEP_STRIDE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2398 | other |
| ISAC_DCI_WATCH_RNTI | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:5113 | other |
| ISAC_DCI_WIDE_PROBE_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:109 | other |
| ISAC_DC_FIX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3703 | other |
| ISAC_DELTA_SCAN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1020 | other |
| ISAC_DISCOVERY_EVIDENCE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2815 | other |
| ISAC_DISCOVER_DIAG | 14 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1899 | diag |
| ISAC_DISCOVER_DOMINANCE_K | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2579 | other |
| ISAC_DISCOVER_DOMINANCE_N | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2585 | other |
| ISAC_DISCOVER_MIN_BG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2531 | other |
| ISAC_DISC_NO_RESYNC | 1 | executables/nr-ue.c:1819 | other |
| ISAC_DMRS_ANCHOR | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1962 | other |
| ISAC_DMRS_FO_APPLY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4369 | other |
| ISAC_DMRS_NID_SOLVE | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:43 | other |
| ISAC_DMRS_NID_SOLVE_CORE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:250 | other |
| ISAC_DMRS_NID_SOLVE_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:43 | other |
| ISAC_DMRS_NID_SOLVE_GRACE_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:289 | other |
| ISAC_DMRS_NID_SOLVE_MAINTAIN_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:290 | other |
| ISAC_DMRS_NID_SOLVE_OPT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:46 | other |
| ISAC_DMRS_NID_SOLVE_RECHECK_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:291 | other |
| ISAC_FORCE_DCI_LEN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6648 | other |
| ISAC_FORCE_GLOBAL_SYNC | 2 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:171 | other |
| ISAC_GNB_PDCCH_DMRS_ID | 1 | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:331 | other |
| ISAC_GPU_CHECK_ASYNC_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:846 | other |
| ISAC_GPU_CHECK_ASYNC_QUEUE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:838 | other |
| ISAC_GPU_COPY_FENCE | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:41 | other |
| ISAC_GPU_DISC_SMS_LOW | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:117 | other |
| ISAC_GPU_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:386 | diag |
| ISAC_GPU_FORCE_DISCRETE | 1 | openair1/PHY/TOOLS/nr_gpu_mem.c:10 | other |
| ISAC_GPU_GREEN_LOG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:200 | diag |
| ISAC_GPU_HEARTBEAT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:847 | other |
| ISAC_GPU_LDPC_CL_FORCE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu:608 | other |
| ISAC_GPU_LDPC_NOCLUSTER | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu:598 | other |
| ISAC_GPU_MEMORY_MODEL | 1 | openair1/PHY/TOOLS/nr_gpu_platform.c:170 | other |
| ISAC_GPU_PASS | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:234 | other |
| ISAC_GPU_PASS_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:405 | other |
| ISAC_GPU_PASS_CLASSES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:314 | other |
| ISAC_GPU_PASS_FP32 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu:932 | other |
| ISAC_GPU_PASS_LOG_UNSEEN | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu:1192 | diag |
| ISAC_GPU_PASS_POLL_SLEEP_US | 1 | openair1/PHY/TOOLS/nr_gpu_poll.h:49 | other |
| ISAC_GPU_PASS_RETRY_OLD | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3224 | other |
| ISAC_GPU_PASS_SEG_CLASSES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:359 | other |
| ISAC_GPU_RT_DEADLINE_US | 1 | openair1/PHY/TOOLS/nr_gpu_budget.c:57 | other |
| ISAC_GPU_RT_PARTS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:100 | other |
| ISAC_GPU_SELFCHECK_ENFORCE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2810 | other |
| ISAC_GPU_SHARED_MANAGED | 1 | openair1/PHY/TOOLS/nr_gpu_platform.c:171 | other |
| ISAC_HIDDEN_MULTI_CORESET | 1 | openair1/SIMULATION/NR_PHY/hidden_waveform.h:89 | other |
| ISAC_HIDDEN_RECONFIG | 1 | openair1/SIMULATION/NR_PHY/hidden_waveform.h:92 | other |
| ISAC_HIDDEN_SEED | 1 | openair1/SIMULATION/NR_PHY/dlsim.c:784 | other |
| ISAC_HIDDEN_WAVEFORM_DIR | 1 | openair1/SIMULATION/NR_PHY/dlsim.c:783 | other |
| ISAC_L2MAXH_DELTA | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1888 | other |
| ISAC_L2MAXH_HEADROOM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1858 | other |
| ISAC_L2MAXH_PEAK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1773 | other |
| ISAC_LANE_ALS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2342 | other |
| ISAC_LANE_UNION_LLR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8045 | other |
| ISAC_LDPC_ITERS | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1873 | other |
| ISAC_LDPC_ITERS_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1999 | other |
| ISAC_LLRFILL_ALL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5618 | other |
| ISAC_LLR_NORM | 5 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:216 | other |
| ISAC_LLR_SCALE | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5296 | other |
| ISAC_MALLOC_NOTRIM | 1 | common/utils/system.c:401 | other |
| ISAC_MAP_PASS0_ONLY | 5 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:169 | other |
| ISAC_MAX_POS_ACC | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:49 | other |
| ISAC_METRICS_PATH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c:236 | diag |
| ISAC_MLOCK | 1 | common/utils/system.c:407 | other |
| ISAC_MMSE2_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4886 | diag |
| ISAC_MMSE_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1086 | diag |
| ISAC_MMSE_FLOAT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1025 | other |
| ISAC_OBS_PATH | 1 | executables/nr-uesoftmodem.c:432 | other |
| ISAC_OBS_TEST_RING_SLOTS | 1 | executables/nr-uesoftmodem.c:434 | other |
| ISAC_OBS_TEST_WRITER_PAUSE_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:183 | other |
| ISAC_OTA_CFG | 2 | openair2/LAYER2/NR_MAC_UE/config_ue.c:1816 | other |
| ISAC_PASSIVE_REPLAY_CAPTURE | 2 | executables/nr-uesoftmodem.c:648 | other |
| ISAC_PASSIVE_REPLAY_FAILURES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:125 | other |
| ISAC_PASSIVE_REPLAY_INPUT | 3 | executables/nr-uesoftmodem.c:182 | other |
| ISAC_PASSIVE_REPLAY_UL_CONFIG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:319 | other |
| ISAC_PASSIVE_REPLAY_UL_PROBE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:318 | other |
| ISAC_PASSIVE_REPLAY_UL_UCI | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:3017 | other |
| ISAC_PBCH_CAPTURE | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1894 | other |
| ISAC_PBCH_CAPTURE_PATH | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1899 | other |
| ISAC_PBCH_CFO_SWEEP | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1271 | other |
| ISAC_PBCH_L2_DELTA | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1683 | other |
| ISAC_PBCH_NO_TRACK | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:2174 | other |
| ISAC_PBCH_OFFSET_BIAS | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:989 | other |
| ISAC_PBCH_REPLAY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:572 | other |
| ISAC_PBCH_SHIFT_CLAMP | 1 | executables/nr-ue.c:2013 | other |
| ISAC_PDCCH_ANON_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3416 | diag |
| ISAC_PDCCH_ANON_DUMP_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3417 | diag |
| ISAC_PDCCH_ANT_MASK | 2 | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:770 | other |
| ISAC_PDCCH_C0_OCC_DB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:343 | other |
| ISAC_PDCCH_CAPTURE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6153 | other |
| ISAC_PDCCH_CFGTRACE | 1 | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:456 | diag |
| ISAC_PDCCH_CFGTRACE_SLOT | 1 | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:459 | diag |
| ISAC_PDCCH_CSS_NRNTI0 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2868 | other |
| ISAC_PDCCH_DCIGT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9618 | other |
| ISAC_PDCCH_DISCOVERED_FILE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1349 | other |
| ISAC_PDCCH_DISCOVERY_BUDGET_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2592 | other |
| ISAC_PDCCH_DISCOVERY_REPLAY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3658 | other |
| ISAC_PDCCH_DISCOVERY_TRACE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2607 | diag |
| ISAC_PDCCH_DISC_ENUM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1189 | other |
| ISAC_PDCCH_DISC_ENUM_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1219 | other |
| ISAC_PDCCH_DISC_ENUM_NIDS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1219 | other |
| ISAC_PDCCH_DMRS_GATE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7496 | other |
| ISAC_PDCCH_DMRS_RANK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3552 | other |
| ISAC_PDCCH_DWELL_ADAPTIVE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1487 | other |
| ISAC_PDCCH_DWELL_PRESCREEN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:833 | other |
| ISAC_PDCCH_DWELL_PRESCREEN_Z | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:843 | other |
| ISAC_PDCCH_ENERGY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7591 | other |
| ISAC_PDCCH_EXTENT_BATCH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1787 | other |
| ISAC_PDCCH_FULLCRC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:4210 | other |
| ISAC_PDCCH_GATE_CFAR_Z | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7622 | other |
| ISAC_PDCCH_GPU_CPU_ARITH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.cu:204 | other |
| ISAC_PDCCH_GPU_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_record.c:18 | diag |
| ISAC_PDCCH_GPU_DUMP_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_record.c:19 | diag |
| ISAC_PDCCH_GPU_EXACT_FEP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:180 | other |
| ISAC_PDCCH_GPU_FEP | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:11135 | other |
| ISAC_PDCCH_JOINT_ANON_BUDGET_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3493 | other |
| ISAC_PDCCH_JOINT_ANON_CORE | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2964 | other |
| ISAC_PDCCH_JOINT_ANON_MM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3469 | other |
| ISAC_PDCCH_JOINT_ANON_OCC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3457 | other |
| ISAC_PDCCH_JOINT_ANON_Q | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3169 | other |
| ISAC_PDCCH_JOINT_ANON_TOPK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3061 | other |
| ISAC_PDCCH_KNOWN_PRESCREEN_MIN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7657 | other |
| ISAC_PDCCH_LEN_MEMORY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6904 | other |
| ISAC_PDCCH_LLRPROBE | 1 | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1178 | other |
| ISAC_PDCCH_LLRPROBE_SLOT | 1 | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1185 | other |
| ISAC_PDCCH_LLR_CACHE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6160 | other |
| ISAC_PDCCH_LLR_CACHE_VERIFY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6188 | other |
| ISAC_PDCCH_NEON_SCORE | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3505 | other |
| ISAC_PDCCH_NID_GPU_ACQUIRE_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:293 | other |
| ISAC_PDCCH_NID_GPU_BATCH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:302 | other |
| ISAC_PDCCH_NID_GPU_BUDGET_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.cu:565 | other |
| ISAC_PDCCH_NID_GPU_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:303 | other |
| ISAC_PDCCH_NID_GPU_INCREMENTAL | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:460 | other |
| ISAC_PDCCH_NID_GPU_MAINTAIN_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:294 | other |
| ISAC_PDCCH_NID_GPU_NODROP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:295 | other |
| ISAC_PDCCH_NID_GPU_OCC_DB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:512 | other |
| ISAC_PDCCH_NID_GPU_OCC_FLOOR_PCT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:517 | other |
| ISAC_PDCCH_NID_GPU_OCC_GATE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:510 | other |
| ISAC_PDCCH_NID_GPU_SYMBOLS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:478 | other |
| ISAC_PDCCH_NID_GPU_WINDOW | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:304 | other |
| ISAC_PDCCH_NID_MULTISYMBOL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.cu:571 | other |
| ISAC_PDCCH_NO_FAKE_FOOTPRINT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1527 | diag |
| ISAC_PDCCH_NO_MISMATCH_GATE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9448 | other |
| ISAC_PDCCH_NO_PDSCH_DECODE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10629 | other |
| ISAC_PDCCH_NRNTI | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1058 | other |
| ISAC_PDCCH_PAR_BANKS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5057 | other |
| ISAC_PDCCH_PAR_CORE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5022 | other |
| ISAC_PDCCH_PASS_ADAPT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:758 | other |
| ISAC_PDCCH_PRESCREEN_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:858 | diag |
| ISAC_PDCCH_SCAN_EVERY_SLOT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4817 | other |
| ISAC_PDCCH_SHARED_DMRS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:446 | other |
| ISAC_PDCCH_SHARED_DMRS_MIN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:447 | other |
| ISAC_PDCCH_SHARED_PRESCREEN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6493 | other |
| ISAC_PDCCH_SHARED_PRESCREEN_AUDIT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:433 | other |
| ISAC_PDCCH_SHARED_PRESCREEN_MIN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6508 | other |
| ISAC_PDCCH_SIM_SPARSE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:869 | other |
| ISAC_PDCCH_TASK_CAP64 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7454 | other |
| ISAC_PDCCH_ULDCIGT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8917 | other |
| ISAC_PDCCH_USS_TRACKER | 2 | executables/nr-uesoftmodem.c:223 | other |
| ISAC_PDSCHQ_REAL_FIRST | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1274 | other |
| ISAC_PDSCHQ_SIGNAL_TIME | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1491 | other |
| ISAC_PDSCH_ANT_GATE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4755 | other |
| ISAC_PDSCH_CDM_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3947 | diag |
| ISAC_PDSCH_CPU_SMALL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:26 | other |
| ISAC_PDSCH_CPU_SMALL_NL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:28 | other |
| ISAC_PDSCH_CSI_EXTENT_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5984 | other |
| ISAC_PDSCH_CSI_RB_EXTENT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:250 | other |
| ISAC_PDSCH_DECODE_DIAG | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2168 | diag |
| ISAC_PDSCH_DMRS_ID2_ASYNC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1399 | other |
| ISAC_PDSCH_EVM | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5026 | other |
| ISAC_PDSCH_EVM_RB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5685 | other |
| ISAC_PDSCH_GPU_BATCH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:30 | other |
| ISAC_PDSCH_GPU_BATCH_ASYNC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:836 | other |
| ISAC_PDSCH_GPU_BATCH_SLOTS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:31 | other |
| ISAC_PDSCH_GPU_CHEST_TRIM | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:1477 | other |
| ISAC_PDSCH_GPU_CSI_DISJOINT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:63 | other |
| ISAC_PDSCH_GPU_CTX_LIMIT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:228 | other |
| ISAC_PDSCH_GPU_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:106 | diag |
| ISAC_PDSCH_GPU_DUMP_FULL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:109 | diag |
| ISAC_PDSCH_GPU_DUMP_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:107 | diag |
| ISAC_PDSCH_GPU_EXACT_DEADLINE_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:832 | other |
| ISAC_PDSCH_GPU_EXACT_DEMOD_EVM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:644 | other |
| ISAC_PDSCH_GPU_EXACT_DEMOD_PROF | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu:1494 | other |
| ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:1224 | other |
| ISAC_PDSCH_GPU_EXACT_DIRECT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:830 | other |
| ISAC_PDSCH_GPU_EXACT_LDPC_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:74 | other |
| ISAC_PDSCH_GPU_GRAPH | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:235 | other |
| ISAC_PDSCH_GPU_GRAPH_MB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:286 | other |
| ISAC_PDSCH_GPU_GRAPH_PROBE_RESERVE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu:1085 | other |
| ISAC_PDSCH_GPU_GRAPH_SPLIT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:815 | other |
| ISAC_PDSCH_GPU_LDPC | 1 | openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface_load.c:34 | other |
| ISAC_PDSCH_GPU_PRIO | 4 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:76 | other |
| ISAC_PDSCH_GPU_STAGE_DUMP | 3 | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2216 | diag |
| ISAC_PDSCH_HARQ_COMBINE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:143 | other |
| ISAC_PDSCH_K0_PROBE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1871 | other |
| ISAC_PDSCH_LLR_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5207 | diag |
| ISAC_PDSCH_LLR_DUMP_ALL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5199 | diag |
| ISAC_PDSCH_LLR_DUMP_FULL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5198 | diag |
| ISAC_PDSCH_LLR_DUMP_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5207 | diag |
| ISAC_PDSCH_MMSE2_GAIN | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:936 | other |
| ISAC_PDSCH_MMSE2_NL34_QPSK | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1431 | other |
| ISAC_PDSCH_MMSE_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6066 | other |
| ISAC_PDSCH_MMSE_AB_QPSK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6078 | other |
| ISAC_PDSCH_MMSE_EXACT_CAL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:837 | other |
| ISAC_PDSCH_MMSE_EXACT_NVAR_DB | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:851 | other |
| ISAC_PDSCH_OWN_PDCCH_DB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6158 | other |
| ISAC_PDSCH_OWN_PDCCH_RM | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2597 | other |
| ISAC_PDSCH_PDCCH_OCC_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2366 | diag |
| ISAC_PDSCH_PDCCH_OCC_IDS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2360 | other |
| ISAC_PDSCH_PIN_PREWARM_ALL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:296 | other |
| ISAC_PDSCH_PROBE_C1 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1817 | other |
| ISAC_PDSCH_RAW_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3990 | diag |
| ISAC_PDSCH_RAW_DUMP_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3990 | diag |
| ISAC_PDSCH_RAW_DUMP_MINNL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3990 | diag |
| ISAC_PDSCH_READING_RV | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9931 | other |
| ISAC_PDSCH_RECOVERY_CORES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:133 | other |
| ISAC_PDSCH_RECOVERY_Q | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:231 | other |
| ISAC_PDSCH_RECOVERY_SNAP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2944 | other |
| ISAC_PDSCH_RETRY_BUDGET_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6454 | other |
| ISAC_PDSCH_RETRY_SNR_MIN_DB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6610 | other |
| ISAC_PDSCH_RM_GENIE | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:4890 | other |
| ISAC_PDSCH_RM_GENIE_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_rm_genie.c:38 | other |
| ISAC_PDSCH_SIGNAL_GEOM_RETRY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6365 | other |
| ISAC_PDSCH_SLOT_TIMING | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1477 | diag |
| ISAC_PDSCH_SNR_GATE_DB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1353 | other |
| ISAC_PDSCH_SOLVER_DIAG | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10022 | diag |
| ISAC_PDSCH_SOLVER_FAIR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:824 | other |
| ISAC_PDSCH_SOLVER_LEADER | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:840 | other |
| ISAC_PDSCH_SOLVER_MAX_LIVE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9835 | other |
| ISAC_PDSCH_SOLVER_PREF | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:858 | other |
| ISAC_PDSCH_SOLVER_PREF_MCS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:859 | other |
| ISAC_PDSCH_STATIC_CSI_RETRY | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6118 | diag |
| ISAC_PDSCH_TBPARM | 4 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1918 | other |
| ISAC_PDSCH_TINTERP_RETRY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6580 | other |
| ISAC_PDSCH_TINT_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6023 | other |
| ISAC_PDSCH_TYPEB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:186 | other |
| ISAC_PHY_DIAG | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1493 | diag |
| ISAC_PRACH_MARGIN_DB10 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_prach_passive.c:118 | other |
| ISAC_PRG_SWEEP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:721 | other |
| ISAC_PROBE_ALL | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1722 | other |
| ISAC_PROBE_HORIZON | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3189 | other |
| ISAC_PSS_PEAK_RATIO | 1 | openair1/PHY/NR_UE_TRANSPORT/pss_nr.c:216 | other |
| ISAC_PTRS_K | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2821 | other |
| ISAC_PTRS_L | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2821 | other |
| ISAC_PUSCH_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:467 | diag |
| ISAC_PUSCH_SEARCH_PRIO | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:354 | other |
| ISAC_PUSCH_TIMING | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:475 | diag |
| ISAC_QM_ORACLE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:3004 | other |
| ISAC_RAW_IQ_DECIM | 1 | radio/COMMON/raw_iq.c:185 | other |
| ISAC_RAW_IQ_DIRECTORY | 1 | radio/COMMON/raw_iq.c:176 | other |
| ISAC_RAW_IQ_GAP2_AT_S | 1 | radio/COMMON/raw_iq.c:215 | other |
| ISAC_RAW_IQ_GAP2_S | 1 | radio/COMMON/raw_iq.c:215 | other |
| ISAC_RAW_IQ_GAP_AT_S | 1 | radio/COMMON/raw_iq.c:209 | other |
| ISAC_RAW_IQ_GAP_S | 1 | radio/COMMON/raw_iq.c:209 | other |
| ISAC_RAW_IQ_SPEED | 1 | radio/COMMON/raw_iq.c:181 | other |
| ISAC_RAW_IQ_WANDER | 1 | radio/COMMON/raw_iq.c:188 | other |
| ISAC_RAW_IQ_WANDER_UNTIL_S | 1 | radio/COMMON/raw_iq.c:191 | other |
| ISAC_RECONF_DISCOVERY_DUTY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4690 | other |
| ISAC_RECONF_GAP_HARD_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:436 | other |
| ISAC_RECONF_N_SUSPECT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:243 | other |
| ISAC_RECONF_SIB1_MAX_OCCASIONS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_sources.c:66 | other |
| ISAC_RECONF_SI_BUMP_WITHOUT_SIB1 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:100 | other |
| ISAC_RECONF_SI_GRACE_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:98 | other |
| ISAC_RF_FAIL_AFTER_READS | 1 | executables/nr-ue-ru.c:378 | other |
| ISAC_RF_STALL_MAX_REINIT | 1 | executables/nr-ue.c:2345 | other |
| ISAC_RF_STALL_WATCHDOG | 1 | executables/nr-ue.c:2071 | other |
| ISAC_RNTI_LAYOUT_CACHE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:1757 | other |
| ISAC_RNTI_LAYOUT_CACHE_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9885 | other |
| ISAC_RNTI_LAYOUT_CACHE_FAILS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9873 | other |
| ISAC_RV_RETRY | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5435 | other |
| ISAC_RX_BEST_BY_SNR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1648 | other |
| ISAC_RX_BRANCH | 3 | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1819 | other |
| ISAC_RX_BRANCH_FO | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1558 | other |
| ISAC_RX_BRANCH_MIN_DB | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1379 | other |
| ISAC_RX_CHAN_MAP | 1 | radio/USRP/usrp_lib.cpp:2448 | other |
| ISAC_RX_EQ_AB | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4707 | other |
| ISAC_RX_GAIN_TRIM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ue_rf_helpers.c:49 | other |
| ISAC_RX_IRC | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2029 | other |
| ISAC_RX_IRC_SHRINK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2030 | other |
| ISAC_RX_ML_2L | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1403 | other |
| ISAC_RX_MRC_PER_RNTI_AB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:727 | other |
| ISAC_RX_MRC_PER_RNTI_SNR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:740 | other |
| ISAC_RX_MRC_SHIFT_SUM | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1836 | other |
| ISAC_RX_NVAR_FIX | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2556 | other |
| ISAC_RX_NVAR_PERBRANCH | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2563 | other |
| ISAC_RX_QAM_AGC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2399 | other |
| ISAC_RX_RECOVER_BACKOFF_MAX_MS | 1 | radio/USRP/usrp_lib.cpp:1163 | other |
| ISAC_RX_RECOVER_BACKOFF_MS | 1 | radio/USRP/usrp_lib.cpp:1162 | other |
| ISAC_RX_RECOVER_LEAD_MS | 1 | radio/USRP/usrp_lib.cpp:1164 | other |
| ISAC_RX_RECOVER_MAX_FAULTS | 1 | radio/USRP/usrp_lib.cpp:1056 | other |
| ISAC_RX_RECOVER_NOCTRL | 1 | radio/USRP/usrp_lib.cpp:1075 | other |
| ISAC_RX_RECOVER_TEST_COARSE_EPOCH | 1 | executables/nr-ue.c:283 | other |
| ISAC_RX_RECOVER_WINDOW_S | 1 | radio/USRP/usrp_lib.cpp:1064 | other |
| ISAC_SCAN2_AUDIT_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7344 | other |
| ISAC_SCAN2_ULSSH_AUDIT_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7880 | other |
| ISAC_SCAN2_UL_Z | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7348 | other |
| ISAC_SCAN_CONFIRM | 1 | executables/nr-ue.c:1122 | other |
| ISAC_SCAN_SCOREMEMO_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:186 | other |
| ISAC_SCAN_SCRATCH_MB | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:752 | other |
| ISAC_SFO_CORRECT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4815 | other |
| ISAC_SHIFT_CENSUS | 1 | executables/nr-ue.c:1705 | other |
| ISAC_SHUTDOWN_TIMEOUT_S | 1 | common/utils/nr_shutdown.c:210 | other |
| ISAC_SIB1_CACHE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:6083 | other |
| ISAC_SIB1_CACHE_DIR | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:6071 | other |
| ISAC_SIB1_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_decoding.c:245 | diag |
| ISAC_SIB1_SIBLING | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:325 | other |
| ISAC_SIM_CANDIDATE_RNTI | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c:252 | other |
| ISAC_SIM_CANDIDATE_RNTIS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c:250 | other |
| ISAC_SI_SOFTCOMBINE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:152 | other |
| ISAC_SRS_CPU_PCT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_srs_passive.c:265 | other |
| ISAC_SSB_IQ | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1006 | other |
| ISAC_SUBSET_DIAG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1720 | diag |
| ISAC_SWEEP_OCCASIONS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2297 | other |
| ISAC_SYNC_ANT_MASK | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:95 | other |
| ISAC_TARGET_PCI | 1 | executables/nr-ue.c:211 | other |
| ISAC_TB_LOG_WIDTHS | 1 | openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c:3171 | diag |
| ISAC_TB_PUSCH_IDS | 1 | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:1676 | other |
| ISAC_TB_PUSCH_MCSTAB | 1 | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:1688 | other |
| ISAC_TB_PUSCH_TP | 2 | openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_phytest.c:330 | other |
| ISAC_TB_UL_VARY | 1 | openair2/LAYER2/NR_MAC_gNB/gNB_scheduler_phytest.c:254 | other |
| ISAC_TDD_SKIP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5519 | other |
| ISAC_TD_CB0_ELIM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:1288 | other |
| ISAC_TD_CB0_GPU_AUTOREG | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.c:72 | other |
| ISAC_TD_CB0_GPU_BACKOFF | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_backend.c:107 | other |
| ISAC_TD_CB0_GPU_ITERS | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_gpu_backend.c:27 | other |
| ISAC_TD_CERT_CONFIRMED | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c:386 | other |
| ISAC_TD_DCI_ADJ | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c:313 | other |
| ISAC_TD_FIELDBOOK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:1855 | other |
| ISAC_TD_GRANTWORK | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:11150 | other |
| ISAC_TD_GW_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7425 | other |
| ISAC_TD_GW_PROBE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1946 | other |
| ISAC_TD_GW_PROFILE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7550 | other |
| ISAC_TD_IGNORE_SIB1 | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:82 | other |
| ISAC_TD_K0_ORACLE_LEGACY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:446 | other |
| ISAC_TD_PROBE_EQUIV_BRANCH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7650 | other |
| ISAC_TD_PROBE_EQUIV_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7618 | other |
| ISAC_TD_TB_CPU_WHILE_ACQ | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_gpu_backend.c:39 | other |
| ISAC_TSYNC_AUDIT | 5 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:157 | other |
| ISAC_TSYNC_CRC_GATE | 1 | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1946 | other |
| ISAC_TSYNC_DUMP | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:120 | diag |
| ISAC_TSYNC_GLOBAL_REBASE | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:252 | other |
| ISAC_TSYNC_MEASURE | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:295 | other |
| ISAC_TSYNC_MIN_EWIN | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:290 | other |
| ISAC_TSYNC_REBASE_RATIO | 1 | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:261 | other |
| ISAC_TSYNC_RESET | 2 | executables/nr-ue.c:1449 | other |
| ISAC_UECTX_PATH | 1 | executables/nr-uesoftmodem.c:419 | other |
| ISAC_UECTX_PERIOD_S | 1 | executables/nr-uesoftmodem.c:422 | other |
| ISAC_UECTX_TEST_WRITER_PAUSE_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.c:510 | other |
| ISAC_UE_RT_CORE | 1 | executables/nr-ue.c:2518 | other |
| ISAC_UHD_CTRL_THREADS | 1 | executables/nr-uesoftmodem.c:213 | other |
| ISAC_UL_BGDISC_CORE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c:70 | other |
| ISAC_UL_BGDISC_MAX_AGE_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c:120 | other |
| ISAC_UL_CONFIG_DISC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_config_disc.c:140 | other |
| ISAC_UL_CPU_BUDGET_PCT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:851 | other |
| ISAC_UL_DBG_LOG | 2 | executables/nr-ue.c:1757 | diag |
| ISAC_UL_DCI_BANK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_config_disc.c:83 | other |
| ISAC_UL_DISC_EVERY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_disc.c:315 | other |
| ISAC_UL_DMRS_DISC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1998 | other |
| ISAC_UL_FDRA_REFUSE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.c:225 | other |
| ISAC_UL_FORCE_IDS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:165 | other |
| ISAC_UL_FORCE_OPTS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1715 | other |
| ISAC_UL_GPU_FEP_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1036 | other |
| ISAC_UL_GPU_FEP_DEADLINE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_fep.cuh:199 | other |
| ISAC_UL_GPU_FEP_PCT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1040 | other |
| ISAC_UL_GPU_LDPC_DEADLINE_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:391 | other |
| ISAC_UL_GPU_LDPC_MIN_BITS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:397 | other |
| ISAC_UL_GPU_LDPC_PCT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:391 | other |
| ISAC_UL_GPU_PASS | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:234 | other |
| ISAC_UL_GPU_PASS_CHECK | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:549 | other |
| ISAC_UL_GPU_PASS_CLASSES | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:530 | other |
| ISAC_UL_GPU_PASS_SEED | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:353 | other |
| ISAC_UL_GPU_PRIO | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_diag.h:28 | other |
| ISAC_UL_HARQ_MAX_SEG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:230 | other |
| ISAC_UL_HYP_MAX_AGE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:129 | other |
| ISAC_UL_LAYOUT_BRUTE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:563 | other |
| ISAC_UL_LAYOUT_BRUTE_IDS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:569 | other |
| ISAC_UL_LAYOUT_CONTROL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1555 | other |
| ISAC_UL_LLR_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:44 | diag |
| ISAC_UL_LLR_DUMP_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:52 | diag |
| ISAC_UL_LLR_DUMP_MAX_MB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:54 | diag |
| ISAC_UL_LLR_DUMP_MCS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:50 | diag |
| ISAC_UL_LLR_DUMP_MIN_RB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:51 | diag |
| ISAC_UL_LLR_DUMP_OK_MAX | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:53 | diag |
| ISAC_UL_LLR_DUMP_SNR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:49 | diag |
| ISAC_UL_LOCK_CLIP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1383 | other |
| ISAC_UL_LOCK_COHERENT | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:412 | other |
| ISAC_UL_LOCK_EDGE | 3 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:411 | other |
| ISAC_UL_LOCK_LLR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:710 | other |
| ISAC_UL_LOCK_NO_TRIAL_UNLOCKED | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1584 | other |
| ISAC_UL_LOCK_PRIOR_DWELL | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1880 | other |
| ISAC_UL_LOCK_PRIOR_SEARCH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1872 | other |
| ISAC_UL_LOCK_SPRT | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:415 | other |
| ISAC_UL_LOOKALIKE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:718 | other |
| ISAC_UL_MCS_TABLE | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:222 | other |
| ISAC_UL_NID_BG_ADDPOS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:96 | other |
| ISAC_UL_NID_BG_CORE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:105 | other |
| ISAC_UL_NID_BG_DBG_TRY | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:94 | other |
| ISAC_UL_NID_BG_DROP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:103 | other |
| ISAC_UL_NID_BG_GAP_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:93 | other |
| ISAC_UL_NID_BG_HYP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:102 | other |
| ISAC_UL_NID_BG_JOB_MS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:101 | other |
| ISAC_UL_NID_BG_LOG | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:104 | diag |
| ISAC_UL_NID_BG_MCS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:108 | other |
| ISAC_UL_NID_BG_MIN_RB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:91 | other |
| ISAC_UL_NID_BG_NBWP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:107 | other |
| ISAC_UL_NID_BG_QM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:90 | other |
| ISAC_UL_NID_BG_REPEAT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:92 | other |
| ISAC_UL_NID_BG_SNR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:89 | other |
| ISAC_UL_NID_BG_TOPM | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:95 | other |
| ISAC_UL_NID_BG_TRIALS | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:97 | other |
| ISAC_UL_PIPELINE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:264 | other |
| ISAC_UL_PROBE | 1 | executables/nr-ue.c:1649 | other |
| ISAC_UL_PROBE_DEPTH | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:845 | other |
| ISAC_UL_PROBE_TA | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:413 | other |
| ISAC_UL_RETRY_MAX_RB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:243 | other |
| ISAC_UL_RETRY_SNR_DB | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:218 | other |
| ISAC_UL_RX_BRANCH | 3 | openair1/PHY/NR_ESTIMATION/nr_ul_channel_estimation.c:664 | other |
| ISAC_UL_SPRT_DUMP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1434 | diag |
| ISAC_UL_STALL_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_diag.h:60 | other |
| ISAC_UL_TA_SWEEP | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:411 | other |
| ISAC_UL_TEST_RT_LOAD | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_fep.cuh:148 | other |
| ISAC_UL_TIMING_DIAG | 1 | openair1/PHY/NR_ESTIMATION/nr_ul_channel_estimation.c:667 | diag |
| ISAC_UL_TIMING_DISC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1996 | diag |
| ISAC_UL_UCI_BUDGET_US | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:306 | other |
| ISAC_UL_UE_STATE | 4 | executables/nr-uesoftmodem.c:418 | diag |
| ISAC_USRP_ISOLATION_CHECK | 1 | radio/USRP/usrp_lib.cpp:1448 | other |
| ISAC_USRP_RESTART_MAX | 1 | radio/USRP/usrp_lib.cpp:1013 | other |
| ISAC_USRP_RESTART_UNTIL_SYNC | 1 | radio/USRP/usrp_lib.cpp:1104 | other |
| ISAC_USRP_RING_CORE | 2 | executables/nr-uesoftmodem.c:203 | other |
| ISAC_USRP_RING_MS | 2 | executables/nr-uesoftmodem.c:203 | other |
| ISAC_USRP_RING_PKTS | 1 | radio/USRP/usrp_lib.cpp:1138 | other |
| ISAC_USRP_RING_POLL | 1 | radio/USRP/usrp_lib.cpp:1147 | other |
| ISAC_USRP_RING_POLL_TIMEOUT_MS | 1 | radio/USRP/usrp_lib.cpp:1148 | other |
| ISAC_USRP_RING_PRIO | 1 | radio/USRP/usrp_lib.cpp:1127 | other |
| ISAC_USRP_RING_TRAFFIC_METRICS | 2 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c:55 | diag |
| ISAC_XANT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3490 | other |
| LDPC_BENCH | 3 | openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_decoder.c:30 | other |
| LDPC_CB0_BREAKER_N | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:522 | other |
| LDPC_CB0_BREAKER_S | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:524 | other |
| LDPC_CB0_CHUNK | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:530 | other |
| LDPC_CB0_GRAPH | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:531 | other |
| LDPC_CB0_MAX_ITEMS | 2 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:528 | other |
| LDPC_CB0_MEM | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:546 | other |
| LDPC_CB0_NCW | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:606 | other |
| LDPC_CB0_SLOTS | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:529 | other |
| LDPC_CB0_TEST_INJECT | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:526 | other |
| LDPC_CB0_TEST_STALL_MS | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:527 | other |
| LDPC_CB0_TIMEOUT_MS | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:518 | other |
| LDPC_CB0_TIMEOUT_US_PER_ITEM | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:520 | other |
| LDPC_CB0_WORK_CW | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:555 | other |
| LDPC_CUDA_BREAKER_N | 2 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:522 | other |
| LDPC_CUDA_BREAKER_S | 2 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:524 | other |
| LDPC_CUDA_GRAPH | 2 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:531 | other |
| LDPC_CUDA_TEST_INJECT | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1050 | other |
| LDPC_CUDA_TEST_QUEUE_CAP | 1 | openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:171 | other |
| LDPC_CUDA_TEST_SKIP_LAUNCH | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1048 | other |
| LDPC_CUDA_TEST_STALL_MS | 1 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1049 | other |
| LDPC_CUDA_TIMEOUT_MS | 2 | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:518 | other |
| LDPC_CUDA_WARMUP_ITERS | 1 | openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:388 | other |
| NR_GPU_CB0 | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_batch.c:844 | other |
| NR_GPU_FORCE_ANT | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:770 | other |
| NR_GPU_LLR_SCALE | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:768 | other |
| NR_GPU_POLAR | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_polar_gpu.h:54 | other |
| NR_TD_CB0_CUDA_LDPC | 1 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_batch.c:848 | other |
| OAI_CONFIGMODULE | 1 | common/config/config_load_configmodule.c:252 | other |
| OAI_GDBSTACKS | 1 | common/utils/assertions.h:19 | other |
| OAI_RNGSEED | 1 | openair1/SIMULATION/TOOLS/rangen_double.c:40 | other |
| UHD_CONFIG_FILE | 1 | common/utils/nr_core_plan.c:247 | other |
| USRP_DECIM_WORKERS | 1 | radio/USRP/usrp_lib.cpp:3315 | other |
