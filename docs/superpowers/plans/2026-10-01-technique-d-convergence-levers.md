# Technique D Convergence Levers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Technique D (per-RNTI PDSCH configuration search) converge within ≤ 30 s cold / ≤ 2 s steady state on SA and NSA-like cells with zero wrong winners, via a grant trial gate, top-K CB0 probes, ordering scores and a per-field CellFieldBook — P1 first, failure-only probe evidence (P2) behind a simulator gate — and make cold acquisition limited by informative grants, not compute (spec §9: correctness fixes, shared per-grant work, legality bitsets, signatures, batched GPU PHY work).

**Revision 2 (2026-10-01):** adds spec §9 (compute acceleration) and the verified prerequisites K28–K32: new Tasks 4b, 4c (pure), F1–F3 (correctness fixes + profile), G1–G4 (GPU), and R1–R4 (runtime, replacing old Tasks 8–10).

**Architecture:** New pure units (`nr_td_gate`, `nr_td_order`, `nr_td_fieldbook`) and an engine extension (`next_k`/`feed_k` + score-ordered rounds) inside `nr_pdsch_config_sweep.c`, all default-off so the baseline is bit-identical; a correlated Monte-Carlo simulator (`nr_td_sim`) linking the real engine decides defaults and the P2 gate; runtime work comes after the Track-A cloud branch is merged: first the correctness fixes (F1 chest cache K28, F2 stale credit K29) and a per-grant/per-hypothesis profile (F3), then shared per-grant work (GrantWork, R1) and wiring (R2); the GPU track (G1–G4) fixes K30/K31 before any GPU decode is trusted and batches CB0 probes across grants × hypotheses.

**Tech Stack:** C11 (OAI style), C++17 gtest, CMake/Ninja, Python 3 stdlib (campaign summaries), OAI rfsim beds.

**Spec:** `docs/superpowers/specs/2026-10-01-technique-d-convergence-levers-design.md` (read it fully; it argues every rule below). Background: `PROJECT_MEMORY.md` §23.9, §11.8, §12 G8, §14.1.

## Global Constraints

- Hard constraints remove hypotheses. Soft information only reorders them. KL evidence decides the winner.
- The KL anytime acceptance rule (`nr_crc_interval()`, 1e-6 budget, separation test, 300-trial fallback) is not changed. No Bayesian rewrite. The joint hypothesis remains the decision unit.
- All levers off (K = 1, weights 0, field book off, gate off, P2 off) ⇒ **bit-identical** hypothesis sequence and winner vs today, for the same RNG seed.
- Side information is called ordering score / search priority, never "prior".
- MCS table is never promoted cell-wide.
- Gate is pre-outcome: it may never use any result or by-product of decoding the grant it judges.
- P2: a probe FAIL is one KL failure only if: same computation as the full decode on CB0 (segmentation, rate matching, LLR scaling, scrambling, max iterations, early termination), no HARQ soft combining (new transmission), same IQ within sample lifetime, gate ELIGIBLE. A probe PASS never adds a KL success.
- Speed-up accepted only if the wrong-winner rate does not increase. Targets: cold ≤ 30 s median (≤ 60 s worst of 5), steady ≤ 2 s median at 4 RX; 1 RX ≤ 3× with `UNDECIDABLE` instead of hangs; compute ≤ 4 extra cores (avg and peak) at equal traffic without added sample loss or queue backlog.
- **P2 same-decoder rule (spec §9.3):** a probe FAIL is admissible only if the probe ran the same LDPC implementation and iteration policy as the hypothesis's full decode (CPU layered ≠ CUDA flooding).
- **No GPU LDPC result is trusted before G1 (K30 false-pass fix); no GPU FEP result before G3 (K31).** Statistics/state never run on the GPU.
- **No top-K probing (K > 1) and no P2 at runtime before F1 (K28) and F2 (K29) are merged.**
- Starting values: K = 3; ordering weights 0 (neutral); contradiction threshold 2 distinct RNTIs, M = 64; energy start/end observable off.
- Repository rules (CLAUDE.md / PROJECT_MEMORY §4.0): sens6 files frozen (`git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` before every commit); evidence labels; explicit `git add`; no `git stash`; SIGINT not SIGKILL.

## Merge gating (operator decision "A")

- **Pure (any time):** Tasks 1–7, 4b, 4c, and G1 (CUDA LDPC module only, DGX GPU required).
- **After the merge of `cloud/dgx-next-steps` into `adaptive-rx-UL-DL`** (Track-A A2 metrics / A3 observations; the cloud session finished 2026-10-01 at `bcbe3549a2`): F1–F3, G2–G4, R1–R4. Check: `git log --oneline adaptive-rx-UL-DL | grep -i "observation API"` shows the A3 commit.
- Order of the runtime chain: F1 → F2 → F3 → R1 → R2 → (G3, G4) → R3 → R4. G1 and G2 can run in parallel with F/R (separate files).
- Work on branch `td/convergence-levers` (worktree `/home/nicola/NICOLA/wt/td-levers`, superpowers:using-git-worktrees). Push after each reviewed task.

## Agent / model / plugin policy

Same table as `docs/superpowers/plans/2026-10-01-dgx-next-steps.md`: Haiku 🔁 for repeated simulator/rfsim runs and score collection; Sonnet for implementation/tests/debugging; **Opus for Task 4 (engine change, ★), Task 7 (P2, ★), F1 (★), R1 (★), G4 (★) and all reviews**. Implementers use superpowers:test-driven-development, superpowers:systematic-debugging, superpowers:verification-before-completion; reviews via superpowers:requesting-code-review; `/code-review` before the final push.

## Review Focus

1. **K = 1 with a non-zero score weight but no side information** (e.g. SIB1 absent, observables absent) — must still be today's order exactly (all scores 0 ⇒ stable sort leaves the shuffle untouched). Pinned in Task 4 Step 1 (`ZeroScoresKeepShuffleOrder`).
2. **RNTI churn larger than `RNTI_CTX_MAX` (64)** — the field book must keep supporting/contradicting RNTI evidence when per-RNTI contexts are evicted. Pinned in Task 3 (`EvictedRntiStillCountsForPromotion`).
3. **A promoted field later contradicted by exactly one RNTI** — must NOT withdraw (threshold = 2 distinct RNTIs), but a second distinct RNTI must. Pinned in Task 3 (`OneContradictionKeepsTwoWithdraw`).
4. **A probe on a hypothesis that is a near-twin of the truth** — P2 must never count a FAIL that the full decode would have passed; the simulator's correlated model must show 0 wrong winners with near-twins present. Pinned in Task 7 (`NearTwinNeverEliminatesTruthUnderP2`).
5. **Config epoch bump while a context is mid-search** — promoted fields lose their ordering bonus immediately; no context keeps a stale order forever. Pinned in Task 3 (`EpochBumpDropsBonus`) and Task 4 (scores are recomputed at every round rebuild).

---

## PURE UNITS — any time (Tasks 1–7, 4b, 4c)

### Task 1: Grant trial gate (`nr_td_gate`) (Sonnet)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_td_gate.h`
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_td_gate.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_gate_test.cc`
- Modify: `CMakeLists.txt` (test block next to `test_nr_pdsch_config_sweep` ~2553; source into `nr_pdcch_blind_monitor` library ~1464)

**Interfaces:**
- Produces:
```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_TD_GATE_H
#define NR_TD_GATE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { NR_TD_ELIGIBLE = 0, NR_TD_GATED_PHYSICAL = 1, NR_TD_GATED_CHANNEL_QUALITY = 2 } nr_td_gate_t;
/* Everything the gate may look at. All of it must be known BEFORE any decode of this grant (pre-outcome rule). */
typedef struct {
  int layers;            /* layers implied by the DCI antenna-ports field; -1 = layout not pinned yet (unknown) */
  int mcs;               /* DCI MCS index */
  int mcs_table_most_permissive; /* lowest-rate table still alive in this context (0/1/2), -1 unknown */
} nr_td_grant_view_t;
typedef struct {
  int n_rx;              /* receive antennas in use */
  float snr_est_db;      /* post-equaliser SNR estimate for this RNTI from EARLIER grants (or this grant's DM-RS) */
  int snr_samples;       /* how many estimates back snr_est_db */
  float margin_db;       /* ISAC_TD_GATE_SNR_MARGIN_DB, default 6 */
} nr_td_rx_view_t;
/* Required SNR (dB) to decode `mcs` under `table` at 10 % BLER, AWGN, 1 layer: a conservative monotone table. */
float nr_td_required_snr_db(int mcs, int table);
nr_td_gate_t nr_td_grant_gate(const nr_td_grant_view_t *g, const nr_td_rx_view_t *rx);
#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_gate.h"
}
static nr_td_rx_view_t rx(int n_rx, float snr, int samples) { return {n_rx, snr, samples, 6.0f}; }

TEST(TdGate, UnknownLayersNeverGatesOnRank) {
  nr_td_grant_view_t g = {-1, 5, 0};
  const nr_td_rx_view_t r = rx(1, 30, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r), NR_TD_ELIGIBLE);
}
TEST(TdGate, RankAboveRxIsPhysical) {
  nr_td_grant_view_t g = {2, 5, 0};
  const nr_td_rx_view_t r = rx(1, 30, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r), NR_TD_GATED_PHYSICAL);
  const nr_td_rx_view_t r4 = rx(4, 30, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r4), NR_TD_ELIGIBLE);
}
TEST(TdGate, ChannelQualityNeedsEnoughSamplesAndMargin) {
  nr_td_grant_view_t g = {1, 27, 1};            /* high MCS, 256QAM table */
  const float need = nr_td_required_snr_db(27, 1);
  const nr_td_rx_view_t few = rx(1, need - 20, 5);
  EXPECT_EQ(nr_td_grant_gate(&g, &few), NR_TD_ELIGIBLE);              /* < 20 samples: never gate on SNR */
  const nr_td_rx_view_t low = rx(1, need - 6.5f, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &low), NR_TD_GATED_CHANNEL_QUALITY); /* beyond the 6 dB margin */
  const nr_td_rx_view_t edge = rx(1, need - 5.5f, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &edge), NR_TD_ELIGIBLE);             /* within margin: still a trial */
}
TEST(TdGate, UnknownTableUsesMostPermissiveAssumption) {
  nr_td_grant_view_t g = {1, 20, -1};
  const nr_td_rx_view_t r = rx(1, nr_td_required_snr_db(20, 2) - 5.0f, 50);
  EXPECT_EQ(nr_td_grant_gate(&g, &r), NR_TD_ELIGIBLE); /* table 2 (LowSE) is the lowest rate: no gating */
}
TEST(TdGate, RequiredSnrIsMonotoneInMcs) {
  for (int t = 0; t < 3; t++)
    for (int m = 1; m < 28; m++)
      EXPECT_GE(nr_td_required_snr_db(m, t), nr_td_required_snr_db(m - 1, t)) << "table " << t << " mcs " << m;
}
```
CMake:
```cmake
  add_executable(test_nr_td_gate ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_td_gate_test.cc
                                 ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_td_gate.c)
  target_include_directories(test_nr_td_gate PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_td_gate PRIVATE GTest::gtest GTest::gtest_main m)
  add_dependencies(tests test_nr_td_gate)
  add_test(NAME test_nr_td_gate COMMAND ./test_nr_td_gate)
```

- [ ] **Step 2: Run to verify it fails** — `cd cmake_targets/ran_build/build && cmake . >/dev/null && ninja test_nr_td_gate` → FAIL (missing source).

- [ ] **Step 3: Implement `nr_td_gate.c`**

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_gate.h"
/* Conservative AWGN 1-layer requirements (dB, ~10 % BLER) per MCS index. Coarse and deliberately
 * pessimistic-for-gating: the margin and the 20-sample floor make gating rare; it only removes grants
 * that are far beyond the link. Table 0 = 64QAM, 1 = 256QAM, 2 = 64QAM-LowSE (TS 38.214 Tables 5.1.3.1-1/2/3). */
static const float k_req[3][32] = {
  {-6.5f, -5.5f, -4.5f, -3.5f, -2.5f, -1.5f, -0.5f, 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f,
   10.5f, 11.5f, 12.5f, 13.5f, 14.5f, 15.5f, 16.5f, 17.5f, 18.5f, 19.5f, 20.5f, 20.5f, 20.5f, 20.5f, 20.5f},
  {-6.5f, -4.5f, -2.5f, -0.5f, 1.5f, 3.0f, 4.5f, 6.0f, 7.5f, 9.0f, 10.5f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f,
   18.0f, 19.0f, 20.0f, 21.0f, 22.0f, 23.0f, 24.0f, 25.0f, 26.0f, 27.0f, 28.0f, 28.0f, 28.0f, 28.0f, 28.0f},
  {-9.0f, -8.5f, -8.0f, -7.5f, -7.0f, -6.5f, -6.0f, -5.5f, -5.0f, -4.5f, -4.0f, -3.5f, -3.0f, -2.5f, -2.0f, -1.5f,
   -0.5f, 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f, 10.5f, 10.5f, 10.5f, 10.5f},
};
float nr_td_required_snr_db(int mcs, int table)
{
  if (mcs < 0) mcs = 0;
  if (mcs > 31) mcs = 31;
  if (table < 0 || table > 2) table = 2;
  return k_req[table][mcs];
}
nr_td_gate_t nr_td_grant_gate(const nr_td_grant_view_t *g, const nr_td_rx_view_t *rx)
{
  if (g->layers > 0 && g->layers > rx->n_rx)
    return NR_TD_GATED_PHYSICAL;
  if (rx->snr_samples >= 20) {
    const int table = g->mcs_table_most_permissive < 0 ? 2 : g->mcs_table_most_permissive;
    if (rx->snr_est_db + rx->margin_db < nr_td_required_snr_db(g->mcs, table))
      return NR_TD_GATED_CHANNEL_QUALITY;
  }
  return NR_TD_ELIGIBLE;
}
```
Note: `mcs_table_most_permissive` = the alive table with the **lowest** required SNR for this MCS (LowSE=2 < 64QAM=0 < 256QAM=1 at most indices); the caller computes it (Task R2).

- [ ] **Step 4: Run tests** — `ninja test_nr_td_gate && ./test_nr_td_gate` → 5 PASS.
- [ ] **Step 5: Commit** — `git add openair1/PHY/NR_UE_TRANSPORT/nr_td_gate.[ch] openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_gate_test.cc CMakeLists.txt && git commit -m "feat(td): grant trial gate (physical / channel-quality), pure and pre-outcome"`

---

### Task 2: Ordering score (`nr_td_order`) incl. 38.214 default table A (Sonnet)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_td_order.h`, `nr_td_order.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_order_test.cc`
- Modify: `CMakeLists.txt` (test block; library source)

**Interfaces:**
- Consumes: `nr_pdsch_cfg_hypothesis_t` (`nr_pdsch_config_sweep.h:49-58`).
- Produces:
```c
#include "nr_pdsch_config_sweep.h"
#define NR_TD_MAX_SIB1_TDRA 16
typedef struct { uint8_t S, L, mapping, k0; } nr_td_tdra_t;
typedef struct {
  /* SIB1 common TDRA list (0 entries = SIB1 absent or ignored) */
  int n_sib1; nr_td_tdra_t sib1[NR_TD_MAX_SIB1_TDRA];
  int dmrs_typeA_pos;          /* MIB dmrs-TypeA-Position 2 or 3; 0 = unknown */
  int obs_dmrs_mask;           /* measured DM-RS symbol mask, -1 = none */
  int obs_qm;                  /* measured modulation order for obs_mcs, -1 = none */
  int obs_mcs;
  int obs_last_symbol;         /* last symbol with PDSCH energy, -1 = none (optional observable) */
  /* promoted cell fields (Task 6), -1 = not promoted; confidence 0..1 */
  int f_S, f_L, f_mapping, f_k0, f_dmrs_add_pos, f_dmrs_max_len; float f_conf;
  float w_sib1, w_default, w_obs, w_field;   /* ISAC_TD_W_*; all 0 = neutral (today's order) */
  float w_probe;   /* P1 probe-ordering bonus applied by the engine (Task 4) to hypotheses with probe_pass > 0; not used by nr_td_ordering_score() */
} nr_td_side_info_t;
bool nr_td_default_table_a(int row, int dmrs_typeA_pos, nr_td_tdra_t *out); /* rows 1..16 */
float nr_td_ordering_score(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_side_info_t *si);
```

- [ ] **Step 1: Failing test**

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_order.h"
#include "nr_pdsch_qm_oracle.h"
}
static nr_pdsch_cfg_hypothesis_t H(int S, int L, int map, int k0) {
  nr_pdsch_cfg_hypothesis_t h = {}; h.tda_start = S; h.tda_length = L; h.mapping_type = map; h.k0 = k0; return h;
}
static nr_td_side_info_t neutral() {
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1; return s;
}
TEST(TdOrder, AllZeroWeightsGiveZero) {
  nr_td_side_info_t s = neutral(); s.n_sib1 = 1; s.sib1[0] = {1, 13, 0, 0}; s.dmrs_typeA_pos = 2;
  const nr_pdsch_cfg_hypothesis_t h = H(1, 13, 0, 0);
  EXPECT_EQ(nr_td_ordering_score(&h, &s), 0.0f);
}
TEST(TdOrder, Sib1MatchScoresAboveNonMatch) {
  nr_td_side_info_t s = neutral(); s.w_sib1 = 1; s.n_sib1 = 2; s.sib1[0] = {1, 13, 0, 0}; s.sib1[1] = {1, 5, 0, 0};
  const nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0), b = H(2, 12, 0, 0);
  EXPECT_GT(nr_td_ordering_score(&a, &s), nr_td_ordering_score(&b, &s));
}
TEST(TdOrder, DefaultTableARow1DependsOnTypeAPos) {
  nr_td_tdra_t r;
  ASSERT_TRUE(nr_td_default_table_a(1, 2, &r)); EXPECT_EQ(r.S, 2); EXPECT_EQ(r.L, 12); EXPECT_EQ(r.mapping, 0);
  ASSERT_TRUE(nr_td_default_table_a(1, 3, &r)); EXPECT_EQ(r.S, 3); EXPECT_EQ(r.L, 11);
  EXPECT_FALSE(nr_td_default_table_a(17, 2, &r));
}
TEST(TdOrder, ObservedMaskAgreementScores) {
  nr_td_side_info_t s = neutral(); s.w_obs = 1; s.obs_dmrs_mask = 0x804;
  nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0), b = H(1, 13, 0, 0); a.dmrs_mask = 0x804; b.dmrs_mask = 0x4;
  EXPECT_GT(nr_td_ordering_score(&a, &s), nr_td_ordering_score(&b, &s));
}
TEST(TdOrder, PromotedFieldsScaleWithConfidence) {
  nr_td_side_info_t s = neutral(); s.w_field = 1; s.f_S = 1; s.f_L = 13; s.f_conf = 0.5f;
  const nr_pdsch_cfg_hypothesis_t a = H(1, 13, 0, 0);
  const float half = nr_td_ordering_score(&a, &s); s.f_conf = 1.0f;
  EXPECT_FLOAT_EQ(nr_td_ordering_score(&a, &s), 2 * half);
}
TEST(TdOrder, ScoreIsNeverNegative) {
  nr_td_side_info_t s = neutral(); s.w_sib1 = s.w_default = s.w_obs = s.w_field = 1;
  const nr_pdsch_cfg_hypothesis_t a = H(9, 4, 1, 1);
  EXPECT_GE(nr_td_ordering_score(&a, &s), 0.0f);
}
```

- [ ] **Step 2: Run → FAIL.**

- [ ] **Step 3: Implement `nr_td_order.c`** — default table A per TS 38.214 Table 5.1.2.1.1-2 (normal CP). **Verify every row against the spec table before committing and correct in the same commit if any differs**; the values below are the implementer's starting transcription:

```c
/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_order.h"
#include "nr_pdsch_qm_oracle.h"
/* {mapping (0=A,1=B), S if pos2, L if pos2, S if pos3, L if pos3}; k0 = 0 for all rows. */
static const uint8_t k_def_a[16][5] = {
  {0, 2, 12, 3, 11}, {0, 2, 10, 3, 9}, {0, 2, 9, 3, 8}, {0, 2, 7, 3, 6}, {0, 2, 5, 3, 4},
  {1, 9, 4, 10, 4},  {1, 4, 4, 6, 4},  {1, 5, 7, 5, 7}, {1, 5, 2, 5, 2}, {1, 9, 2, 9, 2},
  {1, 12, 2, 12, 2}, {0, 1, 13, 1, 13}, {0, 1, 6, 1, 6}, {0, 2, 4, 2, 4}, {1, 4, 7, 4, 7}, {1, 8, 4, 8, 4},
};
bool nr_td_default_table_a(int row, int pos, nr_td_tdra_t *out)
{
  if (row < 1 || row > 16 || (pos != 2 && pos != 3))
    return false;
  const uint8_t *r = k_def_a[row - 1];
  out->mapping = r[0]; out->k0 = 0;
  out->S = pos == 2 ? r[1] : r[3]; out->L = pos == 2 ? r[2] : r[4];
  return true;
}
static bool tdra_eq(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_tdra_t *t)
{
  return h->tda_start == t->S && h->tda_length == t->L && h->mapping_type == t->mapping && h->k0 == t->k0;
}
float nr_td_ordering_score(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_side_info_t *si)
{
  float s = 0.0f;
  if (si->w_sib1 > 0)
    for (int i = 0; i < si->n_sib1; i++)
      if (tdra_eq(h, &si->sib1[i])) { s += si->w_sib1; break; }
  if (si->w_default > 0 && si->dmrs_typeA_pos) {
    nr_td_tdra_t t;
    for (int r = 1; r <= 16; r++)
      if (nr_td_default_table_a(r, si->dmrs_typeA_pos, &t) && tdra_eq(h, &t)) { s += si->w_default; break; }
  }
  if (si->w_obs > 0) {
    if (si->obs_dmrs_mask >= 0 && h->dmrs_mask == (uint16_t)si->obs_dmrs_mask) s += si->w_obs;
    if (si->obs_qm > 0 && nr_pdsch_qm_of_mcs(si->obs_mcs, h->mcs_table) == si->obs_qm) s += si->w_obs;
    if (si->obs_last_symbol >= 0 && h->tda_start + h->tda_length - 1 == si->obs_last_symbol) s += si->w_obs;
  }
  if (si->w_field > 0) {
    int n = 0;
    n += si->f_S >= 0 && h->tda_start == si->f_S;
    n += si->f_L >= 0 && h->tda_length == si->f_L;
    n += si->f_mapping >= 0 && h->mapping_type == si->f_mapping;
    n += si->f_k0 >= 0 && h->k0 == si->f_k0;
    n += si->f_dmrs_add_pos >= 0 && h->dmrs_add_pos == si->f_dmrs_add_pos;
    n += si->f_dmrs_max_len >= 0 && h->dmrs_max_len == si->f_dmrs_max_len;
    s += si->w_field * si->f_conf * (float)n;
  }
  return s;
}
```
(Check `nr_pdsch_qm_of_mcs` signature in `nr_pdsch_qm_oracle.h` and adapt the call; the test target links `nr_pdsch_qm_oracle.c`.)

- [ ] **Step 4: Run → 6 PASS.**
- [ ] **Step 5: Commit** — `"feat(td): ordering score (SIB1, 38.214 default table A, observables, promoted fields), neutral by default"`.

---

### Task 3: CellFieldBook (`nr_td_fieldbook`) (Sonnet)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_td_fieldbook.h`, `nr_td_fieldbook.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_fieldbook_test.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
```c
#include <stdbool.h>
#include <stdint.h>
#include "nr_td_order.h"
typedef enum { NR_TD_F_TDRA = 0, NR_TD_F_DMRS_ADD_POS, NR_TD_F_DMRS_MAX_LEN, NR_TD_F_COUNT } nr_td_field_t;
/* MCS table is deliberately NOT a field (UE-capability specific; never promoted). TDRA value packs S,L,mapping,k0. */
#define NR_TD_FB_MAX_RNTI 16
typedef struct {
  int32_t value;                 /* promoted value, -1 = unknown */
  int32_t candidate;             /* value with support but not yet promoted, -1 none */
  uint16_t support[NR_TD_FB_MAX_RNTI]; int n_support;           /* distinct RNTIs supporting `candidate`/`value` */
  uint16_t contra[NR_TD_FB_MAX_RNTI];  int n_contra;            /* distinct RNTIs contradicting `value` */
  uint32_t epoch;                /* config_epoch when last confirmed */
  uint64_t last_confirmed_slot;
} nr_td_field_entry_t;
typedef struct {
  nr_td_field_entry_t f[NR_TD_F_COUNT];
  uint32_t epoch;
  int promote_rntis;             /* default 2 */
  int withdraw_rntis;            /* default 2 */
} nr_td_fieldbook_t;
void nr_td_fieldbook_init(nr_td_fieldbook_t *fb, int promote_rntis, int withdraw_rntis);
int32_t nr_td_pack_tdra(int S, int L, int mapping, int k0);
/* A context for `rnti` converged on hypothesis `h` (call once per CONVERGED). */
void nr_td_fieldbook_converged(nr_td_fieldbook_t *fb, uint16_t rnti, const nr_pdsch_cfg_hypothesis_t *h, uint64_t slot);
/* `rnti` produced contradiction evidence for field `f` (M-failures rule evaluated by the caller). */
void nr_td_fieldbook_contradict(nr_td_fieldbook_t *fb, uint16_t rnti, nr_td_field_t f);
void nr_td_fieldbook_bump_epoch(nr_td_fieldbook_t *fb);
/* Copy the fields that are promoted AND current-epoch into side info (f_* and f_conf = 1.0). */
void nr_td_fieldbook_fill_side_info(const nr_td_fieldbook_t *fb, nr_td_side_info_t *si);
```

- [ ] **Step 1: Failing test** (covers Review Focus 2, 3, 5):

```cpp
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_fieldbook.h"
}
static nr_pdsch_cfg_hypothesis_t H(int S, int L, int ap) {
  nr_pdsch_cfg_hypothesis_t h = {}; h.tda_start = S; h.tda_length = L; h.dmrs_add_pos = ap; h.dmrs_max_len = 1; return h;
}
static nr_td_side_info_t si0() {
  nr_td_side_info_t s = {}; s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1; return s;
}
TEST(TdFieldBook, OneRntiNeverPromotesTwoDo) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 0x4601, &h, 10);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 0x4601, &h, 20);           /* same RNTI again: still one */
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_converged(&fb, 0x4602, &h, 30);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, nr_td_pack_tdra(1, 13, 0, 0));
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, 1);
}
TEST(TdFieldBook, FieldsPromoteIndependently) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  const auto a = H(1, 13, 1), b = H(1, 13, 2);              /* same TDRA, different add-pos */
  nr_td_fieldbook_converged(&fb, 1, &a, 1); nr_td_fieldbook_converged(&fb, 2, &b, 2);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  EXPECT_EQ(fb.f[NR_TD_F_DMRS_ADD_POS].value, -1);
}
TEST(TdFieldBook, OneContradictionKeepsTwoWithdraw) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1); nr_td_fieldbook_converged(&fb, 2, &h, 2);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA);
  nr_td_fieldbook_contradict(&fb, 3, NR_TD_F_TDRA);        /* same RNTI twice = one */
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
  nr_td_fieldbook_contradict(&fb, 4, NR_TD_F_TDRA);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);
}
TEST(TdFieldBook, ConvergedWithOtherValueContradicts) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1), o = H(2, 12, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1); nr_td_fieldbook_converged(&fb, 2, &h, 2);
  nr_td_fieldbook_converged(&fb, 3, &o, 3); nr_td_fieldbook_converged(&fb, 4, &o, 4);
  EXPECT_EQ(fb.f[NR_TD_F_TDRA].value, -1);                 /* withdrawn by 2 distinct contradicting RNTIs */
}
TEST(TdFieldBook, EpochBumpDropsBonus) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1); nr_td_fieldbook_converged(&fb, 2, &h, 2);
  nr_td_side_info_t s = si0(); nr_td_fieldbook_fill_side_info(&fb, &s); EXPECT_EQ(s.f_S, 1);
  nr_td_fieldbook_bump_epoch(&fb);
  s = si0(); nr_td_fieldbook_fill_side_info(&fb, &s); EXPECT_EQ(s.f_S, -1);
  nr_td_fieldbook_converged(&fb, 5, &h, 9);                /* one re-confirmation restores it */
  s = si0(); nr_td_fieldbook_fill_side_info(&fb, &s); EXPECT_EQ(s.f_S, 1);
}
TEST(TdFieldBook, EvictedRntiStillCountsForPromotion) {
  /* The field book keeps its own RNTI sets; it does not depend on Technique D's 64-slot RNTI context table. */
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  const auto h = H(1, 13, 1);
  nr_td_fieldbook_converged(&fb, 1, &h, 1);
  for (int r = 100; r < 200; r++) { const auto o = H(r % 3, 10, 0); (void)o; } /* churn elsewhere: no calls */
  nr_td_fieldbook_converged(&fb, 2, &h, 500);
  EXPECT_NE(fb.f[NR_TD_F_TDRA].value, -1);
}
```

- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement** `nr_td_fieldbook.c`: `add_rnti(set, n, rnti)` returns false if already present or full (full ⇒ ignore new RNTI; log nothing — pure unit); `converged()`: for each field compute the value from `h` (`TDRA` = `nr_td_pack_tdra(h->tda_start, h->tda_length, h->mapping_type, h->k0)` with `pack = S | L<<4 | mapping<<9 | k0<<10`), then: if `value == -1`: if `candidate == v` add rnti to `support` else reset `candidate = v`, `support = {rnti}`; when `n_support >= promote_rntis` → `value = v`, clear `contra`, `epoch = fb->epoch`. If `value == v`: add to support, refresh `epoch` and `last_confirmed_slot`. If `value != -1 && value != v`: `contradict(rnti, f)`. `contradict()`: add rnti to `contra`; when `n_contra >= withdraw_rntis` → `value = -1`, `candidate = -1`, clear support/contra. `bump_epoch()`: `fb->epoch++` (entries keep `value`; `fill_side_info` only exports entries with `epoch == fb->epoch`). `fill_side_info()`: unpack TDRA into `f_S, f_L, f_mapping, f_k0`; `f_dmrs_add_pos`, `f_dmrs_max_len`; `f_conf = 1.0f` if any exported.
- [ ] **Step 4: Run → 6 PASS.**
- [ ] **Step 5: Commit** — `"feat(td): CellFieldBook (per-field promotion by distinct RNTIs, withdrawal, config epoch; MCS table never promoted)"`.

---

### Task 4 ★: Engine — `next_k` / `feed_k`, probe statistics, score-ordered rounds (Opus)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h` (new API + state fields)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c` (`nr_pdsch_config_sweep_next` ~490-525 region, new functions)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc` (append tests)

**Interfaces:**
- Consumes: `nr_td_ordering_score()` (Task 2) — linked into the test target (add `nr_td_order.c` to `test_nr_pdsch_config_sweep` sources).
- Produces (header additions):
```c
typedef enum { NR_TD_FULL_TB = 0, NR_TD_CB_PROBE = 1 } nr_td_outcome_kind_t;
typedef enum { NR_TD_PASS = 0, NR_TD_FAIL = 1, NR_TD_INCONCLUSIVE = 2 } nr_td_outcome_result_t;
typedef struct { int hyp; uint8_t kind; uint8_t result; bool p2_admissible; } nr_td_outcome_t;
#define NR_TD_MAX_K 8
/* State additions (append to nr_pdsch_config_sweep_state_t; uint16 to bound memory):
 *   uint16_t probe_pass[NR_PDSCH_SWEEP_MAX_HYP], probe_fail[...], probe_inconclusive[...];
 *   const struct nr_td_side_info_s *side;   NULL = neutral ordering (today)
 *   bool p2;                                 failure-only probe evidence enabled */
/* K = 1 is exactly nr_pdsch_config_sweep_next(). Returns n filled (1..K); idx[0] = main hypothesis. */
int nr_pdsch_config_sweep_next_k(nr_pdsch_config_sweep_state_t *st, int K, int idx[], nr_pdsch_cfg_hypothesis_t out[]);
/* outcomes[0] must be the main FULL_TB outcome. Returns the winner index or -1 (same contract as _feed). */
int nr_pdsch_config_sweep_feed_k(nr_pdsch_config_sweep_state_t *st, const nr_td_outcome_t *outcomes, int n);
```
(`nr_td_side_info_t` is a typedef of `struct nr_td_side_info_s` — adjust Task 2's typedef to name the struct; do it in this task's commit.)

Design (exact):
- **Score-ordered rounds:** where `_next` rebuilds the round (`if (!st->cursor) nr_crc_shuffle(st->order, ...)`), after the shuffle, if `st->side != NULL`, apply a **stable** sort of `st->order` by `key(i) = nr_td_ordering_score(&st->hyp[i], st->side) + (st->probe_pass[i] > 0 ? st->side->w_probe : 0)` descending (the probe bonus is how P1 uses probe outcomes for ordering). With all scores 0 the stable sort is a no-op ⇒ identical order (Review Focus 1). RNG consumption is unchanged (the shuffle still runs).
- **`next_k`:** `idx[0] = nr_pdsch_config_sweep_next(st, &out[0])` (unchanged path, unchanged RNG). Probes: walk `st->order` starting at `st->cursor` (wrapping), pick up to K−1 distinct indices ≠ idx[0] that are not yet "cleared" (cleared = `trials[i] >= SWEEP_MIN_TRIALS` with `ok[i] == 0`), **without** advancing `st->cursor` or consuming RNG. If `st->winner >= 0` return 1.
- **`feed_k`:** `outcomes[0]` → existing `nr_pdsch_config_sweep_feed(st, hyp, result == PASS)` (INCONCLUSIVE main outcome is not fed — the caller must not send it). For i ≥ 1 (probes): update `probe_pass/fail/inconclusive` (saturating uint16). If `st->p2 && result == FAIL && p2_admissible` → call `nr_pdsch_config_sweep_feed(st, hyp, false)` (one KL failure). A probe PASS never calls `_feed`. Return `st->winner`.

- [ ] **Step 1: Failing tests** (append):

```cpp
TEST(PdschSweepK, K1IsBitIdenticalToNext) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4); nr_pdsch_config_sweep_init(&b, 4);
  unsigned seed = 7;
  for (int i = 0; i < 20000 && a.winner < 0; i++) {
    nr_pdsch_cfg_hypothesis_t ha, hb[NR_TD_MAX_K]; int ib[NR_TD_MAX_K];
    const int ia = nr_pdsch_config_sweep_next(&a, &ha);
    ASSERT_EQ(nr_pdsch_config_sweep_next_k(&b, 1, ib, hb), 1);
    ASSERT_EQ(ia, ib[0]);
    const bool ok = (ia == 3) && (rand_r(&seed) % 100 < 60);
    nr_pdsch_config_sweep_feed(&a, ia, ok);
    const nr_td_outcome_t o = {ib[0], NR_TD_FULL_TB, (uint8_t)(ok ? NR_TD_PASS : NR_TD_FAIL), false};
    nr_pdsch_config_sweep_feed_k(&b, &o, 1);
  }
  EXPECT_EQ(a.winner, b.winner);
}
TEST(PdschSweepK, ZeroScoresKeepShuffleOrder) {
  static nr_pdsch_config_sweep_state_t a, b;
  nr_pdsch_config_sweep_init(&a, 4); nr_pdsch_config_sweep_init(&b, 4);
  nr_td_side_info_t s = {}; s.obs_dmrs_mask = -1; s.obs_qm = -1; s.obs_last_symbol = -1;
  s.f_S = s.f_L = s.f_mapping = s.f_k0 = s.f_dmrs_add_pos = s.f_dmrs_max_len = -1; s.w_sib1 = 1; /* weight on, no data */
  b.side = &s;
  for (int i = 0; i < 3 * a.n_hyp; i++) {
    nr_pdsch_cfg_hypothesis_t h; ASSERT_EQ(nr_pdsch_config_sweep_next(&a, &h), nr_pdsch_config_sweep_next(&b, &h));
    nr_pdsch_config_sweep_feed(&a, 0, false); nr_pdsch_config_sweep_feed(&b, 0, false);
  }
}
TEST(PdschSweepK, ProbesAreDistinctAndDoNotAdvanceCursor) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  int idx[NR_TD_MAX_K]; nr_pdsch_cfg_hypothesis_t h[NR_TD_MAX_K];
  const int c0 = st.cursor; const int n = nr_pdsch_config_sweep_next_k(&st, 3, idx, h);
  ASSERT_EQ(n, 3); EXPECT_NE(idx[0], idx[1]); EXPECT_NE(idx[1], idx[2]); EXPECT_NE(idx[0], idx[2]);
  EXPECT_EQ(st.cursor, (c0 + 1) % st.n_hyp); /* only the main selection advanced it */
}
TEST(PdschSweepK, P1ProbeFailuresDoNotTouchKlStats) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  const nr_td_outcome_t o[2] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {5, NR_TD_CB_PROBE, NR_TD_FAIL, true}};
  nr_pdsch_config_sweep_feed_k(&st, o, 2);
  EXPECT_EQ(st.trials[5], 0u); EXPECT_EQ(st.probe_fail[5], 1);
}
TEST(PdschSweepK, P2AdmissibleProbeFailIsOneKlFailureAndPassIsNothing) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4); st.p2 = true;
  const nr_td_outcome_t o[3] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {5, NR_TD_CB_PROBE, NR_TD_FAIL, true},
                                {6, NR_TD_CB_PROBE, NR_TD_PASS, true}};
  nr_pdsch_config_sweep_feed_k(&st, o, 3);
  EXPECT_EQ(st.trials[5], 1u); EXPECT_EQ(st.ok[5], 0u);
  EXPECT_EQ(st.trials[6], 0u); EXPECT_EQ(st.probe_pass[6], 1);
  const nr_td_outcome_t na[2] = {{0, NR_TD_FULL_TB, NR_TD_FAIL, false}, {7, NR_TD_CB_PROBE, NR_TD_FAIL, false}};
  nr_pdsch_config_sweep_feed_k(&st, na, 2);
  EXPECT_EQ(st.trials[7], 0u); /* not admissible: no KL evidence */
}
```

- [ ] **Step 2: Run → FAIL** (`ninja test_nr_pdsch_config_sweep`).
- [ ] **Step 3: Implement** the design above. Measure `sizeof(nr_pdsch_config_sweep_state_t)` before/after (print in a one-off test) and record it in the commit message; check how contexts are allocated (`grep -n "MAX_CONTEXTS" nr_pdsch_config_sweep.c`) and report the RSS impact at 1024 contexts — if > 64 MB extra, allocate the probe arrays lazily per context instead of embedding them (decide in this step, test still green).
- [ ] **Step 4: Run** — the 5 new tests PASS; the existing 44 + 1 skip still PASS; shuffle seeds 1/3/5 green.
- [ ] **Step 5: Commit** — `"feat(td): next_k/feed_k, probe statistics, score-ordered rounds (K=1 and neutral side info bit-identical)"`.

---

### Task 4b: Legality bitsets and rate-matching feasibility (spec §4.2, §9.1) (Sonnet)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_td_legal.h`, `nr_td_legal.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_legal_test.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
```c
#include <stdbool.h>
#include <stdint.h>
#include "nr_pdsch_config_sweep.h"
#define NR_TD_WORDS ((NR_PDSCH_SWEEP_MAX_HYP + 63) / 64)
typedef struct { uint64_t w[NR_TD_WORDS]; } nr_td_mask_t;
static inline void nr_td_mask_set(nr_td_mask_t *m, int i) { m->w[i >> 6] |= 1ull << (i & 63); }
static inline bool nr_td_mask_get(const nr_td_mask_t *m, int i) { return (m->w[i >> 6] >> (i & 63)) & 1; }
void nr_td_mask_and(nr_td_mask_t *dst, const nr_td_mask_t *a, int n_hyp);
int nr_td_mask_count(const nr_td_mask_t *m, int n_hyp);
/* Rate-matching feasibility of one code-block geometry: the SAME predicates that make the LDPC rate recovery reject
 * a geometry (nr_rate_matching.c ~540/551/638 and nrLDPC_coding_segment_decoder.c:192), evaluated before decoding. */
typedef struct { uint32_t G; uint32_t tbs; int qm, nl, C, K, F, Zc, BG; uint32_t Ncb; int rv; } nr_td_rm_geom_t;
bool nr_td_rm_feasible(const nr_td_rm_geom_t *g);
```
- Consumes: nothing; Task R2 builds the per-grant masks by computing each hypothesis's geometry with the existing OAI TBS/segmentation helpers and calling `nr_td_rm_feasible`.

- [ ] **Step 1: Read the reject conditions** in `openair1/PHY/CODING/nr_rate_matching.c` (~540, ~551, ~638: "invalid parameters (Foffset … > Ncb …)") and `openair1/PHY/CODING/nrLDPC_coding/.../nrLDPC_coding_segment_decoder.c:192` ("Problem in rate_matching"). Copy each condition **verbatim** (same arithmetic, same k0/Foffset formula for rv, same E per CB rule: E = Nl·Qm·⌊G/(Nl·Qm·C)⌋ or ⌈⌉ for the last C − (G/(Nl·Qm)) mod C blocks) into a comment block in `nr_td_legal.c` with file:line references.
- [ ] **Step 2: Failing test**
```cpp
#include <gtest/gtest.h>
extern "C" {
#include "nr_td_legal.h"
}
TEST(TdLegal, MaskAndCount) {
  nr_td_mask_t a = {}, b = {}, d = {};
  nr_td_mask_set(&a, 1); nr_td_mask_set(&a, 70); nr_td_mask_set(&a, 200);
  nr_td_mask_set(&b, 70); nr_td_mask_set(&b, 200); nr_td_mask_set(&b, 201);
  nr_td_mask_and(&d, &a, 233); nr_td_mask_and(&d, &b, 233);  /* d starts all-zero: AND keeps zero */
  EXPECT_EQ(nr_td_mask_count(&d, 233), 0);
  d = a; nr_td_mask_and(&d, &b, 233);
  EXPECT_EQ(nr_td_mask_count(&d, 233), 2); EXPECT_TRUE(nr_td_mask_get(&d, 70)); EXPECT_FALSE(nr_td_mask_get(&d, 1));
}
TEST(TdLegal, TypicalGeometryIsFeasible) {
  const nr_td_rm_geom_t g = {50000, 25000, 4, 1, 3, 8448, 0, 384, 1, 25344, 0};
  EXPECT_TRUE(nr_td_rm_feasible(&g));
}
TEST(TdLegal, ZeroGOrTbsIsInfeasible) {
  nr_td_rm_geom_t g = {0, 25000, 4, 1, 3, 8448, 0, 384, 1, 25344, 0};
  EXPECT_FALSE(nr_td_rm_feasible(&g));
  g.G = 50000; g.tbs = 0; EXPECT_FALSE(nr_td_rm_feasible(&g));
}
TEST(TdLegal, MatchesDecoderRejections) {
  /* Each vector is a geometry the real decoder rejected with "Problem in rate_matching" / "invalid parameters",
   * captured from an rfsim log during Step 1 (fill the table with >= 3 captured cases and >= 3 accepted ones). */
  struct V { nr_td_rm_geom_t g; bool ok; };
  const V v[] = {
    /* captured cases go here — the implementer replaces this comment with the real tuples before Step 3 */
  };
  for (const V &x : v) EXPECT_EQ(nr_td_rm_feasible(&x.g), x.ok);
}
```
To capture real vectors: run the 106-PRB rfsim baseline (Track-A `tests/passive_rx/dgx/rfsim_arm.sh`) with `ISAC_DISCOVER_DIAG=1` and grep `Problem in rate_matching|invalid parameters` together with the job geometry logged next to it (add a temporary `LOG_I` of G/tbs/Qm/Nl/C/K/F/Zc/BG/Ncb/rv at the reject site on a throwaway branch). The test must contain ≥ 3 rejected and ≥ 3 accepted real tuples; an empty table is a review failure.
- [ ] **Step 3: Run → FAIL.**
- [ ] **Step 4: Implement** `nr_td_mask_and`, `nr_td_mask_count` (`__builtin_popcountll`, last word masked to `n_hyp`), and `nr_td_rm_feasible` with exactly the copied predicates plus `G > 0 && tbs > 0 && C >= 1`.
- [ ] **Step 5: Run → PASS; commit** `"feat(td): legality bitsets + rate-matching feasibility identical to the decoder's reject rules"`.

---

### Task 4c: Computational signature (spec §9.1) (Sonnet)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_td_legal.{h,c}` (same unit: per-hypothesis static properties)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_legal_test.cc` (append)

**Interfaces:**
- Produces:
```c
/* Hypotheses with equal signature share FEP, DM-RS channel estimate, equalisation and LLRs for a given grant;
 * only TBS/rate-matching/LDPC (MCS-table dependent) differ. qm = modulation order the hypothesis implies for the grant's MCS. */
uint64_t nr_td_signature(const nr_pdsch_cfg_hypothesis_t *h, int nl, int qm);
int nr_td_count_signatures(const nr_pdsch_cfg_hypothesis_t *hyp, int n, int nl, const int *qm_per_hyp);
```

- [ ] **Step 1: Failing test**
```cpp
TEST(TdSignature, McsTableOnlyDifferenceSharesSignatureWhenQmEqual) {
  nr_pdsch_cfg_hypothesis_t a = {}, b = {};
  a.tda_start = b.tda_start = 1; a.tda_length = b.tda_length = 13; a.dmrs_mask = b.dmrs_mask = 0x804;
  a.mcs_table = 0; b.mcs_table = 1;
  EXPECT_EQ(nr_td_signature(&a, 1, 4), nr_td_signature(&b, 1, 4));
  EXPECT_NE(nr_td_signature(&a, 1, 4), nr_td_signature(&b, 1, 6)); /* different Qm => different LLRs */
}
TEST(TdSignature, DmrsMaskOrTdraChangesSignature) {
  nr_pdsch_cfg_hypothesis_t a = {}, b = {};
  a.tda_start = b.tda_start = 1; a.tda_length = b.tda_length = 13; a.dmrs_mask = 0x804; b.dmrs_mask = 0x4;
  EXPECT_NE(nr_td_signature(&a, 1, 2), nr_td_signature(&b, 1, 2));
  b.dmrs_mask = 0x804; b.tda_length = 12;
  EXPECT_NE(nr_td_signature(&a, 1, 2), nr_td_signature(&b, 1, 2));
}
TEST(TdSignature, CountOnFullCatalog) {
  static nr_pdsch_config_sweep_state_t st; nr_pdsch_config_sweep_init(&st, 4);
  std::vector<int> qm(st.n_hyp, 2);
  const int n = nr_td_count_signatures(st.hyp, st.n_hyp, 1, qm.data());
  EXPECT_GT(n, 0); EXPECT_LT(n, st.n_hyp); /* report n in the test output: */
  std::cout << "catalog " << st.n_hyp << " hypotheses -> " << n << " signatures (Qm fixed)" << std::endl;
}
```
- [ ] **Step 2: Run → FAIL. Step 3: Implement**: `sig = tda_start | tda_length<<4 | k0<<8 | mapping_type<<14 | (uint64_t)dmrs_mask<<16 | (uint64_t)dmrs_max_len<<30 | (uint64_t)nl<<32 | (uint64_t)qm<<36` (dmrs_add_pos is implied by dmrs_mask; mcs_table enters only through qm); count = distinct values via sort of a copy.
- [ ] **Step 4: Run → PASS; record the printed ratio in the commit message. Commit** `"feat(td): computational signature + count (signature groups << hypotheses)"`.

---

### Task 5: Correlated Monte-Carlo simulator `nr_td_sim` (Sonnet; 🔁 Haiku for campaigns)

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc` (library-style simulator + `main()` CLI)
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim_test.cc` (gtest smoke + invariants)
- Create: `tests/passive_rx/td_sim/campaign.py` (matrix runner + summary)
- Modify: `CMakeLists.txt` (two targets)

**Interfaces:**
- Consumes: engine (Task 4), gate (Task 1), order (Task 2), field book (Task 3).
- Produces CLI: `nr_td_sim --acq N --seed S --catalog-tda T --p-true-snr-mu DB --fade-db DB --snr-est-sigma DB --n-rx R --rank2-frac F --grants-per-s G --sib1 0|1 --K k --w-sib1 x --w-default x --w-obs x --w-field x --w-probe x --fieldbook 0|1 --gate 0|1 --p2 0|1 --twins N --rntis-per-acq M --probe-inconclusive P --table-exercise Q` → one JSON line per acquisition: `{"acq","rnti_rank","grants","seconds","winner_ok","wrong","undecidable","n_full","n_probe","gated_phys","gated_chan","promotions","withdrawals"}`, and a final `{"summary":...}` line (median/p95 seconds, wrong count).
- Produces function for tests: `struct SimResult run_sim(const SimCfg &cfg);` (`SimCfg`/`SimResult` plain structs in the .cc, exposed to the test via `#include "nr_td_sim.cc"` guarded by `#ifndef NR_TD_SIM_NO_MAIN`).

Model (exact, from spec §6.1):
- Catalogue: `nr_pdsch_config_sweep_init(&st, catalog_tda)`; truth = a fixed hypothesis index drawn per acquisition; `twins` = up to N other hypotheses that differ from the truth **only** in `mcs_table` (exercised per grant with probability `table_exercise`, default 0.9) — a twin's outcome on a grant equals the truth's outcome when the grant does not exercise the table.
- Each acquisition = `rntis_per_acq` RNTIs arriving in sequence on the same cell (field book shared across them, engine state per RNTI); RNTI k's "seconds" = grants ÷ `grants_per_s`.
- Per grant: latent SNR = `mu + N(0, fade)`; rank = 2 with probability `rank2_frac` else 1; MCS drawn uniformly 0..27; table = truth's table. Truth passes iff `rank <= n_rx && snr >= nr_td_required_snr_db(mcs, table)`. Wrong non-twin hypotheses fail. Twins: equal to truth if the grant does not exercise the table, else fail.
- Gate (when on): view = `{layers=rank, mcs, mcs_table_most_permissive}`, rx = `{n_rx, snr + N(0, snr_est_sigma), samples, 6}`; non-ELIGIBLE ⇒ not a trial (counted).
- Probes (K ≥ 2): for each probe hypothesis, outcome = INCONCLUSIVE w.p. `probe_inconclusive`; else PASS if that hypothesis's full decode would pass on this grant (same latent state — **shared-IQ correlation**), else FAIL. `p2_admissible = true` iff the grant is a "new transmission" (draw: 75 % new) and the outcome is not INCONCLUSIVE. Near-twin probes inherit the truth's outcome on non-exercising grants (correlated).
- Side info: `sib1` on ⇒ `si.sib1[]` = truth's TDRA + 3 random others; `w_obs` uses `obs_dmrs_mask = truth.dmrs_mask` (the existing DM-RS oracle is reliable in rfsim); field book via `nr_td_fieldbook_fill_side_info` before each RNTI starts.
- Wrong winner = engine winner ≠ truth **and** not a twin that is indistinguishable on every exercised grant (an exact twin is "undecided-correct", per §23.9 semantics).

- [ ] **Step 1: Failing tests** (`nr_td_sim_test.cc`):
```cpp
#define NR_TD_SIM_NO_MAIN
#include "nr_td_sim.cc"
#include <gtest/gtest.h>
TEST(TdSim, DeterministicForSeed) {
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 42;
  EXPECT_EQ(run_sim(c).total_grants, run_sim(c).total_grants);
}
TEST(TdSim, BaselineNeverWrong) {
  SimCfg c = SimCfg::defaults(); c.acq = 200; c.twins = 2;
  EXPECT_EQ(run_sim(c).wrong, 0);
}
TEST(TdSim, OneRxRank2OnlyReportsUndecidable) {
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.n_rx = 1; c.rank2_frac = 1.0; c.gate = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0); EXPECT_EQ(r.undecidable, r.acquisitions_rntis);
}
TEST(TdSim, ProbesShareTheGrantState) {
  /* With probe_inconclusive=0 and K=2, on every grant where the truth fails for SNR, a probe on the truth fails too. */
  SimCfg c = SimCfg::defaults(); c.acq = 5; c.K = 2; c.probe_inconclusive = 0; c.check_correlation = true;
  EXPECT_EQ(run_sim(c).correlation_violations, 0);
}
```
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement** the simulator per the model (≈ 300 lines C++; use `std::mt19937_64` seeded from `--seed` for the channel model — the engine keeps its own RNG; `SimCfg::defaults()` = 4 RX, rank2_frac 0.3, mu 15 dB, fade 6 dB, est σ 2 dB, 200 grants/s, catalog_tda 4, K 1, all weights 0, field book 0, gate 0, p2 0, twins 0, rntis_per_acq 4, probe_inconclusive 0.1, table_exercise 0.9).
- [ ] **Step 4: Run → 4 PASS; then baseline numbers** (🔁 Haiku): `./nr_td_sim --acq 2000 --seed 1` at default and at `--n-rx 1`; record median/p95 seconds and wrong count.
- [ ] **Step 5: `campaign.py`** — runs a matrix given as JSON (`{"arms": {name: {flag: value}}, "cells": {"SA": {"sib1":1}, "NSA-like": {"sib1":0}}, "rx": [4, 1], "acq": 2000}`), invokes `nr_td_sim` per combination, writes `results.jsonl` + `summary.md` (per arm × cell × rx: median/p95 cold seconds = first two RNTIs, median steady seconds = later RNTIs, wrong, n_full, n_probe, gated). Unit test `tests/passive_rx/td_sim/test_campaign.py` with a fake simulator script.
- [ ] **Step 6: Commit** — `"test(td): correlated shared-IQ Monte-Carlo simulator linking the real engine + campaign runner"`.

---

### Task 6: Simulator wiring of all P1 levers + ablation (Sonnet; 🔁 Haiku runs)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc` (levers already parameterised in Task 5 — this task runs and analyses)
- Create: `tests/passive_rx/td_sim/ablation_p1.json`, results under `tests/passive_rx/td_sim/results_<date>/` (summary.md committed; jsonl only if < 5 MB)

- [ ] **Step 1:** Ablation arms (P1): `baseline`; `+gate`; `+topK` (K=3, P1: probe outcomes go to `feed_k` with `p2=false`; ordering uses them through `w_probe=1`); `+order` (w_sib1=w_default=w_obs=1); `+fieldbook` (w_field=1, fieldbook=1); `all_P1`. Cells SA/NSA-like × rx 4/1 × 2000 acquisitions × 4 RNTIs.
- [ ] **Step 2:** Run (🔁 Haiku) `campaign.py ablation_p1.json`.
- [ ] **Step 3:** Analyse (Sonnet): table of median/p95 cold and steady seconds, wrong (must be 0 everywhere), N_full, N_probe, gated. Choose P1 defaults (weights, K) that minimise cold time with 0 wrong; write them into `nr_td_side_info` defaults documentation (spec §8) and the plan's Task R2 env defaults.
- [ ] **Step 4:** Commit summary + chosen defaults: `"evidence(td): P1 simulator ablation SA/NSA-like x 4/1 RX"`.

---

### Task 7 ★: P2 failure-only probe evidence — simulator gate (Opus)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim_test.cc` (add Review-Focus-4 test)
- Create: `tests/passive_rx/td_sim/gate_p2.json`; results dir

- [ ] **Step 1: Failing test** (Review Focus 4):
```cpp
TEST(TdSim, NearTwinNeverEliminatesTruthUnderP2) {
  SimCfg c = SimCfg::defaults(); c.acq = 3000; c.K = 3; c.p2 = 1; c.twins = 3; c.table_exercise = 0.5;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_EQ(r.truth_eliminated_by_probe, 0); /* a probe FAIL was never admitted on a grant where the truth's full decode passes */
}
```
(`truth_eliminated_by_probe` counts admitted probe FAILs on the truth hypothesis on grants where its full decode would pass — must be impossible by the model's construction; the test proves the simulator and `feed_k` respect it.)
- [ ] **Step 2: Run → FAIL** until the counter exists; implement the counter; PASS.
- [ ] **Step 3: P2 Monte-Carlo gate** (🔁 Haiku runs, Opus decides): `gate_p2.json` = arms `all_P1` vs `all_P2`, cells SA/NSA-like, rx 4/1, twins {0, 3}, table_exercise {0.9, 0.5}, K {2, 3, 4}, mu {8, 15, 25} dB, catalog {4, 16}; total ≥ 20 000 acquisitions per (cell, rx). Pass criteria (spec §5.4): wrong = 0 everywhere; for every seed the P2 winner equals the P1 winner; median trials of the true hypothesis not worse than P1; full-TB trials and convergence time reduced.
- [ ] **Step 4:** Write the decision into the spec §5.4 (date, evidence path, pass/fail per criterion). If any criterion fails: P2 stays off (`ISAC_TD_P2=0` default) and the report says which.
- [ ] **Step 5:** Commit `"evidence(td): P2 failure-only probe evidence Monte-Carlo gate"`.

---

## AFTER THE CLOUD MERGE — correctness fixes and profile (F1–F3)

### Task F1 ★: Chest cache correctness (K28) (Opus implements; Sonnet runs rfsim)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (`t_chest_cache` struct ~1486-1492, hit test ~2240-2253, chest loop ~2342, store ~2529-2536; in-place post-processing at ~2445-2447, ~3159, ~3270)
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_chest_key.h` (pure key type + equality, unit-testable)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_chest_key_test.cc`

**Interfaces:**
- Produces:
```c
typedef struct {
  long slot; uint16_t dmrs_pos /* FULL slot DM-RS symbol mask, not [S, S+L) */; uint8_t cfg_type, nscid, cdm, nl;
  uint16_t dmrs_ports /* all 12 bits */; uint16_t scr; int rb_lo, rb_n; int bwp_start, bwp_size, ref_point;
  int only_ant; int n_ant; double fo_hz; int seg; /* seg != 0 => never cached */
} nr_pdsch_chest_key_t;
static inline bool nr_pdsch_chest_key_eq(const nr_pdsch_chest_key_t *a, const nr_pdsch_chest_key_t *b)
{
  return !a->seg && !b->seg && a->slot == b->slot && a->dmrs_pos == b->dmrs_pos && a->cfg_type == b->cfg_type &&
         a->nscid == b->nscid && a->cdm == b->cdm && a->nl == b->nl && a->dmrs_ports == b->dmrs_ports &&
         a->scr == b->scr && a->rb_lo == b->rb_lo && a->rb_n == b->rb_n && a->bwp_start == b->bwp_start &&
         a->bwp_size == b->bwp_size && a->ref_point == b->ref_point && a->only_ant == b->only_ant &&
         a->n_ant == b->n_ant && a->fo_hz == b->fo_hz;
}
```
Design (exact): (1) the cached estimate is always computed over **every DM-RS symbol of the slot** given by the grant's full `dlDmrsSymbPos` (never truncated by a probe horizon or by S/L); (2) the cache stores an **immutable** copy; the decode works on a per-call working copy, so interpolation / branch zeroing / SFO rotation never touch the cached data; (3) the hit test is `nr_pdsch_chest_key_eq` on the full key (12-bit ports, BWP/refPoint, fo, antenna count, single-branch).

- [ ] **Step 1: Failing key test**
```cpp
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_chest_key.h"
}
static nr_pdsch_chest_key_t K() { nr_pdsch_chest_key_t k = {}; k.slot = 5; k.dmrs_pos = 0x884; k.dmrs_ports = 1; k.n_ant = 4; k.only_ant = -1; return k; }
TEST(ChestKey, PortsAboveEightDoNotAlias) { auto a = K(), b = K(); a.dmrs_ports = 0x001; b.dmrs_ports = 0x100; EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b)); }
TEST(ChestKey, SegmentedNeverHits) { auto a = K(), b = K(); a.seg = b.seg = 1; EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b)); }
TEST(ChestKey, FoOrAntennaCountChangeMisses) { auto a = K(), b = K(); b.fo_hz = 10; EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b)); b = K(); b.n_ant = 1; EXPECT_FALSE(nr_pdsch_chest_key_eq(&a, &b)); }
TEST(ChestKey, IdenticalHits) { auto a = K(), b = K(); EXPECT_TRUE(nr_pdsch_chest_key_eq(&a, &b)); }
```
- [ ] **Step 2: Run → FAIL. Step 3: Implement** design points (1)–(3) in `nr_pdsch_passive_decode.c`.
- [ ] **Step 4: Probe ≡ full check (debug harness):** add env `ISAC_TD_PROBE_EQUIV_CHECK=1` (off by default): for a sample of 1-in-50 probed grants, also run the full decode of the same hypothesis and compare the CB0 LLR vectors element-wise; log `PROBE_EQUIV mismatches=n/total`. Run the 106-PRB rfsim arm with `ISAC_PROBE_ALL=1 ISAC_TD_PROBE_EQUIV_CHECK=1` → expected `mismatches=0`. (Before F1 this check is expected to show mismatches on multi-DM-RS grants — record the "before" number too.)
- [ ] **Step 5:** ctest; rfsim regression gate (A1 `rfsim_regress.sh 2`) unchanged or better CRC; rank-4 pinned bed (`pin49r4`, default env) still 100 %. Commit `"fix(rx): chest cache keyed on full DM-RS signature, immutable cached estimate (K28)"`; PROJECT_MEMORY K28 → resolved.

### Task F2: No stale credit (K29) (Sonnet)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c` (between decode end and `nr_pdsch_config_sweep_feedback`, ~984-1131), `nr_passive_sample_lifetime.h`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_sample_lifetime_test.cc` (create if absent)

**Interfaces:**
- Produces: `static inline bool nr_passive_credit_allowed(long producer_slot_now, long job_slot, int slots_per_frame)` in `nr_passive_sample_lifetime.h` — identical rule to `nr_passive_samples_valid` evaluated **after** decode; when false the job outcome is INCONCLUSIVE: no Technique D feedback, no layout feedback, counted `stale_after_decode`.

- [ ] **Step 1: Failing test**
```cpp
#include <gtest/gtest.h>
extern "C" {
#include "nr_passive_sample_lifetime.h"
}
TEST(Lifetime, CreditAllowedOnlyWhileSamplesValid) {
  EXPECT_TRUE(nr_passive_credit_allowed(100, 99, 20));
  EXPECT_FALSE(nr_passive_credit_allowed(100, 99 - 18, 20)); /* spf - 2 = 18 slots: overwritten */
  EXPECT_FALSE(nr_passive_credit_allowed(98, 99, 20));        /* producer behind job: invalid */
}
```
- [ ] **Step 2: Run → FAIL. Step 3: Implement** (read `nr_passive_samples_valid` and reuse it; add the post-decode check before every feedback call; GPU path included).
- [ ] **Step 4:** ctest + rfsim gate; metrics line (A2) gets `pdschq_stale_after_decode`. Commit `"fix(rx): no Technique D credit for decodes whose samples expired mid-decode (K29)"`.

### Task F3: Per-grant / per-hypothesis profile (spec §9 "profile first") (Sonnet; 🔁 Haiku runs)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c` (`g_pdtim_*`, ~75-144: add stages `rm` (rate recovery) and `llr` split from `demod`; per-call hypothesis-count tag)
- Create: `tests/passive_rx/dgx/profile_report.py` (parse the PDTIM lines → JSON + markdown table)

- [ ] **Step 1:** Extend the existing PDTIM timers (enabled by `ISAC_PDCCH_TIMING=1`) with `rm_ns` and `llr_ns`; print µs/grant per stage: fep, chest, demod, llr, rm, ldpc, total; and probes vs full separately.
- [ ] **Step 2:** `profile_report.py` parses lines `SENSING: PDTIM ...` → table. Unit test with a fixture line.
- [ ] **Step 3:** Run (🔁 Haiku) on the DGX: 106 PRB rank 1; 273 PRB rank 1; 273 PRB rank 4 pinned bed; each with `ISAC_PROBE_ALL=0` and `=1`. Commit the report under `tests/passive_rx/dgx_host_snapshot_2026-09-30/profile_<date>.md` and quote the table in PROJECT_MEMORY §14.
- [ ] **Step 4 (decision):** order GPU tasks G2–G4 and R1's shared stages by measured µs; record the decision in the spec §9.4 table.

## GPU track (G1–G4) — DGX only

### Task G1: CUDA LDPC pool safety (K30) (Sonnet; Opus review)

**Files:**
- Modify: `openair1/PHY/CODING/nrLDPC_cuda/ldpc_decoder.cu` (~991-1088), `openair1/PHY/CODING/nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c` (~111-270)
- Test: `openair1/PHY/CODING/nrLDPC_cuda/tests/ldpc_cuda_pool_test.cc` (new; CMake inside the `ENABLE_LDPC_CUDA` block)

Fix list (exact): (1) `ldpc_pool_decode` returns an error code; on any error or skipped launch every affected slot is **poisoned** (output bits filled with a pattern that cannot pass CRC, e.g. CRC of zero inverted) and the TB is reported failed — never a stale pass; (2) requests > 512 CBs are split into several launches instead of silently skipped; (3) bounded request queue (`g_queue`) with rejection → CPU fallback; (4) slot reservation all-or-nothing (no partial holds → no deadlock); (5) waits with timeout (default 50 ms) → CPU fallback + one-shot LOG_W; (6) `CHECK_CUDA` propagates errors; (7) `ldpc_decoder_shutdown` frees the real device pointers.

- [ ] **Step 1: Failing tests** — (a) `SkippedLaunchNeverPasses`: inject a forced launch skip (test hook env `LDPC_CUDA_TEST_SKIP_LAUNCH=1`) after a successful decode of TB A, then decode TB B (different data) → B must FAIL CRC; (b) `OversizeRequestIsSplit`: 600 CBs decode correctly; (c) `QueueFullFallsBackToCpu`; (d) `TimeoutFallsBackToCpu` (hook `LDPC_CUDA_TEST_STALL_MS=200`).
- [ ] **Step 2: Run → FAIL. Step 3: Implement. Step 4: Run → PASS**; `ldpctest -v _cuda` BLER table unchanged vs 2026-10-01 (K32 numbers).
- [ ] **Step 5:** Commit `"fix(gpu): CUDA LDPC pool can no longer produce a stale CRC pass; split, bounded, timed fallback (K30)"`; PROJECT_MEMORY K30 → resolved.

### Task G2: Unified-memory GPU paths on GB10 (Sonnet)

- Modify: the four CUDA modules (`ldpc_decoder.cu`, `nr_pdsch_gpu_fep.cu`, `nr_polar_sc_cuda.cu`, `nr_pdcch_gpu_fep.cu`) behind a CMake option `GPU_UNIFIED_MEMORY` (ON for aarch64 + GB10): replace pinned-staging + `cudaMemcpy` with `cudaMallocManaged` (or mapped host memory + `cudaHostGetDevicePointer`); set `LDPC_CUDA_ARCH` default to `121` when `CMAKE_SYSTEM_PROCESSOR == aarch64` and the detected GPU is GB10 (keep 89 elsewhere).
- [ ] Steps: failing benchmark assertion (copy bytes per launch = 0 with the option ON, measured by a counter) → implement → GPU tests (`nr_pdsch_gpu_fep_test`, `nr_polar_sc_cuda_test`, G1 tests) pass → benchmark table (probe batch 1/32/256 µs, LDPC µs/CB) before/after → commit.

### Task G3: GPU FEP stale samples (K31) (Sonnet)

- Modify: `nr_pdsch_passive_queue.c` (~654-962), `nr_pdsch_gpu_fep.cu` (~446-457): IQ buffers refcounted per slot (the ring slot cannot be reused while a GPU job references it; the producer skips/drops instead of overwriting and counts it) **or** a post-copy lifetime re-check that turns the job INCONCLUSIVE. Choose refcount if F3 shows the copy dominates; else re-check.
- [ ] Steps: failing test with an artificially delayed worker (env hook) proving a stale job is now INCONCLUSIVE/never decoded → implement → `NR_GPU_FEP=1` rfsim A/B vs CPU (CRC equal within spread) → commit; K31 → resolved.

### Task G4 ★: Batched CB0 probes across grants × hypotheses (Opus design review, Sonnet implements)

- Depends on: R1 (GrantWork), G1, G2, F3 decision.
- Design (exact): RT path enqueues `GrantTrial{GrantWork*, main, probes[K−1], generation, config_epoch}` (bounded ring, drop-and-count when full; never blocks); a GPU worker collects up to B trials (default: whatever is queued within 1 slot, max 64 grants), computes LLRs once per signature (Task 4c) per grant, rate-dematches each hypothesis, and runs one batched LDPC launch for all CB0 probes (CUDA decoder, so the P2 same-decoder rule requires the main decode of probed hypotheses to also run on the CUDA decoder or P2 stays off for them); ≥ 2 CUDA streams overlap pre-processing and LDPC; outcomes go back through `feedback_k`; old-epoch trials dropped; GrantWork released by refcount.
- [ ] Steps: unit test of the batching queue (ordering, drop-on-full, epoch drop) → implement → rfsim 273-PRB with K=3: probes/s, GPU util, CPU-core equivalents vs CPU-probe baseline → compute row of spec §1 → commit.

## AFTER THE CLOUD MERGE — runtime (R1–R4)

### Task R1 ★: GrantWork and compute modes (Opus)

- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_td_grantwork.{h,c}`: `nr_td_grantwork_t` = {refcount, IQ slot ref, FEP output, per-signature chest + LLRs (lazily computed, immutable), G per signature, legality masks (Task 4b), config_epoch, generation}; API `grantwork_get_llr(gw, signature)`; built once per grant by the decode consumer.
- Compute modes per context: COLD / VERIFY / TRACKING (spec §9.1); VERIFY entered on an epoch change (reconfiguration spec) with the previous winner first and K ≤ 2; TRACKING forces K = 1.
- [ ] Steps: unit tests (refcount lifetime, lazy per-signature compute exactly once, mode transitions) → wire into the consumer for main + probes (CPU) → `ISAC_TD_PROBE_EQUIV_CHECK` = 0 mismatches → F3 profile again (shared-work saving per extra hypothesis) → commit.

### Task R2: Runtime wiring of the levers (Sonnet; Opus review) — was Task 8

Additions vs the former Task 8: the gate and legality masks come from GrantWork; probes run on GrantWork LLRs (CPU) or via G4 (GPU); `ISAC_TD_MODE_*` thresholds; metrics fields `td_*` incl. `td_probe_equiv_mismatch`, `td_stale_after_decode`, `td_signatures_per_grant`.

- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.{c,h}` — context-level `nr_pdsch_config_sweep_select_k(...)` (same arguments as `_select` + `int K`, `nr_pdsch_sweep_ticket_k_t *ticket`, `nr_pdsch_cfg_hypothesis_t out[]`) and `nr_pdsch_config_sweep_feedback_k(const nr_pdsch_sweep_ticket_k_t *, const nr_td_outcome_t *, int n, nr_pdsch_cfg_hypothesis_t *winner)`; `nr_pdsch_sweep_ticket_k_t` = `nr_pdsch_sweep_ticket_t` + `int n; int hyp[NR_TD_MAX_K]; uint32_t config_epoch;`. Field book is a module-level `nr_td_fieldbook_t` under the existing `g_lock`; when `ISAC_TD_FIELDBOOK=1`, contexts are **not** pruned by `g_prior` (ordering via field book instead); default 0 keeps today's pruning.
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (the `nr_pdsch_config_sweep_select` call, ~6216): build the gate views (layers from the pinned layout's antenna ports, else −1; per-RNTI SNR EMA from earlier decodes) → gate; settled contexts always decode; gated + unsettled → count, no trial; else `select_k` with `K = ISAC_TD_K` (default 1).
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.{c,h}` — job carries the ticket_k; main decode + probes on GrantWork; outcomes (`p2_admissible` = new transmission && completed probe && same decoder && samples valid after decode (F2)); `feedback_k`.
- Side info: SIB1 common TDRA (`nr_pdcch_blind_monitor_set_tda_common` → module-level `nr_td_side_info_t` unless `ISAC_TD_IGNORE_SIB1=1`), MIB `dmrs-TypeA-Position`, observables from the existing oracles.
- Env flags (all default = today): `ISAC_TD_K=1`, `ISAC_TD_GATE=0`, `ISAC_TD_GATE_SNR_MARGIN_DB=6`, `ISAC_TD_W_SIB1/DEFAULT/OBS/FIELD/PROBE=0`, `ISAC_TD_FIELDBOOK=0`, `ISAC_TD_P2=0`, `ISAC_TD_IGNORE_SIB1=0`, `ISAC_TD_PROBE_BUDGET_US=400`.
- [ ] **Step 1:** failing context-level tests `SelectK1EqualsSelect`, `FieldBookOffKeepsPriorPruning`, `FieldBookOnOrdersInsteadOfPruning`.
- [ ] **Step 2–4:** implement; tests green; full ctest; rfsim regression gate with all flags default ⇒ PASS and identical CONVERGED lines vs pre-change.
- [ ] **Step 5:** rfsim with levers on (106 PRB, K=3, gate=1, chosen weights, fieldbook=1): CONVERGED, CRC ≥ 98 %, compute row of spec §1 (avg + peak per thread, `pdschq_max_lag`, drops). Commit.

### Task R3: Live multi-UE acceptance bed — was Task 9

- [ ] **Step 1:** Check the 5G core: `docker ps` (operator runs it if the agent lacks access), confirm an OAI CN5G (AMF/SMF/UPF) is up; else fall back to the OCUDU/OAI-over-ZMQ bed (`tests/passive_rx/run_ocudu_passive.sh`, needs `-DOAI_ZMQ=ON`). Record which bed in PROJECT_MEMORY §10.4.
- [ ] **Step 2:** SA bed: OAI gNB (SA, rfsim, 2 TX for rank 2) + N = 4 OAI UEs with iperf, receiver 4 RX and 1 RX, via the Track-A campaign runner, arms: baseline / all_P1 (/ all_P2 if Task 7 passed) / compute ablation arms of spec §9.5; cells SA and NSA-like (`ISAC_TD_IGNORE_SIB1=1`); ≥ 5 runs per arm; UE detach/re-attach every 60 s for churn.
- [ ] **Step 3:** Score per RNTI time-to-CONVERGED, wrong winners vs gNB truth (validation only), compute row incl. GPU utilisation reported separately.
- [ ] **Step 4:** Commit campaign summaries; PROJECT_MEMORY §14 subsection; K-entries for failures. Follow-up: simulator replay mode (spec §6.1) fed with this bed's A3 observation records.

### Task R4: Defaults, documentation, review (Opus) — was Task 10

- [ ] **Step 1:** If targets met with 0 wrong: set chosen defaults (K, weights, gate, fieldbook, P2, GPU paths) in code; else keep neutral defaults and record the gap.
- [ ] **Step 2:** PROJECT_MEMORY: §23.9, §10.2 env table, §11.8 new log lines, §16, §24 (K28–K32 status), §25.
- [ ] **Step 3:** `/code-review` on `adaptive-rx-UL-DL..td/convergence-levers`; superpowers:finishing-a-development-branch; push.

## Self-review record (revision 2, 2026-10-01)

- Spec coverage: §1 targets → Tasks 6, 7, R3, R4; §2 principles → Global Constraints + Task 4 tests; §4.1 gate → Task 1 + R2; §4.2 exclusions → Task 4b (+ R2 masks); §4.3 → Tasks 4, R2; §4.4 probe decoder → F1 (correct chest), F2, R1, G4; §4.5 → Tasks 2, 4; §4.6 → Tasks 3, R2; §5 P1/P2 (+ §9.3 same decoder) → Tasks 4, 6, 7, R2; §6.1 simulator (shared-IQ, SNR error) → Task 5, replay mode → R3 follow-up; §6.2 → R3; §6.3/§9.5 ablation → Tasks 6, 7, R3; §6.4 → Tasks 4, R2; §9.1 GrantWork → R1, legality bitsets → 4b, signatures → 4c, information-aware ordering → optional term, not scheduled (add to Task 2 only if Task 6 shows ordering is the bottleneck), modes → R1; §9.2 GPU → G1–G4; §9.4 V1–V9 → F1 (V1), F1/R1 (V2), F2 (V3), G1 (V4, V6), G3 (V5), G2 (V7), V8 out of scope (PDCCH GPU), V9 → K32 x86 comparison in R4 notes.
- Known limitation: Task 4b's real reject vectors must be captured from a live log (Step 2 explains how); an empty vector table fails review.
