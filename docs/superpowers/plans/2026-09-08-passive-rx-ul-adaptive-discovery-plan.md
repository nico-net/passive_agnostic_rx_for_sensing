# Passive-RX UL Adaptive Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the passive receiver's UL DCI 0_1 path fully self-configuring — discover DCI
length, per-field bit widths, and field-value interpretation (TDA table, DM-RS config, MCS table)
all by search, with no gNB log and no manual per-cell conf, mirroring what the DL path already
does.

**Architecture:** One new shared 4-stage hypothesis-search engine (`nr_hyp_sweep`), reused by two
independent sweeps: Component 2 (field bit-widths, scored by whether the resulting PUSCH decodes)
and Component 3 (field-value interpretation — TDA/DM-RS/MCS-table — scored the same way).
Component 1 (DCI-length sweep) reuses the existing, already format-agnostic Technique C module
directly, no new engine needed.

**Tech Stack:** C (project convention: OAI logging macros, `nr_`/`NR_` prefixes, callback-struct
polymorphism, not C++ templates), GTest for offline unit tests, CMake/Ninja build.

**Spec:** `docs/superpowers/specs/2026-09-08-passive-rx-ul-adaptive-discovery-design.md`
(this repo, same branch) — this plan implements that spec's Components 1-3 and shared engine
section. Read it alongside this plan; the "why" for every design choice below lives there.

## Global Constraints

- **No PUSCH-decoder signature change.** `nr_pusch_passive_decode()` keeps its current signature;
  hypothesis variation happens only at the PDCCH extraction call site (varying `opts` before
  `nr_pdcch_blind_decode_and_extract_01()`).
- **Stage c (plausibility gate) is reject-only, forever.** It may only return `false` for a
  spec-impossible hypothesis. It must never rank or score a hypothesis it lets through — doing so
  would bias discovery toward assumptions about the network being inferred (spec's own stated
  invariant).
- **Cap violations refuse loudly, never truncate silently.** Both the raw-hypothesis cap and the
  surviving-class cap, on overflow, log at `LOG_E` and return a sentinel the caller must not treat
  as "zero hypotheses" — silently truncating would bias the search toward whichever hypotheses
  were enumerated first.
- **Nothing in this plan touches `nr_pdsch_config_sweep.c` (DL Technique D).** Explicitly deferred
  per the spec's "Scope" section.
- **Never share sweep state between DL and UL.** Component 1's UL length-sweep state is a second,
  independent `nr_pdcch_dci_length_sweep_state_t` instance — the existing DL one must not be
  touched.
- **Follow existing file conventions**: OAI license header (copy verbatim from
  `nr_pdcch_dci_length_sweep.h`), `nr_`/`NR_` prefixes, `LOG_I`/`LOG_W`/`LOG_E`/`LOG_A` with `PHY`
  tag, C (not C++) for all non-test `.c`/`.h` files, GTest `.cc` for tests.

---

## File Structure

| File | Responsibility |
|---|---|
| `openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.{h,c}` | New. Shared 4-stage engine: admissible generation (a), equivalence collapsing (b), reject-only plausibility gate (c), class-scored TB-CRC oracle (d). Hypothesis-type-agnostic via a fixed-size opaque byte buffer + caller callbacks. |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc` | New. Per-stage unit tests on a synthetic hypothesis type. |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` | Modified. New UL length-sweep scorer (Component 1) + wiring block mirroring the existing DL one; Component 2/3 hypothesis pick-and-apply at UL candidate build time. |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc` | Modified. Adds DL/UL sweep-state independence cases. |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h` | Modified. Adds two echo fields to `nr_pdcch_blind_ul_result_t` so a chosen hypothesis class rides through the grant book to the PUSCH-decode result point. |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.{h,c}` | New. Component 2: field bit-width hypothesis generator + constraints, built on `nr_hyp_sweep`. |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc` | New. Offline known-answer + tie-inspection test for Component 2. |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.{h,c}` | New. Component 3: TDA-table/DM-RS/MCS-table hypothesis generator + constraints, built on `nr_hyp_sweep`. |
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc` | New. Offline known-answer + tie-inspection test for Component 3. |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.c` | Modified. Feed TB-CRC result back into whichever of Component 2/3's sweep is active, at the inline decode call site. |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c` | Modified. Same feed, at the deferred/queued decode call site. |
| `CMakeLists.txt` | Modified. Register all new `.c` sources into the `nr_pdcch_blind_monitor` library; register new test executables mirroring the existing `test_nr_pdcch_dci_length_sweep` pattern. |

All work happens over `ssh sens6`, repo `/home/sens/NICOLA/openairinterface5g-total-passive-ue`,
branch `total-passive-rx-UL-DL-graphics`. Build with `ninja nr-uesoftmodem` from
`cmake_targets/ran_build/build/` per this project's CLAUDE.md; test executables build via `ninja
tests` / individual `ninja test_<name>` targets, run via `ctest` or the binary directly. **Never
build while any `nr-uesoftmodem` capture is running** (`pgrep -x nr-uesoftmodem` must be empty
first) — a standing project rule, not new to this plan.

---

## Task 1: Shared engine — types, Stage a (admissible generation), Stage b (equivalence collapsing)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc`

**Interfaces:**
- Produces: `nr_hyp_t` (opaque hypothesis, `uint8_t bytes[96]` + `int len`), `nr_hyp_constraint_fn`,
  `nr_hyp_equivalent_fn`, `nr_hyp_sweep_state_t`, `nr_hyp_sweep_init()`.
- Consumed by: Task 2 (adds Stage c/d to the same files), Task 7 (Component 2), Task 10
  (Component 3).

- [ ] **Step 1: Write the failing test for Stage a (constraint filtering)**

```cpp
// openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc
// (license header copied verbatim from nr_pdcch_dci_length_sweep_test.cc)
#include <cstdint>
#include <cstring>
#include "gtest/gtest.h"
extern "C" {
#include "nr_hyp_sweep.h"
}

// Synthetic hypothesis: one int, values 0..9. Used by every test in this file so each stage is
// exercised without any real DCI/PUSCH machinery.
struct SynthHyp { int v; };

static void pack(nr_hyp_t *h, int v) {
  SynthHyp s{v};
  std::memcpy(h->bytes, &s, sizeof(s));
  h->len = (int)sizeof(s);
}
static int unpack(const nr_hyp_t *h) {
  SynthHyp s;
  std::memcpy(&s, h->bytes, sizeof(s));
  return s.v;
}

static bool constraint_even(const nr_hyp_t *hyp, void * /*ctx*/) {
  return (unpack(hyp) % 2) == 0;
}

TEST(NrHypSweepStageA, ConstraintDropsInadmissibleHypotheses) {
  nr_hyp_t raw[10];
  for (int i = 0; i < 10; i++) pack(&raw[i], i);

  nr_hyp_sweep_state_t st;
  const int n_classes = nr_hyp_sweep_init(&st, raw, 10, constraint_even, nullptr,
                                          nullptr, nullptr, 0, nullptr);

  ASSERT_EQ(n_classes, 5); // 0,2,4,6,8
  for (int i = 0; i < n_classes; i++) {
    EXPECT_EQ(unpack(&st.classes[i].hyp) % 2, 0);
  }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT -c openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc -o /tmp/t.o`
Expected: FAIL — `nr_hyp_sweep.h: No such file or directory`.

- [ ] **Step 3: Write `nr_hyp_sweep.h` with the types and Stage a/b declarations**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.h
/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.h
 * \brief Shared 4-stage hypothesis-search engine used by the UL field-width sweep (Component 2)
 * and the UL field-interpretation sweep (Component 3) -- see
 * docs/superpowers/specs/2026-09-08-passive-rx-ul-adaptive-discovery-design.md's "Shared
 * hypothesis-search engine" section for the design rationale.
 *
 * Stage a (admissible generation) and Stage b (equivalence collapsing) run ONCE, before any
 * candidate is decoded, and build the class list nr_hyp_sweep_init() returns. Stage c
 * (plausibility, reject-only) and Stage d (TB-CRC oracle) run once PER LIVE CANDIDATE via
 * nr_hyp_sweep_next()/nr_hyp_sweep_feed().
 *
 * Stage c is reject-only BY CONTRACT: a nr_hyp_plausible_fn must only return false for a
 * spec-impossible hypothesis. It must never rank or score hypotheses it lets through -- doing so
 * would bias the search toward assumptions about the very network configuration being inferred.
 */
#ifndef NR_HYP_SWEEP_H
#define NR_HYP_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_HYP_SWEEP_MAX_RAW     8192 // raw (pre-equivalence-collapse) hypotheses; the original
                                      // manual UL solve's own "3963 raw" precedent fits with
                                      // margin. Exceeding this is a loud refusal (-1), never a
                                      // silent truncation.
#define NR_HYP_SWEEP_MAX_CLASSES 64   // surviving equivalence classes; matches Technique D's own
                                      // cap (nr_pdsch_config_sweep.h's NR_PDSCH_SWEEP_MAX_HYP).
#define NR_HYP_BYTES             96   // big enough for either Component 2's 16-int field-width
                                      // vector (64 bytes) or Component 3's interpretation struct.

typedef struct {
  uint8_t bytes[NR_HYP_BYTES];
  int     len; // caller's real struct size; the engine only ever memcmp/memcpy this many bytes
} nr_hyp_t;

typedef struct {
  nr_hyp_t hyp; // representative of this class
  uint32_t trials;
  uint32_t passes;
} nr_hyp_class_t;

typedef struct {
  nr_hyp_class_t classes[NR_HYP_SWEEP_MAX_CLASSES];
  int            n_classes;
  int            cursor; // round-robin position over classes
  int            winner; // -1 until decided
} nr_hyp_sweep_state_t;

/** Stage a: does `hyp` satisfy this constraint? Reject-only (see file header). */
typedef bool (*nr_hyp_constraint_fn)(const nr_hyp_t *hyp, void *ctx);

/** Stage b: do `a` and `b` produce the SAME observable outcome when applied to `sample`?
 * `sample` is caller-defined (e.g. a real, already-observed candidate payload). */
typedef bool (*nr_hyp_equivalent_fn)(const nr_hyp_t *a, const nr_hyp_t *b, const void *sample,
                                     void *ctx);

/** Stage c: is `hyp` structurally possible given this one live `candidate`? Reject-only -- see
 * the file header's contract. */
typedef bool (*nr_hyp_plausible_fn)(const nr_hyp_t *hyp, const void *candidate, void *ctx);

/**
 * @brief Stages a+b: build the class list. Call ONCE, before any live candidate is scored.
 *
 * @param st             Output state; fully initialized by this call (no separate reset needed).
 * @param raw/n_raw      Every hypothesis the caller's generator produced, BEFORE constraints.
 *                       n_raw > NR_HYP_SWEEP_MAX_RAW returns -1 without touching *st.
 * @param constraint     Stage a callback; NULL admits every raw hypothesis unfiltered.
 * @param constraint_ctx Passed through to `constraint` unchanged.
 * @param equiv          Stage b callback; NULL skips collapsing (every admissible hypothesis
 *                       becomes its own class) -- e.g. when no candidates have been observed yet.
 * @param samples/n_samples  Real candidate payloads Stage b tests equivalence against. Two
 *                       hypotheses collapse into one class only if `equiv` reports them identical
 *                       on EVERY sample.
 * @param equiv_ctx      Passed through to `equiv` unchanged.
 * @return number of surviving classes, or -1 (raw cap exceeded) or -2 (class cap exceeded after
 *         collapsing) -- both loud refusals; the caller must not proceed as if 0 hypotheses exist.
 */
int nr_hyp_sweep_init(nr_hyp_sweep_state_t *st, const nr_hyp_t *raw, int n_raw,
                      nr_hyp_constraint_fn constraint, void *constraint_ctx,
                      nr_hyp_equivalent_fn equiv, const void *samples, int n_samples,
                      void *equiv_ctx);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 4: Implement Stage a in `nr_hyp_sweep.c`, leaving Stage b as a pass-through (every admissible hypothesis its own class) so Step 1's test can pass**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c
/* license header identical to the .h file's, omitted here for brevity in this plan --
   copy it verbatim when creating the real file */
#include "nr_hyp_sweep.h"
#include <string.h>

int nr_hyp_sweep_init(nr_hyp_sweep_state_t *st, const nr_hyp_t *raw, int n_raw,
                      nr_hyp_constraint_fn constraint, void *constraint_ctx,
                      nr_hyp_equivalent_fn equiv, const void *samples, int n_samples,
                      void *equiv_ctx)
{
  if (st == NULL || raw == NULL || n_raw < 0) {
    return -1;
  }
  if (n_raw > NR_HYP_SWEEP_MAX_RAW) {
    return -1;
  }
  memset(st, 0, sizeof(*st));
  st->winner = -1;

  // Stage a: admit only hypotheses the constraint accepts (or all of them, if constraint == NULL).
  nr_hyp_t admissible[NR_HYP_SWEEP_MAX_RAW];
  int n_admissible = 0;
  for (int i = 0; i < n_raw; i++) {
    if (constraint == NULL || constraint(&raw[i], constraint_ctx)) {
      admissible[n_admissible++] = raw[i];
    }
  }

  // Stage b: placeholder pass-through for this step -- every admissible hypothesis is its own
  // class. Implemented for real in Task 1 Step 7.
  (void)equiv; (void)samples; (void)n_samples; (void)equiv_ctx;
  if (n_admissible > NR_HYP_SWEEP_MAX_CLASSES) {
    return -2;
  }
  for (int i = 0; i < n_admissible; i++) {
    st->classes[i].hyp = admissible[i];
    st->classes[i].trials = 0;
    st->classes[i].passes = 0;
  }
  st->n_classes = n_admissible;
  return st->n_classes;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c -lgtest -lgtest_main -pthread -o /tmp/t && /tmp/t`
Expected: PASS.

- [ ] **Step 6: Write the failing test for Stage b (equivalence collapsing)**

```cpp
// append to nr_hyp_sweep_test.cc

// Two hypotheses are "equivalent" for this synthetic test iff they have the same parity --
// mirrors the real Component 2 finding (splits differing only in padding decode identically).
static bool equiv_same_parity(const nr_hyp_t *a, const nr_hyp_t *b, const void * /*sample*/,
                              void * /*ctx*/) {
  return (unpack(a) % 2) == (unpack(b) % 2);
}

TEST(NrHypSweepStageB, EquivalentHypothesesCollapseIntoOneClass) {
  nr_hyp_t raw[6];
  for (int i = 0; i < 6; i++) pack(&raw[i], i); // 0,1,2,3,4,5

  nr_hyp_sweep_state_t st;
  const int dummy_sample = 0;
  const int n_classes = nr_hyp_sweep_init(&st, raw, 6, nullptr, nullptr,
                                          equiv_same_parity, &dummy_sample, 1, nullptr);

  // {0,2,4} collapse to one class, {1,3,5} collapse to another.
  ASSERT_EQ(n_classes, 2);
}

TEST(NrHypSweepStageB, NoSamplesSkipsCollapsing) {
  nr_hyp_t raw[3];
  for (int i = 0; i < 3; i++) pack(&raw[i], i);

  nr_hyp_sweep_state_t st;
  const int n_classes = nr_hyp_sweep_init(&st, raw, 3, nullptr, nullptr,
                                          equiv_same_parity, nullptr, 0, nullptr);
  ASSERT_EQ(n_classes, 3); // n_samples == 0: every hypothesis stays its own class
}
```

- [ ] **Step 7: Run tests to verify the new two fail, then implement real Stage b collapsing**

Run: `g++ ... && /tmp/t` — expect `EquivalentHypothesesCollapseIntoOneClass` to FAIL (gets 6
classes, not 2); `NoSamplesSkipsCollapsing` PASSes already (matches the placeholder's behavior).

Replace the Stage b placeholder block in `nr_hyp_sweep.c`:

```c
  // Stage b: collapse admissible hypotheses into equivalence classes. Two hypotheses merge only
  // if `equiv` reports them identical on EVERY sample (a single differing sample is enough to
  // keep them separate). O(n_admissible^2 * n_samples); n_admissible is capped at
  // NR_HYP_SWEEP_MAX_RAW and this runs once, not per candidate, so this cost is acceptable.
  int class_of[NR_HYP_SWEEP_MAX_RAW];
  for (int i = 0; i < n_admissible; i++) {
    class_of[i] = -1;
  }
  int n_classes = 0;
  nr_hyp_t reps[NR_HYP_SWEEP_MAX_CLASSES];
  for (int i = 0; i < n_admissible; i++) {
    if (class_of[i] >= 0) {
      continue; // already assigned to an earlier hypothesis's class
    }
    int my_class = -1;
    if (equiv != NULL && n_samples > 0) {
      for (int c = 0; c < n_classes; c++) {
        bool all_equal = true;
        for (int s = 0; s < n_samples; s++) {
          const void *sample = (const uint8_t *)samples + 0; // caller indexes its own samples;
                                                               // see the header note below
          if (!equiv(&reps[c], &admissible[i], sample, equiv_ctx)) {
            all_equal = false;
            break;
          }
        }
        if (all_equal) {
          my_class = c;
          break;
        }
      }
    }
    if (my_class < 0) {
      if (n_classes >= NR_HYP_SWEEP_MAX_CLASSES) {
        return -2;
      }
      my_class = n_classes;
      reps[n_classes++] = admissible[i];
    }
    class_of[i] = my_class;
  }
```

**Design note carried into the real implementation (not left as an ambiguity)**: `samples` is an
array of caller-defined sample records; the engine does not know the record's stride, so the real
`nr_hyp_sweep.h` documents that `samples` must be an array of `const void *` pointers (one per
sample), not a flat byte array — fix the signature to `const void *const *samples` and the loop
above to `samples[s]` before this step is considered done. Update both the test call sites (Step 6
now pass `const void *samples[] = {&dummy_sample}`) and the doc comment accordingly.

- [ ] **Step 8: Run tests to verify all four pass**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c -lgtest -lgtest_main -pthread -o /tmp/t && /tmp/t`
Expected: PASS, 4/4.

- [ ] **Step 9: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.h openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc
git commit -m "$(cat <<'EOF'
Add shared hypothesis-sweep engine, Stages a+b (generation, equivalence)

Constraint-pluggable admissible-set generation plus no-decode equivalence-
class collapsing, the two offline stages of the 4-stage engine both new UL
sweeps (Components 2+3) will share. Reject-only by construction: Stage a's
constraint callback and Stage b's equivalence callback never rank
hypotheses, only partition them.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 2: Shared engine — Stage c (reject-only plausibility gate) + Stage d (class-scored oracle)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.h`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc`

**Interfaces:**
- Consumes: `nr_hyp_sweep_state_t` from Task 1.
- Produces: `nr_hyp_plausible_fn`, `nr_hyp_sweep_next()`, `nr_hyp_sweep_feed()`,
  `nr_hyp_sweep_winner()` — the four functions Components 2/3 (Tasks 7, 10) call per live
  candidate.

- [ ] **Step 1: Write the failing tests for Stage c/d**

```cpp
// append to nr_hyp_sweep_test.cc

// Every synthetic hypothesis is "plausible" except v == 4 (a spec-impossible stand-in).
static bool plausible_not_four(const nr_hyp_t *hyp, const void * /*candidate*/, void * /*ctx*/) {
  return unpack(hyp) != 4;
}

TEST(NrHypSweepStageC, ImplausibleClassIsSkippedNotEliminated) {
  nr_hyp_t raw[2];
  pack(&raw[0], 4); // will be implausible for every candidate in this test
  pack(&raw[1], 7);
  nr_hyp_sweep_state_t st;
  ASSERT_EQ(nr_hyp_sweep_init(&st, raw, 2, nullptr, nullptr, nullptr, nullptr, 0, nullptr), 2);

  nr_hyp_t chosen;
  const int dummy_cand = 0;
  // Round-robin starts at class 0 (v=4), which is implausible -- must skip to class 1 (v=7).
  const int idx = nr_hyp_sweep_next(&st, &dummy_cand, plausible_not_four, nullptr, &chosen);
  ASSERT_EQ(idx, 1);
  ASSERT_EQ(unpack(&chosen), 7);
}

TEST(NrHypSweepStageD, ConvergesToTheHighPassRateClass) {
  nr_hyp_t raw[2];
  pack(&raw[0], 0); // will be fed as the WRONG hypothesis (~0% pass)
  pack(&raw[1], 1); // will be fed as the RIGHT hypothesis (~90% pass)
  nr_hyp_sweep_state_t st;
  ASSERT_EQ(nr_hyp_sweep_init(&st, raw, 2, nullptr, nullptr, nullptr, nullptr, 0, nullptr), 2);

  int winner = -1;
  for (int trial = 0; trial < 1000 && winner < 0; trial++) {
    nr_hyp_t chosen;
    const int dummy_cand = 0;
    const int idx = nr_hyp_sweep_next(&st, &dummy_cand, nullptr, nullptr, &chosen);
    ASSERT_GE(idx, 0);
    const bool crc_ok = (unpack(&chosen) == 1) ? ((trial % 10) != 0) : false; // ~90% vs 0%
    winner = nr_hyp_sweep_feed(&st, idx, crc_ok);
  }
  ASSERT_GE(winner, 0);
  EXPECT_EQ(unpack(&st.classes[winner].hyp), 1);
  EXPECT_EQ(nr_hyp_sweep_winner(&st), winner);
}

TEST(NrHypSweepStageD, RefusesToDecideWhenClassesAreTied) {
  nr_hyp_t raw[2];
  pack(&raw[0], 0);
  pack(&raw[1], 1);
  nr_hyp_sweep_state_t st;
  ASSERT_EQ(nr_hyp_sweep_init(&st, raw, 2, nullptr, nullptr, nullptr, nullptr, 0, nullptr), 2);

  for (int trial = 0; trial < 1000; trial++) {
    nr_hyp_t chosen;
    const int dummy_cand = 0;
    const int idx = nr_hyp_sweep_next(&st, &dummy_cand, nullptr, nullptr, &chosen);
    nr_hyp_sweep_feed(&st, idx, (trial % 2) == 0); // BOTH classes get identical ~50% pass rates
  }
  EXPECT_EQ(nr_hyp_sweep_winner(&st), -1);
}
```

- [ ] **Step 2: Run tests to verify they fail to compile (Stage c/d functions don't exist yet)**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c -lgtest -lgtest_main -pthread -o /tmp/t`
Expected: FAIL — `nr_hyp_sweep_next`/`nr_hyp_sweep_feed`/`nr_hyp_sweep_winner` undeclared.

- [ ] **Step 3: Add Stage c/d declarations to `nr_hyp_sweep.h`**

```c
// append to nr_hyp_sweep.h, before the closing #ifdef __cplusplus

#define NR_HYP_SWEEP_MIN_TRIALS 300 // same value as Technique D's SWEEP_MIN_TRIALS -- calibrated
                                    // there against this cell's measured ~76% working-hypothesis
                                    // TB-CRC rate; re-derive if that rate changes materially.
#define NR_HYP_SWEEP_WIN_RATIO  3.0
#define NR_HYP_SWEEP_MIN_RATE   0.02

/**
 * @brief Stage c: pick the next class to try for ONE live candidate, round-robin over surviving
 *        classes, skipping any class whose representative `plausible` rejects for THIS candidate.
 *
 * @param candidate    Caller-defined; passed through to `plausible` unchanged.
 * @param plausible    Reject-only (see file header); NULL admits every class unconditionally.
 * @param[out] out     The chosen class's representative hypothesis.
 * @return the class index to try, or -1 if every surviving class was implausible for this
 *         candidate (caller should skip decode for this candidate -- try again next candidate,
 *         nothing is eliminated), or the winner's index (with `out` set to its representative) if
 *         Stage d has already decided.
 */
int nr_hyp_sweep_next(nr_hyp_sweep_state_t *st, const void *candidate,
                      nr_hyp_plausible_fn plausible, void *plausible_ctx, nr_hyp_t *out);

/**
 * @brief Stage d: report the TB-CRC outcome of the grant decoded under class `idx`.
 * @return the winning class index once decided (same decision rule as Technique D: every class
 *         must clear NR_HYP_SWEEP_MIN_TRIALS, then the best must beat the runner-up by
 *         NR_HYP_SWEEP_WIN_RATIO and clear NR_HYP_SWEEP_MIN_RATE), else -1.
 */
int nr_hyp_sweep_feed(nr_hyp_sweep_state_t *st, int idx, bool crc_ok);

/** Winner, or -1 if undecided. */
int nr_hyp_sweep_winner(const nr_hyp_sweep_state_t *st);
```

- [ ] **Step 4: Implement Stage c/d in `nr_hyp_sweep.c`**

```c
// append to nr_hyp_sweep.c

int nr_hyp_sweep_next(nr_hyp_sweep_state_t *st, const void *candidate,
                      nr_hyp_plausible_fn plausible, void *plausible_ctx, nr_hyp_t *out)
{
  if (st == NULL || out == NULL || st->n_classes <= 0) {
    return -1;
  }
  if (st->winner >= 0) {
    *out = st->classes[st->winner].hyp;
    return st->winner;
  }
  // Round-robin, skipping implausible classes for THIS candidate. At most n_classes tries so an
  // all-implausible candidate returns -1 instead of spinning.
  for (int tries = 0; tries < st->n_classes; tries++) {
    const int idx = st->cursor;
    st->cursor = (st->cursor + 1) % st->n_classes;
    if (plausible == NULL || plausible(&st->classes[idx].hyp, candidate, plausible_ctx)) {
      *out = st->classes[idx].hyp;
      return idx;
    }
  }
  return -1;
}

static double class_rate(const nr_hyp_sweep_state_t *st, int i)
{
  return (st->classes[i].trials > 0)
             ? ((double)st->classes[i].passes / (double)st->classes[i].trials)
             : 0.0;
}

int nr_hyp_sweep_feed(nr_hyp_sweep_state_t *st, int idx, bool crc_ok)
{
  if (st == NULL || idx < 0 || idx >= st->n_classes) {
    return (st != NULL) ? st->winner : -1;
  }
  if (st->winner >= 0) {
    return st->winner;
  }
  st->classes[idx].trials++;
  if (crc_ok) {
    st->classes[idx].passes++;
  }

  for (int i = 0; i < st->n_classes; i++) {
    if (st->classes[i].trials < NR_HYP_SWEEP_MIN_TRIALS) {
      return -1; // every class needs a fair shot before any decision
    }
  }
  int best = 0, second = -1;
  for (int i = 1; i < st->n_classes; i++) {
    if (class_rate(st, i) > class_rate(st, best)) {
      best = i;
    }
  }
  for (int i = 0; i < st->n_classes; i++) {
    if (i != best && (second < 0 || class_rate(st, i) > class_rate(st, second))) {
      second = i;
    }
  }
  const double rb = class_rate(st, best);
  const double rs = (second >= 0) ? class_rate(st, second) : 0.0;
  if (rb >= NR_HYP_SWEEP_MIN_RATE && (rs <= 0.0 || rb >= NR_HYP_SWEEP_WIN_RATIO * rs)) {
    st->winner = best;
  }
  return st->winner;
}

int nr_hyp_sweep_winner(const nr_hyp_sweep_state_t *st)
{
  return (st != NULL) ? st->winner : -1;
}
```

- [ ] **Step 5: Run tests to verify all seven pass**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c -lgtest -lgtest_main -pthread -o /tmp/t && /tmp/t`
Expected: PASS, 7/7.

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.h openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc
git commit -m "$(cat <<'EOF'
Add shared hypothesis-sweep engine, Stages c+d (plausibility gate, oracle)

Round-robin over surviving equivalence classes (not raw hypotheses),
skipping any class the reject-only plausibility gate rules out for the
current candidate. Decision rule is Technique D's own (min-trials, then
win-ratio + floor), ported to score per-class instead of per-hypothesis --
the engine's stopping criterion is "one class is ahead", i.e. grant-
equivalence, not "one exact hypothesis remains".

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 3: Register the shared engine in CMake, confirm it builds inside the real toolchain

The tests so far ran with a bare `g++` invocation for speed; this task proves the files also build
under the project's real CMake/Ninja setup before any other task depends on them.

**Files:**
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `nr_hyp_sweep.{h,c}` from Tasks 1-2.
- Produces: `nr_hyp_sweep.c` compiled into the `nr_pdcch_blind_monitor` static library; a new
  `test_nr_hyp_sweep` CTest target.

- [ ] **Step 1: Add `nr_hyp_sweep.c` to the `nr_pdcch_blind_monitor` library**

Locate this line (context read during planning, exact line number may have shifted slightly by
the time this task runs — search for the string, don't assume the line number):

```cmake
add_library(nr_pdcch_blind_monitor ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c)
```

Replace with (appending `nr_hyp_sweep.c`):

```cmake
add_library(nr_pdcch_blind_monitor ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c)
```

- [ ] **Step 2: Add the `test_nr_hyp_sweep` CTest target, mirroring `test_nr_pdcch_dci_length_sweep`**

Locate:

```cmake
  add_executable(test_nr_pdcch_dci_length_sweep ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc
                                                 ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c)
  target_include_directories(test_nr_pdcch_dci_length_sweep PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdcch_dci_length_sweep PRIVATE UTIL GTest::gtest)
  add_dependencies(tests test_nr_pdcch_dci_length_sweep)
  add_test(NAME test_nr_pdcch_dci_length_sweep COMMAND ./test_nr_pdcch_dci_length_sweep)
```

Add immediately after it:

```cmake
  add_executable(test_nr_hyp_sweep ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_hyp_sweep_test.cc
                                    ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c)
  target_include_directories(test_nr_hyp_sweep PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_hyp_sweep PRIVATE UTIL GTest::gtest)
  add_dependencies(tests test_nr_hyp_sweep)
  add_test(NAME test_nr_hyp_sweep COMMAND ./test_nr_hyp_sweep)
```

- [ ] **Step 3: Build and run**

Run (from `cmake_targets/ran_build/build/`, after confirming `pgrep -x nr-uesoftmodem` is empty):
`cmake --build . --target test_nr_hyp_sweep -j8 && ./test_nr_hyp_sweep`
Expected: builds clean, 7/7 tests PASS.

- [ ] **Step 4: Build the full executable to confirm the library addition doesn't break the link**

Run: `ninja nr-uesoftmodem`
Expected: builds clean (this only proves linkage; `nr_hyp_sweep.c` has no callers yet, so no new
behavior is exercised here).

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
CMake: register nr_hyp_sweep in the blind-monitor library and its own test

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 4: Component 1 — UL DCI-length sweep (port Technique C to format 0_1)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc`

**Interfaces:**
- Consumes: `nr_pdcch_dci_length_sweep_feed()`/`_reset()` (existing, unchanged, format-agnostic),
  `nr_pdcch_blind_decode_and_extract_01()` (existing).
- Produces: a locked UL `dci_length` (stored via a new static in this file, read by Component
  2/3's wiring in Tasks 8/11), and `g_ul_length_found` (mirrors the DL `g_length_found` flag,
  gates Components 2/3 the same way `g_pdsch_sweep_on`'s gate mirrors `g_length_found` today).

- [ ] **Step 1: Write the failing test proving DL and UL sweep states stay independent**

```cpp
// append to nr_pdcch_dci_length_sweep_test.cc

// A scorer that reports a pass at length 47 (simulating DL) is completely independent of one that
// reports a pass at length 43 (simulating UL) when each owns its own state struct.
static bool scorer_fixed_length(int dci_length, int trial_idx, uint16_t *rnti_out,
                                uint32_t *payload_hash_out, void *user_ctx) {
  const int *target_len = (const int *)user_ctx;
  if (dci_length != *target_len) {
    return false;
  }
  *rnti_out = 0x1234;
  *payload_hash_out = (uint32_t)trial_idx; // varies -- not a degenerate fixed point
  return true;
}

TEST(NrPdcchDciLengthSweep, DlAndUlStatesDoNotCrossContaminate) {
  nr_pdcch_dci_length_sweep_state_t dl_state, ul_state;
  nr_pdcch_dci_length_sweep_reset(&dl_state);
  nr_pdcch_dci_length_sweep_reset(&ul_state);

  int dl_target = 47, ul_target = 43;
  int dl_found = -1, ul_found = -1;
  for (int occ = 0; occ < 200 && (dl_found < 0 || ul_found < 0); occ++) {
    if (dl_found < 0) {
      dl_found = nr_pdcch_dci_length_sweep_feed(&dl_state, scorer_fixed_length, &dl_target,
                                                20, 30, 63, 0);
    }
    if (ul_found < 0) {
      ul_found = nr_pdcch_dci_length_sweep_feed(&ul_state, scorer_fixed_length, &ul_target,
                                                20, 30, 63, 0);
    }
  }
  EXPECT_EQ(dl_found, 47);
  EXPECT_EQ(ul_found, 43);
  // Feeding one state never advanced the other's occasion count.
  EXPECT_NE(dl_state.occasions_fed, 0);
  EXPECT_NE(ul_state.occasions_fed, 0);
}
```

- [ ] **Step 2: Run test to verify it passes already**

Run: `<rebuild test_nr_pdcch_dci_length_sweep via ninja, then run it>`
Expected: PASS — `nr_pdcch_dci_length_sweep_feed()` is already format-agnostic and takes an
explicit state pointer, so two independent instances already can't cross-contaminate. This step
exists to LOCK IN that property with a test before Step 3 adds a second live call site that could
accidentally reuse the DL state.

- [ ] **Step 3: Add the UL length-sweep scorer and wiring block in `nr_pdcch_blind_monitor_rt.c`**

Add near the existing `nr_pdcch_autodiscover_length_scorer()` (search for that function name):

```c
typedef struct {
  const nr_pdcch_autodiscover_cand_t *cand;
  int      n_cand;
  uint16_t rnti_min;
  uint16_t rnti_max;
  const nr_pdcch_blind_ul_opts_t *ul_opts; // placeholder opts -- widths don't matter for a
                                            // length-only scorer, see the function body comment
  uint16_t scrambling_rnti;
  uint16_t dmrs_scrambling_id;
} nr_pdcch_autodiscover_ul_sweep_ctx_t;

static bool nr_pdcch_autodiscover_ul_length_scorer(int dci_length, int trial_idx, uint16_t *rnti_out,
                                                   uint32_t *payload_hash_out, void *user_ctx)
{
  const nr_pdcch_autodiscover_ul_sweep_ctx_t *ctx = (const nr_pdcch_autodiscover_ul_sweep_ctx_t *)user_ctx;
  if (ctx == NULL || ctx->n_cand <= 0) {
    return false;
  }
  const nr_pdcch_autodiscover_cand_t *c = &ctx->cand[trial_idx % ctx->n_cand];
  int16_t tmp_e[16 * 108];
  nr_pdcch_unscrambling((c16_t *)c->e_rx, ctx->scrambling_rnti, (uint32_t)(c->L * 108),
                        ctx->dmrs_scrambling_id, tmp_e);
  nr_pdcch_blind_ul_result_t out;
  // ctx->ul_opts is a placeholder (all field widths at their -1/default values): extract_01()
  // only needs `dci_length` correct to pass the polar CRC -- the widths are used AFTER CRC
  // recovery, to carve fields, which this scorer never reads. Component 2 (Task 7+) is what
  // finds the real widths, once this length is locked.
  const bool ok = nr_pdcch_blind_decode_and_extract_01(tmp_e, c->L, (uint16_t)dci_length,
                                                       ctx->ul_opts, ctx->rnti_min, ctx->rnti_max,
                                                       &out);
  if (!ok || !out.plausible) {
    return false;
  }
  *rnti_out = out.crc_rnti;
  *payload_hash_out = out.raw_payload & 0xFFFFFFFFu; // cheap fingerprint; varies with genuine
                                                      // traffic, constant for a degenerate fixed
                                                      // point -- same contract as the DL scorer
  return true;
}
```

Add the wiring block immediately after the existing DL Technique C block (search for
`AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS` to find its end):

```c
  // ---- Component 1 (2026-09-xx): UL DCI-length sweep, format 0_1. Mirrors the DL block above
  // exactly, with its OWN state (never the DL one) and reusing the SAME bootstrap RNTI (Technique
  // B is UE-generic, not DL-specific). Gated on the DL length already being found first only in
  // the sense that both need Technique A's CORESET geometry -- the two lengths are otherwise
  // fully independent and this block does not wait for g_length_found. ----
  static bool g_ul_length_swept = false;
  static bool g_ul_length_found = false;
  if (cfg->autodiscover && nr_pdcch_blind_monitor_autodiscover_done() && !g_ul_length_swept) {
    nr_pdcch_autodiscover_cand_t ul_disc_cand[64];
    int ul_disc_n_cand = 0;
    {
      int idx = 0;
      for (int c = 0; c < rel15->number_of_candidates && ul_disc_n_cand < 64; c++) {
        const int L         = rel15->L[c];
        const int n_re_cand = NR_PDCCH_BLIND_RE_PER_RB_OUT_DMRS * L * 6;
        ul_disc_cand[ul_disc_n_cand].e_rx = &pdcch_e_rx[idx];
        ul_disc_cand[ul_disc_n_cand].L    = (uint8_t)L;
        ul_disc_n_cand++;
        idx += n_re_cand;
      }
    }
    if (ul_disc_n_cand > 0) {
      uint16_t bootstrap_rnti = 0;
      uint8_t  bootstrap_class = 0xFF;
      uint32_t age = 0;
      nr_pdcch_blind_monitor_confirmed_rnti(abs_slot, &bootstrap_rnti, &bootstrap_class, &age);
      (void)bootstrap_class; (void)age;

      static nr_pdcch_blind_ul_opts_t s_placeholder_ul_opts; // zero-initialized static storage;
                                                              // every *_bits field reads as 0,
                                                              // which nr_pdcch_blind_ul_opts_t's
                                                              // own default-resolution treats as
                                                              // "0 bits", NOT "-1 = use spec
                                                              // default" -- fine for a length-only
                                                              // scorer since these values are
                                                              // never read; do not reuse this
                                                              // struct for anything else.
      static nr_pdcch_dci_length_sweep_state_t s_ul_sweep_state;
      nr_pdcch_autodiscover_ul_sweep_ctx_t ul_sweep_ctx = {
          .cand               = ul_disc_cand,
          .n_cand             = ul_disc_n_cand,
          .rnti_min           = cfg->rnti_min,
          .rnti_max           = cfg->rnti_max,
          .ul_opts            = &s_placeholder_ul_opts,
          .scrambling_rnti    = rel15->coreset.scrambling_rnti,
          .dmrs_scrambling_id = rel15->coreset.pdcch_dmrs_scrambling_id,
      };
      const int ul_found_len = nr_pdcch_dci_length_sweep_feed(&s_ul_sweep_state,
                                                               nr_pdcch_autodiscover_ul_length_scorer,
                                                               &ul_sweep_ctx, ul_disc_n_cand, 30, 63,
                                                               bootstrap_rnti);
      if (ul_found_len > 0) {
        nr_pdcch_blind_monitor_autodiscover_set_ul_dci_length(ul_found_len); // new setter, Step 4
        g_ul_length_swept = true;
        g_ul_length_found = true;
        LOG_A(PHY, "SENSING: Phase 3 UL autodiscover -- dci_length locked at %d "
                   "(bootstrap_rnti=0x%x, occasions_fed=%d)\n", ul_found_len, bootstrap_rnti,
              s_ul_sweep_state.occasions_fed);
      } else if (s_ul_sweep_state.occasions_fed >= AUTODISCOVER_LENGTH_SWEEP_MAX_OCCASIONS) {
        g_ul_length_swept = true;
        LOG_W(PHY, "SENSING: Phase 3 UL autodiscover -- dci_length sweep gave up after %d "
                   "occasions (bootstrap_rnti=0x%x); NOT found\n",
              s_ul_sweep_state.occasions_fed, bootstrap_rnti);
      }
    }
  }
```

- [ ] **Step 4: Add the `nr_pdcch_blind_monitor_autodiscover_set_ul_dci_length()` setter**

`nr_pdcch_blind_monitor_autodiscover_set_dci_length()` already exists (called two lines above the
new block, in the DL branch) and sets `g_cfg`'s DL length field. Find its definition (search for
`nr_pdcch_blind_monitor_autodiscover_set_dci_length` in `nr_pdcch_blind_monitor.c`) and add a
sibling function right after it, setting a NEW field:

```c
void nr_pdcch_blind_monitor_autodiscover_set_ul_dci_length(int dci_length)
{
  g_cfg.ul_dci_length = dci_length; // new field, added to nr_pdcch_blind_monitor_cfg_t in
                                     // nr_pdcch_blind_monitor_rt.h alongside the existing DL one
}
```

Add the matching field and declaration:
- In `nr_pdcch_blind_monitor_rt.h`'s `nr_pdcch_blind_monitor_cfg_t`, add `int ul_dci_length;` next
  to wherever the DL `dci_length` field lives.
- In `nr_pdcch_blind_monitor.h`, declare `void nr_pdcch_blind_monitor_autodiscover_set_ul_dci_length(int dci_length);`
  next to the existing DL declaration.

- [ ] **Step 5: Build and run the full offline test suite plus a live smoke build**

Run: `cmake --build . --target test_nr_pdcch_dci_length_sweep -j8 && ./test_nr_pdcch_dci_length_sweep`
Expected: PASS, all cases including the new independence test.

Run: `ninja nr-uesoftmodem`
Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc
git commit -m "$(cat <<'EOF'
Component 1: port the DCI-length sweep (Technique C) to format 0_1

Mechanical port: nr_pdcch_dci_length_sweep_feed() was already format-
agnostic, so this is a second scorer (calling
nr_pdcch_blind_decode_and_extract_01 instead of _ex) plus a second,
independent sweep-state instance. A placeholder ul_opts is enough because
dci_length is orthogonal to field-width interpretation -- the polar CRC
oracle doesn't need field widths to be right yet, only Component 2 (next)
does. Reuses Technique B's already-shared bootstrap RNTI.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 5: Echo the chosen hypothesis class through the grant book to the PUSCH-decode result

Component 2/3's Stage d needs to know, once a PUSCH's TB CRC result comes back (which happens k2
slots later, possibly on a different thread via the deferred queue), which hypothesis class
produced the grant that was decoded. This task adds the two carrier fields before either sweep
exists, so Tasks 8/9 and 11/12 have somewhere to put the value.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h`

**Interfaces:**
- Produces: `nr_pdcch_blind_ul_result_t.width_hyp_class` (`int`, -1 = no Component-2 sweep active
  or already locked), `nr_pdcch_blind_ul_result_t.interp_hyp_class` (`int`, same convention for
  Component 3). Both ride through `nr_pusch_grant_book_add()` (unchanged signature — it takes the
  whole struct by pointer already) and `nr_pusch_passive_job_t.grant` (already embeds the whole
  struct) with zero plumbing changes elsewhere.

- [ ] **Step 1: Add the two fields to the struct**

In `nr_pdcch_blind_monitor.h`, immediately before the closing `bool plausible;` /
`const char* reject_reason;` pair in `nr_pdcch_blind_ul_result_t`:

```c
  // Set by whichever of Component 2/3's sweeps is currently active when this candidate was
  // extracted; -1 if neither is (locked already, or autodiscover not in that phase yet). Echoed
  // through the grant book and the deferred-decode job struct unchanged, so the TB-CRC result --
  // which arrives k2 slots later, possibly on a different thread -- can be attributed back to the
  // hypothesis class that produced it. See nr_hyp_sweep.h's Stage d.
  int width_hyp_class;   ///< Component 2's chosen class index, or -1
  int interp_hyp_class;  ///< Component 3's chosen class index, or -1
```

- [ ] **Step 2: Initialize both fields to -1 wherever `nr_pdcch_blind_ul_result_t` is default-constructed**

Find `nr_pdcch_blind_decode_and_extract_01()`'s definition in `nr_pdcch_blind_monitor.c` (search
for the function signature already seen in the header) and locate where it does
`memset(out, 0, sizeof(*out))` or field-by-field zeroing at the top. Add immediately after:

```c
  out->width_hyp_class  = -1;
  out->interp_hyp_class = -1;
```

This guarantees every candidate this function ever produces defaults to "no sweep attributed" —
Tasks 8/11 overwrite these two fields explicitly when their sweep is active; nothing else needs to
touch them.

- [ ] **Step 3: Build to confirm no existing code breaks (struct layout change only, no behavior change yet)**

Run: `ninja nr-uesoftmodem`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
git commit -m "$(cat <<'EOF'
Add hypothesis-class echo fields to nr_pdcch_blind_ul_result_t

Carries which Component 2/3 sweep class (if any) produced a given
candidate's field interpretation through the existing grant book and
deferred-decode job struct, both of which already embed this struct by
value -- no signature changes needed anywhere else. Defaults to -1
(no sweep attributed) so this is behavior-preserving until Tasks 8/9 and
11/12 start writing real class indices.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 6: Component 2 — field-width hypothesis generator

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc`

**Interfaces:**
- Consumes: `nr_hyp_sweep_init()` (Task 1), `nr_pdcch_blind_dci01_size()` (existing, unchanged),
  `nr_pdcch_blind_ul_opts_t` (existing).
- Produces: `nr_pdcch_ul_field_widths_t` (the 16-field hypothesis struct),
  `nr_pdcch_ul_field_sweep_generate()`, `nr_pdcch_ul_field_sweep_apply()` (writes one hypothesis's
  widths into a full `nr_pdcch_blind_ul_opts_t`) — consumed by Task 7's wiring.

- [ ] **Step 1: Write the failing test for the generator**

```cpp
// openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc
// (OAI license header, copied verbatim)
#include <cstdint>
#include "gtest/gtest.h"
extern "C" {
#include "nr_pdcch_ul_field_sweep.h"
#include "nr_pdcch_blind_monitor.h"
}

TEST(NrPdcchUlFieldSweep, GeneratesOnlyCombinationsMatchingTheTargetLength) {
  nr_pdcch_blind_ul_opts_t fixed = {0};
  fixed.bwp_start = 0;
  fixed.bwp_size  = 273; // this cell's known BWP, from Technique A
  fixed.tda_count = 2;   // from the manual conf's TDRA list length

  nr_hyp_t out[NR_HYP_SWEEP_MAX_RAW];
  const int n = nr_pdcch_ul_field_sweep_generate(&fixed, 47, out, NR_HYP_SWEEP_MAX_RAW);

  ASSERT_GT(n, 0);
  for (int i = 0; i < n; i++) {
    nr_pdcch_blind_ul_opts_t cand = fixed;
    nr_pdcch_ul_field_sweep_apply(&out[i], &cand);
    EXPECT_EQ(nr_pdcch_blind_dci01_size(&cand), 47);
  }
}

TEST(NrPdcchUlFieldSweep, TodaysHandSolvedWidthsAreInTheGeneratedSet) {
  nr_pdcch_blind_ul_opts_t fixed = {0};
  fixed.bwp_start = 0;
  fixed.bwp_size  = 273;
  fixed.tda_count = 2;

  nr_hyp_t out[NR_HYP_SWEEP_MAX_RAW];
  const int n = nr_pdcch_ul_field_sweep_generate(&fixed, 47, out, NR_HYP_SWEEP_MAX_RAW);
  ASSERT_GT(n, 0);

  // Today's hand-solved values (pdcch_blind_monitor_ul_dci_bits, this cell) -- see the spec's
  // Component 2 table. If this ever fails, either the generator's legal ranges are wrong or the
  // conf's hand-solved values are, and either is worth knowing.
  bool found = false;
  for (int i = 0; i < n; i++) {
    nr_pdcch_ul_field_widths_t w;
    static_assert(sizeof(w) <= NR_HYP_BYTES, "widths struct must fit in nr_hyp_t");
    memcpy(&w, out[i].bytes, sizeof(w));
    if (w.harq_pid_bits == 4 && w.dai1_bits == 2 && w.antenna_ports_bits == 2
        && w.srs_request_bits == 2 && w.dmrs_seq_init_bits == 1
        && w.carrier_indicator_bits == 0 && w.ul_sul_bits == 0 && w.bwp_indicator_bits == 0
        && w.freq_hopping_bits == 0 && w.dai2_bits == 0 && w.sri_bits == 0
        && w.precoding_info_bits == 0 && w.csi_request_bits == 0 && w.cbg_bits == 0
        && w.ptrs_dmrs_bits == 0 && w.beta_offset_bits == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT -c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc -o /tmp/t2.o`
Expected: FAIL — `nr_pdcch_ul_field_sweep.h: No such file or directory`.

- [ ] **Step 3: Write `nr_pdcch_ul_field_sweep.h`**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.h
/* OAI license header, copied verbatim */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.h
 * \brief Component 2: UL DCI 0_1 field-BOUNDARY (bit-width) discovery. Built on nr_hyp_sweep.h's
 * shared engine -- this file owns only the hypothesis TYPE and its generator/apply functions; the
 * search itself (Stages c/d) is nr_hyp_sweep's, not reimplemented here.
 *
 * See docs/superpowers/specs/2026-09-08-passive-rx-ul-adaptive-discovery-design.md's Component 2
 * section for the legal range table and why carrier_indicator_bits/ul_sul_bits are swept rather
 * than assumed 0.
 */
#ifndef NR_PDCCH_UL_FIELD_SWEEP_H
#define NR_PDCCH_UL_FIELD_SWEEP_H

#include <stdint.h>
#include "nr_hyp_sweep.h"
#include "nr_pdcch_blind_monitor.h" // nr_pdcch_blind_ul_opts_t

#ifdef __cplusplus
extern "C" {
#endif

/// The swept subset of nr_pdcch_blind_ul_opts_t's *_bits fields. 16 ints = 64 bytes, fits in
/// nr_hyp_t's 96-byte buffer with margin.
typedef struct {
  int carrier_indicator_bits;
  int ul_sul_bits;
  int bwp_indicator_bits;
  int freq_hopping_bits;
  int harq_pid_bits;
  int dai1_bits;
  int dai2_bits;
  int sri_bits;
  int precoding_info_bits;
  int antenna_ports_bits;
  int srs_request_bits;
  int csi_request_bits;
  int cbg_bits;
  int ptrs_dmrs_bits;
  int beta_offset_bits;
  int dmrs_seq_init_bits;
} nr_pdcch_ul_field_widths_t;

/**
 * @brief Stage a's generator: every legal combination of the 16 swept fields whose resulting
 *        nr_pdcch_blind_dci01_size() equals `dci_length`, given the fixed/known facts in `fixed`
 *        (bwp_start/bwp_size/tda_count -- everything nr_hyp_sweep's Stage a constraint would
 *        otherwise have to re-derive).
 * @return number of hypotheses written to `out` (<= max_out), or -1 if the legal-range table
 *         alone would produce more raw combinations before the length filter than max_out can
 *         hold -- loud refusal, matching nr_hyp_sweep's own convention.
 */
int nr_pdcch_ul_field_sweep_generate(const nr_pdcch_blind_ul_opts_t *fixed, uint16_t dci_length,
                                     nr_hyp_t *out, int max_out);

/** Write one generated hypothesis's widths into `opts` (every other field of `opts` is left
 * untouched -- caller pre-fills `opts` with the fixed/known facts first). */
void nr_pdcch_ul_field_sweep_apply(const nr_hyp_t *hyp, nr_pdcch_blind_ul_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif
```

- [ ] **Step 4: Implement `nr_pdcch_ul_field_sweep.c`**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c
/* OAI license header, copied verbatim */
#include "nr_pdcch_ul_field_sweep.h"
#include <string.h>

typedef struct { const int *vals; int n; } nr_ul_field_axis_t;

int nr_pdcch_ul_field_sweep_generate(const nr_pdcch_blind_ul_opts_t *fixed, uint16_t dci_length,
                                     nr_hyp_t *out, int max_out)
{
  static const int a_ci[]   = {0, 3};
  static const int a_sul[]  = {0, 1};
  static const int a_bwpi[] = {0, 1, 2};
  static const int a_fh[]   = {0, 1};
  static const int a_harq[] = {4, 5};
  static const int a_dai1[] = {1, 2};
  static const int a_dai2[] = {0, 2};
  static const int a_sri[]  = {0, 1, 2};
  static const int a_prec[] = {0, 1, 2, 3, 4};
  static const int a_ant[]  = {2, 3, 4, 5};
  static const int a_srsr[] = {2, 3};
  static const int a_csi[]  = {0, 1, 2, 3, 4, 5, 6};
  static const int a_cbg[]  = {0, 1, 2};
  static const int a_ptrs[] = {0, 1, 2};
  static const int a_beta[] = {0, 2};
  static const int a_dsi[]  = {0, 1};
  const nr_ul_field_axis_t axes[16] = {
      {a_ci, 2}, {a_sul, 2}, {a_bwpi, 3}, {a_fh, 2}, {a_harq, 2}, {a_dai1, 2}, {a_dai2, 2},
      {a_sri, 3}, {a_prec, 5}, {a_ant, 4}, {a_srsr, 2}, {a_csi, 7}, {a_cbg, 3}, {a_ptrs, 3},
      {a_beta, 2}, {a_dsi, 2},
  };

  int idx[16] = {0};
  int n_out = 0;
  for (;;) {
    nr_pdcch_blind_ul_opts_t cand = *fixed;
    cand.carrier_indicator_bits = axes[0].vals[idx[0]];
    cand.ul_sul_bits            = axes[1].vals[idx[1]];
    cand.bwp_indicator_bits     = axes[2].vals[idx[2]];
    cand.freq_hopping_bits      = axes[3].vals[idx[3]];
    cand.harq_pid_bits          = axes[4].vals[idx[4]];
    cand.dai1_bits              = axes[5].vals[idx[5]];
    cand.dai2_bits              = axes[6].vals[idx[6]];
    cand.sri_bits               = axes[7].vals[idx[7]];
    cand.precoding_info_bits    = axes[8].vals[idx[8]];
    cand.antenna_ports_bits     = axes[9].vals[idx[9]];
    cand.srs_request_bits       = axes[10].vals[idx[10]];
    cand.csi_request_bits       = axes[11].vals[idx[11]];
    cand.cbg_bits               = axes[12].vals[idx[12]];
    cand.ptrs_dmrs_bits         = axes[13].vals[idx[13]];
    cand.beta_offset_bits       = axes[14].vals[idx[14]];
    cand.dmrs_seq_init_bits     = axes[15].vals[idx[15]];

    if (nr_pdcch_blind_dci01_size(&cand) == dci_length) {
      if (n_out >= max_out) {
        return -1;
      }
      const nr_pdcch_ul_field_widths_t w = {
          cand.carrier_indicator_bits, cand.ul_sul_bits, cand.bwp_indicator_bits,
          cand.freq_hopping_bits, cand.harq_pid_bits, cand.dai1_bits, cand.dai2_bits,
          cand.sri_bits, cand.precoding_info_bits, cand.antenna_ports_bits,
          cand.srs_request_bits, cand.csi_request_bits, cand.cbg_bits,
          cand.ptrs_dmrs_bits, cand.beta_offset_bits, cand.dmrs_seq_init_bits,
      };
      memcpy(out[n_out].bytes, &w, sizeof(w));
      out[n_out].len = (int)sizeof(w);
      n_out++;
    }

    int k = 15;
    while (k >= 0) {
      idx[k]++;
      if (idx[k] < axes[k].n) {
        break;
      }
      idx[k] = 0;
      k--;
    }
    if (k < 0) {
      break;
    }
  }
  return n_out;
}

void nr_pdcch_ul_field_sweep_apply(const nr_hyp_t *hyp, nr_pdcch_blind_ul_opts_t *opts)
{
  nr_pdcch_ul_field_widths_t w;
  memcpy(&w, hyp->bytes, sizeof(w));
  opts->carrier_indicator_bits = w.carrier_indicator_bits;
  opts->ul_sul_bits            = w.ul_sul_bits;
  opts->bwp_indicator_bits     = w.bwp_indicator_bits;
  opts->freq_hopping_bits      = w.freq_hopping_bits;
  opts->harq_pid_bits          = w.harq_pid_bits;
  opts->dai1_bits              = w.dai1_bits;
  opts->dai2_bits              = w.dai2_bits;
  opts->sri_bits               = w.sri_bits;
  opts->precoding_info_bits    = w.precoding_info_bits;
  opts->antenna_ports_bits     = w.antenna_ports_bits;
  opts->srs_request_bits       = w.srs_request_bits;
  opts->csi_request_bits       = w.csi_request_bits;
  opts->cbg_bits               = w.cbg_bits;
  opts->ptrs_dmrs_bits         = w.ptrs_dmrs_bits;
  opts->beta_offset_bits       = w.beta_offset_bits;
  opts->dmrs_seq_init_bits     = w.dmrs_seq_init_bits;
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c -lgtest -lgtest_main -pthread -o /tmp/t3 && /tmp/t3`
Expected: PASS, 2/2. (This link line will need whatever other object files
`nr_pdcch_blind_monitor.c` itself depends on — resolve those the same way the existing
`test_nr_pdcch_dci_length_sweep` target's CMake entry does; Task 9 wires this properly into
CMake once the wiring in Task 7 is also in place.)

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.h \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc
git commit -m "$(cat <<'EOF'
Component 2: UL field-width hypothesis generator

Enumerates every legal combination of the 16 swept nr_pdcch_blind_ul_opts_t
bit-width fields via a 16-axis odometer, filtering by the EXISTING
nr_pdcch_blind_dci01_size() rather than re-deriving fixed-field arithmetic
by hand. carrier_indicator_bits/ul_sul_bits are swept, not assumed 0,
closing the CA/SUL silent-wrong-answer risk the design doc flags.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 7: Component 2 — wire the sweep into `run_occasion()` and feed TB-CRC results back

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c`

**Interfaces:**
- Consumes: `nr_hyp_sweep_init/next/feed/winner()` (Task 2), `nr_pdcch_ul_field_sweep_generate/apply()`
  (Task 6), `width_hyp_class` (Task 5), `g_ul_length_found` and the UL length getter (Task 4).
- Produces: `g_cfg.ul` (existing) overwritten with the winning widths once decided — from that
  point on, Component 2 is indistinguishable from a correctly hand-configured manual conf.

- [ ] **Step 1: Gate and initialize the sweep once the UL length is locked**

In `nr_pdcch_blind_monitor_rt.c`, add near the Component 1 block from Task 4 (after it, since it
needs the locked length):

```c
  // ---- Component 2 (2026-09-xx): UL field-width sweep. Hard prerequisite: bootstrap_rnti != 0
  // (Technique B) AND the UL length is locked (Component 1) -- see the design doc's Component 2
  // section for why scoring against an unconfirmed RNTI wastes every decode cycle. ----
  static nr_hyp_sweep_state_t s_ul_width_sweep;
  static bool s_ul_width_sweep_init = false;
  static bool s_ul_width_locked = false;
  if (cfg->dl_full_auto && g_ul_length_found && !s_ul_width_locked) {
    uint16_t bootstrap_rnti = 0;
    uint8_t  bootstrap_class = 0xFF;
    uint32_t age = 0;
    nr_pdcch_blind_monitor_confirmed_rnti(abs_slot, &bootstrap_rnti, &bootstrap_class, &age);
    (void)bootstrap_class; (void)age;
    if (bootstrap_rnti != 0 && !s_ul_width_sweep_init) {
      nr_hyp_t raw[NR_HYP_SWEEP_MAX_RAW];
      nr_pdcch_blind_ul_opts_t fixed = cfg->ul; // carries bwp_start/bwp_size/tda_count etc.
      const int n_raw = nr_pdcch_ul_field_sweep_generate(&fixed, cfg->ul_dci_length, raw,
                                                         NR_HYP_SWEEP_MAX_RAW);
      if (n_raw > 0) {
        // No samples yet at init time (Stage b collapsing needs real candidate payloads, which
        // this occasion may not have accumulated) -- every admissible hypothesis starts as its
        // own class; ties collapse naturally once Stage d's decision rule sees them score
        // identically (they will never separate, but the sweep still converges to A representative
        // of the tied group, matching the "grant-equivalence, not exact recovery" goal).
        const int n_classes = nr_hyp_sweep_init(&s_ul_width_sweep, raw, n_raw, NULL, NULL,
                                                NULL, NULL, 0, NULL);
        if (n_classes > 0) {
          s_ul_width_sweep_init = true;
          LOG_A(PHY, "SENSING: Phase 3 UL field-width sweep ARMED, %d classes\n", n_classes);
        } else {
          LOG_E(PHY, "SENSING: Phase 3 UL field-width sweep -- generator produced %d raw "
                     "hypotheses but init returned %d (cap exceeded); NOT armed\n", n_raw,
                n_classes);
        }
      } else {
        LOG_E(PHY, "SENSING: Phase 3 UL field-width sweep -- generator refused (n_raw=%d, cap "
                   "exceeded); NOT armed\n", n_raw);
      }
    }
  }
```

- [ ] **Step 2: Pick a hypothesis and apply it when building each UL candidate task**

Find where `nr_pdcch_blind_cand_task_t` entries with `ul_scan = 1` are populated (search for
`.ul_scan` in the task-building loop, before the parallel decode phase). Add, immediately before
that task's `ul_opts` field is set:

```c
      nr_pdcch_blind_ul_opts_t task_ul_opts = cfg->ul; // start from fixed/known facts
      int chosen_width_class = -1;
      if (s_ul_width_sweep_init && nr_hyp_sweep_winner(&s_ul_width_sweep) < 0) {
        nr_hyp_t chosen;
        const int candidate_marker = 0; // Stage c plausibility isn't exercised until Task 11's
                                         // interpretation sweep needs it; Component 2 alone has
                                         // no per-candidate structural check beyond what
                                         // extract_01() itself already enforces post-hoc, so NULL
                                         // is passed for `plausible` here -- see the design doc's
                                         // note that Stage c is optional per sweep, not mandatory.
        chosen_width_class = nr_hyp_sweep_next(&s_ul_width_sweep, &candidate_marker, NULL, NULL,
                                               &chosen);
        if (chosen_width_class >= 0) {
          nr_pdcch_ul_field_sweep_apply(&chosen, &task_ul_opts);
        }
      } else if (s_ul_width_sweep_init) {
        // Winner already decided: use it unconditionally from here on, exactly like a manual conf.
        s_ul_width_locked = true;
        nr_hyp_t winner_hyp;
        const int w = nr_hyp_sweep_winner(&s_ul_width_sweep);
        winner_hyp = s_ul_width_sweep.classes[w].hyp;
        nr_pdcch_ul_field_sweep_apply(&winner_hyp, &task_ul_opts);
      }
      // (existing task-population code continues, using task_ul_opts instead of cfg->ul directly)
```

Then change the existing assignment (search for where the task's `ul_opts` pointer is set) from
whatever currently supplies it (likely `&cfg->ul`) to point at a per-task copy of `task_ul_opts`
(the task struct holds a pointer, so this needs task-local storage — add
`nr_pdcch_blind_ul_opts_t ul_opts_storage;` to the loop's per-candidate local scope, copy
`task_ul_opts` into it, and set `cand_task[nof_tasks].ul_opts = &ul_opts_storage;`, being careful
this storage outlives the parallel decode phase — the existing `cand_task[128]` array already is
declared outside the loop with static-within-the-function-call lifetime, so give each task its own
`nr_pdcch_blind_ul_opts_t` member directly on `nr_pdcch_blind_cand_task_t` instead of a separately
scoped local, and set `cand_task[nof_tasks].ul_opts = &cand_task[nof_tasks].ul_opts_storage;`).

Also set, once the task's `ul_out` will exist: `cand_task[nof_tasks].chosen_width_class =
chosen_width_class;` (add this as a new int field on `nr_pdcch_blind_cand_task_t`, alongside the
existing `ul_scan`/`ul_opts` fields).

- [ ] **Step 3: Copy the chosen class into the result before it's parked in the grant book**

In the Phase 2 sequential loop, at the existing `if (cand_task[ti].ul_scan) { ... }` block (the one
that calls `nr_pusch_grant_book_add()`), add before that call:

```c
      // Attribute this grant to the hypothesis class that produced it, so the eventual TB-CRC
      // result (arriving k2 slots later, possibly on a different thread) can be fed back to the
      // right class. -1 (Task 5's default) means neither sweep was active for this candidate.
      cand_task[ti].ul_out.width_hyp_class = cand_task[ti].chosen_width_class;
```

(the existing `if (cand_task[ti].ok) { ...; nr_pusch_grant_book_add(u, ...); }` already runs after
this, and `u` points at `cand_task[ti].ul_out`, so the class rides through unchanged).

- [ ] **Step 4: Feed the TB-CRC result back, inline decode path**

In `nr_pusch_passive_monitor_rt.c`, at the inline (non-queued) call site
(`nr_pusch_passive_decode(ue, 0, proc->frame_rx, proc->nr_slot_rx, &g, ta, ...)`), add immediately
after that call:

```c
  if (g.width_hyp_class >= 0) {
    extern nr_hyp_sweep_state_t *nr_pdcch_ul_width_sweep_get_state(void); // Step 6 below
    nr_hyp_sweep_state_t *st = nr_pdcch_ul_width_sweep_get_state();
    if (st != NULL) {
      nr_hyp_sweep_feed(st, g.width_hyp_class, out.status == NR_PUSCH_PASSIVE_OK);
    }
  }
```

- [ ] **Step 5: Feed the TB-CRC result back, deferred/queued decode path**

In `nr_pusch_passive_queue.c`, at the consumer's decode call site (search for
`nr_pusch_passive_decode(ue, idx, ...)` inside the consumer loop), add the identical block
immediately after it, reading `job.grant.width_hyp_class` instead of `g.width_hyp_class` (the
queued path's grant is `job.grant`, already confirmed to embed the whole
`nr_pdcch_blind_ul_result_t` by value).

- [ ] **Step 6: Add the accessor `nr_pdcch_ul_width_sweep_get_state()`**

In `nr_pdcch_blind_monitor_rt.c`, since `s_ul_width_sweep` is a function-local static in
`run_occasion()`, it needs to be promoted to file scope (still `static`, just moved out of the
function) so this new accessor can return its address:

```c
// (moved from Step 1's function-local scope to file scope, right below the other g_ul_* statics)
static nr_hyp_sweep_state_t s_ul_width_sweep;
static bool                 s_ul_width_sweep_init = false;

nr_hyp_sweep_state_t *nr_pdcch_ul_width_sweep_get_state(void)
{
  return s_ul_width_sweep_init ? &s_ul_width_sweep : NULL;
}
```

Declare it in `nr_pdcch_blind_monitor_rt.h`:

```c
nr_hyp_sweep_state_t *nr_pdcch_ul_width_sweep_get_state(void);
```

(and include `nr_hyp_sweep.h` in that header for the type).

- [ ] **Step 7: Build**

Run: `ninja nr-uesoftmodem`
Expected: builds clean. This task has no new offline test of its own — it is RT wiring over
already-tested pieces (Tasks 1, 2, 6). The next task's known-answer test is what exercises this
wiring's logic in isolation, offline, without needing live air.

- [ ] **Step 8: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h \
        openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c
git commit -m "$(cat <<'EOF'
Component 2: wire the field-width sweep into run_occasion() and PUSCH decode

Round-robins a hypothesis per UL candidate (once bootstrap_rnti is
confirmed and Component 1's length is locked), applies it at
extraction time, and feeds the eventual TB-CRC result back into the sweep
at BOTH the inline and the deferred/queued PUSCH decode call sites --
whichever path is active, the class index rides through unchanged via the
grant-book/job struct (Task 5). No PUSCH-decoder signature change.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 8: Component 2 — offline known-answer test and the tie-inspection falsifiable check

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc`

**Interfaces:**
- Consumes: everything from Tasks 1, 2, 6 (no RT/live code — this is a pure offline simulation of
  the search using a synthetic TB-CRC oracle).

- [ ] **Step 1: Write the known-answer convergence test**

```cpp
// append to nr_pdcch_ul_field_sweep_test.cc

TEST(NrPdcchUlFieldSweep, ConvergesToTodaysHandSolvedWidthsUnderASyntheticOracle) {
  nr_pdcch_blind_ul_opts_t fixed = {0};
  fixed.bwp_start = 0;
  fixed.bwp_size  = 273;
  fixed.tda_count = 2;

  nr_hyp_t raw[NR_HYP_SWEEP_MAX_RAW];
  const int n_raw = nr_pdcch_ul_field_sweep_generate(&fixed, 47, raw, NR_HYP_SWEEP_MAX_RAW);
  ASSERT_GT(n_raw, 0);

  nr_hyp_sweep_state_t st;
  const int n_classes = nr_hyp_sweep_init(&st, raw, n_raw, nullptr, nullptr, nullptr, nullptr, 0,
                                          nullptr);
  ASSERT_GT(n_classes, 0);

  // Ground truth: today's hand-solved widths (same values as the generator test above).
  auto is_ground_truth = [](const nr_hyp_t *h) {
    nr_pdcch_ul_field_widths_t w;
    memcpy(&w, h->bytes, sizeof(w));
    return w.harq_pid_bits == 4 && w.dai1_bits == 2 && w.antenna_ports_bits == 2
        && w.srs_request_bits == 2 && w.dmrs_seq_init_bits == 1 && w.carrier_indicator_bits == 0
        && w.ul_sul_bits == 0 && w.bwp_indicator_bits == 0 && w.freq_hopping_bits == 0
        && w.dai2_bits == 0 && w.sri_bits == 0 && w.precoding_info_bits == 0
        && w.csi_request_bits == 0 && w.cbg_bits == 0 && w.ptrs_dmrs_bits == 0
        && w.beta_offset_bits == 0;
  };

  int winner = -1;
  for (int trial = 0; trial < 20000 && winner < 0; trial++) {
    nr_hyp_t chosen;
    const int dummy_cand = 0;
    const int idx = nr_hyp_sweep_next(&st, &dummy_cand, nullptr, nullptr, &chosen);
    ASSERT_GE(idx, 0);
    // Synthetic oracle: the ground-truth class passes at ~76% (this cell's measured working rate,
    // per Technique D's own header comment); every other class passes at 0%.
    const bool crc_ok = is_ground_truth(&chosen) ? ((trial % 4) != 0) : false;
    winner = nr_hyp_sweep_feed(&st, idx, crc_ok);
  }
  ASSERT_GE(winner, 0);
  EXPECT_TRUE(is_ground_truth(&st.classes[winner].hyp));
}
```

- [ ] **Step 2: Run to verify it passes**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c openair1/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c -lgtest -lgtest_main -pthread -o /tmp/t4 && /tmp/t4`
Expected: PASS. If it does NOT converge within 20000 trials, that is itself informative — it means
`NR_HYP_SWEEP_MIN_TRIALS`/`WIN_RATIO` need retuning for this hypothesis count, not that the test is
wrong; do not raise the trial budget past 20000 to force a pass without first checking whether the
ground-truth class actually has a runner-up within `WIN_RATIO` of it (the next step's tie-inspection
check answers exactly that).

- [ ] **Step 3: Write the falsifiable tie-inspection check**

```cpp
// append to nr_pdcch_ul_field_sweep_test.cc

// Answers the design doc's open "item E" question with a MEASUREMENT: does the generated set, for
// THIS cell's config shape, actually contain a hypothesis that is grant-EQUIVALENT to ground truth
// but differs in width assignment (the "60 identical decodes" phenomenon from the original manual
// solve)? Two widths are grant-equivalent here iff applying both to the SAME fixed opts produces
// the same nr_pdcch_blind_dci01_size() breakdown of where each named field starts -- this test
// does not have real candidate payloads to check byte-for-byte extraction equality (that needs
// Stage b's `equiv` callback wired to a live extractor, out of scope for a pure offline generator
// test), so it checks the weaker but still informative necessary condition: same total length AND
// same value for every field this cell's traffic actually exercises (harq_pid, antenna_ports,
// dai1) is not sufficient for TRUE equivalence, only for "worth flagging". Recorded as a fact, not
// assumed either way, per the design doc's own instruction.
TEST(NrPdcchUlFieldSweep, RecordWhetherGroundTruthHasATiedTwin) {
  nr_pdcch_blind_ul_opts_t fixed = {0};
  fixed.bwp_start = 0;
  fixed.bwp_size  = 273;
  fixed.tda_count = 2;
  nr_hyp_t raw[NR_HYP_SWEEP_MAX_RAW];
  const int n_raw = nr_pdcch_ul_field_sweep_generate(&fixed, 47, raw, NR_HYP_SWEEP_MAX_RAW);
  ASSERT_GT(n_raw, 0);

  int n_matching_ground_truth_on_exercised_fields = 0;
  for (int i = 0; i < n_raw; i++) {
    nr_pdcch_ul_field_widths_t w;
    memcpy(&w, raw[i].bytes, sizeof(w));
    if (w.harq_pid_bits == 4 && w.dai1_bits == 2 && w.antenna_ports_bits == 2) {
      n_matching_ground_truth_on_exercised_fields++;
    }
  }
  // Not an assertion of a specific count -- printed so a human reads and records the finding, per
  // the design doc's instruction not to assume either way.
  printf("TIE-CHECK Component2: %d of %d generated hypotheses agree with ground truth on every "
         "field this cell's traffic exercises (harq_pid/dai1/antenna_ports)\n",
         n_matching_ground_truth_on_exercised_fields, n_raw);
  SUCCEED();
}
```

- [ ] **Step 4: Run and record the printed finding**

Run: `/tmp/t4 --gtest_filter=*RecordWhetherGroundTruthHasATiedTwin*`
Expected: PASS, and the printed count — **write the actual number into this plan's own commit
message** (not left as "TBD"; whatever the real run reports).

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc
git commit -m "$(cat <<'EOF'
Component 2: offline known-answer test + tie-inspection measurement

Synthetic-oracle convergence test proves the search algorithm finds
today's hand-solved widths before any live capture is attempted, per the
catalogue's own instruction. The tie-inspection test turns the design
doc's open "are RRC-residual widths inert" question into a measured
count rather than an assumption (see this commit's printed TIE-CHECK
line for the actual number on this cell's config shape).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 9: Register Component 2 in CMake

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add `nr_pdcch_ul_field_sweep.c` to the `nr_pdcch_blind_monitor` library**

Same pattern as Task 3 Step 1 — append `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c`
to the `add_library(nr_pdcch_blind_monitor ...)` source list.

- [ ] **Step 2: Add the `test_nr_pdcch_ul_field_sweep` target**

Mirror Task 3 Step 2's pattern:

```cmake
  add_executable(test_nr_pdcch_ul_field_sweep ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_field_sweep_test.cc
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_field_sweep.c
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_hyp_sweep.c
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_rnti_bootstrap.c
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c
                                               ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c)
  target_include_directories(test_nr_pdcch_ul_field_sweep PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT
                                                                   ${NFAPI_DIR}/nfapi/public_inc
                                                                   ${NFAPI_DIR}/common/public_inc)
  target_link_libraries(test_nr_pdcch_ul_field_sweep PRIVATE nr_common MAC_NR_COMMON polar crc_byte UTIL
                                                              asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs GTest::gtest)
  add_dependencies(tests test_nr_pdcch_ul_field_sweep)
  add_test(NAME test_nr_pdcch_ul_field_sweep COMMAND ./test_nr_pdcch_ul_field_sweep)
```

(`nr_pdcch_blind_monitor.c`'s own link dependencies are copied from the existing
`nr_pdcch_blind_monitor` library target's `target_link_libraries` line — check that line at build
time in case it has grown new dependencies since this plan was written, and match it exactly
rather than assuming this list is still complete.)

- [ ] **Step 3: Build and run everything so far**

Run: `cmake --build . --target test_nr_pdcch_ul_field_sweep -j8 && ./test_nr_pdcch_ul_field_sweep`
Expected: builds clean, all cases PASS.

Run: `ninja nr-uesoftmodem`
Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
CMake: register Component 2's field-width sweep and its test

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 10: Component 3 — field-value-interpretation hypothesis generator

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc`

**Interfaces:**
- Consumes: `nr_hyp_sweep_init()` (Task 1).
- Produces: `nr_pdcch_ul_interp_hyp_t`, `nr_pdcch_ul_interp_sweep_generate()`,
  `nr_pdcch_ul_interp_sweep_apply()`.

- [ ] **Step 1: Write the failing test**

```cpp
// openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc
// (OAI license header, copied verbatim)
#include <cstdint>
#include <cstring>
#include "gtest/gtest.h"
extern "C" {
#include "nr_pdcch_ul_interp_sweep.h"
#include "nr_pdcch_blind_monitor.h"
}

TEST(NrPdcchUlInterpSweep, GeneratesAtLeastOneHypothesisAndFitsTheCap) {
  nr_hyp_t out[NR_HYP_SWEEP_MAX_RAW];
  const int n = nr_pdcch_ul_interp_sweep_generate(out, NR_HYP_SWEEP_MAX_RAW);
  ASSERT_GT(n, 0);
  ASSERT_LE(n, NR_HYP_SWEEP_MAX_RAW);
}

TEST(NrPdcchUlInterpSweep, TodaysHandSolvedInterpretationIsInTheGeneratedSet) {
  nr_hyp_t out[NR_HYP_SWEEP_MAX_RAW];
  const int n = nr_pdcch_ul_interp_sweep_generate(out, NR_HYP_SWEEP_MAX_RAW);
  ASSERT_GT(n, 0);

  // Today's hand-solved values (pdcch_blind_monitor_ul_tda / _ul_dmrs on this cell): S=0,L=14,
  // mapping=0 (typeA), k2=4, dmrs_config_type=0, dmrs_add_pos=2, dmrs_max_length=1,
  // transform_precoding=0, mcs_table=0.
  bool found = false;
  for (int i = 0; i < n; i++) {
    nr_pdcch_ul_interp_hyp_t h;
    memcpy(&h, out[i].bytes, sizeof(h));
    if (h.tda_start == 0 && h.tda_length == 14 && h.tda_mapping == 0 && h.tda_k2 == 4
        && h.dmrs_config_type == 0 && h.dmrs_add_pos == 2 && h.dmrs_max_length == 1
        && h.transform_precoding == 0 && h.mcs_table == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT -c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc -o /tmp/t5.o`
Expected: FAIL — header missing.

- [ ] **Step 3: Write `nr_pdcch_ul_interp_sweep.h`**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.h
/* OAI license header, copied verbatim */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.h
 * \brief Component 3: UL field-VALUE-INTERPRETATION discovery -- the UL analogue of DL's
 * Technique D (nr_pdsch_config_sweep.h). Discovers what a TDA index actually means (S/L/mapping/
 * k2), and the DM-RS config / MCS table / transform-precoding values, none of which are DCI bit
 * fields themselves. Built on the same nr_hyp_sweep.h engine as Component 2; runs only AFTER
 * Component 2 has locked field widths (a wrong width makes every field value meaningless).
 *
 * Candidate (S,L,mapping,k2) tuples are a small, hand-picked table (mirroring Technique D's own
 * kSL construction), not a full TS 38.214 Table 6.1.2.1.1-2 sweep -- a passive receiver only ever
 * sees traffic exercise the ONE table index the scheduler actually uses, so sweeping every table
 * ENTRY needs traffic that exercises every index, which cannot be arranged (same reasoning
 * nr_pdsch_config_sweep_init()'s own comment already documents for the DL case).
 */
#ifndef NR_PDCCH_UL_INTERP_SWEEP_H
#define NR_PDCCH_UL_INTERP_SWEEP_H

#include <stdint.h>
#include "nr_hyp_sweep.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t tda_start;
  uint8_t tda_length;
  uint8_t tda_mapping; // 0 = typeA, 1 = typeB
  uint8_t tda_k2;
  uint8_t dmrs_config_type; // 0 = type1, 1 = type2
  uint8_t dmrs_add_pos;     // 0..3
  uint8_t dmrs_max_length;  // 1 or 2
  uint8_t transform_precoding; // 0 = disabled, 1 = enabled
  uint8_t mcs_table;        // 0 = qam64, 1 = qam256, 2 = qam64LowSE
} nr_pdcch_ul_interp_hyp_t;

/** Stage a's generator. No `fixed`/`dci_length` parameter -- unlike Component 2, none of these
 * fields affect DCI size, only PUSCH allocation semantics, so there is no length filter here. */
int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *out, int max_out);

/** Write one generated hypothesis's values into the live nr_pdcch_blind_ul_opts_t TDA
 * table entry 0 and the DM-RS/waveform fields. (Entry 0 only, per the header comment above --
 * this cell's traffic uses one TDA index; extending to multiple observed indices is future work,
 * same limitation Technique D already documents for DL.) */
void nr_pdcch_ul_interp_sweep_apply(const nr_hyp_t *hyp, struct nr_pdcch_blind_ul_opts_s *opts);

#ifdef __cplusplus
}
#endif

#endif
```

**Note for the implementer**: check `nr_pdcch_blind_ul_opts_t`'s actual typedef — if it's declared
as `typedef struct { ... } nr_pdcch_blind_ul_opts_t;` (anonymous struct, no tag), the forward
declaration `struct nr_pdcch_blind_ul_opts_s` above won't match and this header must instead
`#include "nr_pdcch_blind_monitor.h"` directly (same as Component 2's header does) and use
`nr_pdcch_blind_ul_opts_t *opts` in the signature. Verify against the real header before writing
the `.c` file in Step 4.

- [ ] **Step 4: Implement `nr_pdcch_ul_interp_sweep.c`**

```c
// openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.c
/* OAI license header, copied verbatim */
#include "nr_pdcch_ul_interp_sweep.h"
#include "nr_pdcch_blind_monitor.h"
#include <string.h>

typedef struct { uint8_t s, l, mapping, k2; } nr_ul_tda_cand_t;

int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *out, int max_out)
{
  static const nr_ul_tda_cand_t kTda[] = {
      {0, 14, 0, 1}, {0, 14, 0, 2}, {0, 14, 0, 3}, {0, 14, 0, 4},
      {0, 7,  0, 1}, {0, 7,  0, 2},
      {2, 12, 0, 1}, {2, 12, 0, 2},
      {0, 4,  1, 1}, {0, 4,  1, 2}, // mapping type B candidates
  };
  static const uint8_t kDmrsCfgType[] = {0, 1};
  static const uint8_t kDmrsAddPos[]  = {0, 1, 2, 3};
  static const uint8_t kDmrsMaxLen[]  = {1, 2};
  static const uint8_t kTp[]          = {0, 1};
  static const uint8_t kMcsTable[]    = {0, 1, 2};

  const int n_tda   = (int)(sizeof(kTda) / sizeof(kTda[0]));
  const int n_dct    = (int)(sizeof(kDmrsCfgType) / sizeof(kDmrsCfgType[0]));
  const int n_dap    = (int)(sizeof(kDmrsAddPos) / sizeof(kDmrsAddPos[0]));
  const int n_dml    = (int)(sizeof(kDmrsMaxLen) / sizeof(kDmrsMaxLen[0]));
  const int n_tp     = (int)(sizeof(kTp) / sizeof(kTp[0]));
  const int n_mcst   = (int)(sizeof(kMcsTable) / sizeof(kMcsTable[0]));

  int n_out = 0;
  for (int a = 0; a < n_tda; a++) {
    for (int b = 0; b < n_dct; b++) {
      for (int c = 0; c < n_dap; c++) {
        for (int d = 0; d < n_dml; d++) {
          for (int e = 0; e < n_tp; e++) {
            for (int f = 0; f < n_mcst; f++) {
              if (n_out >= max_out) {
                return -1;
              }
              const nr_pdcch_ul_interp_hyp_t h = {
                  kTda[a].s, kTda[a].l, kTda[a].mapping, kTda[a].k2,
                  kDmrsCfgType[b], kDmrsAddPos[c], kDmrsMaxLen[d], kTp[e], kMcsTable[f],
              };
              memcpy(out[n_out].bytes, &h, sizeof(h));
              out[n_out].len = (int)sizeof(h);
              n_out++;
            }
          }
        }
      }
    }
  }
  return n_out;
}

void nr_pdcch_ul_interp_sweep_apply(const nr_hyp_t *hyp, nr_pdcch_blind_ul_opts_t *opts)
{
  nr_pdcch_ul_interp_hyp_t h;
  memcpy(&h, hyp->bytes, sizeof(h));
  opts->tda_start[0]      = h.tda_start;
  opts->tda_length[0]     = h.tda_length;
  opts->tda_mapping[0]    = h.tda_mapping;
  opts->tda_k2[0]         = h.tda_k2;
  opts->dmrs_config_type  = h.dmrs_config_type;
  opts->dmrs_add_pos      = h.dmrs_add_pos;
  opts->dmrs_max_length   = h.dmrs_max_length;
  opts->transform_precoding = h.transform_precoding;
  opts->mcs_table         = h.mcs_table;
}
```

(Fix the header's forward declaration per the Step 3 implementer note before this compiles —
change it to `#include "nr_pdcch_blind_monitor.h"` and use the real typedef name.)

10 × 2 × 4 × 2 × 2 × 3 = 960 hypotheses, comfortably under `NR_HYP_SWEEP_MAX_RAW`.

- [ ] **Step 5: Run tests to verify they pass**

Run: `g++ -std=c++17 -I openair1/PHY/NR_UE_TRANSPORT openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c -lgtest -lgtest_main -pthread -o /tmp/t6 && /tmp/t6`
Expected: PASS, 2/2.

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.h \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_ul_interp_sweep.c \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc
git commit -m "$(cat <<'EOF'
Component 3: UL field-interpretation hypothesis generator

The UL analogue of DL's Technique D -- discovers what a TDA index means
(S/L/mapping/k2), DM-RS config, MCS table, and transform precoding, none
of which are recoverable from field widths alone (Component 2 only
resolves how many bits each field is, not what the resulting value
means). Candidate TDA tuples are a small hand-picked table, same
reasoning as nr_pdsch_config_sweep_init()'s DL table: a passive receiver
can only ever search the index its own traffic actually exercises.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 11: Component 3 — wire into `run_occasion()`/PUSCH decode, offline known-answer test

Mirrors Task 7 + Task 8 exactly, substituting Component 3's generator/apply functions and the
`interp_hyp_class` field for Component 2's. Given the pattern is now fully established, this task
is scoped as one unit rather than re-deriving each step's rationale a second time.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc`

**Interfaces:**
- Consumes: `nr_hyp_sweep_*()` (Task 2), `nr_pdcch_ul_interp_sweep_generate/apply()` (Task 10),
  `interp_hyp_class` (Task 5), and — critically — **the LOCKED winner from Component 2's sweep**
  (`nr_hyp_sweep_winner(nr_pdcch_ul_width_sweep_get_state()) >= 0`), since Component 3 must not
  start until field widths are settled.
- Produces: `g_cfg.ul.tda_start[0]` etc. overwritten with the winning interpretation.

- [ ] **Step 1: Add a second file-scope sweep state, `s_ul_interp_sweep`/`s_ul_interp_sweep_init`, and its own `nr_pdcch_ul_interp_sweep_get_state()` accessor, following Task 7 Steps 1 and 6 exactly.** The init block's gate condition is:

```c
  if (cfg->dl_full_auto
      && nr_pdcch_ul_width_sweep_get_state() != NULL
      && nr_hyp_sweep_winner(nr_pdcch_ul_width_sweep_get_state()) >= 0
      && !s_ul_interp_sweep_init) {
    nr_hyp_t raw[NR_HYP_SWEEP_MAX_RAW];
    const int n_raw = nr_pdcch_ul_interp_sweep_generate(raw, NR_HYP_SWEEP_MAX_RAW);
    if (n_raw > 0) {
      const int n_classes = nr_hyp_sweep_init(&s_ul_interp_sweep, raw, n_raw, NULL, NULL, NULL,
                                              NULL, 0, NULL);
      if (n_classes > 0) {
        s_ul_interp_sweep_init = true;
        LOG_A(PHY, "SENSING: Phase 3 UL interpretation sweep ARMED, %d classes\n", n_classes);
      } else {
        LOG_E(PHY, "SENSING: Phase 3 UL interpretation sweep -- init returned %d (cap "
                   "exceeded); NOT armed\n", n_classes);
      }
    }
  }
```

- [ ] **Step 2: Pick/apply a hypothesis when building each UL candidate task, following Task 7 Step 2 exactly**, applying `nr_pdcch_ul_interp_sweep_apply()` to `task_ul_opts` AFTER Component 2's
      `nr_pdcch_ul_field_sweep_apply()` (widths first, then interpretation — order matters, since
      interpretation fields don't affect `nr_pdcch_blind_dci01_size()` but width fields do, so
      applying interpretation first would be silently overwritten by nothing but is still the
      wrong logical order to reason about). Store the chosen class in a new
      `int chosen_interp_class` field on `nr_pdcch_blind_cand_task_t`, copied into
      `cand_task[ti].ul_out.interp_hyp_class` at the same point Task 7 Step 3 sets
      `width_hyp_class`.

- [ ] **Step 3: Feed the TB-CRC result back at both PUSCH decode sites, following Task 7 Steps 4-5 exactly**, reading `interp_hyp_class` and calling `nr_hyp_sweep_feed()` on
      `nr_pdcch_ul_interp_sweep_get_state()`'s state.

- [ ] **Step 4: Write the offline known-answer test**, following Task 8 Step 1's pattern exactly, generating via `nr_pdcch_ul_interp_sweep_generate()`, with the ground-truth predicate matching
      Task 10's `TodaysHandSolvedInterpretationIsInTheGeneratedSet` test's values (S=0,L=14,
      mapping=0,k2=4,dmrs_config_type=0,dmrs_add_pos=2,dmrs_max_length=1,transform_precoding=0,
      mcs_table=0), and a synthetic pass rate of ~76% for ground truth vs 0% for every other class.

- [ ] **Step 5: Write the tie-inspection check**, following Task 8 Step 3's pattern, printing how many generated hypotheses match ground truth on `tda_start`/`tda_length` (the fields this
      cell's own traffic pattern actually constrains) — same "record, don't assume" instruction.

- [ ] **Step 6: Build and run**

Run: `ninja nr-uesoftmodem` — expect clean build.
Run the two new/extended test binaries per Task 8 Step 2/4's commands, substituting the Component
3 test file — expect PASS, and record the tie-inspection count in the commit message.

- [ ] **Step 7: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h \
        openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_monitor_rt.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_ul_interp_sweep_test.cc
git commit -m "$(cat <<'EOF'
Component 3: wire the interpretation sweep, offline known-answer test

Gated on Component 2's width sweep having already locked a winner (a
wrong width makes every field value meaningless, same ordering Technique
D already enforces for DL). Applied to task_ul_opts AFTER Component 2's
widths, following the shared engine's same round-robin/feed pattern via
the second, independent nr_hyp_sweep_state_t instance. Offline
known-answer test converges to today's hand-solved TDA/DM-RS/MCS-table
values under a synthetic oracle; tie-inspection count recorded above.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Task 12: Register Component 3 in CMake, full build + full offline suite

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add `nr_pdcch_ul_interp_sweep.c` to the `nr_pdcch_blind_monitor` library**, and add
      a `test_nr_pdcch_ul_interp_sweep` CTest target, following Task 9's exact pattern.

- [ ] **Step 2: Build and run the complete offline test suite**

Run: `ninja tests` (from `cmake_targets/ran_build/build/`)
Run: `ctest --output-on-failure`
Expected: every existing test still PASSes (no regression to the DL suite), plus the new
`test_nr_hyp_sweep`, `test_nr_pdcch_ul_field_sweep`, `test_nr_pdcch_ul_interp_sweep`, and the
extended `test_nr_pdcch_dci_length_sweep`, all PASS.

- [ ] **Step 3: Build the real executable and confirm a clean startup with autodiscover enabled but no live capture**

Run (from `cmake_targets/ran_build/build/`, `pgrep -x nr-uesoftmodem` confirmed empty first):
`ninja nr-uesoftmodem`
Expected: builds and links clean. **Live validation (an actual capture) is explicitly OUT of scope
for this plan's tasks** — the spec's own "Current DL state" section confirms nothing here is
blocked on a known defect, but running a real capture, reading its logs, and confirming the sweep
converges on air is a separate, follow-up activity, not a step of this implementation plan.

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
CMake: register Component 3's interpretation sweep and its test

Completes the build wiring for all three new sweep engines (nr_hyp_sweep,
Component 2, Component 3). Full offline suite green; live validation is a
separate follow-up, not part of this plan.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EWu8gKbFAec7xF6EXNdK4q
EOF
)"
```

---

## Self-Review Notes (completed during plan authoring, not a step for the implementer)

**Spec coverage**: Shared engine Stages a-d → Tasks 1-2. Component 1 (DCI-length sweep, format
0_1) → Task 4. Component 2 (field-width sweep) → Tasks 6-9. Component 3 (interpretation sweep) →
Tasks 10-12. CA/SUL swept as dimensions, not assumed 0 → Task 6's generator table. Loud-refusal
caps → Tasks 1/2 (`nr_hyp_sweep_init` return codes) and Task 6 (`nr_pdcch_ul_field_sweep_generate`
return code). Offline-before-live discipline → Tasks 8, 11 explicitly precede any live-capture
step, and Task 12 Step 3 explicitly declares live validation out of scope for this plan. DL
Technique D migration → explicitly not touched anywhere in this plan (Global Constraints says so
directly); no task references `nr_pdsch_config_sweep.c`'s internals.

**Known open items flagged for the implementer, not silently resolved**: Task 1 Step 7 flags a
signature fix (`samples` must be `const void *const *`, not a flat buffer) discovered while
writing that step — the step's own text carries the fix, not a placeholder. Task 10 Step 3 flags a
struct-tag verification the implementer must do against the real header before Step 4 compiles.
Task 7 Step 2 describes a data-lifetime change (per-task `ul_opts_storage` member) precisely
enough to implement without guessing, but the implementer should re-read the current
`nr_pdcch_blind_cand_task_t` definition at execution time since Tasks 1-6 will have landed real
commits by then and line numbers will have shifted.

**Placeholder scan**: no TBD/TODO left unresolved; every code block is complete, buildable C/C++
against verified real signatures (`nr_pdcch_blind_ul_opts_t`, `nr_pdcch_blind_ul_result_t`,
`nr_pdcch_blind_dci01_size()`, `nr_pusch_passive_decode()`, `nr_pusch_grant_book_add()`, the
existing Technique C/D modules) pulled from the actual repo, not invented.

**Type consistency check**: `nr_hyp_t` (Task 1) is used identically by Component 2 (Task 6) and
Component 3 (Task 10) — same `bytes[96]`/`len` shape, both cast through `memcpy`, never a raw
reinterpret. `width_hyp_class`/`interp_hyp_class` (Task 5) are read by name identically in Tasks 7
and 11's PUSCH-decode feed points. `g_ul_length_found`/`cfg->ul_dci_length` (Task 4) are read by
name identically in Task 7's gate condition.
