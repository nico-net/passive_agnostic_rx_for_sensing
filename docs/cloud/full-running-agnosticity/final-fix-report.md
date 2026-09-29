# Final-review fix report (R27)

Lane `final`, worktree `sens6:/home/sens/NICOLA/agn-wt/final`, branch `sdd/agn-final`, base `3eb1b070c3`.
Paths are relative to `openair1/PHY/NR_UE_TRANSPORT/`, and line numbers are at the final HEAD `25a8699a64`.
I did not use the X410 and did not run nr-uesoftmodem.

## Commits (oldest first)

| Commit | Findings |
|---|---|
| `24877e0d06` CMake: nr_scrambling_id_sweep.c lives in the nr_pdcch_blind_monitor library only | I7 |
| `652ddd3599` nr_arm_sweep: evidence-triggered (incumbent) variant for the PRG sweep | C1 (pure part) |
| `d907ee08e0` nr_dmrs_id_estimate: two-window driver, stage 1 always on, stage 2 throttled and capped | I5 (pure part), M1 |
| `e0831aa013` nr_scrambling_id_sweep: dedicated-identity rule, link-health tracker, walk gate | I1/I2 (pure part) |
| `b9ab1d132c` FDRA staging: reversible DL retirement; UL 0_1 booking refusal off by default | I3 |
| `b06bf9f633` Passive DL/UL: gate PRG on evidence, protect per-RNTI state, apply scrambling IDs per spec | wiring for C1, I1, I2, I4, I5, I6 |
| `25a8699a64` Technique D: build contexts outside g_lock from a prebuilt catalog template | I8 |

The wiring for C1, I1, I2, I4, I5 and I6 is one commit on purpose. These findings share:
- the per-RNTI `rnti_dec_t` state,
- the new grant fields `rnti_class`, `dci11` and `scr_dedicated`,
- the DL link-health tracker.

Their logic is split out as pure, separately tested commits.

I9 is an edit to the local plan file and has no commit.

## Verification

- **Build.** `lane-make.sh final -k nr-uesoftmodem tests` builds nr-uesoftmodem and every test.
  - The only error is the known `test_nr_ue_ra_procedures` link failure.
  - None of the changed files adds a new warning. The four warnings present (`n_verified`, `dmrs_first`,
    `passive_ul_unav_res`, zero-length format) also appear in the baseline build logs.
- **ctest.** 110/112 pass, the same as the baseline run on the untouched HEAD. The two failures are the
  expected ones: `test_nr_ue_ra_procedures` (Not Run) and `test_vrtsim_cirdb`. Nothing else fails.
  - An intermediate run caught a real bug: a zero-initialised two-window state read as "decided id 0",
    which failed `test_nr_pdcch_blind_monitor`. It was fixed before commit (see I5) and is pinned by a test.
- **New and extended unit tests, all passing:**
  - `test_nr_arm_sweep`: 16 tests (9 new).
  - `test_nr_scrambling_id_sweep`: 11 tests (6 new).
  - `test_nr_dmrs_id_estimate`: 12 tests (4 new).
  - `test_nr_pdcch_dci11_layout_sweep`: 2 new FDRA tests.
  - `test_nr_pdcch_dci01_layout_sweep`: enforce-off assertion added.
- **nm.** I compared the undefined symbols of `libnr_pdcch_blind_monitor.a` (70 external) with the symbols
  `libPHY_NR_UE.a` defines.
  - Only `nr_pdcch_dmrs_ref` is left. It predates this branch, as the review notes.
  - `nr_scrambling_id_sweep_*` is now defined in the library, and `libPHY_NR_UE.a` defines none of it.
- **Other link units.** I built every other executable that links PHY_NR_UE: polartest, nr_dlschsim,
  nr_pbchsim, nr_pucchsim, nr_dlsim, nr_prachsim, nr_ulschsim, nr_psbchsim, nr_ulsim, nr_srssim and
  nr_ulsim_mu_mimo.
  - These built: polartest, nr_dlschsim, nr_pbchsim, nr_pucchsim, nr_prachsim, nr_ulschsim, and nr_dlsim,
    which links both libraries.
  - psbchsim, ulsim, srssim and ulsim_mu_mimo fail to link. Every missing symbol is an existing
    `nr_isac_*`, `nr_ue_diag_*`, `nr_ue_pending_rebase_*`, `nr_pdcch_blind_dci10_size` or
    `nr_pdcch_sib1_prior_set` reference from `phy_procedures_nr_ue.c` / `config_ue.c`.
  - None of the missing symbols is one this work defines or moved; I grepped for 0 matches of
    `nr_scrambling_id`, `nr_scr_link` and `2stage`.
  - I did not build these four at the base commit, so I cannot confirm they failed before this work.
    The missing symbols are ones I never touched, so I read it as a separate, earlier problem.

## Per finding

### C1: PRG sweep is evidence-triggered — FIXED
- **What changed.**
  - `nr_arm_sweep.h:168` onward adds `nr_arm_sweep_gated_t`, with `_init`, `_pick` and `_feed` (`:202`).
  - `nr_pdsch_passive_decode.c` wiring:
    - `prg_sweep_enabled()` at `:339`, the `ISAC_PRG_SWEEP=0` kill switch.
    - The pick is gated at `:1493`: DCI 1_1 only (`grant->dci11`), sweepable RNTIs only, and the knob.
      DCI 1_0 is excluded because TS 38.214 5.1.2.3 fixes its PRG.
    - The feed passes link health, and the first exploration is logged
      (`PRG rnti=… wideband CRC <= 25% … exploring prg=2/4`).
- **Trigger: N = 32, threshold 25 %.**
  - Only arm-0 trials fed while the link is healthy count, and a window that is not poor is discarded.
  - Why N = 32 and 25 %: a healthy wideband decode (true rate ≥ 0.5) reads ≤ 8/32 with probability
    P(Bin(32,0.5) ≤ 8) = 3.5e-3 per window. A PRG mismatch collapses wide grants to about 0 %
    (OTA 2026-09-12: 0/10000 full-band), so it trips in the first window.
  - "Link healthy" is `nr_scr_link_healthy()`: an N_ID^cell grant (SIB1/RAR/paging/CSS) or another RNTI's
    dedicated grant passed within the last 256 outcomes.
- **Ties.** A Wilson upper-bound tie goes to arm 0. After n_arms × 128 exploration trials (384 for PRG)
  with no arm separated, the sweep latches arm 0. This bounds the total cost and stops a SISO or
  wideband-precoded cell from paying forever.
- **Addition beyond the ruling.** A segmented arm that returns UNSUPPORTED is now fed as a failed trial
  (`prg_arm_unsupported`, `:549`). The cases are PT-RS, CSI-RS parity and a bad list. Unfed, its untried
  bound of 1.0 would have been re-picked on every grant; the review names this defect.
- **Tests** (`tests/nr_arm_sweep_test.cc`):
  - HealthyIncumbentNeverExplores, DeadLinkNeverTriggers, TriggersAfterExactlyOnePoorHealthyWindow,
    NonPoorWindowIsDiscarded, FindsTheBetterArmOnceTriggered, TieLatchesTheIncumbent (with a bound on
    exploration cost), TieBreakPrefersArmZero.
  - The dual bandit the review asked for, VRB-L and PRG on one CRC stream, in both directions:
    VRB-L wrong does not trigger PRG, and both converge when PRG is wrong.
- **Not done.** The T9 DMRSFO/CSI-RS-parity item stays deferred per the ruling, because PRG is no longer
  default-on.

### I1: data-scrambling-ID walk gate — FIXED
- **New gate:** `nr_scrambling_walk_eligible()` (`nr_scrambling_id_sweep.h:101`). All three must hold:
  - the DM-RS ID of the grant's nSCID is decided (by the margin gate);
  - `nr_scr_link_healthy()` is true;
  - ≥ 20 consecutive dedicated-class CRC fails, in a window that any dedicated pass resets.

  Technique D convergence is no longer required. The Technique D prior lookup is gone from
  `nr_pdcch_blind_monitor_rt.c:6048`.
- **Counting.** `nr_pdsch_passive_crc_note()` (`nr_pdsch_passive_decode.c:566`) keeps the counts.
  - It is called on the queue path (`nr_pdsch_passive_queue.c:971`, layout probes excluded) and on the
    in-line path (`nr_pdcch_blind_monitor_rt.c:6456`). This folds in the T13 deferred item.
  - N_ID^cell grants (SIB1, 1_0 in the CSS) neither count toward the window nor reset it.
- **Logging.** The DL walk logs `DATA_ID_WALK START/STEP/LATCHED`, with WRAPPED noted on the step line,
  and each line carries the RNTI, the candidate and the reason. See `data_id_current`/`_feed` at
  `nr_pdsch_passive_decode.c:449`.
- **UL.** `blind_ul_apply_scrambling_ids()` (`nr_pdcch_blind_monitor.c:4412`) applies the same gate, via
  `nr_pusch_passive_ul_walk_eligible()` (`nr_pusch_passive_ul_ids.c:66`).
  - It never walks for DCI 0_0 and never applies the result to it.
  - The UL window counts only 0_1 TBs, and 0_0 passes feed link health (`ul_crc_note`, `:72`).
  - The UL walk logs `UL DATA_ID_WALK …` lines.
- **Tests:** ScramblingLink.* and ScramblingWalk.*, covering: outage is not healthy, a common pass makes
  it healthy, an RNTI's own pass does not count, old passes expire, and all three conjuncts are required.
- **Concern (the ruling's gate, not changed).** A dedicated-class failure streak with another cause can
  still start the walk while the link is healthy. Examples are Technique D still exploring, or an MCS or
  rank limit. The walk tries each candidate once and wraps back to the PCI, so the worst case is 1024 TBs
  per RNTI, and every step is logged.

### I2: IDs applied only where the spec allows — FIXED
- **The rule** is `nr_scrambling_dedicated()` (`nr_scrambling_id_sweep.h:71`): C-RNTI class, and not a
  fallback DCI in a common search space.
  - This is TS 38.211 7.3.1.1 / 6.3.1.1.
  - It is also OAI's own UE condition: `nr_ue_procedures.c` checks `TYPE_C_RNTI_ && ss_type != common`,
    and `nr_ue_scheduler.c` checks `TYPE_C_RNTI_ && !(0_0 && common)`.
- **DL.** The scan thread computes `dl_dedicated` (`rt.c:6032`) from `out.rnti_class`, `is_dci10` and
  `css0_occasion`.
  - The decided DM-RS ID and the data walk apply only when `dl_dedicated` holds. SI, RA, TC, P and CSS 1_0
    grants get the PCI for both.
  - The flag travels in `grant.scr_dedicated`. The DL DM-RS accumulator learns only from those grants
    (`nr_pdsch_passive_queue.c:1073`).
- **UL.** Only DCI 0_1 is dedicated, for both application and accumulation (`nr_pusch_passive_decode.c:1003`).
- **Deviation from the spec (not from the ruling).** For DM-RS, TS 38.211 7.4.1.1.1 / 6.4.1.1.1.1 (and OAI's
  UE) would also allow scramblingID0 on a C-RNTI 1_0 in the CSS and on a C-RNTI 0_0. I applied the
  ruling's stricter rule to DM-RS as well, for two reasons:
  - the monitor cannot reliably tell a TC-RNTI from a C-RNTI in a CSS;
  - the 0_0 extractor does not know its search space.

  A wrong ID on a Msg4/TC grant is fatal, while the PCI on a rare CSS C-RNTI grant costs only that grant.
  The reasoning is documented in the header.
- **Tests:** ScramblingDedicated.OnlyCRntiOutsideCssFallback.

### I3: FDRA staging — FIXED
- **Reversible retirement.** `nr_dci11_resolver_arm_next_mode()` (`nr_pdcch_dci11_layout_sweep.c:443`):
  - It now appends the next stage first.
  - With nothing retired yet, it retires the refuted live set as before, but only if the new stage added
    layouts. The old code could empty the set, and an empty set is never refuted again.
  - With layouts already retired (a later stage refuted too), it revives them with fresh counters instead.
  - A new `retired[]` flag separates revivable layouts from layouts pruned by stage 1, and `last_revived`
    feeds the rt.c log.
  - `disarm()` clears the revivable set (`:399`).
- **Header** (`nr_pdcch_dci11_layout_sweep.h:215`): the "not destructive" claim is corrected to describe
  retirement and why reversibility bounds it.
- **Log.** The rt.c staging log (around `:1022`) reports REVIVED, and states that `dl_link_ok` counts
  1_0/SIB1 passes. This also covers M7.
- **UL refusal off by default.** `nr_dci01_fdra_book(…, enforce)` and `nr_dci01_fdra_refuse_enforced()`
  (`nr_pdcch_dci01_layout_sweep.c:216/221`) implement `ISAC_UL_FDRA_REFUSE=1`.
  - Without the knob, `rt.c:5376` counts and logs `UL_FDRA_WOULD_REFUSE` and books the grant.
  - The periodic log says "would-refuse (not enforced)".
- **Tests:** Dci11Fdra.ALaterRefutationRevivesTheRetiredStage, Dci11Fdra.EveryStageArmedStillRevives, and
  an enforce-off assertion in the Dci01 refusal test. The existing staging tests pass unchanged.
- **Not done.** The review's other suggestion, requiring link health from the same RNTI class instead of
  SIB1, was not implemented. The ruling asked for reversibility, and reversibility makes a SIB1-driven
  false refutation cost time only.

### I4: `rnti_dec_t` — FIXED
- **What changed** (`nr_pdsch_passive_decode.c:355` onward):
  - 64 slots.
  - Evidence-protected eviction, the same victim order as T1's `rnti_ctx()`: a slot with evidence is
    evicted only when every slot has evidence (logged, rate-limited).
  - `rnti_sweepable()` (`:360`) means no slot is ever created for 0xFFFF, 0xFFFE, or the SI/RA/P RNTI
    classes. For those grants, PT-RS, n_L and PRG fall back to the cell seed or arm 0.
- **Evidence** is any own latch or walk, or ≥ 8 trials in total (`:367`). A bare "any trial" test would let
  a noise RNTI protect itself after its first decode.
- **Deviation.** The RA RNTI range is excluded by grant class (`grant.rnti_class`), not by numeric range.
  RA-RNTI values (1..17920) overlap about 27 % of the C-RNTI space, so a numeric rule would drop real UEs.
- **Test.** None offline: the state is static in the PHY-coupled decode file. Verified by build and by
  reading the code.

### I5: DM-RS ID escalation — FIXED (DL and UL)
- **What changed.** `nr_dmrs_id_2stage_t` (`nr_dmrs_id_estimate.c:245/257`):
  - Stage 1 (0..1023) accumulates on every call until anything decides.
  - Stage 2 (1024..65535) is armed after 64 undecided stage-1 grants.
  - Stage 2 is evaluated on 1 call in 2048, at most 64 times in total. That is about 11 s of one consumer's
    CPU for its whole lifetime. After an undecided budget its arrays are freed.
  - All work stops once either stage decides.
- **Publication (fixes M1).** The decision is `decided_p1` (identity + 1, so 0 means undecided), with a
  release store and an acquire inline read, `nr_dmrs_id_2stage_decided()`.
- **Wiring.**
  - DL: `nr_pdsch_passive_queue.c` state and accumulate.
  - UL: `nr_pusch_passive_ul_ids.c:33` state, `nr_pusch_passive_decode.c:1003` accumulate.
  - `nr_pdsch_passive_dl_dmrs_id()` and `nr_pusch_passive_ul_dmrs_id()` now return the two-window type.
- **Tests:** DmrsId2Stage.PrematureEscalationDoesNotLoseAStage1Id, FindsAStage2IdThrottledAndCapped,
  Stage2BudgetIsAHardCap, ZeroInitialisedStateIsUndecided.
- **Cost note.** Stage 1 still costs about 2 ms per dedicated job until an identity is decided. That is the
  same as before escalation, and it is what "stage 1 always on" means.

### I6: DM-RS oracle and k0 probe — FIXED
- `dmrs_oracle_measure()` takes `nid` (`nr_pdsch_passive_queue.c:463`). Both call sites pass
  `job.dlsch_pdu.dlDmrsScramblingId`.
- That value, as set by the scan thread, is the decided ID for the job's nSCID on a dedicated grant and the
  PCI otherwise, which is the I2 rule.
- `nr_dmrs_prb_coherence()` has no nid ≥ 1024 limit, so stage-2 IDs work.
- The BWP-discovery probe still uses the PCI; it was out of scope.
- **Test.** None: this is PHY-coupled wiring, verified by build and reading.

### I7: layering — FIXED
- **What changed.** `CMakeLists.txt`: the source moved into the `nr_pdcch_blind_monitor` sources. It is
  removed from `PHY_NR_UE_SRC` and from `test_nr_pdcch_blind_monitor`, which links the library.
- `test_nr_scrambling_id_sweep` keeps its direct copy because it does not link the library, so every link
  unit has exactly one definition.
- **Evidence:** see the nm and "Other link units" results under Verification above.

### I8: locks and context creation — FIXED
- **Technique D contexts** (`nr_pdsch_config_sweep.c`):
  - `catalog_template()` (`:592`) builds each catalog, keyed by (typeA, legality), once under its own
    `g_tmpl_lock`. The lock order is g_lock → g_tmpl_lock and is never reversed.
  - A new context copies the template. Its buffer is either the one-slot recycled `g_spare_state` or a
    `malloc` done outside g_lock; it is filled and then installed under g_lock (`find_context`, `:755`).
  - A race loser or an evicted state goes back to the spare slot (`:837`) or is freed after unlock.
  - `context_catalog()` (reopen and probation rebuild) also copies the template instead of enumerating.
- **Lock acquisitions per DL grant on the scan thread, after the fix:**

  | Grant | Locks taken |
  |---|---|
  | 1_0 (any class) | none of the three the review named. `rnti_prior_get` (g_lock) is removed from the gate; `data_id_current` is not called for non-dedicated grants. |
  | C-RNTI 1_0 in a USS | none, until a walk has started somewhere: `data_id_current` has a lock-free fast path (atomic walk counter). |
  | 1_1, sweep on, context exists | g_lock once (`select`). |
  | 1_1, new context | g_lock twice, with the allocation and template copy between the two. |
  | 1_1, interleaved VRB bit set | also g_ptrs_lock once (`nr_pdsch_vrbl_pick`). |
  | 1_1, walk active or latched | also g_ptrs_lock once (`data_id_current`). |

  The link-health and fail-window reads are plain atomics.
- **Not done.** No lock-free per-RNTI prior cache was added, because the gate no longer reads the prior.

### I9: plan coverage matrix — FIXED
Edited `/home/sens/NICOLA/docs/superpowers/plans/2026-09-25-full-running-agnosticity.md`, which is local and
has no commit. The matrix now says:
- **PUSCH RA type 0 / dynamicSwitch: PARTIAL.** Detected and refused; decode is not implemented; refusal is
  opt-in.
- **PUSCH TDRA type B: PARTIAL.** Only (0,4) and (2,12); full search is follow-up work (UL DM-RS mask
  oracle).
- **PDSCH data scrambling ID: PARTIAL, gate fixed by I1.** What now works is stated, and the limits are
  listed.
- **PUSCH scrambling IDs:** DCI 0_1 only.
- **DM-RS ID0/ID1:** two-window, dedicated-class only.
- **PRG:** evidence-triggered.

## Deferred items
The triage lists two must-fix items. Both are fixed:
- the T10 false-refutation item, in I3;
- the T13 in-line eligibility counter, in I1.

T9 stays deferred because PRG is no longer default-on. Every other ledger item stays "can wait", as the
triage says.

## Concerns
1. None of this ran on air. The I1 gate, the C1 trigger and the I3 revival are verified by unit tests and
   the build only.
2. I1: a dedicated-class failure streak with a non-scrambling cause can still start the walk while the link
   is healthy, as described under I1. It is bounded and logged.
3. I2: on DM-RS the receiver is stricter than the spec for C-RNTI fallback DCIs in the CSS and for 0_0, as
   explained under I2. On a scramblingID0 ≠ PCI cell those grants fail.
4. I did not build psbchsim, ulsim, srssim and ulsim_mu_mimo at the base, so I cannot show their link
   failures predate this work. The missing symbols are unrelated to it.
