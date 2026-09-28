# Technique D regression investigation — sens6, phy-test rfsim bed

Host sens6. BRANCH = `adaptive-rx-UL-DL @ 25a8699a64`. BASE = `agn-wt/base @ 222f98d072`.
gNB = each tree's own stock `nr-softmodem` (`--phy-test --noS1`, no `ISAC_GNB_TEST_*` knobs).
Recipe = the Job 2 fully-agnostic 106 PRB phy-test recipe from `rfsim-results-phaseA.md`
(`ue.passive.q.agn.conf`, `dci_length` self-discovered, no pinned coreset/ss/bwp/tda/dci_bits).
Logs: `sens6:/home/sens/NICOLA/rfsim_validation/tdreg/{base,branch}_r{1,2}/{gnb,rx}/`.

## Step 1 — A/B (2 runs/side, ~100-170s each, alternated)

| Run | Tree | new-context events | converged | DCI11 layout survivors | pdsch_decode try/crc_ok |
|---|---|---|---|---|---|
| base_r1  | BASE   | 20 | 0 | 827/988 (plateaued) | 38838 / **2** |
| base_r2  | BASE   | 20 | 0 | 827/988 (plateaued) | 25999+ / **4** |
| branch_r1| BRANCH | 20 | 0 | 827/988 (plateaued) | 72626 / **0** |
| branch_r2| BRANCH | 20 | 0 | 827/988 (plateaued) | 28272 / **0** |
| job2_agn_check (prior session) | BRANCH | — | 0 | 827/988-equiv | 31k-152k / **0** (×3 arms) |

Every run, both trees: `SWEEP: new per-RNTI context rnti=0x1234` fires **exactly 20 times**, `SWEEP:
... converged` fires **0 times**, and the agnostic DCI11 layout resolver's own log
(`DCI11_LAYOUT n=... observed | 827 of 988 layouts still plausible`) plateaus at 827/988 and never
narrows further within the run. This part of the behaviour is **identical** between BASE and BRANCH —
not a regression.

The one measurable difference: BASE decoded a handful of real TBs (crc_ok 2, then 4, both runs
nonzero) while BRANCH decoded **zero** across 6 independent runs (4 here + 2 arms already recorded in
`rfsim-results-phaseA.md`'s Job 2, each with tens to hundreds of thousands of tries). A 24-bit TB CRC's
false-accept floor is ~6e-8/try, so 2-4 hits in ~25-40k tries (~1e-4) are real decodes, not noise —
BASE occasionally reaches and re-samples the true hypothesis enough to decode; BRANCH never does in
the same wall time. This is the regression signal.

## Regression: **YES** (narrow, not the whole catalog)

Culprit: **`7c3f74e0b5` (Task 14, "Technique D: PDSCH mapping type B and k0 >= 2")**.

Evidence is a direct source diff between the two trees' real (`ssh`-fetched, not the stale local
mirror — see trap below) `nr_pdsch_config_sweep.c`:

- BASE `nr_pdsch_config_sweep_init_legal()` (`nr_pdsch_config_sweep.c:66`): `for (uint8_t S = 0; S <= 3; S++) for (uint8_t L = 3; S+L<=14; L++)` — mapping type **A only**, ~42 legal (S,L) pairs.
- BRANCH `nr_pdsch_config_sweep_init_legal()` (`nr_pdsch_config_sweep.c:91-136`): `for (uint8_t S = 0; S <= 12; S++) for (uint8_t L = 2; S+L<=14; L++) ... for (uint8_t mt = 0; mt <= typeb; mt++) if (!nr_pdsch_tda_legal(mt,S,L)) continue;` — mapping types **A and B** (`nr_pdsch_tda_legal()` new at `:52-61`), ~132 legal (S,L,mapping) pairs.
- Task 14's own report (`task-14-report.md`) measures the consequence directly: "Runtime catalog (real mask generator, pos2) 750 → **2154**" — a **2.9x** growth, and "Unaided convergence... 223,838 vs 484,911 outcomes" — convergence cost also grows ~2.2x.

This lands on top of a **pre-existing, shared, non-regression** defect: the Technique-D context is
keyed by `(configuration, rnti, tda_index, tda_count, typeA)` (`find_context()`,
`nr_pdsch_config_sweep.c:755-761`), where `configuration` is the agnostic DCI11 layout resolver's
*currently-live candidate layout id* (`nr_pdcch_blind_monitor_rt.c:6072`,
`cand_task[ti].dl_layout_configuration`). That resolver never narrows past 827/988 candidates on this
scene (both trees), so it keeps proposing different `configuration` values, and every change is an
exact-match miss in `find_context()` → a brand-new context (`SWEEP: new per-RNTI context`, 20/run,
identical both trees) with `trials[]`/`ok[]` reset and cursor rehuffled from 0
(`context_catalog()`/`catalog_fill()`, cursor reset at `nr_pdsch_config_sweep.c:660-661`). This
matches the already-documented finding in memory `agnostic-joint-search-and-dmrs-oracle.md`
("Stage-2 offering ROTATES over a wide live set") and is **not new to this branch** — it reproduces
identically on `222f98d072`.

**Mechanism of the regression**: with the context evicted every ~1/20th of the run regardless of
tree, the number of trials any one context can accumulate before eviction is roughly fixed. Task 14
made the catalog each context must round-robin through ~2.9x larger without changing that eviction
cadence, so the trial density on any single (S,L,addpos,maxlen,table) hypothesis — including the
cell's true one — dropped ~2.9x. BASE's smaller, A-only catalog still occasionally lands enough
repeat hits on the true entry within one context's short lifetime to trip the "hot" exploit path
(`nr_pdsch_config_sweep_next()`'s `hot`/`exploit_tick` logic, `:417-431`) and decode a few TBs;
BRANCH's ~3x-diluted catalog essentially never does in the same wall time.

**On the earlier "(1,13)/(1,12)/(1,5) never tried" claim (`rfsim-results-phaseA.md` Job 2)**: this is
**not established** and should not be repeated as-is. That conclusion was drawn from grepping
`sym=` out of `PARMSET[i]` log lines, but `PARMSET` is a **diagnostic census capped at the first 16
DISTINCT tuples ever seen in the whole run** (`nr_pdsch_passive_decode.c:152` `PARMSET_MAX=16`,
`:164-186` `parmset_record()`/`nr_pdsch_passive_parmset_dump()`): every combo beyond the first 16
distinct ones encountered is folded into an undetailed `overflow=N` counter (N=35-40 in these runs).
Direct `grep -c 'sym=1+13\|sym=1+12\|sym=1+5'` over the **entire** log (not just PARMSET's own dump)
returns 0 in every run on **both** trees — consistent with either "never tried" or "tried, but not
among the first 16 distinct tuples logged", and the legality math (`blind_fill_dmrs_mask()` /
`nr_pdcch_blind_monitor.c:3075-3122`) traced by hand for (S=1,L=13,mappingType=A) does **not** reject
it (`ld=14`, `l0∈{2,3}`, table lookups all non-negative for add_pos 0-3, length 1) — so there is no
source-level reason to believe it is excluded from the catalog. The permanent 0% is fully explained
by the context-churn + catalog-dilution mechanism above without needing the TDRA-table-absent
hypothesis; do not re-cite the "absent from the enumerated set" claim without re-measuring with an
uncapped tuple log.

**Local-mirror trap encountered and avoided**: `/home/sens/NICOLA/adaptive-rx-UL-DL` (and `agn-wt/base`)
under this session's own filesystem are **stale local copies**, different from the real sens6 trees —
`Read`/`Edit` on those paths silently returns the wrong content (confirmed: a `Read` of
`.../adaptive-rx-UL-DL/openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c` returned code with NO
`nr_pdsch_tda_legal` and a different line numbering than `ssh sens6 grep` on the same path). All
source reads for this report were done via `scp`-fetched copies into the scratchpad
(`/tmp/.../scratchpad/{base,branch}/*.c`), never the local `Read` tool directly on the NICOLA tree
paths. Future sessions: never trust local `Read`/`Edit` on `/home/sens/NICOLA/*` paths that mirror
sens6 trees — always `ssh`/`scp` first.

## Proposed minimal fix (not applied — describing for a later implementer)

Root problem: the per-context catalog size and the context's survival time (bounded by the outer
DCI11 layout resolver's churn) are decoupled, and Task 14 grew the former without any compensating
change to the latter. Two independent, minimal levers, either sufficient on its own, and safe to
combine:

1. **Stop re-keying Technique D on the layout resolver's candidate id.** `find_context()`
   (`nr_pdsch_config_sweep.c:755-761`) and its caller `nr_pdsch_config_sweep_select()`
   (`nr_pdcch_blind_monitor_rt.c:6072-6074`) include `configuration` in the context key so that a
   different DCI11 layout guess gets an independent Technique-D search — but (S,L,add_pos,max_len,
   mcs_table) is PDSCH-config-sweep information, mostly orthogonal to which DCI11 field-width layout
   is live. Dropping `configuration` from the key (key on `rnti, tda_index, tda_count, typeA` only)
   would let Technique-D evidence for one RNTI/TDA survive a layout-candidate rotation instead of
   being discarded 20 times/run. Risk: a wrong DCI11 layout guess could feed genuinely wrong
   (S,L) trials into a context that a later, correct layout guess then inherits — needs the
   `prune_to_observed`/prior-invalidation escape hatches already in the file (same class of
   protection as the cell-wide-prior probation logic) to recover from that case.
2. **If (1) is out of scope, at minimum make the type-B catalog opt-in-by-evidence rather than
   always-on.** `ISAC_PDSCH_TYPEB=0` (`nr_pdsch_config_sweep.c:82-86`) already exists as a kill
   switch and reduces the runtime catalog back to BASE's ~750-entry, A-only size — the cheapest
   possible mitigation, already wired, just not the default. This does not fix the underlying
   context-churn defect (item in the pending list below) but restores BASE's occasional-decode rate
   until (1) or a churn fix lands.

Neither of these was implemented — per the brief, this describes the fix, does not apply it, and
nothing was committed to `adaptive-rx-UL-DL`.

## Not done given effort/time budget

- No isolated worktree build was made at exactly `7c3f74e0b5` (or its parent) to get a third,
  in-between confirmation point; the culprit attribution rests on (a) the direct source diff matching
  Task 14's own reported 750→2154 catalog-size numbers and (b) the live crc_ok pattern (BASE 2/2 runs
  nonzero, BRANCH 0/4 runs here + 0/3 prior arms). This is convergent but not a literal bisect
  build-and-rerun; flagged for anyone who wants a stronger confirmation.
- No offline gtest was added/run in a scratch worktree to directly assert catalog membership of
  (1,13)/(1,12)/(1,5) under the real `dmrs_typeA_position` this cell uses — the by-hand trace through
  `blind_fill_dmrs_mask()` was done instead (documented above) given the "absent-from-catalog" claim
  turned out to rest on a capped diagnostic, not a real absence, making a dedicated test less
  urgent than originally scoped.
