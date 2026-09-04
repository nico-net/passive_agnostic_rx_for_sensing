# Phase 2 — SSB as a Standalone CFR Source Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add PBCH-DM-RS-derived per-antenna Ĥ as a new sensing source (`NR_ISAC_SRC_SSB`), so the receiver has a reference signal that requires no grant, no RNTI, and no decode — the one source that structurally cannot fail from a cell-specific misconfiguration.

**Architecture:** Same three-layer shape every existing source in this module already follows: (1) enum/token/ref-type plumbing so `sensing.sources = "ssb"` is recognised; (2) a real-time tap in the PBCH tracking path (`phy_procedures_nr_ue.c`), submitted through the existing `nr_isac_submit_cfr_multi()` API — no new submission mechanism; (3) a small, independently-testable pure function that derives the CRB0-relative absolute subcarrier axis (`k_abs`) SSB's channel estimate sits on, because every fusion bug this project has hit so far ("silent-garbage failure of the same class as a wrong `csirs_monitor` scramb_id") has been exactly this kind of axis mismatch.

**Tech Stack:** C (OAI PHY, `phy_procedures_nr_ue.c`), C++ (`NR_UE_ISAC` module), gtest, CMake/Make, live validation against the X410 on sens6.

**Spec:** [Cell-Agnostic Passive Receiver roadmap](https://claude.ai/code/artifact/e1e6ae5d-25f6-4c30-97cb-09f2c0f239a2) (artifact; "Phase 2" section) — read alongside this plan, which fills in the engineering detail the roadmap leaves at the concept level.

## Global Constraints

- **Host and repo:** all work happens on **sens6**, in `/home/sens/NICOLA/openairinterface5g-total-passive-ue`, branch `total-passive-rx-UL-DL-graphics` (there is no local git repo in this environment — every read/edit/build/test/commit goes through `ssh sens6 "..."` or `scp`).
- **Never build while a capture is running.** `pgrep -x nr-uesoftmodem` must return nothing before any `make`.
- **Verify the binary, not the tree** after any build that must reach the air: `strings <binary> | grep -c '<a literal just added>'` must be non-zero.
- **`NANT=1` for offline/plumbing work; a live validation capture may use more antennas once the single-antenna path is proven** (this project's own working convention — receive branches 1-3 are individually undecodable on this rig, so isolate variables).
- **All fusion sources must submit `k_abs` on the same absolute-subcarrier axis (relative to CRB0/point A)** — this is stated directly in `nr_isac_submit_cfr()`'s own doc comment (`nr_isac.h`) and is the single most consequential correctness requirement in this plan.
- **A new source token must never silently alias an existing one.** A prior bug (`nr_isac.cc`'s `source_bit_from_token`, the `pusch_dmrs` case) had a token return the bare enum value instead of `1u << enum`, which happened to equal a DIFFERENT source's bit — the token silently enabled the wrong source instead of being rejected. Every new branch in this plan must return `1u << NR_ISAC_SRC_SSB`, never the bare enum.
- **A new `nr_isac_source_t` case must be added explicitly to `nr_isac_source_to_ref_type()`'s switch** in `detection_report.cc` — that switch's `default` case returns `"csi_rs"`, so an unhandled new enum value silently mislabels every SSB-sourced report as `csi_rs` rather than failing loudly.
- **CFR-fusion axis facts already validated on this cell** (ARFCN 630000, PCI 2, 273 PRB/100 MHz, SCS 30 kHz) from Phase 1's work, reusable as real test fixtures rather than synthetic numbers: `ssb_start_subcarrier = 1478` (confirmed independently by the UE's own blind scan, "GSCN: 8019, with SSB offset: 1478"), and the same `ssb_offset_point_a = (ssb_start_subcarrier - ssb_sc_offset_norm) / 12` derivation Phase 1 used and log-verified.

---

### Task 1: Enum, token, and ref-type plumbing for `NR_ISAC_SRC_SSB`

Adds the new source to the type system everywhere an existing source already appears, with a test that would have caught the `pusch_dmrs` aliasing bug had it existed then.

**Files:**
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac.h:48-66` (the `nr_isac_source_e` enum)
- Modify: `openair1/PHY/NR_UE_ISAC/nr_isac.cc:70-92` (`source_bit_from_token`)
- Modify: `openair1/PHY/NR_UE_ISAC/detection_report.cc:46-62` (`nr_isac_source_to_ref_type`)
- Create: `openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc`
- Modify: `CMakeLists.txt` (register the new test target, following the existing pattern at the `test_isac_aoa`/`test_sparse_doppler` entries)

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `NR_ISAC_SRC_SSB` (enum value 6, `NR_ISAC_SRC_COUNT` becomes 7); `source_bit_from_token("ssb")` returns `1u << NR_ISAC_SRC_SSB`; `nr_isac_source_to_ref_type(NR_ISAC_SRC_SSB)` returns `"ssb"`. Task 2 and Task 3 both consume this enum value.

- [ ] **Step 1: Write the failing test**

```bash
ssh sens6 "cat > /tmp/nr_isac_ssb_source_test.cc" << 'EOF'
/*! \file openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc
 * \brief Phase 2 (roadmap artifact): the type-system plumbing for NR_ISAC_SRC_SSB, tested in
 * isolation from any RT tap. Written first per this project's own precedent — a prior source
 * ("pusch_dmrs") silently returned the WRONG bit from source_bit_from_token() because a branch
 * forgot the `1u <<` shift and happened to collide with a different source's already-shifted
 * value; nothing caught it until it was found by inspection. This test exists so the same class
 * of bug fails a build instead of shipping quietly for a different source.
 */
#include <gtest/gtest.h>

extern "C" {
#include "nr_isac.h"
}

// The parser and ref_type functions are C++ (nr_isac.cc / detection_report.cc), declared here
// with C++ linkage matching their actual definitions.
uint32_t source_bit_from_token(const std::string& tok);
const char* nr_isac_source_to_ref_type(nr_isac_source_t source);

TEST(SsbSource, TokenMapsToTheShiftedSsbBitNotABareEnum) {
  const uint32_t bit = source_bit_from_token("ssb");
  EXPECT_EQ(bit, 1u << NR_ISAC_SRC_SSB);
  // The exact failure mode this guards: a forgotten shift would return NR_ISAC_SRC_SSB itself
  // (6), which is NOT equal to 1u << NR_ISAC_SRC_SSB (64) and would silently collide with
  // whatever OTHER source happens to have bit value 6 if the enum ever grows that far — assert
  // the shift explicitly rather than only checking non-zero.
  EXPECT_NE(bit, static_cast<uint32_t>(NR_ISAC_SRC_SSB));
}

TEST(SsbSource, UnknownTokenStillReturnsZero) {
  // Regression guard: adding a new branch must not accidentally widen the fallthrough.
  EXPECT_EQ(source_bit_from_token("not_a_real_source"), 0u);
}

TEST(SsbSource, RefTypeIsExplicitNotTheDefaultFallback) {
  // detection_report.cc's switch defaults to "csi_rs" for anything unhandled. This assertion
  // fails loudly if a future refactor removes the SSB case and lets it fall through silently.
  EXPECT_STREQ(nr_isac_source_to_ref_type(NR_ISAC_SRC_SSB), "ssb");
  EXPECT_STRNE(nr_isac_source_to_ref_type(NR_ISAC_SRC_SSB), "csi_rs");
}

TEST(SsbSource, EnumCountIncludesSsb) {
  // NR_ISAC_SRC_COUNT sizes the enabled-source bitmask (sources_mask) — every source must be
  // strictly below it or its bit is silently unreachable.
  EXPECT_LT(static_cast<int>(NR_ISAC_SRC_SSB), static_cast<int>(NR_ISAC_SRC_COUNT));
}

TEST(SsbSource, IlluminatorIsDownlinkLikeEveryOtherDlSource) {
  // nr_isac_source_illum() special-cases only the two PUSCH (uplink-illuminated) sources; SSB is
  // transmitted by the gNB, so it must fall through to NR_ISAC_ILLUM_DL like csi_rs/pdsch_*.
  EXPECT_EQ(nr_isac_source_illum(NR_ISAC_SRC_SSB), static_cast<unsigned>(NR_ISAC_ILLUM_DL));
}
EOF
scp sens6:/tmp/nr_isac_ssb_source_test.cc - > /dev/null 2>&1 || true
ssh sens6 "mv /tmp/nr_isac_ssb_source_test.cc /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc"
```

- [ ] **Step 2: Register the CMake target**

Read `CMakeLists.txt` around the `test_isac_aoa` entry (search `add_executable(test_isac_aoa`) and add an equivalent block immediately after it:

```cmake
  # Phase 2 (roadmap artifact "Cell-Agnostic Passive Receiver"): type-system plumbing for the SSB
  # CFR source (NR_ISAC_SRC_SSB), tested in isolation from the RT tap.
  add_executable(test_nr_isac_ssb_source ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc
                                          ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/nr_isac.cc
                                          ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/detection_report.cc)
  target_include_directories(test_nr_isac_ssb_source PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_ISAC)
  target_link_libraries(test_nr_isac_ssb_source PRIVATE UTIL GTest::gtest)
  add_dependencies(tests test_nr_isac_ssb_source)
  add_test(NAME test_nr_isac_ssb_source COMMAND ./test_nr_isac_ssb_source)
```

Note: `nr_isac.cc` and `detection_report.cc` are compiled directly into this test target (not linked against the full `NR_UE_ISAC` static lib) because at this point in the plan neither file has any dependency beyond `nr_isac.h` — if the build fails with unresolved symbols, check whether `nr_isac.cc`/`detection_report.cc` already pull in something heavier (e.g. `sensing_engine.h`) via an include this task doesn't expect, and switch to linking `NR_UE_ISAC` (the pattern `test_isac_aoa`/`test_sparse_doppler` use) instead of compiling the two `.cc` files directly.

- [ ] **Step 3: Run the test to verify it fails**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_isac_ssb_source -j8 2>&1 | tail -30"
```

Expected: compile error — `NR_ISAC_SRC_SSB` is not a member of `nr_isac_source_e`.

- [ ] **Step 4: Add the enum value**

In `nr_isac.h`, extend the enum (append, do not renumber existing values — every existing config/report/persisted value keys off the current numbering):

```c
typedef enum nr_isac_source_e {
  NR_ISAC_SRC_CSI_RS = 0,
  NR_ISAC_SRC_PDSCH_DMRS = 1,
  NR_ISAC_SRC_PDSCH_DATA = 2,
  NR_ISAC_SRC_PDSCH_DMRS_BLIND = 3,
  NR_ISAC_SRC_PUSCH_DMRS = 4,
  NR_ISAC_SRC_PUSCH_DATA = 5,
  /// SSB (PBCH DM-RS) channel estimate. Requires no grant, no RNTI, no decode of any kind — every
  /// NR cell transmits it on a fixed raster with known structure, so it is the one source that
  /// cannot fail for a configuration reason. Trade-off: 20 ms burst period caps unambiguous
  /// velocity at +-2.17 m/s (lambda/(4*T) at 86.9 mm/20 ms) -- see Phase 2 of the roadmap
  /// (https://claude.ai/code/artifact/e1e6ae5d-25f6-4c30-97cb-09f2c0f239a2).
  NR_ISAC_SRC_SSB    = 6,
  NR_ISAC_SRC_COUNT  = 7
} nr_isac_source_t;
```

- [ ] **Step 5: Add the token branch**

In `nr_isac.cc`'s `source_bit_from_token()`, add before the final `return 0;`:

```c
  if (tok == "ssb") {
    return 1u << NR_ISAC_SRC_SSB;
  }
```

- [ ] **Step 6: Add the ref_type case**

In `detection_report.cc`'s `nr_isac_source_to_ref_type()`, add a new case before `case NR_ISAC_SRC_CSI_RS:`:

```c
    case NR_ISAC_SRC_SSB:
      return "ssb";
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_isac_ssb_source -j8 && ./test_nr_isac_ssb_source"
```

Expected: `[==========] 5 tests from 1 test suite ran. [ PASSED ] 5 tests.`

- [ ] **Step 8: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/PHY/NR_UE_ISAC/nr_isac.h openair1/PHY/NR_UE_ISAC/nr_isac.cc openair1/PHY/NR_UE_ISAC/detection_report.cc openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc CMakeLists.txt && git commit -m 'Phase 2: add NR_ISAC_SRC_SSB to the source type system

Enum + sensing.sources token (\"ssb\") + ref_type label, following the exact
existing pattern for every other source. 5 new gtests, including one that
would have caught the pusch_dmrs bare-enum-vs-shifted-bit bug had it existed
for that source.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 2: The CRB0-relative subcarrier axis derivation (pure, testable)

Isolates the one genuinely error-prone piece of this whole feature — mapping SSB's own 240-subcarrier channel estimate onto the same absolute axis every other fused source shares — into a small pure function with a real (not synthetic) test fixture from this cell's own already-validated numbers.

**Files:**
- Create: `openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.h`
- Create: `openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.c`
- Modify: `openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc` (add the axis tests to the same file/target from Task 1)
- Modify: `CMakeLists.txt` (add the new `.c` file to `test_nr_isac_ssb_source`'s sources)

**Interfaces:**
- Consumes: nothing beyond stdint types.
- Produces: `void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out /* [240] */)` — fills `k_abs_out[i]` with the CRB0-relative absolute subcarrier index of SSB channel-estimate index `i`, for `i` in `[0, 240)` (`NR_PBCH_NUM_RB * NR_NB_SC_PER_RB` = 20 RB * 12 SC). Task 3 (the RT tap) calls this directly.

- [ ] **Step 1: Write the failing test**

Append to `nr_isac_ssb_source_test.cc` (same file Task 1 created):

```cpp
extern "C" {
#include "nr_isac_ssb_axis.h"
}

// Real cell facts from this project's own Phase 1 work (PHASE1_CSS0_AUTOCONF_HANDOVER.md and the
// live-verified UE blind scan "GSCN: 8019, with SSB offset: 1478"), not synthetic numbers — ARFCN
// 630000, PCI 2, 273 PRB / 100 MHz, SCS 30 kHz, ssb_start_subcarrier = 1478.
TEST(SsbAxis, MatchesTheLiveVerifiedCellAtIndexZero) {
  uint32_t k_abs[240];
  // k_ssb = 0 is the common case for this deployment (no sub-RB SSB shift); ofdm_symbol_size for
  // 273 PRB / 30 kHz numerology 1 is 4096 (the standard OAI FFT size at this bandwidth/SCS combo
  // -- cross-check against fp->ofdm_symbol_size in a live run before trusting this constant, see
  // Task 3's Step 1).
  nr_isac_ssb_k_abs(/*ssb_start_subcarrier=*/1478, /*k_ssb=*/0, /*ofdm_symbol_size=*/4096, k_abs);
  // The SAME derivation Phase 1 used and log-verified for this exact cell:
  // ssb_offset_point_a = (ssb_start_subcarrier - k_ssb) / 12 = 1478 / 12 = 123 (integer division,
  // matching nr_ue_dci_configuration.c's own ssb_offset_point_a computation).
  // k_abs[0] must be exactly ssb_offset_point_a * 12 (the RB-aligned start), because index 0 of the
  // 240-wide SSB channel estimate sits at the first subcarrier of the SSB's own lowest RB.
  const uint32_t expected_ssb_offset_point_a = (1478 - 0) / 12;
  EXPECT_EQ(k_abs[0], expected_ssb_offset_point_a * 12);
}

TEST(SsbAxis, IsMonotonicAndContiguousOverAllTwoHundredFortySubcarriers) {
  uint32_t k_abs[240];
  nr_isac_ssb_k_abs(1478, 0, 4096, k_abs);
  for (int i = 1; i < 240; i++) {
    // Every fusion source shares one absolute-subcarrier axis; a gap or wraparound here would
    // silently misplace SSB's Ĥ relative to every other source's k_abs on the same carrier.
    EXPECT_EQ(k_abs[i], k_abs[i - 1] + 1) << "discontinuity at index " << i;
  }
}

TEST(SsbAxis, HandlesAWraparoundNearTheFftEdgeWithoutOverflowing) {
  uint32_t k_abs[240];
  // A pathological but legal input: an SSB placed such that its 240-subcarrier span would cross
  // the FFT buffer's own wraparound point (mirrors how existing k index computations elsewhere in
  // this codebase, e.g. dci_nr.c's `(cs_sc + rb*12 + ...) % symb_sz`, are all modulo the symbol
  // size). Use a small ofdm_symbol_size so 240 subcarriers genuinely wrap.
  nr_isac_ssb_k_abs(/*ssb_start_subcarrier=*/200, /*k_ssb=*/0, /*ofdm_symbol_size=*/256, k_abs);
  for (int i = 0; i < 240; i++) {
    EXPECT_LT(k_abs[i], 256u);
  }
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_isac_ssb_source -j8 2>&1 | tail -20"
```

Expected: compile error — `nr_isac_ssb_axis.h` does not exist.

- [ ] **Step 3: Write the header**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.h" << 'EOF'
/*! \file openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.h
 * \brief CRB0-relative absolute-subcarrier axis for SSB's 240-subcarrier channel estimate.
 *
 * Every fusion source submitted through nr_isac_submit_cfr*() must place its k_abs values on the
 * SAME absolute subcarrier axis (relative to CRB0 / point A) -- see nr_isac.h's own doc comment
 * on nr_isac_submit_cfr(). This is a pure function, deliberately split out of the RT tap
 * (phy_procedures_nr_ue.c) so the one genuinely error-prone piece of Phase 2 (the axis
 * derivation) has its own unit test, independent of live hardware.
 *
 * The derivation mirrors nr_ue_dci_configuration.c's ssb_offset_point_a computation exactly
 * (Phase 1, PHASE1_CSS0_AUTOCONF_HANDOVER.md): ssb_offset_point_a = (ssb_start_subcarrier -
 * k_ssb) / 12 gives the RB-aligned CRB0-relative offset of the SSB's lowest RB; index 0 of the
 * 240-wide estimate sits at that RB's first subcarrier.
 */
#ifndef NR_ISAC_SSB_AXIS_H
#define NR_ISAC_SSB_AXIS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill k_abs_out[0..239] with the CRB0-relative absolute subcarrier index of each RE in
 *        SSB's own 240-subcarrier (20 RB) channel estimate.
 *
 * @param ssb_start_subcarrier fp->ssb_start_subcarrier (frame_parms), the FFT-relative subcarrier
 *                              the SSB's own RE 0 sits at.
 * @param k_ssb                Sub-RB SSB shift (fp->ssb_subcarrier_offset), 0 for this deployment.
 * @param ofdm_symbol_size     fp->ofdm_symbol_size, the FFT size k_abs must be taken modulo.
 * @param k_abs_out            Caller-owned array of at least 240 uint32_t.
 */
void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out);

#ifdef __cplusplus
}
#endif

#endif
EOF"
```

- [ ] **Step 4: Write the implementation**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.c" << 'EOF'
#include "nr_isac_ssb_axis.h"

void nr_isac_ssb_k_abs(int ssb_start_subcarrier, int k_ssb, int ofdm_symbol_size, uint32_t* k_abs_out)
{
  // Same derivation as nr_ue_dci_configuration.c's ssb_offset_point_a (Phase 1): integer division
  // is intentional -- it is RB-aligned, matching how nr_pdcch_channel_estimation and every other
  // MIB-derived RB offset in this codebase is computed.
  const int ssb_offset_point_a = (ssb_start_subcarrier - k_ssb) / 12;
  const int base_sc            = ssb_offset_point_a * 12;
  for (int i = 0; i < 240; i++) {
    k_abs_out[i] = (uint32_t)((base_sc + i) % ofdm_symbol_size);
  }
}
EOF"
```

- [ ] **Step 5: Add the new source file to the test target**

Update `CMakeLists.txt`'s `test_nr_isac_ssb_source` executable to include the new `.c` file:

```cmake
  add_executable(test_nr_isac_ssb_source ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc
                                          ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/nr_isac.cc
                                          ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/detection_report.cc
                                          ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/nr_isac_ssb_axis.c)
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_isac_ssb_source -j8 && ./test_nr_isac_ssb_source"
```

Expected: `[==========] 8 tests from 3 test suites ran. [ PASSED ] 8 tests.`

- [ ] **Step 7: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.h openair1/PHY/NR_UE_ISAC/nr_isac_ssb_axis.c openair1/PHY/NR_UE_ISAC/tests/nr_isac_ssb_source_test.cc CMakeLists.txt && git commit -m 'Phase 2: pure, tested CRB0-relative axis derivation for the SSB CFR tap

Split out of the RT tap deliberately -- axis mismatches between fusion
sources are this projects most common silent-garbage failure class
(same shape as a wrong csirs_monitor scramb_id). Test fixture uses this
cells own live-verified ssb_start_subcarrier=1478 from Phase 1, not a
synthetic number.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 3: The RT tap — submit SSB's per-antenna Ĥ as a CFR row

Wires Tasks 1-2 into the live PBCH tracking path. No live capture in this task (that is Task 4) — this task's deliverable is a clean, verified build.

**Files:**
- Modify: `openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1375-1396` (the per-antenna `nr_pbch_channel_estimation()` loop, live PBCH-tracking path — NOT the initial-sync-only paths in `nr_initial_sync.c`)

**Interfaces:**
- Consumes: `NR_ISAC_SRC_SSB` (Task 1), `nr_isac_ssb_k_abs()` (Task 2), and the pre-existing `nr_isac_source_enabled(int)` / `nr_isac_submit_cfr_multi(...)` / `nr_isac_aoa_antennas()` API (`nr_isac.h`, already built and used by every other RT tap in this codebase — read `PHY/NR_UE_TRANSPORT/csi_rx.c`'s existing CSI-RS tap call site as the pattern to match if anything here is ambiguous).
- Produces: nothing new for later tasks in this plan; Task 4 is a live-capture validation of this task's output, not a code consumer.

- [ ] **Step 1: Confirm `NR_PBCH_NUM_RB`/`NR_NB_SC_PER_RB` and `fp->ofdm_symbol_size` at this call site**

Before writing the tap, confirm the exact array size and FFT size in scope here (Task 2's test assumed `ofdm_symbol_size=4096` for this cell — verify against the live struct, not the assumption):

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && grep -n 'NR_PBCH_NUM_RB\|NR_NB_SC_PER_RB' openair1/PHY/defs_nr_common.h openair1/PHY/impl_defs_top.h 2>/dev/null | head -5"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && sed -n '1370,1382p' openair1/SCHED_NR_UE/phy_procedures_nr_ue.c"
```

`NR_PBCH_NUM_RB * NR_NB_SC_PER_RB` must equal 240 (already used at line ~1394's `memcpy(..., sizeof(*dl_ch_estimates_symbol) * NR_PBCH_NUM_RB * NR_NB_SC_PER_RB)` in the same function) — if it does not, Task 2's `nr_isac_ssb_k_abs()` array size (`[240]`) and this task's loop bound must both change to match; do not hardcode 240 without this check.

- [ ] **Step 2: Add the RT tap after the per-antenna channel-estimation loop**

In `phy_procedures_nr_ue.c`, immediately after the closing brace of the `for (int aarx = 0; aarx < fp->nb_antennas_rx; aarx++) { nr_pbch_channel_estimation(...); ... }` loop (the one containing the `UEscopeCopy` call, ending around line 1396) and before the `// Copy current symbol estimate for FO estimation` comment, add:

```c
  // Phase 2 (roadmap artifact "Cell-Agnostic Passive Receiver"): submit this PBCH symbol's
  // per-antenna channel estimate as a CFR row. No grant, no RNTI, no decode required -- every
  // NR cell transmits this on a fixed raster, so it is the sensing source most robust to
  // cell-specific misconfiguration. Guarded by nr_isac_source_enabled() so this is a true no-op
  // (not even the k_abs derivation runs) unless "ssb" is in sensing.sources.
  if (nr_isac_enabled() && nr_isac_source_enabled(NR_ISAC_SRC_SSB)) {
    uint32_t k_abs[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
    uint32_t l_sym[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB];
    nr_isac_ssb_k_abs(ssb_start_subcarrier, fp->ssb_subcarrier_offset, fp->ofdm_symbol_size, k_abs);
    for (uint32_t i = 0; i < NR_PBCH_NUM_RB * NR_NB_SC_PER_RB; i++) {
      l_sym[i] = (uint32_t)relPbchSymb;
    }
    const uint32_t nof_ant = nr_isac_aoa_antennas() > 0
                                  ? (nr_isac_aoa_antennas() < (uint32_t)fp->nb_antennas_rx ? nr_isac_aoa_antennas()
                                                                                            : (uint32_t)fp->nb_antennas_rx)
                                  : 1;
    // dl_ch_estimates is ANTENNA-MAJOR ALREADY at this call site (dl_ch_estimates[aarx]), and
    // nr_isac_submit_cfr_multi() wants one contiguous antenna-major buffer with an explicit
    // stride -- dl_ch_estimates[aarx] are separate allocations, not one contiguous block, so pack
    // them into a stack buffer rather than assuming a stride across dl_ch_estimates itself.
    float h_packed[2 * (NR_PBCH_NUM_RB * NR_NB_SC_PER_RB) * NR_MAX_RX_ANTENNAS];
    for (uint32_t a = 0; a < nof_ant; a++) {
      const c16_t* est = (const c16_t*)dl_ch_estimates[a];
      for (uint32_t i = 0; i < NR_PBCH_NUM_RB * NR_NB_SC_PER_RB; i++) {
        h_packed[2 * (a * (NR_PBCH_NUM_RB * NR_NB_SC_PER_RB) + i) + 0] = (float)est[i].r;
        h_packed[2 * (a * (NR_PBCH_NUM_RB * NR_NB_SC_PER_RB) + i) + 1] = (float)est[i].i;
      }
    }
    const nr_isac_carrier_t carrier = {
        .nof_prb         = fp->N_RB_DL,
        .scs_hz           = (uint32_t)(15000u << fp->numerology_index),
        .dl_center_hz     = (uint64_t)fp->dl_CarrierFreq,
        .pci              = (uint16_t)fp->Nid_cell,
        .slots_per_frame  = (uint16_t)fp->slots_per_frame,
    };
    nr_isac_submit_cfr_multi(proc->nr_slot_rx,
                             0.0f,
                             NR_ISAC_SRC_SSB,
                             &carrier,
                             h_packed,
                             nof_ant,
                             NR_PBCH_NUM_RB * NR_NB_SC_PER_RB,
                             k_abs,
                             l_sym,
                             NR_PBCH_NUM_RB * NR_NB_SC_PER_RB,
                             0.0f);
  }
```

Field-name caveats to resolve while implementing (do not guess — grep the actual `NR_DL_FRAME_PARMS` struct definition before trusting these): `fp->numerology_index` / `fp->dl_CarrierFreq` / `fp->Nid_cell` are the plan's best-available names from this codebase's conventions elsewhere, but this exact call site's `fp` may already have a locally-scoped SCS/frequency variable in context (check the top of the containing function) — prefer an already-in-scope value over re-deriving one if both exist, to avoid a second source of truth. `NR_MAX_RX_ANTENNAS` — confirm this constant exists (grep `defs_nr_common.h`) and is large enough for this deployment's maximum antenna count; if it is not defined, use a literal matching the codebase's existing per-antenna buffer sizing convention at this call site instead of inventing a new bound.

- [ ] **Step 3: Build and verify the binary**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make nr-uesoftmodem -j8 2>&1 | tail -40"
ssh sens6 "strings /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build/nr-uesoftmodem | grep -c nr_isac_ssb_k_abs"
```

Expected: clean build, non-zero `strings` count (confirms the new function is actually linked into the deployed binary, not just compiled into an unused object).

- [ ] **Step 4: Run the existing offline test suites to confirm no regression**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && ./test_nr_isac_ssb_source && ./test_nr_pdcch_blind_monitor && ./test_isac_aoa && ./test_isac_sync"
```

Expected: all four suites pass (this task did not touch anything they depend on, so this should be a clean confirmation, not a debugging step).

- [ ] **Step 5: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/SCHED_NR_UE/phy_procedures_nr_ue.c && git commit -m 'Phase 2: RT tap submitting SSB per-antenna CFR from the live PBCH tracking path

Guarded by nr_isac_source_enabled(NR_ISAC_SRC_SSB) -- a true no-op unless
ssb is in sensing.sources. Uses the Task 2 axis helper and the existing
nr_isac_submit_cfr_multi() API, no new submission mechanism. Build-verified
only in this task; live validation is Task 4.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 4: Live validation on the X410

Isolates the new source (no fusion confound, matching this project's own established single-variable-at-a-time methodology) and checks it against the roadmap's own stated predictions and Task 2's axis derivation, using real gNB/log ground truth rather than assumption.

**Files:**
- Modify: `/home/sens/NICOLA/nrue.passive_rx.autoconf.conf` (or a new sibling conf, implementer's judgment) — `sensing.sources = "ssb"` alone for the isolation run
- Create: `PHASE2_SSB_CFR_SOURCE_HANDOVER.md` (repo root, matching this project's one-handover-doc-per-phase convention: `PHASE1_CSS0_AUTOCONF_HANDOVER.md`, `PHASE3_AOA_MULTISTATIC_HANDOVER.md`, etc.)

**Interfaces:**
- Consumes: Task 3's RT tap.
- Produces: nothing for a later task in THIS plan; this is the closing validation.

- [ ] **Step 1: Confirm no capture is running, then run an isolated SSB-only capture**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cp /home/sens/NICOLA/nrue.passive_rx.selfconf.conf /home/sens/NICOLA/nrue.passive_rx.ssbonly.conf"
ssh sens6 "sed -i 's/sources *=.*/sources = \"ssb\";/' /home/sens/NICOLA/nrue.passive_rx.ssbonly.conf"
ssh sens6 "grep -n 'sources' /home/sens/NICOLA/nrue.passive_rx.ssbonly.conf"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures && ARM=ssb_only CONF=/home/sens/NICOLA/nrue.passive_rx.ssbonly.conf DUR=150 TRIES=1 NANT=1 ./run_arm.sh"
```

- [ ] **Step 2: Confirm a CPI actually closes on SSB alone, and read its slow-time period**

```bash
ssh sens6 "L=\$(ls -dt /home/sens/NICOLA/captures/ssb_only_*/ | head -1)/run.log; grep -aoE 'SENSING: CPI #[0-9]+ .{0,200}' \"\$L\" | head -3"
```

Pass: at least one `CPI #` line appears, and its reported slow-time row period is consistent with SSB's 20 ms burst period (40 slots at this cell's 30 kHz numerology) — not the sub-millisecond period a slot-rate source like `pdsch_data` would show. If zero CPIs close, check whether `cpi_slots` in the conf is sized sensibly for a ~20 ms-per-row source before assuming the RT tap itself is broken (a `cpi_slots` tuned for a slot-rate source will take far longer in wall time to accumulate on a 20 ms-per-row source).

- [ ] **Step 3: Cross-check the reported velocity axis against the roadmap's own prediction**

```bash
ssh sens6 "L=\$(ls -dt /home/sens/NICOLA/captures/ssb_only_*/ | head -1)/run.log; grep -aoE 'vel_max[^ ]* [0-9.]+' \"\$L\" | head -3"
```

Pass: `vel_max` (unambiguous velocity) reads close to the roadmap's stated ±2.17 m/s at this cell's λ = 86.9 mm / 20 ms period — not the ±86.9 m/s a slot-rate source would show. A large discrepancy here means either the burst period isn't what's assumed or `l_sym`/`slot_idx` submission in Task 3's tap is feeding the CPI accumulator at the wrong cadence — re-check Task 3 Step 1's `NR_PBCH_NUM_RB`/`ofdm_symbol_size` confirmation before changing anything else.

- [ ] **Step 4: Cross-check the axis against an already-validated source**

Run a second short capture with `sources = "ssb,csi_rs"` (both enabled, same carrier) and confirm SSB's detections land on plausible range bins relative to CSI-RS's (both share the same physical LOS path, so a gross axis error would show as SSB's LOS peak sitting at a wildly different range bin than CSI-RS's):

```bash
ssh sens6 "sed -i 's/sources *=.*/sources = \"ssb,csi_rs\";/' /home/sens/NICOLA/nrue.passive_rx.ssbonly.conf"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures && ARM=ssb_csirs_fused CONF=/home/sens/NICOLA/nrue.passive_rx.ssbonly.conf DUR=150 TRIES=1 NANT=1 ./run_arm.sh"
ssh sens6 "L=\$(ls -dt /home/sens/NICOLA/captures/ssb_csirs_fused_*/ | head -1)/run.log; grep -aoE 'ref_type=\"[^\"]*\"' \"\$L\" | sort | uniq -c"
```

Pass: `ref_type` values include `ssb`, `csi_rs`, and/or a fused label containing both (per `nr_isac_sources_to_ref_type()`'s `+`-joined format) — confirming both sources are contributing to the same fused grid, not silently disagreeing about where they sit.

- [ ] **Step 5: Write the handover document**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/PHASE2_SSB_CFR_SOURCE_HANDOVER.md" << 'EOF'
# Phase 2 — SSB as a standalone CFR source — HANDOVER

**Status: [fill in from Steps 1-4's actual results before committing this doc]**
**Branch:** total-passive-rx-UL-DL-graphics
**Host:** sens6 · **Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue

## What this phase adds

NR_ISAC_SRC_SSB: a CFR source requiring no grant, no RNTI, no decode -- PBCH DM-RS's own known
gold sequence over the SSB's 240 subcarriers, tapped from the live PBCH tracking path in
phy_procedures_nr_ue.c (NOT the initial-sync-only paths), submitted through the existing
nr_isac_submit_cfr_multi() API alongside every other source.

## Measured [fill in with real numbers from Steps 2-4]

- CPI closure period on ssb-only: ___ ms/row (predicted ~20 ms)
- vel_max on ssb-only: ___ m/s (predicted +-2.17 m/s at this cell's lambda=86.9mm)
- ssb+csi_rs fused ref_type distribution: ___
- Axis cross-check (ssb vs csi_rs LOS range-bin agreement): ___

## Known limitation, unchanged from the roadmap

Unambiguous velocity is capped at +-2.17 m/s -- this source alone finds walking-pace targets, not
vehicles or drones. It is a guaranteed floor, not a replacement for the slot-rate sources; use it
fused with csi_rs/pdsch_* (Phase 1) for the combined coverage.

## Still open

- [Anything Steps 1-4 found but did not resolve -- fill in honestly, following this project's own
  convention (see PHASE1_CSS0_AUTOCONF_HANDOVER.md for the house style: measured numbers, not
  assumed ones, and explicit "not yet done" sections rather than silence].
EOF"
```

Fill in every bracketed placeholder in that file with the ACTUAL measured values from Steps 1-4 before committing — a handover doc with unfilled brackets is worse than no doc, per this project's own established discipline (`verify-before-asserting-never-inherit-a-number`).

- [ ] **Step 6: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add PHASE2_SSB_CFR_SOURCE_HANDOVER.md && git commit -m 'Phase 2: live-validate the SSB CFR source and record measured numbers

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```
