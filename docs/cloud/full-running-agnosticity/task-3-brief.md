### Task 3: Pin the CSI-RS search to the IDSWEEP candidate until it confirms

`nr_csirs_blind_next()` is pure round-robin over ~683 candidates. A candidate is only scored when its turn lands on a CSI-RS slot; for a period-P resource that is about once every n·P calls (≈14 s at n=683, P=40, fewer when the function is not called every slot). Confirmation needs 3 periodic hits, so after the scrambling ID is written back (Task 1) confirmation can take minutes — the lab run never confirmed within 200 s. Pinning serves the chosen candidate on every call, so 3 hits arrive in ~3 periods.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_search_test.cc`

**Interfaces:**
- Consumes: `nr_csirs_blind_init`, `nr_csirs_blind_next`, `nr_csirs_blind_feed`, `nr_csirs_blind_confirmed` (existing).
- Produces:
  - `void nr_csirs_blind_pin(nr_csirs_blind_state_t *st, int idx, uint32_t budget);`
  - `#define NR_CSIRS_BLIND_PIN_SWEEP_CALLS (32 * 640)` and `#define NR_CSIRS_BLIND_PIN_CONFIRM_CALLS (4 * 640)`
  - new state fields `int pinned; uint32_t pin_left;`

- [ ] **Step 1: Write the failing tests**

Add to `tests/nr_csirs_blind_search_test.cc` (add `#include <memory>` at the top if absent):

```cpp
// ---- pinning ------------------------------------------------------------------------------------

TEST(CsirsBlindPin, PinnedCandidateIsServedUntilBudgetThenRoundRobinResumes) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 273, 2), 10);
  nr_csirs_blind_pin(st.get(), 7, 3);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 7);
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 0);  // budget spent: round-robin resumes from its cursor
  EXPECT_EQ(nr_csirs_blind_next(st.get()), 1);
}

TEST(CsirsBlindPin, PinnedPeriodicResourceConfirmsWithinThreePeriods) {
  auto st = std::make_unique<nr_csirs_blind_state_t>();
  ASSERT_GT(nr_csirs_blind_init(st.get(), 273, 2), 10);
  const int truth = 7, period = 40, offset = 2;
  nr_csirs_blind_pin(st.get(), truth, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
  uint32_t s = 0;
  for (; s < 4 * 640; s++) {
    const int idx = nr_csirs_blind_next(st.get());
    const double z = (idx == truth && s % period == offset) ? 6.0 : 1.0;  // bar is 3 x 4/3 = 4
    if (nr_csirs_blind_feed(st.get(), idx, s, z, 4.0 / 3.0))
      break;
  }
  uint16_t p = 0, o = 0;
  ASSERT_NE(nr_csirs_blind_confirmed(st.get(), &p, &o), nullptr);
  EXPECT_EQ(st->confirmed, truth);
  EXPECT_EQ(p, period);
  EXPECT_EQ(o, offset);
  EXPECT_LE(s, (uint32_t)(2 * period + offset));  // hits at 2, 42, 82
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_csirs_blind_search 2>&1 | grep -m1 error"`
Expected: `'nr_csirs_blind_pin' was not declared`.

- [ ] **Step 3: Implement**

In `nr_csirs_blind_search.h`, add to `nr_csirs_blind_state_t` right after `int confirmed; ///< index of a resolved resource, or -1`:

```c
  int                  pinned;      ///< candidate served on every next() while pin_left > 0, or -1
  uint32_t             pin_left;    ///< remaining pinned next() calls
```

and next to the `nr_csirs_blind_next` declaration:

```c
/** Serve candidate idx on EVERY next() call for up to `budget` calls (or until confirmed), then resume
 *  round-robin. Round-robin scores a candidate only when its turn lands on a CSI-RS slot -- about once
 *  per n*period calls -- so a candidate the scramblingID sweep is working on, or has just solved,
 *  would otherwise wait minutes for the hits it needs. */
void nr_csirs_blind_pin(nr_csirs_blind_state_t *st, int idx, uint32_t budget);
#define NR_CSIRS_BLIND_PIN_SWEEP_CALLS   (32 * 640)  /* 1024 ids / 32 per aligned visit x longest period */
#define NR_CSIRS_BLIND_PIN_CONFIRM_CALLS (4 * 640)   /* > CSIRS_MIN_HITS periods at the longest period */
```

In `nr_csirs_blind_search.c`, in `nr_csirs_blind_init()` immediately after `st->confirmed = -1;` add:

```c
  st->pinned = -1;
  st->pin_left = 0;
```

Replace `nr_csirs_blind_next()` with:

```c
void nr_csirs_blind_pin(nr_csirs_blind_state_t *st, int idx, uint32_t budget)
{
  if (st == NULL || idx < 0 || idx >= st->n)
    return;
  st->pinned = idx;
  st->pin_left = budget;
}

int nr_csirs_blind_next(nr_csirs_blind_state_t *st)
{
  if (st == NULL || st->n <= 0) {
    return -1;
  }
  if (st->confirmed >= 0) {
    return st->confirmed;
  }
  if (st->pin_left > 0 && st->pinned >= 0 && st->pinned < st->n) {
    st->pin_left--;
    return st->pinned;
  }
  const int idx = st->cursor;
  st->cursor = (st->cursor + 1) % st->n;
  return idx;
}
```

In `nr_csirs_blind_rt.c`, replace

```c
    if (g_id_pin >= 0)
      LOG_A(PHY, "SENSING: CSIRS_BLIND IDSWEEP pinned to row%u fd%u l%u (mean epr=%.2f over %u) -- "
                 "sweeping 1024 scramblingIDs on THAT candidate only\n",
            g_st.cand[g_id_pin].row, g_st.cand[g_id_pin].freq_domain, g_st.cand[g_id_pin].symb_l0,
            best_m, g_epr_n[g_id_pin]);
```

with

```c
    if (g_id_pin >= 0) {
      LOG_A(PHY, "SENSING: CSIRS_BLIND IDSWEEP pinned to row%u fd%u l%u (mean epr=%.2f over %u) -- "
                 "sweeping 1024 scramblingIDs on THAT candidate only\n",
            g_st.cand[g_id_pin].row, g_st.cand[g_id_pin].freq_domain, g_st.cand[g_id_pin].symb_l0,
            best_m, g_epr_n[g_id_pin]);
      /* The sweep advances only on visits that land on a CSI-RS slot; visit it every call. */
      nr_csirs_blind_pin(&g_st, g_id_pin, NR_CSIRS_BLIND_PIN_SWEEP_CALLS);
    }
```

and immediately after the Task 1 line `g_st.cand[g_id_pin].scramb_id = trial.scramb_id;` add:

```c
          nr_csirs_blind_pin(&g_st, g_id_pin, NR_CSIRS_BLIND_PIN_CONFIRM_CALLS);
```

- [ ] **Step 4: Run the tests**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 test_nr_csirs_blind_search test_nr_csirs_blind_synth nr-uesoftmodem 2>&1 | tail -2 && ctest -R 'test_nr_csirs_blind' --output-on-failure | tail -4"`
Expected: both CSI-RS test binaries `Passed`.

- [ ] **Step 5: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_search_test.cc && git commit -F -" <<'EOF'
Blind CSI-RS: pin the search to the IDSWEEP candidate while it sweeps and until it confirms

Round-robin over ~683 candidates scores a candidate only when its turn lands on a CSI-RS slot
(about once per n*period calls), so after the scramblingID was solved the lab run never collected
the 3 periodic hits in 200 s. nr_csirs_blind_pin() serves one candidate every call for a bounded
budget, then round-robin resumes.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

