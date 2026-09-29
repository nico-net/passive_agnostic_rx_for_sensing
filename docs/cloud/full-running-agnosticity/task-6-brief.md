### Task 6: Modulation-order oracle for the Technique D MCS table

With the DM-RS oracle, Technique D is left with the MCS table (3 hypotheses) decided only by TB-CRC trials (≥64 on the leader plus separation, or 300 each). On a cell where few TBs decode that takes a long time. The equalised constellation tells the modulation order directly, and for most MCS indices the three tables predict different orders (e.g. MCS 20: 64QAM in table 0, 256QAM in table 1). Raw EVM cannot be used — a denser grid always fits noise better — so each grid's residual is normalised by its uniform-quantisation floor (1/3 per dimension): an on-grid signal reads ≪ 1, off-grid or noise ≈ 1 or more. The oracle abstains unless one grid is clearly best, and Technique D prunes only after two agreeing observations.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_qm_oracle_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.{h,c}`, `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.{h,c}`, `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `nr_pdsch_sweep_ticket_t`, `ticket_context()`, `g_lock`, `sweep_context_t` (existing, `nr_pdsch_config_sweep.c`).
- Produces:
  ```c
  int nr_pdsch_qm_of_mcs(uint8_t mcs, uint8_t mcs_table);                 /* 2/4/6/8, 0 if invalid */
  int nr_pdsch_qm_classify(const int16_t *iq, uint32_t n, double *t_best); /* 2/4/6/8, 0 = abstain */
  int nr_pdsch_config_sweep_prune_qm(nr_pdsch_config_sweep_state_t *st, uint8_t mcs, int qm);
  int nr_pdsch_config_sweep_observe_qm(const nr_pdsch_sweep_ticket_t *ticket, uint8_t mcs, int qm);
  /* nr_pdsch_passive_decode_result_t gains: uint8_t qm_measured; */
  ```

- [ ] **Step 1: Write the failing oracle tests**

Create `tests/nr_pdsch_qm_oracle_test.cc`:

```cpp
#include <cmath>
#include <random>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_qm_oracle.h"
}

namespace {
// Square QAM of order qm, unit average power, AWGN at snr_db, scaled into int16 by an arbitrary gain
// (the receiver's fixed-point scale is unknown; the oracle must not depend on it).
std::vector<int16_t> Qam(int qm, double snr_db, int n, unsigned seed)
{
  std::mt19937 g(seed);
  const int lmax = (1 << (qm / 2)) - 1;
  double ms = 0.0; int nl = 0;
  for (int l = 1; l <= lmax; l += 2) { ms += l * l; nl++; }
  const double norm = std::sqrt(2.0 * ms / nl), sigma = std::sqrt(0.5 / std::pow(10.0, snr_db / 10.0));
  std::uniform_int_distribution<int> lev(0, (lmax + 1) / 2 - 1);  // positive odd levels 1..lmax
  std::uniform_int_distribution<int> sgn(0, 1);
  std::normal_distribution<double> nz(0.0, sigma);
  std::vector<int16_t> iq(2 * n);
  for (int i = 0; i < 2 * n; i++) {
    const double a = (2 * lev(g) + 1) * (sgn(g) ? 1.0 : -1.0) / norm;
    iq[i] = (int16_t)std::lround((a + nz(g)) * 2500.0);
  }
  return iq;
}
}  // namespace

TEST(QmOracle, McsTablesMatchTs38214) {
  // Table 5.1.3.1-1 (mcs_table 0, 64QAM), -2 (1, 256QAM), -3 (2, 64QAM LowSE); 28..31 reserved.
  const int t0[32] = {2,2,2,2,2,2,2,2,2,2,4,4,4,4,4,4,4,6,6,6,6,6,6,6,6,6,6,6,6,2,4,6};
  const int t1[32] = {2,2,2,2,2,4,4,4,4,4,4,6,6,6,6,6,6,6,6,6,8,8,8,8,8,8,8,8,2,4,6,8};
  const int t2[32] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,4,4,4,4,4,4,6,6,6,6,6,6,6,6,2,4,6};
  for (int m = 0; m < 32; m++) {
    EXPECT_EQ(nr_pdsch_qm_of_mcs(m, 0), t0[m]) << m;
    EXPECT_EQ(nr_pdsch_qm_of_mcs(m, 1), t1[m]) << m;
    EXPECT_EQ(nr_pdsch_qm_of_mcs(m, 2), t2[m]) << m;
  }
  EXPECT_EQ(nr_pdsch_qm_of_mcs(32, 0), 0);
  EXPECT_EQ(nr_pdsch_qm_of_mcs(0, 3), 0);
}

TEST(QmOracle, ClassifiesEveryOrderAtHighSnr) {
  for (int qm : {2, 4, 6, 8}) {
    auto iq = Qam(qm, 32.0, 2048, 100 + qm);
    double t = -1;
    EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 2048, &t), qm) << "qm=" << qm << " t=" << t;
  }
}

TEST(QmOracle, PureNoiseAbstains) {
  std::mt19937 g(7);
  std::normal_distribution<double> nz(0.0, 1500.0);
  std::vector<int16_t> iq(4096);
  for (auto &v : iq) v = (int16_t)std::lround(nz(g));
  EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 2048, nullptr), 0);
}

TEST(QmOracle, LowSnr256QamAbstains) {
  auto iq = Qam(8, 15.0, 2048, 9);
  EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 2048, nullptr), 0);
}

TEST(QmOracle, TooFewSymbolsAbstains) {
  auto iq = Qam(6, 32.0, 100, 3);
  EXPECT_EQ(nr_pdsch_qm_classify(iq.data(), 100, nullptr), 0);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

Register in `CMakeLists.txt` after the Task 4 block:

```cmake
  add_executable(test_nr_pdsch_qm_oracle ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_qm_oracle_test.cc
                                         ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.c)
  target_include_directories(test_nr_pdsch_qm_oracle PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdsch_qm_oracle PRIVATE GTest::gtest m)
  add_dependencies(tests test_nr_pdsch_qm_oracle)
  add_test(NAME test_nr_pdsch_qm_oracle COMMAND ./test_nr_pdsch_qm_oracle)
```

Append `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.c` to the `add_library(nr_pdcch_blind_monitor …)` list, and add the same file to the sources of `add_executable(test_nr_pdsch_config_sweep …)` (find it with `grep -n 'add_executable(test_nr_pdsch_config_sweep' CMakeLists.txt`), because that test compiles `nr_pdsch_config_sweep.c` directly.

- [ ] **Step 2: Run to verify it fails**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && cmake . >/dev/null && make test_nr_pdsch_qm_oracle 2>&1 | grep -m1 -E 'error|No such file'"`
Expected: `nr_pdsch_qm_oracle.h: No such file or directory`.

- [ ] **Step 3: Implement the oracle**

Create `nr_pdsch_qm_oracle.h`:

```c
#ifndef NR_PDSCH_QM_ORACLE_H
#define NR_PDSCH_QM_ORACLE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* MODULATION-ORDER ORACLE for Technique D's MCS-table hypothesis. The equalised PDSCH symbols
 * (rxdataF_comp) fit the ideal grid of their true order; for most MCS indices the three MCS tables
 * predict different orders, so the measured order prunes the table without TB-CRC trials.
 *
 * Raw EVM cannot compare grids: a denser grid always fits NOISE better. Each grid's per-dimension
 * squared residual (after RMS normalisation, which cancels the fixed-point scale as EQDIAG does) is
 * divided by 1/3, the value for input spread uniformly over the grid spacing: t << 1 on-grid, t ~ 1
 * for noise, t > 1 for a coarser constellation sitting between the grid's points. */

/** Modulation order of MCS index `mcs` in mcs_table 0 (64QAM), 1 (256QAM), 2 (64QAM LowSE):
 *  TS 38.214 Tables 5.1.3.1-1/-2/-3 including the reserved indices. 0 if out of range. */
int nr_pdsch_qm_of_mcs(uint8_t mcs, uint8_t mcs_table);

/** Qm in {2,4,6,8} whose grid fits `n` interleaved int16 (I,Q) symbols best, or 0 (abstain) unless
 *  the best t < NR_QM_ORACLE_T_MAX and t_best < NR_QM_ORACLE_RATIO * t_second, or n < NR_QM_ORACLE_MIN_N.
 *  *t_best (optional) receives the best normalised residual. */
int nr_pdsch_qm_classify(const int16_t *iq, uint32_t n, double *t_best);

#define NR_QM_ORACLE_MIN_N 256
#define NR_QM_ORACLE_T_MAX 0.5
#define NR_QM_ORACLE_RATIO 0.5

#ifdef __cplusplus
}
#endif
#endif
```

Create `nr_pdsch_qm_oracle.c`:

```c
#include "nr_pdsch_qm_oracle.h"
#include <math.h>

int nr_pdsch_qm_of_mcs(uint8_t mcs, uint8_t mcs_table)
{
  if (mcs > 31 || mcs_table > 2)
    return 0;
  static const uint8_t first4[3] = {10, 5, 15}, first6[3] = {17, 11, 21}, first8[3] = {255, 20, 255};
  static const uint8_t reserved[3] = {29, 28, 29};
  if (mcs >= reserved[mcs_table])
    return 2 + 2 * (mcs - reserved[mcs_table]); /* reserved indices: Qm 2,4,6(,8) in order */
  if (mcs >= first8[mcs_table]) return 8;
  if (mcs >= first6[mcs_table]) return 6;
  if (mcs >= first4[mcs_table]) return 4;
  return 2;
}

/* Per-dimension squared residual to the nearest odd-integer level (clipped to +-lmax), after scaling
 * the stream to the grid's ideal power, divided by the uniform floor 1/3. */
static double t_on_grid(const int16_t *iq, uint32_t n, double p, int qm)
{
  const int lmax = (1 << (qm / 2)) - 1;
  double ms = 0.0;
  int nl = 0;
  for (int l = 1; l <= lmax; l += 2) { ms += (double)l * l; nl++; }
  const double scale = sqrt(2.0 * ms / nl / p);
  double err = 0.0;
  for (uint32_t i = 0; i < 2 * n; i++) {
    const double v = iq[i] * scale;
    double s = 2.0 * floor(v / 2.0) + 1.0;
    if (s > lmax) s = lmax;
    else if (s < -lmax) s = -lmax;
    err += (v - s) * (v - s);
  }
  return (err / (2.0 * n)) / (1.0 / 3.0);
}

int nr_pdsch_qm_classify(const int16_t *iq, uint32_t n, double *t_best)
{
  if (t_best) *t_best = -1.0;
  if (iq == NULL || n < NR_QM_ORACLE_MIN_N)
    return 0;
  double p = 0.0;
  for (uint32_t i = 0; i < 2 * n; i++)
    p += (double)iq[i] * iq[i];
  p /= (double)n;
  if (p <= 0.0)
    return 0;
  static const int qms[4] = {2, 4, 6, 8};
  double t[4];
  int b = 0, s = -1;
  for (int k = 0; k < 4; k++) {
    t[k] = t_on_grid(iq, n, p, qms[k]);
    if (t[k] < t[b]) b = k;
  }
  for (int k = 0; k < 4; k++)
    if (k != b && (s < 0 || t[k] < t[s])) s = k;
  if (t_best) *t_best = t[b];
  return (t[b] < NR_QM_ORACLE_T_MAX && t[b] < NR_QM_ORACLE_RATIO * t[s]) ? qms[b] : 0;
}
```

- [ ] **Step 4: Run the oracle tests**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_pdsch_qm_oracle 2>&1 | tail -1 && ./test_nr_pdsch_qm_oracle 2>&1 | grep -E 'PASSED|FAILED|Failure' "`
Expected: `[  PASSED  ] 5 tests`. If `ClassifiesEveryOrderAtHighSnr` or an abstain test fails, stop and report the printed `t` — do not retune the constants without a recorded measurement.

- [ ] **Step 5: Write the failing Technique D tests**

Add to `tests/nr_pdsch_config_sweep_test.cc` (after the per-RNTI section):

```cpp
TEST(PdschConfigSweepQm, PruneQmKeepsOnlyTablesPredictingTheMeasuredOrder) {
  nr_pdsch_config_sweep_state_t st;
  ASSERT_GT(nr_pdsch_config_sweep_init(&st, 2), 0);
  // MCS 20: table 0 -> 64QAM, table 1 -> 256QAM, table 2 -> 16QAM. Measured 256QAM => table 1 only.
  const int n = nr_pdsch_config_sweep_prune_qm(&st, 20, 8);
  ASSERT_GT(n, 0);
  for (int i = 0; i < st.n_hyp; i++) EXPECT_EQ(st.hyp[i].mcs_table, 1);
}

TEST(PdschConfigSweepQm, UninformativeMcsLeavesTheCatalogAlone) {
  nr_pdsch_config_sweep_state_t st;
  const int full = nr_pdsch_config_sweep_init(&st, 2);
  EXPECT_EQ(nr_pdsch_config_sweep_prune_qm(&st, 2, 2), full);  // MCS 2 is QPSK in every table
  EXPECT_EQ(st.n_hyp, full);
}

TEST(PdschConfigSweepQm, LiveObservationPrunesOnlyAfterTwoAgreeingSightings) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  auto t = select_context(31, 0x4601, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // first sighting: evidence only
  EXPECT_GT(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // second agreeing: pruned
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // nothing left to remove: no reset
}

TEST(PdschConfigSweepQm, ConflictingObservationsResetInsteadOfPruning) {
  nr_pdsch_config_sweep_reset_all();
  nr_pdsch_config_sweep_prior_reset();
  auto t = select_context(32, 0x4602, 0);
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 8), 0);   // implies table 1
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 6), 0);   // implies table 0: conflict -> reset
  EXPECT_EQ(nr_pdsch_config_sweep_observe_qm(&t, 20, 6), 0);   // first sighting after reset
}
```

- [ ] **Step 6: Run to verify they fail**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_pdsch_config_sweep 2>&1 | grep -m1 error"`
Expected: `'nr_pdsch_config_sweep_prune_qm' was not declared`.

- [ ] **Step 7: Implement in the sweep**

In `nr_pdsch_config_sweep.h`, after the `nr_pdsch_config_sweep_observe_mask` declaration:

```c
/** Qm oracle (nr_pdsch_qm_oracle.h): keep only hypotheses whose MCS table maps `mcs` to the measured
 *  order `qm`. Same contract as prune_mask: 0 = nothing matched (state untouched); unchanged = count. */
int nr_pdsch_config_sweep_prune_qm(nr_pdsch_config_sweep_state_t *st, uint8_t mcs, int qm);
/** Live context, two-observation rule: the tables consistent with each observation are intersected per
 *  context and applied once two agree; a conflict (empty intersection) resets the evidence. Returns the
 *  surviving count only when this call removed hypotheses, else 0. ISAC_QM_ORACLE=0 disables. */
int nr_pdsch_config_sweep_observe_qm(const nr_pdsch_sweep_ticket_t *ticket, uint8_t mcs, int qm);
```

In `nr_pdsch_config_sweep.c`:

1. Add `#include "nr_pdsch_qm_oracle.h"` and `#include <stdlib.h>` (for `getenv`; not currently included).
2. Add to `sweep_context_t` after `bool reported;`: `uint8_t qm_tables, qm_obs; /* Qm-oracle evidence: consistent-table bitmask, sightings */` (contexts are zeroed on creation, so `qm_obs == 0` means no evidence).
3. After `nr_pdsch_config_sweep_prune_mask()` add:

```c
static uint8_t qm_table_mask(uint8_t mcs, int qm)
{
  uint8_t mask = 0;
  for (uint8_t t = 0; t < 3; t++)
    if (nr_pdsch_qm_of_mcs(mcs, t) == qm)
      mask |= (uint8_t)(1u << t);
  return mask;
}

static int prune_tables(nr_pdsch_config_sweep_state_t *st, uint8_t mask)
{
  if (st == NULL || st->n_hyp <= 0 || mask == 0)
    return 0;
  nr_pdsch_cfg_hypothesis_t keep[NR_PDSCH_SWEEP_MAX_HYP];
  int n = 0;
  for (int i = 0; i < st->n_hyp; i++)
    if (mask & (1u << st->hyp[i].mcs_table))
      keep[n++] = st->hyp[i];
  if (n <= 0 || n == st->n_hyp)
    return n == st->n_hyp ? n : 0;
  memcpy(st->hyp, keep, (size_t)n * sizeof(keep[0]));
  st->n_hyp = n;
  memset(st->trials, 0, sizeof(st->trials));
  memset(st->ok, 0, sizeof(st->ok));
  for (int i = 0; i < n; i++)
    st->order[i] = i;
  st->cursor = 0;
  st->winner = -1;
  return n;
}

int nr_pdsch_config_sweep_prune_qm(nr_pdsch_config_sweep_state_t *st, uint8_t mcs, int qm)
{
  return prune_tables(st, qm_table_mask(mcs, qm));
}
```

4. After `nr_pdsch_config_sweep_observe()` add:

```c
int nr_pdsch_config_sweep_observe_qm(const nr_pdsch_sweep_ticket_t *ticket, uint8_t mcs, int qm)
{
  static int enabled = -1;
  if (enabled < 0) {
    const char *e = getenv("ISAC_QM_ORACLE");
    enabled = (e != NULL && atoi(e) == 0) ? 0 : 1;
  }
  if (!enabled || ticket == NULL || ticket->generation == 0 || qm <= 0)
    return 0;
  const uint8_t mask = qm_table_mask(mcs, qm);
  if (mask == 0 || mask == 0x7)
    return 0; /* impossible for this MCS, or every table agrees: no information */
  pthread_mutex_lock(&g_lock);
  sweep_context_t *c = ticket_context(ticket);
  int n = 0;
  if (c != NULL && c->state.winner < 0) {
    const uint8_t inter = c->qm_obs ? (uint8_t)(c->qm_tables & mask) : mask;
    if (inter == 0) {
      c->qm_obs = 0;
      c->qm_tables = 0;
    } else {
      c->qm_tables = inter;
      if (c->qm_obs < 255) c->qm_obs++;
      if (c->qm_obs >= 2) {
        const int before = c->state.n_hyp;
        n = prune_tables(&c->state, inter);
        if (n == before) n = 0;
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}
```


- [ ] **Step 8: Run the sweep tests**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_pdsch_config_sweep 2>&1 | tail -1 && ./test_nr_pdsch_config_sweep --gtest_filter='*Qm*:*RntiCache*' 2>&1 | grep -E 'PASSED|FAILED'"`
Expected: all `PdschConfigSweepQm.*` and the RNTI-cache test pass.

- [ ] **Step 9: Measure in the decoder and feed it to the sweep**

In `nr_pdsch_passive_decode.h`, add to `nr_pdsch_passive_decode_result_t` after `uint32_t nvar;`:

```c
  uint8_t              qm_measured; ///< modulation order from the equalised symbols (nr_pdsch_qm_oracle.h), 0 = abstained
```

In `nr_pdsch_passive_decode.c` (inside `nr_pdsch_passive_decode()`, where `out` is the result pointer — see `out->nvar = nvar;` ~line 2356):

1. Add `#include "nr_pdsch_qm_oracle.h"`.
2. Directly after `out->nvar = nvar;` add `out->qm_measured = 0;`.
3. Directly after `pdtim_add(PDTIM_DEMOD, pdt_dem);` add:

```c
  /* Qm oracle: same symbol choice as EQDIAG -- the one with the most valid data REs. */
  if (demod_ok) {
    int qm_m = -1;
    uint32_t qm_n = 0;
    for (int m = dlsch_config->start_symbol; m < dlsch_config->start_symbol + dlsch_config->number_symbols; m++)
      if (dl_valid_re[m] > qm_n) { qm_n = dl_valid_re[m]; qm_m = m; }
    if (qm_m >= 0)
      out->qm_measured = (uint8_t)nr_pdsch_qm_classify((const int16_t *)rxdataF_comp[qm_m][0],
                                                       qm_n > 4096 ? 4096 : qm_n, NULL);
  }
```

In `nr_pdsch_passive_queue.c`:

1. Add `#include "nr_pdsch_qm_oracle.h"` next to the `nr_pdsch_config_sweep.h` include.
2. Zero-initialise both results: `nr_pdsch_passive_decode_result_t dec;` → `nr_pdsch_passive_decode_result_t dec = {0};` (~line 683) and the same for `dec2` (~line 710).
3. Immediately before `nr_pdcch_dci11_layout_feedback(job.sweep_ticket.layout_index, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);` add:

```c
      if (!job.sweep_ticket.settled && job.sweep_ticket.generation && dec.qm_measured) {
        const int kept = nr_pdsch_config_sweep_observe_qm(&job.sweep_ticket, job.grant.mcs, dec.qm_measured);
        if (kept > 0)
          LOG_A(PHY, "SENSING: Technique D Qm oracle rnti=0x%x mcs=%u qm=%u -> %d hypotheses\n",
                job.sweep_ticket.rnti, job.grant.mcs, dec.qm_measured, kept);
      }
```

- [ ] **Step 10: Build the receiver and rerun all touched suites**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 nr-uesoftmodem test_nr_pdsch_config_sweep test_nr_pdsch_qm_oracle 2>&1 | grep -E ' error|Built target nr-uesoftmodem' && ctest -R 'test_nr_pdsch_config_sweep|test_nr_pdsch_qm_oracle' --output-on-failure | tail -4"`
Expected: `Built target nr-uesoftmodem`; both tests pass.

- [ ] **Step 11: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_qm_oracle_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.c openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c CMakeLists.txt && git commit -F -" <<'EOF'
Technique D: modulation-order oracle prunes the MCS table from the equalised constellation

The DM-RS oracle leaves only mcs_table to the TB CRC, which needs tens to hundreds of decodes. The
equalised symbols fit the grid of their true order; residuals are normalised by the uniform
quantisation floor so a denser grid cannot win on noise, and the oracle abstains unless one grid is
clearly best. Technique D prunes after two agreeing sightings and resets on conflict.
ISAC_QM_ORACLE=0 disables.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

