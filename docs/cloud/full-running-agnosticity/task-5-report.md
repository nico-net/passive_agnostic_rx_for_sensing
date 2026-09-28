# Task 5 report: Walk the AL1 cover first in the lookahead lanes (ISAC_AL1_COVER=1)

Lane: al1 (`sdd/agn-al1` @ `/home/sens/NICOLA/agn-wt/al1`, sens6).

## Status: DONE_WITH_CONCERNS

Both commits landed. Implementation matches the brief verbatim; `nr-uesoftmodem` builds and links
clean with the change; the new AL1-map test (8/8, was 7/7) passes. The one concern is a **pre-existing,
unrelated** link failure in `test_nr_pdcch_blind_monitor` / `test_nr_dl_adaptive` (same binary) — see
"Concern" below. It reproduces identically with Task 5's diff stashed out, at the base commit
`e4ef4dd55d`, so it is not something this task introduced or can fix within its own file scope.

## Commits

1. `e4ef4dd55d` — "AL1 map: drop unused kset_find" (pre-step cleanup, per instructions)
   - Deleted the dead static `kset_find()` from `nr_pdcch_al1_map.c` (only `kset_add` was ever
     called). 1 file changed, 7 deletions.
2. `3c23f34ec7` — "Blind PDCCH: optional AL1 cover lap before the staged mapping walk
   (ISAC_AL1_COVER=1)" — Task 5 itself. 4 files changed, 97 insertions(+), 3 deletions(-).

## What changed (Task 5)

- `nr_pdcch_blind_monitor.h`: added `bool al1_only;` to `nr_pdcch_lookahead_geom_t`, right after
  `fast_length_only`, exactly as specified.
- `nr_pdcch_blind_monitor.c`:
  - `#include "nr_pdcch_al1_map.h"` added with the other local includes.
  - `bool al1_only;` added to the internal `nr_pdcch_lookahead_lane_t`.
  - New statics/functions above `lane_map_count()`: `s_lane_after_cover_stage` (init 0),
    `al1_cover_enabled()` (`ISAC_AL1_COVER=1` gate, cached), `al1_cover_for_span()` (8-slot cache of
    `nr_pdcch_al1_cover()` results keyed by `(span_rb, duration)`, converting `nr_pdcch_al1_map_t` to
    `nr_pdcch_map_cand_t`).
  - `lane_map_count()`: `if (s_lane_dispatch_stage < 0) return al1_cover_for_span(...)` inserted right
    after `const int span_rb = ...`.
  - `lane_catalog_map_max()`: `if (s_lane_dispatch_stage < 0) return NR_PDCCH_AL1_MAX_COVER;` at the top
    (avoids computing the cover for every extent at init).
  - `lane_assign_next()`: new first branch inside `if (s_lane_dispatch_map >= s_lane_dispatch_map_max)`
    that, on stage `-1` (cover lap exhausted), resets to `s_lane_after_cover_stage` and `continue`s,
    logging `SENSING: autodiscover AL1 cover lap done, continuing with the staged mapping walk`.
    `ln->al1_only = (s_lane_dispatch_stage < 0);` added next to the existing `fast_length_only`
    assignment; the `ISAC_DISCOVER_DIAG` `LOOKAHEAD_ASSIGN` log line extended with ` al1=%d`.
  - `lookahead_lanes_init()`: `map_env`-derived stage is now stored in `s_lane_after_cover_stage`, and
    `s_lane_dispatch_stage = al1_cover_enabled() ? -1 : s_lane_after_cover_stage;`.
  - `nr_pdcch_blind_lookahead_get()`: `out->al1_only = ln->al1_only;` added.
- `nr_pdcch_blind_monitor_rt.c`:
  - `#include "nr_pdcch_al1_map.h"` added next to `nr_pdcch_joint_live.h`.
  - In the lane loop, right after `uint8_t ln_al_active = ln_als[...]` (confirmed `geom` in scope):
    `if (geom.al1_only) ln_al_active = 1;` forces AL1-only scanning for cover-lap lanes.
  - New `static void al1_verify_report(...)` added directly above the
    `nr_pdcch_blind_monitor_process_body` forward declaration (the enclosing function containing the
    verification call site), computing the AL1 REG set of the verified CCE, narrowing the full
    mapping catalogue against it, and logging
    `SENSING: AL1_VERIFY rnti=... cce=... mapping=B/I/S consistent_mappings=N distinct_al1_families=F
    [(AL1 coverage partial)]`.
  - At the verification site: `nr_pdcch_lookahead_geom_t vg; const bool have_vg =
    nr_pdcch_blind_lookahead_get(lane, &vg);` fetched before `nr_pdcch_blind_lookahead_observe()`
    consumes the lane; on `just_verified`, calls `al1_verify_report(&vg, ...)` when
    `have_vg && vg.al1_only && cand_task[ti].L == 1`.
- `tests/nr_pdcch_al1_map_test.cc`: added `Al1Map.CoverSurvivesTheLaneTypeRoundTrip` verbatim from the
  brief (Step 1). Passes.

## Rulings applied

- The brief's `map_env` staging variable name matched the real code exactly (`ISAC_MAP_PASS0_ONLY` →
  `map_env` → `s_lane_dispatch_stage`); no renaming/adaptation was needed.
- **Default path unchanged by construction**: `al1_cover_enabled()` reads `ISAC_AL1_COVER`; when unset
  it returns `0`, so `lookahead_lanes_init()` sets `s_lane_dispatch_stage = s_lane_after_cover_stage`
  (0 or 1, exactly the old `map_env`-derived value) — identical to the pre-Task-5 assignment. Every new
  branch (`lane_map_count`'s cover early-return, `lane_catalog_map_max`'s cover early-return,
  `lane_assign_next`'s cover-exhausted reset, `ln->al1_only`/`geom.al1_only` gating in rt.c) is guarded
  on `s_lane_dispatch_stage < 0`, a value the code can only reach via `al1_cover_enabled()`. With the
  knob unset that condition is never true, so no new code path executes and `al1_only` is always
  `false`.

## Test summary

- `test_nr_pdcch_al1_map`: **8/8 PASS** (7 pre-existing + 1 new `CoverSurvivesTheLaneTypeRoundTrip`),
  both after the `kset_find` removal and again after the Task 5 wiring.
- `nr-uesoftmodem`: builds and links clean (`Built target nr-uesoftmodem`), both from a from-scratch
  lane build and after a forced targeted recompile of the two touched files. No errors; no new
  warnings attributable to this change (only two pre-existing, unrelated warnings remain:
  `n_verified` unused variable at rt.c:2768, and a zero-length printf format at rt.c:3625 — both
  predate this task and are far from the touched code).
- `test_nr_pdcch_blind_monitor` / `test_nr_dl_adaptive`: **Not Run** — see Concern below.

## Concern (does not block the commit)

`ctest -R 'test_nr_pdcch_blind_monitor|test_nr_dl_adaptive|test_nr_pdcch_al1_map'` cannot fully pass:
`test_nr_pdcch_blind_monitor` (the binary both tests share) fails to **link**, independent of Task 5:

```
libnr_pdcch_blind_monitor.a(nr_pdcch_blind_monitor.c.o): in function `nr_pdcch_blind_monitor_discovered_poll':
nr_pdcch_blind_monitor.c:831: undefined reference to `nr_pdcch_blind_monitor_bank_has_geometry'
libnr_pdcch_blind_monitor.a(nr_passive_acq_state.c.o): in function `nr_passive_acq_note_sib1_tdd':
nr_passive_acq_state.c:205: undefined reference to `nr_tdd_config_init'
libnr_pdcch_blind_monitor.a(nr_passive_acq_state.c.o): in function `nr_passive_acq_tdd_slot_has_downlink':
nr_passive_acq_state.c:219: undefined reference to `nr_tdd_slot_has_downlink'
collect2: error: ld returned 1 exit status
```

Root cause: `nr_pdcch_blind_monitor_bank_has_geometry` is defined in `nr_pdcch_blind_monitor_rt.c`, and
`nr_tdd_config_init`/`nr_tdd_slot_has_downlink` in `nr_tdd_pattern.c` — neither file is in the
`nr_pdcch_blind_monitor` CMake library's source list (`CMakeLists.txt:1464`), yet
`nr_pdcch_blind_monitor.c`/`nr_passive_acq_state.c` (both already in that library) call them. This is a
CMakeLists.txt source-list gap, not something in Task 5's Modify list.

**Verified pre-existing, not caused by this task**: I `git stash`ed all of Task 5's changes (back to
`e4ef4dd55d`, the Task-4 + kset_find-cleanup commit) and rebuilt `test_nr_pdcch_blind_monitor` —
identical failure, identical three undefined symbols, identical line numbers. Then restored the stash
and committed as planned.

**Indirect evidence Task 5's rt.c/`.c`/`.h` wiring itself is correct**: `nr-uesoftmodem` links the full
program successfully, which pulls in `nr_pdcch_blind_monitor_rt.c` via `PHY_NR_UE` and satisfies every
symbol `nr_pdcch_blind_monitor.c`/`nr_pdcch_blind_monitor_rt.c` reference, including the new
`al1_verify_report()`/`nr_pdcch_al1_*` calls — so the only thing unverified live is the isolated
unit-test binary, not the real executable.

Not fixed here: out of Task 5's file scope (CMakeLists.txt isn't listed as a file to modify), and the
lane rules say not to touch files outside the task or other lanes' concerns. Flagging for whichever
task/lane owns `CMakeLists.txt`'s `nr_pdcch_blind_monitor`/`test_nr_pdcch_blind_monitor` target
definitions.
