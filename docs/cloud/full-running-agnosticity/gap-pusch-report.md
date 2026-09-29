# Gap lane: pusch

Status: **DONE_WITH_CONCERNS**

Commits:
- `sdd/gap-pusch` @ `77234c7f21` ("Gap lane pusch: PUSCH RA type 0/dynamicSwitch decode, TDRA
  type B energy oracle, 0_0 scrambling-ID audit"), forked from `sdd/validation` @ `c295fa18f1`.
- `sdd/gap-pusch` @ `ab5145de09` ("Gap lane pusch fix round 1: fix pin/energy-oracle races, keep
  type-B mapping ambiguity, test fixups") — fix round addressing the G5 review of `77234c7f21`,
  see "Fix round 1" below.

Files touched: `nr_pdcch_blind_monitor.{h,c}`, `nr_pdcch_blind_monitor_rt.c`,
`nr_pdcch_ul_discovery.c`, `nr_pdcch_ul_interp_sweep.{h,c}`, `nr_pusch_passive_decode.c`,
`tests/nr_pdcch_blind_monitor_test.cc`, `tests/nr_pdcch_ul_interp_sweep_test.cc`.

## Fix round 1 (addressing G5 review of `77234c7f21`)

Three "needs fixes" items and two minors, all addressed in `ab5145de09`:

1. **Race in `nr_pusch_ul_dmrs_pin_set()`.** Was check-then-act: load the validity flag, then plain
   (unguarded) writes to `g_ul_dmrs_pin_S/L/mapping`, then store the flag — no ordering against a
   concurrent writer, so two UL consumer threads racing in (`ul_thread` can run more than one) could
   let a reader observe a torn mix of two callers' values. Fixed with a 3-state claim: `EMPTY` ->
   `WRITING` via `atomic_compare_exchange_strong` -> `VALID` via a release-store. Only the CAS winner
   ever writes the fields; the reader's acquire-load only accepts `VALID`, so once visible the plain
   writes are always whole. A losing CAS (already `VALID`, or another thread mid-`WRITING`) discards
   its own `(S,L)` — correct under "first observation wins".
2. **Race in `ul_dmrs_energy_oracle_observe()`.** Was a relaxed load/RMW/store on `g_seen_mask` plus
   plain (non-atomic) writes into `g_profile[14]` — same concurrent-UL-thread case, and a double
   array can't be updated atomically piecewise regardless. Fixed with a `pthread_mutex_t` guarding
   only the shared-state merge (`g_ul_energy_profile[]`/`g_ul_energy_seen_mask`) and the span-ready
   check; each call's own per-symbol energy is computed *outside* the lock (a pure read of that
   grant's own already-FEP'd, per-thread `rxdataF` — not shared), so the held section is a handful
   of stores. `nr_pusch_ul_dmrs_pin_set()` is still called outside the lock (independently
   thread-safe per fix 1) — no nested-lock ordering concern.
3. **Mapping-type guess at S==0 was spec-ambiguous and latched wrong forever.** TS 38.214 Table
   6.1.2.1-1 makes type B legal at S=0 too (any L with S+L<=14), not only type A (S=0, L>=4) — an
   `(S,L)` like `(0,14)` is legal under *either* mapping and energy occupancy alone cannot
   distinguish them. The original `(S==0 && L>=4) ? typeA : typeB` heuristic silently picked type A
   and would have permanently excluded every legal type-B row at that exact `(S,L)`. Fixed: a new
   `NR_PUSCH_MAPPING_EITHER` sentinel; the pin setter now tests *both* mapping types against
   `nr_pusch_tda_legal()` and pins `EITHER` when both admit the span (a single definite type when
   only one does, nothing when neither does); `nr_pdcch_ul_interp_sweep_generate_pinned()` generates
   the full k2×96-combo block for *every* mapping type an `EITHER` pin admits (concatenated), so an
   ambiguous pin still reaches the whole legal candidate set instead of silently dropping half of
   it. The test that asserted the old wrong single-type guess
   (`UlDmrsPin.DerivesMappingTypeFromSAndL`) is replaced by
   `UlDmrsPin.AmbiguousSpanKeepsBothMappingTypesRatherThanGuessing` (asserts `EITHER` is pinned and
   both mapping types' hypotheses are generated) and `UlDmrsPin.ShortSpanAtSZeroIsUnambiguouslyTypeB`
   (S==0 with L<4 is unambiguous type B — S==0 never by itself implies type A).

Minors also fixed: the `width_hyp_class` comment at `nr_pdcch_blind_monitor_rt.c` (~5449) read as
though describing the state *after* the fix-up line when it described the state *before* it —
reworded in order; `Dci01ExtractRaType0BitmapProducesTheCorrectPrbList` used `bwp_size=52`, where
52 = 13×4 exactly so the "last, partial" RBG it claimed to exercise was actually a full 4 PRBs —
changed to `bwp_size=50` (13×4−2), whose last RBG is genuinely 2 PRBs, exercising
`nr_ra_type0_prbs()`'s `bwp_size-1` boundary branch instead of accidentally skipping it.

New test `UlDmrsPin.ConcurrentSetIsNeverTornEvenUnderContention`: 5 threads racing
`nr_pusch_ul_dmrs_pin_set()` with distinct `(S,L)` pairs, 50 trials, asserts the winner's `(S,L)`
always matches one full candidate (never a mix) and its mapping is exactly what that `(S,L)`
legally admits. Cannot prove the absence of a race in one run, but is the shape a thread sanitizer
or an unlucky interleaving would catch, and pins the invariant the CAS-claim fix depends on.

**Verification**: rebuilt via `lane-make.sh` (`nr-uesoftmodem` + `test_nr_pdcch_blind_monitor`),
both clean, no new warnings introduced. `./test_nr_pdcch_blind_monitor`: **170/172** (2 pre-existing
skips) in default order, and under `--gtest_shuffle` at 5 different `--gtest_random_seed` values
(1–5) plus `--gtest_repeat=3` — 0 failures attributable to this lane's code or tests in any run.
`ctest` on the 7 relevant suites: 7/7.

**One pre-existing, unrelated finding surfaced by `--gtest_shuffle`** (seeds 3 and 5, requested by
the reviewer as a validation method): `Css0Autoconf.TurnsOffEverySettingThatDescribesTheDedicatedSearchSpace`
intermittently fails under shuffle, with `c->energy_adapt_factor`/`c->energy_min` left non-zero
after `nr_pdcch_blind_monitor_autoconf_css0()` is asked to clear them — a global-singleton
(`nr_pdcch_blind_monitor_get_cfg()`) cross-test pollution issue in fields this lane's code never
touches (CFAR/energy-gate config, not PUSCH/UL FDRA/TDRA). Confirmed NOT caused by this lane:
(a) no path in this diff writes `energy_adapt_factor`/`energy_min` or calls `autoconf_css0`;
(b) the test passes every time run in isolation (`--gtest_filter=Css0Autoconf.*`);
(c) built a throwaway worktree at the parent commit `c295fa18f1` (pre-this-lane) and confirmed the
same class of shuffle-order dependency is structurally present there too — the direct trace (which
test runs immediately before it, and the exact fields left dirty) shows global test-singleton
pollution unrelated to any of this lane's UL PUSCH changes. Left unfixed as out of scope for this
lane (a pre-existing test-isolation defect in a 3700+-line shared test file, not a "new commit"
issue) — flagged here for whoever owns general test-suite hygiene.

Tests after fix round 1: **15** new tests total (14 from `77234c7f21`, minus the 1 replaced by the
2 ambiguity tests, plus the 1 new concurrency test = 15).

## What was closed

### 1. PUSCH RA type 0 / dynamicSwitch (DCI 0_1) — was "detected and refused", now decodes the
   contiguous case

Root cause: `nr_pdcch_blind_ul_opts_t`/`nr_pdcch_blind_ul_result_t` had no `fdra_mode` field at all
— `blind_ul_field_bits()` always sized the frequency-domain field as a RIV and `blind_ul_finish()`
always called `riv_to_prb_alloc()`. The existing "detection" (`g_dci01_fdra_armed` /
`g_dci01_fdra_refuse` in `nr_pdcch_blind_monitor_rt.c`) is a type-1-refutation *verdict* built on
the DL 1_1 side's `nr_dci11_resolver_t` machinery (reused for UL as `g_dci01_resolver`) — it decides
whether to book or refuse, but nothing downstream ever tried a different interpretation.

Fix:
- Added `fdra_mode`/`fdra_bwp_start` to the opts struct (appended at the struct's *end*, not
  grouped with the UL BWP fields, specifically so as not to disturb the alignment padding the
  pre-existing test `UlFeedbackOwnershipIgnoresConfigurationPadding` pins between
  `dmrs_typeA_position` and `tda_count` — this bit me once, caught by that test, fixed by moving the
  fields rather than touching the test).
- Added `ra_type0`/`rbg_size`/`rbg_bwp_start`/`rbg_bitmap` to the result struct.
- `blind_ul_field_bits()` and `blind_ul_finish()` now branch on `fdra_mode` and decode via
  `nr_pdsch_prb_set.h`'s `nr_fdra_bits()`/`nr_fdra_prbs()` — the exact primitives the DL 1_1 side
  already uses for the same TS 38.214 6.1.2.2.1 table. This is a pure decode-primitive change: any
  caller that sets `fdra_mode` now gets a correct PRB list out.
- New `nr_pdcch_blind_ul_fdra_mode_candidates(opts, observed_len, ...)`: given an
  already-established DCI length (from the existing type-1 decode of this RNTI's grants), returns
  which of `{TYPE0_CFG1, TYPE0_CFG2, DYN_CFG1, DYN_CFG2}` reproduce that same length with every
  other field width held fixed — the "DCI length consistency" discovery method the brief named.
  This is a *discriminator*, not a joint search over every other field width too (that would be a
  much larger undertaking, see Concerns); the eventual TB CRC is still the real oracle.
- Wired into `nr_pdcch_blind_monitor_rt.c`: when the existing verdict would have refused a grant
  outright (`ISAC_UL_FDRA_REFUSE=1`, the same pre-existing opt-in gate — no new knob added), the
  grant is now re-extracted under each length-consistent candidate mode instead of being dropped.
  **Only a CONTIGUOUS resulting PRB set is booked.** Reason: this receiver's actual PUSCH decode
  (`nr_pusch_passive_decode.c`) calls `nr_rx_pusch_group_tp()`, which is the *real gNB PHY receive
  function*, shared with actual gNB uplink reception — it reads `rb_start`/`rb_size` only and has no
  `rb_bitmap` support (this is R11's pre-existing finding: "OAI's gNB PUSCH receiver here ignores
  rb_bitmap/resource_alloc"). A genuinely non-contiguous RA-type-0 grant is still refused, loudly,
  with the reason logged — decoding it would need this receiver's own per-segment UL extraction
  (repeated `nr_rx_pusch_group_tp()` calls, one per contiguous segment, mirroring how the DL 1_1
  side already handles non-contiguous PRB lists via `nr_prb_segments()`), which is **not built**.
  The retry's own TB CRC outcome is tagged non-oracle-class (`width_hyp_class=0`) so
  `nr_dci01_fdra_note()` cannot misread a pass under a type-0 interpretation as evidence *for*
  type 1 — it correctly falls into the same `link_ok` bucket a 0_0 grant's CRC pass already does.

### 2. PUSCH TDRA type B — was "only (0,4),(2,12)", now reaches the full legal (S,L) space once pinned

Root cause: the curated candidate catalogue (`nr_pdcch_ul_interp_sweep_generate()`, 10 TDA rows) is
deliberately narrow because the full legal set (11 type A + 105 type B) × k2{1..4} × 96 field
combinations is 44,544 raw hypotheses against `NR_HYP_SWEEP_MAX_RAW = 8192`, and there was no oracle
to prune it first.

Fix: a UL DM-RS **energy** oracle (not a DM-RS *sequence* oracle — unlike the DL side's
`nr_dmrs_prb_coherence()`, dmrs-Type/maxLength/AdditionalPosition are exactly what isn't known yet,
so sequence correlation isn't available; energy occupancy is):
- `nr_pusch_ul_energy_span(energy[14], rel_thresh, &S, &L)` — pure function, occupied-symbol span
  from a per-symbol energy profile (same physical justification as `dmrs_oracle_measure()`'s
  "last symbol" rule in `nr_pdsch_passive_queue.c`: an allocated symbol is within a few dB of the
  peak, an unallocated one reads near-zero).
- `nr_pusch_ul_dmrs_pin_set/get/reset()` — one cell-wide pin, first observation wins (same rule the
  DL DM-RS oracle already uses), mapping type derived from `(S,L)` via `nr_pusch_tda_legal()`.
- `nr_pdcch_ul_interp_sweep_generate_pinned(S, L, mapping)` — once pinned, generates the full
  k2{1..4} × 96-combo product at that ONE (S,L,mapping) instead of the curated list: 384
  hypotheses, well under the cap, and now reachable at ANY legal (S,L), not just the two curated
  type-B rows.
- Wired into `nr_pdcch_ul_discovery.c`'s `init_search()`: uses the pinned generator when a pin
  exists, falls back to the old curated catalogue byte-for-byte otherwise.
- Live measurement (`nr_pusch_passive_decode.c`'s `ul_dmrs_energy_oracle_observe()`) is read-only
  over the rxdataF THIS grant's own FEP already computed — **zero extra FEP calls**, so no added RT
  cost on the steady-state path (checked first: a no-op once a pin exists). It accumulates a
  per-symbol max-energy profile across grants; since the curated catalogue's own `(0,14)` row is
  tried by every interpretation search and already spans the whole slot, symbol coverage reaches
  all 14 symbols on its own well before this is needed.

### 3. PUSCH scrambling IDs for DCI 0_0 — audited, confirmed already spec-correct, pinned with a test

Checked against TS 38.214 §6.1.1.1 and TS 38.211 §§6.3.1.1/6.4.1.1.1.1: format 0_0's field list is
fully spec-fixed (no RRC-derived content at all), and both the data and DM-RS scrambling identities
of a 0_0-scheduled PUSCH are the physical cell ID — this is unconditional, with no USS/CSS
distinction (0_0's *lack* of RRC content, not its search-space location, is what forces PCI).
`blind_ul_apply_scrambling_ids()` already implements exactly this (`dedicated := format==0_1`), so
**no production fix was needed** — added
`Dci00ScramblingIdsAreAlwaysThePciNeverTheDedicatedEstimate` as a regression guard instead.

**Documented enhancement, not implemented**: the more general `nr_scrambling_dedicated()` rule
already used for DL 1_0 (dedicated for C-RNTI outside a CSS occasion, PCI otherwise) is *provably
inapplicable* to 0_0 given the spec text above (0_0 forces PCI regardless of search space), so this
is not a gap after all on reflection — flagged in the commit message as something I considered and
ruled out by spec, not left half-investigated.

## Tests

15 new tests (after fix round 1), all passing:
- `nr_pdcch_blind_monitor_test.cc`: `Dci01FdraModeCandidatesNarrowByLengthConsistency`,
  `Dci01ExtractRaType0BitmapProducesTheCorrectPrbList`, `Dci01ExtractDynamicSwitchBothBranches`,
  `Dci01ExtractRaType0EmptyBitmapIsRejected`, `Dci01FdraModeChangesTotalDciLength`,
  `Dci00ScramblingIdsAreAlwaysThePciNeverTheDedicatedEstimate`.
- `nr_pdcch_ul_interp_sweep_test.cc`: `UlInterpSweepPinned.ReachesAFullTypeBPointTheCuratedCatalogueNeverOmits`,
  `UlInterpSweepPinned.RejectsAnIllegalPin`, `UlEnergySpan.RecoversTheOccupiedSpanFromAPeakedProfile`,
  `UlEnergySpan.IgnoresNoiseBelowTheRelativeThreshold`, `UlEnergySpan.AllZeroProfileFails`,
  `UlDmrsPin.GetFailsUntilSetThenLatchesTheFirstValue`, `UlDmrsPin.RejectsAnIllegalSpanAndStaysUnset`,
  `UlDmrsPin.AmbiguousSpanKeepsBothMappingTypesRatherThanGuessing`,
  `UlDmrsPin.ShortSpanAtSZeroIsUnambiguouslyTypeB`, `UlDmrsPin.ConcurrentSetIsNeverTornEvenUnderContention`.

Ran (all on `gap-pusch`'s own build dir, replicated from `val`'s CMake cache):
- `./test_nr_pdcch_blind_monitor` (direct binary run): **170/172 passed** in default order, 2
  pre-existing skips (`PdcchReplay.OtaCss0DecoderContract`, `PdcchReplay.BudgetTimingEveryWidth`),
  0 failures, 0 regressions (started at 156/158 before this lane's changes). Also run under
  `--gtest_shuffle` at 5 seeds and `--gtest_repeat=3` — see "Fix round 1" above for the one
  pre-existing, unrelated finding that surfaced and why it isn't this lane's.
- `ctest -R 'nr_pdcch_blind_monitor|nr_pdsch_prb_set|nr_pdcch_ul_interp_sweep|nr_pdcch_ul_field_sweep|nr_pdcch_dci01_layout_sweep|nr_pdcch_dci11_layout_sweep|nr_scrambling_id_sweep'`:
  **7/7 suites pass**.
- `nr-uesoftmodem` (the full executable, all production files): **builds clean** with every change
  in this lane applied together (confirmed with a full rebuild, not just the object files that
  changed), both before and after fix round 1.

No live capture was run for this lane's own validation (see Concerns) — a `pgrep -x nr-uesoftmodem`
check was run before every build per the shared rule, and this lane waited out several other lanes'
captures (`gap-rank4`, `val`, `gap-perf`) during the session rather than building over them.

## Concerns / what's still open

1. **The RT wiring (item 1) and the live energy-oracle wiring (item 2) are PHY/RT-thread-coupled
   and are verified by build and reading only, not by a unit test** — matching this codebase's own
   established convention for RT-thread changes (e.g. `nr_pusch_grant_book_add`'s caller,
   `nr_pdcch_dci01_layout_observe`, and others in this file carry the same "verified by build" note
   in `final-fix-report.md`'s I6). Neither has been exercised on a live capture.
2. **Item 1's decode is genuinely limited to the contiguous case, by design, not by oversight.**
   A non-contiguous RA-type-0/dynamicSwitch grant is refused, loudly, with a log line naming the
   reason. Closing this fully needs this receiver's own per-segment UL extraction (repeated
   `nr_rx_pusch_group_tp()` calls per contiguous segment) — I judged building and correctness-
   verifying that without any live-test capability in this session to be more likely to introduce a
   subtle, hard-to-detect wrong-PRB decode than to leave it explicitly refused. This is the
   "still-open item" referenced in the commit message.
3. **Item 1's candidate-mode search is NOT a joint search.** `nr_pdcch_blind_ul_fdra_mode_candidates()`
   holds every OTHER DCI 0_1 field width fixed at whatever the cell's current baseline is (spec
   default unless RRC-derived overrides are configured) while varying only `fdra_mode`. On a cell
   where a *different* field width also needs to change to accommodate a non-type-1 FDRA (e.g. a
   configured `bwp_indicator` making the true total length match under a combination this receiver
   isn't holding fixed), the candidate search will correctly find nothing, and the grant stays
   refused rather than silently misdecoded. The DL 1_1 side needed a considerably larger joint
   resolver (`nr_dci11_resolver_t`, Task 10/15) to do this fully; replicating that for UL 0_1 is a
   materially larger undertaking than this lane's scope.
4. **Item 2's live wiring has not measured how quickly a pin actually forms on real traffic.** The
   design assumes the curated catalogue's `(0,14)` row gets tried often enough, across enough
   distinct grants, to cover all 14 symbols before a real deployment's TDRA discovery would
   otherwise need the pin — this is a reasonable expectation (that row is tried on every
   interpretation-search attempt) but is asserted, not measured, in this session.
5. **OCUDU live validation, per the lane brief.** The brief states OAI's own gNB ignores
   `rb_bitmap` on UL (R11) so item 1's non-contiguous case can never be validated against it even in
   principle, and that a test patch for RA type 0 is being built in another lane for the OCUDU gNB.
   For THIS lane's contiguous-case fix, the expected live evidence on a real cell that schedules RA
   type 0 or dynamicSwitch for UL would be:
   - Config knob: whatever the OCUDU-side patch exposes to force `resourceAllocation`/`rbg-Size` on
     `PUSCH-Config` to type 0 or `dynamicSwitch` (not available on stock OAI gNB, which is
     RIV-only for UL — confirmed by reading, not run).
   - Expected log line on this receiver: `SENSING: UL_FDRA_TYPE0_DECODED n=<n> rnti=0x<rnti>
     mode=<NR_FDRA_*> rbg_bitmap=0x<bitmap> start_rb=<s> num_rb=<n>` (new in this commit,
     `nr_pdcch_blind_monitor_rt.c`), followed by the grant's PUSCH TB CRC passing at the
     `PUSCHQ`/`pusch_passive` census lines that already exist.
   - For item 2: `SENSING: UL_DMRS_PIN S=<s> L=<l> mapping=<A|B>` (new, `nr_pdcch_ul_interp_sweep.c`)
     should appear once per cell/run, followed by `UL discovery interpretation armed:` picking up a
     larger `classes=` count than the pre-existing 10-row catalogue would produce.
6. **`nr_pdcch_blind_extract_00()` has no CSS/USS input**, which is why item 3's documented
   enhancement (immaterial per the spec re-check, see above) couldn't have been wired even if it had
   turned out to matter — noted for anyone revisiting DCI 0_0 extraction later.
