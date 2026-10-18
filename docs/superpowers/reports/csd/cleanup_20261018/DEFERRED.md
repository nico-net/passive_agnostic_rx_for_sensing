# DEFERRED / DO NOT USE (2026-10-18)

Every environment variable the receiver reads that is NOT in the frozen config t26zE. Each getenv()/wrapper site carries the comment
`/* DEFERRED/DO NOT USE: <FLAG> not in the frozen config t26zE (2026-10-18); slated for removal */` directly above it (comment-only change; behaviour unchanged).
Source list: CLEANUP-20261018-inventory.md section 3 (class C), minus the 5 flags later added to t26zE (ISAC_GPU_PASS_FP32, ISAC_GPU_PASS_POLL_SLEEP_US, ISAC_GPU_PASS_RETRY_OLD, ISAC_PDSCH_GPU_GRAPH, ISAC_PDSCH_GPU_GRAPH_SPLIT). Site line = line of the marker in the file at commit time.
Not marked: function-head markers (none added: no helper verified as reachable only through one flag); ISAC_TB_UL_VARY (no string-literal read site found); flags read through composed names; test sources.

Flags marked: 561, sites: 729, files: 114

| flag | marker sites (file:line) |
|---|---|
| ISAC_A2E_TRACE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:3061 |
| ISAC_ACQ_CARRIER_FROM_RS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:265 |
| ISAC_ACQ_CFO_MAX_HZ | openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:671 |
| ISAC_ACQ_PROBE | openair2/LAYER2/NR_MAC_UE/config_ue.c:2401 |
| ISAC_AGNOSTIC_V2 | openair1/PHY/NR_UE_TRANSPORT/nr_agnostic_v2.h:12 |
| ISAC_AL1_COVER | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1765 |
| ISAC_ANCHOR_ENTER | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1982 |
| ISAC_ANCHOR_SIGN | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1991 |
| ISAC_ANCHOR_STAY | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1985 |
| ISAC_ANONLEN_AUDIT_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6942 |
| ISAC_ANONLEN_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6993 |
| ISAC_ANONLEN_MIN_RNTI | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6991 |
| ISAC_ANONLEN_Z | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6939 |
| ISAC_AUTO_ACQUIRE | executables/nr-ue.c:1111, openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:663, openair2/LAYER2/NR_MAC_UE/config_ue.c:2397 |
| ISAC_BANDSCAN_FIRST_HZ | executables/nr-ue-ru.c:158 |
| ISAC_BG_CORES | common/utils/nr_bg_lane.c:22 |
| ISAC_BG_JANON_BACKLOG | openair1/PHY/TOOLS/nr_admit.c:35 |
| ISAC_BG_JANON_DUTY_PCT | openair1/PHY/TOOLS/nr_admit.c:36 |
| ISAC_BG_JANON_NICE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3002 |
| ISAC_BG_LANE | common/utils/nr_bg_lane.c:16, openair1/PHY/TOOLS/nr_admit.c:29 |
| ISAC_BG_MIN_DUTY_PCT | openair1/PHY/TOOLS/nr_admit.c:30 |
| ISAC_BWP_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6384 |
| ISAC_BWP_TRACK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4410 |
| ISAC_CELL_STATE | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:482 |
| ISAC_CELL_STATE_SECS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:503 |
| ISAC_CFO_TRACK_APPLY | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1826 |
| ISAC_CFO_TRACK_HZ | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1818 |
| ISAC_CFO_TRACK_PERIOD | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1828 |
| ISAC_CFO_TRACK_SPREAD_HZ | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1821 |
| ISAC_CHEST_COH | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1546 |
| ISAC_CHEST_DFT_APPLY | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1128 |
| ISAC_CHEST_DFT_WIN | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:77, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1122, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1631, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2241, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2368 |
| ISAC_CHEST_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4499 |
| ISAC_CHEST_GPU_DELAY_CHECK | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1623, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2216, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2343 |
| ISAC_CHEST_OCC | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:208, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3760 |
| ISAC_CHEST_OCC_AB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2001 |
| ISAC_CHEST_RAW | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1522 |
| ISAC_CHEST_STAGE_TIMING | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:64 |
| ISAC_CHEST_TINTERP | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:209, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1107, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1774, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4056 |
| ISAC_CIR_TEST | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1122 |
| ISAC_COREMAP_EMITBAR | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:856 |
| ISAC_COREMAP_GT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:684 |
| ISAC_COREMAP_GT_CSET | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:702 |
| ISAC_COREMAP_IDSWEEP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:445 |
| ISAC_COREMAP_OCCBAR | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:877 |
| ISAC_COREMAP_ONACCEPT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:132 |
| ISAC_COREMAP_PHASE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:614 |
| ISAC_COREMAP_SUBBAND | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:81 |
| ISAC_COREMAP_Z | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:811 |
| ISAC_CORESET_DISC_MISS_S | openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe_rt.c:247 |
| ISAC_CORESET_DISC_RECOVER_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_dl_traffic_probe_rt.c:249 |
| ISAC_CSIRS_BLIND | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:729 |
| ISAC_CSIRS_BLIND_EPRDUMP | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:867 |
| ISAC_CSIRS_BLIND_IDSWEEP | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1455 |
| ISAC_CSIRS_BLIND_PER_SLOT | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1005 |
| ISAC_CSIRS_BLIND_RANK | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:794 |
| ISAC_CSIRS_BLIND_RB | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c:326 |
| ISAC_CSIRS_BLIND_ROWS | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c:276 |
| ISAC_CSIRS_BLIND_SLOTSWEEP | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1496 |
| ISAC_CSIRS_BLIND_WIDE | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1197 |
| ISAC_CSIRS_DISC_BACKEND | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_backend.c:224 |
| ISAC_CSIRS_DISC_BUDGET | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_budget.cuh:29 |
| ISAC_CSIRS_DISC_CHUNK_UNITS | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu:221 |
| ISAC_CSIRS_DISC_CHUNK_US | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu:224, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu:232 |
| ISAC_CSIRS_DISC_CONFIRM_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:949 |
| ISAC_CSIRS_DISC_CORE | common/utils/nr_core_plan.c:276, executables/nr-uesoftmodem.c:249, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c:105, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c:418, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:145 |
| ISAC_CSIRS_DISC_DELTA_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:154 |
| ISAC_CSIRS_DISC_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:488, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:508, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:600, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_fp.c:699 |
| ISAC_CSIRS_DISC_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c:308, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:487 |
| ISAC_CSIRS_DISC_FEED_ACQ_DIV | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:200 |
| ISAC_CSIRS_DISC_FEED_BURST | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:201 |
| ISAC_CSIRS_DISC_FEED_DIV | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:49 |
| ISAC_CSIRS_DISC_FEED_WIN | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_td.c:50 |
| ISAC_CSIRS_DISC_MANAGED | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:236 |
| ISAC_CSIRS_DISC_RATELIMIT_CYCLES | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:250 |
| ISAC_CSIRS_DISC_RATELIMIT_MISS | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:246 |
| ISAC_CSIRS_DISC_RATELIMIT_NOTICE_S | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:244 |
| ISAC_CSIRS_DISC_RATELIMIT_PDET | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:248 |
| ISAC_CSIRS_DISC_RING | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:210, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_feed.c:488 |
| ISAC_CSIRS_DISC_SWEEP_HYPS | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:143 |
| ISAC_CSIRS_FULL_AB | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:2079 |
| ISAC_CSIRS_FULL_AB_EVID | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:2032 |
| ISAC_CSIRS_FULL_VALID | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:1906 |
| ISAC_CSI_DIAG | openair1/PHY/NR_UE_TRANSPORT/csi_rx.c:1421 |
| ISAC_CSS0_ALS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:397 |
| ISAC_CSS0_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5664 |
| ISAC_DCI11_DIAG_FILTER | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1634 |
| ISAC_DCI11_OBSERVE_MM | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9184 |
| ISAC_DCI11_PARSE_CACHE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1459 |
| ISAC_DCI11_PRUNELOG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1992 |
| ISAC_DCI11_RV_CONSISTENCY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:398 |
| ISAC_DCI11_S1_OFF | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci11_layout_sweep.c:774 |
| ISAC_DCI11_STAGE2 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1335 |
| ISAC_DCI_FIELDS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10734 |
| ISAC_DCI_LEN_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2339, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:105 |
| ISAC_DCI_LEN_MIN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2328 |
| ISAC_DCI_RAW_LOG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9165 |
| ISAC_DCI_SWEEP_STRIDE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2419 |
| ISAC_DCI_WATCH_RNTI | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:5136 |
| ISAC_DCI_WIDE_PROBE_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:110 |
| ISAC_DC_FIX | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:215, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3726 |
| ISAC_DELTA_SCAN | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1020 |
| ISAC_DISCOVERY_EVIDENCE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2839 |
| ISAC_DISCOVER_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1910, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2434, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2527, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2654, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4629, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4641, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4766, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4776, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6450, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6725, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7257, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7443, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:640, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:671 |
| ISAC_DISCOVER_DOMINANCE_K | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2597 |
| ISAC_DISCOVER_DOMINANCE_N | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2604 |
| ISAC_DISCOVER_MIN_BG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2548 |
| ISAC_DISC_NO_RESYNC | executables/nr-ue.c:1828 |
| ISAC_DMRS_ANCHOR | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1978 |
| ISAC_DMRS_FO_APPLY | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:210, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4402 |
| ISAC_DMRS_NID_SOLVE | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:43, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:286 |
| ISAC_DMRS_NID_SOLVE_CORE | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:253 |
| ISAC_DMRS_NID_SOLVE_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:44 |
| ISAC_DMRS_NID_SOLVE_GRACE_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:290 |
| ISAC_DMRS_NID_SOLVE_MAINTAIN_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:292 |
| ISAC_DMRS_NID_SOLVE_OPT | openair1/PHY/NR_UE_TRANSPORT/nr_dmrs_nid_solve_live.c:48 |
| ISAC_DMRS_NID_SOLVE_RECHECK_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:294 |
| ISAC_FORCE_DCI_LEN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6706 |
| ISAC_FORCE_GLOBAL_SYNC | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:175, openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1948 |
| ISAC_GNB_PDCCH_DMRS_ID | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:331 |
| ISAC_GPU_CHECK_ASYNC_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:864 |
| ISAC_GPU_CHECK_ASYNC_QUEUE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:855 |
| ISAC_GPU_COPY_FENCE | openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu.cu:41, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc_gpu_sweep.cu:318 |
| ISAC_GPU_DISC_SMS_LOW | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:118, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6007 |
| ISAC_GPU_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:386 |
| ISAC_GPU_FORCE_DISCRETE | openair1/PHY/TOOLS/nr_gpu_mem.c:10 |
| ISAC_GPU_GREEN_LOG | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:202 |
| ISAC_GPU_HEARTBEAT | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:853 |
| ISAC_GPU_LDPC_CL_FORCE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu:609 |
| ISAC_GPU_LDPC_NOCLUSTER | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.cu:598 |
| ISAC_GPU_MEMORY_MODEL | openair1/PHY/TOOLS/nr_gpu_platform.c:170 |
| ISAC_GPU_PASS | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:251, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:817 |
| ISAC_GPU_PASS_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:429 |
| ISAC_GPU_PASS_CLASSES | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:335 |
| ISAC_GPU_PASS_LOG_UNSEEN | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu:1193, openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu:1354 |
| ISAC_GPU_PASS_SEG_CLASSES | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:382 |
| ISAC_GPU_RT_DEADLINE_US | openair1/PHY/TOOLS/nr_gpu_budget.c:57 |
| ISAC_GPU_RT_PARTS | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:100 |
| ISAC_GPU_SELFCHECK_ENFORCE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2825 |
| ISAC_GPU_SHARED_MANAGED | openair1/PHY/TOOLS/nr_gpu_platform.c:172 |
| ISAC_HIDDEN_MULTI_CORESET | openair1/SIMULATION/NR_PHY/hidden_waveform.h:89 |
| ISAC_HIDDEN_RECONFIG | openair1/SIMULATION/NR_PHY/hidden_waveform.h:93 |
| ISAC_HIDDEN_SEED | openair1/SIMULATION/NR_PHY/dlsim.c:785 |
| ISAC_HIDDEN_WAVEFORM_DIR | openair1/SIMULATION/NR_PHY/dlsim.c:783 |
| ISAC_L2MAXH_DELTA | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1904, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:344 |
| ISAC_L2MAXH_HEADROOM | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1873, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:322 |
| ISAC_L2MAXH_PEAK | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1786, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:321 |
| ISAC_LANE_ALS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2362 |
| ISAC_LANE_UNION_LLR | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8121 |
| ISAC_LDPC_ITERS | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1882, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2597 |
| ISAC_LDPC_ITERS_AB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2011 |
| ISAC_LLRFILL_ALL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5669 |
| ISAC_LLR_NORM | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:232, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2587, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5425, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7306, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7363 |
| ISAC_LLR_SCALE | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:216, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5344, openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.c:45 |
| ISAC_MALLOC_NOTRIM | common/utils/system.c:401 |
| ISAC_MAP_PASS0_ONLY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:169, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1825, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1930, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2114, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:2152 |
| ISAC_MAX_POS_ACC | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:49 |
| ISAC_METRICS_PATH | openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c:237 |
| ISAC_MLOCK | common/utils/system.c:408 |
| ISAC_MMSE2_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:222, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4926 |
| ISAC_MMSE_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1092 |
| ISAC_MMSE_FLOAT | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1030, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:328 |
| ISAC_OBS_PATH | executables/nr-uesoftmodem.c:453 |
| ISAC_OBS_TEST_RING_SLOTS | executables/nr-uesoftmodem.c:456 |
| ISAC_OBS_TEST_WRITER_PAUSE_MS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_obs.c:183 |
| ISAC_OTA_CFG | openair2/LAYER2/NR_MAC_UE/config_ue.c:1816, openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c:574 |
| ISAC_PASSIVE_REPLAY_CAPTURE | executables/nr-uesoftmodem.c:672, openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:111 |
| ISAC_PASSIVE_REPLAY_FAILURES | openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:126 |
| ISAC_PASSIVE_REPLAY_INPUT | executables/nr-uesoftmodem.c:182, executables/nr-uesoftmodem.c:670, openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:3030 |
| ISAC_PASSIVE_REPLAY_UL_CONFIG | openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:322 |
| ISAC_PASSIVE_REPLAY_UL_PROBE | openair1/PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.c:320 |
| ISAC_PASSIVE_REPLAY_UL_UCI | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:3031 |
| ISAC_PBCH_CAPTURE | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1906 |
| ISAC_PBCH_CAPTURE_PATH | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1912 |
| ISAC_PBCH_CFO_SWEEP | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1274 |
| ISAC_PBCH_L2_DELTA | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1686 |
| ISAC_PBCH_NO_TRACK | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:2194 |
| ISAC_PBCH_OFFSET_BIAS | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:989 |
| ISAC_PBCH_REPLAY | openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:572 |
| ISAC_PBCH_SHIFT_CLAMP | executables/nr-ue.c:2023 |
| ISAC_PDCCH_ANON_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3447 |
| ISAC_PDCCH_ANON_DUMP_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3449 |
| ISAC_PDCCH_ANT_MASK | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:773, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.cu:207 |
| ISAC_PDCCH_C0_OCC_DB | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:344 |
| ISAC_PDCCH_CAPTURE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6203 |
| ISAC_PDCCH_CFGTRACE | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:456 |
| ISAC_PDCCH_CFGTRACE_SLOT | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:460 |
| ISAC_PDCCH_CSS_NRNTI0 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2893 |
| ISAC_PDCCH_DCIGT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9700 |
| ISAC_PDCCH_DISCOVERED_FILE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1354 |
| ISAC_PDCCH_DISCOVERY_BUDGET_US | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2614 |
| ISAC_PDCCH_DISCOVERY_REPLAY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3696 |
| ISAC_PDCCH_DISCOVERY_TRACE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2630 |
| ISAC_PDCCH_DISC_ENUM | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1191 |
| ISAC_PDCCH_DISC_ENUM_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1222 |
| ISAC_PDCCH_DISC_ENUM_NIDS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1223 |
| ISAC_PDCCH_DMRS_GATE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7566 |
| ISAC_PDCCH_DMRS_RANK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3589 |
| ISAC_PDCCH_DWELL_ADAPTIVE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1493 |
| ISAC_PDCCH_DWELL_PRESCREEN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:839 |
| ISAC_PDCCH_DWELL_PRESCREEN_Z | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:850 |
| ISAC_PDCCH_ENERGY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7663 |
| ISAC_PDCCH_EXTENT_BATCH | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1796 |
| ISAC_PDCCH_FULLCRC | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:4231 |
| ISAC_PDCCH_GATE_CFAR_Z | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7695 |
| ISAC_PDCCH_GPU_CPU_ARITH | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.cu:204 |
| ISAC_PDCCH_GPU_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_record.c:18 |
| ISAC_PDCCH_GPU_DUMP_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_record.c:20 |
| ISAC_PDCCH_GPU_EXACT_FEP | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:181 |
| ISAC_PDCCH_GPU_FEP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:11225, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.h:119 |
| ISAC_PDCCH_JOINT_ANON_BUDGET_US | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3528 |
| ISAC_PDCCH_JOINT_ANON_CORE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2990, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2999, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:146 |
| ISAC_PDCCH_JOINT_ANON_MM | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3503 |
| ISAC_PDCCH_JOINT_ANON_OCC | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3490 |
| ISAC_PDCCH_JOINT_ANON_Q | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3199 |
| ISAC_PDCCH_JOINT_ANON_TOPK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3090 |
| ISAC_PDCCH_KNOWN_PRESCREEN_MIN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7731 |
| ISAC_PDCCH_LEN_MEMORY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6966 |
| ISAC_PDCCH_LLRPROBE | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1182 |
| ISAC_PDCCH_LLRPROBE_SLOT | openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:1190 |
| ISAC_PDCCH_LLR_CACHE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6211 |
| ISAC_PDCCH_LLR_CACHE_VERIFY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6240 |
| ISAC_PDCCH_NEON_SCORE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:3541, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6573, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7623 |
| ISAC_PDCCH_NID_GPU_ACQUIRE_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:297 |
| ISAC_PDCCH_NID_GPU_BATCH | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:310 |
| ISAC_PDCCH_NID_GPU_BUDGET_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.cu:565 |
| ISAC_PDCCH_NID_GPU_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:312 |
| ISAC_PDCCH_NID_GPU_INCREMENTAL | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:463, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:308 |
| ISAC_PDCCH_NID_GPU_MAINTAIN_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:299 |
| ISAC_PDCCH_NID_GPU_NODROP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:301 |
| ISAC_PDCCH_NID_GPU_OCC_DB | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:518 |
| ISAC_PDCCH_NID_GPU_OCC_FLOOR_PCT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:524 |
| ISAC_PDCCH_NID_GPU_OCC_GATE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:515 |
| ISAC_PDCCH_NID_GPU_SYMBOLS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c:482 |
| ISAC_PDCCH_NID_GPU_WINDOW | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.c:314 |
| ISAC_PDCCH_NID_MULTISYMBOL | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_nid_gpu.cu:572 |
| ISAC_PDCCH_NO_FAKE_FOOTPRINT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1534 |
| ISAC_PDCCH_NO_MISMATCH_GATE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9529 |
| ISAC_PDCCH_NO_PDSCH_DECODE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10717 |
| ISAC_PDCCH_NRNTI | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1068 |
| ISAC_PDCCH_PAR_BANKS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5104 |
| ISAC_PDCCH_PAR_CORE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5068, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:147 |
| ISAC_PDCCH_PASS_ADAPT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:763 |
| ISAC_PDCCH_PRESCREEN_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:866 |
| ISAC_PDCCH_SCAN_EVERY_SLOT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4862 |
| ISAC_PDCCH_SHARED_DMRS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:449 |
| ISAC_PDCCH_SHARED_DMRS_MIN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:451 |
| ISAC_PDCCH_SHARED_PRESCREEN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6548 |
| ISAC_PDCCH_SHARED_PRESCREEN_AUDIT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:435 |
| ISAC_PDCCH_SHARED_PRESCREEN_MIN | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:6564 |
| ISAC_PDCCH_SIM_SPARSE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:878 |
| ISAC_PDCCH_TASK_CAP64 | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7523 |
| ISAC_PDCCH_ULDCIGT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8995 |
| ISAC_PDCCH_USS_TRACKER | executables/nr-uesoftmodem.c:228, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_uss_tracker.c:457 |
| ISAC_PDSCHQ_REAL_FIRST | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1275 |
| ISAC_PDSCHQ_SIGNAL_TIME | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1495 |
| ISAC_PDSCH_ANT_GATE | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:217, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4793 |
| ISAC_PDSCH_CDM_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3973 |
| ISAC_PDSCH_CPU_SMALL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:26 |
| ISAC_PDSCH_CPU_SMALL_NL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:29 |
| ISAC_PDSCH_CSI_EXTENT_AB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6040 |
| ISAC_PDSCH_CSI_RB_EXTENT | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:251 |
| ISAC_PDSCH_DECODE_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2188, openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:338, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:327, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1122 |
| ISAC_PDSCH_DMRS_ID2_ASYNC | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1401 |
| ISAC_PDSCH_EVM | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:211, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5068, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5073 |
| ISAC_PDSCH_EVM_RB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5737 |
| ISAC_PDSCH_GPU_BATCH | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:32 |
| ISAC_PDSCH_GPU_BATCH_ASYNC | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:841 |
| ISAC_PDSCH_GPU_BATCH_SLOTS | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.c:34 |
| ISAC_PDSCH_GPU_CHEST_TRIM | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:1484, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:1560 |
| ISAC_PDSCH_GPU_CSI_DISJOINT | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:64 |
| ISAC_PDSCH_GPU_CTX_LIMIT | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_green.h:231 |
| ISAC_PDSCH_GPU_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:108 |
| ISAC_PDSCH_GPU_DUMP_FULL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:113 |
| ISAC_PDSCH_GPU_DUMP_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:110 |
| ISAC_PDSCH_GPU_EXACT_DEADLINE_US | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:836 |
| ISAC_PDSCH_GPU_EXACT_DEMOD_EVM | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:660 |
| ISAC_PDSCH_GPU_EXACT_DEMOD_PROF | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu:1494 |
| ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:1243 |
| ISAC_PDSCH_GPU_EXACT_DIRECT | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:833 |
| ISAC_PDSCH_GPU_EXACT_LDPC_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:74 |
| ISAC_PDSCH_GPU_GRAPH_MB | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:306 |
| ISAC_PDSCH_GPU_GRAPH_PROBE_RESERVE | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_graph.cu:1085 |
| ISAC_PDSCH_GPU_LDPC | openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface_load.c:34 |
| ISAC_PDSCH_GPU_PRIO | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact.c:76, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_prio.cuh:21, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6342, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2060 |
| ISAC_PDSCH_GPU_STAGE_DUMP | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2224, openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:2358, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.c:27 |
| ISAC_PDSCH_HARQ_COMBINE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:143 |
| ISAC_PDSCH_K0_PROBE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1877 |
| ISAC_PDSCH_LLR_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:218, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5253 |
| ISAC_PDSCH_LLR_DUMP_ALL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5244 |
| ISAC_PDSCH_LLR_DUMP_FULL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5242 |
| ISAC_PDSCH_LLR_DUMP_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5254 |
| ISAC_PDSCH_MMSE2_GAIN | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:940 |
| ISAC_PDSCH_MMSE2_NL34_QPSK | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1440, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:361 |
| ISAC_PDSCH_MMSE_AB | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:223, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6124 |
| ISAC_PDSCH_MMSE_AB_QPSK | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6137 |
| ISAC_PDSCH_MMSE_EXACT_CAL | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:839 |
| ISAC_PDSCH_MMSE_EXACT_NVAR_DB | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:854, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:650 |
| ISAC_PDSCH_OWN_PDCCH_DB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6219 |
| ISAC_PDSCH_OWN_PDCCH_RM | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:224, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2614, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4934, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6641 |
| ISAC_PDSCH_PDCCH_OCC_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2378 |
| ISAC_PDSCH_PDCCH_OCC_IDS | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2371 |
| ISAC_PDSCH_PIN_PREWARM_ALL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:298 |
| ISAC_PDSCH_PROBE_C1 | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1825 |
| ISAC_PDSCH_RAW_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4017 |
| ISAC_PDSCH_RAW_DUMP_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4018 |
| ISAC_PDSCH_RAW_DUMP_MINNL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4019 |
| ISAC_PDSCH_READING_RV | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10017 |
| ISAC_PDSCH_RECOVERY_CORES | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:134 |
| ISAC_PDSCH_RECOVERY_Q | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:238 |
| ISAC_PDSCH_RECOVERY_SNAP | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2960 |
| ISAC_PDSCH_RETRY_BUDGET_US | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6518 |
| ISAC_PDSCH_RETRY_SNR_MIN_DB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6677 |
| ISAC_PDSCH_RM_GENIE | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:219, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:4912, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_rm_genie.c:25 |
| ISAC_PDSCH_RM_GENIE_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_rm_genie.c:39 |
| ISAC_PDSCH_SIGNAL_GEOM_RETRY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6428 |
| ISAC_PDSCH_SLOT_TIMING | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1480 |
| ISAC_PDSCH_SNR_GATE_DB | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1365 |
| ISAC_PDSCH_SOLVER_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:10109, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2282 |
| ISAC_PDSCH_SOLVER_FAIR | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:827 |
| ISAC_PDSCH_SOLVER_LEADER | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:844 |
| ISAC_PDSCH_SOLVER_MAX_LIVE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9918 |
| ISAC_PDSCH_SOLVER_PREF | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:863 |
| ISAC_PDSCH_SOLVER_PREF_MCS | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:865 |
| ISAC_PDSCH_STATIC_CSI_RETRY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6178, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6923 |
| ISAC_PDSCH_TBPARM | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1928, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3119, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4500, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5881 |
| ISAC_PDSCH_TINTERP_RETRY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6646 |
| ISAC_PDSCH_TINT_AB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:6080 |
| ISAC_PDSCH_TYPEB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:186 |
| ISAC_PHY_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pbch.c:1495, openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1058, openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:2227 |
| ISAC_PRACH_MARGIN_DB10 | openair1/PHY/NR_UE_TRANSPORT/nr_prach_passive.c:118 |
| ISAC_PRG_SWEEP | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:724 |
| ISAC_PROBE_ALL | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1727, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1943 |
| ISAC_PROBE_HORIZON | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3210 |
| ISAC_PSS_PEAK_RATIO | openair1/PHY/NR_UE_TRANSPORT/pss_nr.c:216 |
| ISAC_PTRS_K | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2839 |
| ISAC_PTRS_L | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2840 |
| ISAC_PUSCH_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:472 |
| ISAC_PUSCH_SEARCH_PRIO | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:357 |
| ISAC_PUSCH_TIMING | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:481 |
| ISAC_QM_ORACLE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:3008 |
| ISAC_RAW_IQ_DECIM | radio/COMMON/raw_iq.c:187 |
| ISAC_RAW_IQ_DIRECTORY | radio/COMMON/raw_iq.c:176 |
| ISAC_RAW_IQ_GAP2_AT_S | radio/COMMON/raw_iq.c:222 |
| ISAC_RAW_IQ_GAP2_S | radio/COMMON/raw_iq.c:223 |
| ISAC_RAW_IQ_GAP_AT_S | radio/COMMON/raw_iq.c:214 |
| ISAC_RAW_IQ_GAP_S | radio/COMMON/raw_iq.c:215 |
| ISAC_RAW_IQ_SPEED | radio/COMMON/raw_iq.c:182 |
| ISAC_RAW_IQ_WANDER | radio/COMMON/raw_iq.c:191 |
| ISAC_RAW_IQ_WANDER_UNTIL_S | radio/COMMON/raw_iq.c:195 |
| ISAC_RECONF_DISCOVERY_DUTY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4732 |
| ISAC_RECONF_GAP_HARD_MS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:439 |
| ISAC_RECONF_N_SUSPECT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:245 |
| ISAC_RECONF_SIB1_MAX_OCCASIONS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_sources.c:66 |
| ISAC_RECONF_SI_BUMP_WITHOUT_SIB1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:102 |
| ISAC_RECONF_SI_GRACE_MS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:99 |
| ISAC_RF_FAIL_AFTER_READS | executables/nr-ue-ru.c:379 |
| ISAC_RF_STALL_MAX_REINIT | executables/nr-ue.c:2357 |
| ISAC_RF_STALL_WATCHDOG | executables/nr-ue.c:2082 |
| ISAC_RNTI_LAYOUT_CACHE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:1764 |
| ISAC_RNTI_LAYOUT_CACHE_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9970 |
| ISAC_RNTI_LAYOUT_CACHE_FAILS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:9957 |
| ISAC_RV_RETRY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5485, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:5857 |
| ISAC_RX_BEST_BY_SNR | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1658, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:320 |
| ISAC_RX_BRANCH | openair1/PHY/NR_UE_ESTIMATION/nr_dl_channel_estimation.c:1825, openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1749, openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1933, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:326 |
| ISAC_RX_BRANCH_FO | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:220, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:1564 |
| ISAC_RX_BRANCH_MIN_DB | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1386, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:339, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:341 |
| ISAC_RX_CHAN_MAP | radio/USRP/usrp_lib.cpp:2464 |
| ISAC_RX_EQ_AB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4743, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1992 |
| ISAC_RX_GAIN_TRIM | openair1/PHY/NR_UE_TRANSPORT/nr_ue_rf_helpers.c:49 |
| ISAC_RX_IRC | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2047, openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:212, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:319, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3838 |
| ISAC_RX_IRC_SHRINK | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2049 |
| ISAC_RX_ML_2L | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1411, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:325, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4744 |
| ISAC_RX_MRC_PER_RNTI_AB | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:728 |
| ISAC_RX_MRC_PER_RNTI_SNR | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_solver.c:742 |
| ISAC_RX_MRC_SHIFT_SUM | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1850, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:347 |
| ISAC_RX_NVAR_FIX | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2569, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4121 |
| ISAC_RX_NVAR_PERBRANCH | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:2577, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4142 |
| ISAC_RX_QAM_AGC | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:2420, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:318 |
| ISAC_RX_RECOVER_BACKOFF_MAX_MS | radio/USRP/usrp_lib.cpp:1175 |
| ISAC_RX_RECOVER_BACKOFF_MS | radio/USRP/usrp_lib.cpp:1173 |
| ISAC_RX_RECOVER_LEAD_MS | radio/USRP/usrp_lib.cpp:1177 |
| ISAC_RX_RECOVER_MAX_FAULTS | radio/USRP/usrp_lib.cpp:1058 |
| ISAC_RX_RECOVER_NOCTRL | radio/USRP/usrp_lib.cpp:1079 |
| ISAC_RX_RECOVER_TEST_COARSE_EPOCH | executables/nr-ue.c:284 |
| ISAC_RX_RECOVER_WINDOW_S | radio/USRP/usrp_lib.cpp:1067 |
| ISAC_SCAN2_AUDIT_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7410 |
| ISAC_SCAN2_ULSSH_AUDIT_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7955 |
| ISAC_SCAN2_UL_Z | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:7415 |
| ISAC_SCAN_CONFIRM | executables/nr-ue.c:1125 |
| ISAC_SCAN_SCOREMEMO_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:186 |
| ISAC_SCAN_SCRATCH_MB | openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync.c:755, openair1/PHY/NR_UE_TRANSPORT/nr_initial_sync_budget.c:39 |
| ISAC_SFO_CORRECT | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:213, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:4854 |
| ISAC_SHIFT_CENSUS | executables/nr-ue.c:1712 |
| ISAC_SHUTDOWN_TIMEOUT_S | common/utils/nr_shutdown.c:210 |
| ISAC_SIB1_CACHE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:6109 |
| ISAC_SIB1_CACHE_DIR | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:6095, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:6103 |
| ISAC_SIB1_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_decoding.c:245 |
| ISAC_SIB1_SIBLING | openair1/PHY/NR_UE_TRANSPORT/nr_passive_acq_state.c:326 |
| ISAC_SIM_CANDIDATE_RNTI | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c:253 |
| ISAC_SIM_CANDIDATE_RNTIS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c:250 |
| ISAC_SI_SOFTCOMBINE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:153 |
| ISAC_SRS_CPU_PCT | openair1/PHY/NR_UE_TRANSPORT/nr_srs_passive.c:265 |
| ISAC_SSB_IQ | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1007 |
| ISAC_SUBSET_DIAG | openair1/PHY/NR_UE_TRANSPORT/nr_dlsch_demodulation.c:1731, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod_host.c:324 |
| ISAC_SWEEP_OCCASIONS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:2314 |
| ISAC_SYNC_ANT_MASK | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:96 |
| ISAC_TARGET_PCI | executables/nr-ue.c:211 |
| ISAC_TB_LOG_WIDTHS | openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c:3171 |
| ISAC_TB_PUSCH_IDS | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:1677 |
| ISAC_TB_PUSCH_MCSTAB | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:1691 |
| ISAC_TB_PUSCH_TP | openair2/LAYER2/NR_MAC_gNB/nr_radio_config.c:1686 |
| ISAC_TDD_SKIP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5567 |
| ISAC_TD_CB0_ELIM | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:1290 |
| ISAC_TD_CB0_GPU_AUTOREG | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.c:75, openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.c:89 |
| ISAC_TD_CB0_GPU_BACKOFF | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_backend.c:107 |
| ISAC_TD_CB0_GPU_ITERS | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_gpu_backend.c:27, openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.c:49 |
| ISAC_TD_CERT_CONFIRMED | openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c:387 |
| ISAC_TD_DCI_ADJ | openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c:313 |
| ISAC_TD_FIELDBOOK | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:1858 |
| ISAC_TD_GRANTWORK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:11241, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1957 |
| ISAC_TD_GW_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7496 |
| ISAC_TD_GW_PROBE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:1954 |
| ISAC_TD_GW_PROFILE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7622 |
| ISAC_TD_IGNORE_SIB1 | openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:82, openair1/SIMULATION/NR_PHY/hidden_waveform.h:96 |
| ISAC_TD_K0_ORACLE_LEGACY | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:447 |
| ISAC_TD_PROBE_EQUIV_BRANCH | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7724 |
| ISAC_TD_PROBE_EQUIV_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:7691 |
| ISAC_TD_TB_CPU_WHILE_ACQ | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:225, openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_gpu_backend.c:40, openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.c:41 |
| ISAC_TSYNC_AUDIT | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:160, openair1/PHY/NR_UE_TRANSPORT/csi_rx.c:1049, openair1/PHY/NR_UE_TRANSPORT/dci_nr.c:683, openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1741, openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1763 |
| ISAC_TSYNC_CRC_GATE | openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1961 |
| ISAC_TSYNC_DUMP | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:122 |
| ISAC_TSYNC_GLOBAL_REBASE | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:257 |
| ISAC_TSYNC_MEASURE | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:303 |
| ISAC_TSYNC_MIN_EWIN | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:297 |
| ISAC_TSYNC_REBASE_RATIO | openair1/PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c:267 |
| ISAC_TSYNC_RESET | executables/nr-ue.c:1453, executables/nr-ue.c:1538 |
| ISAC_UECTX_PATH | executables/nr-uesoftmodem.c:437 |
| ISAC_UECTX_PERIOD_S | executables/nr-uesoftmodem.c:442 |
| ISAC_UECTX_TEST_WRITER_PAUSE_MS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.c:510 |
| ISAC_UE_RT_CORE | common/utils/nr_core_plan.c:272, common/utils/nr_core_plan.c:347, executables/nr-ue.c:2531, executables/nr-uesoftmodem.c:203, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:142 |
| ISAC_UHD_CTRL_THREADS | executables/nr-uesoftmodem.c:217 |
| ISAC_UL_BGDISC_CORE | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c:70 |
| ISAC_UL_BGDISC_MAX_AGE_MS | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_bgdisc.c:121 |
| ISAC_UL_CONFIG_DISC | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_config_disc.c:141 |
| ISAC_UL_CPU_BUDGET_PCT | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:859 |
| ISAC_UL_DBG_LOG | executables/nr-ue.c:1765, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8901 |
| ISAC_UL_DCI_BANK | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_config_disc.c:83 |
| ISAC_UL_DISC_EVERY | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_ul_disc.c:315 |
| ISAC_UL_DMRS_DISC | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:2009 |
| ISAC_UL_FDRA_REFUSE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci01_layout_sweep.c:225 |
| ISAC_UL_FORCE_IDS | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:165 |
| ISAC_UL_FORCE_OPTS | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:1730 |
| ISAC_UL_GPU_FEP_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1043 |
| ISAC_UL_GPU_FEP_DEADLINE | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_fep.cuh:200 |
| ISAC_UL_GPU_FEP_PCT | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1048 |
| ISAC_UL_GPU_LDPC_DEADLINE_US | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:392 |
| ISAC_UL_GPU_LDPC_MIN_BITS | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:400 |
| ISAC_UL_GPU_LDPC_PCT | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc_host.c:393 |
| ISAC_UL_GPU_PASS | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_dl.c:252, openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:288, openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:519 |
| ISAC_UL_GPU_PASS_CHECK | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:553 |
| ISAC_UL_GPU_PASS_CLASSES | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:533 |
| ISAC_UL_GPU_PASS_SEED | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:356 |
| ISAC_UL_GPU_PRIO | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_diag.h:28 |
| ISAC_UL_HARQ_MAX_SEG | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:231 |
| ISAC_UL_HYP_MAX_AGE | openair1/PHY/NR_UE_TRANSPORT/nr_gpu_pass_ul.c:129 |
| ISAC_UL_LAYOUT_BRUTE | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:568 |
| ISAC_UL_LAYOUT_BRUTE_IDS | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:575 |
| ISAC_UL_LAYOUT_CONTROL | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:1564 |
| ISAC_UL_LLR_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:44 |
| ISAC_UL_LLR_DUMP_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:56 |
| ISAC_UL_LLR_DUMP_MAX_MB | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:60 |
| ISAC_UL_LLR_DUMP_MCS | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:52 |
| ISAC_UL_LLR_DUMP_MIN_RB | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:54 |
| ISAC_UL_LLR_DUMP_OK_MAX | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:58 |
| ISAC_UL_LLR_DUMP_SNR | openair1/PHY/NR_UE_TRANSPORT/nr_ul_llr_dump.c:50 |
| ISAC_UL_LOCK_CLIP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1400 |
| ISAC_UL_LOCK_COHERENT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:419, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:423, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:429 |
| ISAC_UL_LOCK_EDGE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:417, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:424, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:430 |
| ISAC_UL_LOCK_LLR | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:725 |
| ISAC_UL_LOCK_NO_TRIAL_UNLOCKED | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1603 |
| ISAC_UL_LOCK_PRIOR_DWELL | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1901 |
| ISAC_UL_LOCK_PRIOR_SEARCH | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1892 |
| ISAC_UL_LOCK_SPRT | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:425, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:431 |
| ISAC_UL_LOOKALIKE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:734 |
| ISAC_UL_MCS_TABLE | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:223, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:643 |
| ISAC_UL_NID_BG_ADDPOS | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:103 |
| ISAC_UL_NID_BG_CORE | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:118 |
| ISAC_UL_NID_BG_DBG_TRY | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:99 |
| ISAC_UL_NID_BG_DROP | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:114 |
| ISAC_UL_NID_BG_GAP_MS | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:97 |
| ISAC_UL_NID_BG_HYP | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:112 |
| ISAC_UL_NID_BG_JOB_MS | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:110 |
| ISAC_UL_NID_BG_LOG | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:116 |
| ISAC_UL_NID_BG_MCS | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:123 |
| ISAC_UL_NID_BG_MIN_RB | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:93 |
| ISAC_UL_NID_BG_NBWP | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:121 |
| ISAC_UL_NID_BG_QM | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:91 |
| ISAC_UL_NID_BG_REPEAT | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:95 |
| ISAC_UL_NID_BG_SNR | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:89 |
| ISAC_UL_NID_BG_TOPM | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:101 |
| ISAC_UL_NID_BG_TRIALS | openair1/PHY/NR_UE_TRANSPORT/nr_ul_nid_bg.c:105 |
| ISAC_UL_PIPELINE | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:266 |
| ISAC_UL_PROBE | executables/nr-ue.c:1655 |
| ISAC_UL_PROBE_DEPTH | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:852 |
| ISAC_UL_PROBE_TA | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:417 |
| ISAC_UL_RETRY_MAX_RB | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:244 |
| ISAC_UL_RETRY_SNR_DB | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c:218 |
| ISAC_UL_RX_BRANCH | openair1/PHY/NR_ESTIMATION/nr_ul_channel_estimation.c:664, openair1/PHY/NR_TRANSPORT/nr_ulsch_demodulation.c:46, openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:2179 |
| ISAC_UL_SPRT_DUMP | openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_layout_lock.c:1452 |
| ISAC_UL_STALL_US | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_diag.h:61 |
| ISAC_UL_TA_SWEEP | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:415 |
| ISAC_UL_TEST_RT_LOAD | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_fep.cuh:148 |
| ISAC_UL_TIMING_DIAG | openair1/PHY/NR_ESTIMATION/nr_ul_channel_estimation.c:668 |
| ISAC_UL_TIMING_DISC | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:2006 |
| ISAC_UL_UCI_BUDGET_US | openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_decode.c:308 |
| ISAC_UL_UE_STATE | executables/nr-uesoftmodem.c:435, executables/nr-uesoftmodem.c:440, openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.c:556, openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_ul_ids.c:80 |
| ISAC_USRP_ISOLATION_CHECK | radio/USRP/usrp_lib.cpp:1462 |
| ISAC_USRP_RESTART_MAX | radio/USRP/usrp_lib.cpp:1014 |
| ISAC_USRP_RESTART_UNTIL_SYNC | radio/USRP/usrp_lib.cpp:1109 |
| ISAC_USRP_RING_CORE | common/utils/nr_core_plan.c:273, common/utils/nr_core_plan.c:348, executables/nr-uesoftmodem.c:205, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_recovery.c:143, radio/USRP/usrp_lib.cpp:1123, radio/USRP/usrp_lib.cpp:1479 |
| ISAC_USRP_RING_MS | executables/nr-uesoftmodem.c:206, radio/USRP/usrp_lib.cpp:992 |
| ISAC_USRP_RING_PKTS | radio/USRP/usrp_lib.cpp:1146 |
| ISAC_USRP_RING_POLL | radio/USRP/usrp_lib.cpp:1156 |
| ISAC_USRP_RING_POLL_TIMEOUT_MS | radio/USRP/usrp_lib.cpp:1158 |
| ISAC_USRP_RING_PRIO | radio/USRP/usrp_lib.cpp:1134 |
| ISAC_USRP_RING_TRAFFIC_METRICS | openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics.c:55, openair1/PHY/NR_UE_TRANSPORT/nr_passive_metrics_json.c:185 |
| ISAC_XANT | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c:3512 |
| LDPC_BENCH | openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_decoder.c:30, openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:879, openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:41 |
| LDPC_CB0_BREAKER_N | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:525 |
| LDPC_CB0_BREAKER_S | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:529 |
| LDPC_CB0_CHUNK | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:541 |
| LDPC_CB0_GRAPH | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:543 |
| LDPC_CB0_MAX_ITEMS | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:537, openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_batch.cu:87 |
| LDPC_CB0_MEM | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:560 |
| LDPC_CB0_NCW | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:622 |
| LDPC_CB0_SLOTS | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:539 |
| LDPC_CB0_TEST_INJECT | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:533 |
| LDPC_CB0_TEST_STALL_MS | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:535 |
| LDPC_CB0_TIMEOUT_MS | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:518 |
| LDPC_CB0_TIMEOUT_US_PER_ITEM | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:522 |
| LDPC_CB0_WORK_CW | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:570 |
| LDPC_CUDA_BREAKER_N | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:526, openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:175 |
| LDPC_CUDA_BREAKER_S | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:530, openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:177 |
| LDPC_CUDA_GRAPH | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:544, openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1160 |
| LDPC_CUDA_TEST_INJECT | openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1053 |
| LDPC_CUDA_TEST_QUEUE_CAP | openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:173 |
| LDPC_CUDA_TEST_SKIP_LAUNCH | openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1049 |
| LDPC_CUDA_TEST_STALL_MS | openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu:1051 |
| LDPC_CUDA_TIMEOUT_MS | openair1/PHY/CODING/nrLDPC_cuda/ldpc_cb0.cu:519, openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:171 |
| LDPC_CUDA_WARMUP_ITERS | openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c:393 |
| NR_GPU_CB0 | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_batch.c:844 |
| NR_GPU_FORCE_ANT | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:771 |
| NR_GPU_LLR_SCALE | openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu:768 |
| NR_GPU_POLAR | openair1/PHY/NR_UE_TRANSPORT/nr_polar_gpu.h:54 |
| NR_TD_CB0_CUDA_LDPC | openair1/PHY/NR_UE_TRANSPORT/nr_td_cb0_batch.c:849 |
| OAI_CONFIGMODULE | common/config/config_load_configmodule.c:252 |
| OAI_GDBSTACKS | common/utils/assertions.h:18 |
| OAI_RNGSEED | openair1/SIMULATION/TOOLS/rangen_double.c:40 |
| UHD_CONFIG_FILE | common/utils/nr_core_plan.c:247 |
| USRP_DECIM_WORKERS | radio/USRP/usrp_lib.cpp:3332 |
