# UL-REALBWP-20261017: why the UL BWP never locks on the real captures

Branch csd/int16-ulrfsim, commit 4a12f9ee05 (not merged). Labels: [MEASURED, DGX replay, capture, n] unless stated; cells never pooled. Driver p99_replay.sh, t26gE.env + sd_t26ul.cfg; every arm also had ISAC_UL_MCS_TABLE_SWEEP/BWP_SEED/K2_PAIR/TDA_PAIR=1.

**Outcome:** `ISAC_UL_BWP_DL_PRIOR=1` locks the UL BWP on b and tim (6/6 replays). Real grants still give 0 genuine UL CRC passes: unknown per-UE UL scrambling ids and low UL SNR at the passive RX are the blockers below the lock.

## Root cause (by weight)
1. **Legacy hard-vote lock excludes wide grants.** t26gE sets ISAC_UL_LOCK_SIGN=2, not ISAC_UL_LOCK_SPRT. Probes/hypotheses only if range <= PROBE_MAX_RB=16 (nr_pdcch_ul_layout_lock.c:21, booking ~1217, nr_ul_layout_probe_plan ~1259). Busiest UE on b (RNTI 0x5186, 90-190 PRB, ~25 % of UL DCIs) got 16 of 1035 probes [b, n=1]. Votes dominated by 3-PRB blobs; 22-66 of ~2600-4000 probes reach 6 dB; lock needs 6 votes; observed votes=2 [b], 12 votes over 10 hypotheses [b, brute8].
2. **DM-RS energy cannot attribute a range to a BWP size.** RIV 21609 on b: N=214 -> 4+115 (16.7 dB), N=215 -> 109+101 (15.0 dB), N=217 -> 90+119 (weak). Shared DM-RS sequences make many ranges look strong. SPRT re-run with PCI id and TA=0: no lock, top z ~3.5-4.4, 217@0 not top [b, n=1 each]. Consistent with UL-TWINS/UL-LOCKSPEED/UL-BWP.
3. **Probes use fixed TA (1600 ±256 samples, nr_pusch_passive_monitor_rt.c:119) and PCI-seeded DM-RS.** On b the visible UE is at advance ~0 ±48 (54 UL_TIMING_DISC acceptances, all 0x5186); its hits are 10-17 dB with dmrs_id=415 (PCI). On tim: 0 detections with PCI, 781 or 1931 at 16 TA values; 0 timing acceptances. TA=0 alone does not lock (SPRT, b).
4. **CRC-first needs ids the receiver lacks.** Real grants after seed lock: 0 of ~3000/run with SNR >= 5 dB, median -6 to -9 dB. Only CRC OK on a real grant (b, dlp_b1) was an all-zero TB. CRC-oracle brute force on one b grant (10 twin ranges × 1024 data ids × 3 tables = 30 720 decodes): 2 single-CB CRC16 hits with different ids/BWPs = false alarms [b, n=1 grant].
5. **Check 6 is not a cause here:** all 8874 UL DCIs counted are 0_1 [b, tim]. The 0_0 USS rule (nr_pdcch_blind_monitor.c:5599) stays a latent note.

## Fix: ISAC_UL_BWP_DL_PRIOR=1 (default off, needs ISAC_UL_BWP_SEED=1)
- With nothing configured, the UL BWP prior = SIB1 initialUplinkBWP if decoded, else own DL BWP (TDD pair shares centre, 38.331). Receiver-derived only; tim -> SIB1 0+217, b -> DL 217@0.
- SEED_LOCK once the DCI fit admits the RIV width; marked uncertain (sib1_ul_dl), re-opens after 8 CRC failures; partner/look-alike hypotheses first.
- Code: nr_pdcch_blind_monitor_rt.c, nr_pdcch_ul_layout_lock.c/.h.
- Test UlLayoutLock.DlBwpPriorSeedsAnUncertainLockOnlyWhenTheFitAdmitsItsSize; `test_nr_pdcch_blind_monitor --gtest_filter='UlLayout*'` 14/14 [UNIT, DGX, build_t26]. Full ctest not run.
- Diagnostic hooks (probe TA/NID/TASCAN/MAP, data-id brute force) kept out of tree as ulrealbwp_data/diag_probe_ta_nid_map_brute.patch (base 7cf3e8525e).

## Results [DGX replay, build_t26, SPEED=1, TMO=90]
| capture | arm | n | UL BWP lock | real-grant attempts | genuine UL CRC | DL real p50/p99 (us) |
|---|---|---|---|---|---|---|
| b | 4 flags | 2 | 1/2: 216@1 by votes (look-alike) at 5.6 s; 0/2 SEED | 285, 0 | 0 | 395/1955, 395/1445 |
| b | + DL_PRIOR | 3 | 217@0 SEED_LOCK 3/3 | 3878, 1196, 1000 | 0 | 375/1655, 435/1795, 415/1705 |
| tim | 4 flags | 2 | 0 | 0, 0 | 0 | 415/1375, 435/3765 (n=436) |
| tim | + DL_PRIOR | 3 | 217@0 SEED_LOCK 3/3 | 3055, 2673, 2993 | 0 | 425/1555, 415/1475, 385/1365 |

- DL within run noise. The DL columns are the agent's per-run real-grant figures and are not comparable to goal_metrics tot latency in other reports.
- 2+2 DL_PRIOR runs on the diagnostic build, 1+1 on 4a12f9ee05; they match.
- 217@0 is a prior, not proven truth: no CRC pass confirms it.

## Still blocking UL decode
- Real-grant UL SNR at the passive RX: median -6 to -9 dB, none >= 5 dB.
- Per-UE UL DM-RS/data ids unknown (b: 0x5186 uses 415 = PCI at TA ~0; other signals with 249; tim: none found with PCI/781/1931).
- Next lever: per-UE background id + timing search over snapshots (UL-ONAIR Stage 3 Task 5 / ISAC_UL_BGDISC), not started.
- Probe TA not fed back from the decode's timing search (alone does not lock).

## Risks
- n=2-3 per capture, 12 s captures. A seed can hold a wrong BWP if the real UL BWP is smaller than SIB1's (earlier tim OTA locks at 107@x hint at this); only CRC can correct it.
- UL_PUSCH_CRC does not print the trial hypothesis, so a future CRC pass cannot be traced to its BWP.
