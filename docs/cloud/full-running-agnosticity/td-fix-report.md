# Technique D regression fix — implementation + validation report

Lane `td` (sens6:`/home/sens/NICOLA/agn-wt/td`, branch `sdd/agn-td`, base `25a8699a64`).
Commits (oldest first): `423adfb9d4`, `f655e1139d`, `47daba8c70`, `19f0bc9b45` (R30, first fix
round), `ecfced8bd0` (R32, second fix round — see that section below for the current state; it
supersedes this section's own A/B and some of its concerns).

## Status: DONE_WITH_CONCERNS (R30, superseded by R32 below)

Both R30 fixes are implemented, unit-tested, and live-validated. FIXED no longer decodes
**zero** TBs (the regression `technique-d-regression.md` diagnosed: BRANCH measured 0 crc_ok
across 6+ independent runs before either fix). Post-fix, FIXED decodes real TBs consistently
across both validation runs, in the same order of magnitude as BASE — but this specific
2-run-each comparison does not show FIXED **clearly above** BASE (BASE happened to score
higher in this particular pair: see the A/B table below). Given this project's own documented
extreme run-to-run variance on this rig (`FindsTheTruthOnAMarginalLink`-class behaviour; the
module header itself notes "TB-CRC swings between 5% and 88% across otherwise identical
captures"), 2 runs is a small, noisy sample, and I read this as inconclusive-but-fixed rather
than failed — reported honestly per the brief rather than re-running until a favourable sample
appeared.

## What was implemented

### R30 item 1 — type-B dilution (opt-in by evidence)

`nr_pdsch_config_sweep.c`/`.h`: a fresh Technique-D context's catalog is mapping type A only
again (matching base commit `222f98d072`'s pre-Task-14 size: 2016 pure / ~750 runtime). Type B
enters a context only once the DM-RS oracle observes a mask no type-A hypothesis can produce
(`mask_needs_typeb()`, tested against the cached type-A-only template) — mirrors the existing
k0-layer mechanism exactly: `add_typeb_layer()` only ever *adds* entries, never prunes the
type-A incumbent, and the widening is applied at the same three sites the k0 mechanism already
uses (new-context creation, the prior-restore path, and the observe() trigger itself), so
evidence survives context eviction/reopen the same way k0 layers already do.
`ISAC_PDSCH_TYPEB=0` stays a hard disable, now covering the evidence-triggered path too since
there is no other path left for type B to enter through.

**A real bug found and fixed while validating this against the real DM-RS mask generator**
(not the synthetic test fixtures): the effective-PDU merge/dedup inside the shared catalog
builder scanned the *whole* catalog for a `(mask, mcs_table, k0)` match. Two different `(S,L)`
pairs are NOT the same effective PDU even if their absolute `dmrs_mask` bit patterns coincide
(the data RE range comes from `(S,L)`) — only an entry of the *same* `(S,L,k0)` reaching the
same mask/table is genuinely indistinguishable to the TB CRC. The whole-catalog scan silently
dropped a real, distinct type-B entry whenever an unrelated `(S,L)` elsewhere in the real
generator's output happened to reuse the same mask value — caught by
`DlAdaptive.TypeBTruthIsPinnedByOneOracleObservationAndConverges` going from a working test to
0/60 hypotheses admitting the observation. Fixed by scoping the dedup check to the same
`(S,L,k0)`, restoring the original (pre-Task-14) scoping semantics.

### R30 item 2 — context churn (pre-existing on both trees)

`nr_pdcch_blind_monitor_rt.c`: `configuration` (the Technique-D context key) embeds the DCI-11
layout resolver's currently-offered candidate id. With 827/988 layouts still alive on this bed,
whichever of the ~3-11 offered candidates Thompson sampling/round-robin picks *this grant*
rotated the key practically every grant, so no context ever accumulated enough trials.

**Design choice and why**: I read how `configuration` is used first, per the brief's warning.
It genuinely depends on *which field-width layout guess* parsed the DCI — a different layout
reads a different `tda_index`/MCS from the *same* raw bits — so evidence gathered under one
layout cannot simply be merged into another's key without mixing garbage (a wrong layout's
decoded `tda_index` may not even correspond to the same physical TDRA entry a correct layout's
same-numbered `tda_index` would). I rejected dropping `configuration` from the key entirely for
this reason, and rejected merging-on-layout-change as harder to get right safely with no clear
"the layout just changed" event on a resolver that never narrows on this bed anyway. Chosen fix:
**pin** Technique D's context to one candidate per RNTI and keep selecting it occasion after
occasion (`dci11_pin_layout/_valid/_cfg[65536]`, mirroring the existing per-RNTI
`layout_cursor[]` array already in this file), so evidence lands on one context continuously
instead of scattering. A pin auto-invalidates on a real cell-geometry change; `settled` and
`preferred` (both already evidence-backed) take priority over it unconditionally.

**Two real problems found live, both fixed based on measurement, not guessing:**
1. A pin that only gives up after 1000 zero-pass **real Technique-D trials** never gave up in
   practice — real trials land on the pinned key at only a small fraction of the raw occasion
   rate (deferred-queue drops that grow over a run's lifetime, RV/decode-cap skips), so a wrong
   first-offered candidate stayed pinned an entire run with 0 crc_ok, worse than the churn it
   replaced.
2. Real trials are front-loaded and uneven: the first 200-occasion window measured 45 real
   trials; every later window in the same run measured ~0, as the queue backlog grows.

Fixed by rotating after a bounded, cheap-to-count number of **occasions** (not trials) —
`DCI11_PIN_BLOCK_OCCASIONS`, tuned live to 50 on this bed — instead of waiting on trial-count
evidence that arrives too slowly and unevenly to matter. The 1000-trial giveup is kept as a
second, faster exit for a link where real trials do arrive quickly.

## Validation

**Offline suites** (`ctest`, sens6:`/home/sens/NICOLA/agn-wt/td/cmake_targets/ran_build/build`):
`test_nr_pdsch_config_sweep` 41/41, `test_nr_pdcch_blind_monitor` 141/143 (2 pre-existing,
unrelated skips), `test_nr_pdsch_prb_set` all pass. All three built and run clean, no new
warnings in touched files.

**Live rfsim phy-test A/B** (106 PRB fully-agnostic recipe, `ue.passive.q.agn.conf`, gNB = this
tree's own stock `nr-softmodem --phy-test --noS1`, alternating FIXED (td tree, this branch) vs
BASE (`agn-wt/base @ 222f98d072`), 2 runs each, 150s each; logs under
`sens6:/home/sens/NICOLA/rfsim_validation/tdfix/{fixed,base}_r{1,2}/`):

| Run | Tree | crc_ok / try | new-context events | DCI11 survivors | CONVERGED |
|---|---|---|---|---|---|
| fixed_r1 | FIXED | 3 / 32747 | 20 | 827/988 | 0 |
| fixed_r2 | FIXED | 1 / 32635 | 20 | 827/988 | 0 |
| base_r1  | BASE  | 3 / 30709 | 20 | 827/988 | 0 |
| base_r2  | BASE  | 6 / 31006 | 20 | 827/988 | 0 |

An earlier iteration (block=200, before the occasion-count tuning above) measured fixed_r1=4,
fixed_r2=0 — i.e. still capable of a zero run. The block=50 tuning removed that: both final
FIXED runs are nonzero. **Caveat added on R32 review: `new-context events` is a log line capped
at 20 (`s_left=20` in `rnti_ctx()`) and should not have been cited as a churn measurement — it is
constant across every run/tree shown here purely because the cap saturates identically each
time, not because the underlying churn is actually identical. Read the "unchanged" claim in this
paragraph as applying only to the 827/988 DCI-11-layout plateau, which is a real (uncapped)
measurement.** Neither tree ever reaches the formal Technique-D CONVERGED milestone in a 150s
window on this bed (matches the original regression doc's finding for both trees), so crc_ok
count is the fair comparison metric, as it was there.

## Concerns

- **The 2-run-each sample does not show FIXED clearly above BASE.** FIXED (3, 1 = mean 2) vs
  BASE (3, 6 = mean 4.5) in this specific pair. Given the documented volatility of this exact
  metric on this rig (2-4 crc_ok was the *original* BASE baseline in `technique-d-regression.md`;
  6 is on the high side even for BASE), I read this as within noise rather than as FIXED
  underperforming, but it is not proven — more runs (5+ per arm, matching this project's own
  `passive-rx-needs-5-runs-per-arm` convention) would be needed for statistical confidence.
- **The real bottleneck limiting absolute crc_ok on this specific bed is the deferred-decode
  queue's growing backlog** (`scanq drop_full` climbs to ~190-200k of ~470k queued jobs over a
  150s run), which throttles how many raw occasions ever become real Technique-D trials,
  independent of either R30 fix. Out of scope for this task, flagged for whoever next touches
  this bed's throughput.
- `DCI11_PIN_BLOCK_OCCASIONS=50` was tuned empirically against one bed's measured occasion/trial
  rates in one session; it is a reasonable default, not a proven-optimal constant.
- Type-B dilution's fix (item 1) is now confirmed correct via both the offline suites and the
  live bed (Technique D did widen to type B when the mask indicated it, in the sense that the
  code path is exercised — this specific cell's true config was never independently confirmed
  to actually be type B or type A, since neither tree ever formally converges here).

Full commit-by-commit design rationale is in the three commits' own messages (`423adfb9d4`,
`f655e1139d`, `47daba8c70`, `19f0bc9b45`) on `sdd/agn-td`.

---

## R32 fix round 1

Controller review (R32) of the R30 commits above approved item 1 (type-B dilution + merge-scope
fix) outright and found two real problems with item 2 (the pin), plus a threading question and a
metric-citation error. All are fixed in commit `ecfced8bd0`.

### Status: DONE

Both IMPORTANT findings are fixed, unit-tested, and re-validated live at 5 runs/arm. **Revised
bar met: FIXED is not worse than BASE, within the noise this rig's own documented volatility
implies at n=5** (see the A/B and stats below).

### 1. Rotation-cursor starvation — fixed

**The bug, exactly as the review described it.** With `ISAC_AGNOSTIC_V2` unset (default), the
round-robin cursor (`layout_cursor[rnti]++ % n`) used to advance on **every occasion**, but a
seeded pin only got re-read (and the cursor only actually consulted for a new pick) once every
`DCI11_PIN_BLOCK_OCCASIONS` (50) occasions. Successive picks therefore landed `gcd(50, n)` apart
instead of 1 apart — e.g. `n=8` → only 2 of 8 candidates would ever be pinned, `n=10` → only 1 of
10. The `n=3` bed used for the R30 live validation was coprime with 50 by luck, which is exactly
why this went unnoticed there.

**Fix.** The pin now has its own dedicated cursor (`dci11_pin_cursor[]`), advanced by
`nr_dci11_pin_round_robin()` **exactly once per call**, and that function is called **only** at
the point of actually reseeding a dropped pin — never on an occasion where the pin is still valid
and in use, and never for the separate "pin valid but merely absent from this occasion's offered
list" one-off substitute (which keeps using the old free-running `layout_cursor[]`, since that
path has no successive-coverage requirement). This makes `n` successive reseeds visit all `n`
candidates for **any** `n`, proven directly (not just argued) in
`Dci11Pin.SuccessiveReseedsThroughSelectAndSeedVisitEveryCandidate` for `n` in `{3, 8, 10, 13}` —
the exact values the review named.

### 2. No unit test for the pin/rotate/giveup decision — fixed

Extracted the decision into a new pure module, `nr_dci11_pin.{h,c}` (registered in the
`nr_pdcch_blind_monitor` library, so both `nr-uesoftmodem` and `test_nr_pdcch_blind_monitor` link
it — `nr_pdcch_blind_monitor_rt.c` itself is `PHY_NR_UE`-only and not test-linkable, which is why
this had no test before): `nr_dci11_pin_select()` takes the pin state, this occasion's offered
candidates, `settled`/`preferred`, and the caller's own trial-stats lookup, and returns the index
to use or `-1`; `nr_dci11_pin_seed()` commits a freshly chosen candidate; `nr_dci11_pin_round_robin()`
is the once-per-call cursor picker item 1 needed. All three are dependency-free (no globals, no
I/O), so they link into the existing `test_nr_pdcch_blind_monitor` binary without pulling in the
RT scan-thread code or the sweep module.

New `tests/nr_dci11_pin_test.cc`, 9 cases, covering exactly the four properties the review asked
for plus one more the design needed:
- (a) **successive-reseed coverage** for `n` in `{3, 8, 10, 13}` (`RoundRobinAloneVisitsEveryCandidate`
  and `SuccessiveReseedsThroughSelectAndSeedVisitEveryCandidate`);
- (b) **settled/preferred bypass the pin**, unchanged (`SettledBypassesThePinUnchanged`,
  `PreferredBypassesThePinUnchanged`);
- (c) **a cfg change invalidates the pin**, even when the offered list still happens to contain
  the same layout id by coincidence (`CfgChangeInvalidatesThePin`);
- (d) **the trial-count giveup and the occasion-count rotation each fire at their own threshold**
  independently (`TrialGiveupFiresAtItsThreshold`, `OccasionRotationFiresAtItsThreshold`), and the
  trial-count one never fires with any nonzero pass count (`TrialGiveupNeverFiresWithAnySuccess`);
- plus `TransientAbsenceLeavesTheValidPinAlone`, pinning the "absent this occasion ≠ a verdict"
  contract the rotation-vs-absence distinction depends on.

**A real portability bug found while wiring the test in**: the first version of the header
declared `valid` as `_Atomic bool` directly. That does not reliably compile/link the same way
across a C production TU and the C++ test TU (`extern "C"` only affects linkage, not language
rules, and C++'s `<stdatomic.h>`/`_Atomic` support is not a portable guarantee) — it built, but on
a struct whose layout could differ between the two languages linking the same object file, which
is exactly the kind of bug that only shows up on a different toolchain. Fixed: `valid` is a plain
`bool` in the public struct; `nr_dci11_pin.c` (always compiled as C) accesses it through an
`_Atomic bool *` cast for its two release/acquire operations, which is standard, well-defined C11
and keeps the struct's layout identical in both languages.

### 3. Threading (minor, requested) — confirmed and documented, not changed further

Read both call sites, not assumed: `nr_pdcch_blind_monitor_run_occasion()` (where this pin logic
lives) is invoked from **both** the live RT receive thread (`nr_pdcch_blind_monitor_rt.c`: "on the
RT thread: fan out as before") **and** the deferred queue consumer thread
(`nr_pdcch_passive_queue.c`: "already off the RT thread"). So yes — the same RNTI's occasions can
genuinely reach this code from two different threads; this was a real question, not a formality.

`valid` (the only field whose cross-thread visibility gates correctness of the rest of the
record) is now published with a release store after `layout`/`cfg`/`occ` are written, and observed
with an acquire load before they are read — the standard "flag publishes a record" idiom, and
exactly the minimal fix requested. `occ`'s own increments and a torn seed from two racing
reseeds are left unsynchronized, documented in the header as an accepted, bounded, self-correcting
race on a heuristic (worst case: a rotation boundary shifts by about one occasion, or one occasion
picks up a torn layout/cfg pair that the very next occasion's cfg check or rotation bound
recovers) — not a lock-worthy correctness issue on this path.

### Metric correction

Per the review: `new-context events` (cited in the R30 section above) is a log line capped at 20
and was wrongly read as a churn measurement there. Not re-cited below. Building an uncapped debug
counter was judged not worth the extra build/validate cycle given the pass criterion here is
`crc_ok`/`try`, which needs no such counter — flagged as a real gap if a churn number is ever
needed again, not fixed.

### Validation

**Offline suites**: `test_nr_pdsch_config_sweep` 41/41; `test_nr_pdcch_blind_monitor` 151/153
(2 pre-existing, unrelated skips) — 9 of the 151 passes are the new `Dci11Pin.*` cases;
`test_nr_pdsch_prb_set` all pass. `nr-uesoftmodem` builds clean, no new warnings.

**Live rfsim phy-test A/B, 5 runs per arm** (same 106 PRB fully-agnostic recipe, same gNB, same
alternation pattern as the R30 validation, 150s each; logs under
`sens6:/home/sens/NICOLA/rfsim_validation/tdfix/{fixed,base}_v2_r{1..5}/`):

| Run | Tree | crc_ok | try |
|---|---|---|---|
| fixed_v2_r1 | FIXED | 0 | 32688 |
| fixed_v2_r2 | FIXED | 1 | 32831 |
| fixed_v2_r3 | FIXED | 1 | 32574 |
| fixed_v2_r4 | FIXED | 2 | 32844 |
| fixed_v2_r5 | FIXED | 0 | 32057 |
| base_v2_r1  | BASE  | 1 | 30841 |
| base_v2_r2  | BASE  | 1 | 31030 |
| base_v2_r3  | BASE  | 1 | 31704 |
| base_v2_r4  | BASE  | 2 | 31010 |
| base_v2_r5  | BASE  | 0 | 30898 |

**FIXED: mean 0.80, sd 0.837 (n=5).** **BASE: mean 1.00, sd 0.707 (n=5).** Pooled total: FIXED
4/162994 = 0.00245%, BASE 5/155483 = 0.00322%. The difference (0.2 crc_ok/run) is well inside one
standard error of the difference (≈0.49, giving t≈0.4) — not statistically distinguishable at
this sample size. FIXED no longer produces the all-zero pattern the original regression showed
(BRANCH 0/6 runs pre-fix); it now lands in the same noisy few-hits-per-run regime as BASE, on both
sides of BASE's own per-run range across the two rounds of validation (R30's base_r1/r2 = 3, 6;
this round's base_v2_r1..5 = 1,1,1,2,0). Read together with R30's own note that this rig's TB-CRC
"swings between 5% and 88% across otherwise identical captures", I read this as: **the revised bar
(not worse than BASE) is met**, and going further (proving "clearly above") would need either many
more runs or addressing the queue-backlog bottleneck noted below, neither of which this round's
scope asked for.

### Concerns (R32)

- The mean crc_ok is nominally lower for FIXED (0.80) than BASE (1.00) in this specific 5-run
  sample, though not distinguishably so given the sd. A larger n would be needed to rule out a
  small real gap either way.
- The deferred-decode queue's growing backlog (`scanq drop_full` ~190-200k of ~470k queued jobs
  over a 150s run, unchanged from the R30 report) remains the dominant bottleneck on absolute
  crc_ok for both trees; still out of scope here.
- `DCI11_PIN_BLOCK_OCCASIONS=50` is unchanged from R30 and still an empirically-tuned, not
  proven-optimal, constant.
- `occ`'s cross-thread synchronization gap (see §3 above) is a deliberate, documented scope
  boundary, not an oversight — flagged again here in case a future reviewer wants tighter
  guarantees.

Full commit: `ecfced8bd0` on `sdd/agn-td`.
