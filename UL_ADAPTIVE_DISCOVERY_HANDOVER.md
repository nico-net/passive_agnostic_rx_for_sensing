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

### Gate 5 (reacquisition): tested, and the first test FAILED

The tracker above was committed as "gate 4/5 groundwork" before the gate was run. Running it
showed that claim was wrong, which is the whole reason this section exists.

**Test mechanism** (offline replay only; no live-radio equivalent): `raw_iq_source` gained a
one-shot sample skip (`ISAC_RAW_IQ_GAP_AT_S` / `ISAC_RAW_IQ_GAP_S`, 0 = disabled = byte-identical
behaviour). Skipping samples makes the returned timestamp jump, so the receiver's OWN continuity
test (`nr_rx_continuity_check` → `RXDISCONT`) sees precisely what a real stream loss looks like —
the stimulus is not simulated at the detector, only at the source. `run_raw_replay.py` gained
`--gap-at-s` / `--gap-s`, and now scores the gate itself into `result.json`
(`acceptance_gate_5`): gap injected AND discontinuity detected AND tracker declared `LOST` AND
tracker recovered.

**First run (before the fix): FAIL.** 3 s gap at 60 s → `RAW_IQ_GAP injected skipped=368640000`,
`RXDISCONT ... -> INVALIDATING SYNC`, receiver reacquired — and the tracker **stayed in
`TRACKING` throughout, logging nothing**. Root cause: every input it reads is LATCHED discovery
state (`g_length_found`, CORESET extent, `bwp_size`, sweep winners); none of it clears on a stream
loss, so the event was structurally invisible. A state machine that can only move forward is not a
reacquisition test.

**Fix:** `nr_passive_acq_note_sync_loss()`, called once from `nr-ue.c` at the existing RXDISCONT
invalidation point. Drops straight to `LOST` with **no** hysteresis — unlike the evidence path, a
discontinuity is not noisy evidence that might recover, it is proof the frame-to-sample mapping is
gone (the same reasoning `nr-ue.c`'s own comment gives for not waiting on PBCH failures).

**Second run: PASS**, harness-scored (`captures/gate5_20260912T103129Z`,
`VALID_FAULT_INJECTION`, EOF, 0 other faults):

```
verdict: PASS   gap_injected ✓  discontinuity_detected ✓  acq_declared_lost ✓  acq_recovered ✓
SEARCHING_PDCCH → CELL_CONFIGURED → UL_CONVERGED → LOST → UL_CONVERGED → TRACKING
```

**Control** (`captures/gate5_control_20260912T103548Z`, same capture, no gap):
`VALID_TRANSPORT_REPLAY`, verdict `NOT_APPLICABLE`, **zero** `LOST` transitions. That control is
what makes the PASS mean anything — without an injected gap the tracker never declares `LOST`, so
the verdict is not trivially true.

**What this gate does NOT establish:** that the receiver's *decoding* recovers correctly, only that
loss is detected, reported, and followed by re-progression through the discovery states. These
replays run `ISAC_SYNC_ONLY=1`, so no TB decode is exercised. Gate 5's remaining half — a
configuration CHANGE (not just a gap) mid-stream — is still untested.

## Full offline gate campaign (2026-09-12) — every saved capture, harness-scored

Runner: `tests/passive_rx/raw_baseline/run_gate_campaign.sh` (strictly sequential, one receiver
at a time, refuses to start while a build or another `nr-uesoftmodem` runs — CPU contention
produces false RXDISCONT on paced replays). One JSON line per stage in `summary.jsonl`; nothing
below was judged by eye. Artifacts: `captures/campaign_20260912T{115154,121401,121933,122412}Z`.

| gate | stage | result |
|---|---|---|
| 1 manual equivalence | saved-IQ decoder oracle (`/tmp/agnostic-dci10-validation/replay-input.bin`) | **45/45 DL bit-identical, 0 failed, 15 raw UL** — exactly PROGRESS.md's documented figure for this input |
| 4 raw acquisition | `raw_batch` window_1, window_2, `raw_fullband_4s` (4 s each) | `PASS_RAW_BROADCAST_ACQUISITION`; window_2 **5/5** on rerun after one 1-in-6 miss (marginal length, not a regression — 2 PBCH + SIB1 needed, 4 s is barely enough) |
| 4 raw acquisition | `raw_long_5min/capture_2s` | FAIL, correctly: tracker shows `PBCH_LOCKED` and nothing after — **minimum acquirable capture is between 2 s and 4 s on this cell** |
| 5 reacquisition | 3 s gap @ 60 s (×3 incl. regressions), 3 s @ 5 s, **10 s** @ 60 s, **50 ms** @ 60 s | **all PASS**, every run ends in `TRACKING`. Note: even a 50 ms gap costs a full reacquisition — that is the receiver's existing RXDISCONT policy (any timestamp jump invalidates sync), not the tracker's |
| 5 control | no gap | `NOT_APPLICABLE`, zero `LOST` transitions |
| 7 regression | full `ctest` (91 tests, first time the whole suite BUILT on this branch) | **90/91**; the one failure is upstream `test_vrtsim_cirdb`, deterministic 3/3 in isolation (server never creates its shm segment at parametrization /1, LOG uninitialised so its error is swallowed), `radio/vrtsim` untouched by this branch |

Gate 7 needed three stale tests fixed first (25914f658c — `make tests` did not build, so every
earlier "N/N" was a hand-picked subset): `isac_sync_test.cc` + `isac_sync_replay.cc` (a
`row_illum` argument added in a67f55faec without updating the callers; 9/9 pass once they compile,
so the break hid no behavioural regression), `sparse_doppler_test.cc` (missing `<algorithm>`),
`test_nr_ue_ra_procedures.cpp` (stopped linking after CSS0 autoconf; link-only stubs, every call
site verified gated off).

### Two defects the campaign found in the tracker itself (fixed, 5c8d11caf9)

1. **Blind on short captures.** On every 4 s capture the tracker logged nothing — measured: the
   blind PDCCH monitor (its only poll site) ran **zero** occasions in 4 s while MIB and SIB1 both
   decoded. Fixed with event edges at the real sites (`note_pbch_locked` in `nr-ue.c` after the MIB
   is applied, `note_sib1` in `config_ue.c` at the passive SIB1 publish) and two new early states.
   Now: `PBCH_LOCKED → SIB1_DECODED` on every 4 s run before any occasion; a 2 s run stops at
   `PBCH_LOCKED`, which is the diagnosis. The event trace also exposed that the receiver re-syncs on
   the second SSB (two `pbch_locked` events per short run) — previously invisible.
2. **HEAD ≠ tested tree.** `41bf02c6d8` had `note_sync_loss()` after an unrelated UL-probe block, not
   at the RXDISCONT site: a zero-context `git apply --cached` landed a hunk at the right HEAD line
   but the wrong place, because ~100 lines of foreign uncommitted WIP sit in the same function. All
   gate-5 numbers were measured on the working-tree binary (correct placement) so they stand; HEAD
   did not implement them. Fixed by constructing the staged file from HEAD + exact edits and
   syntax-checking the staged content with the executable's own flags.

### Still not done
- Gate 5's other half: a configuration **change** mid-stream (BWP/CORESET change), not just a gap.
- Decode recovery after a gap: all replays run `ISAC_SYNC_ONLY=1`, so TB decode is not exercised.
- Gate 1 general 1_1 equivalence, gate 2 multiple layouts, gate 3/6 beyond tested scope: unchanged.
- `test_vrtsim_cirdb` (upstream) and the 2 s-vs-4 s acquisition floor are recorded, not fixed.

### Corrections (2026-09-12, later the same day)
- **Retracted:** "these replays run `ISAC_SYNC_ONLY=1`, so TB decode is not exercised" (stated
  twice above). `ISAC_SYNC_ONLY` is not read by any C source — it is a dead env var in the harness —
  and the replays decode: gate-5 regression run, UL `pusch_passive try=14359 crc_ok=10562 (73.6%)`,
  DL `PDSCHQ decoded=150452 crc_ok=47710 (31.7%)`.
- **Decode recovery after a gap, measured** (same run, cumulative-count deltas either side of the
  3 s gap at 60 s): UL **85.7 %** CRC on the 3,582 TBs after the gap vs 69.3 % cumulative before;
  DL 6.1 % → 53.1 % (the DL interpretation search settling late in the run, not the gap). No collapse.
- **`TRACKING` is overstated.** `ul_converged` is `width_winners>0 || interp_winners>0`, and the
  interpretation search (Component 3) armed in **zero** replays (`icls=0 itrials=0` in every
  heartbeat): it only starts when the width winner's CRC upper bound is < 0.60, and the winner decodes
  at ~74 %. So on every capture on disk `TRACKING` means "width converged + DL settled", never
  "interpretation resolved". Component 3 is unexercised on real data and cannot be exercised with the
  captures we have.
- **Hand-set values introduced by this work, none derived from a measurement:**
  `NR_PASSIVE_ACQ_LOSS_HYSTERESIS = 8` (never exercised on real data — the real loss path bypasses it);
  `coreset_extent_verified := !autodiscover || verified` (operator-configured CORESET counted as verified
  by fiat); the gate-5 verdict's "recovered" = any post-LOST state other than LOST/SEARCHING (too weak:
  `PBCH_LOCKED` alone would pass — should require regaining at least the pre-loss state);
  `NR_ISAC_ILLUM_DL` for every row in the repaired ISAC tests (single-illuminator assumption). The
  gate-1 oracle command pins `-r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150` by design —
  it is the documented decoder regression, NOT gate-1 inference, and must not be read as agnostic.

## Three formerly-assumed parameters now measured off the air (2026-09-12, `d848c29298`)

| parameter | how | live verdict on `raw_5min_last120` (two independent replays) |
|---|---|---|
| DM-RS scrambling identity, PDSCH | blind, all 1024 candidates, adjacent-pilot coherence on the receiver's own LS estimate (`nr_dmrs_id_estimate.{h,c}`) | **n_ID = 2 = PCI**; 20.5 / 20.2 dB over median, 15.8 / 14.9 dB over runner-up, 16 CRC-OK grants |
| DM-RS scrambling identity, PUSCH | same estimator, gNB-style rxdataF ring, consumer-only | **n_ID = 2**; 16.2 / 16.3 dB over median, 11.1 / 11.1 dB over runner-up |
| xOverhead | reject-only TB-CRC elimination (`nr_pdsch_xoverhead.{h,c}`): a CRC-OK TB refutes every value whose TBS for that allocation differs | **0 CONFIRMED** after the first CRC-OK decode; 6/12/18 each refuted, 0 TBS-indistinct |
| carrier (BW, numerology, position in the started grid) | SIB1 `carrierBandwidth`/`subcarrierSpacing` exact; `offsetToPointA` + MIB `k_SSB` against where the SSB was *found* must put Point A at grid subcarrier 0 and the carrier end at 12·N_RB | **CONFIRMED**: 273 PRB µ=1, Point A at 0, end 3276 = grid end, SSB at 150 |

Each has a gtest with a known answer (`test_nr_dmrs_id_estimate` 5, `test_nr_pdsch_xoverhead` 5,
carrier cases in `test_nr_passive_acq_state`), and a **MISMATCH** path that logs at `LOG_E`
naming both values — the detector these assumptions never had. The ACQ heartbeat line carries all
three verdicts. Gate-5 regression with the new binary: PASS, verdicts reproduced.

Design notes that are not obvious from the code:
- The DM-RS score accumulates the complex numerator and real denominator across grants; the
  first version accumulated per-grant |num|/den and the margin could never grow with evidence
  (the magnitude of a random walk is biased positive). Decision gate is relative (dB over the
  median of all 1024), so gain/SNR/allocation size do not enter it. 16 grants + 10 dB are
  hand-set; the measured margins sit 6–10 dB above the gate.
- xOverhead is NOT a 4th Technique-D dimension on purpose: that multiplies DL convergence by 4
  and would not settle inside the 120 s captures (DL settles ~75 s via the full MIN_TRIALS gate;
  the 60 % LCB shortcut cannot fire at 23–53 % CRC). If the assumed value were wrong nothing
  passes CRC and Technique D never settles — the fallback (a real 4-way search) is documented,
  not built.
- The carrier check is not a PHY re-init (impossible after sync): it converts "started with" into
  "confirmed from air" or a loud mismatch.
- The UL estimator is gated on the PUSCH queue running: `nr_pusch_passive_decode()` is also the
  RT thread's in-line decode, and a millisecond burst there is a timing-loop hit.

**Agnosticity count, revised** (same 35-item list as before): validated-autonomous **24/35 ≈ 69 %**
(was 20); designed-but-unproven 4; supplied/assumed 7 — remaining: initial RF centre (a scan-time
problem), DL/UL *data*-scrambling IDs (gated fallback only), DMRS type-2 / maxLength-2 and
transform precoding (receiver refuses rather than estimates), rank > 1, CSI-RS resources.

Also recorded from this campaign: decode rates on the same capture read UL 54 % / DL 23 % on one
replay and 74 % / 32 % on another with an identical binary — the rig's documented run-to-run
swing; no per-binary comparison is claimed from n = 1.

### Two more verdicts at zero cost (`23f89fffab`)
- **Data-scrambling IDs, DL and UL** — were "assumed = PCI"; a CRC-OK TB is already the proof
  (c_init = RNTI·2¹⁵ + n_ID). Now stated once per direction. Live: PDSCH n_id=2, PUSCH n_id=2.
- **Carrier centre** — the SIB1 check now derives the cell's absolute carrier centre
  (Point A + 12·offsetToCarrier + 6·BW in the started grid's SCS). Live: 3450.000000 MHz = started.
  On a mismatch it is logged as the retune target. Still NOT done: the cold-start band walk to find
  the SSB in the first place (a retune loop a replay cannot exercise).

Agnosticity count, revised again: **27/35 ≈ 77 %** validated-autonomous. Remaining assumed:
initial RF tune (scan-time), DMRS type-2 / maxLength-2, transform precoding (refused, not
estimated), rank > 1, CSI-RS resources.

## First OTA run of the agnostic receiver (2026-09-12, X410, live cell, 2 UEs bidirectional iperf) — `e93f946d0a`

Harness: `tests/passive_rx/captures/run_arm.sh` with `REPO=` pointed at this tree and a conf with
NO manual layout (`captures/agnostic_ota.conf`). Artifacts `captures/agnostic_ota_*`,
`rankprobe_*`, `cdmprobe2_*`, `dcigt_*`, `layoutpref_171950` (the VALID one).

| stage | result |
|---|---|
| acquisition | PBCH → SIB1 → carrier CONFIRMED → CELL_CONFIGURED, every try |
| UL | 0_1 length + width search converged; **PUSCH CRC 71–87 %**; this branch previously had zero UL 0_1 accepts |
| six off-air verdicts | all reproduced OTA (DM-RS id PDSCH ~20 dB, PUSCH ~17 dB) |
| RNTIs | 0x461e / 0x46ae, confirmed against the gNB log over the byte-bracketed window |
| DL, first 3 tries | **VOID_DL_RATE**: full-band 13-symbol grants 0/10,000, short grants 8/9 |
| DL, after fix | **VALID**: family preferred at 8 passes → Technique D converged (S=1 L=13, DM-RS 2/7/11, 256QAM) → **82 % CRC post-convergence**, 58 % over 200 s |

**Hypotheses refuted by measurement, in order** (each one a new probe kept in-tree): rank 2 —
DM-RS port-pair coherence 1.00 on 16,499 failing grants (and RI is pinned to 1); 2 CDM groups —
the δ=1 comb carries data on 21,481 failing grants; LBRM layers/table — refuted analytically
(N_ref never clips at MCS 10), its sweep reverted unrun; link margin — `segs_decoded=1725/1.36M`,
zero code blocks pass inside failing TBs, so systematic.

**Root cause**: several DCI-1_1 field-width families are valid at one length and the RT path
round-robined them per grant; the family that decodes got ~1/14 of the grants, so its Technique-D
context never reached `min_trials`. Overall DL CRC (0.3 / 15 / 26 % across runs) was just that
share. **Fix**: prefer the family whose context has TB-CRC passes (≥ 8) — reject-only, same
principle as every other estimator today.

**Also found**: HEAD's `nr_pdsch_config_sweep.h` did not compile (a zero-context-staged
declaration inside a comment block, from `0e8972973a`). Fixed; the staged tree is now
syntax-checked per file before every commit.

**Agnosticity, revised**: rank and CDM-group count are now *measured* → **29/35 ≈ 83 %**.
Remaining assumed: initial RF tune, DMRS type-2 / maxLength-2, transform precoding (refused),
rank > 1 *decode* (measured but not decodable), CSI-RS resources.

**Not established**: run-to-run repeatability (one VALID run; the rig's own rule is ≥ 5 per arm),
and both UEs' DL — the VALID run's converged contexts are for 0x47eb only.
