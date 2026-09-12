# UL Adaptive Discovery (hypothesis-sweep engine) — handover

**Status:** Implemented per `docs/superpowers/plans/2026-09-08-passive-rx-ul-adaptive-discovery-plan.md`
(spec: `docs/superpowers/specs/2026-09-08-passive-rx-ul-adaptive-discovery-design.md`). All 12 plan
tasks landed; this doc covers what closing out the plan's last three (turned out to be four) known
test failures actually found. **Offline/synthetic-test validated only — not yet run OTA.**

**Branch:** adaptive-rx-UL-DL · **Host:** sens6 · **Repo:** /home/sens/NICOLA/adaptive-rx-UL-DL
**Head at time of writing:** `8a3fa10eaf`

## What this adds

Makes the passive receiver's UL DCI 0_1 path self-configuring: discover DCI length, per-field bit
widths, and field-value interpretation (TDA table, DM-RS config, MCS table) all by search, with no
gNB log and no manual per-cell conf — mirroring what the DL path (Technique A/B/C/D, see the spec's
"Current DL state") already does.

- **`nr_hyp_sweep.{h,c}`** (new): a shared, hypothesis-type-agnostic 4-stage search engine reused by
  both new UL components, via a fixed-size opaque byte buffer + caller callbacks:
  - Stage a — admissible generation (spec-legal hypotheses only).
  - Stage b — equivalence collapsing (hypotheses the TB-CRC oracle can't actually distinguish merge
    into one class).
  - Stage c — reject-only plausibility gate (never scores or ranks, only refuses a spec-impossible
    hypothesis — a documented, permanent invariant).
  - Stage d — class-scored oracle: an anytime-Hoeffding-bound bandit (`nr_crc_evidence.h`) that
    allocates trials preferentially toward the current leader, prunes classes whose confidence
    interval has fallen below it, and declares a winner once one class's CRC pass rate clears both
    an absolute floor (`NR_HYP_SWEEP_MIN_RATE`) and a margin over the runner-up
    (`NR_HYP_SWEEP_WIN_RATIO`), or via an early "leader separated from every rival at a 60%
    lower-confidence-bound" shortcut once every class has enough trials.
- **`nr_pdcch_ul_field_sweep.{h,c}`** (Component 2): generates the admissible field-bit-width
  hypothesis space (harq_pid/dai/antenna_ports/srs_request/dmrs_seq_init bit counts, etc.) for a
  given DCI length and baseline geometry, on top of `nr_hyp_sweep`. Live-cell-derived numbers are
  pinned as regression tests (e.g. length 43 at this cell's real 6-entry TDA list: 199 admissible
  vectors, was 400 before a TS 38.212-derived tightening).
- **`nr_pdcch_ul_interp_sweep.{h,c}`** (Component 3, not in the original catalogue — added once the
  gap was noticed): the UL analogue of the DL side's Technique D — discovers TDA table contents
  (start/length/mapping/k2 per index), DM-RS config (type/additional-position/max-length), MCS
  table, and transform precoding. 960 combinations in the full catalogue.
- **`nr_pdcch_ul_discovery.c`**: the controller wiring both sweeps to live UL grants —
  per-identity contexts (keyed on RNTI + DCI length + baseline RRC geometry via `same_options()`),
  sample freezing once a search arms (real traffic is an endless supply of distinct payloads, so an
  unfrozen sample set never stabilizes a class definition), CRC-interval-based baseline validation
  (`nr_crc_interval`, a Clopper-Pearson-style anytime bound) in place of a raw pass-rate threshold,
  and late-arriving-evidence class splitting (`distinguishes()`) that refines a provisionally-merged
  class without discarding its already-scored representative's accumulated trials.

## Bugs found and fixed while closing out the plan's known test failures

The plan's own tracking (`tests/passive_rx/agnostic/IMPLEMENTATION_STATUS.md`) carried three
known-failing tests forward without root-causing them ("their contracts and implementations still
require investigation. Do not rewrite these tests merely to make the suite green."). Investigating
found one real algorithmic bug, and three tests whose contracts an already-in-tree, independently
well-reasoned redesign had superseded — not a single case of "just make it green."

### 1. `nr_hyp_sweep_next()` reshuffle-mid-pass bug (real bug)

The exploration-order reshuffle (`nr_crc_shuffle`) was gated on `cursor==0`, checked *inside* the
per-candidate loop rather than once at call entry. Whenever a call's starting cursor wasn't 0 (the
common case), the loop's own cursor wrap reshuffled the very `order[]` array it was mid-iteration
over — so some classes got revisited while others due that same pass were silently never reached.
Once the bandit pruning had eliminated most rival classes, this let `nr_hyp_sweep_next()` return -1
("every class unselectable") even though the true class was fully eligible and simply wasn't
visited that particular call.

Root-caused with an instrumented run of `UlFieldSweep.NeverInterpretableClassCannotBlockConvergence`:
failed at t=1072 with the true class (class 1) sitting at 120 trials / 90 passes and every rival
permanently pruned at 119 trials / 0 passes — a state where the algorithm should always find the
truth class on the next call, but that one call returned -1 instead.

**Fix:** hoist the reshuffle check out of the loop so it fires at most once, at entry. A call
always advances the cursor by exactly `n_classes` steps when it doesn't return early — one full,
stable lap over a *fixed* `order[]` — so every call now visits every class exactly once, regardless
of pruning state or where in the cycle it starts.

### 2 & 3. `NR_HYP_SWEEP_CLASS_OVERFLOW` made structurally unreachable (test-contract update, not a bug)

An already-in-tree change unified `NR_HYP_SWEEP_MAX_CLASSES` with `NR_HYP_SWEEP_MAX_RAW` (8192),
fixing a real, measured problem: the UL width search was permanently refusing on this cell's real
config once finite-sample class splitting pushed past the old, separate 512-class cap —
"autonomous UL sat at zero" per `nr_hyp_sweep.h`'s own comment. Direct, checked-not-assumed
consequence: a class always consumes ≥1 raw hypothesis, so `n_classes ≤ n`, and `n` is already
bounded by `NR_HYP_SWEEP_MAX_RAW` before `nr_hyp_sweep_init()` ever runs — so `n_classes ≤ n ≤
MAX_RAW == MAX_CLASSES` always, and `CLASS_OVERFLOW` can no longer fire from any call that already
passed the raw-cap check. Two tests still asserted the old, now-impossible overflow:
`nr_hyp_sweep_test.cc`'s `RefusalClearsOldWinnerAndChecksBounds` and
`nr_pdcch_ul_interp_sweep_test.cc`'s `FullCatalogueCannotSilentlyBypassClassCap` (renamed
`FullCatalogueFitsAndRawCapStillRefusesLoudly`). Updated both to assert the invariant instead
(`n_classes == n`, `n ≤ MAX_CLASSES`), keeping the raw-cap enforcement — the only limit that can
still trip — covered unchanged.

### 4. UL context pooling reverted to per-identity (test-contract update, not a bug)

`nr_pdcch_ul_discovery.c`'s context matching (already in the tree) now keys on RNTI again instead
of pooling CRC evidence across every UE sharing one DCI length. The pooled design (added 2026-09-09,
per the source's own superseded comment) needed a fragile "corroborated by every contributing
identity" veto to avoid confidently applying one busy UE's layout to a different UE that merely
shares a length but not a configuration; that veto and its bookkeeping
(`contrib_rnti`/`contrib_trials`/`contrib_passes`, `pooled_winner_corroborated_locked()`) are gone
from the source, not merely untested. `BlindPdcchTest.UlControllerPoolsEvidenceAcrossUesAtTheSameDciLength`
still asserted the pooled contract (three UEs sharing one `hyp_generation`); rewritten as
`UlControllerKeepsEachIdentityIndependentAtTheSameDciLength`, asserting three independent
generations instead. The two neighboring tests that check per-DCI-length independence and eviction
were unaffected by this and already passed before and after.

## Verification

- `nr-uesoftmodem` (the full executable) builds clean.
- All 9 relevant `ctest` targets green: `test_nr_hyp_sweep`, `test_nr_pdcch_dci_length_sweep`,
  `test_nr_pdcch_ul_field_sweep`, `test_nr_pdcch_ul_interp_sweep`, `test_nr_pdsch_config_sweep`,
  `test_nr_pdcch_blind_monitor`, `test_nr_pdcch_blind_rnti_bootstrap`, `test_nr_pdcch_coreset_map`,
  `test_nr_dl_adaptive`.
- `test_nr_pdcch_blind_monitor`: **123/123** (was 120/123).
- `test_nr_hyp_sweep`: 4/4 (was 3/4 once the `MAX_CLASSES`/`MAX_RAW` unification's consequence was
  also caught here, not only in the interp-sweep test).
- A pre-existing, unrelated build failure in `openair1/PHY/NR_UE_ISAC` (`isac_sync_test.cc`,
  `isac_sync_replay.cc` — a different subsystem, zero changes in this working tree) is untouched
  and out of scope; it blocks the blanket `make tests` target but not any target listed above.

Committed at `8a3fa10eaf`, scoped to exactly the 8 files this work touched (`nr_hyp_sweep.{h,c}`,
`nr_pdcch_ul_discovery.{h,c}`, the new `nr_crc_evidence.h`, and three test files). The much larger
uncommitted WIP already sitting in this tree (a separate raw-IQ full-passive-receiver /
"agnostic receiver" effort — DL-side `nr_pdsch_config_sweep.*`, `nr_initial_sync.c`,
`radio/COMMON/raw_iq*.c`, `executables/*`, etc., tracked in `tests/passive_rx/agnostic/`) is
unreviewed and out of this plan's scope, and was deliberately left untouched.

## Not yet done

- **No OTA/live-cell validation of any of this.** Everything above is offline unit-test coverage
  (synthetic hypothesis spaces, synthetic ground-truth payloads) — no run against a real gNB's UL
  traffic exists yet for Components 2/3 specifically, as distinct from the DL-side techniques this
  mirrors (which are separately live-validated — see `PASSIVE_UL_HANDOVER.md`).
- Convergence-time budget is unverified live: cost is `NR_HYP_SWEEP_MIN_TRIALS × n_classes`
  transport blocks per search, so a wide admissible set (e.g. DCI length 45's 1480 raw hypotheses
  before collapsing) could still take a long time to converge against real, sparser-than-synthetic
  traffic.
- `nr_pdcch_ul_discovery.c`'s per-identity keying means a C-RNTI reassignment on re-attach starts a
  fresh context from zero — the exact cost the (now-reverted) pooled design was trying to avoid.
  Not re-measured against this cell's actual RNTI churn rate; per `gnb-rnti-recheck-every-prompt`
  practice, the C-RNTI changes on every re-attach, so this is a real, not hypothetical, cost.

## Acquisition state tracker (added 2026-09-12, commit `0e8972973a`)

Gate 4/5 groundwork. `nr_passive_acq_state.{h,c}`: explicit states
`SEARCHING_PDCCH → PDCCH_LOCKED → CORESET_VERIFIED → CELL_CONFIGURED → {DL,UL}_CONVERGED → TRACKING`
plus `LOST`. Forward/lateral moves apply immediately; demotion to `LOST` needs 8 consecutive
regressed updates (hysteresis). Every transition logs at `LOG_A` with the full evidence vector, and
a `SENSING: ACQ state=...` heartbeat line (which also surfaces the UL-discovery census for the first
time) prints at the existing period-guarded RT summary cadence — one call site, reads of existing
counters only, no control-flow change. **No SSB/PBCH state exists**: the call site has no such
signal, so `SEARCHING_PDCCH` spans "no SSB" through "PDCCH not locked"; adding a PBCH hook is the
next step, not implied.

Offline: 6/6 gtests (`test_nr_passive_acq_state`); 10/10 relevant ctest targets green.

**Replayed against the saved 120 s raw capture** (`captures/raw_5min_last120.v51DBe/capture_120s`,
artifact `captures/acq_state_replay_20260912T100720Z`, `VALID_TRANSPORT_REPLAY`, EOF reached,
0 faults, 238 s wall):

| update | transition | evidence |
|---|---|---|
| 1 | `SEARCHING_PDCCH → CELL_CONFIGURED` | DCI length, CORESET extent and SIB1 UL BWP all known by the first summary |
| 131 | `CELL_CONFIGURED → DL_CONVERGED` | 1 keyed DL PDSCH-interpretation context settled (2 by end) |
| 182 | `DL_CONVERGED → TRACKING` | UL width search declared a winner: 396 classes, 5,737 trials |

Read the last row carefully: 5,737 trials over 396 classes is far below the
`MIN_TRIALS × n_classes ≈ 119k` full-oracle budget, so that winner came from the early
"leader lower-confidence-bound ≥ 0.60, separated from every rival" shortcut in
`nr_hyp_sweep_feed()`, not from exhausting the catalogue. An earlier 220 s run of the same capture
(VOID, timed out before EOF) had NOT yet converged UL — convergence landed in the last ~15 s. Whether
that shortcut's winner is the *correct* layout is not established by this replay (no UL TB decode
runs under `ISAC_SYNC_ONLY=1`); it establishes that the state machine observes and reports the
receiver's real progression on real IQ, which is what it is for.
