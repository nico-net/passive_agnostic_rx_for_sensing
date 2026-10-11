# SOLVERCOVER (2026-10-11): why the PDSCH solver leaves TIM RNTIs undecoded, and a general fix

Host: DGX GB10 (aarch64), worktree `rr-csd-scv`, branch `csd/solver-cover` (from `csd/otatest` 84c702792f).
Commits: 6293f54932 (diagnostics, contrast ranking, fair share, table reuse, contrast-aware "decided"),
fc93702433 (fair share made work-conserving, WHY entries only in diag mode), 13d9afd4fd (cell default hypothesis kept
while an RNTI has no UE-specific id). The final configuration is "fix4" = 13d9afd4fd with
`ISAC_PDSCH_DMRS_ID_CONTRAST=1 ISAC_PDSCH_SOLVER_FAIRSHARE=1` (plus `ISAC_PDSCH_SOLVER_WHY=1`, logging only). All new behaviour is behind
default-off flags. Every build, unit-test run and replay went through `rr-orchestration/heavy.sh`; no radio, no
captures. Replays: `tests/passive_rx/timul_replay.sh`, env `rr-orchestration/ota/t26r.env`, 4 RX. Results are kept per
capture and per speed; captures, cells and speeds are never merged.

## 1. Root cause

The task statement ("29 of 45 RNTIs never got a single decode attempt") counted `EVMGRANT` lines. `EVMGRANT` is only
logged for full decodes, and a full decode exists only after an RNTI is solved. Unsolved RNTIs get first-code-block
probes, which are not logged. So the starved RNTIs were probed. Their probes failed because the hypotheses were wrong.

- **[MEASURED, OTA DGX 2026-10-10, TIM PCI 923, runs ab_Tq1_1_230208 / ab_Tq1_1_231425 / ab_Tq2_1_231945]** RNTIs with DCI 1_1
  without any `EVMGRANT`: 44 / 26 / 29. Of those, 37 / 19 / 20 logged `PDSCH_SOLVER ... reopening pruned layouts after N failed
  CRC probes` (N >= 32 credited failed probes), and every one with >= 100 DCIs did (13/13, 7/7, 8/8). They were searched; the
  search failed.
- **[MEASURED, same OTA logs, `DMRS_SCAN ids` lines]** The per-RNTI blind DM-RS identity scan (`ISAC_PDSCH_DMRS_ID_SCAN`) votes the
  identities of a slot for every RNTI scheduled in it. One identity that is present in almost every slot held one of the two
  hypothesis slots of almost every RNTI: 65 (runs 231425 and 231945) and 928 (run 230208). No RNTI was ever solved with it. The other
  slot was often the identity of the busiest UE (for example 1337 = 0x5114, 1302 = 0x4ba4, 1509 = 0x54ec in run 231945). Unsolved
  0x51bc had 14 of 21 top-2 entries equal to another RNTI's CRC-confirmed identity, and 0x57b0 had 16 of 25. The solved RNTIs all
  used (DM-RS id = own id, data id = 923) with add_pos 1, table 1 and S 1.
- **[MEASURED, replay raw_pci923_tim_12s_tile3, `ISAC_PDSCH_SOLVER_PROBELOG=1`]** The truth below comes from the TIMPDSCH/TIMORC
  identity map: 0x4ac6 -> 298, 0x5264 -> 1798, 0x5098 -> 638, 0x4e32 -> 1677. In one run, 0x4ac6 spent 1170 probes and 0x5264 spent
  404 probes, and neither probe set contained its own identity. Their top-2 were (1280, 88) and (1280, 1677/781): 1280 is a
  cell-wide identity on this capture, and 1677 is 0x4e32's. Mechanism: once an RNTI's best identity has 2 votes over 4 slots, it is
  "decided" and gets no more full 16-bit scans. A cell-wide identity makes every RNTI decided before its own identity is found.
- **[MEASURED, replay TIM 12 s, `ISAC_PDSCH_SOLVER_WHY=1`]** One busy RNTI whose search cannot succeed (0x4e32, which has the right
  identity but is probably SNR-limited per TIMPDSCH) planned 65 % of all probes (2428 of 3756). On some grants other RNTIs had no
  budget (`budget0`, up to half of their grants), because those probes kept the decode ring above the 25 % admission fill.
  **[MEASURED, OTA]** On TIM the PDSCH queue dropped 22-34 % of its jobs as stale (15083 of 67801 in run 231945; max lag 1889
  slots), against 7 % on Vodafone (Vq1). A stale probe is not credited.
- **[CODE-READ]** The solver keeps per-RNTI state for at most 64 RNTIs and never frees an entry. From the 65th distinct RNTI
  onwards, `slot_of()` returns NULL, and the RNTI is never planned for the rest of the run. That is a permanent starvation once RNTI
  churn exceeds 64. At 45 RNTIs per 150 s on TIM, that point is a few minutes away.
- **[CODE-READ]** The scan votes only for the RNTI whose probe triggered it. Other RNTIs in the same slot that arrived while the
  scan was running got no vote, so their evidence was lost.
- Not causes **[MEASURED, WHY counters, TIM 12 s]**: the backoff (`backoff=` <= 25 grants), CSI-RS slots (`csirs=` a few %),
  `ISAC_PDSCH_SOLVER_MAX_LIVE`, and the per-occasion cap (`cap=` <= 2).
- Vodafone **[MEASURED, OTA ab_Vq1_1_231143]**: 40 of 46 DCI RNTIs solved, and every RNTI with >= 100 DCIs was solved. There is no
  starvation of the same kind: its identity is the PCI (415) for everyone, so a polluted top-2 still contains it.

## 2. Fix (general, no per-cell constants; all default off)

| Flag | What it does | Why it is general |
|---|---|---|
| `ISAC_PDSCH_DMRS_ID_CONTRAST=1` | Ranks an RNTI's scanned identities by **contrast** = (share of its voted slots carrying the id) - (share of the cell's *other* voted slots carrying it), instead of raw votes. Ids are only reordered, never dropped, and the TB CRC still decides. An RNTI is "decided" (stops full scans) only when its best id has contrast >= 0.25. RNTIs that arrive while a slot's scan is running are voted when it completes. | A UE-specific id is over-represented in that UE's slots; a cell-wide or co-scheduled id is not. With a single UE (no other slots) contrast reduces to the vote share. When every UE shares one id (Vodafone, PCI), all ids score ~0 and the order falls back to the old one. 0.25 is a minimum effect size, not a fitted value. |
| `ISAC_PDSCH_SOLVER_FAIRSHARE=1` | **Work-conserving fair share** of the probe rate. While the probe bucket is contended (below its burst or the ring above its fill), each unsolved RNTI active in the last 0.2 s may plan at most its share `rate / n_active` (a per-RNTI bucket, capped at the per-grant budget). A new RNTI starts with a full budget. With spare capacity there is no cap and no debt. A full 64-RNTI table reuses the least recently active entry idle for >= 2 s of signal; that entry's in-flight probes are invalidated. | Equal share under contention, so no RNTI is starved by a busy RNTI that keeps failing, and no capacity is wasted otherwise. Bounded state under RNTI churn. |
| (CONTRAST, 13d9afd4fd) | While an unsolved RNTI has no UE-specific id (best contrast < 0.25), the cell default (PCI, PCI) stays in its scrambling cycle. | On a cell where every UE uses the PCI, a cell-wide neighbour/background id (249/442 on raw_pci421_aligned_20261006, seen in base runs too) could push the PCI out of the top-2. |
| `ISAC_PDSCH_SOLVER_WHY=1` | Diagnostics: per-RNTI reason counters (`gate budget0 throttle cap live noread plan csirs leader hog nlskip backoff wide probes p_ok p_fail p_nocred`), logged as `PDSCH_SOLVER why rnti=...` with the per-rnti report. | Explains why each RNTI was not attempted. |

Cost: the contrast ranking and the fair share run on the planning path of **unsolved** RNTIs only. The fair share is one scan of
64 entries under the existing solver mutex. Solved RNTIs pay nothing new on the decode path. The contrast table holds 512 ids
and is updated once per voted slot. TLS is unchanged (0x256438 = 2.34 MB).

An earlier version (in 6293f54932) applied the fair share always.
**[MEASURED, replay TIM 12 s, 0.25x, ablation]** Always-on fair share alone: 4 solved; contrast alone: 6 and 6 solved;
base: 6 and 7 solved. An always-on cap left capacity unused when there was no contention, so it became work-conserving in fc93702433.

## 3. Validation

**[OFFLINE VERIFIED, DGX unit tests via heavy.sh, @fc93702433; 13d9afd4fd changes only nr_pdcch_blind_monitor_rt.c]** `test_nr_pdsch_solver` 15/15 passed. New tests:
`FairShareSplitsTheProbeRateBetweenActiveRntis` (equal split under contention, no cap without contention) and
`FullTableReusesOnlyIdleEntries`. `test_nr_pdsch_dmrs_scan` 5/5 passed. New test: `ContrastRanksTheUesOwnIdentityAboveCellWideOnes`
(a background id and a busy co-scheduled id outvote a quiet UE's own id under raw votes; contrast puts the own id first and drops
nothing; the decided rule; a single UE keeps its id).

**[MEASURED, replay DGX GB10, t26r.env, 4 RX; base = csd/otatest 84c702792f binary, fix3 = fc93702433, fix4 = 13d9afd4fd,
flags as above]** Captures: t12 = raw_pci923_tim_12s_tile3 (TIM PCI 923), t4 = raw_pci923_tim_4s_1010_1853, vb = raw_pci415_12s_b,
v8 = raw_pci415_12s_20261008 (Vodafone PCI 415), p421 = raw_pci421_aligned_20261006 (4 s, PCI 415 grid, -C 3680.01 MHz, SSB 1171, FO 6900).
Columns: runs, then means over runs of RNTIs with DCI 1_1, RNTIs with >= 1 full-decode attempt (EVMGRANT), solved, CRC-OK RNTIs,
total CRC-OK full decodes, and the median over runs of the per-run median lag first DCI -> first CRC-OK (s, receiver time). The last
column lists every run. Other agents shared the host (heavy.lock serialises replays, not CPU-wide load), and run-to-run variance is
large, especially at 1x and on the 4 s captures.

| cap | speed | arm | n | DCI RNTIs | attempted | solved | CRC-OK RNTIs | CRC-OK | lag med | runs |
|---|---|---|---|---|---|---|---|---|---|---|
| t12 | 0.25 | base | 4 | 11.8 | 5.2 | 5.5 | 4.2 | 235 | 1.82 | dci12 sol6 ok4 crc192/381 lag1.98 ; dci13 sol7 ok6 crc455/717 lag2.23 ; dci10 sol5 ok4 crc227/391 lag1.65 ; dci12 sol4 ok3 crc67/131 lag0.28 |
| t12 | 0.25 | fix3 | 2 | 13.0 | 6.0 | 6.0 | 5.5 | 347 | 2.57 | dci13 sol6 ok5 crc332/654 lag2.90 ; dci13 sol6 ok6 crc362/688 lag2.24 |
| t12 | 0.25 | fix4 | 4 | 12.8 | 5.2 | 5.8 | 4.8 | 307 | 2.12 | dci12 sol5 ok4 crc182/370 lag2.21 ; dci13 sol6 ok6 crc351/656 lag3.17 ; dci13 sol6 ok4 crc342/645 lag1.94 ; dci13 sol6 ok5 crc352/687 lag2.02 |
| t12 | 1 | base | 3 | 10.3 | 3.7 | 3.7 | 2.7 | 156 | 0.28 | dci10 sol3 ok2 crc50/99 lag3.65 ; dci11 sol5 ok3 crc355/542 lag0.28 ; dci10 sol3 ok3 crc63/121 lag0.28 |
| t12 | 1 | fix3 | 2 | 10.0 | 4.0 | 4.0 | 4.0 | 234 | 0.35 | dci10 sol4 ok4 crc234/405 lag0.35 ; dci10 sol4 ok4 crc234/404 lag0.35 |
| t4 | 0.25 | base | 2 | 9.5 | 1.0 | 1.0 | 0.5 | 1 | 0.92 | dci10 sol2 ok1 crc2/8 lag0.92 ; dci9 sol0 ok0 crc0/0 lag- |
| t4 | 0.25 | fix3 | 2 | 10.0 | 2.0 | 2.0 | 1.0 | 2 | 0.88 | dci10 sol2 ok1 crc1/20 lag0.97 ; dci10 sol2 ok1 crc2/28 lag0.78 |
| t4 | 0.25 | fix4 | 2 | 10.0 | 1.0 | 1.0 | 1.0 | 1 | 0.98 | dci10 sol1 ok1 crc1/17 lag0.98 ; dci10 sol1 ok1 crc1/20 lag0.98 |
| t4 | 1 | base | 2 | 8.5 | 0.5 | 0.5 | 0.5 | 2 | 0.28 | dci9 sol0 ok0 crc0/0 lag- ; dci8 sol1 ok1 crc5/10 lag0.28 |
| t4 | 1 | fix3 | 2 | 7.5 | 2.0 | 1.0 | 2.0 | 4 | 0.27 | dci8 sol1 ok2 crc4/9 lag0.29 ; dci7 sol1 ok2 crc3/8 lag0.25 |
| vb | 0.25 | base | 4 | 44.8 | 28.5 | 28.2 | 26.0 | 1410 | 2.86 | dci44 sol27 ok26 crc1387/2920 lag3.67 ; dci45 sol23 ok21 crc1376/2784 lag2.83 ; dci44 sol34 ok32 crc1481/3349 lag2.89 ; dci46 sol29 ok25 crc1395/3677 lag2.79 |
| vb | 0.25 | fix3 | 2 | 44.0 | 33.5 | 33.0 | 31.0 | 1968 | 0.78 | dci44 sol33 ok32 crc2283/5612 lag0.43 ; dci44 sol33 ok30 crc1652/5432 lag1.12 |
| vb | 0.25 | fix4 | 4 | 44.0 | 30.8 | 30.0 | 28.0 | 1783 | 0.78 | dci43 sol35 ok33 crc2084/5118 lag0.31 ; dci46 sol28 ok26 crc1608/3974 lag0.79 ; dci44 sol28 ok27 crc1714/5109 lag1.39 ; dci43 sol29 ok26 crc1725/5351 lag0.77 |
| vb | 1 | base | 2 | 43.5 | 33.0 | 32.0 | 30.0 | 2189 | 0.77 | dci43 sol33 ok30 crc2198/5159 lag0.96 ; dci44 sol31 ok30 crc2180/5047 lag0.59 |
| vb | 1 | fix3 | 2 | 33.5 | 25.5 | 24.5 | 22.5 | 1564 | 0.47 | dci23 sol17 ok16 crc1055/2022 lag0.57 ; dci44 sol32 ok29 crc2072/4949 lag0.38 |
| v8 | 0.25 | base | 4 | 4.0 | 4.0 | 4.0 | 4.0 | 682 | 0.57 | dci4 sol4 ok4 crc658/971 lag0.57 ; dci4 sol4 ok4 crc694/1064 lag0.25 ; dci4 sol4 ok4 crc685/980 lag0.86 ; dci4 sol4 ok4 crc693/1011 lag0.57 |
| v8 | 0.25 | fix3 | 2 | 4.0 | 4.0 | 4.0 | 4.0 | 651 | 0.54 | dci4 sol4 ok4 crc596/832 lag0.97 ; dci4 sol4 ok4 crc706/1225 lag0.11 |
| v8 | 0.25 | fix4 | 4 | 4.0 | 4.0 | 4.0 | 4.0 | 486 | 0.54 | dci4 sol4 ok4 crc682/1115 lag0.23 ; dci4 sol4 ok4 crc340/495 lag0.22 ; dci4 sol4 ok4 crc638/945 lag0.91 ; dci4 sol4 ok4 crc286/410 lag0.85 |
| v8 | 1 | base | 2 | 4.0 | 4.0 | 4.0 | 4.0 | 606 | 1.02 | dci4 sol4 ok4 crc586/806 lag0.91 ; dci4 sol4 ok4 crc625/969 lag1.14 |
| v8 | 1 | fix3 | 2 | 4.0 | 4.0 | 4.0 | 4.0 | 522 | 1.56 | dci4 sol4 ok4 crc665/937 lag0.87 ; dci4 sol4 ok4 crc379/542 lag2.25 |
| p421 | 0.25 | base | 4 | 11.8 | 3.8 | 3.2 | 2.8 | 45 | 0.33 | dci18 sol4 ok4 crc108/296 lag0.33 ; dci8 sol3 ok3 crc45/139 lag0.70 ; dci8 sol0 ok0 crc0/4 lag- ; dci13 sol6 ok4 crc28/80 lag0.30 |
| p421 | 0.25 | fix3 | 2 | 13.0 | 2.5 | 1.5 | 1.5 | 28 | 0.49 | dci8 sol3 ok3 crc56/154 lag0.49 ; dci18 sol0 ok0 crc0/4 lag- |
| p421 | 0.25 | fix4 | 4 | 11.0 | 4.0 | 3.5 | 2.5 | 44 | 0.60 | dci15 sol3 ok2 crc17/65 lag0.72 ; dci9 sol3 ok3 crc54/173 lag0.49 ; dci10 sol4 ok2 crc50/180 lag0.93 ; dci10 sol4 ok3 crc54/151 lag0.49 |
| p421 | 1 | base | 2 | 8.0 | 3.5 | 2.5 | 2.0 | 59 | 0.62 | dci10 sol5 ok4 crc118/258 lag0.62 ; dci6 sol0 ok0 crc0/4 lag- |
| p421 | 1 | fix3 | 2 | 5.5 | 1.0 | 0.0 | 0.0 | 0 | nan | dci4 sol0 ok0 crc0/4 lag- ; dci7 sol0 ok0 crc0/4 lag- |
| p421 | 1 | fix4 | 2 | 7.5 | 2.5 | 1.5 | 1.5 | 16 | 0.48 | dci9 sol3 ok3 crc32/161 lag0.48 ; dci6 sol0 ok0 crc0/4 lag- |

**Pooled CRC-OK % by MCS bucket at 0.25x (base -> fix4), and decode stage means (us, fep/chest/demod/ldpc):**

| cap | 0-4 | 5-9 | 10-16 | 17+ | stages base -> fix4 |
|---|---|---|---|---|---|
| t12 | 61 %/566 -> 54 %/822 | 45 %/540 -> 44 %/713 | 74 %/443 -> 62 %/743 | 41 %/71 -> 9 %/80 | 137/281/484/262 -> 107/242/528/285 |
| t4 | 40 %/5 -> 8 %/25 | 0 %/3 -> - | - -> 0 %/2 | - -> 0 %/10 | 94/172/313/114 -> 121/415/507/153 |
| vb | 50 %/10630 -> 41 %/16661 | 20 %/1749 -> 12 %/2222 | 2 %/323 -> 2 %/624 | 0 %/28 -> 0 %/45 | 128/185/358/238 -> 120/181/371/249 |
| v8 | 78 %/3238 -> 74 %/2411 | 25 %/536 -> 30 %/362 | 23 %/153 -> 27 %/113 | 33 %/99 -> 16 %/79 | 249/532/464/357 -> 222/398/446/265 |
| p421 | 65 %/226 -> 66 %/205 | 20 %/178 -> 16 %/246 | 0 %/115 -> 0 %/117 | - | 369/950/1185/369 -> 432/1029/1350/455 |

Reading the results (all per capture; nothing is pooled across captures):
- **TIM 12 s (t12), 0.25x:** CRC-OK full decodes went from a mean of 235 to 307 over 4 runs each, and the run minimum from 67 to
  182. The solved and CRC-OK RNTI counts did not change: 5.5 -> 5.8 and 4.2 -> 4.8. The RNTIs still unsolved (0x4e32, 0x5098,
  0x5a94) are probed with their right identity hundreds of times (PROBELOG: 0x5098 got 276 probes with 638, 0x4e32 got 464 with
  1677) and still fail. The TIMPDSCH report called this **[HYPOTHESIS]** SNR-limited. This capture has only ~10 DCI RNTIs, so it
  cannot reproduce the 45-RNTI, 150 s OTA starvation.
- **Vodafone b (vb), 0.25x:** solved went from 28.2 to 30.0, CRC-OK RNTIs from 26.0 to 28.0, and CRC-OK decodes from 1410 to 1783
  (+26 %). The median lag fell from 2.86 s to 0.78 s. The pooled CRC % per bucket is lower because more, and weaker, RNTIs are now
  decoded (more decodes in the denominator); this is not a per-RNTI loss.
- **Vodafone 8 (v8):** 4/4 RNTIs solved and CRC-OK in every run of both arms. The CRC-OK total was lower in 2 of the 4 fix4 runs
  (340 and 286, against 658-694 for base). In those runs the PDCCH saw far fewer DCIs for the same RNTIs (fix4 d: 0xe01e had 57 DCIs
  against 603; 0x13ae had 31 against 392). Decodes per DCI were unchanged: 0x8f5a got 325 EVMGRANT out of 465 DCIs, against 325
  of 462 for base. So the loss happened upstream of the PDSCH planner that these flags change (DCI detection variance). The cause
  was not proven in this session.
- **p421 (4 s):** solved 3.2 -> 3.5, CRC-OK RNTIs 2.8 -> 2.5, CRC-OK 45 -> 44 over 4 runs each: no change beyond the large
  variance (one base run had 0 solved). At 1x, 1 of 2 fix4 runs and 1 of 2 base runs solved nothing, from late acquisition.
- **TIM 4 s (t4):** ~1-2 RNTIs are solved in 4 s in either arm, so this capture is not discriminative.
- 1x (fix3 = fc93702433, without the default hypothesis): t12 solved 4 and 4 (base 3, 5, 3), CRC-OK 234 and 234 (base 50-355).
  vb: one run acquired late (23 DCI RNTIs), the other run matched base. p421: 0 and 0 (base 5 and 0), which led to 13d9afd4fd.
  fix4 at 1x was run on p421 only: 3 solved and 0.


## 4. Risks and open points

- **No OTA validation, and no long replay.** The OTA symptom (45 RNTIs, 150 s, 22-34 % of queue jobs dropped stale) is larger
  than any capture here. The 12 s TIM capture has about 10 RNTIs, and its unsolved ones look SNR-limited. The gain on the OTA
  starvation is therefore **[HYPOTHESIS]** until an OTA A/B (t26r vs t26r + `ISAC_PDSCH_DMRS_ID_CONTRAST=1
  ISAC_PDSCH_SOLVER_FAIRSHARE=1`) is run.
- **Variance.** There are n = 2-4 runs per arm on a shared host, and many ranges overlap. Only these results are consistent in
  every run: vb at 0.25x (CRC-OK and lag better in 4/4) and the minimum CRC-OK on t12 at 0.25x.
- **v8 CRC-OK total.** It was halved in 2 of the 4 fix4 runs. The traced cause is fewer DCIs seen (upstream of these flags), but
  it is unexplained. Re-check it in the OTA A/B.
- **Contrast semantics.** When every UE shares one identity (Vodafone), contrast is ~0 for all ids. The order is then close to
  arbitrary, which is why the cell default is kept (13d9afd4fd). If two UEs share a UE-specific id, its contrast is lower but
  still positive. The 0.25 decided threshold and the 0.2 s activity / 2 s idle windows are general constants, not fitted to a
  capture, but they are untuned.
- **More full DM-RS scans.** RNTIs with only cell-wide ids are no longer "decided", so they keep requesting full 16-bit scans. The
  scans stay rate-limited (16/s) on the async worker core, so decode cores are not affected. Pool checks rose a little.
- **Fair share** caps only under contention. On the OTA, where the ring is often full, a hopeless busy RNTI now gets 1/n of the
  probe rate instead of most of it. Solvable RNTIs get more probes, but the total probes are unchanged.
- **Table reuse** drops an idle (>= 2 s) solved RNTI's solution. If that RNTI returns, it must solve again (with the reuse prior,
  this took a few grants).
- **Flags off = unchanged** is **[CODE-READ]** only: every new path is gated on its flag, the diagnostic counters are
  increments, and `note_skip` creates no table entry without `ISAC_PDSCH_SOLVER_WHY=1`. The new binary was not replayed with the
  flags off; the "base" arm is the csd/otatest binary.
- Not done: a per-RNTI weighting by need (for example SNR or MCS), PROBELOG on OTA, and a 1x matrix of the final fix4 on every
  capture (fix4 ran at 1x on p421 only; fix3 covers 1x for the others).
