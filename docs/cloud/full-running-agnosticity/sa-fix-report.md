# SA discovery-stall fix report (R31)

Lane: sens6:/home/sens/NICOLA/agn-wt/sa, branch `sdd/agn-sa`. Commit `028ed102cf`.

## Status: DONE

Both gates fixed, tests added and passing, `nr-uesoftmodem` builds clean. No rfsim/OTA validation
attempted here (deferred per instructions to another machine with a core).

## Gate 1 (structural)

`nr_pdcch_blind_monitor_process_body()` (`nr_pdcch_blind_monitor_rt.c`) returned early on
`nr_pdcch_coreset_bank_count() == 0` right after the per-slot Technique-A discovery step, which
made that early return fire every DL slot for the whole capture until autodiscover converged.
`nr_pdcch_blind_monitor_run_occasion()` — called further down, past the on-occasion gate — already
has a cheap CORESET#0-USS/RAR-anchor pass specifically for `bank_count()==0`, but it was
structurally unreachable.

Fix: extracted the decision into `nr_pdcch_blind_monitor_discovery_block_early_return(int
bank_count)` (declared in `nr_pdcch_blind_monitor.h`, implemented in `nr_pdcch_blind_monitor.c`,
next to `nr_pdcch_blind_monitor_discovery_paused()`) — always returns `false`. `rt.c`'s
process_body now calls this instead of the inline check, so control always falls through to the
on-occasion gate and `run_occasion()`, whose own `n==0` branch (CSS0-USS pass + default pass)
already handles the empty-bank case correctly and cheaply (same cost this deployment already pays
once discovery finishes or is paused — verified against the BTIM profiling comment already in
`rt.c`: 69-102 us/occasion at 165-1530 grants/s). The predicate is a pure function purely so it's
unit-testable, since `nr_pdcch_blind_monitor_rt.c` needs a live `PHY_VARS_NR_UE` and is not linked
into the test binary (`nr_pdcch_blind_monitor.c` is).

## Gate 2 (DSP)

Technique A's `ISAC_DISCOVER_MIN_BG=3` gate (`nr_pdcch_blind_monitor.c`) required the whole-carrier
median hit count to reach 3 before any dwell could complete, regardless of how significant one
window already was. Live evidence: top window climbed 16->138 hits over 45000 calls while every
other window stayed near the noise floor and the median never reached 3 — zero dwells ever
completed.

Fix: inside the existing `bg < s_min_bg` branch, added a dominance bypass. A window is now also
admitted when:
- its hit count >= **K=4** times the best of every *other* window (CSS0's window range excluded
  from both sides of the comparison, mirroring the exclusion already used elsewhere in this
  function for the same reason), **and**
- its hit count >= **N=3** times `AUTODISCOVER_HITS_PER_WINDOW` (30).

**K=4 justification**: reused, not invented — `nr_pdcch_coreset_map.c`'s own
`CORESET_MAP_CORR_THRESHOLD` is documented as "4x the noise floor... well [above chance]" for a
single correlation sample. Applied here as a hit-count dominance ratio. Cross-checked against this
file's own recorded false positive (an 11-hit window against a background of 6, ~2 sigma, was
wrongly declared a footprint before the `lit_floor` fix that comment documents) — that case is a
1.8x margin, well under K=4, so the bypass still correctly rejects it.

**N=3 justification**: three multiples of `AUTODISCOVER_HITS_PER_WINDOW`, the code's own "one
window's worth of trustworthy dwell evidence" bar. Every hit counted here already independently
cleared `nr_pdcch_coreset_map_scan()`'s own adaptive correlation threshold (capped at
`CORESET_MAP_CORR_THRESHOLD=0.836`), so the ruling's "correlation >= a threshold the code already
uses for real" requirement is inherited for free — no separate correlation check was added.
N*30=90 hits is also why pure noise cannot trigger this bypass: noise essentially never clears the
scan's own correlation floor (documented ~0.1-0.25 vs a 0.35 absolute floor), so it cannot
accumulate that volume of hits on any one window within a bounded dwell.

Neither constant was tuned to the lab/macro cell — both are derived from statistics
`nr_pdcch_coreset_map.c`/`nr_pdcch_blind_monitor.c` already trust.

## Tests

`openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc`, new `DiscoveryGates` suite:
- `Gate1EmptyBankNeverBlocksTheOnOccasionFallback` — calls the extracted decision helper with
  `bank_count` 0/1/5, asserts always `false` (rt.c can't be unit-tested directly).
- `Gate2SparseBackgroundDominantWindowConverges` — synthesizes one genuinely-occupied 6-RB window
  (real PDCCH DM-RS, same technique as `nr_pdcch_coreset_map_test.cc`) on a 48-PRB carrier and
  drives `nr_pdcch_blind_monitor_autodiscover_step()` directly; asserts it converges (returns
  `true`, `coreset_rb_offset` matches the injected window) within ~8000-9000 calls, where the old
  gate could only ever clear the median floor via `AUTODISCOVER_MAX_OBS_CALLS=400000`/dwell (400000x
  more calls per dwell). One real cross-test-isolation bug found and fixed in the test itself: static
  globals `s_css0_excl_first_w/last_w` are set (and never reset) by earlier `Css0Autoconf`-style
  tests in the same binary, which was blanket-excluding this test's whole small carrier when run as
  part of the full suite (passed filtered alone, failed in the full run) — fixed by having the test
  own that state explicitly via `nr_pdcch_blind_monitor_autoconf_css0()` before running.

## Test summary

- `test_nr_pdcch_blind_monitor` (full binary): **143 passed**, 2 skipped (`PdcchReplay.OtaCss0DecoderContract`,
  `PdcchReplay.BudgetTimingEveryWidth` — the two known pre-existing skips), 0 failed.
- `test_nr_dl_adaptive` (`DlAdaptive.*:DlGeometry.*`, via ctest): all pass (11 tests, `Passed 0.15 sec`).
- `nr-uesoftmodem`: builds clean (`Built target nr-uesoftmodem`).

## Concerns

- Gate 2's K/N were justified analytically and against the one recorded false-positive case in the
  code's own comments, not against a fresh live capture — per the brief, no rfsim/OTA run was done
  here. Worth re-checking against a live sparse-traffic capture on another machine before treating
  the constants as final.
- The cross-test-isolation gap in `nr_pdcch_blind_monitor.c`'s static globals (CSS0 exclusion state,
  long-term dwell accumulation) that the Gate 2 test had to work around is pre-existing and broader
  than this task — not fixed generally, only worked around locally in the new test.
- Gate 1's fix removes a return that also happened to bound per-slot CPU while undiscovered; the
  new cost matches what this deployment already pays once discovery is done/paused (documented and
  measured elsewhere in `rt.c`), but that has not been re-measured live in this session.

## Fix round 1 (controller ruling on 028ed102cf review)

Commit `801de181a1`.

### 1. `nr_pdcch_blind_monitor_autodiscover_reset()` now clears the long-term dwell state (IMPORTANT)

Checked every caller first (`grep -rn nr_pdcch_blind_monitor_autodiscover_reset` across the whole
tree): as of this fix it has **no live RT/production caller at all** — only three test call sites
(`nr_dl_adaptive_test.cc`'s fixture SetUp/TearDown and one ad hoc call, plus the Gate 2 test added
in the first round). So clearing more state here could not regress any runtime behavior today;
adding the clears simply makes "reset" mean what its name promises, ready for whenever a production
re-discovery path (BWP switch, cell change) starts calling it.

Added to the reset: `memset` of `s_lt_hits`/`s_lt_dwells`/`s_lt_rnti` and `s_lt_ndwell = 0`. These
are NOT diagnostic-only, despite an adjacent stale comment claiming so (corrected in the same
patch) — `nr_pdcch_blind_monitor_autodiscover_step()`'s `MIN_ORACLE_DWELLS`/seed-selection logic
reads `s_lt_dwells`/`s_lt_hits` directly, and Gate 2's own dominance bypass shares that same
long-term state's convergence bookkeeping. Leaving them uncleared meant a second discovery in the
same process could seed itself from the first discovery's leftover recurrence counts — order-
dependent under `--gtest_shuffle`/sharding, and in production a real re-discovery latching onto a
stale footprint from before a BWP switch or cell change.

New test: `DiscoveryGates.Gate2ReconvergesAfterPriorDiscoveryInTheSameProcess` — runs the Gate 2
synthetic-discovery scenario twice in the same process (window RB 18, then reset, then window RB 0),
and requires the SECOND run to both converge within the same ~8000-9000 call bound and land on the
SECOND window's `coreset_rb_offset`. Without the reset fix this would have been able to pass
suspiciously fast by latching onto the first window's already-accumulated `s_lt_dwells` (>=
`recurrence_floor` from the first run) instead of genuinely re-discovering the new one — the
`EXPECT_EQ` on `coreset_rb_offset` is what would have caught that. Refactored the synthetic-symbol
construction shared between both Gate2 tests into `RunSparseDiscoveryToConvergence()` /
`ParkCss0OnWindow7()` to avoid duplicating it.

### 2. K/N exposed as env overrides

`ISAC_DISCOVER_DOMINANCE_K` / `ISAC_DISCOVER_DOMINANCE_N`, read once (static, `-1` sentinel) and
floored at 1, exactly `ISAC_DISCOVER_MIN_BG`'s existing pattern. Defaults unchanged (K=4, N=3).

### 3. LOG_A on the dominance bypass decision

One `LOG_A(PHY, "SENSING: Gate2 dominance bypass -- w%d=%d hits vs best-other=%d (K=%d N=%d) ...")`
right where `dominant` is found true. Rate-limited (first occurrence logged immediately, then every
2000th) since `dominant` stays true for most of a long dwell once it first fires — logging
unconditionally would flood the RT log at the same rate as the existing `DISCOVERGATE` diagnostic's
own occasion volume.

### Test summary (fix round 1)

- `test_nr_pdcch_blind_monitor` full binary, normal order: **144 passed** (146 total, 2 known skips:
  `PdcchReplay.OtaCss0DecoderContract`, `PdcchReplay.BudgetTimingEveryWidth`), 0 failed. (Count went
  143→144 vs the first round because of the new `Gate2Reconverges...` test.)
- Same binary with `--gtest_shuffle`: 144 passed, 2 skipped, 0 failed — no `FAILED` lines, confirming
  the reset fix removed the order-dependency the ruling flagged.
- `test_nr_dl_adaptive` (`DlAdaptive.*:DlGeometry.*`, via ctest): passes (`Passed 0.17 sec`).
- `nr-uesoftmodem`: builds clean.

### Concerns (fix round 1)

- The reset fix is currently reachable only from tests (no production caller exists yet); the
  "in production a re-discovery would start from stale evidence" risk the ruling named is closed
  pre-emptively, not measured live, since there is no live path to measure yet.
- K/N env overrides and the new LOG_A are untested beyond the existing dominance-bypass test (which
  exercises the code path at its defaults); no test explicitly sets
  `ISAC_DISCOVER_DOMINANCE_K/_N` env vars to confirm the override plumbing itself, since the static
  `-1`-sentinel-once pattern only reads the env var on the FIRST call in a process — matching
  `ISAC_DISCOVER_MIN_BG`'s existing, already-untested-for-override convention, so this is consistent
  with prior art rather than a new gap.
