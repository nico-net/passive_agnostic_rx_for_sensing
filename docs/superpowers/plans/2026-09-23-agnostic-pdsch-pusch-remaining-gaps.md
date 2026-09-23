# Agnostic PDSCH/PUSCH Data-Decode Remaining Gaps — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the confirmed remaining gaps that stand between "dedicated PDCCH decodes blind" (done, validated 2026-09-22/23) and "dedicated PDSCH/PUSCH transport-block data decodes blind" on the lab cell, X410 unavailable during this work — every task must build and be validated with **no live radio**, using synthetic/offline tests only. Live-hardware validation is a separate, explicitly gated final task.

**Architecture:** This session already discovered — by reading the code rather than assuming — that most of what looked "missing" from an earlier planning pass already exists in `openair1/PHY/NR_UE_TRANSPORT/`: `nr_pdsch_config_sweep.c` (Technique D) already sweeps TDA/DM-RS-position/DM-RS-length/MCS-table on TB CRC with cell-wide priors, wired into `nr_pdcch_blind_monitor_rt.c`; `nr_hyp_sweep.c` is a generic, reusable four-stage bounded hypothesis-sweep primitive; LBRM `n_L` is already a debugged per-RNTI lookup (`rnti_nl_get`), not a naive assumption. **This plan therefore does NOT re-implement any of that.** It covers only what a careful audit in this session did not find already built: (1) a real, measured LDPC decode cost number for this cell's actual transport block size, (2) a resolved understanding of dedicated-BWP-size handling (audit first — this exact "assumed missing, actually already exists" mistake has already happened twice in this session for other fields, so guessing a third time is not acceptable), (3) any BWP-size hypothesis-search code the audit finds is genuinely missing, built on `nr_hyp_sweep`, and (4) a from-scratch GF(2) algebraic n_RNTI-recovery module (5GDescrambler-style) for cells where `n_RNTI = C-RNTI`, which is confirmed absent anywhere in this tree.

**Tech Stack:** C (OAI codebase conventions: `LOG_*` macros, `NR_`/`nr_` prefixes, existing struct patterns), GoogleTest (`.cc` test files under `openair1/PHY/NR_UE_TRANSPORT/tests/`, registered in the root `CMakeLists.txt`'s `ENABLE_TESTS` block), Python (throwaway proof-of-concept script for the GF(2) linear-algebra design, not shipped).

**Spec:** No separate spec document exists for this work; the requirements are captured in this plan's Background section above and were established through direct conversation and code reading in the session dated 2026-09-23 (see the repo's `CLAUDE.md` for the project's broader architecture and coding conventions, which this plan follows).

## Global Constraints

- No task in this plan may require the X410 (or any live radio) to build or pass its own tests. Every test is either a GoogleTest unit test against synthetic/constructed data, or a standalone tool run (`ldpctest`) that needs no radio.
- Match the surrounding file's existing brace style, naming (`nr_`/`NR_` prefixes), and logging macros (`LOG_I`/`LOG_W`/`LOG_E`/`LOG_A` with the `PHY` tag) — do not reformat existing code.
- New source/header files go in `openair1/PHY/NR_UE_TRANSPORT/`, matching every existing module this plan touches.
- New test files go in `openair1/PHY/NR_UE_TRANSPORT/tests/`, registered in the root `CMakeLists.txt` inside the existing `if(ENABLE_TESTS)` block (see Task 1's Step for the exact insertion pattern, copied from the real `test_nr_hyp_sweep`/`test_nr_pdsch_xoverhead` entries already in that file).
- Every non-trivial function gets one runnable check (GoogleTest case), per this project's own standing convention — no exceptions for "it's obviously right."
- Do not touch `./gnb_remote_logs/` or `./cuLogs/` (read-only mirrors, per this repo's CLAUDE.md).
- Never assume a field is unimplemented from a partial grep — this exact mistake (assuming MCS-table and DM-RS-position sweeping were missing, when `nr_pdsch_config_sweep.c` already implements both) was made and corrected twice in the session that produced this plan. Task 3 exists specifically to not repeat it a third time for BWP size.

## Review Focus

- **A `ldpctest` run at this cell's real TBS (159749-bit LBRM size, per the live gNB log captured in-session) takes long enough to make Task 1 itself slow** — the tool's own default case already takes several seconds per configuration; budget for it, don't assume it is instant.
- **The GF(2) module (Task 4) must never be wired into any live decode path in this plan** — it is a synthetic-only proof-of-concept and unit-tested module in this plan; wiring it into `nr_pdcch_blind_monitor_rt.c`'s live RNTI-hypothesis path is explicitly out of scope here (it would need live validation this plan cannot do without the X410) and must not be attempted as a "bonus."
- **Task 3's audit must produce a written decision even if the answer is "nothing to build"** — an audit that quietly fizzles into silence is worse than one that concludes "already handled by X, no new code needed" on the record.
- **`nr_hyp_t`'s raw-byte encoding (`NR_HYP_BYTES = 96`) must be used correctly** — a hypothesis is an opaque byte blob compared by the caller-supplied `equivalent`/`plausible` callbacks, not a typed struct; Task 3's BWP-size hypothesis (if built) must encode/decode through `memcpy`, exactly like the existing `nr_hyp_sweep_test.cc` does for its `int` example, not invent a different convention.
- **The GF(2) matrix construction must be validated against brute force on a SMALL synthetic case before any C is written** — this is a genuinely novel, mathematically subtle component (unlike the other three tasks, which port an already-proven pattern), and writing confident-looking C for it without independent verification would be worse than not building it at all.

---

## Task 1: Measure real LDPC decode cost at this cell's actual transport-block size

**Files:**
- No new files. Runs the existing standalone tool `cmake_targets/ran_build/build/ldpctest` (built from `openair1/PHY/CODING/TESTBENCH/ldpctest.c`, already compiled and present on `sens6`).
- Create: `docs/superpowers/plans/2026-09-23-ldpc-cost-measurement.md` (the recorded result).

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a real, measured `ldpc_decoder` microseconds-per-decode number at this cell's `BG`/`Zc`/`Kprime`/rate, which Task 5 (the final, hardware-gated task) uses to sanity-check whether any future multi-hypothesis PDSCH sweep can run inside the existing consumer-thread budget. No other task in this plan consumes this output directly.

- [ ] **Step 1: Derive this cell's real LDPC parameters from the already-captured gNB log line**

The dedicated grant captured live on 2026-09-23 (`rnti=0x4614`, DCI 1_1) logged `tb_size_lbrm=159749 ldpc_base_graph=2`. Base graph 2 (`BG=1` in `ldpctest`'s own 0-indexed `--BG` flag, since OAI's `ldpc_base_graph` field is 1 or 2 and `ldpctest --BG` takes 0 or 1 — confirm this mapping by reading the flag's help text before running, since getting BG backwards silently benchmarks the wrong graph):

```bash
ssh sens6 "/home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build/ldpctest --help 2>&1 | grep -A2 -- '--BG\|--ZC\|--kprime\|--rate\|--n_segments'"
```

Read the flag names this prints (they may differ slightly by version — do not assume `--BG`/`--ZC`/`--kprime` are exactly right until confirmed by this `--help` output) and note them for Step 2.

- [ ] **Step 2: Run `ldpctest` at the real cell's parameters, several times, and record the decoder timing**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && \
  for i in 1 2 3 4 5; do ./ldpctest --BG 1 --ZC 384 --kprime 8448 -n 1 2>&1 | grep 'ldpc_decoder:'; done"
```

(`--ZC 384 --kprime 8448` matches the `Kprime_8448` case already present in this build directory's leftover `ldpctest_BG_0_Zc_0_rate_..._Kprime_8448_*.txt` files from a prior run — reuse that Kprime value since it is already known to be a valid configuration on this build; adjust `--BG`/`--ZC` per what Step 1's `--help` output says, not by guessing.)

- [ ] **Step 3: Record the result**

Write `docs/superpowers/plans/2026-09-23-ldpc-cost-measurement.md` containing: the exact command run, the 5 raw `ldpc_decoder:` timing lines, their mean, and one sentence stating whether this number is compatible with running 2-5 full LDPC decode attempts per grant (per this session's earlier discussion of a bounded, arithmetically-pre-filtered hypothesis search) within the existing PDSCH consumer-thread pipeline described in `nr_pdcch_blind_monitor.c`'s `pdcch_blind_monitor_pdsch` doc string (6 consumers, per the conf already in `/home/sens/NICOLA/captures/cons6_ota_q64.conf`).

- [ ] **Step 4: Commit**

```bash
cd /home/sens/NICOLA && git add docs/superpowers/plans/2026-09-23-ldpc-cost-measurement.md && \
  git commit -m "docs: record measured LDPC decode cost at this cell's real TBS parameters

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

(If `/home/sens/NICOLA` is not a git repository, `git init` it first and confirm with the user before the first commit — do not commit to a repository that was not already git-managed without asking.)

---

## Task 2: Audit dedicated-BWP-size handling before writing any new code for it

**Files:**
- Read only: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (the `pbwp_snap`/`pbwp_on`/`pbwp_n` mechanism around lines 4258 and 4367, and the `g_cfg.bwp_size = n_rb_carrier;` assignment around line 1758 of `nr_pdcch_blind_monitor.c`), `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c` (the full 910-line implementation, not just the header read so far this session), and the "Passive BWP tracking" memory file this session's own memory system already recorded (`~/.claude/projects/-home-sens-NICOLA/memory/passive-bwp-tracking-and-phy-test-bed.md`, referenced by name in this project's `MEMORY.md` index).
- Create: `docs/superpowers/plans/2026-09-23-bwp-size-audit.md` (the decision record).

**Interfaces:**
- Consumes: nothing.
- Produces: a written decision that Task 3 depends on directly — Task 3 must not start until this file exists and states one of two outcomes explicitly (see Step 4).

- [ ] **Step 1: Read the full `pbwp_snap` mechanism**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/openair1/PHY/NR_UE_TRANSPORT && \
  grep -n 'pbwp_snap\|pbwp_on\|pbwp_n\b\|pbwp_probe' nr_pdcch_blind_monitor_rt.c | head -60"
```

For each hit, read 15 lines of surrounding context (`sed -n '<line-10>,<line+15>p' nr_pdcch_blind_monitor_rt.c`) to determine: does this mechanism discover an UNKNOWN dedicated BWP size by hypothesis, or does it only track ADDITIONAL, separately-signaled BWPs whose size is assumed known some other way? Write the answer down before moving to Step 2.

- [ ] **Step 2: Read the memory file on passive BWP tracking**

```bash
cat /home/sens/.claude/projects/-home-sens-NICOLA/memory/passive-bwp-tracking-and-phy-test-bed.md
```

Its one-line index summary is `"length = K+riv(N)+d, collisions undiscoverable, OAI DM-RS ref = BWP star…"` — read the full file, not just this summary, since the summary is truncated. Note whether it describes a solved problem, an open one, or something orthogonal to "the DECODE-time BWP size used for RIV interpretation is hardcoded to the full carrier."

- [ ] **Step 3: Read `nr_pdsch_config_sweep.c`'s implementation, not just its header**

The header (already read this session) documents Techniques A-D's scope as "recovers interpretation, not geometry... assumes Techniques A-C have already converged" and explicitly excludes `dci_length`/`bwp_size` from its own swept hypothesis fields ("anything the polar CRC already pins... is not swept here"). Confirm this by reading the actual 910-line `.c` file's `nr_pdsch_config_sweep_init_legal`/`nr_pdsch_config_sweep_select` bodies:

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/openair1/PHY/NR_UE_TRANSPORT && grep -n 'BWPSize\|bwp_size' nr_pdsch_config_sweep.c"
```

If this returns nothing, it confirms BWP size genuinely sits outside Technique D's scope, as the header claims. If it returns something, read that context before concluding anything.

- [ ] **Step 4: Write the decision record**

Write `docs/superpowers/plans/2026-09-23-bwp-size-audit.md` with exactly one of these two conclusions, stated in its first paragraph:

- **Conclusion A — genuinely missing:** "`pbwp_snap` handles [state precisely what it actually handles, from Step 1]; it does not resolve an unknown MAIN dedicated BWP size used for RIV interpretation. `nr_pdsch_config_sweep.c` confirms this is out of its scope by design. No existing mechanism sweeps or resolves a dedicated BWP size different from the assumed full-carrier value. Task 3 should build a `nr_hyp_sweep`-based BWP-size hypothesis module." — if this is the conclusion, list the exact call site (file:line) where the new module's `nr_hyp_sweep_next()`-selected value must be substituted for `cfg->bwp_size`, so Task 3 does not have to re-discover it.
- **Conclusion B — already handled:** "[state exactly what already handles it, and how]. Task 3 is cancelled; go directly to Task 4." — if this is the conclusion, do not write Task 3's code; skip to Task 4 and note the cancellation in that task's own PR description.

- [ ] **Step 5: Commit**

```bash
cd /home/sens/NICOLA && git add docs/superpowers/plans/2026-09-23-bwp-size-audit.md && \
  git commit -m "docs: audit dedicated-BWP-size handling before building new hypothesis-search code

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 3: BWP-size hypothesis module (ONLY if Task 2 concluded "genuinely missing")

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc`
- Modify: `CMakeLists.txt` (register the new test target, mirroring the existing `test_nr_hyp_sweep` entry read in this session at approximately line 2461)
- Modify: the exact call site Task 2's Step 4 recorded (substituting the hypothesis module's current best BWP size for the hardcoded `cfg->bwp_size` / full-carrier value)

**Interfaces:**
- Consumes: `nr_hyp_sweep_init`, `nr_hyp_sweep_next`, `nr_hyp_sweep_feed`, `nr_hyp_sweep_winner`, and the `nr_hyp_t`/`nr_hyp_sweep_state_t` types from `nr_hyp_sweep.h` (already read this session, signatures below), and TB-CRC-pass/fail feedback from wherever Task 2 identified the decode outcome is known.
- Produces: `int nr_pdsch_bwp_sweep_current(uint16_t *bwp_size_out)` (returns 1 and fills `*bwp_size_out` with the current best hypothesis whether or not it has converged yet, 0 if the module has not observed enough to have any candidate), for Task 5's wiring step to call at the recorded substitution site.

Exact `nr_hyp_sweep.h` signatures this task builds on (already confirmed by reading the file this session):
```c
typedef struct { uint8_t bytes[NR_HYP_BYTES]; int len; } nr_hyp_t;   // NR_HYP_BYTES = 96
typedef bool (*nr_hyp_constraint_fn)(const nr_hyp_t *, void *);
typedef bool (*nr_hyp_equivalent_fn)(const nr_hyp_t *, const nr_hyp_t *, const void *, void *);
typedef bool (*nr_hyp_plausible_fn)(const nr_hyp_t *, const void *, void *);
int nr_hyp_sweep_init(nr_hyp_sweep_state_t *, const nr_hyp_t *, int,
                     nr_hyp_constraint_fn, void *, nr_hyp_equivalent_fn,
                     const void *const *, int, void *);
int nr_hyp_sweep_next(nr_hyp_sweep_state_t *, const void *, nr_hyp_plausible_fn, void *, nr_hyp_t *);
int nr_hyp_sweep_feed(nr_hyp_sweep_state_t *, int, bool);
int nr_hyp_sweep_winner(const nr_hyp_sweep_state_t *);
```

- [ ] **Step 1: Write the failing test — legal BWP-size catalog for band n78**

```cpp
// openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc
#include <cstring>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_bwp_sweep.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *,const char *,int,const char *,int) { std::abort(); }
}

TEST(BwpSweep, CatalogContainsTheKnownLabCellSize) {
  nr_pdsch_bwp_sweep_state_t st;
  ASSERT_GT(nr_pdsch_bwp_sweep_init(&st, /*carrier_prb=*/273), 0);
  bool found_273 = false;
  for (int i = 0; i < st.hyp.n_classes; i++) {
    uint16_t v;
    memcpy(&v, st.hyp.classes[i].hyp.bytes, sizeof(v));
    if (v == 273) found_273 = true;
  }
  EXPECT_TRUE(found_273) << "the catalog must include the full-carrier size as one legal hypothesis, "
                            "since that is this session's own already-confirmed-correct value on the lab cell";
}
```

- [ ] **Step 2: Run it to confirm it fails to compile (the header does not exist yet)**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && \
  g++ -c -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc -o /tmp/t.o 2>&1 | head -5"
```

Expected: `fatal error: nr_pdsch_bwp_sweep.h: No such file or directory`.

- [ ] **Step 3: Write the header**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.h
#ifndef NR_PDSCH_BWP_SWEEP_H
#define NR_PDSCH_BWP_SWEEP_H
#include <stdbool.h>
#include <stdint.h>
#include "nr_hyp_sweep.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Dedicated BWP size (PRBs) used to interpret the RIV frequency-domain field of a DCI 1_1/0_1 grant.
 * The RRC-configured dedicated BWP is ciphered and unavailable to a passive receiver; this receiver
 * currently assumes it equals the full carrier width, which is correct on the lab cell (confirmed
 * 2026-09-23: bwp=[0..273) matches the gNB's own log for a real dedicated grant) but is a documented,
 * deliberate approximation, not a derivation -- see nr_pdcch_blind_monitor.c's g_cfg.bwp_size
 * assignment comment. This module resolves it by the SAME TB-CRC hypothesis-and-confirm method already
 * proven for TDA/DM-RS-position/MCS-table in nr_pdsch_config_sweep.c, using the generic nr_hyp_sweep
 * primitive: candidate = one of the legal common BWP sizes at this numerology (TS 38.101-1 Table
 * 5.3.5-1), a grant decoded and TB-CRC-checked under a wrong size almost never passes, so CRC alone
 * discriminates. This is a CELL-WIDE property once confirmed, same convention as the cell-wide MCS
 * table/TDA prior already implemented elsewhere in this codebase. */
typedef struct {
  nr_hyp_sweep_state_t hyp;
  uint16_t carrier_prb;
} nr_pdsch_bwp_sweep_state_t;

/* Builds the legal catalog bounded by carrier_prb (every standard BWP size <= carrier_prb, plus
 * carrier_prb itself). Returns the number of candidates, or a negative nr_hyp_sweep error code. */
int nr_pdsch_bwp_sweep_init(nr_pdsch_bwp_sweep_state_t *st, uint16_t carrier_prb);

/* Next candidate to try. Returns the index (feed this back to _feed), fills *out_prb. */
int nr_pdsch_bwp_sweep_next(nr_pdsch_bwp_sweep_state_t *st, uint16_t *out_prb);

/* Record the TB-CRC outcome for the candidate index _next returned. Returns the winning PRB count
 * once one is established, else 0 (not yet converged -- caller keeps using the full-carrier default
 * until this returns non-zero, so a not-yet-converged module changes nothing). */
uint16_t nr_pdsch_bwp_sweep_feed(nr_pdsch_bwp_sweep_state_t *st, int idx, bool tb_crc_ok);

/* Current best value whether or not converged: the winner if one exists, else the full-carrier
 * default. Always returns 1 and fills *bwp_size_out; there is no "no answer" case by design, so the
 * caller's substitution site (Task 5) never needs a fallback branch of its own. */
int nr_pdsch_bwp_sweep_current(const nr_pdsch_bwp_sweep_state_t *st, uint16_t *bwp_size_out);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 4: Write the implementation**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.c
#include <string.h>
#include "nr_pdsch_bwp_sweep.h"

/* TS 38.101-1 Table 5.3.5-1, FR1 common PRB counts across 15/30/60 kHz SCS: the union of every
 * standard channel-bandwidth PRB count, since the dedicated BWP is not required to equal the
 * carrier's own bandwidth class. Kept as one static array rather than a per-numerology table: the
 * catalog is filtered to <= carrier_prb in _init, so entries larger than the carrier are harmless
 * and never selected. */
static const uint16_t nr_bwp_common_prb[] = {11, 18, 24, 25, 31, 32, 38, 51, 52, 65, 66, 78, 79, 106,
                                             107, 133, 135, 160, 162, 189, 192, 216, 217, 245, 248, 264, 273};
#define NR_BWP_COMMON_N (int)(sizeof(nr_bwp_common_prb) / sizeof(nr_bwp_common_prb[0]))

static uint16_t bwp_value(const nr_hyp_t *h)
{
  uint16_t v;
  memcpy(&v, h->bytes, sizeof(v));
  return v;
}

static bool bwp_equivalent(const nr_hyp_t *a, const nr_hyp_t *b, const void *sample, void *ctx)
{
  (void)sample;
  (void)ctx;
  return bwp_value(a) == bwp_value(b);
}

int nr_pdsch_bwp_sweep_init(nr_pdsch_bwp_sweep_state_t *st, uint16_t carrier_prb)
{
  st->carrier_prb = carrier_prb;
  nr_hyp_t raw[NR_BWP_COMMON_N];
  int n = 0;
  for (int i = 0; i < NR_BWP_COMMON_N; i++) {
    if (nr_bwp_common_prb[i] > carrier_prb)
      continue;
    raw[n].len = sizeof(uint16_t);
    memcpy(raw[n].bytes, &nr_bwp_common_prb[i], sizeof(uint16_t));
    n++;
  }
  /* The full-carrier value must always be a candidate even if it is not in the standard table for
   * some numerology this table omits (matches this session's own confirmed-correct lab-cell value). */
  bool have_carrier = false;
  for (int i = 0; i < n; i++)
    have_carrier |= bwp_value(&raw[i]) == carrier_prb;
  if (!have_carrier && n < NR_BWP_COMMON_N) {
    raw[n].len = sizeof(uint16_t);
    memcpy(raw[n].bytes, &carrier_prb, sizeof(uint16_t));
    n++;
  }
  return nr_hyp_sweep_init(&st->hyp, raw, n, NULL, NULL, bwp_equivalent, NULL, 0, NULL);
}

int nr_pdsch_bwp_sweep_next(nr_pdsch_bwp_sweep_state_t *st, uint16_t *out_prb)
{
  nr_hyp_t h;
  const int idx = nr_hyp_sweep_next(&st->hyp, NULL, NULL, NULL, &h);
  if (idx < 0)
    return idx;
  *out_prb = bwp_value(&h);
  return idx;
}

uint16_t nr_pdsch_bwp_sweep_feed(nr_pdsch_bwp_sweep_state_t *st, int idx, bool tb_crc_ok)
{
  const int w = nr_hyp_sweep_feed(&st->hyp, idx, tb_crc_ok);
  if (w < 0)
    return 0;
  return bwp_value(&st->hyp.classes[w].hyp);
}

int nr_pdsch_bwp_sweep_current(const nr_pdsch_bwp_sweep_state_t *st, uint16_t *bwp_size_out)
{
  const int w = nr_hyp_sweep_winner(&st->hyp);
  *bwp_size_out = (w >= 0) ? bwp_value(&st->hyp.classes[w].hyp) : st->carrier_prb;
  return 1;
}
```

- [ ] **Step 5: Run the test from Step 1 to confirm it now passes**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && \
  g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT \
    openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc \
    openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.c \
    openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c \
    -lgtest -lgtest_main -lpthread -o /tmp/test_bwp && /tmp/test_bwp"
```

Expected: `[  PASSED  ] 1 test.`

- [ ] **Step 6: Write the second test — convergence on a synthetic wrong-then-right sequence**

```cpp
// append to nr_pdsch_bwp_sweep_test.cc
TEST(BwpSweep, ConvergesWhenOneCandidateAlwaysPassesAndOthersAlwaysFail) {
  nr_pdsch_bwp_sweep_state_t st;
  ASSERT_GT(nr_pdsch_bwp_sweep_init(&st, 273), 0);
  uint16_t truth = 106;
  uint16_t winner = 0;
  for (int trial = 0; trial < 2000 && !winner; trial++) {
    uint16_t candidate;
    const int idx = nr_pdsch_bwp_sweep_next(&st, &candidate);
    ASSERT_GE(idx, 0);
    winner = nr_pdsch_bwp_sweep_feed(&st, idx, candidate == truth);
  }
  EXPECT_EQ(winner, truth);
  uint16_t current;
  ASSERT_EQ(nr_pdsch_bwp_sweep_current(&st, &current), 1);
  EXPECT_EQ(current, truth);
}
TEST(BwpSweep, DefaultsToFullCarrierBeforeConvergence) {
  nr_pdsch_bwp_sweep_state_t st;
  ASSERT_GT(nr_pdsch_bwp_sweep_init(&st, 273), 0);
  uint16_t current;
  ASSERT_EQ(nr_pdsch_bwp_sweep_current(&st, &current), 1);
  EXPECT_EQ(current, 273) << "before any evidence, the module must not change today's behaviour";
}
```

- [ ] **Step 7: Run all three tests, confirm they pass**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && \
  g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT \
    openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc \
    openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.c \
    openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c \
    -lgtest -lgtest_main -lpthread -o /tmp/test_bwp && /tmp/test_bwp"
```

Expected: `[  PASSED  ] 3 tests.`

- [ ] **Step 8: Register the test target in CMakeLists.txt**

Insert immediately after the existing `test_nr_hyp_sweep` block (the exact block read in this session):

```cmake
  add_executable(test_nr_pdsch_bwp_sweep ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc
                                        ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.c
                                        ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c)
  target_include_directories(test_nr_pdsch_bwp_sweep PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdsch_bwp_sweep PRIVATE UTIL GTest::gtest)
  add_dependencies(tests test_nr_pdsch_bwp_sweep)
  add_test(NAME test_nr_pdsch_bwp_sweep COMMAND ./test_nr_pdsch_bwp_sweep)
```

- [ ] **Step 9: Do NOT wire this into the live decode path in this task**

Task 5 (hardware-gated) does the wiring at the exact call site Task 2 recorded, since wiring it changes live receiver behavior and this plan's constraint is that every task up to the final one must be safely buildable and testable with zero live-radio risk. Leave the module built and unit-tested, unwired, at the end of this task.

- [ ] **Step 10: Commit**

```bash
cd /home/sens/NICOLA && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.h \
  openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_bwp_sweep.c \
  openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_bwp_sweep_test.cc CMakeLists.txt && \
  git commit -m "feat: add dedicated-BWP-size hypothesis sweep (unwired, unit-tested only)

Resolves the assumed-full-carrier BWP size by TB-CRC hypothesis-and-confirm, the same method
already proven for TDA/DM-RS-position/MCS-table in nr_pdsch_config_sweep.c, built on the
generic nr_hyp_sweep primitive. Not wired into the live decode path pending X410 availability
for validation (see Task 5 of the 2026-09-23 plan).

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 4: GF(2) algebraic n_RNTI recovery — proof of concept, then C port

**Files:**
- Create: `docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py` (throwaway proof-of-concept, not shipped as part of the build)
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc`
- Modify: `CMakeLists.txt` (register the new test target)

**Interfaces:**
- Consumes: nothing from other tasks (fully independent of Tasks 1-3).
- Produces: `int nr_pdcch_gf2_rnti_recover(const uint8_t *descrambled_bits_with_rnti0, int n_bits, uint16_t *rnti_out)` — given the bits as they would appear if descrambled ASSUMING n_RNTI=0, plus a known-bad CRC (proving the true n_RNTI is non-zero), returns 1 and the recovered RNTI, or 0 if recovery fails. **Not called from any other task or any live path in this plan** — this is scoped as a standalone, unit-tested module only, per the Review Focus item above.

- [ ] **Step 1: Prove the linear-algebra approach on a small synthetic case in Python, independently verified by brute force, before writing any C**

This is the one genuinely novel component in this plan — port 5GDescrambler's core insight (the whole DCI data-scrambling + CRC-mask chain is linear over GF(2), so RNTI recovery collapses to one matrix inversion) but verify the construction is right on a toy size first.

```python
# docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py
import numpy as np

def gold_generic_step(x1, x2):
    """Mirrors openair1/PHY/gold.h's gold_generic() recurrence exactly (already read this session):
    x1(n+31) = x1(n+3) XOR x1(n); x2(n+31) = x2(n+3) XOR x2(n+2) XOR x2(n+1) XOR x2(n)."""
    b1 = (x1 & 1) ^ ((x1 >> 3) & 1)
    b2 = (x2 & 1) ^ ((x2 >> 1) & 1) ^ ((x2 >> 2) & 1) ^ ((x2 >> 3) & 1)
    x1 = (x1 >> 1) | (b1 << 30)
    x2 = (x2 >> 1) | (b2 << 30)
    return x1, x2

def gold_bit(x1, x2):
    return (x1 ^ x2) & 1

def gold_sequence(c_init, n_bits, n_skip=1600):
    x1, x2 = 1, c_init
    for _ in range(n_skip):
        x1, x2 = gold_generic_step(x1, x2)
    out = []
    for _ in range(n_bits):
        out.append(gold_bit(x1, x2))
        x1, x2 = gold_generic_step(x1, x2)
    return np.array(out, dtype=np.uint8)

def scramble_c_init(n_rnti, n_id):
    """TS 38.211 7.3.2.3: c_init = (n_RNTI << 16) + n_ID -- already used elsewhere this session."""
    return ((n_rnti << 16) + n_id) & 0x7FFFFFFF

def build_rnti_linear_model(n_id, n_bits):
    """Every scrambling bit is c_init's Gold sequence, and c_init is an AFFINE function of the 16
    RNTI bits (n_rnti << 16, added to a fixed n_id term) -- but the Gold LFSR recurrence is itself
    linear in c_init's bit representation only through its INITIAL STATE, so the output bit at
    position k, as a function of the RNTI's 16 bits, is some fixed linear (GF(2)) combination of
    those bits XOR a constant (the n_id=0 baseline). Confirm this holds by construction: perturb
    one RNTI bit at a time from an all-zero baseline and record which output bits flip -- that IS
    the corresponding column of the linear map, no assumption required."""
    baseline = gold_sequence(scramble_c_init(0, n_id), n_bits)
    M = np.zeros((n_bits, 16), dtype=np.uint8)
    for bit in range(16):
        perturbed = gold_sequence(scramble_c_init(1 << bit, n_id), n_bits)
        M[:, bit] = perturbed ^ baseline
    return M, baseline

def recover_rnti_gf2(observed_scrambled_xor_data, n_id, n_bits):
    """observed_scrambled_xor_data: the scrambling sequence bits AS IF the true RNTI produced them
    (in a real receiver this is recovered from known/CRC-checkable bits XORed with the ciphertext;
    here, for the proof, the caller passes the true sequence directly). Returns recovered RNTI."""
    M, baseline = build_rnti_linear_model(n_id, n_bits)
    target = (observed_scrambled_xor_data ^ baseline).astype(np.uint8)
    # Solve M @ rnti_bits = target over GF(2) via Gaussian elimination (16 unknowns, n_bits >= 16 equations).
    A = np.concatenate([M[:16].copy(), target[:16].reshape(-1, 1)], axis=1) % 2
    for col in range(16):
        pivot = next((r for r in range(col, 16) if A[r, col]), None)
        if pivot is None:
            continue
        A[[col, pivot]] = A[[pivot, col]]
        for r in range(16):
            if r != col and A[r, col]:
                A[r] ^= A[col]
    bits = A[:, 16]
    rnti = 0
    for i, b in enumerate(bits):
        rnti |= int(b) << i
    return rnti

if __name__ == "__main__":
    n_id, n_bits, true_rnti = 2, 64, 0x4615
    true_seq = gold_sequence(scramble_c_init(true_rnti, n_id), n_bits)
    recovered = recover_rnti_gf2(true_seq, n_id, n_bits)
    print(f"true RNTI=0x{true_rnti:04x} recovered=0x{recovered:04x} {'PASS' if recovered == true_rnti else 'FAIL'}")
    # Brute-force cross-check: confirm no OTHER RNTI in 0..65535 also matches these 64 bits,
    # which would mean 64 bits is not enough evidence, not that the linear solve is wrong.
    matches = [r for r in range(65536) if np.array_equal(gold_sequence(scramble_c_init(r, n_id), n_bits), true_seq)]
    print(f"brute-force matches at n_bits={n_bits}: {matches} {'PASS (unique)' if matches == [true_rnti] else 'AMBIGUOUS'}")
```

- [ ] **Step 2: Run the proof-of-concept**

```bash
python3 docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py
```

Expected: both lines print `PASS`. **If either prints `FAIL`/`AMBIGUOUS`, stop this task and do not write C** — it means the linear-model construction (Step 1's `build_rnti_linear_model`) or the chosen `n_bits` is wrong, and that must be fixed and re-verified in Python before any C is written, per this task's own Review Focus constraint.

- [ ] **Step 3: Write the failing C test, mirroring the now-verified Python logic exactly**

```cpp
// openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_gf2_rnti.h"
#include "openair1/PHY/gold.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_userapi.h"
configmodule_interface_t *uniqCfg = nullptr;
void exit_function(const char *,const char *,int,const char *,int) { std::abort(); }
}

static void gold_sequence(uint32_t c_init, int n_bits, uint8_t *out)
{
  uint32_t x1 = 1, x2 = c_init;
  for (int n = 1; n < 1600 + 31; n++) {
    x1 = (x1 >> 1) ^ (x1 >> 4);
    x1 = x1 ^ (x1 << 31) ^ (x1 << 28);
    x2 = (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4);
    x2 = x2 ^ (x2 << 31) ^ (x2 << 30) ^ (x2 << 29) ^ (x2 << 28);
  }
  for (int n = 0; n < n_bits; n++) {
    out[n] = (uint8_t)((x1 ^ x2) & 1);
    x1 = (x1 >> 1) ^ (x1 >> 4);
    x1 = x1 ^ (x1 << 31) ^ (x1 << 28);
    x2 = (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3) ^ (x2 >> 4);
    x2 = x2 ^ (x2 << 31) ^ (x2 << 30) ^ (x2 << 29) ^ (x2 << 28);
  }
}

TEST(Gf2Rnti, RecoversTheKnownLabCellRnti) {
  const uint16_t n_id = 2, true_rnti = 0x4615;
  const int n_bits = 64;
  uint32_t c_init = ((uint32_t)true_rnti << 16) + n_id;
  uint8_t seq[64];
  gold_sequence(c_init, n_bits, seq);
  uint16_t recovered = 0;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti);
}

TEST(Gf2Rnti, RecoversRntiZero) {
  const uint16_t n_id = 2, true_rnti = 0;
  const int n_bits = 64;
  uint8_t seq[64];
  gold_sequence(n_id, n_bits, seq);
  uint16_t recovered = 0xFFFF;
  ASSERT_EQ(nr_pdcch_gf2_rnti_recover(seq, n_bits, n_id, &recovered), 1);
  EXPECT_EQ(recovered, true_rnti);
}
```

- [ ] **Step 4: Run to confirm it fails to compile (header does not exist yet)**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && \
  g++ -c -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc -o /tmp/t2.o 2>&1 | head -5"
```

Expected: `fatal error: nr_pdcch_gf2_rnti.h: No such file or directory`.

- [ ] **Step 5: Write the header**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.h
#ifndef NR_PDCCH_GF2_RNTI_H
#define NR_PDCCH_GF2_RNTI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* GF(2) algebraic RNTI recovery (5GDescrambler-style, arxiv.org/abs/2609.07367): TS 38.211 7.3.2.3's
 * PDCCH data-scrambling c_init = (n_RNTI << 16) + n_ID is a FIXED n_ID offset plus n_RNTI shifted
 * into the initial LFSR state, and the Gold-sequence recurrence is linear (XOR-only) in that initial
 * state -- so each output scrambling bit is a fixed linear combination of the 16 RNTI bits, XOR a
 * constant baseline (the n_RNTI=0 sequence). Given >= 16 known scrambling bits (e.g. the bits a UE-
 * specific search space's descrambled-at-RNTI-0 hypothesis and a subsequently CRC-confirmed payload
 * disagree on -- the caller's problem to supply, not this module's), this recovers the RNTI as ONE
 * GF(2) linear solve instead of a 65536-way brute-force sweep. Proof of the linear-model construction
 * lives in docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py, independently cross-checked
 * against brute force before this C port was written. NOT wired into any live decode path in this
 * plan -- see the 2026-09-23 plan's Review Focus for why. */

/* seq: n_bits >= 16 scrambling-sequence bits (0/1 per byte) produced with the TRUE n_RNTI (in
 * production this comes from XORing a known-plaintext/CRC-recoverable bit sequence against the
 * observed ciphertext bits -- this module only does the linear algebra, not that recovery step).
 * n_id: the cell/CORESET's pdcch-DMRS-ScramblingID (already known independently, e.g. from this
 * project's own stage-1 blind nID discovery). Returns 1 and fills *rnti_out on success, 0 if
 * n_bits < 16 (underdetermined) or the resulting system has no solution (seq is not a valid
 * scrambling sequence for any RNTI at this n_id -- e.g. corrupted input). */
int nr_pdcch_gf2_rnti_recover(const uint8_t *seq, int n_bits, uint16_t n_id, uint16_t *rnti_out);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 6: Write the implementation, porting the verified Python directly**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.c
#include <string.h>
#include "nr_pdcch_gf2_rnti.h"
#include "openair1/PHY/gold.h"

#define GF2_MAX_BITS 64 /* the test above uses 64; the header requires only >=16 */

static void gold_seq(uint32_t c_init, int n_bits, uint8_t *out)
{
  uint32_t x1 = 1, x2 = c_init;
  for (int n = 0; n < 1600; n++)
    (void)gold_generic(&x1, &x2, n == 0);
  for (int n = 0; n < n_bits; n++)
    out[n] = (uint8_t)(gold_generic(&x1, &x2, 0) & 1);
}

/* Perturb one RNTI bit at a time from the n_RNTI=0 baseline, exactly as the verified Python does,
 * to build the 16-column GF(2) linear map without assuming its structure. */
static void build_model(uint16_t n_id, int n_bits, uint8_t M[GF2_MAX_BITS][16], uint8_t *baseline)
{
  const uint32_t c0 = ((uint32_t)0 << 16) + n_id;
  gold_seq(c0, n_bits, baseline);
  for (int bit = 0; bit < 16; bit++) {
    uint8_t perturbed[GF2_MAX_BITS];
    const uint32_t c = ((uint32_t)(1u << bit) << 16) + n_id;
    gold_seq(c, n_bits, perturbed);
    for (int i = 0; i < n_bits; i++)
      M[i][bit] = perturbed[i] ^ baseline[i];
  }
}

int nr_pdcch_gf2_rnti_recover(const uint8_t *seq, int n_bits, uint16_t n_id, uint16_t *rnti_out)
{
  if (n_bits < 16 || n_bits > GF2_MAX_BITS)
    return 0;
  uint8_t M[GF2_MAX_BITS][16], baseline[GF2_MAX_BITS];
  build_model(n_id, n_bits, M, baseline);
  /* Gaussian elimination over GF(2) on the first 16 (independent, by construction: each column is a
   * distinct single-bit perturbation) equations. */
  uint8_t A[16][17];
  for (int r = 0; r < 16; r++) {
    for (int c = 0; c < 16; c++)
      A[r][c] = M[r][c];
    A[r][16] = seq[r] ^ baseline[r];
  }
  for (int col = 0; col < 16; col++) {
    int pivot = -1;
    for (int r = col; r < 16; r++)
      if (A[r][col]) { pivot = r; break; }
    if (pivot < 0)
      return 0; /* singular -- should not happen given the construction, but never silently guess */
    if (pivot != col) {
      uint8_t tmp[17];
      memcpy(tmp, A[col], 17);
      memcpy(A[col], A[pivot], 17);
      memcpy(A[pivot], tmp, 17);
    }
    for (int r = 0; r < 16; r++)
      if (r != col && A[r][col])
        for (int c = 0; c < 17; c++)
          A[r][c] ^= A[col][c];
  }
  uint16_t rnti = 0;
  for (int i = 0; i < 16; i++)
    if (A[i][16])
      rnti |= (uint16_t)(1u << i);
  *rnti_out = rnti;
  return 1;
}
```

- [ ] **Step 7: Run the tests from Step 3, confirm they pass**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && \
  g++ -std=c++17 -I . -I openair1/PHY/NR_UE_TRANSPORT \
    openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc \
    openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.c \
    -lgtest -lgtest_main -lpthread -o /tmp/test_gf2 && /tmp/test_gf2"
```

Expected: `[  PASSED  ] 2 tests.`

- [ ] **Step 8: Add a negative test — insufficient bits must fail cleanly, not guess**

```cpp
// append to nr_pdcch_gf2_rnti_test.cc
TEST(Gf2Rnti, RejectsInsufficientBits) {
  uint8_t seq[8] = {0};
  uint16_t recovered = 0;
  EXPECT_EQ(nr_pdcch_gf2_rnti_recover(seq, 8, 2, &recovered), 0);
}
```

Run the same command from Step 7 again; expected `[  PASSED  ] 3 tests.`

- [ ] **Step 9: Register the test target in CMakeLists.txt**

```cmake
  add_executable(test_nr_pdcch_gf2_rnti ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc
                                       ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.c)
  target_include_directories(test_nr_pdcch_gf2_rnti PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT})
  target_link_libraries(test_nr_pdcch_gf2_rnti PRIVATE UTIL GTest::gtest)
  add_dependencies(tests test_nr_pdcch_gf2_rnti)
  add_test(NAME test_nr_pdcch_gf2_rnti COMMAND ./test_nr_pdcch_gf2_rnti)
```

(Note the closing paren typo risk: the line above must read `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT` with no stray `}` — copy the exact pattern from the already-working `test_nr_hyp_sweep` block read this session, not this snippet verbatim, if in doubt.)

- [ ] **Step 10: Commit**

```bash
cd /home/sens/NICOLA && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.h \
  openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gf2_rnti.c \
  openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_gf2_rnti_test.cc \
  docs/superpowers/plans/2026-09-23-gf2-rnti-recovery-poc.py CMakeLists.txt && \
  git commit -m "feat: add GF(2) algebraic n_RNTI recovery (5GDescrambler-style), unwired

Standalone, unit-tested module for the n_RNTI=C-RNTI case (macro cells with a configured
pdcch-DMRS-ScramblingID, where a new UE's RNTI cannot be brute-forced cheaply). Python
proof-of-concept in docs/superpowers/plans/ independently verifies the linear-model
construction against brute force before this C port. Not wired into any live path.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Task 5: Hardware-gated validation (BLOCKED — do not start until the X410 is confirmed available)

**Files:**
- Modify: the exact call site Task 2's audit recorded (only if Task 3 was built) — substitute `nr_pdsch_bwp_sweep_current()`'s output for the hardcoded `cfg->bwp_size`/full-carrier value.
- Modify: `/home/sens/NICOLA/captures/cons6_ota_pdschtest.conf` (already exists from this session, `sensing.enable=1` and `sources` already set) — no changes expected, reuse as-is unless Step 1 finds it stale.

**Interfaces:**
- Consumes: Task 1's measured LDPC cost number (to judge whether observed timing is reasonable), Task 3's `nr_pdsch_bwp_sweep_current()` (if built).
- Produces: a real, live `pdsch_decode[try=... crc_ok=...]` count from `nr_pdcch_blind_monitor_rt.c`'s own summary line, and Technique D's own convergence log lines, on the lab cell.

- [ ] **Step 1: Confirm the X410 is available and the receiver is idle**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem | wc -l"
```

Expected: `0`. If not, stop and follow this project's standing rule: SIGINT (not `-9`), wait for exit, wait 60s before the next launch.

- [ ] **Step 2: Wire Task 3's module at the recorded call site (skip this step entirely if Task 2 concluded "already handled")**

Use the exact file:line Task 2's `docs/superpowers/plans/2026-09-23-bwp-size-audit.md` recorded. The change is a substitution: wherever `cfg->bwp_size` (or the equivalent full-carrier value) is assigned into the DCI/PDSCH config struct, call `nr_pdsch_bwp_sweep_current(&g_bwp_sweep_state, &bwp_size)` instead and use `bwp_size`. Feed its `_next`/`_feed` calls from the same TB-CRC outcome location `nr_pdsch_config_sweep_feedback()` already reads from (line 5999 of `nr_pdcch_blind_monitor_rt.c`, read this session) — call both sweeps' feedback functions at that one site, since they observe the same CRC outcome.

- [ ] **Step 3: Build (only when the receiver is confirmed idle)**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j8 nr-uesoftmodem 2>&1 | grep -E 'error|Built target nr-uesoftmodem'"
```

Expected: `[100%] Built target nr-uesoftmodem`, no `error` lines.

- [ ] **Step 4: Launch on the lab cell using the already-built PDSCH test conf, with a Monitor watching for the outcome**

```bash
ssh sens6 "cd /home/sens/NICOLA/captures && pgrep -x nr-uesoftmodem && exit 1; sudo rm -f /tmp/passive_rx/idsweep_*.bin /tmp/coresets_discovered.txt; \
  (ARM=pdschtest1 CONF=/home/sens/NICOLA/captures/cons6_ota_pdschtest.conf DUR=300 CARRIER=3450000000 SSB=150 \
   XENV='ISAC_COREMAP_IDSWEEP=12 ISAC_LANE_ALS=1,2,4,8,16 ISAC_TDD_SKIP=1' setsid nohup ./run_sw.sh > pdschtest1_launcher.out 2>&1 < /dev/null &) ; \
  (N=10 setsid nohup ./discover_live.sh > pdschtest1_discovery.out 2>&1 < /dev/null &) ; sleep 1; echo launched"
```

Then arm a Monitor on the run directory's log for `cfr_submits\|pdsch_decode\[try\|Segmentation\|terminate called`, per this project's standing rule to never wait on a capture without a Monitor armed.

- [ ] **Step 5: After the run ends, read the real numbers**

```bash
ssh sens6 "L=\$(ls -td /home/sens/NICOLA/captures/pdschtest1_*/ | head -1); grep -a 'cfr_submits\|pdsch_decode\[try' \$L/run.log | tail -1"
```

Record the raw `try=`/`crc_ok=`/`cfr_submits=` numbers exactly as printed — do not round or characterize them until they are written down.

- [ ] **Step 6: Write the result, following this session's own established discipline for reporting numbers**

Write `docs/superpowers/plans/2026-09-23-pdsch-live-validation-result.md` with: the exact command run, the exact raw counter line from Step 5, and one paragraph stating plainly whether `try > 0` (decode was attempted at all — the first open question from this session) and whether `crc_ok > 0` (data actually decoded). **Do not claim a percentage from a single run** — this project's own standing finding (`passive-rx-needs-5-runs-per-arm`) is that single-run numbers on this rig are not reliable; if `crc_ok > 0` at all, that answers "does it work" (existence), and a percentage claim needs >= 3 runs, which is a follow-on task, not part of this one.

- [ ] **Step 7: Commit**

```bash
cd /home/sens/NICOLA && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c \
  docs/superpowers/plans/2026-09-23-pdsch-live-validation-result.md && \
  git commit -m "feat: wire dedicated-BWP-size sweep into the live DCI decode path; record first PDSCH live-validation result

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage:** The four confirmed-genuine gaps from this session's discussion (real LDPC cost number, BWP-size resolution, GF(2) RNTI recovery, and actually running the existing-but-never-exercised PDSCH decode path together with tonight's new discovery/multi-RNTI machinery) each map to Tasks 1, 2-3, 4, and 5 respectively. The three items this session discovered were ALREADY built (MCS table, DM-RS position/length sweep, LBRM n_L) are explicitly excluded and the Background section states why, so a reader does not wonder why they are missing from this plan.

**2. Placeholder scan:** Checked every step for "TBD"/"handle appropriately"/"similar to Task N" — none found; every code step contains complete, real code using real function/struct names read directly from the source this session.

**3. Type consistency:** `nr_pdsch_bwp_sweep_state_t`, `nr_pdsch_bwp_sweep_init/_next/_feed/_current` are used identically across Task 3's steps and Task 5's wiring step. `nr_pdcch_gf2_rnti_recover`'s signature is identical between its header (Task 4 Step 5) and its test (Task 4 Step 3) and implementation (Task 4 Step 6).

**4. Review Focus:** Each of the five items in the Review Focus section has its test in the owning task: the `ldpctest` real-parameter run (Task 1, Steps 1-2) exercises the timing item; Task 4's explicit "no wiring" step (Step 9) and Task 5's own gating exercise the "GF(2) never live-wired in this plan" item; Task 2's own Step 4 forces a written decision either way, exercising the "audit must not fizzle silently" item; Task 3's Step 1 test explicitly exercises the `nr_hyp_t` byte-encoding convention via `memcpy`, matching the existing test's own pattern; Task 4's Steps 1-2 (Python proof against brute force) exercise the "verify before C" item directly.

---

Plan complete and saved to `docs/superpowers/plans/2026-09-23-agnostic-pdsch-pusch-remaining-gaps.md`. Please review the plan. Which execution approach would you prefer?

- **Subagent-driven** — a fresh subagent implements each task and a fresh reviewer checks it before the next one starts, then a whole-branch review at the end. Most thorough; costs a fresh context per task and per review.
- **Native** — I implement every task myself in this session, the way this harness runs work, then one fresh reviewer on the most capable model checks the whole branch. Cheapest and fastest; no independent review until the end.

For this plan I recommend **Native**, because Tasks 1-4 are almost entirely independent of each other (only Task 5 depends on Task 2's audit outcome and Tasks 3-4's outputs) and each is small enough that a shipped mistake is cheap to catch and fix in the same session — a fresh-subagent-per-task setup would mostly pay context cost without buying much independence here. Task 5 stays blocked on you telling me the X410 is back regardless of which approach we pick. Does the plan capture what you want, and which approach should we use?
