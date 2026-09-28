# Task 11 report: Apply interleaved VRB-to-PRB mapping

## Status: DONE

Resumed after sens6 rebooted and SSH connectivity returned. Both Step 0 (controller ruling R23)
and Task 11 are implemented, built clean, tested, and committed on `sdd/agn-prb`.

## Commits (on sens6, `/home/sens/NICOLA/agn-wt/prb`, branch `sdd/agn-prb`)
1. `b8787a2b6d` — "Blind PDCCH: allocate the DCI layout resolvers on first arm" (Step 0 / R23).
2. `ce50b211cb` — "Passive PDSCH: apply interleaved VRB-to-PRB mapping (1_0 fixed L=2, 1_1 L swept
   by TB CRC)" (Task 11).

## Step 0 (R23): resolver heap allocation
`g_dci11_resolver` and `g_dci01_resolver` in `nr_pdcch_blind_monitor_rt.c` (both `nr_dci11_resolver_t`,
~17.2 MB each at `NR_DCI11_LAYOUT_MAX=32768`) were plain static globals — 34 MB of BSS locked by
`mlockall(MCL_CURRENT)` at startup regardless of whether either resolver is ever armed. Changed both
to pointers, calloc'd once on first arm via a shared `dci11_resolver_ensure(nr_dci11_resolver_t **slot,
const char *tag)` helper (mirrors Task 14's `sweep_context_t.state` heap-on-first-use pattern), fail
closed (LOG_E + resolver stays permanently unavailable) on a calloc failure.
- Every read/write site besides the two arm points was already reached only once armed
  (`g_dci11_state`/`g_dci01_state == 1`, or — for `g_dci01_resolver` — the existing
  `nr_dci01_fdra_verdict()` call already tolerates a NULL `r` by design, since it's called with
  literal `NULL` at one call site pre-arm).
- The one exception, `nr_pdcch_dci11_layout_feedback()` (external entry point, called from both the
  deferred consumer thread and the in-line decode path, with no state gate of its own), now checks
  the pointer explicitly and no-ops when NULL — matching the old all-zero static struct's inert
  pre-arm behaviour (in practice a real, non-sentinel `layout_index` cannot reach it before arm,
  since it only originates once `dl_auto` is set, itself gated on `g_dci11_state==1`).
- No behaviour change otherwise.

## Task 11: interleaved VRB-to-PRB mapping
**Files touched**: `nr_pdcch_blind_monitor_rt.c`, `nr_pdsch_passive_decode.{h,c}`,
`openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_prb_set_test.cc` (the brief's path
`tests/nr_pdsch_prb_set_test.cc` doesn't exist; ruled to use the real path).

- **`nr_pdcch_blind_monitor_rt.c`** (grant → PRB list): added an `else if (out.vrb_to_prb)` branch
  right after the existing RA-type-0 block (RA type 1 only; type 0 is never interleaved, matching
  the ruling). Mirrors the type-0 pattern exactly: builds the PRB list into `freq_alloc.prb_list`
  via `nr_vrb_to_prb_interleaved()` (Task 8, unchanged) and normalises it with
  `nr_pdsch_passive_alloc_normalise()` (Task 9's path) — so every downstream consumer (fast enqueue,
  deferred, in-line decode, data-aided tap) sees the same data-ordered list the RA-type-0 case
  already produces.
  - **DCI 1_0 in a common search space**: `bwp_start=0` passed as `nr_vrb_to_prb_interleaved`'s
    alignment parameter, `bwp_size = dlsch_pdu.BWPSize` (already the initial DL BWP / CORESET#0 size
    for this case), `L=2` fixed. Confirmed from the existing SIB1/1_0 decode path
    (memory note "Passive PDSCH origin must match RIV reference"): `rb_origin` (`dci10_rb_base`)
    already carries CORESET#0's own CRB offset *separately* and adds it back downstream
    (`base_sc = rb_origin + ...`), matching TS 38.211 7.3.1.6's own footnote that sets
    `N_start_BWP=0` for exactly this case — using `rb_origin` itself here would double-count the
    offset.
  - **DCI 1_0 elsewhere / DCI 1_1**: `bwp_start = rb_origin` (already resolves to the active BWP's
    own CRB in both cases via the existing `is_dci10 ? dci10_rb_base : (bwp_entry>0 ? pbwp start :
    cfg->bwp_start)` logic).
  - **DCI 1_1's L** comes from `nr_pdsch_vrbl_pick(out.rnti)` (2 or 4); the picked value is stashed
    in a new local `vrb_l_used` and copied into both grant-construction literals (`grant_q` at the
    fast-enqueue site and `grant` at the chest-computed site) via a new `.vrb_l` field, so the
    deferred queue path (which copies the whole grant struct wholesale) and the in-line path both
    carry it with no further changes needed to `nr_pdsch_passive_queue.c`.
  - An invalid interleave (`nr_vrb_to_prb_interleaved` returns 0, or normalise fails) drops the
    grant via `grantdrop(..., "vrb-il-prb-list-invalid")`, same shape as the type-0 failure path.

- **`nr_pdsch_passive_decode.h`**: added `uint8_t vrb_l` to `nr_pdsch_passive_grant_t` (0 = not
  interleaved or DCI 1_0's fixed L — previous behaviour for every existing caller that never sets
  it) and declared the exported `int nr_pdsch_vrbl_pick(uint16_t rnti)`.

- **`nr_pdsch_passive_decode.c`** (per-RNTI bundle-size hypothesis, next to the PT-RS sweep in
  `rnti_dec_t`): **ruling on concern 2 applied — did NOT use the generic `nr_hyp_sweep`.** Its only
  two in-tree users (`nr_pdcch_ul_discovery.c`) carry their own sizing comment: `nr_hyp_sweep_state_t`
  is ~1.16 MB per instance. At `RNTI_DEC_MAX=16` an inline field of that type would add ~18.6 MB of
  exactly the always-resident, `mlockall()`-locked static state Step 0 exists to remove elsewhere on
  this same file. Instead added a minimal, self-contained `nr_vrbl_sweep_t` — `uint32_t tr[2], ok[2]`
  plus an `int latched` (arm 0 = L2, arm 1 = L4) — as a new field on `rnti_dec_t`, using the *same*
  Wilson-interval pick/latch algorithm already proven in the neighbouring `nr_ptrs_sweep_t`
  (`nr_pdsch_ptrs_unav.c`), sized down from its 7 arms to 2 rather than duplicating a smaller,
  untested ad-hoc rule. New `rnti_vrbl_pick()`/`rnti_vrbl_feed()` wrappers mirror `rnti_ptrs_pick()`/
  `rnti_ptrs_feed()` exactly (same `g_ptrs_lock`, since it already guards the whole `g_rnti_dec[]`
  table, not just the PT-RS field). The exported `nr_pdsch_vrbl_pick()` translates the internal arm
  index to the caller-facing L value (2 or 4).
  - Feedback happens *inside* `nr_pdsch_passive_decode()` itself, right next to the existing PT-RS
    feed call (same location, same trigger — the just-computed `ldpc_ok`), reading the grant's
    `vrb_l` field: `if (grant->vrb_l == 2 || grant->vrb_l == 4) { ... rnti_vrbl_feed(...) ...}`. This
    needed no changes to the two external `nr_pdcch_dci11_layout_feedback()` call sites (rt.c and
    the deferred consumer in `nr_pdsch_passive_queue.c`) — that function is for the unrelated DCI
    1_1 *field-layout* sweep, not this one.
  - Logs `SENSING: VRB_IL rnti=0x%x L=%u latched` once, on the transition to latched — the exact
    format the brief specifies.

## Test summary
`ctest -R 'test_nr_pdsch|test_nr_hyp_sweep'` from the lane build dir: **5/5 pass**
(`test_nr_hyp_sweep`, `test_nr_pdsch_xoverhead`, `test_nr_pdsch_config_sweep`,
`test_nr_pdsch_prb_set` — including the new `InterleavedCssUsesCoreset0Grid` case — and
`test_nr_pdsch_ptrs_unav`). New test asserts `nr_vrb_to_prb_interleaved(0, 48, 2, 0, 48, p)` is a
permutation and that VRBs {2,3} land on PRBs {24,25} (bundle 1 → PRB bundle C=12), matching the
brief's Step 2 exactly — passed unmodified since `nr_vrb_to_prb_interleaved` (Task 8) needed no
change. Full `nr-uesoftmodem` build (lane `prb`) is clean for both commits; only pre-existing,
unrelated warnings appear (`n_verified` unused, a zero-length-format warning, and a
`dmrs_first`-maybe-uninitialized note in an unrelated function).

`nr_hyp_sweep_test.cc` was not touched, per the brief's "only if the generic sweep needs a change" —
it doesn't, since the generic sweep isn't used here at all (see the ruling above).

## Concerns
1. None of the five ctest executables in the `test_nr_pdsch*`/`test_nr_hyp_sweep` family link
   `nr_pdsch_passive_decode.c` (it's not unit-test-friendly standalone — too many OAI dependencies),
   so the new `nr_vrbl_sweep_t` pick/feed logic and the `VRB_IL` log line are exercised only by the
   full `nr-uesoftmodem` build succeeding, not by an automated test. Risk is low: the algorithm is a
   direct, size-reduced copy of the already-proven `nr_ptrs_sweep_t` Wilson-interval logic sitting
   right next to it in the same file, not new/uninspected DSP.
2. Per instruction, did NOT run `nr-uesoftmodem` or touch the X410 — verification is build + ctest
   only, as directed.
3. Neither commit was pushed, per standing rules.

## Report path
`/home/sens/NICOLA/docs/superpowers/sdd/full-running-agnosticity/task-11-report.md` (this file).
