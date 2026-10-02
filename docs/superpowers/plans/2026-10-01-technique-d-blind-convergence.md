# Technique D blind-case convergence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Technique D converge when the DM-RS/Qm oracles are missing or wrong ("blind"), without wrong winners, using exact equivalence crediting, a reversible-pruning field book with fail-open, and (pending approval) CRC-pass acceptance.

**Architecture:** Engine changes stay in `nr_pdsch_config_sweep.{c,h}` (per-hypothesis evidence, dormant masks, acceptance) and the pure modules `nr_td_legal` (equivalence key) and `nr_td_fieldbook` (field state machine). Everything is first validated in the shared-IQ simulator `nr_td_sim` against today's runtime; runtime wiring stays in levers-plan Task R2. Defaults keep today's behaviour bit-identical.

**Tech Stack:** C11 (OAI), C++17 gtest, Python 3 campaign runner, Ninja/CMake on the DGX (aarch64).

**Spec:** `docs/superpowers/specs/2026-10-01-technique-d-blind-convergence-design.md` (addendum; wins over) `docs/superpowers/specs/2026-10-01-technique-d-convergence-levers-design.md`.

## Global Constraints

- All new flags default off (`--equiv 0`, `--crc-accept 0`, `--fieldbook 0`, `ISAC_TD_EQUIV=0`, `ISAC_TD_CRC_ACCEPT=0`, `ISAC_TD_FIELDBOOK=0`) ⇒ **bit-identical** hypothesis sequence and winner vs today for the same RNG seed.
- The KL anytime acceptance rule (`nr_crc_interval()`, 1e-6 budget, separation test, 300-trial fallback) is not changed. Lever C (CRC-pass acceptance) is an **additional, experimental** rule behind a default-off flag (operator approved design + simulation 2026-10-01); runtime enablement is a separate operator decision after BC6. Its 1e-6 argument is analytical; Monte Carlo exposes implementation/correlation errors only.
- MCS table is never promoted cell-wide. Side information is "ordering score", never "prior".
- Dormant hypotheses accumulate no evidence; a cause that would leave zero active hypotheses is refused.
- Field-book promotion/contradiction only from **independently converged RNTIs in the current `config_epoch`** (field not pruned in that context, or context in fail-open). Old-epoch support never maintains pruning.
- Fail-open is counted in eligible trials, not wall-clock time: `N_fo = ceil(n_active · ln(1/α) / p_min)`, α = 1e-3, p_min = 0.05.
- Simulator numbers are labelled `[SIMULATED, DGX host, nr_td_sim @<commit>]`; never mixed with rfsim/OTA numbers.
- Repository rules (CLAUDE.md): sens6 frozen (`git diff --quiet sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf tests/passive_rx/sens6_host_snapshot_2026-09-30` before every commit); `git add <explicit paths>`, never `-A`, never `git stash`; never build while `nr-uesoftmodem` runs; commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Build: `cmake_targets/ran_build/build`, `ninja tests nr_td_sim`; focused tests `ctest -R "nr_td|td_sim|pdsch_config_sweep"`; full `ctest -j4` before each code commit (known ARM failures only: dft_test, test_nr_modulation, test_nr_pusch_ra0_qam256, test_nr_pusch_ra0_qam64 intermittent). Simulator campaigns: ≤ 8 parallel processes.

## Review Focus

- Low-traffic RNTI: no trials ⇒ the fail-open counter must not advance (counts trials, not time) — BC3 test `FailOpenCountsTrialsNotTime`.
- A promoted field whose value no catalogue entry carries would make a context empty — `set_dormant` must refuse and leave it fully active — BC3 test `DormantRefusesToEmptyCatalogue`.
- Asynchronous feedback for a hypothesis that became dormant between select and feedback must add no evidence — BC3 test `FeedOnDormantIsIgnored`.
- Epoch bump while fields prune: afterwards no field prunes and callers restore field masks — BC4 test `EpochBumpStopsPruningKeepsHint`.
- HARQ retransmission decoded by a `k0 ± 1` neighbour must never be accepted by lever C — BC2 test `HarqTrapNeverAcceptedByCrcRule`.

## File map

| File | Responsibility | Tasks |
|---|---|---|
| `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc`, `nr_td_sim_test.cc` | simulator + tests | BC0, BC1, BC2, BC5 |
| `openair1/PHY/NR_UE_TRANSPORT/nr_td_legal.{h,c}`, `tests/nr_td_legal_test.cc` | `nr_td_equiv_key` | BC1 |
| `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.{h,c}`, `tests/nr_pdsch_config_sweep_test.cc` | `feed_equiv`, lever C, dormant masks, fail-open, `prune_keep` | BC1, BC2, BC3 |
| `openair1/PHY/NR_UE_TRANSPORT/nr_td_fieldbook.{h,c}`, `tests/nr_td_fieldbook_test.cc` | field state machine | BC4 |
| `tests/passive_rx/td_sim/campaign.py`, `test_campaign.py`, `gate_bc.json`, `results_<date>_bc/` | campaigns | BC0, BC6 |

Order (operator-confirmed): BC0 → BC1 → BC3 → BC4 → BC5 → BC2 (experimental) → BC2b (experimental, operator 2026-10-01) → BC7 (K39) → BC8 (sim v2) → BC9 (DCI adjacency) → BC10 (fast-path stream) → BC11 (adaptive k0 test) → BC6. BC3 and BC4 touch different files and may run in parallel worktrees; BC5 needs both.

---

### Task BC0: Simulator realism — imperfect oracles, HARQ trap, CRC false pass (Sonnet; 🔁 Haiku runs)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc` (SimCfg/RntiRec/SimResult, oracle block, `full_pass`, CLI, summary)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim_test.cc`
- Modify: `tests/passive_rx/td_sim/campaign.py`, `tests/passive_rx/td_sim/test_campaign.py` (new columns)
- Create: `tests/passive_rx/td_sim/baseline_bc0_2026-10-01.txt`

**Interfaces:**
- Produces CLI: `--oracle-miss P` (default 0), `--oracle-wrong P` (default 0), `--harq-trap P` (default 0), `--crc-false P` (default 0; use 5.96e-8 = 2⁻²⁴ in campaigns). JSON per RNTI adds `"oracle_state":"ok|miss|wrong"`; summary adds `"oracle_miss_rntis"`, `"oracle_wrong_rntis"`, `"harq_trap_passes"`, `"false_passes"`.
- Produces SimCfg fields `double oracle_miss, oracle_wrong, harq_trap, crc_false;` (defaults 0).
- Random streams: a new per-RNTI stream `orng = mt19937_64(mix(seed, a, 0x400 + k))` for oracle draws and a per-RNTI `frng = mix(seed, a, 0x500 + k)` for trap/false-pass draws, so the channel stream `crng` is unchanged (arms stay paired).

Model (exact):
- Per RNTI draw once from `orng`: `u < oracle_miss` ⇒ state `miss` (no DM-RS observation, no Qm sighting for this RNTI); else `u < oracle_miss + oracle_wrong` ⇒ state `wrong`; else `ok`.
- `wrong`: the DM-RS observation uses a decoy: the first catalogue entry (template order, starting at index `orng() % n_hyp`) whose `(dmrs_mask, tda_start + tda_length)` differs from the truth's; prune to the decoy's `(dmrs_mask, S+L, k0 = truth k0)`. The Qm sighting reports `qm_of(mcs, wrong_table)` with `wrong_table = (truth_table + 1) % 3`.
- Cell-wide pre-pruning of RNTIs `k >= 2` (`if (cfg.oracle && k >= 2) do_observe();`) happens only if at least 2 earlier RNTIs of the acquisition had state `ok`.
- `harq_trap`: on each grant, with probability P (from `frng`), every alive hypothesis that equals the truth except `k0' = k0 ± 1` (same table) passes (`full_pass` returns true for it); counted in `harq_trap_passes`.
- `crc_false`: any decode that `full_pass` would FAIL passes instead with probability P (from `frng`); counted in `false_passes`.

- [ ] **Step 1: Failing tests** (append to `nr_td_sim_test.cc`):
```cpp
TEST(TdSim, DefaultsUnchangedByRealismFlags) {
  SimCfg a = SimCfg::defaults(); a.acq = 20; a.seed = 11;
  SimCfg b = a; b.oracle_miss = 0; b.oracle_wrong = 0; b.harq_trap = 0; b.crc_false = 0;
  EXPECT_EQ(run_sim(a).total_grants, run_sim(b).total_grants);
}
TEST(TdSim, OracleMissAllIsBlind) {
  SimCfg a = SimCfg::defaults(); a.acq = 10; a.seed = 5; a.oracle = 1; a.oracle_miss = 1.0;
  SimCfg b = a; b.oracle = 0; b.oracle_miss = 0;
  EXPECT_EQ(run_sim(a).total_grants, run_sim(b).total_grants);
}
TEST(TdSim, OracleWrongPrunesTheTruthAndNeverMakesAWrongWinner) {
  SimCfg c = SimCfg::defaults(); c.acq = 10; c.seed = 3; c.oracle_wrong = 1.0; c.cap_s = 60;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.undecidable, 0); /* today's runtime has no recovery from a destructive wrong oracle */
}
TEST(TdSim, HarqTrapPassesAreCounted) {
  SimCfg c = SimCfg::defaults(); c.acq = 5; c.oracle = 0; c.harq_trap = 0.05; c.rntis_per_acq = 1;
  EXPECT_GT(run_sim(c).harq_trap_passes, 0);
}
```
- [ ] **Step 2: Run → FAIL** (`ninja nr_td_sim_test && ctest -R td_sim --output-on-failure`; expected: compile errors on the new fields).
- [ ] **Step 3: Implement** the model above in `nr_td_sim.cc` (SimCfg fields + defaults, CLI parsing next to `--table-exercise`, the per-RNTI `orng` draw before `do_observe` is first used, decoy selection, the `k >= 2` condition, `full_pass` extensions with `frng`, counters, JSON fields). Keep `wrong` semantics unchanged (a winner pruned to a decoy can only be non-truth ⇒ counted wrong; the test asserts this does not happen because all decoy-catalogue hypotheses fail).
- [ ] **Step 4: Run → PASS**; `python3 tests/passive_rx/td_sim/test_campaign.py` (add the new summary columns `oracle_miss_rntis`, `oracle_wrong_rntis`, `harq_trap_passes`, `false_passes` to `campaign.py` summary.md and to the fake-simulator fixture).
- [ ] **Step 5: Baselines** (🔁 Haiku, ≤ 8 parallel): `--acq 2000 --seed 1` at 4 RX and 1 RX for each of: `--oracle 1`; `--oracle 0`; `--oracle 1 --oracle-miss 0.3`; `--oracle 1 --oracle-wrong 0.05`; `--oracle 0 --harq-trap 0.01 --crc-false 5.96e-8`. Write raw summary lines + cold/steady split to `baseline_bc0_2026-10-01.txt` with label `[SIMULATED, DGX host, nr_td_sim @<commit>]`.
- [ ] **Step 6: Commit** — `git add` the five files above; message `test(td): nr_td_sim imperfect oracles (miss/wrong), HARQ trap, CRC false pass + baselines`.

---

### Task BC1 ★: Equivalence crediting (lever E) (Opus)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_td_legal.h`, `nr_td_legal.c`; Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_legal_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h`, `nr_pdsch_config_sweep.c`; Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc`, `nr_td_sim_test.cc`

**Interfaces:**
- Produces (nr_td_legal.h):
```c
/* Exact grant-equivalence key: equal keys <=> identical receiver computation on a grant (same geometry, DM-RS,
 * layers, Qm and target code rate R x1024 of the grant's MCS under the hypothesis's table). Never merges distinct
 * computations (may over-split via add_pos/max_len). */
uint64_t nr_td_equiv_key(const nr_pdsch_cfg_hypothesis_t *h, int nl, int qm, uint32_t code_rate_x1024);
```
  Implementation: `return nr_td_signature(h, nl, qm) ^ ((uint64_t)(code_rate_x1024 & 0x3FFFF) << 42);` (bits 42..59; the signature uses bits 0..41).
- Produces (nr_pdsch_config_sweep.h):
```c
/* Credit one full-TB outcome to idx[0] (the decoded hypothesis) and to its grant-equivalent alive hypotheses
 * idx[1..n-1]. Duplicates and out-of-range indices are ignored. n == 1 is bit-identical to
 * nr_pdsch_config_sweep_feed(st, idx[0], tb_crc_ok). The acceptance check runs once, after crediting, whenever any
 * credited hypothesis reached a multiple of 16 trials. Returns the winner or -1. */
int nr_pdsch_config_sweep_feed_equiv(nr_pdsch_config_sweep_state_t *st, const int *idx, int n, bool tb_crc_ok,
                                     bool new_data /* new transmission (NDI toggled); used only by lever C (BC2) */);
```
- Produces simulator flag `--equiv 0|1` (SimCfg `int equiv`, default 0). With `equiv=1` the sim requires `twins >= 2` (exit with an error otherwise: the stress arm is not equivalence-consistent). Class of decoded hypothesis `d` on a grant: all alive `j` with identical `tda_start, tda_length, k0, dmrs_add_pos, dmrs_max_len, dmrs_mask, mapping_type` and (`hyp[j].mcs_table == hyp[d].mcs_table` or `!gr.exercised`). Only the main (K=1) full decode is credited this way; probes are unchanged.
- Runtime wiring (levers-plan R2, not here): the queue computes each alive hypothesis's key with `nr_get_Qm_dl(mcs, table)` and `nr_get_code_rate_dl(mcs, table)` (openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h:275-276).

- [ ] **Step 1: Failing tests**
`nr_td_legal_test.cc`:
```cpp
TEST(TdEquiv, SameComputationSameKey) {
  nr_pdsch_cfg_hypothesis_t a = {}; a.tda_start = 2; a.tda_length = 12; a.dmrs_mask = 0x884; a.dmrs_max_len = 1;
  nr_pdsch_cfg_hypothesis_t b = a; b.mcs_table = 1; /* differs only in table */
  EXPECT_EQ(nr_td_equiv_key(&a, 1, 4, 490), nr_td_equiv_key(&b, 1, 4, 490)); /* same Qm and R on this MCS */
}
TEST(TdEquiv, DifferentRateOrGeometryDifferentKey) {
  nr_pdsch_cfg_hypothesis_t a = {}; a.tda_start = 2; a.tda_length = 12; a.dmrs_mask = 0x884; a.dmrs_max_len = 1;
  nr_pdsch_cfg_hypothesis_t b = a;
  EXPECT_NE(nr_td_equiv_key(&a, 1, 4, 490), nr_td_equiv_key(&b, 1, 4, 553));
  b.k0 = 1;
  EXPECT_NE(nr_td_equiv_key(&a, 1, 4, 490), nr_td_equiv_key(&b, 1, 4, 490));
}
```
`nr_pdsch_config_sweep_test.cc`:
```cpp
TEST(PdschSweepEquiv, SingleIsBitIdenticalToFeed) {
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4); memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  for (int t = 0; t < 20000; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(a.get(), &h);
    const int j = nr_pdsch_config_sweep_next(b.get(), &h); ASSERT_EQ(i, j);
    const bool ok = (i == 7) && (t % 3 == 0);
    const int wa = nr_pdsch_config_sweep_feed(a.get(), i, ok), wb = nr_pdsch_config_sweep_feed_equiv(b.get(), &i, 1, ok, true);
    ASSERT_EQ(wa, wb); if (wa >= 0) break;
  }
  EXPECT_EQ(0, memcmp(a->trials, b->trials, sizeof(a->trials)));
  EXPECT_EQ(0, memcmp(a->ok, b->ok, sizeof(a->ok)));
}
TEST(PdschSweepEquiv, CreditsEveryMemberOnceIgnoringDuplicates) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int idx[] = {3, 5, 5, 9, -1, 1 << 20};
  nr_pdsch_config_sweep_feed_equiv(s.get(), idx, 6, true, true);
  EXPECT_EQ(s->trials[3], 1u); EXPECT_EQ(s->trials[5], 1u); EXPECT_EQ(s->trials[9], 1u);
  EXPECT_EQ(s->ok[3], 1u); EXPECT_EQ(s->ok[5], 1u); EXPECT_EQ(s->ok[9], 1u);
}
```
`nr_td_sim_test.cc`:
```cpp
TEST(TdSim, EquivNeverWrongAndNotSlowerBlind) {
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 9; c.oracle = 0; c.rntis_per_acq = 1;
  SimCfg e = c; e.equiv = 1;
  const SimResult rc = run_sim(c), re = run_sim(e);
  EXPECT_EQ(re.wrong, 0); EXPECT_EQ(re.undecidable, 0);
  EXPECT_LE(re.mean_grants, rc.mean_grants);
}
```
- [ ] **Step 2: Run → FAIL** (undefined `nr_td_equiv_key`, `nr_pdsch_config_sweep_feed_equiv`, `SimCfg::equiv`).
- [ ] **Step 3: Implement.** In `nr_pdsch_config_sweep.c` move the acceptance block of `nr_pdsch_config_sweep_feed` (from `if ((st->trials[idx] % 16) == 0)` through the fallback) into `static int sweep_decide(nr_pdsch_config_sweep_state_t *st, bool check_separation)` and make `feed` call it with `check_separation = (st->trials[idx] % 16) == 0` (pure refactor, keep comments). `feed_equiv`: if `st == NULL || n < 1` return as `feed` does; credit each distinct in-range index once (`trials++`, `ok++` on pass), set `check = true` if any credited index has `trials % 16 == 0`, then `return sweep_decide(st, check)`. In the sim, when `cfg.equiv` and `i == 0`, build the class (above) over the current state and call `feed_equiv` for the main outcome, `feed_k` for the probe outcomes only (pass `out + 1, n - 1` when `n > 1`).
- [ ] **Step 4: Run → PASS** (focused ctest); full `ctest -j4`.
- [ ] **Step 5: Measure** (🔁 Haiku): `--acq 2000 --seed 1 --equiv 1` at oracle 0 and 1, 4/1 RX; append to `tests/passive_rx/td_sim/baseline_bc0_2026-10-01.txt` under `## BC1 equiv`.
- [ ] **Step 6: Commit** — `feat(td): exact grant-equivalence crediting (feed_equiv, nr_td_equiv_key) + simulator arm`.

---

### Task BC2 ★: CRC-pass acceptance (lever C), experimental, default off (Opus)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h`, `.c`; Test: `tests/nr_pdsch_config_sweep_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc`, `nr_td_sim_test.cc`

**Interfaces:**
- Consumes: `nr_pdsch_config_sweep_feed_equiv` (BC1), dormant/active API (BC3: when BC3 has landed, "alive" = active).
- Produces (state fields, cleared wherever `trials`/`ok`/`probe_*` are cleared, including `prune_commit` and `nr_pdsch_config_sweep_rebuild`):
```c
uint16_t ok_unique[NR_PDSCH_SWEEP_MAX_HYP]; ///< passes on grants where the hypothesis was alone in its equivalence class
bool     crc_accept;          ///< lever C enabled (configuration: preserved across rebuild like side/p2)
bool     crc_accept_blocked;  ///< a second hypothesis has a unique pass: lever C off until the next prune/rebuild
```
```c
/* Smallest m >= 2 with n_alive * C(t_max, m) * 2^(-24 m) <= 1e-6 (log domain). */
int nr_pdsch_config_sweep_crc_accept_m(int n_alive, uint32_t t_max);
```
- Rule inside `feed_equiv` after crediting: if `st->crc_accept && !st->crc_accept_blocked && tb_crc_ok && new_data && n_credited == 1` then `ok_unique[idx[0]]++` (saturating). A HARQ retransmission (`new_data == false`) never adds a unique pass. The simulator passes `gr.new_tx`. Then, before `sweep_decide`: let `U` = set of alive hypotheses with `ok_unique > 0`; if `|U| >= 2` set `crc_accept_blocked = true`; else if `|U| == 1`, `L ∈ U`, and `ok_unique[L] >= crc_accept_m(n_alive, max_trials_alive)` ⇒ `st->winner = L; return L`.
- Simulator `--crc-accept 0|1` (requires `--equiv 1`); summary adds `"crc_accepts"` (decisions made by lever C).

- [ ] **Step 1: Failing tests**
```cpp
TEST(PdschSweepCrcAccept, MValues) {
  EXPECT_EQ(nr_pdsch_config_sweep_crc_accept_m(10, 100), 2);
  EXPECT_EQ(nr_pdsch_config_sweep_crc_accept_m(750, 1000), 3);
  EXPECT_EQ(nr_pdsch_config_sweep_crc_accept_m(1, 0), 2);
}
TEST(PdschSweepCrcAccept, TwoUniquePassesDecideWhenClean) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->crc_accept = true;
  const int a = 7; int w = nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true); EXPECT_EQ(w, -1);
  w = nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true); EXPECT_EQ(w, 7);
}
TEST(PdschSweepCrcAccept, SecondUniquePasserBlocksTheRule) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->crc_accept = true;
  const int a = 7, b = 8;
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  nr_pdsch_config_sweep_feed_equiv(s.get(), &b, 1, true, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true), -1);
  EXPECT_TRUE(s->crc_accept_blocked);
}
TEST(PdschSweepCrcAccept, SharedPassIsNotUnique) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->crc_accept = true;
  const int cls[] = {7, 9};
  nr_pdsch_config_sweep_feed_equiv(s.get(), cls, 2, true, true); nr_pdsch_config_sweep_feed_equiv(s.get(), cls, 2, true, true);
  EXPECT_EQ(s->ok_unique[7], 0); EXPECT_EQ(s->winner, -1);
}
TEST(PdschSweepCrcAccept, RetransmissionPassIsNotUnique) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->crc_accept = true;
  const int a = 7;
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  EXPECT_EQ(nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, false), -1); /* HARQ retx of the same TB */
  EXPECT_EQ(s->ok_unique[7], 1);
}
```
`nr_td_sim_test.cc` (Review Focus 5):
```cpp
TEST(TdSim, HarqTrapNeverAcceptedByCrcRule) {
  SimCfg c = SimCfg::defaults(); c.acq = 200; c.seed = 21; c.oracle = 0; c.equiv = 1; c.crc_accept = 1;
  c.harq_trap = 0.02; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0);
  EXPECT_GT(r.crc_accepts, 0);
}
```
- [ ] **Step 2: Run → FAIL.** **Step 3: Implement** (fields, clearing sites, `crc_accept_m` with `lgamma`, rule in `feed_equiv`; sim flag/counter). **Step 4: Run → PASS**; full ctest.
- [ ] **Step 4b: Analytical check (stress arm):** run `--oracle 0 --equiv 1 --crc-accept 1 --crc-false 1e-3 --acq 2000` and compare the measured wrong-winner rate with the bound `P(wrong) <= sum_h C(T_h, m*) p_f^m*` evaluated on the run's trial counts (print both in the report). The measured rate must not exceed the bound; at the default `p_f = 2^-24` the bound (not the Monte Carlo) is the evidence for 1e-6.
- [ ] **Step 5: Commit** — `feat(td): CRC-pass acceptance behind a default-off flag (lever C) + simulator arm`.

---

### Task BC2b ★: Partition (geometry) acceptance (lever P), experimental, default off (Opus; Sonnet by operator override 2026-10-01)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_td_legal.h`, `.c` (`nr_td_geom_key`); Test: `tests/nr_td_legal_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h`, `.c`; Test: `tests/nr_pdsch_config_sweep_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc`, `nr_td_sim_test.cc`

**Interfaces:**
- Consumes: BC2 (`feed_equiv` with `new_data`, `lever_c_restart`, `crc_accept_m`), BC3 (dormant causes, `active()`, `n_active`).
- Produces (nr_td_legal.h):
```c
/* Geometry group of a hypothesis: the fields a CRC pass pins regardless of the MCS table. */
uint64_t nr_td_geom_key(const nr_pdsch_cfg_hypothesis_t *h);
/* = tda_start | tda_length << 4 | k0 << 8 | mapping_type << 14 | (dmrs_mask & 0x3FFF) << 16 */
```
- Produces (nr_pdsch_config_sweep.h):
```c
#define NR_TD_DORMANT_GEOM 4          /* new dormant cause; NR_TD_DORMANT_CAUSES becomes 5 */
#define NR_TD_GEOM_SLOTS 8
/* state fields (evidence: cleared wherever ok_unique is cleared, including lever_c_restart) */
uint64_t geom_key[NR_TD_GEOM_SLOTS]; uint16_t ok_geom[NR_TD_GEOM_SLOTS]; int n_geom; bool geom_blocked;
bool     geom_pin;   /* configuration (preserved across rebuild like crc_accept), default false */
```
- Rule inside `feed_equiv`, **before** lever C and `sweep_decide`: if `geom_pin && !geom_blocked && tb_crc_ok && new_data && active(idx[0])`: add 1 to the slot of `nr_td_geom_key(&hyp[idx[0]])` (new slot if absent; if `n_geom == NR_TD_GEOM_SLOTS` ⇒ `geom_blocked = true`). If ≥ 2 slots have `ok_geom > 0` ⇒ `geom_blocked = true`. Else if the single slot G has `ok_geom >= crc_accept_m(n_groups_active, T_max)` (n_groups_active = number of distinct geometry keys among active hypotheses; T_max = max trials over active) ⇒ `set_dormant(st, NR_TD_DORMANT_GEOM, keep = geom_key == G)`; the resulting active-set change restarts lever C/P evidence (`lever_c_restart` also clears `geom_key/ok_geom/n_geom/geom_blocked`). If `set_dormant` refuses (would empty), do nothing.
- Simulator: `--geom-pin 0|1` (SimCfg `int geom_pin`); requires the main decode to go through `feed_equiv` (pass the full class as BC1/BC2 define; with `--equiv 0` pass the singleton class `{idx[0]}` — lever P needs no crediting, only attribution). Summary adds `"geom_pins"`, `"geom_blocks"`.

- [ ] **Step 1: Failing tests**
```cpp
TEST(TdEquiv, GeomKeyIgnoresTableAndAddPos) {
  nr_pdsch_cfg_hypothesis_t a = {}; a.tda_start = 2; a.tda_length = 12; a.dmrs_mask = 0x884;
  nr_pdsch_cfg_hypothesis_t b = a; b.mcs_table = 1; b.dmrs_add_pos = 2;
  EXPECT_EQ(nr_td_geom_key(&a), nr_td_geom_key(&b));
  b = a; b.k0 = 1; EXPECT_NE(nr_td_geom_key(&a), nr_td_geom_key(&b));
}
TEST(PdschSweepGeomPin, TwoPassesPinTheGeometryReversibly) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->geom_pin = true;
  const int a = 7; const uint64_t g = nr_td_geom_key(&s->hyp[a]);
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), nr_td_geom_key(&s->hyp[i]) == g);
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, SecondGeometryWithAPassBlocks) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->geom_pin = true;
  int a = 7, b = -1;
  for (int i = 0; i < s->n_hyp && b < 0; i++) if (nr_td_geom_key(&s->hyp[i]) != nr_td_geom_key(&s->hyp[a])) b = i;
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  nr_pdsch_config_sweep_feed_equiv(s.get(), &b, 1, true, true);
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  EXPECT_TRUE(s->geom_blocked); EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, RetransmissionDoesNotCount) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4); s->geom_pin = true;
  const int a = 7;
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, true);
  nr_pdsch_config_sweep_feed_equiv(s.get(), &a, 1, true, false);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepGeomPin, OffIsBitIdentical) { /* geom_pin false: same sequence/winner as today (reuse the BC3 NoMasksIsBitIdentical pattern with feed_equiv) */ }
```
`nr_td_sim_test.cc`:
```cpp
TEST(TdSim, GeomPinNeverWrongUnderHarqTrapAndFalsePasses) {
  SimCfg c = SimCfg::defaults(); c.acq = 200; c.seed = 31; c.oracle = 0; c.geom_pin = 1;
  c.harq_trap = 0.02; c.crc_false = 1e-4; c.rntis_per_acq = 1;
  const SimResult r = run_sim(c); EXPECT_EQ(r.wrong, 0); EXPECT_GT(r.geom_pins, 0);
}
TEST(TdSim, GeomPinRecoversFromWrongPriorViaFailOpen) {
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 33; c.oracle = 0; c.fieldbook = 2; c.inject_wrong_field = 0; c.geom_pin = 1;
  const SimResult r = run_sim(c); EXPECT_EQ(r.wrong, 0); EXPECT_EQ(r.undecidable, 0);
}
TEST(TdSim, GeomPinFasterBlind) {
  SimCfg c = SimCfg::defaults(); c.acq = 30; c.seed = 35; c.oracle = 0; c.rntis_per_acq = 1;
  SimCfg p = c; p.geom_pin = 1;
  EXPECT_LT(run_sim(p).mean_grants, 0.5 * run_sim(c).mean_grants);
}
```
(The implementer writes `OffIsBitIdentical` in full following `PdschSweepDormant.NoMasksIsBitIdentical`.)
- [ ] **Step 2: Run → FAIL. Step 3: Implement** (engine rule, cause 5, clearing in `lever_c_restart`/`clear_probe_stats`, sim flag + counters). **Step 4: Run → PASS**; full ctest.
- [ ] **Step 4b: Analytical check:** `--oracle 0 --geom-pin 1 --crc-false 1e-3 --acq 2000`: measured wrong rate ≤ the bound `sum over wrong groups C(T_g, m_P*) p_f^m_P*` (print both).
- [ ] **Step 5: Commit** — `feat(td): partition (geometry) acceptance behind a default-off flag (lever P) + simulator arm`.

---

### Task BC3 ★: Engine dormant masks + fail-open (Opus)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h`, `.c`; Test: `tests/nr_pdsch_config_sweep_test.cc`

**Interfaces:**
- Produces (nr_pdsch_config_sweep.h):
```c
#define NR_TD_DORMANT_PRIOR 0
#define NR_TD_DORMANT_FIELD_BASE 1 /* + nr_td_field_t (nr_td_fieldbook.h) */
#define NR_TD_DORMANT_CAUSES 4
#define NR_TD_DWORDS ((NR_PDSCH_SWEEP_MAX_HYP + 63) / 64)
/* state fields (configuration+membership; compacted by every destructive prune, preserved by rebuild): */
uint64_t dormant[NR_TD_DORMANT_CAUSES][NR_TD_DWORDS];
bool     fail_open;   ///< all hypotheses active regardless of dormant masks (per context)
uint32_t since_pass;  ///< credited trials since the last PASS of an active hypothesis
typedef bool (*nr_td_keep_fn_t)(const nr_pdsch_cfg_hypothesis_t *h, const void *arg);
int  nr_pdsch_config_sweep_set_dormant(nr_pdsch_config_sweep_state_t *st, int cause, nr_td_keep_fn_t keep, const void *arg);
     /* marks !keep hypotheses dormant for `cause`; returns the number newly dormant, -1 if cause is out of range or the
      * result would leave zero active hypotheses (nothing changed) */
int  nr_pdsch_config_sweep_clear_dormant(nr_pdsch_config_sweep_state_t *st, int cause); /* returns number re-activated */
void nr_pdsch_config_sweep_set_fail_open(nr_pdsch_config_sweep_state_t *st, bool on);
bool nr_pdsch_config_sweep_is_active(const nr_pdsch_config_sweep_state_t *st, int i);
int  nr_pdsch_config_sweep_n_active(const nr_pdsch_config_sweep_state_t *st);
bool nr_pdsch_config_sweep_fail_open_due(const nr_pdsch_config_sweep_state_t *st, double alpha, double p_min);
     /* since_pass >= ceil(n_active * ln(1/alpha) / p_min) and !fail_open */
int  nr_pdsch_config_sweep_prune_keep(nr_pdsch_config_sweep_state_t *st, nr_td_keep_fn_t keep, const void *arg);
     /* destructive prune by predicate == prune_commit semantics (evidence reset, cursor 0), compacting dormant masks */
```
- Semantics: active(i) = `fail_open || no cause marks i`. `next`/`next_k` skip inactive entries in the round order without changing RNG consumption (the shuffle still covers all `n_hyp`); the exploit hot candidate must be active. `feed`, `feed_k`, `feed_equiv` ignore inactive indices (no evidence). The acceptance (leader search, separation test, union-bound class count `n_active`, fallback min-trials, ratio test) ranges over active hypotheses. `since_pass`: +1 per feed call that credited at least one active hypothesis; reset to 0 on a PASS credited to an active hypothesis. With all masks zero and `fail_open == false` every function is bit-identical to today.

- [ ] **Step 1: Failing tests**
```cpp
static bool keep_even(const nr_pdsch_cfg_hypothesis_t *h, const void *) { return (h->tda_length % 2) == 0; }
static bool keep_none(const nr_pdsch_cfg_hypothesis_t *, const void *) { return false; }
TEST(PdschSweepDormant, NoMasksIsBitIdentical) {
  auto a = std::make_unique<nr_pdsch_config_sweep_state_t>(), b = std::make_unique<nr_pdsch_config_sweep_state_t>();
  nr_pdsch_config_sweep_init(a.get(), 4); memcpy((void *)b.get(), (void *)a.get(), sizeof(*a));
  ASSERT_GT(nr_pdsch_config_sweep_set_dormant(b.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr), 0);
  nr_pdsch_config_sweep_clear_dormant(b.get(), NR_TD_DORMANT_PRIOR);
  for (int t = 0; t < 30000; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(a.get(), &h), j = nr_pdsch_config_sweep_next(b.get(), &h);
    ASSERT_EQ(i, j); const bool ok = (i == 11) && (t % 2 == 0);
    ASSERT_EQ(nr_pdsch_config_sweep_feed(a.get(), i, ok), nr_pdsch_config_sweep_feed(b.get(), j, ok));
  }
}
TEST(PdschSweepDormant, DormantNeverSelectedAndAccumulatesNothing) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE, keep_even, nullptr);
  for (int t = 0; t < 5000; t++) {
    nr_pdsch_cfg_hypothesis_t h; const int i = nr_pdsch_config_sweep_next(s.get(), &h);
    ASSERT_TRUE(nr_pdsch_config_sweep_is_active(s.get(), i)); nr_pdsch_config_sweep_feed(s.get(), i, false);
  }
  for (int i = 0; i < s->n_hyp; i++) if (!nr_pdsch_config_sweep_is_active(s.get(), i)) ASSERT_EQ(s->trials[i], 0u);
}
TEST(PdschSweepDormant, FeedOnDormantIsIgnored) { /* Review Focus 3 */
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  int odd = -1; for (int i = 0; i < s->n_hyp && odd < 0; i++) if (s->hyp[i].tda_length % 2) odd = i;
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_feed(s.get(), odd, true); nr_pdsch_config_sweep_feed_equiv(s.get(), &odd, 1, true, true);
  EXPECT_EQ(s->trials[odd], 0u); EXPECT_EQ(s->ok[odd], 0u);
}
TEST(PdschSweepDormant, DormantRefusesToEmptyCatalogue) { /* Review Focus 2 */
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  EXPECT_EQ(nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_none, nullptr), -1);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepDormant, ClearRestoresOnlyItsCause) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  const int n0 = s->n_hyp;
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  const int n1 = nr_pdsch_config_sweep_n_active(s.get());
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE, [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->k0 == 0; }, nullptr);
  ASSERT_LT(nr_pdsch_config_sweep_n_active(s.get()), n1);
  nr_pdsch_config_sweep_clear_dormant(s.get(), NR_TD_DORMANT_FIELD_BASE);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), n1); EXPECT_LT(n1, n0);
}
TEST(PdschSweepDormant, FailOpenCountsTrialsNotTime) { /* Review Focus 1 */
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  EXPECT_FALSE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05)); /* no trials yet */
  const int na = nr_pdsch_config_sweep_n_active(s.get());
  const int need = (int)ceil(na * log(1e3) / 0.05);
  for (int t = 0; t < need - 1; t++) { nr_pdsch_cfg_hypothesis_t h; nr_pdsch_config_sweep_feed(s.get(), nr_pdsch_config_sweep_next(s.get(), &h), false); }
  EXPECT_FALSE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05));
  { nr_pdsch_cfg_hypothesis_t h; nr_pdsch_config_sweep_feed(s.get(), nr_pdsch_config_sweep_next(s.get(), &h), false); }
  EXPECT_TRUE(nr_pdsch_config_sweep_fail_open_due(s.get(), 1e-3, 0.05));
  nr_pdsch_config_sweep_set_fail_open(s.get(), true);
  EXPECT_EQ(nr_pdsch_config_sweep_n_active(s.get()), s->n_hyp);
}
TEST(PdschSweepDormant, DestructivePruneCompactsMasks) {
  auto s = std::make_unique<nr_pdsch_config_sweep_state_t>(); nr_pdsch_config_sweep_init(s.get(), 4);
  nr_pdsch_config_sweep_set_dormant(s.get(), NR_TD_DORMANT_PRIOR, keep_even, nullptr);
  nr_pdsch_config_sweep_prune_keep(s.get(), [](const nr_pdsch_cfg_hypothesis_t *h, const void *) { return h->k0 == 0; }, nullptr);
  for (int i = 0; i < s->n_hyp; i++)
    ASSERT_EQ(nr_pdsch_config_sweep_is_active(s.get(), i), s->hyp[i].tda_length % 2 == 0);
}
```
(Fail-open test note: if the engine decides a winner before `need` trials because all active fail, assert on `since_pass` directly; the implementer documents which.)
- [ ] **Step 2: Run → FAIL.** **Step 3: Implement** (fields; `prune_commit` compacts `dormant` with the same keep-index mapping and is reused by `prune_keep`; `nr_pdsch_config_sweep_rebuild` preserves `dormant`/`fail_open`; skip logic in `next`, `next_k`, the exploit pick, `feed*`, `sweep_decide`). Measure `sizeof(nr_pdsch_config_sweep_state_t)` before/after and report.
- [ ] **Step 4: Run → PASS**; existing `PdschSweepK.K1IsBitIdentical*` tests still green; full ctest.
- [ ] **Step 5: Commit** — `feat(td): dormant (reversible) hypothesis masks per cause + fail-open counter in the sweep engine`.

---

### Task BC4: Field-book state machine (Sonnet)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_td_fieldbook.h`, `nr_td_fieldbook.c`; Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_fieldbook_test.cc`
- Modify callers: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc` (new `converged` argument: pass `0`)

**Interfaces:**
- Produces:
```c
typedef enum { NR_TD_FS_UNSEEN = 0, NR_TD_FS_CANDIDATE, NR_TD_FS_PROMOTED, NR_TD_FS_SUSPECT } nr_td_field_state_t;
/* entry gains: nr_td_field_state_t state; int32_t hint_value; (WITHDRAWN = transition back to CANDIDATE, counted) */
/* fieldbook gains: uint32_t generation; (bumped on promote, suspect, reconfirm, withdraw, epoch bump) uint32_t n_withdrawn; */
void nr_td_fieldbook_converged(nr_td_fieldbook_t *fb, uint16_t rnti, const nr_pdsch_cfg_hypothesis_t *h, uint64_t slot,
                               uint32_t pruned_fields /* bit f set: field f was pruned in this context (not independent) */);
nr_td_field_state_t nr_td_fieldbook_state(const nr_td_fieldbook_t *fb, nr_td_field_t f);
bool nr_td_fieldbook_prunes(const nr_td_fieldbook_t *fb, nr_td_field_t f, int32_t *value); /* state == PROMOTED */
bool nr_td_fieldbook_hyp_matches(nr_td_field_t f, int32_t value, const nr_pdsch_cfg_hypothesis_t *h);
uint32_t nr_td_fieldbook_generation(const nr_td_fieldbook_t *fb);
void nr_td_fieldbook_force_promote(nr_td_fieldbook_t *fb, nr_td_field_t f, int32_t value); /* simulator/test hook: PROMOTED, no supporters */
```
- Transitions (per field, current epoch only):
  - `converged` with field f independent (bit f clear): add the RNTI to the value's support. If state ∈ {UNSEEN, CANDIDATE} and the value has ≥ `promote_rntis` distinct supporters ⇒ PROMOTED (value set). If PROMOTED/SUSPECT and the value equals the promoted value from an RNTI not already supporting ⇒ (SUSPECT ⇒ PROMOTED, contradictions cleared). If PROMOTED/SUSPECT and the value differs ⇒ contradiction from this RNTI.
  - contradiction (from `converged` or `nr_td_fieldbook_contradict`): distinct RNTIs; 1 ⇒ SUSPECT; ≥ `withdraw_rntis` ⇒ withdrawn: value = −1, state CANDIDATE, `n_withdrawn++`, contradictions cleared, candidate rows kept (the contradicting value may promote immediately if it has enough support).
  - Caller contract on SUSPECT (operator 2026-10-01; implemented by callers via `generation`): new contexts do not prune on the field; existing **unsettled** contexts immediately `clear_dormant(FIELD_BASE + f)` (only that field's cause); converged contexts keep their winner and set an `untrusted_fields` bit. The field book exposes `nr_td_fieldbook_prunes()` = PROMOTED only, so a caller that re-evaluates `prunes()` on every generation change gets this behaviour.
  - `bump_epoch`: every PROMOTED/SUSPECT field ⇒ CANDIDATE with `hint_value = value`, value = −1; all support and contradiction sets cleared. `fill_side_info` uses `value` when PROMOTED, else `hint_value` (ordering only).
  - Bit f set in `pruned_fields` ⇒ the converge neither supports nor contradicts field f.

- [ ] **Step 1: Failing tests** (append; existing Task 3 tests must keep passing — update their `converged` calls with `, 0`):
```cpp
static nr_pdsch_cfg_hypothesis_t H(int S, int L) { nr_pdsch_cfg_hypothesis_t h = {}; h.tda_start = S; h.tda_length = L; h.dmrs_add_pos = 1; h.dmrs_max_len = 1; return h; }
TEST(FieldBookSM, PromoteSuspectWithdraw) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2);
  auto a = H(2, 12), b = H(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_SUSPECT);
  int32_t v; EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v));
  nr_td_fieldbook_converged(&fb, 4, &b, 0, 0);
  EXPECT_EQ(fb.n_withdrawn, 1u);
  EXPECT_TRUE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v)); /* b had 2 supporters: promoted at once */
  EXPECT_EQ(v, nr_td_pack_tdra(1, 13, 0, 0));
}
TEST(FieldBookSM, SuspectReconfirmed) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = H(2, 12), b = H(1, 13);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  nr_td_fieldbook_converged(&fb, 3, &b, 0, 0); nr_td_fieldbook_converged(&fb, 5, &a, 0, 0);
  EXPECT_EQ(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
}
TEST(FieldBookSM, PrunedContextIsNotIndependent) {
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = H(2, 12);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 1u << NR_TD_F_TDRA); nr_td_fieldbook_converged(&fb, 2, &a, 0, 1u << NR_TD_F_TDRA);
  EXPECT_NE(nr_td_fieldbook_state(&fb, NR_TD_F_TDRA), NR_TD_FS_PROMOTED);
}
TEST(FieldBookSM, EpochBumpStopsPruningKeepsHint) { /* Review Focus 4 */
  nr_td_fieldbook_t fb; nr_td_fieldbook_init(&fb, 2, 2); auto a = H(2, 12);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0); nr_td_fieldbook_converged(&fb, 2, &a, 0, 0);
  const uint32_t g = nr_td_fieldbook_generation(&fb);
  nr_td_fieldbook_bump_epoch(&fb);
  int32_t v; for (int f = 0; f < NR_TD_F_COUNT; f++) EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, (nr_td_field_t)f, &v));
  EXPECT_GT(nr_td_fieldbook_generation(&fb), g);
  nr_td_side_info_t si; memset(&si, 0, sizeof(si)); si.f_S = si.f_L = -1;
  nr_td_fieldbook_fill_side_info(&fb, &si); EXPECT_EQ(si.f_S, 2); EXPECT_EQ(si.f_L, 12);
  nr_td_fieldbook_converged(&fb, 1, &a, 0, 0);
  EXPECT_FALSE(nr_td_fieldbook_prunes(&fb, NR_TD_F_TDRA, &v)); /* old-epoch support does not count */
}
TEST(FieldBookSM, HypMatches) {
  auto a = H(2, 12);
  EXPECT_TRUE(nr_td_fieldbook_hyp_matches(NR_TD_F_TDRA, nr_td_pack_tdra(2, 12, 0, 0), &a));
  EXPECT_FALSE(nr_td_fieldbook_hyp_matches(NR_TD_F_DMRS_ADD_POS, 2, &a));
}
```
- [ ] **Step 2: Run → FAIL. Step 3: Implement** (state field, transitions, generation, hint, force_promote, `hyp_matches`; reuse the existing per-value candidate table). **Step 4: Run → PASS**; full ctest.
- [ ] **Step 5: Commit** — `feat(td): field-book state machine (SUSPECT/withdraw, independence, epoch hints)`.

---

### Task BC5: Reversible-pruning field book in the simulator + stale-field injection (Sonnet)

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_td_sim.cc`, `nr_td_sim_test.cc`; `tests/passive_rx/td_sim/campaign.py`, `test_campaign.py`

**Interfaces:**
- Consumes: BC3 API (`set_dormant`, `clear_dormant`, `set_fail_open`, `fail_open_due`, `n_active`, `prune_keep`), BC4 API (`prunes`, `hyp_matches`, `state`, `generation`, `force_promote`, `converged(..., pruned_fields)`).
- Produces CLI: `--fieldbook 0|1|2` (0 = today: prior pruning; 1 = ordering-only field book, prior off — unchanged legacy arm; 2 = reversible pruning: prior as dormant cause `NR_TD_DORMANT_PRIOR` + each PROMOTED field as `NR_TD_DORMANT_FIELD_BASE + f`), `--inject-wrong-field F` (−1 default; 0 TDRA, 1 add_pos, 2 max_len), `--fo-alpha` (1e-3), `--fo-pmin` (0.05). JSON per RNTI adds `"active_start"`, `"fail_open"`, `"pruned_fields"`; summary adds `"fail_opens"`, `"active_start_mean"`, `"recovery_grants"` (per acquisition with injection: grants from the acquisition start until the injected field leaves PROMOTED/SUSPECT; `-1` if never), `"withdrawals"`.
- Replace the local `sim_prune` by `nr_pdsch_config_sweep_prune_keep` (removes the copied prune semantics).

Behaviour with `--fieldbook 2`, per RNTI:
1. Start: copy template; if the cell prior is valid, `set_dormant(PRIOR, keep = prior predicate)`; for each field with `prunes(f, &v)`: `set_dormant(FIELD_BASE + f, keep = hyp_matches(f, v, h))`, record bit f in `pruned_fields` when it returned ≥ 0.
2. Each grant after feedback: if `fail_open_due(st, fo_alpha, fo_pmin)` ⇒ `set_fail_open(st, true)`, `pruned_fields = 0`, `rec.fail_open = true`.
3. On convergence: `converged(fb, rnti, winner, g, pruned_fields)`; prior bookkeeping as today.
4. After each converge, if `generation` changed: later RNTIs rebuild their masks at their start (step 1). The sim runs RNTIs one after another, so the "existing unsettled contexts restore on SUSPECT" rule (BC4 caller contract) is not exercised here; it is a levers-plan R2 runtime test (`SuspectRestoresUnsettledContexts`). Count converged RNTIs whose winner relied on a field that later became SUSPECT/WITHDRAWN (`"untrusted_after"`).
5. `--inject-wrong-field F`: at acquisition start `force_promote(fb, F, wrong)` where wrong = TDRA of the first template entry with a different `(S, L)` and the same mapping; add_pos `= (truth + 1) % 4` if present in the template else skip; max_len `= 3 − truth` (1 ↔ 2).

- [ ] **Step 1: Failing tests**
```cpp
TEST(TdSim, ReversibleFieldBookNeverWrong) {
  SimCfg c = SimCfg::defaults(); c.acq = 50; c.seed = 4; c.oracle = 0; c.fieldbook = 2;
  const SimResult r = run_sim(c); EXPECT_EQ(r.wrong, 0); EXPECT_EQ(r.undecidable, 0);
}
TEST(TdSim, WrongPromotionRecovers) {
  SimCfg c = SimCfg::defaults(); c.acq = 20; c.seed = 6; c.oracle = 0; c.fieldbook = 2; c.inject_wrong_field = 0;
  const SimResult r = run_sim(c);
  EXPECT_EQ(r.wrong, 0); EXPECT_EQ(r.undecidable, 0);
  EXPECT_GT(r.fail_opens, 0); EXPECT_GT(r.withdrawals, 0);
}
TEST(TdSim, FieldBookTwoNotSlowerThanPriorSteady) {
  SimCfg p = SimCfg::defaults(); p.acq = 30; p.seed = 8; p.oracle = 0;
  SimCfg f = p; f.fieldbook = 2;
  EXPECT_LE(run_sim(f).mean_s_steady, 1.10 * run_sim(p).mean_s_steady);
}
```
(`mean_s_steady` = mean seconds of RNTIs `k >= 2`, add to SimResult if not present.)
- [ ] **Step 2: Run → FAIL. Step 3: Implement. Step 4: Run → PASS**; campaign columns for the new summary keys + fake fixture; full ctest.
- [ ] **Step 5: Commit** — `test(td): reversible-pruning field book with fail-open and stale-field injection in nr_td_sim`.

---

## PHASE K0 (operator 2026-10-02): K39 fix, simulator v2, certified k0 evidence — runs BEFORE BC6

Order (operator): BC7 (K39) → BC8 (simulator v2) → BC9 (DCI-adjacency certified evidence) → BC10 (fast-path stream) →
BC11 (adaptive k0-neighbour test) → BC6. Fix B (k0-sibling zero-pass guard, BC2b round 1) is a **safety fallback**, not
the speed path. Design principle: **only certified, discriminative grants may provide k0 evidence; DM-RS present ⇒ slot
plausible, not k0 proven.** Background: `docs/superpowers/specs/2026-10-01-technique-d-k0-speed-recovery-notes.md`
(§0 findings, §1 DCI adjacency, §2 sibling test, §5 fast-path schedule, §6 simulator modelling, §7.2 assumptions).

### Task BC7 ★: K39 — DM-RS presence never pins k0 (Sonnet; Opus review)

**Root cause (code-read):** both DM-RS oracle call sites run only on jobs whose hypothesised k0 is 0 and record
`k0 = 0` as *observed* (`nr_pdsch_passive_queue.c` in-line path ~577 `observe(ticket, mask, last_sym, 0)` and deferred
path ~805 `observe(..., job.sweep_ticket.k0)` under `job.sweep_ticket.k0 == 0`); `obs_admits` (`nr_pdsch_config_sweep.c`
~481-492) then hard-prunes every other k0. Under traffic in adjacent slots the DCI's own slot carries the previous
grant's DM-RS, so a true k0 = 1 is pruned.

**Files:** `nr_pdsch_config_sweep.{h,c}` (obs set: k0 becomes a plausibility mask), `nr_pdsch_passive_queue.c` (both
call sites), tests `nr_pdsch_config_sweep_test.cc`; simulator: the oracle model's k0 pruning follows the new rule.

**Interfaces:**
```c
/* DM-RS mask / last symbol observed in the slot the job decoded: prunes on mask and last symbol only; k0 is NOT pinned.
 * k0_plausible: the hypothesised k0 is recorded as plausible (bit set in a uint32_t plausible mask, ordering/logging only). */
int nr_pdsch_config_sweep_observe(const nr_pdsch_sweep_ticket_t *t, uint16_t dmrs_mask, int last_symbol, int k0_plausible);
/* k0 certified by deterministic evidence (BC9 DCI adjacency / TDD direction): the only call that may prune k0. */
int nr_pdsch_config_sweep_certify_k0(const nr_pdsch_sweep_ticket_t *t, uint32_t k0_allowed_mask);
```
`obs_admits` ignores k0 unless a certified mask exists for that observation set. Default behaviour changes (this is a
correctness fix): flag `ISAC_TD_K0_ORACLE_LEGACY=1` restores the old pinning for A/B only.

- [ ] **Step 1: Failing tests** — `DmrsObservationDoesNotPruneOtherK0` (observe mask with k0_plausible=0 on a catalogue
  with k0 ∈ {0,1}: every k0=1 entry with the same mask/last symbol stays), `CertifiedK0Prunes`, `LegacyFlagRestoresPinning`,
  and a simulator test `AdjacentTrafficTrueK0OneSurvivesOracle` (oracle 1, truth k0 = 1, adjacent traffic on: never
  undecidable from a k0 prune).
- [ ] **Step 2–4:** implement; focused + full ctest; rfsim regression gate (106 PRB) PASS; report ttc before/after (more
  k0 hypotheses survive ⇒ convergence may be slower: measure, label `[MEASURED, DGX rfsim 106 PRB]`); rank-4 pin49r4 bed
  still 100 %. PROJECT_MEMORY K39 → resolved with evidence.
- [ ] **Step 5: Commit** — `fix(rx): DM-RS presence marks k0 plausible, never pins it (K39)`.

### Task BC8: Simulator v2 — slot-indexed traffic, adjacency and DCI observation (Sonnet)

Per notes §6. **Files:** `tests/nr_td_sim.cc`, `nr_td_sim_test.cc`, `tests/passive_rx/td_sim/campaign.py` (+ test).

Model (new flags; all default to the v1 behaviour so v1 output stays byte-identical):
- `--slot-model 1`: grants are generated on a slot timeline per RNTI (`--grant-prob` per DL slot, `--persist rho`:
  probability that the next grant repeats the previous allocation/MCS/TBS; link adaptation changes MCS with
  probability 1−rho). Each grant knows its DCI slot, its PDSCH slot (truth k0) and the neighbouring slots' grants.
- **Physical shifted-slot trap:** hypothesis with k0' ≠ k0 decodes the PDSCH in slot `dci_slot + k0'`; it passes iff that
  slot carries a grant for the same RNTI whose (TBS, PRBs, MCS, symbols, DM-RS, rv-compatible) equal the current DCI's
  and that transmission would pass at its SNR. No random trap probability in v2.
- **DCI observation:** each DCI is observed with miss probability `--dci-miss` and a false-accept probability
  `--dci-false` (a spurious DCI with random fields); the receiver-side DCI history the fast path may use contains only
  observed DCIs.
- **TDD pattern:** `--tdd "DDDSU"`-style string; slots of the wrong direction carry no DL PDSCH.
- **Certified flag:** each grant computes, from the observed DCI history only, whether it is k0-unambiguous for each
  k0 sibling (notes §1.2 per-world occupant check; unseen neighbour ⇒ ambiguous; compatible neighbour ⇒ no information).
- Counters: `false_passes`, `retx_trap_passes`, `k0_trap_passes` (physical), `certified_grants`, `dci_missed`,
  `dci_false`.
- BC2b review carry-forwards: k0+1 and k0−1 sides decided separately (each neighbour slot has its own grant or none);
  bursty persistence (allocation runs, not i.i.d. per grant); MCS-change probability per slot; TDD UL slots carry no
  PDSCH; arms with adjacency fraction ∈ {0, 0.5, 1}; a wrong-field injection that differs **only in k0**
  (`--inject-wrong-field 3` = TDRA with same S/L/mapping, other k0) so the "dormant true sibling" hole is exercised;
  fail-open available outside fieldbook 2 when P pins are active (so a wrong pin can recover).

- [ ] **Step 1: Failing tests** — `SlotModelOffIsByteIdentical` (cmp vs HEAD binary), `PersistentAllocationProducesK0Trap`,
  `TddWrongDirectionNeverCarriesPdsch`, `UnseenNeighbourIsNeverCertified`, `CompatibleNeighbourGivesNoCertification`.
- [ ] **Step 2–4:** implement; focused ctest; baselines (`--acq 500`, 4/1 RX, oracle 0 and 1, rho ∈ {0.5, 0.9},
  dci-miss ∈ {0.01, 0.1}) appended to `baseline_bc0_2026-10-01.txt` under `## BC8 v2`. **Step 5: Commit.**

### Task BC9 ★: DCI-adjacency certified k0 evidence (Opus; Sonnet by override if needed)

**Files:** PDCCH blind monitor (new DL DCI history ring per RNTI, written at DCI accept time before any grant drop —
notes §1.1; file `nr_pdcch_blind_monitor_rt.c` + a small pure module `nr_dci_history.{c,h}` with tests), engine
(`feed_attr` gains `bool certified`; levers P/C count only certified passes; `certify_k0` from BC7 for deterministic
exclusions), simulator (uses BC8's certified flag).

Rules (operator): observed incompatible DCI in the neighbour slot ⇒ certified evidence against that neighbour; neighbour
DCI unseen ⇒ ambiguous; compatible neighbour allocation ⇒ no k0 information; wrong TDD direction ⇒ deterministic
exclusion (`certify_k0`). Soundness conditions to state in the header (notes §1.5): one PDSCH per RNTI per slot, PDCCH
false-accept rate bound, truth in the catalogue.

- BC2b review carry-forward: the k0-sibling guard may ignore a dormant sibling only when its dormancy cause certifies k0
  (GEOM after a guarded pin, DCI-adjacency / TDD `certify_k0`); siblings dormant through PRIOR/FIELD must be tested or
  block. Sibling liveness: a sibling that cannot be decoded (TDD UL slot, slot not captured) must not stall the RNTI —
  skip with a bounded duty cycle (also BC10/R2).
- [ ] Tests: `DciHistoryRingOrderAndEviction`, `IncompatibleObservedNeighbourCertifies`, `MissedNeighbourDciIsAmbiguous`,
  `CompatibleNeighbourNotCertified`, `TddWrongDirectionExcludesK0`, engine `UncertifiedPassDoesNotCountForPC`, sim
  `CertifiedPCNeverWrongUnderPhysicalTrap` (BC8 model, rho 0.9, dci-miss 0.1, dci-false 1e-3: wrong = 0, wrong pins = 0).
- [ ] rfsim regression gate PASS with the history ring on; commit.

### Task BC10: Dedicated fast-path stream (Sonnet; Opus review)

Per notes §5 / §2.3: a separate per-context fast-path schedule (fixed duty or K hypotheses per grant) whose selection is
outcome-independent, so P/C evidence no longer depends on the KL round-robin share (fix A's ≈ 8.8 s cost). Optional
anytime e-process replacing `m*(T_max)` only if the BC2b-style stress check agrees with its bound. Tests: stream
selection independent of outcomes (`FastPathScheduleIgnoresOutcomes`), bound check at p_f 1e-3, KL unchanged
(bit-identity with the stream off). Commit.

### Task BC11: Adaptive k0-neighbour test (Sonnet; Opus review)

Per notes §2.1–2.2 / §3a: anytime Ville/SPRT test per neighbour with the 1e-6 budget split; p_min from the leader's
confirmation-phase lower bound (assumption: shift-stationarity of the truth's pass probability — stress-tested in the
simulator with non-stationary SNR); neighbours bundled on the same grant; CB0 / abort-after-first-failed-CB decode only
where the same-decoder rule holds and Nl = 1 (K38). Only certified grants give discrimination credit. Tests include
`NonStationarySnrNeverWrong`. Commit.

### Task BC6: Blind-convergence gate campaign and decisions (Opus decides; 🔁 Haiku runs)

**Files:**
- Create: `tests/passive_rx/td_sim/gate_bc.json`, `tests/passive_rx/td_sim/results_<date>_bc/summary.md` (+ `analysis.md`; jsonl only if < 5 MB)
- Modify: `docs/superpowers/specs/2026-10-01-technique-d-blind-convergence-design.md` (§8 decision block), `docs/superpowers/plans/2026-10-01-technique-d-convergence-levers.md` (R2 env line)

- [ ] **Step 0 (operator 2026-10-02):** use the BC8 v2 model by default (slot model, physical k0 trap, DCI miss/false, TDD); the any-grant v1 trap and the retransmission-only trap only as comparison arms; report ordinary CRC false passes, HARQ/retransmission ambiguity and new-TB consecutive-slot k0 traps separately. Required comparison (operator): baseline, P/C pre-fix, A, A+B, A+B+adaptive (BC11), A+B+best safe acceleration (BC9/BC10), at 4 RX and 1 RX; measure first/later-RNTI time, wrong winners, wrong pins, sibling trials, full TB decodes, CB0 probes, CPU/GPU cost, fail-open/recovery. Hard rule: 0 wrong winners and 0 wrong pins.
- [ ] **Step 1:** `gate_bc.json` arms: `prior` (today, fieldbook 0); `F_order` (ordering-only field book, fieldbook 1); `E` (equiv 1); `E+F` (equiv 1, fieldbook 2, reversible pruning); `E+F+inject` (… , inject-wrong-field 0, and separately 1); `E+F+C` (crc-accept 1, experimental); `F+P` (fieldbook 2, geom-pin 1); `F+P+C` (fieldbook 2, geom-pin 1, crc-accept 1); `P_stress` (oracle 0, geom-pin 1, `--crc-false 1e-3`); `C_stress` (oracle 0, equiv 1, crc-accept 1, `--crc-false 1e-3`: measured wrong rate vs analytical bound). Dimensions: oracle settings {`--oracle 1`, `--oracle 0`, `--oracle 1 --oracle-miss 0.3`, `--oracle 1 --oracle-wrong 0.05`} × rx {4, 1} × cell {SA sib1 1, NSA-like sib1 0}; fixed: twins 2, gate 1, K 1, `--harq-trap 0.01 --crc-false 5.96e-8`, seed 1, 2000 acquisitions × 4 RNTIs (oracle-0 cells may drop to 500 acquisitions if the pilot predicts > 4 h; say so).
- [ ] **Step 2:** Pilot `--acq 50` per arm → wall-time estimate; then run (≤ 8 parallel).
- [ ] **Step 3: Decide** per spec §8 pass criteria (**0 wrong winners is the hard rule**; state the Monte-Carlo resolution ≈ 3/N; lever C runtime enablement is only *recommended* here — the operator decides) (wrong = 0 everywhere; undecidable ≤ baseline; oracle-1 time ≤ baseline + seed noise; recovery from injected wrong promotion within 2 RNTIs; blind cold median ≤ 30 s at 4 RX / ≤ 90 s at 1 RX, else report the gap). Write the decision block into the addendum §8 and the chosen runtime flags into levers-plan R2 env line (`ISAC_TD_EQUIV`, `ISAC_TD_FIELDBOOK` 0/2, `ISAC_TD_CRC_ACCEPT` only if approved and passed), label `[SIMULATED, DGX host, nr_td_sim @<commit>]`.
- [ ] **Step 4: Commit** — `evidence(td): blind-convergence gate (equivalence, reversible field book, CRC accept)`.

---

## Follow-ups (not tasks here)

- **Soft oracles (lever S):** own spec + plan after BC0/BC6 quantify missing/wrong oracle impact (addendum §5).
- **GPU full-TB decodes per grant (lever G):** recorded in levers-plan G4.
- **Runtime wiring** of E, F (and C if approved): levers-plan R2 (queue computes equivalence keys with `nr_get_Qm_dl`/`nr_get_code_rate_dl`; contexts get dormant masks from the module-level field book under `g_lock`; fail-open per context).

## Self-review record (2026-10-01)

- Spec coverage: addendum §2 → BC1; §3 → BC2 (blocked on approval); §4 → BC3 (engine), BC4 (state machine), BC5 (simulator); §5 → follow-up; §6 → levers G4 note; §7 → BC0 (+ BC5 metrics); §8 → BC6; §9 → controller doc edits committed with this plan.
- Placeholders: none; the fail-open test's early-decision caveat is an explicit implementer instruction.
- Type consistency: `nr_td_keep_fn_t`, `NR_TD_DORMANT_*`, `feed_equiv`, `crc_accept_m`, `converged(..., pruned_fields)` used identically in BC1–BC5.
- Review Focus: five items mapped to BC3 (3), BC4 (1), BC2 (1).
