# Final whole-branch review: full-running-agnosticity

Branch `adaptive-rx-UL-DL`, range `222f98d072..3eb1b070c3` (56 commits, 59 files, +6827/-795).
Reviewed on 2026-09-26 from the pre-generated diff, in passes: CMake and headers; CSI-RS (T1-T3, T16); DM-RS ID and
scrambling (T13); AL1 and lanes (T4, T5, T15); `nr_pdcch_blind_monitor_rt.c`, 2k diff lines; DCI layouts and FDRA (T10);
Technique D (T6, T14); PDSCH decode and queue (T9, T11, T12, T18); PUSCH (T13 UL, T10 UL); tests (skimmed). I checked the
cross-task claims with read-only greps and `nm` on sens6 at HEAD `3eb1b070c3`. I ran nothing and built nothing.
Line numbers below are at HEAD.

## Strengths

- **The pure modules are small, well-bounded and tested by their properties.** `nr_pdsch_prb_set.c` (RBG, dynamicSwitch,
  interleaver f(j), PRG segments, gather) matches TS 38.211/38.214 arithmetic. I checked the interleaver permutation and
  the bundle sizes of the first and last bundle. `nr_pdcch_al1_map.c` is exhaustively tested against all legal shapes.
  `nr_scrambling_id_sweep.c` and `nr_arm_sweep.h` are minimal.
- **Memory discipline improved where it mattered most.** The resolvers (2x17.2 MB) and the Technique D context states
  (up to 185 MB) moved from mlocked BSS to heap-on-first-use (R19, R23). The per-layout TLS arrays moved to heap-backed
  thread-locals, which removes the known AVX-alignment trap. Every large new buffer carries its size in a comment.
- **Real latent bugs were fixed along the way:**
  - `layout_index` now defaults to 0xFFFF, so SIB1 and 1_0 passes are no longer credited to stage-2 layout 0.
  - CSI-RS rows 3 and 5 now get all their port planes cleared.
  - The DM-RS oracle now indexes carrier CRBs (T18).
  - The PUSCH `(2,12)` row was relabelled to type B.
  - The `null_median()` log now reports the real median.
  - Evidence-protected RNTI context eviction.
  - The lifetime-counter data-ID latch bug from the T13 review.
- **The contiguous PDSCH path is untouched when the decode is not segmented.** Every new branch in
  `nr_pdsch_passive_decode()` is guarded by `seg_path`, and the queue's slot-share union excludes list and PRG jobs
  (`CONTIG_JOB`). The flaw is that T12 makes `seg_path` the common case; see C1.
- **Layering work (R14, fix2) is real.** The CORESET bank, `nr_tdd_pattern` and the UL-ID state moved into the library
  intact. One residual dependency remains (I7).
- **Thread-safety work is mostly careful.** Resolver appends are published with release stores. The AL1 union fields
  sit under `g_al1_mu`. `rnti_dec_t` is under `g_ptrs_lock`. Stage-2 DM-RS accumulates use trylock off the RT thread.

## Issues

### Critical (Must Fix)

**C1. The T12 PRG sweep is on by default and diverts about 2/3 of all PDSCH decodes onto the segmented path from the
first grant, with no evidence. That makes channel estimation roughly 100x more expensive and strips per-grant CFO/SFO
correction.** `nr_pdsch_passive_decode.c:1379` (pick) and `:1926` (per-segment estimation loop).

- **What is wrong.**
  - `rnti_prg_pick()` runs on every decode unless the GPU path is active. No `ISAC_*` knob gates it, and there is no
    agnostic-v2 gate.
  - `nr_arm_sweep_pick()` with no trials breaks Wilson-upper-bound ties by the fewest trials. So the picks go arm 0, then
    arm 1 (prg=2), then arm 2 (prg=4), from the very first grants. I verified the arithmetic: a 1/1 arm has
    Wilson hi = 1.0, which ties an untried arm.
  - The plan says: "Arm 0 (wideband) = today's behaviour and the default until the sweep has evidence".
- **What it costs.**
  - Channel estimation cost:
    - At prg=2 a 273-PRB grant becomes 137 segments.
    - Each segment calls `nr_pdsch_channel_estimation()`, per DM-RS symbol and per layer.
    - Each call regenerates pilots from CRB 0 up to the end of its segment, which is quadratic in total.
    - Each call memsets a whole `ofdm_symbol_size` row per antenna.
    - With more than one RX antenna, each call pushes and joins `nb_antennas_rx` thread-pool tasks, so hundreds to more
      than 1,600 pool round-trips per decode.
    - The whole contiguous decode had a measured budget of about 775 µs; this is orders of magnitude more.
  - Correction lost on `seg_path`:
    - DMRSFO per-branch CFO (`:2220`), the SFO correction, the slot chest cache (`:1873`), the PTRS sweep, and the GPU
      front end are all bypassed.
    - Grants with PT-RS return UNSUPPORTED (`:1480`).
    - Contiguous grants with an odd `first_rb` and CSI-RS rate matching are refused (`:1530`, the T9 deferred minor).
    - An UNSUPPORTED arm is never fed back, so it keeps being picked.
  - When the path runs:
    - The in-line decode path (defer off) runs all of this on the PHY or scan thread.
  - Where it never ends:
    - On a cell where every arm decodes about equally (SISO, or wideband precoding), the Wilson intervals never separate.
      The sweep never latches, and the diversion is permanent.
  - Where the evidence gets reset:
    - `rnti_dec_t` is a 16-slot pure LRU (I4), so noise RNTIs reset the sweep repeatedly.
  - Effect on the other sweeps:
    - Every other sweep on the same TB-CRC stream sees extra, uncorrelated failures: Technique D, stage-2 layouts, FDRA
      refutation, the data-ID stall counter, and the Qm oracle.
- **Why it matters.** This is the lab cell's default decode path: type 1, 273 PRB, 4 RX antennas. It very likely
  collapses consumer throughput (dropped_full / dropped_stale) and lowers CRC. It violates the Review Focus item
  "Segmented decode changes the contiguous path" and the Global Constraint that discovery-order changes are opt-in.
- **Fix.**
  - Gate the sweep behind `ISAC_PRG_SWEEP=1`, default off, or explore arms 1 and 2 only after arm 0 shows a sustained CRC
    deficit. A reasonable gate is multi-layer or precoded grants whose arm-0 rate stays below the cell's recent rate.
  - Do not run it on DCI 1_0. TS 38.214 5.1.2.3 fixes the PRG at 2 for 1_0.
  - Longer term, estimate once over the whole allocation with PRG interpolation boundaries instead of one estimator call
    per segment.
  - Add the missing dual-bandit test (VRB-L + PRG on one CRC stream) with this change.

### Important (Should Fix)

**I1. The data-scrambling-ID discovery cannot fire on the case it exists for, but it does fire on ordinary link outages
and then walks off the correct ID with no log.** `nr_pdcch_blind_monitor_rt.c:6027-6030`, `nr_pdsch_passive_queue.c:991`,
`nr_pdcch_blind_monitor.c:4416`, `nr_pusch_passive_ul_ids.c:103`.

- **DL.**
  - The walk may advance only when `nr_pdsch_config_sweep_rnti_prior_get()` returns true. The per-RNTI prior becomes
    valid only when a Technique D context converges on TB-CRC passes (`nr_pdsch_config_sweep.c` around line 954).
  - With a wrong `dataScramblingIdentityPDSCH`, no CRC ever passes, so the prior is never set and the walk never
    advances. The gate is circular. This is a plan defect: the Review Focus wording produced it.
  - The gate does open for an already-converged RNTI after any 20-fail streak: a CFO mis-lock, an RFSTALL, a fade, or
    first-code-block layout probes, which count as failures. The walk then leaves the PCI on the first failed trial.
    After the link recovers, every grant uses a wrong ID until the walk wraps back to the PCI, up to about 1023 TBs per
    RNTI.
  - There is no log for the walk starting, advancing or latching.
  - The failure counter is updated only on the queue path, so RNTIs decoded in-line never become eligible. This was the
    T13 deferred item.
- **UL.**
  - The gate is only "20 consecutive UL failures, cell-wide". It has no convergence gate, which is the exact Review Focus
    hazard: "burns decodes on a config mismatch".
  - UL width discovery and MCS-limited stretches trip it on healthy cells. Every grant then uses a walking ID, including
    DCI 0_0, which by spec must use the PCI in the CSS. UL-discovery feedback is poisoned for up to about 1000 grants.
- **Fix.**
  - Gate advancement on "link demonstrably healthy while this class fails". That means PCI-scrambled 1_0 or 0_0 CSS
    grants passing in the same window, the same idea as `dl_link_ok`.
  - Do not require Technique D convergence. Accept a DM-RS-oracle-pinned (S,L,mask) plus a Qm-consistent table instead.
  - Never apply the walking ID to CSS or 1_0/0_0 grants.
  - Log the start and each wrap, and update the failure counter on the in-line path too.

**I2. The decided DM-RS ID and the latched data ID are applied to grants for which the spec requires the PCI, and the
DM-RS accumulator mixes both populations.** `nr_pdcch_blind_monitor_rt.c:6015`, `nr_pdcch_blind_monitor.c:4410-4421`,
`nr_pdsch_passive_queue.c` (accumulate block).

- scramblingID0/1 and `dataScramblingIdentity*` apply only to C-RNTI (and similar) grants outside the CSS 1_0/0_0 cases.
  SI-, RA-, P-RNTI, TC-RNTI and Msg3 always use N_ID^cell.
- The code applies `dl_dd->best_id` and the latched data ID to every DL job, including SIB1, RAR and paging, and to every
  UL grant including 0_0.
- The DL accumulator now takes every decode attempt, including SIB1/RAR, which are PCI-scrambled, alongside 1_1 grants,
  which carry scramblingID0.
- On a cell whose scramblingID0 is not the PCI:
  - The estimator may decide correctly for 1_1 and then silently break SIB1, RAR and paging decoding.
  - That also removes the SIB1 "link health" signal that FDRA staging and the proposed I1 fix rely on.
- Harmless on the lab cell (PCI). Wrong on exactly the cells T13 targets.
- **Fix.**
  - Key the application on RNTI class and search space.
  - Accumulate only dedicated-class grants: 1_1, or 1_0 with C-RNTI in the USS.

**I3. FDRA staging's DL retirement is irreversible and can be triggered by failures that have nothing to do with FDRA.
The header still says a false refutation is harmless.** `nr_pdcch_dci11_layout_sweep.c:451`, comment at
`nr_pdcch_dci11_layout_sweep.h:212`, gate at `nr_pdcch_blind_monitor_rt.c:1008`.

- **What happens.**
  - `arm_next_mode()` retires every live layout without a pass (R22b), including all type-1 layouts.
  - `disarm()` can revive a type-1 layout only after a type-1 pass. Stage 2 never offers dead layouts, so that pass cannot
    arrive, and the true layout is gone for the rest of the run.
- **Why it triggers wrongly.**
  - "Link healthy" is any non-stage-2 PDSCH pass, and SIB1 (1_0, low MCS, PCI-scrambled) qualifies. So several
    situations refute a correct type-1 layout set:
    - 1_1 grants that are MCS/rank/LBRM-limited (the rank-4 and MCS-25 history in memory).
    - 1_1 grants failing because of scrambling (I1, I2).
    - 1_1 grants degraded by the PRG sweep (C1).
- **Scope.** DL staging is active only under `ISAC_DCI11_STAGE2` or `ISAC_AGNOSTIC_V2`, which the agnostic runs use.
- **UL.** The same misattribution appears on the UL: "0_1 type-1 PUSCH 0/64 with 0_0 passing" becomes UL_FDRA_REFUSED
  (63 of 64 0_1 bookings refused). A data-scrambling-ID mismatch that affects only 0_1 produces exactly that signature,
  and I1's UL gate never opens because 0_0 keeps resetting the failure counter. The log then blames FDRA.
- **Fix.**
  - Keep refuted stages alive but deprioritised instead of dead. At minimum, let a later type-1 pass on any
    type-1 layout, offered by periodic probing, revive the set.
  - Correct the header comment.
  - Require link health from the same RNTI class (dedicated grants), not from SIB1.

**I4. The per-RNTI hypothesis state in `rnti_dec_t` is a 16-slot pure LRU that noise RNTIs and SI/RA/P RNTIs churn, and
T11-T13 made the churn worse.** `nr_pdsch_passive_decode.c:335`, callers at `nr_pdcch_blind_monitor_rt.c:6029` and the
`nr_pdsch_vrbl_pick` call.

- `rnti_dec_t` now holds the PTRS, VRB-L, PRG and data-ID sweeps, about 2.1 KB, memset on every eviction.
- `data_id_current()` and `vrbl_pick()` call `rnti_dec()` from the scan thread for every DL grant, including SI-RNTI,
  P-RNTI, RA-RNTIs and false accepts.
- T1 measured and fixed exactly this defect for `rnti_ctx` (the real RNTI evicted 5 times in 200 s at 16 slots, now 64
  slots with evidence protection). It was not carried over here.
- Consequence: latched VRB-L, PRG, PTRS and data-ID decisions are lost and re-swept. With C1 active, each eviction
  restarts the lossy PRG exploration.
- **Fix.** Reuse the `rnti_ctx` policy: protect slots carrying a latch or evidence, raise the size, and do not create
  entries for non-C-RNTI classes.

**I5. The DM-RS ID stage-2 escalation is one-way and fires after 64 undecided grants, whatever the cause.**
`nr_pdsch_passive_queue.c:1112`, UL at `nr_pusch_passive_decode.c:1005` onward.

- Early grants include acquisition garbage, CFO mis-lock periods, jobs from false-accept DCIs, and grants under wrong
  Technique D masks, which read a data symbol as DM-RS.
- If stage 1 fails to decide within those 64 grants, the window moves permanently to 1024..65535.
- A true ID in 0..1023 (PCI or not) then becomes undecidable for the rest of the run. Each consumer also pays about
  170 ms per 2048 jobs plus a 64k-entry median sort, forever.
- On the lab cell this is only a CPU cost, because an undecided ID falls back to the PCI.
- **Fix.** Alternate windows, or restart stage 1 after a stage-2 budget, and gate escalation on link health.

**I6. The DM-RS symbol oracle and the new k0 probe still score coherence with `fp->Nid_cell`, not the decided DM-RS ID.**
`nr_pdsch_passive_queue.c:495`.

- On a scramblingID0-not-PCI cell, the mask oracle never fires. Type B (default on by R20, justified as "the oracle
  prunes type B on the first observation") is then never pruned, so Technique D converges about 2.9x slower permanently.
- The k0 probe runs on 1 in 8 unsettled jobs, doing up to 18x14 FFTs plus waits, and never hits.
- This is a T13 x T14 x R20 interaction no single task review could see.
- **Fix.** Pass the job's `dlDmrsScramblingId` through `dmrs_oracle_measure()`.

**I7. Layering: the `nr_pdcch_blind_monitor` library still depends on PHY_NR_UE symbols.**
`nr_pusch_passive_ul_ids.c:26`.

- `nm` on HEAD's `libnr_pdcch_blind_monitor.a` shows `nr_scrambling_id_sweep_{init,current,feed}` undefined in the
  library and defined only in `libPHY_NR_UE.a`.
- The test target compiles `nr_scrambling_id_sweep.c` directly to hide this (CMakeLists comment near line 2385).
- It is the same class as the inversion R14 and fix2 were meant to remove, though small: the module is pure.
- **Fix.** Add `nr_scrambling_id_sweep.c` to the library's source list. PHY_NR_UE users link the library in the same
  `--start-group`; if an executable does not, keep it in both lists, since it is pure and ODR-identical.
- (`nr_pdcch_dmrs_ref` is also external, but that dependency predates this branch.)

**I8. The real-time and scan thread now take more global locks per grant, and new-context creation got about 3x more
expensive.**

- `nr_pdcch_blind_monitor_rt.c:6027-6030` now takes the Technique D `g_lock` (`rnti_prior_get`) and `g_ptrs_lock`
  (`data_id_current`, `vrbl_pick`) for every DL grant, including 1_0.
- Consumers hold `g_lock` during `observe`/prune and during the probation catalog rebuild (`context_catalog` → about 6.3k
  legality calls with type B).
- New contexts in `nr_pdsch_config_sweep_select()` do a 180 KB `calloc` under `g_lock` (`nr_pdsch_config_sweep.c:731`).
  Under `mlockall(MCL_FUTURE)` that faults and locks the pages immediately, then runs a type-A+B `init_legal`, all on
  the scan thread.
- When the PDCCH scan is not deferred, this is the PHY receive thread.
- **Fix.**
  - Compute `dl_data_advance` only for dedicated grants (I2).
  - Cache the prior bit in a lock-free per-RNTI atomic.
  - Preallocate or recycle context states outside the lock.

**I9. The plan's coverage matrix overstates what landed.**

- "PUSCH RA type 0 / dynamicSwitch (9, 10)": RA type 0 PUSCH is detected and refused, never decoded (R11 moved it and
  T10 refuses it).
- "PUSCH TDRA type B (14)": only (0,4) and (2,12) (recorded gap).
- "PDSCH/PUSCH data scrambling ID (13)": non-functional per I1.
- These should be marked partial in the plan and tracked, so that Task 17 does not treat them as covered.

### Minor (Nice to Have)

- **M1.** `nr_dmrs_id_decide()` (`nr_dmrs_id_estimate.c:230`) publishes `best_id` then `decided` with plain stores, and
  the scan thread reads them unlocked (`rt.c:6015`, `blind_monitor.c:4412`). A compiler reorder could expose
  `decided=1` with `best_id=-1`, which applies 65535 for one grant. Use a release store on `decided` and an acquire load
  on the reader.
- **M2.** `nr_dmrs_id_init()` memsets the struct before `set_range()` frees the arrays, so re-initialising a live state
  leaks. No caller re-inits today; guard it anyway.
- **M3.** Qm oracle: two agreeing misreads prune the true MCS table irreversibly for that context. A later contradiction
  only resets the counters. Consider restoring the catalog on contradiction or requiring more sightings.
- **M4.** `nr_dmrs_port_pair_coherence()` rejects `nid >= 1024`, so the rank and CDM probes go silent once a stage-2 ID
  is decided.
- **M5.** Memory under `mlockall`:
  - Static BSS added: about 2.5 MB.
    - `nr_pdcch_dci11_layout_sweep.c` statics `off` (704 KB), `hyp`, `score` and `sorted` (256 KB each).
    - `g_dci01_off` 176 KB, `g_rnti_fails_since_ok` 256 KB, the bank AL1 fields about 104 KB.
  - Heap that is locked on touch, worst case about 245 MB (about 6% of RLIMIT_MEMLOCK):
    - Up to 185 MB of never-freed Technique D states.
    - 34 MB of resolvers.
    - About 6 MB of stage-2 DM-RS arrays.
    - Per consumer thread: about 1.2 MB (`virt` 917 KB, `t_probe` 229 KB, `seg_h`), allocated immediately because of C1.
    - About 0.5 MB of stage-2 scratch per scan thread.
    - 3.5-7 MB of lane vectors per scan thread.
  - No memlock budget code exists on this branch; budget for it before the sensing engine lands here.
- **M6.** T3/T1 interaction (opt-in IDSWEEP): the best-z search keeps running pass after pass, so its noise maximum keeps
  growing. An eventual false "SOLVED" now overwrites the candidate's `scramb_id` (T1 write-back) and sets
  `g_id_solved` forever. Before T1 a false solve was print-only. Cap the passes, or revert the ID when the confirm budget
  expires unconfirmed.
- **M7.** The DL/UL FDRA and scrambling comments describe `dl_link_ok` as "link healthy". Say explicitly that it counts
  1_0/SIB1 passes.
- **M8.** The CSI-RS footprint ring (`fp_record`) is order-free and `infer_period` is mod-based, so it is correct. But
  one spurious "on" hit poisons that cell's periodicity for the next 8 hits. That is acceptable, and could be documented.

## Default-on behaviour changes with no ISAC_* knobs (lab cell: type 1, 273 PRB, AL2, BWPStart 0)

1. **PRG sweep:** about 2/3 of PDSCH decodes take the segmented path (C1).
2. **DL DM-RS ID:** accumulated on every decode attempt, no longer CRC-gated. Stage-2 escalation after 64 undecided
   grants. The decided ID is applied to all DL grants. Expected outcome on this cell: decides the PCI, so no functional
   change; CPU cost only if it escalates.
3. **UL DM-RS ID:** the same, accumulated before `nr_ulsch_decoding` on every attempted grant (queue consumer only). The
   decided ID is applied to all UL grants, including 0_0.
4. **DL per-RNTI data-ID walk:** armed for converged RNTIs after 20 consecutive failures (I1).
5. **UL cell-wide data-ID walk:** armed after 20 consecutive UL failures, and applied to 0_0 too (I1).
6. **UL DCI 0_1 FDRA staging:** arms type 0/dynamic after 0/64 oracle-class 0_1 PUSCH with the link healthy. From then on
   it refuses 63 of 64 oracle-class 0_1 bookings until one type-1 pass arrives.
7. **Technique D type-B catalog:** default on (R20). k0 ≥ 2 probe on 1 in 8 unsettled jobs with no DM-RS in the DCI
   slot (R20). k0 wait bound grows for k0 ≥ 2 jobs.
8. **Qm oracle:** prunes the MCS table after 2 agreeing reads (plan: default on).
9. **Technique D bookkeeping:** class count = live n_hyp instead of the storage cap. RNTI contexts 16 → 64 with evidence
   protection. `prune_to` keeps its evidence when nothing moves.
10. **`sweep_ticket.layout_index = 0xFFFF`:** SIB1 and 1_0 passes are no longer credited to layout 0 and now count as DL
    link health. The in-line path now feeds layout feedback.
11. **Interleaved VRB→PRB:** applied whenever the DCI's bit is 1, with a per-RNTI L sweep for 1_1. This is a bug fix;
    it is inert if the gNB never interleaves.
12. **DCI 1_1 resolver:** cap 2048 → 32768, so no more silent truncation of type-1 layouts. Stage-1 survivor counts
    change and stage-2 hand-over timing shifts, but only with stage 2 enabled.
13. **Joint solver:** branchless, result-identical (golden hash).
14. **CSI-RS:** rows 3/5 get correct port planes. The null-median log is fixed. CSI-RS pinning is inert without
    IDSWEEP/WIDE.
15. **PUSCH interp catalog:** (2,12) is now type B.
16. **Lane AL16:** accepted from the SIB1 CSS prior. Lanes themselves remain opt-in (`ISAC_PDCCH_EXTENT_BATCH`).
17. **Data structures:** `freq_alloc_bitmap_t` +556 B (common/utils, also in `defs_gNB.h`). The job struct grew (R13:
    old replay captures are invalid).

## Triage of deferred items and gaps (`final-review-ledger-items.md`)

| Item | Verdict | Reason |
|---|---|---|
| T8 dynamic_split lacks the <32 shift guard | can wait | inputs bounded (N_RBG ≤ 18, RIV ≤ 16 bits) |
| T8 type0_prbs truncates silently / segments returns -1 | can wait | callers pass NR_PRB_SET_MAX ≥ BWP size; unreachable |
| T4 narrow doc omits the 16-observation cap | can wait | doc only |
| T4 family_count merges invalid candidates | can wait | unreachable from callers |
| T2 no LOG_E on n_ports guard; report wording | can wait | unreachable guard |
| T6 mask observe prunes before feedback (stale tickets) | can wait | a real prune happens about once per context and clears evidence; a winner needs ≥ 64 trials. A generation bump is cheap; do it with I8 |
| T3 pin guard clauses untested | can wait | opt-in path |
| T5 al1_only padding; no stage-transition test | can wait | opt-in |
| T9 DMRSFO skipped on segments; CSI-RM parity refusal; RE-count frame mix; commit title | **must-fix if C1 is resolved by keeping PRG default-on**; can wait if PRG is gated off | these are what make the default PRG arms lossy and starve an arm |
| T9 stale nvar comment; flush-before-refusal; normalise untested; coh[] BWPStart | can wait (coh[] closed by T18) | cosmetic or benign |
| T16 symbol_on per-RE `%`; wide rows never picked by IDSWEEP; row 9 and twins | can wait | opt-in |
| T15 GPU buffers doubled; DMRS ranking off with virtual CCEs; no rt tests; slow AL1 lap | can wait | opt-in (AL1 union needs ISAC_AL1_COVER); GPU is device memory |
| T16 loops keep running after the cap | can wait | perf, opt-in |
| T15 opt-in DMRS gate scores virtual -INF | can wait | opt-in |
| T10 in-thread chest uses the first run; resolver factoring | can wait | SNR gate / DM-RS CFR only |
| GAP: PUSCH type B mostly undiscoverable | can wait (mark partial, I9) | eMBB PUSCH is overwhelmingly type A |
| T14 k0_seen per-RNTI; probe wait unmeasured; hits>>2 | can wait | kill switch exists; measure on the first real run (see I6) |
| Merge: ORACLE_GATE log prints raw num_rbs | can wait | log |
| Fix lane: orphan comment fragment | can wait | comment |
| T10 dl_link_ok underflow / **report overstates false-refutation mitigation** / UL discovery 0_1 never refused | **must-fix** (the false-refutation part, I3); others can wait | retirement is destructive and irreversible |
| T11 no nrvrbl unit test; no non-interleaved pin | can wait | arm_sweep core is tested |
| T12 trivial SmallAdvantage test; no static_assert; dual bandit untested | can wait (add the dual-bandit test with C1) | |
| T13 data-ID eligibility counter only on the deferred path | **must-fix** (fold into the I1 redesign) | in-line RNTIs can never become eligible |

**Counts:** must-fix 2 (the T10 false-refutation item, and the T13 in-line eligibility item). T9 is conditionally
must-fix, only if C1 is fixed by keeping PRG default-on. Can wait: 20.

## Declined to judge

- The VRB→PRB interleaver anchor for DCI 1_0 in the CSS (`il_bwp_start = 0` on the CORESET#0-relative grid). I could not
  confirm the exact TS 38.211 7.3.1.6 wording in this session, and the lab CORESET#0 sits at CRB 1, which makes the anchor
  matter. The executor should check it against the spec text.
- Whether T7 is bit-identical. I relied on the golden-hash test and did not re-derive the GF(2) elimination.
- GPU kernel sizing (`NPC_MAX_E` 2048, the lane stride). There was no GPU review scope.
- The algorithmic correctness of the AL1 cover and union, and of the rows 6-18 footprint matcher. Both are pinned by
  exhaustive tests and are opt-in.
- The Qm oracle's statistical thresholds. T6's tests cover them.
- Test pass status. I did not build or run ctest; the ledger reports 110/112, with 2 unrelated pre-existing failures.
- Whether 245 MB of worst-case locked heap fits a specific budget. This branch has no memlock budget code.
- How well OCUDU's scheduler matches the new hypotheses (PRG, interleaving). Not verifiable from the YAML baseline.

## Recommendations

1. Fix C1 first. It is the only change that degrades the lab cell's default path, and it contaminates every TB-CRC-driven
   sweep, which makes any Task 17 A/B uninterpretable.
2. Redesign the scrambling-ID application and eligibility together (I1, I2, I5, I6) around one rule: dedicated-class
   grants only, and "the link is healthy while this class fails", measured on PCI-scrambled CSS grants.
3. Make FDRA staging non-destructive (I3). A wrong refutation should cost time, not the answer.
4. Carry T1's evidence-protected LRU into `rnti_dec_t` (I4).
5. Before Task 17, add one integration check: a replayed lab capture run with and without the branch's defaults. It
   should show no CRC or throughput regression on the type-1 path.

## Assessment

**Ready to merge?** With fixes.

**Reasoning:** The modules are well built and well tested, and the memory and layering work is a real improvement.
But T12's default-on PRG sweep changes and slows the lab cell's contiguous decode path, and T13's scrambling-ID logic
misfires in both directions: it is inert where it is needed and harmful after an outage. Together with the
irreversible FDRA retirement, these would make the receiver silently worse on air. Fix C1 and I1-I3 before Task 17 or any
real-cell use.
