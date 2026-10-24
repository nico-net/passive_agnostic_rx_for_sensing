# DCI11-RESOLVER: one general DCI 1_1 resolver, evidence-only ranking (2026-10-24, sens6/dci-resolver)

Branch `sens6/dci-resolver` from `5f7c8e73de` (= csd/b2-fix `da253bfd6a` + local sens6 commits incl. `9aef537ffb`). Host sens6 (x86, RTX 4060 Ti, 14 GB RAM), rfsim only. Labels per PROJECT_MEMORY §0.1; nothing pooled with DGX or across cells.

## 1. Pipeline before (code read at 5f7c8e73de) [READ code]

| step | rule (non-general) | file:line at base |
|---|---|---|
| profile | hand-written 3-candidate DCI 1_1 profile: RA type 1 only, 1 codeword, 4-bit ports, HARQ 4, DAI 2, K1 3, SRS 2, no CI/TCI/CBG; BWP ind 0..2 x TDA 0..4; optional-field fallback; refuses > 3 candidates | nr_pdcch_blind_monitor.c:5476-5565 (`count==3` at :5507, :5555) |
| gate | `n == 0` -> grant dropped unless the WL-SOLVER fallback (solver on, resolver armed) hands a "representative" | nr_pdcch_blind_monitor_rt.c:7918, :7935 (`nr_pdcch_dl_solver_fallback_ok`, monitor.c:5567) |
| resolver | FDRA staging: type 1 only at init, type 0 / dynamicSwitch armed only after a TB-CRC refutation of the whole type-1 set; disarmed by a type-1 pass | sweep.c:369 (`fdra_next = 1`), :490 (`kArmOrder`); rt.c:1194 (`g_dl_type1_pass`), :1641 (`arm_next_mode`) |
| resolver | carrier indicator fixed 0; DAI {0,2,4}; no Rel-16/17 fields (2nd TPC, one-shot, PDSCH group, NFI, n-groups, HARQ retx, PUCCH cell, SRS offset); CBGTI not x N_TB; no fields after DM-RS init / padding | sweep.c:43, :82, :76-91 |
| resolver | per-layout field histograms + distribution ranking (a field-structure prior; ranked only, unused at runtime) | sweep.c:806 (`prune_by_distribution`) |
| solver gate | unsolved RNTI skipped while `n_alive > 1024` | rt.c:8599 |
| solver gate | grant skipped when the representative's (= an arbitrary live layout's) rv != 0 | rt.c:8572 |
| readings | first 128 readings / 256 offset keys in layout-id order, the rest cut | rt.c:8588, :8610 |
| solver | 48-RB search cap (`SOLVER_SEARCH_MAX_RB`), wide-only after 16 grants (`SOLVER_WIDE_AFTER`), BWP cap below 96 RB | nr_pdsch_solver.c:14-15, search_max_rb/rb_limit |
| solver | one-layer-QPSK preference (+ QPSK-only MCS table filter) and its WL-SOLVER rotation (every 4th grant while no RNTI of the cell is solved) | solver.c:686 (`reading_nl_qpsk`), :982-1006 |
| solver | search order "most-shared first" (readings sorted by layout count) | solver.c:1213 |

## 2. TS 38.212 §7.3.1.2.2 field table (what the resolver now enumerates) [READ code + spec recollection: the spec text is not on this host, Rel-17 field positions marked]

| # | field | widths (bits) | condition | where it sits for the reader |
|---|---|---|---|---|
| 1 | identifier | 1 | fixed | - |
| 2 | carrier indicator | 0, 3 | crossCarrierSchedulingConfig | pre-FDRA sum (`bwp_ind` = CI + BWP) |
| 3 | BWP indicator | 0, 1, 2 | n_dl_bwp (<= 4) | pre-FDRA sum |
| 4 | FDRA | type 1: ceil(log2(N(N+1)/2)); type 0: N_RBG (rbg-Size cfg 1/2, Table 5.1.2.2.1-1, BWP start alignment); dynamicSwitch: 1 + max(N_RBG, RIV) | resourceAllocation x rbg-Size; cfg 2 with P equal to cfg 1 is the same read (deduped) | read |
| 5 | TDRA | 0..4 | ceil(log2 I), I <= 16 (5-6 bits only for FR2-2 multi-PDSCH: excluded) | read |
| 6 | VRB-to-PRB | 0, 1 | 0 under RA type 0 only (coupling applied) | pre-MCS sum |
| 7 | PRB bundling size ind. | 0, 1 | dynamicBundling | pre-MCS sum |
| 8 | rate matching ind. | 0, 1, 2 | rateMatchPatternGroup1/2 | pre-MCS sum |
| 9 | ZP CSI-RS trigger | 0, 1, 2 | aperiodic ZP sets | pre-MCS sum |
| 10 | TB1 MCS/NDI/RV | 5/1/2 | fixed | read |
| 11 | TB2 MCS/NDI/RV | 0, 8 | maxNrofCodeWordsScheduledByDCI | pre-ports sum |
| 12 | HARQ process | 4, 5 | harq-ProcessNumberSizeDCI-1-1-r17 | pre-ports sum (pid read as 4 bits at rv+2) |
| 13 | DAI | 0, 2, 4, 6 | semi-static / dynamic / dynamic CA or enhancedDynamic-r16 / enhancedDynamic CA | pre-ports sum |
| 14 | TPC PUCCH | 2 | fixed | pre-ports sum |
| 15 | 2nd TPC (Rel-17) | 0, 2 | mTRP PUCCH | pre-ports sum |
| 16 | PUCCH resource ind. | 3 | fixed | pre-ports sum |
| 17 | PDSCH-to-HARQ timing | 0..3 | dl-DataToUL-ACK(-r16/-r17) <= 8 | pre-ports sum |
| 18-21 | one-shot HARQ-ACK req., PDSCH group index, NFI (0/1/2), n requested groups | 0/1 each, NFI 0..2 | Rel-16 type-3 / enhancedDynamic | pre-ports sum (couplings not applied: superset) |
| 22-23 | HARQ-ACK retx ind., PUCCH cell ind. (Rel-17) | 0, 1 | Rel-17 [position INFERRED] | pre-ports sum |
| 24 | antenna ports | 4 (T-1), 5 (T-2 or T-3), 6 (T-4) | DM-RS type x maxLength (also the -1A..-4A twins, same widths) | read; 5 bits = two layouts |
| 25 | TCI | 0, 3 | tci-PresentInDCI | post-ports sum |
| 26 | SRS request | 2, 3 | SUL | post-ports sum |
| 27 | SRS offset ind. (Rel-17) | 0..2 | availableSlotOffset [position INFERRED] | post-ports sum |
| 28 | CBGTI | N_TB x {0,2,4,6,8} | CBG transmission (coupled to TB2) | post-ports sum |
| 29 | CBGFI | 0, 1 | only with CBG | post-ports sum |
| 30 | DM-RS seq. init | 1 | fixed | read (nSCID) |
| 31-35 | priority ind., ChannelAccess-CPext (0..4), min. sched. offset ind., SCell dormancy (0..5), PDCCH monitoring adaptation (0..2, Rel-17) | | Rel-16/17 | tail (never read) |
| 36 | size-alignment padding | 0, 1 | §7.3.1.0 | tail |

A layout = distinct read offsets (FDRA offset/width/mode, TDA offset/width, MCS offset, ports offset/width/table). For one prefix the (post-ports, tail) split only moves the DM-RS-init bit; the latest position the standard allows is stored (other splits differ in that one bit, tested by DM-RS evidence). Not handled (decoder capability, not enumeration): interleaved VRB-to-PRB (bit grouped, never applied), double-symbol DM-RS rows of the maxLength-2 tables (skipped as before), 5-bit HARQ pid read as 4 bits, cross-carrier grants read as this carrier.

## 3. Enumeration count [MEASURED, unit test Dci11Space.EnumerationCount / FieldByFieldSpaceEqualsTheResolver, sens6]

| geometry | old: type-1 stage at arming | old: all modes armed | new: full standard space |
|---|---|---|---|
| Iliad 51 PRB / 48 bits | 2521 | 11 338 | 17 524 |
| Vodafone 217 / 49 | 1329 | 3944 | 5361 |
| TIM 217 / 47 | 694 | 2118 | 2609 |
| rfsim 106 / 46 | 988 (gNB log `armed: 988`) | - | 8505 |
| rfsim 273 / 49 | 988 | - | 2085 |

Capacity 65 535 layouts (~4 MB/resolver; the 464-B/layout histogram is gone). Space max over BWP 6..273 x length 28..62: 97 511 (6 PRB, 62 bits); 173 of 6300 sampled points exceed the capacity. There the first 16 CRC-verified DCIs are applied as hard-refutation evidence before storing (106 PRB / 76 bits: 100 933 -> 34 577 stored, truth kept, 0 not stored); anything still not stored is counted (`n_truncated`) and logged, never silent.

## 4. Changes
1. `nr_pdcch_dci11_layout_sweep.c/.h` (`738321e3ea`): switch tables of §2 with the couplings, group-sum enumeration over every FDRA mode at init, tail/CI support, `nr_dci11_resolver_init_ev`, `nr_dci11_layout_space_count`; FDRA staging, disarm, histograms/distribution score removed.
2. `nr_pdsch_solver.c/.h` (`99a866dba4`, `8fd6c94fe4`): no RB cap, no wide-only wait, no QPSK preference/rotation/QPSK-only table filter; `reading.fit` orders the search and weights each reading's probe share ((load+1)/(0.05+fit), never zero); CRC evidence is (S, L) per TDA index (see §6); `nr_pdsch_solver_layouts()`; ids to 65 535, coverage counts over <= 64 ids per reading.
3. `nr_pdcch_blind_monitor*.c/.h` (`508c0ef8cc`, `2cbf6f2e3d`, `e176fb5686`, `2dec9199b1`): CDM-group-aware fit (type-1 groups 0/1), cell/CORESET ids always in the fit next to the RNTI's own blind ids, the fit waits on the slot gate for the DM-RS symbol (early-start scans otherwise read last frame), search back-off only without DM-RS support; profile + fallback gate deleted; every DCI 1_1 reads through the resolver; reading pass: solved RNTI -> its own layout set, else every live layout; FDRA classes -> DM-RS fit (F1 of the class PRBs vs the PRBs whose type-A DM-RS symbol carries the RNTI's identity, per nSCID; identities = own blind ids, else PCI / CORESET nID / decided DM-RS id); one parse per distinct read value in fit order, parse budget 512/DCI; unsolved RNTIs filter rv != 0 / MCS 28-31 per reading; n_alive > 1024 gate gone.

## 5. Unit tests [MEASURED, sens6 build_dr @2dec9199b1]
- test_nr_pdcch_dci11_layout_sweep: 42/42. New `Dci11Space.*`: a field-by-field dynamic program over the §2 table (independent of the group sums) equals the resolver set exactly for Iliad, Vodafone, TIM, rfsim 106/273, a misaligned 106 and 24 PRB; every width of every field on two base configurations x every FDRA mode; 153 random field combinations (247 more over capacity, covered by the evidence test); real payloads keep their layout and are never impossible: Iliad `0xfffe150e2210` (type 0 cfg1, TDA 4 bits, MCS@19), Vodafone 80 payloads of 0x8f5a (type 0 cfg1, MCS@20; type 1 hard-refuted), TIM 3 RMDCI payloads (type 1, TDA 2 bits @16, MCS@18), rfsim gNB DCI11_WIDTHS 46 / 49.
- Replaced (encoded removed rules): Dci11Fdra staging tests (StagingStarts..., TypeOneCell...Staging, DynamicSwitchFits..., ALaterRefutationRevives..., EveryStageArmedStillRevives, AType1PassDisarms, EveryStageFitsTheCap) -> EveryModeIsInTheSetFromTheStart, EveryLengthIsCompleteOrReportsWhatIsMissing, ResolvesType*/DynamicSwitch* without arming; distribution tests (score/dropped_dist) -> LoadedCellStatisticsNeverDeleteTheTruth; HardRefutationArms... -> HardRefutationRemovesType1...; RejectsImpossibleLayouts (bwp_ind 3 is now CI 3 + BWP 0: limit 5). Dci11ObserveGolden hash regenerated (it hashed the old enumeration).
- test_nr_pdsch_solver: 33/33 (incl. BackoffAppliesOnlyWithoutDmrsSupport: the 1-in-8 search back-off applies only when no reading has DM-RS fit >= 0.8). Replaced: OneLayerQpskReadingsGetFirstProbes..., QpskPreferenceRotates..., SearchCapFollowsBwpWidth, WideOnlyRnti...AfterTheWait, OneNarrowReadingKeepsTheOldBound -> TheDmrsFitOrdersTheSearchNotTheModulation, WithoutDmrsEvidenceEveryReadingIsProbedAlike, WideReadingsAreSearchedFromTheFirstGrant; IneligibleTablesDoNotConsumeLaterQpskTrials expectations updated (no QPSK-only table filter). New MixedSlotIsDecidedByTheTdaFieldOnceItExplainsThePasses.
- test_nr_pdcch_blind_monitor: 265/265 (profile tests -> DlResolverRecoversVaryingFrontWidths, DlResolverReadsIliad51PrbLen48).

## 6. Mixed-slot TDA field and the per-phase learner (a6b95559de)
TDA widths 0-4 are all in the set (TDA 2 bits @15 for rfsim 106 is layout 905, unit test). The solver's representative used to be chosen by S-consistency only, so a tda_bits=0 layout (TDA bits aliased into pre-MCS) stayed representative and the phase learner had to patch the mixed slot per phase. Now every CRC pass is (S, L) evidence, the phase learner's passes included; a layout mapping S1 L13 and S1 L5 to one TDA index is inconsistent, so the representative moves to the layout whose TDA field separates them and S/L are learnt per TDA index; a TDA index's (S, L) survives the 16-pass evidence ring. The phase learner is kept as a FALLBACK: used only while the representative's TDA index has no short length of its own (cells where no layout carries a TDA field that separates the slot). Unit test: rep moves to the TDA-bearing layout and mixed-slot grants in a slot phase never learnt decode 50/50 (phase learner alone cannot).

## 7. sens6 rfsim results
Baseline = csd/b2-fix on sens6 (mixedslot worktree, build_ms @9afc38e09c, measured by the mixedslot agent, runs `S/ms_b2_r1`, `S/ms_b2_r2`); new = `build_dr` @2dec9199b1. Both 4 RX, 106 PRB, 420 s, idle host (other agents frozen for the new arms), scored with score_rx.py (old gate) and score_rx_solver.py (proposed gate). [MEASURED, sens6 rfsim]

| arm | build | solve_s | post-solve CRC (full TBs) | PDSCH CRC ok / decoded (all, incl. probes) | crc_floor | drop_full | probes (why) | mixed slot (slot%10=7) full CRC | max RSS | solver gate |
|---|---|---|---|---|---|---|---|---|---|---|
| ms_b2_r1 | b2-fix | 1.046 | 99.96 % (43 775) | 43 438 / 43 965 | 98.80 | 0.29 % | 501 | 5910/5928 99.70 % | 5.8 GB | PASS |
| ms_b2_r2 | b2-fix | 0.886 | 99.96 % (43 813) | 43 402 / 43 867 | 98.94 | 0.009 % | 439 | 5925/5943 99.70 % | 6.6 GB | PASS |
| dr_g5_r1 | new | 2.068 | 99.96 % (44 158) | 43 291 / 45 395 | 95.37 | 0.001 % | 2083 | 5896/5911 99.75 % | 6.6 GB | PASS |
| dr_g5_r2 | new | 1.526 | 99.97 % (44 000) | 43 107 / 44 667 | 96.51 | 0.014 % | 1541 | 5899/5913 99.76 % | 6.7 GB | PASS |

Old gate (score_rx.py): FAIL on every arm of both builds (no `Technique D CONVERGED` lines since 0fb22ecbd0, PROJECT_MEMORY §25); crc_floor/drop_full pass on all four.
- Post-solve CRC: unchanged (99.96-99.97 %). Mixed slot: unchanged (99.7 %); the representative moves 211 -> 619 (the TDA-bearing layout) at the first mixed-slot pass in both new arms.
- Solve time: REGRESSION, 0.89-1.05 s -> 1.53-2.07 s (gate limit 10 s). The general space is 8.6 x larger at this length (8505 vs 988 layouts) and the full-band truth reading has DM-RS fit 1.0 like every alias that also reads the full band (type-0 bitmaps of all ones, other MCS/port offsets): DM-RS evidence cannot separate them, only CRC does, so 3-5 x more probes are spent (the extra failed probe decodes are the lower all-grant CRC %; CRC-ok totals are equal). The b2-fix's QPSK preference and 48-RB cap happened to fit this bed.
- Intermediate builds (not final, same bed): 99a866/508c0e (fit read stale samples on early-start scans, fit mostly unknown): solve 2.85 / 3.47 s, crc_floor 93.68 / 92.45; e176fb (150 us wait): solve 2.56 s. The fix was the slot-gate wait (2dec919).

273 PRB rank-1 auto bed (180 s, 4 RX, gnb.sa.rfsim.100mhz.conf + ue.passive.auto.100mhz.dgx.cfg) [MEASURED, sens6 rfsim]:
| run | build | solved | solve_s | post-solve CRC | PDSCHQ 0x1234 |
|---|---|---|---|---|---|
| r1s_b2base | b2-fix (r1solver agent) | 0 | - | - | 0/4293 (0 %) |
| dr_r273d | new | 1 | 1.112 | 99.68 % (4432 full TBs; < 5000 only because the arm is 180 s) | 4562/5688 (80 %) |
The 273 PRB rank-1 bed was never solved on b2-fix or 4a94af8c0b (sim_results3: 0 %); the general resolver + evidence ranking solves it in 1.1 s.

OTA replay (Iliad/Vodafone/TIM): NOT done -- the raw captures (`ota_runs/raw_iliad403_4s_1023_2315`, `raw_pci415_12s_b`, `raw_pci923_tim_12s_tile3`) are on the DGX; none is on sens6. Real payloads of all three are covered by the unit tests (§5).

Host note: one earlier arm of this work (13:32, build e176fb) was OOM-killed by host memory pressure (several agents' receivers); the receiver's own max RSS is 6.6 GB on both builds (resolver ~4 MB). All later runs ran in `systemd-run --scope -p MemoryMax=11G -p OOMPolicy=continue`.

## 8. Risks / open
- Stage-1 soft rule kept: Wilson upper bound of the MCS <= 27 rate < 0.75 drops a layout (a cell with > 25 % MCS 28-31 retransmissions could lose the truth); HARQ-sequence relative bar kept. Both are DCI-statistics evidence, not cell constants; not changed.
- Type-0 / dynamicSwitch aliases of a type-1 cell are never hard-refuted, so the live set stays larger (cost bounded by the parse budget and the solved RNTI's own layout set). Unsolved-RNTI cost: one peek pass per live layout per DCI.
- DM-RS fit assumes k0 = 0 and type-A DM-RS; co-scheduled UEs with the same identity inflate the occupied set (rank-only, weights never zero).
- One resolver per cell and DCI length (the first length armed); UEs with another DCI 1_1 length / resolved BWP entries are not read (pre-existing).
- Rel-17 field positions (2nd TPC, HARQ retx, PUCCH cell, SRS offset) are from recollection; a field placed in the wrong group would move the antenna-ports or DM-RS-init offset for configurations using it. The enumeration covers both pre- and post-ports placements only where the sums overlap.
- OTA captures (Iliad/Vodafone/TIM raw IQ) are on the DGX, not on sens6: no replay was possible here; OTA/replay check left to the DGX.
