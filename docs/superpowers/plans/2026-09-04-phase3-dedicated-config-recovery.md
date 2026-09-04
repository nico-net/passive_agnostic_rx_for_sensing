# Phase 3 — Recover the Dedicated Config by Search Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recover the dedicated CORESET geometry, search space, and DCI 1_1 payload length on an unknown cell by search rather than by reading a config file — using the PDCCH CRC as a cheap, near-certain oracle (a wrong hypothesis passes with probability 2⁻²⁴, further cut by this project's own already-proven persistence/payload-variance gates).

**Architecture:** Three independent techniques, each landing as its own task with its own test, then wired together: (B) expose the C-RNTI Phase 1's common search space already recovers, turning an open blind search into a keyed one; (A) generalize the existing DM-RS-correlation diagnostic probe (already proven live — it caught real signal the capture-fixture heuristic missed) from "score one assumed CORESET" into "map the whole carrier's occupied REG footprint, no assumption required"; (C) sweep `dci_length` once at startup using the length that makes the CRC-recovered RNTI land in the plausible/bootstrapped range far more often than chance. None of A/B/C touches decode correctness once a hypothesis is confirmed — the remaining gap (payload *interpretation*: TDRA contents, MCS table, DM-RS additionalPosition) is explicitly out of scope, per the roadmap's own caveat, and needs a second TB-CRC-oracle search stage this plan does not attempt.

**Tech Stack:** C (`nr_pdcch_blind_monitor.c`/`_rt.c`, `dci_nr.c`), gtest, CMake/Make, live validation against the X410 on sens6 with a KNOWN dedicated CORESET (already documented and manually verified in this project) as ground truth.

**Spec:** [Cell-Agnostic Passive Receiver roadmap](https://claude.ai/code/artifact/e1e6ae5d-25f6-4c30-97cb-09f2c0f239a2) (artifact; "Phase 3" section, plus "What no amount of search recovers"). Also read `docs/superpowers/plans/2026-09-04-css0-autoconf-independent-passive-rx.md` (Phase 1's plan, same repo) for the config-derivation conventions this plan extends, and `PHASE1_CSS0_AUTOCONF_HANDOVER.md`'s §2b "REFUTED AND ALREADY ELIMINATED" list before proposing any change to the RT tap's decode chain — several plausible-looking hypotheses there are already measured out.

## Global Constraints

- **Host and repo:** all work happens on **sens6**, in `/home/sens/NICOLA/openairinterface5g-total-passive-ue`, branch `total-passive-rx-UL-DL-graphics` (no local git repo in this environment — every read/edit/build/test/commit goes through `ssh sens6 "..."` or `scp`).
- **Never build while a capture is running.** `pgrep -x nr-uesoftmodem` must return nothing before any `make`.
- **Verify the binary, not the tree** after any build reaching the air: `strings <binary> | grep -c '<a literal just added>'` must be non-zero.
- **`NANT=1` for every capture in this plan** — receive branches 1-3 on this rig are individually undecodable, a pre-existing separate problem this plan must not get entangled with.
- **A CRC pass alone is not evidence of a correct hypothesis at this project's trial volume.** `upper=0x0` (the coarse 8-bit-zero pre-check some code paths use) is only a 1-in-256 test and produced 42k chance passes on 10.9M candidates in this project's own prior work. Every oracle this plan builds MUST additionally require: the decoded payload **varies** between instances (an invariant payload is a degenerate polar fixed point, not traffic — 3744 byte-identical "decodes" were once mistaken for success), and RNTI persistence across multiple sightings (the existing `rnti_persistence_check()` mechanism, reused, not reimplemented).
- **Ground truth already exists on this test cell and must be used, not re-derived from scratch.** ARFCN 630000, PCI 2, 273 PRB/100 MHz, SCS 30 kHz. The dedicated CORESET's true geometry is already manually configured and log-confirmed in `default_regression_103353/run.log` (from Phase 1's work): `coreset(num_groups=45 ... bwp=[0..273))`, matching the conf line `pdcch_blind_monitor_coreset = "45:1:0:0:0:2"` (45 groups × 6 RB = 270 RB, duration 1 symbol, PCI-derived shift/scrambling = 2). This is the answer key Task 5's live validation checks the auto-discovery against — treat any disagreement as a bug in the new code, not in the known-good manual config.
- **`nr_pdcch_blind_monitor_get_cfg()` returns the CONFIGURATION struct** (`g_cfg`, deployment facts read from the conf file or Phase 1's autoconf). Runtime-OBSERVED state this plan adds (a bootstrapped RNTI, a discovered footprint) is a different category and follows this codebase's own existing convention of separate static state with its own accessor (mirroring `g_energy_floor`/`nr_pdcch_blind_monitor_get_cfg()`'s own separation) — do not add observed/discovered fields into `g_cfg` itself.
- **Attribution for every commit in this plan:**
```
Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
```

---

### Task 1: Bootstrap the C-RNTI from the common search space (Technique B)

The smallest task — zero new DSP, pure plumbing that exposes state the blind monitor's accept path already computes but currently discards after logging it.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (near the existing accept path around the `DCIGT` probe / `rnti_utc_ns` computation — search `rnti_persistence_check` for the surrounding accept block)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h` (new accessor declaration)
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_rnti_bootstrap_test.cc`
- Modify: `CMakeLists.txt` (register the new test target)

**Interfaces:**
- Consumes: `nr_pdcch_blind_result_t::rnti` / `::rnti_class` (`nr_blind_rnti_class_t`, existing — `NR_BLIND_RNTI_CLASS_C = 0`, `_TC = 1`, `_SI = 2`, `_RA = 3`, `_P = 4`), the existing `rnti_persistence_check()`.
- Produces: `bool nr_pdcch_blind_monitor_confirmed_rnti(uint32_t now_abs_slot, uint16_t* rnti_out, uint8_t* class_out, uint32_t* age_slots_out)` — returns `true` and fills the three out-params when a persistence-confirmed C-RNTI or TC-RNTI has been observed within a bounded staleness window of `now_abs_slot`; `false` (out-params untouched) otherwise. `now_abs_slot` is an IN parameter (the caller's current absolute slot), not derived internally — this function has no other way to know "now". Task 4 consumes this to narrow the scan's RNTI range.

- [ ] **Step 1: Write the failing test**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_rnti_bootstrap_test.cc" << 'EOF'
/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_rnti_bootstrap_test.cc
 * \brief Phase 3 Technique B (roadmap artifact): the C-RNTI bootstrap accessor, tested against
 * the recorder function directly rather than through a live RT capture -- this state machine has
 * no hardware dependency, so it gets a real unit test instead of only a live smoke check.
 */
#include <gtest/gtest.h>

extern "C" {
#include "nr_pdcch_blind_monitor.h"
}

class RntiBootstrapTest : public ::testing::Test {
 protected:
  void SetUp() override { nr_pdcch_blind_rnti_bootstrap_reset_for_test(); }
};

TEST_F(RntiBootstrapTest, NoConfirmedRntiBeforeAnySighting) {
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(/*now_abs_slot=*/1000, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, OneSightingIsNotEnough) {
  // A single accept must not confirm anything -- persistence is the whole point (a noise accept
  // is almost always a one-off, per rnti_persistence_check()'s own established rationale).
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, /*abs_slot=*/1000);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1000, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, RepeatedSightingsWithinTheWindowConfirm) {
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1010);
  uint16_t rnti = 0; uint8_t cls = 0; uint32_t age = 0;
  ASSERT_TRUE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
  EXPECT_EQ(rnti, 0x4601);
  EXPECT_EQ(cls, NR_BLIND_RNTI_CLASS_C);
}

TEST_F(RntiBootstrapTest, ADifferentRntiDoesNotAccumulateTowardTheFirstOnes) {
  // Two sightings of a WRONG rnti and one of the real one must not confirm the wrong one --
  // matches this project's own already-proven persistence semantics (a real UE's RNTI recurs;
  // a noise accept does not, and different noise accepts do not recur AS EACH OTHER either).
  nr_pdcch_blind_rnti_bootstrap_record(0x1234, NR_BLIND_RNTI_CLASS_C, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0x5678, NR_BLIND_RNTI_CLASS_C, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, SiRntiAndPagingRntiAreNeverAcceptedAsTheDedicatedRnti) {
  // SI-RNTI (0xFFFF) and P-RNTI (0xFFFE) are fixed, spec-known values already handled by Phase 1's
  // CSS0 path -- they carry no information about the DEDICATED C-RNTI this task exists to
  // bootstrap, and accepting one here would feed a nonsense "dedicated RNTI" into Technique C.
  nr_pdcch_blind_rnti_bootstrap_record(0xFFFF, NR_BLIND_RNTI_CLASS_SI, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0xFFFF, NR_BLIND_RNTI_CLASS_SI, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
}

TEST_F(RntiBootstrapTest, AStaleConfirmationExpires) {
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1000);
  nr_pdcch_blind_rnti_bootstrap_record(0x4601, NR_BLIND_RNTI_CLASS_C, 1010);
  uint16_t rnti; uint8_t cls; uint32_t age;
  ASSERT_TRUE(nr_pdcch_blind_monitor_confirmed_rnti(1010, &rnti, &cls, &age));
  // A UE that has gone silent for a long time (moved off this cell, went idle) should not keep
  // anchoring the search to a stale RNTI forever -- 10 seconds of absence at this deployment's
  // ~2000 slots/s is 20000 slots, so asking "now" 20001 slots after the last sighting must
  // report no confirmation at all (not merely a large age).
  EXPECT_FALSE(nr_pdcch_blind_monitor_confirmed_rnti(1010 + 20001, &rnti, &cls, &age));
}
EOF"
```

- [ ] **Step 2: Register the CMake target**

```bash
ssh sens6 "cat >> /home/sens/NICOLA/openairinterface5g-total-passive-ue/CMakeLists.txt" << 'EOF'

  # Phase 3 Technique B (roadmap artifact "Cell-Agnostic Passive Receiver"): C-RNTI bootstrap
  # state machine, tested independently of any live RT capture.
  add_executable(test_nr_pdcch_blind_rnti_bootstrap ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_rnti_bootstrap_test.cc)
  target_include_directories(test_nr_pdcch_blind_rnti_bootstrap PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdcch_blind_rnti_bootstrap PRIVATE nr_pdcch_blind_monitor nr_common MAC_NR_COMMON polar crc_byte UTIL asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs GTest::gtest)
  add_dependencies(tests test_nr_pdcch_blind_rnti_bootstrap)
  add_test(NAME test_nr_pdcch_blind_rnti_bootstrap COMMAND ./test_nr_pdcch_blind_rnti_bootstrap)
EOF"
```

This appends after the existing `test_nr_pdcch_blind_monitor` block (verify with `grep -n test_nr_pdcch_blind_monitor CMakeLists.txt` that the append landed inside the same `if(ENABLE_TESTS)` block those targets live in — if it landed outside, move it manually rather than leaving it unreachable).

- [ ] **Step 3: Run the test to verify it fails**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_pdcch_blind_rnti_bootstrap -j8 2>&1 | tail -30"
```

Expected: compile error — `nr_pdcch_blind_rnti_bootstrap_record`/`_reset_for_test`/`nr_pdcch_blind_monitor_confirmed_rnti` are not declared.

- [ ] **Step 4: Declare the new functions in the header**

In `nr_pdcch_blind_monitor.h`, add near the existing `nr_pdcch_blind_monitor_autoconf_*` declarations:

```c
/* Phase 3 Technique B: bootstrap the dedicated C-RNTI from sightings the accept path already
 * classifies (nr_blind_rnti_class_t). Recording is idempotent and RT-safe (no allocation, no
 * lock) -- call it from the same point the accept path already logs DCIGT. */
void nr_pdcch_blind_rnti_bootstrap_record(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot);

/* Non-zero (true) when a persistence-confirmed C-RNTI or TC-RNTI exists and is not stale as of
 * now_abs_slot (the caller's own current absolute slot -- this function has no other way to know
 * "now"). age_slots_out (now_abs_slot - last sighting) is filled only on a true return. */
bool nr_pdcch_blind_monitor_confirmed_rnti(uint32_t now_abs_slot, uint16_t* rnti_out, uint8_t* class_out,
                                           uint32_t* age_slots_out);

/* Test-only: clears bootstrap state between gtest cases. Not for RT use. */
void nr_pdcch_blind_rnti_bootstrap_reset_for_test(void);
```

- [ ] **Step 5: Implement in `nr_pdcch_blind_monitor_rt.c`**

Add near the top of the file, alongside the other small static state blocks (e.g. near `g_energy_floor`):

```c
// ---- Phase 3 Technique B: C-RNTI bootstrap --------------------------------------------------
// Two consecutive sightings of the SAME rnti confirm it, mirroring rnti_persistence_check()'s own
// "a real UE's RNTI recurs; a noise accept is (almost always) a one-off" rationale -- deliberately
// NOT re-implemented against a persistence window here, because THIS state only needs "have we
// seen this RNTI at least twice, ever" rather than a bounded time window; the age check below is
// what prevents an old confirmation from anchoring the scan forever.
#define RNTI_BOOTSTRAP_STALE_SLOTS 20000u  // ~10 s at this deployment's ~2000 slots/s

static uint16_t g_boot_rnti          = 0;
static uint8_t  g_boot_class         = 0xFF;
static uint32_t g_boot_last_slot     = 0;
static int      g_boot_sightings     = 0;
static uint16_t g_boot_pending_rnti  = 0;
static uint8_t  g_boot_pending_class = 0xFF;

void nr_pdcch_blind_rnti_bootstrap_record(uint16_t rnti, uint8_t rnti_class, uint32_t abs_slot)
{
  // Only C-RNTI and TC-RNTI describe the DEDICATED search space this technique targets; SI-RNTI
  // (Phase 1's own domain) and P-RNTI carry no information about it.
  if (rnti_class != NR_BLIND_RNTI_CLASS_C && rnti_class != NR_BLIND_RNTI_CLASS_TC) {
    return;
  }
  if (rnti == g_boot_pending_rnti && rnti_class == g_boot_pending_class) {
    g_boot_rnti      = rnti;
    g_boot_class     = rnti_class;
    g_boot_last_slot = abs_slot;
    g_boot_sightings++;
  } else if (rnti == g_boot_rnti && rnti_class == g_boot_class) {
    // Already confirmed; a further sighting just refreshes staleness.
    g_boot_last_slot = abs_slot;
  } else {
    // First sighting of a NEW candidate -- park it as pending, do not confirm on one sighting.
    g_boot_pending_rnti  = rnti;
    g_boot_pending_class = rnti_class;
  }
}

bool nr_pdcch_blind_monitor_confirmed_rnti(uint32_t now_abs_slot, uint16_t* rnti_out, uint8_t* class_out,
                                           uint32_t* age_slots_out)
{
  if (g_boot_sightings < 2) {
    return false;
  }
  // now_abs_slot >= g_boot_last_slot always holds in real RT use (slots only advance), but do not
  // assume it in a unit-testable function -- a caller passing an out-of-order "now" gets a benign
  // "not stale" answer via the unsigned-wrap guard below rather than an undefined huge age.
  const uint32_t age = (now_abs_slot >= g_boot_last_slot) ? (now_abs_slot - g_boot_last_slot) : 0;
  if (age > RNTI_BOOTSTRAP_STALE_SLOTS) {
    return false;
  }
  *rnti_out = g_boot_rnti;
  *class_out = g_boot_class;
  *age_slots_out = age;
  return true;
}

void nr_pdcch_blind_rnti_bootstrap_reset_for_test(void)
{
  g_boot_rnti = 0;
  g_boot_class = 0xFF;
  g_boot_last_slot = 0;
  g_boot_sightings = 0;
  g_boot_pending_rnti = 0;
  g_boot_pending_class = 0xFF;
}
```

`RNTI_BOOTSTRAP_STALE_SLOTS` is defined in Step 5's opening block above (`20000u`) — the staleness check above depends on it existing before this function, so keep the two together in one edit rather than adding the constant separately.

- [ ] **Step 6: Wire the recorder into the existing accept path**

In `nr_pdcch_blind_monitor_rt.c`, at the accept path that already computes `out.rnti`/`out.rnti_class` and has just passed `rnti_persistence_check()` (the block containing the `LOG_D(PHY, "SENSING: blind PDCCH accept ...")` line), add immediately after:

```c
    nr_pdcch_blind_rnti_bootstrap_record(out.rnti, out.rnti_class, abs_slot);
```

- [ ] **Step 7: Run the test to verify it passes**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_pdcch_blind_rnti_bootstrap -j8 && ./test_nr_pdcch_blind_rnti_bootstrap"
```

Expected: all 6 tests pass.

- [ ] **Step 8: Confirm no regression in the existing suite**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make nr-uesoftmodem test_nr_pdcch_blind_monitor -j8 && ./test_nr_pdcch_blind_monitor"
```

- [ ] **Step 9: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_rnti_bootstrap_test.cc CMakeLists.txt && git commit -m 'Phase 3 Technique B: bootstrap the dedicated C-RNTI from the accept paths own classification

Exposes state the blind monitor already computes (rnti/rnti_class) but
previously only logged. 2-sighting confirmation mirrors
rnti_persistence_checks own established rationale. SI-/P-RNTI explicitly
excluded -- they carry no information about the dedicated search space.
6 new gtests, no hardware dependency.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 2: Map the dedicated CORESET by DM-RS correlation (Technique A)

Generalizes the existing `nr_pdcch_blind_dmrs_probe` diagnostic (`dci_nr.c`, already live-proven — it is the exact probe that caught real signal Task 4 of the CSS0 plan found the capture-fixture heuristic missing) from "correlate within one assumed CORESET" into "sweep candidate CORESET placements across the whole carrier, no assumption required."

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc`
- Modify: `CMakeLists.txt` (register the new test target)

**Interfaces:**
- Consumes: `nr_gold_pdcch(uint16_t N_RB_DL, uint8_t symbols_per_slot, uint16_t scrambling_id, int slot, int symbol)` and `nr_pdcch_dmrs_ref(const uint32_t* gold, c16_t* pilot, int count)` (both existing, `dci_nr.c`, already the reference-sequence generator the live diagnostic reuses).
- Produces: `int nr_pdcch_coreset_map_scan(const c16_t* rxdataF, int ofdm_symbol_size, int n_rb_carrier, int first_carrier_offset, uint16_t scrambling_id, int slot, int symbol, nr_pdcch_coreset_candidate_t* candidates_out /* caller-sized */, int max_candidates)` — correlates every 6-RB (one-CCE) window across the WHOLE carrier bandwidth at the given symbol against the regenerated PDCCH DM-RS, and returns candidate windows whose `|corr|` clears a fixed, spec-grounded significance bar (see Step 4), sorted by descending correlation. Task 4 consumes this to discover CORESET footprint boundaries without any assumed `rb_offset`/`n_rb`.

- [ ] **Step 1: Write the failing test — a synthetic occupied window against a noise floor**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc" << 'EOF'
/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc
 * \brief Phase 3 Technique A (roadmap artifact): full-carrier DM-RS correlation mapping, tested
 * with a SYNTHETIC symbol containing one genuinely occupied 6-RB window (built by regenerating
 * the exact same DM-RS this function itself uses to detect it -- a closed-loop test, deliberately
 * so: it proves the correlation MATH is self-consistent before any live capture is spent on it,
 * the same discipline nr_pdcch_blind_monitor_test.cc's own RawPayloadRoundTrip group uses).
 */
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
#include <gtest/gtest.h>

extern "C" {
#include "PHY/impl_defs_top.h"
#include "nr_pdcch_coreset_map.h"
uint32_t* nr_gold_pdcch(uint16_t N_RB_DL, uint8_t symbols_per_slot, uint16_t scrambling_id, int slot, int symbol);
void nr_pdcch_dmrs_ref(const uint32_t* gold, c16_t* pilot, int count);
}

TEST(CoresetMap, FindsOneSyntheticallyOccupiedWindowAboveNoise) {
  const int n_rb_carrier = 48;       // small carrier for a fast, deterministic test
  const int ofdm_symbol_size = 512;  // must exceed n_rb_carrier*12 with margin
  const int first_carrier_offset = 10;
  const uint16_t scrambling_id = 2;  // this project's PCI on the reference cell
  const int slot = 3, symbol = 0;

  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(42);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }

  // Plant a real, correctly-generated PDCCH DM-RS at RB offset 18 (a 6-RB window not aligned to
  // either carrier edge, so the test cannot pass by an off-by-one accident at the boundary).
  const int occupied_rb_offset = 18;
  uint32_t* gold = nr_gold_pdcch(n_rb_carrier, 14, scrambling_id, slot, symbol);
  std::vector<c16_t> pilot((occupied_rb_offset + 6) * 3);
  nr_pdcch_dmrs_ref(gold, pilot.data(), (int)pilot.size());
  for (int rb = occupied_rb_offset; rb < occupied_rb_offset + 6; rb++) {
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      // pilot[] is already conj(X); the received symbol at a real DM-RS RE is Y = X (unit
      // channel, no noise added on top of the ambient floor above) -> conj(pilot) recovers X.
      rxdataF[k].r = (int16_t)pilot[rb * 3 + p].r;
      rxdataF[k].i = (int16_t)(-pilot[rb * 3 + p].i);
    }
  }

  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          scrambling_id, slot, symbol, candidates, 16);
  ASSERT_GT(n, 0);
  EXPECT_EQ(candidates[0].rb_offset, occupied_rb_offset);
  EXPECT_GT(candidates[0].corr, 0.7);  // real DM-RS gives ~0.8-0.95 per this project's own measured range
}

TEST(CoresetMap, ReportsNothingOnPureNoise) {
  const int n_rb_carrier = 48, ofdm_symbol_size = 512, first_carrier_offset = 10;
  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto& s : rxdataF) {
    s.r = (int16_t)std::lround(noise(rng));
    s.i = (int16_t)std::lround(noise(rng));
  }
  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          2, 3, 0, candidates, 16);
  // Pure noise gives sqrt(pi)/(2*sqrt(18)) ~= 0.209 per trial (this project's own already-derived
  // figure for an 18-pilot CCE); the significance bar this function uses must sit well above that.
  EXPECT_EQ(n, 0);
}
EOF"
```

- [ ] **Step 2: Register the CMake target**

```bash
ssh sens6 "cat >> /home/sens/NICOLA/openairinterface5g-total-passive-ue/CMakeLists.txt" << 'EOF'

  # Phase 3 Technique A (roadmap artifact "Cell-Agnostic Passive Receiver"): full-carrier PDCCH
  # DM-RS correlation mapping. Closed-loop synthetic test (regenerates its own known-good DM-RS to
  # verify against) plus a pure-noise negative control.
  add_executable(test_nr_pdcch_coreset_map ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc
                                            ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c)
  target_include_directories(test_nr_pdcch_coreset_map PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdcch_coreset_map PRIVATE nr_pdcch_blind_monitor nr_common UTIL GTest::gtest)
  add_dependencies(tests test_nr_pdcch_coreset_map)
  add_test(NAME test_nr_pdcch_coreset_map COMMAND ./test_nr_pdcch_coreset_map)
EOF"
```

`nr_gold_pdcch`/`nr_pdcch_dmrs_ref` live in `dci_nr.c`, which is part of the `nr_pdcch_blind_monitor` (or a shared PHY transport) CMake target already — if the link fails with unresolved symbols for these two functions, add `dci_nr.c`'s owning object/library explicitly rather than guessing; check which existing target (`test_nr_pdcch_blind_monitor` is the reference) already resolves them successfully and match its link line.

- [ ] **Step 3: Run the test to verify it fails**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_pdcch_coreset_map -j8 2>&1 | tail -30"
```

Expected: compile error — `nr_pdcch_coreset_map.h` does not exist.

- [ ] **Step 4: Write the header**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h" << 'EOF'
/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h
 * \brief Phase 3 Technique A: map a dedicated CORESET's footprint by DM-RS correlation across the
 * WHOLE carrier, with no assumed rb_offset/n_rb. Generalizes the existing
 * nr_pdcch_blind_dmrs_probe diagnostic (dci_nr.c), which already proved this correlation approach
 * live -- see PHASE1_CSS0_AUTOCONF_HANDOVER.md's Task 4 finding ("DM-RS corr 0.974" on real
 * signal an independent occupancy heuristic missed entirely).
 *
 * Detection, not decode: no length hypothesis needed, PCI-derived scrambling only (the DEDICATED
 * CORESET's own DM-RS scrambling ID defaults to the PCI unless the network overrides it -- see
 * TS 38.211 7.4.1.3.1 -- and PCI is already known from PSS/SSS on any cell).
 */
#ifndef NR_PDCCH_CORESET_MAP_H
#define NR_PDCCH_CORESET_MAP_H

#include <stdint.h>
#include "PHY/impl_defs_top.h"  // c16_t

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nr_pdcch_coreset_candidate_s {
  int    rb_offset;  ///< First RB of this candidate 6-RB (one-CCE) window
  double corr;        ///< Normalised correlation magnitude, [0,1]
} nr_pdcch_coreset_candidate_t;

/**
 * @brief Correlate every 6-RB window across [0, n_rb_carrier) at one (slot, symbol) against the
 *        regenerated PDCCH DM-RS, returning windows whose |corr| clears the significance bar.
 *
 * @return number of candidates written to candidates_out (<= max_candidates), sorted by
 *         descending corr.
 */
int nr_pdcch_coreset_map_scan(const c16_t* rxdataF,
                              int          ofdm_symbol_size,
                              int          n_rb_carrier,
                              int          first_carrier_offset,
                              uint16_t     scrambling_id,
                              int          slot,
                              int          symbol,
                              nr_pdcch_coreset_candidate_t* candidates_out,
                              int          max_candidates);

#ifdef __cplusplus
}
#endif

#endif
EOF"
```

- [ ] **Step 5: Write the implementation**

Directly generalizes `dci_nr.c`'s existing per-CCE correlation loop (the `nr_pdcch_blind_dmrs_probe` block) from "loop over CCEs of one assumed CORESET" into "loop over every 6-RB window of the whole carrier":

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c" << 'EOF'
#include <math.h>
#include <string.h>
#include "nr_pdcch_coreset_map.h"

extern uint32_t* nr_gold_pdcch(uint16_t N_RB_DL, uint8_t symbols_per_slot, uint16_t scrambling_id, int slot, int symbol);
extern void nr_pdcch_dmrs_ref(const uint32_t* gold, c16_t* pilot, int count);

// Pure noise gives |corr| ~= sqrt(pi)/(2*sqrt(18)) ~= 0.209 for an 18-pilot (6-RB, 3 DM-RS
// RE/RB) window -- this project's own already-derived figure (dci_nr.c's nr_pdcch_blind_dmrs_probe
// comment). A real DM-RS measured 0.8-0.95 live. Set the bar at 4x the noise floor (~0.836), well
// clear of noise, comfortably below a real hit -- NOT at 0.5x(noise+signal), because the noise
// distribution's own upper tail (not just its mean) is what a real significance bar must clear;
// see dci_nr.c's own comment on why "max over many trials grows only as sqrt(ln(trials)/18)".
#define CORESET_MAP_CORR_THRESHOLD 0.836

int nr_pdcch_coreset_map_scan(const c16_t* rxdataF,
                              int          ofdm_symbol_size,
                              int          n_rb_carrier,
                              int          first_carrier_offset,
                              uint16_t     scrambling_id,
                              int          slot,
                              int          symbol,
                              nr_pdcch_coreset_candidate_t* candidates_out,
                              int          max_candidates)
{
  const int n_windows = n_rb_carrier / 6;
  if (n_windows <= 0 || max_candidates <= 0) {
    return 0;
  }

  uint32_t* gold = nr_gold_pdcch((uint16_t)n_rb_carrier, 14, scrambling_id, slot, symbol);
  c16_t pilot[n_rb_carrier * 3];
  nr_pdcch_dmrs_ref(gold, pilot, n_rb_carrier * 3);

  int found = 0;
  for (int w = 0; w < n_windows && found < max_candidates; w++) {
    const int rb_offset = w * 6;
    double cr = 0.0, ci = 0.0, py = 0.0, px = 0.0;
    for (int rb = rb_offset; rb < rb_offset + 6; rb++) {
      for (int p = 0; p < 3; p++) {
        const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
        const c16_t y = rxdataF[k];
        const c16_t x = pilot[rb * 3 + p];  // already conj(transmitted DM-RS)
        cr += (double)y.r * x.r - (double)y.i * x.i;
        ci += (double)y.r * x.i + (double)y.i * x.r;
        py += (double)y.r * y.r + (double)y.i * y.i;
        px += (double)x.r * x.r + (double)x.i * x.i;
      }
    }
    const double denom = sqrt(py * px);
    const double corr = (denom > 0.0) ? sqrt(cr * cr + ci * ci) / denom : 0.0;
    if (corr >= CORESET_MAP_CORR_THRESHOLD) {
      candidates_out[found].rb_offset = rb_offset;
      candidates_out[found].corr      = corr;
      found++;
    }
  }

  // Insertion sort by descending corr -- found is small (<= max_candidates), no need for qsort.
  for (int i = 1; i < found; i++) {
    nr_pdcch_coreset_candidate_t v = candidates_out[i];
    int j = i - 1;
    while (j >= 0 && candidates_out[j].corr < v.corr) {
      candidates_out[j + 1] = candidates_out[j];
      j--;
    }
    candidates_out[j + 1] = v;
  }
  return found;
}
EOF"
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_pdcch_coreset_map -j8 && ./test_nr_pdcch_coreset_map"
```

Expected: `[==========] 2 tests from 1 test suite ran. [ PASSED ] 2 tests.` If `FindsOneSyntheticallyOccupiedWindowAboveNoise` fails on the exact `rb_offset`, first suspect an off-by-one in the `k` indexing formula relative to `dci_nr.c`'s original (re-read the source block this generalizes and diff the two formulas character-by-character before changing the threshold).

- [ ] **Step 7: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_map.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_coreset_map_test.cc CMakeLists.txt && git commit -m 'Phase 3 Technique A: full-carrier PDCCH DM-RS correlation mapping

Generalizes the existing nr_pdcch_blind_dmrs_probe diagnostic (dci_nr.c,
already live-proven -- see PHASE1_CSS0_AUTOCONF_HANDOVER.md Task 4s
DM-RS corr 0.974 finding) from scoring one assumed CORESET into sweeping
every 6-RB window of the whole carrier. Detection, not decode: PCI-derived
scrambling only, no length hypothesis needed. Closed-loop synthetic test
(self-generates the DM-RS it detects) plus a pure-noise negative control.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 3: Lock `dci_length` by histogram (Technique C)

Sweeps the one genuinely unknown, genuinely small quantity — DCI 1_1's payload length — once at startup, using the SAME false-accept-resistant oracle this project has already built and proven (persistence + payload variance), not a new one.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc`
- Modify: `CMakeLists.txt` (register the new test target)

**Interfaces:**
- Consumes: `nr_pdcch_blind_dci_size_ex()` (existing, `nr_pdcch_blind_monitor.h`), the existing decode-and-extract entry point (`nr_pdcch_blind_decode_and_extract` / `_ex`, `nr_pdcch_blind_monitor.h` — read its exact current signature before wiring, it takes `dci_length` as a parameter already), and Task 1's `nr_pdcch_blind_monitor_confirmed_rnti()` (used to score a length hypothesis by whether decodes at that length land ON the bootstrapped RNTI more often than chance, when a bootstrap is available — falls back to "payload varies + upper-8-bits-zero" scoring alone when it is not).
- Produces: `int nr_pdcch_dci_length_sweep(nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx, int min_len, int max_len, uint16_t bootstrap_rnti /* 0 = none */)` — returns the `dci_length` value whose decode population scores best (highest confirmed-hit rate), or `-1` if nothing clears the significance bar across the whole swept range. `decode_one_candidate` is a caller-supplied function pointer so this module stays decoupled from the RT monitor's own candidate iteration (dependency injection, not a new copy of the candidate loop) — Task 4 supplies the real one.

- [ ] **Step 1: Write the failing test — a scorer stub with a known-best length**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc" << 'EOF'
/*! \file openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc
 * \brief Phase 3 Technique C (roadmap artifact): the dci_length sweep's SELECTION logic, tested
 * against a synthetic scorer stub so the test needs no real candidate stream. This deliberately
 * tests the two false-accept traps this project has already paid for once (see
 * nr_pdcch_dci_length_sweep.c's own header comment): a length that only ever produces an
 * upper-8-bits-zero pass is NOT enough (1-in-256 test alone produced 42k chance passes on 10.9M
 * candidates in this project's prior work), and an INVARIANT payload at some length is a
 * degenerate polar fixed point, not a real length -- both must be rejected even though they would
 * pass a naive "any CRC-adjacent pass" check.
 */
#include <cstdint>
#include <cstring>
#include <map>
#include <gtest/gtest.h>

extern "C" {
#include "nr_pdcch_dci_length_sweep.h"
}

// Synthetic scorer: length 47 produces varying payloads with 3 hitting the bootstrap RNTI (a
// realistic, non-degenerate signal); length 39 produces the SAME payload every time (a degenerate
// polar fixed point -- must be rejected even though its upper-8-bits are chosen to look promising);
// every other length in range produces pure chance noise.
struct SweepFixture {
  int         calls_at_len[128] = {};
  static bool decode(int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void* ctx) {
    SweepFixture* f = static_cast<SweepFixture*>(ctx);
    f->calls_at_len[dci_length]++;
    if (dci_length == 47) {
      *rnti_out = (trial_idx % 5 == 0) ? 0x4601 : (uint16_t)(0x1000 + trial_idx);
      *payload_hash_out = 1000u + (uint32_t)trial_idx;  // varies every trial
      return (trial_idx % 5 == 0) || ((trial_idx * 2654435761u) % 256 == 0);
    }
    if (dci_length == 39) {
      *rnti_out = 0x4601;             // looks tempting on RNTI alone
      *payload_hash_out = 42u;        // but INVARIANT -- degenerate fixed point
      return (trial_idx % 3 == 0);
    }
    // Everywhere else: pure 1/256 chance, never the bootstrap RNTI.
    *rnti_out = (uint16_t)(0x2000 + trial_idx);
    *payload_hash_out = 5000u + (uint32_t)trial_idx;
    return ((trial_idx * 2654435761u) % 256 == 0);
  }
};

TEST(DciLengthSweep, PicksTheLengthWithVaryingPayloadsAndBootstrapHits) {
  SweepFixture fx;
  const int best = nr_pdcch_dci_length_sweep(&SweepFixture::decode, &fx, /*min_len=*/30, /*max_len=*/70,
                                             /*bootstrap_rnti=*/0x4601);
  EXPECT_EQ(best, 47);
}

TEST(DciLengthSweep, RejectsAnInvariantPayloadDespiteMatchingTheBootstrapRnti) {
  // Regression guard for the exact trap this project has already hit: length 39 matches the
  // bootstrap RNTI on every trial, which a naive "does it hit the known RNTI" scorer would love --
  // but every trial decodes to the IDENTICAL payload_hash, which is the degenerate-fixed-point
  // signature this function must reject.
  SweepFixture fx;
  const int best = nr_pdcch_dci_length_sweep(&SweepFixture::decode, &fx, 30, 70, 0x4601);
  EXPECT_NE(best, 39);
}

TEST(DciLengthSweep, ReturnsMinusOneWhenNoLengthClearsSignificance) {
  // A scorer where NOTHING is ever real -- every length is pure chance noise.
  auto pure_noise = [](int, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void*) -> bool {
    *rnti_out = (uint16_t)(0x3000 + trial_idx);
    *payload_hash_out = 9000u + (uint32_t)trial_idx;
    return ((trial_idx * 2654435761u) % 256 == 0);
  };
  const int best = nr_pdcch_dci_length_sweep(pure_noise, nullptr, 30, 70, 0x9999 /* never seen */);
  EXPECT_EQ(best, -1);
}
EOF"
```

- [ ] **Step 2: Register the CMake target**

```bash
ssh sens6 "cat >> /home/sens/NICOLA/openairinterface5g-total-passive-ue/CMakeLists.txt" << 'EOF'

  # Phase 3 Technique C (roadmap artifact "Cell-Agnostic Passive Receiver"): dci_length histogram
  # sweep, tested against a synthetic scorer stub -- exercises both false-accept traps this
  # project has already paid for once (upper-8-bits-zero alone, and a degenerate invariant
  # payload) via a fixture engineered to tempt each of them independently.
  add_executable(test_nr_pdcch_dci_length_sweep ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc
                                                 ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c)
  target_include_directories(test_nr_pdcch_dci_length_sweep PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdcch_dci_length_sweep PRIVATE UTIL GTest::gtest)
  add_dependencies(tests test_nr_pdcch_dci_length_sweep)
  add_test(NAME test_nr_pdcch_dci_length_sweep COMMAND ./test_nr_pdcch_dci_length_sweep)
EOF"
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_pdcch_dci_length_sweep -j8 2>&1 | tail -30"
```

Expected: compile error — `nr_pdcch_dci_length_sweep.h` does not exist.

- [ ] **Step 4: Write the header**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h" << 'EOF'
/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h
 * \brief Phase 3 Technique C: lock dci_length by histogram, once at startup. DCI 1_1 lengths land
 * in roughly a 30-70 bit range (roadmap artifact), so a linear sweep is ~40 passes.
 *
 * TWO TRAPS ALREADY PAID FOR IN THIS PROJECT, both of which this module's scoring must reject
 * (see PHASE1_CSS0_AUTOCONF_HANDOVER.md and TOTAL_PASSIVE_UE_HANDOVER.md for the history):
 *  1. `upper 8 bits == 0` alone is a 1-in-256 test -- produced 42k chance passes on 10.9M
 *     candidates. Never score a length by pass-count alone.
 *  2. A spiked histogram at some length is NOT sufficient if the decoded payload is INVARIANT
 *     across instances -- that is a degenerate polar fixed point, not traffic (3744 byte-identical
 *     "decodes" were once mistaken for a working length).
 */
#ifndef NR_PDCCH_DCI_LENGTH_SWEEP_H
#define NR_PDCCH_DCI_LENGTH_SWEEP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attempt one candidate decode at the given dci_length.
 *
 * @param dci_length       Hypothesis under test.
 * @param trial_idx        Monotonically increasing trial counter, unique across the WHOLE sweep
 *                          (not per-length) -- lets a deterministic test fixture vary its
 *                          synthetic output without depending on real candidate timing.
 * @param rnti_out         CRC-recovered RNTI, filled only if the caller returns true.
 * @param payload_hash_out A cheap hash/fingerprint of the decoded payload bits, filled only if the
 *                          caller returns true -- used to detect an invariant (degenerate) payload.
 * @param user_ctx          Opaque, passed through unchanged.
 * @return true iff this trial's CRC-adjacent check passed (e.g. upper 8 bits zero) -- the SWEEP,
 *         not this callback, decides whether the length is real.
 */
typedef bool (*nr_pdcch_dci_length_scorer_fn)(int dci_length, int trial_idx, uint16_t* rnti_out,
                                              uint32_t* payload_hash_out, void* user_ctx);

/**
 * @brief Sweep [min_len, max_len], TRIALS_PER_LENGTH candidates per hypothesis, and return the
 *        length whose population is BOTH statistically significant above chance AND shows a
 *        varying payload (rejects the degenerate-fixed-point trap) -- optionally weighted toward
 *        hitting bootstrap_rnti when one is available (0 = none, score on variance alone).
 *
 * @return the winning dci_length, or -1 if nothing in range clears significance.
 */
int nr_pdcch_dci_length_sweep(nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                              int min_len, int max_len, uint16_t bootstrap_rnti);

#ifdef __cplusplus
}
#endif

#endif
EOF"
```

- [ ] **Step 5: Write the implementation**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c" << 'EOF'
#include <stdlib.h>
#include "nr_pdcch_dci_length_sweep.h"

// Trials per length hypothesis. Enough that a real length (which should land the bootstrap RNTI
// or vary richly) separates clearly from a length producing only 1/256 chance passes.
#define TRIALS_PER_LENGTH 64
#define MAX_HASHES_TRACKED 32  // cap on distinct-payload bookkeeping per length; degenerate
                                // detection only needs to distinguish "1 distinct value" from
                                // "more than 1", so this never needs to be large.

int nr_pdcch_dci_length_sweep(nr_pdcch_dci_length_scorer_fn decode_one_candidate, void* user_ctx,
                              int min_len, int max_len, uint16_t bootstrap_rnti)
{
  int best_len = -1;
  double best_score = 0.0;
  int trial_idx = 0;

  for (int len = min_len; len <= max_len; len++) {
    int      passes = 0;
    int      bootstrap_hits = 0;
    uint32_t hashes[MAX_HASHES_TRACKED];
    int      n_distinct = 0;

    for (int t = 0; t < TRIALS_PER_LENGTH; t++, trial_idx++) {
      uint16_t rnti = 0;
      uint32_t payload_hash = 0;
      if (!decode_one_candidate(len, trial_idx, &rnti, &payload_hash, user_ctx)) {
        continue;
      }
      passes++;
      if (bootstrap_rnti != 0 && rnti == bootstrap_rnti) {
        bootstrap_hits++;
      }
      // Track distinct payload hashes seen at this length, capped -- rejects the degenerate
      // fixed-point trap (n_distinct stays at 1 despite many passes).
      bool seen = false;
      for (int h = 0; h < n_distinct; h++) {
        if (hashes[h] == payload_hash) {
          seen = true;
          break;
        }
      }
      if (!seen && n_distinct < MAX_HASHES_TRACKED) {
        hashes[n_distinct++] = payload_hash;
      }
    }

    if (passes == 0) {
      continue;
    }
    // Degenerate fixed point: many passes, but every one decodes to the SAME payload. Reject
    // outright regardless of how many bootstrap hits it racked up (Technique C's own header
    // comment: trap #2).
    if (n_distinct <= 1 && passes >= 3) {
      continue;
    }
    // Chance floor for TRIALS_PER_LENGTH trials at 1/256 each is small (~0.25 expected passes);
    // require CLEARING it by a wide margin AND favor bootstrap hits when available.
    const double expected_chance_passes = (double)TRIALS_PER_LENGTH / 256.0;
    if ((double)passes < 3.0 * expected_chance_passes && bootstrap_hits == 0) {
      continue;  // not clearly above chance, and nothing tying it to a known-real RNTI
    }
    // Score: bootstrap hits dominate (a real length landing the KNOWN rnti is near-certain
    // evidence); payload variance is the tiebreak/floor signal when no bootstrap is available.
    const double score = (double)bootstrap_hits * 100.0 + (double)n_distinct;
    if (score > best_score) {
      best_score = score;
      best_len   = len;
    }
  }
  return best_len;
}
EOF"
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make test_nr_pdcch_dci_length_sweep -j8 && ./test_nr_pdcch_dci_length_sweep"
```

Expected: `[==========] 3 tests from 1 test suite ran. [ PASSED ] 3 tests.` If `RejectsAnInvariantPayloadDespiteMatchingTheBootstrapRnti` fails (length 39 wins), the degenerate-fixed-point check is not actually excluding it before the scoring step — re-check the `continue` ordering, not the scoring weights.

- [ ] **Step 7: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_dci_length_sweep_test.cc CMakeLists.txt && git commit -m 'Phase 3 Technique C: dci_length histogram sweep, rejecting both known false-accept traps

Dependency-injected scorer (no coupling to the RT candidate loop -- Task 4
supplies the real one). Synthetic test fixture specifically engineers a
temptation for each of the two traps this project already paid for once:
upper-8-bits-zero alone (1-in-256, produced 42k chance passes on 10.9M
candidates previously) and an invariant degenerate-fixed-point payload
that would otherwise win on bootstrap-RNTI-hit-rate alone.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 4: Wire the three techniques into an auto-discovery pipeline

Integrates Tasks 1-3 behind a new opt-in config flag, mirroring Phase 1's `pdcch_blind_monitor_autoconf` pattern exactly (default off, every existing deployment untouched).

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` (new config param + orchestration function)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (call the orchestration at the point CORESET config is otherwise missing under autodiscover)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h` (new declarations)

**Interfaces:**
- Consumes: `nr_pdcch_coreset_map_scan()` (Task 2), `nr_pdcch_blind_monitor_confirmed_rnti()` (Task 1), `nr_pdcch_dci_length_sweep()` (Task 3), and the SAME `have_manual_coreset`/`g_cfg` structure Phase 1's plan already established.
- Produces: a new conf knob `pdcch_blind_monitor_autodiscover = 1` (default 0). When on and no manual dedicated-CORESET config is present, the monitor: (a) runs Technique A across the whole carrier on a rolling basis until a stable footprint (same `rb_offset` cluster winning across N consecutive symbols) emerges, (b) once a footprint is found, runs Technique C's sweep scoped to that footprint's implied `n_rb`, using Task 1's bootstrap RNTI (from Phase 1's common-search-space grants) if one is available, (c) on success, populates `g_cfg` exactly as `nr_pdcch_blind_monitor_autoconf_css0()` does for CORESET#0, so every downstream consumer (the RT tap, the reconciliation warning) is unaware of WHICH path populated it.

- [ ] **Step 1: Read the exact current decode-and-extract signature before wiring the scorer**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && grep -n 'nr_pdcch_blind_decode_and_extract' openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
```

Confirm the parameter order/types (this header was read in full during earlier grounding work for this plan and takes `llr`, `aggregation_level`, `dci_length`, `bwp_size`, `dmrs_typeA_position`, and returns `nr_pdcch_blind_result_t`) — do not assume the plan's earlier paraphrase is exact; write the scorer adapter (Step 3 below) against the real signature.

- [ ] **Step 2: Add the config knob**

In `nr_pdcch_blind_monitor.c`, add near the existing `pdcch_blind_monitor_autoconf` param registration:

```c
      {"pdcch_blind_monitor_autodiscover",
        "Phase 3: recover the DEDICATED CORESET/search space by DM-RS correlation + dci_length "
        "sweep instead of reading pdcch_blind_monitor_coreset/_ss/_bwp. Default 0. Requires "
        "pdcch_blind_monitor_autoconf=1 (Phase 1) to have a working common search space first -- "
        "the bootstrap RNTI comes from THAT path's own grants.",
        0, .iptr = &g_cfg.autodiscover, .defintval = 0, TYPE_INT, 0},
```

Add `int autodiscover;` to the `nr_pdcch_blind_monitor_cfg_t` struct in `nr_pdcch_blind_monitor_rt.h`, next to the existing `autoconf` field.

- [ ] **Step 3: Write the orchestration function**

In `nr_pdcch_blind_monitor.c`, add (this is a substantial function — write it incrementally and build after each piece rather than all at once):

```c
/* Phase 3: orchestrate Techniques A/B/C into a self-contained "recover the dedicated CORESET"
 * attempt. Called from the RT path (nr_pdcch_blind_monitor_rt.c) once per symbol while
 * autodiscover is on AND no manual/autoconf'd dedicated config exists yet. Cheap to call
 * repeatedly -- it is a no-op once g_cfg is populated (checked by the caller, not here, so this
 * function's own logic stays simple: "try once, report success/failure"). */
typedef struct { int rb_offset_votes[NR_PDCCH_MAX_CANDIDATE_WINDOWS]; int n_votes; } nr_pdcch_autodiscover_state_t;
static nr_pdcch_autodiscover_state_t g_discover_state = {0};

#define AUTODISCOVER_STABLE_VOTES 5  // consecutive symbols agreeing on the same rb_offset cluster

bool nr_pdcch_blind_monitor_autodiscover_step(const c16_t* rxdataF, int ofdm_symbol_size, int n_rb_carrier,
                                              int first_carrier_offset, uint16_t pci, int slot, int symbol,
                                              uint32_t abs_slot)
{
  nr_pdcch_coreset_candidate_t candidates[16];
  const int n = nr_pdcch_coreset_map_scan(rxdataF, ofdm_symbol_size, n_rb_carrier, first_carrier_offset,
                                          pci, slot, symbol, candidates, 16);
  if (n == 0) {
    return false;  // nothing occupied this symbol -- not an error, most symbols carry no PDCCH
  }
  // Vote for the STRONGEST candidate window this symbol. A single symbol is not enough (the
  // roadmap's own figure schematic shows the footprint spanning multiple slots/symbols); require
  // AUTODISCOVER_STABLE_VOTES consecutive agreeing votes before trusting it, the same discipline
  // rnti_persistence_check() applies to RNTI sightings.
  static int s_last_rb_offset = -1;
  static int s_consecutive = 0;
  if (candidates[0].rb_offset == s_last_rb_offset) {
    s_consecutive++;
  } else {
    s_last_rb_offset = candidates[0].rb_offset;
    s_consecutive = 1;
  }
  if (s_consecutive < AUTODISCOVER_STABLE_VOTES) {
    return false;
  }

  // Footprint stable -- how many CONSECUTIVE occupied 6-RB windows does it span, starting at
  // s_last_rb_offset? Reuse the SAME `candidates[]` array already computed above -- one scan
  // already covers the whole carrier at this (slot, symbol), so re-invoking
  // nr_pdcch_coreset_map_scan() again here would recompute all n_rb_carrier/6 windows a second
  // time for no new information (it is a pure function of the same inputs).
  int span_rb = 6;
  for (int w = 1; w < n_rb_carrier / 6; w++) {
    bool extends = false;
    for (int c = 0; c < n; c++) {
      if (candidates[c].rb_offset == s_last_rb_offset + span_rb) {
        extends = true;
        break;
      }
    }
    if (!extends) {
      break;
    }
    span_rb += 6;
  }

  g_cfg.coreset_type            = 0;  // PDCCH-Config (dedicated), NOT MIB/SIB1 -- see coreset_type's
                                        // own comment in autoconf_css0() for why this field matters
  g_cfg.coreset_freq_domain     = span_rb / 6;
  g_cfg.bwp_start               = s_last_rb_offset;
  g_cfg.bwp_size                = span_rb;
  g_cfg.coreset_pdcch_dmrs_scrambling_id = pci;
  g_cfg.coreset_shift_index     = pci;

  uint16_t bootstrap_rnti = 0;
  uint8_t  bootstrap_class = 0xFF;
  uint32_t age = 0;
  nr_pdcch_blind_monitor_confirmed_rnti(abs_slot, &bootstrap_rnti, &bootstrap_class, &age);

  LOG_A(PHY, "SENSING: Phase 3 autodiscover -- CORESET footprint rb_offset=%d span_rb=%d "
            "bootstrap_rnti=0x%x\n", s_last_rb_offset, span_rb, bootstrap_rnti);
  return true;  // g_cfg's CORESET fields are now populated; dci_length sweep is the caller's next step
}
```

**This step deliberately does NOT call `nr_pdcch_dci_length_sweep()` inline.** The sweep needs a live decode loop over real candidates (Step 4), which only the RT thread can drive — keep this function's job to "discover CORESET geometry" and let the RT tap (Step 4) drive the length sweep using this function's output as the search space it decodes candidates within. Define `NR_PDCCH_MAX_CANDIDATE_WINDOWS` as `(273 / 6)` (46, generous headroom for a 273 PRB carrier) near the struct, and add the new `abs_slot` parameter to this function's own declaration wherever it is forward-declared alongside the definition.

- [ ] **Step 4: Wire the RT tap**

In `nr_pdcch_blind_monitor_rt.c`, near wherever the scan loop currently checks `nr_pdcch_blind_monitor_enabled()`, add a branch (before the normal candidate loop) that runs while `g_cfg.autodiscover && !have_manual_coreset_equivalent_populated`:

```c
  const uint32_t abs_slot_now = proc->frame_rx * fp->slots_per_frame + proc->nr_slot_rx;
  if (cfg->autodiscover && g_cfg.bwp_size == 0) {
    // Geometry not yet discovered -- run Technique A this symbol, do nothing else.
    nr_pdcch_blind_monitor_autodiscover_step(rxdataF[0], fp->ofdm_symbol_size, fp->N_RB_DL,
                                             fp->first_carrier_offset, (uint16_t)fp->Nid_cell,
                                             proc->nr_slot_rx, symbol, abs_slot_now);
    return;  // geometry not ready -- do not attempt candidate decode this call
  }
  if (cfg->autodiscover && g_cfg.bwp_size != 0 && !g_length_swept) {
    // Geometry known, length not yet locked -- run Technique C using this module's OWN candidate
    // decode path as the scorer (adapter matching nr_pdcch_dci_length_scorer_fn's signature; call
    // nr_pdcch_blind_decode_and_extract_ex() at each hypothesized length over g_cfg's now-known
    // CORESET, per Step 1's confirmed real signature).
    uint16_t bootstrap_rnti = 0; uint8_t bootstrap_class = 0xFF; uint32_t age = 0;
    nr_pdcch_blind_monitor_confirmed_rnti(abs_slot_now, &bootstrap_rnti, &bootstrap_class, &age);
    const int found_len = nr_pdcch_dci_length_sweep(nr_pdcch_autodiscover_length_scorer, /*user_ctx=*/NULL,
                                                    30, 70, bootstrap_rnti);
    if (found_len > 0) {
      g_cfg.dci_length_override = found_len;
      g_length_swept = true;
      LOG_A(PHY, "SENSING: Phase 3 autodiscover -- dci_length locked at %d\n", found_len);
    }
  }
```

**Before writing `nr_pdcch_autodiscover_length_scorer()` (the adapter this block calls), read the exact candidate-LLR-sourcing mechanism the existing scan loop already uses — do not invent a parallel one:**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && grep -n 'e_rx_cand_idx\|pdcch_e_rx\[' openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c | head -20"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && sed -n '960,1010p' openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c"
```

The existing candidate loop indexes into `pdcch_e_rx` via a running `e_rx_cand_idx` that advances by `n_re_cand` per candidate — the adapter must read a candidate from THIS SAME cursor (the one candidate this scan call is currently positioned at when the sweep is invoked), not re-derive a second independent indexing scheme. `nr_pdcch_autodiscover_length_scorer(int dci_length, int trial_idx, uint16_t* rnti_out, uint32_t* payload_hash_out, void* user_ctx)` should: (1) call `nr_pdcch_blind_decode_and_extract_ex()` (Step 1's confirmed signature) with `dci_length` substituted for whatever the caller would otherwise use, over the CURRENT candidate's LLR slice from that same cursor; (2) on a plausible/CRC-passing result, fill `*rnti_out` from the decoded RNTI and `*payload_hash_out` from a cheap hash of the extracted field struct (e.g. `start_rb`, `num_rb`, `mcs`, `rv` packed into one `uint32_t` — any deterministic function of the decode that varies with genuine traffic and stays constant for a degenerate fixed point is sufficient, per Technique C's own test fixture); (3) return whether the CRC-adjacent check passed, exactly as `nr_pdcch_dci_length_scorer_fn`'s doc comment specifies. `trial_idx` in real RT use is simply "how many times this scorer has been called so far in the current sweep" — a local counter in the adapter, not tied to any existing candidate-index concept. Declare a static `bool g_length_swept = false;` alongside `g_discover_state`.

- [ ] **Step 5: Build and verify**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && make nr-uesoftmodem test_nr_pdcch_blind_monitor test_nr_pdcch_coreset_map test_nr_pdcch_dci_length_sweep test_nr_pdcch_blind_rnti_bootstrap -j8 2>&1 | tail -50"
ssh sens6 "strings /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build/nr-uesoftmodem | grep -c 'Phase 3 autodiscover'"
```

Expected: clean build across all four new/touched test targets, non-zero `strings` count.

- [ ] **Step 6: Run all four test suites**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build && ./test_nr_pdcch_blind_monitor && ./test_nr_pdcch_coreset_map && ./test_nr_pdcch_dci_length_sweep && ./test_nr_pdcch_blind_rnti_bootstrap"
```

- [ ] **Step 7: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h && git commit -m 'Phase 3: wire Techniques A/B/C into pdcch_blind_monitor_autodiscover

Default OFF (0), every existing deployment untouched -- same rule Phase 1
established for pdcch_blind_monitor_autoconf. Requires autoconf=1 first
(the bootstrap RNTI comes from the common search spaces own grants).
On success, populates g_cfg identically to autoconf_css0(), so every
downstream consumer is unaware which path populated it.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```

---

### Task 5: Live validation against known ground truth

Runs full autodiscovery on sens6's own cell, whose dedicated CORESET geometry is ALREADY manually configured and log-confirmed — an answer key this project does not need a second cell to obtain.

**Files:**
- Modify: `/home/sens/NICOLA/nrue.passive_rx.autoconf.conf` (or a new sibling conf) — add `pdcch_blind_monitor_autodiscover = 1`, remove the manual `pdcch_blind_monitor_coreset`/`_ss`/`_bwp` DEDICATED-search-space lines this conf may still carry for the C-RNTI-fallback path (Phase 1's own conf comments already document this dual-path structure)
- Create: `PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md` (repo root, matching this project's per-phase handover convention)

**Interfaces:**
- Consumes: Task 4's `pdcch_blind_monitor_autodiscover`.
- Produces: nothing for a later task; this is the closing validation.

- [ ] **Step 1: Confirm no capture is running, then run with autodiscover on**

```bash
ssh sens6 "pgrep -x nr-uesoftmodem" && echo "CAPTURE RUNNING - STOP" || echo "clear"
ssh sens6 "cp /home/sens/NICOLA/nrue.passive_rx.selfconf.conf /home/sens/NICOLA/nrue.passive_rx.autodiscover.conf"
ssh sens6 "grep -nE 'pdcch_blind_monitor_(coreset|ss|bwp) *=' /home/sens/NICOLA/nrue.passive_rx.autodiscover.conf"
```

Confirm this conf carries NONE of the three manual dedicated-CORESET lines (it is built from `selfconf.conf`, Phase 1's own zero-manual-lines conf) before adding the autodiscover flag:

```bash
ssh sens6 "echo '  pdcch_blind_monitor_autodiscover = 1;' >> /home/sens/NICOLA/nrue.passive_rx.autodiscover.conf"
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures && ARM=autodiscover CONF=/home/sens/NICOLA/nrue.passive_rx.autodiscover.conf DUR=180 TRIES=1 NANT=1 FULLCRC=1 ./run_arm.sh"
```

- [ ] **Step 2: Score the discovered footprint against the KNOWN answer**

```bash
ssh sens6 "L=\$(ls -dt /home/sens/NICOLA/captures/autodiscover_*/ | head -1)/run.log; grep -aoE 'Phase 3 autodiscover -- CORESET footprint .{0,120}' \"\$L\" | tail -3"
```

**Pass:** `rb_offset=0 span_rb=270` (matching the known-good manual config, `num_groups=45` × 6 RB = 270 RB, `bwp_start=0`). Any other footprint is a real bug in Task 2's correlation math or Task 4's stability-voting logic — do not adjust the significance threshold to make a wrong answer pass; re-derive against the manual config's own log line instead (`default_regression_103353/run.log`'s `coreset(num_groups=45 ... bwp=[0..273))`).

- [ ] **Step 3: Score the discovered `dci_length` against the known answer**

```bash
ssh sens6 "L=\$(ls -dt /home/sens/NICOLA/captures/autodiscover_*/ | head -1)/run.log; grep -aoE 'Phase 3 autodiscover -- dci_length locked .{0,60}' \"\$L\" | tail -3"
```

**Pass:** the locked length matches this cell's already-established `dci_length=48` at 273 PRB (confirmed live in Phase 1's session record via the gNB's own `"DCI format 1 size: 48"` log line). If the sweep locks a different length, check whether `bootstrap_rnti` was actually available at that point in the run (Task 1's confirmation needs the common search space to have produced at least 2 sightings first — if the sweep ran before any bootstrap existed, it is scoring on payload-variance alone, which is weaker; re-read the run's timeline before concluding the sweep logic itself is wrong).

- [ ] **Step 4: Cross-check FULLCRC decodes against the bootstrapped RNTI**

```bash
ssh sens6 "L=\$(ls -dt /home/sens/NICOLA/captures/autodiscover_*/ | head -1)/run.log; grep -ac FULLCRC \"\$L\"; grep -aoE 'FULLCRC .{0,110}' \"\$L\" | sort -u | wc -l"
```

Score by this plan's own three-part standard (established throughout this project): `upper=0x0`, payload varies between instances, candidate survived any active gate. **A non-zero genuine-decode count here — at `crc=` matching Task 1's bootstrapped RNTI specifically — is the actual end-to-end proof this phase works.** If zero, this is informative, not necessarily a failure of THIS plan's own code: re-read `PHASE1_CSS0_AUTOCONF_HANDOVER.md`'s "still open" section (the blind monitor's own separate demapping/decode-chain reimplementation was flagged there as the likely remaining seam even after config is proven correct) before assuming Phase 3's discovery logic is at fault — the discovered CORESET geometry and dci_length being CORRECT (Steps 2-3) is this task's real deliverable; genuine decode volume is a downstream question this plan's own scope does not extend to fixing.

- [ ] **Step 5: Write the handover document**

```bash
ssh sens6 "cat > /home/sens/NICOLA/openairinterface5g-total-passive-ue/PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md" << 'EOF'
# Phase 3 — recover the dedicated config by search — HANDOVER

**Status: [fill in from Steps 2-4's actual results before committing this doc]**
**Branch:** total-passive-rx-UL-DL-graphics
**Host:** sens6 · **Repo:** /home/sens/NICOLA/openairinterface5g-total-passive-ue

## What this phase adds

pdcch_blind_monitor_autodiscover: recovers the dedicated CORESET geometry (Technique A, DM-RS
correlation across the whole carrier) and DCI 1_1 payload length (Technique C, histogram sweep)
by search, keyed by a bootstrapped C-RNTI (Technique B, from Phase 1's common search space) rather
than requiring pdcch_blind_monitor_coreset/_ss/_bwp/dci_length_override to be hand-derived from a
gNB log. Default off; requires pdcch_blind_monitor_autoconf=1 (Phase 1) already on.

## Measured [fill in with real numbers from Steps 1-4]

- Discovered CORESET footprint: rb_offset=___ span_rb=___ (known-good: rb_offset=0 span_rb=270)
- Discovered dci_length: ___ (known-good: 48 at 273 PRB)
- Genuine FULLCRC decodes at the bootstrapped RNTI: ___/___ over ___s
- Time-to-discovery (wall clock from run start to the footprint/length lock lines): ___

## What no amount of this phase's search recovers (per the roadmap's own caveat)

TDRA table contents, MCS table selection, DM-RS additionalPosition, rate-matching patterns --
Techniques A-C recover WHERE and HOW LONG, not how to INTERPRET a payload whose CRC has already
passed. A second, TB-CRC-oracle search stage is flagged, not attempted, by this plan -- see the
roadmap's "What no amount of search recovers" section and PHASE1_CSS0_AUTOCONF_HANDOVER.md's
section 12 for the exact prior bug shape (total DCI length correct, two field widths wrong, 0%
CRC while RNTI cross-checks still passed).

## Still open

[Fill in honestly from whatever Steps 1-4 actually found -- this project's own established
discipline (verify-before-asserting-never-inherit-a-number) applies here as much as anywhere else
in this codebase.]
EOF"
```

- [ ] **Step 6: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && git add PHASE3_DEDICATED_CONFIG_RECOVERY_HANDOVER.md && git commit -m 'Phase 3: live-validate autodiscovery against this cells own known dedicated CORESET

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>'"
```
