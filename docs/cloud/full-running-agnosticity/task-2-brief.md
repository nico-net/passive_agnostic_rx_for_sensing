### Task 2: Fix the CSI-RS row/port mismatch and the stale partial-band test

`1598309ac9` widened the blind CSI-RS enumeration to rows {1,2,3,4,5}, but `nr_csirs_blind_rt.c` still maps only row 4 to 4 ports and everything else to 1. Rows 3 (2 ports) and 5 (4 ports) are therefore generated with only port 0's buffer cleared, so stale REs from the previous candidate leak into the reference. The enumeration test still asserts rows {1,2,4}, and the unused `kPorts` table is the compiler warning that points at the gap. Separately, `WholeBandMeanDilutesAPartialBandResource` zero-fills 80 % of the band, but the block correlator skips zero-energy blocks by design (`if (e_rx > 0.0 && e_ref > 0.0)`), so zeros no longer model "unrelated REs"; on air those REs carry noise.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c`
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c`
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_search_test.cc`

**Interfaces:**
- Consumes: nothing.
- Produces: `int nr_csirs_blind_row_ports(uint8_t row);` — CSI-RS ports of an enumerated row (38.211 Table 7.4.1.5.3-1), 0 for rows this module does not enumerate.

- [ ] **Step 1: Write the failing tests**

Add to `tests/nr_csirs_blind_search_test.cc`, right after `TEST(CsirsBlindEnum, EnumeratesRealConfigurationsOnly)`:

```cpp
TEST(CsirsBlindEnum, RowPortsMatchTheSpecTable) {
  // TS 38.211 Table 7.4.1.5.3-1: rows 1,2 = 1 port; row 3 = 2 ports (fd-CDM2); rows 4,5 = 4 ports.
  EXPECT_EQ(nr_csirs_blind_row_ports(1), 1);
  EXPECT_EQ(nr_csirs_blind_row_ports(2), 1);
  EXPECT_EQ(nr_csirs_blind_row_ports(3), 2);
  EXPECT_EQ(nr_csirs_blind_row_ports(4), 4);
  EXPECT_EQ(nr_csirs_blind_row_ports(5), 4);
  EXPECT_EQ(nr_csirs_blind_row_ports(6), 0);  // not enumerated (needs multi-bit bitmaps)
}
```

In `EnumeratesRealConfigurationsOnly`, replace

```cpp
    EXPECT_TRUE(c[i].row == 1 || c[i].row == 2 || c[i].row == 4);
```

with

```cpp
    EXPECT_GT(nr_csirs_blind_row_ports(c[i].row), 0) << "row " << (int)c[i].row << " has no port count";
```

In `WholeBandMeanDilutesAPartialBandResource`, replace

```cpp
  for (int i = (int)(0.2 * n); i < n; i++) { rx[2 * i] = (int16_t)0; rx[2 * i + 1] = (int16_t)0; }
```

with

```cpp
  /* Unrelated REs carry energy on air (noise, other channels). Exact zeros no longer model that: the
   * block correlator skips zero-energy blocks by design, so a zero-filled band stopped diluting it. */
  std::mt19937 gz(4242);
  std::normal_distribution<double> ndz(0.0, 500.0);
  for (int i = (int)(0.2 * n); i < n; i++) { rx[2 * i] = (int16_t)ndz(gz); rx[2 * i + 1] = (int16_t)ndz(gz); }
```

- [ ] **Step 2: Run to verify the new test fails to build**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_csirs_blind_search 2>&1 | grep -m2 error"`
Expected: `error: 'nr_csirs_blind_row_ports' was not declared`.

- [ ] **Step 3: Implement**

In `nr_csirs_blind_search.h`, next to the `nr_csirs_blind_row_needs_bits` declaration:

```c
/** CSI-RS antenna ports of an enumerated row (TS 38.211 Table 7.4.1.5.3-1); 0 for a row this module
 *  does not enumerate. Reference generation must clear this many per-port buffers. */
int nr_csirs_blind_row_ports(uint8_t row);
```

In `nr_csirs_blind_search.c`, after the `kDensities` table:

```c
int nr_csirs_blind_row_ports(uint8_t row)
{
  for (unsigned i = 0; i < sizeof(kRows); i++)
    if (kRows[i] == row)
      return kPorts[i];
  return 0;
}
```

In `nr_csirs_blind_rt.c`, replace

```c
  int n_ports = 1;
  switch (c->row) {
    case 4: n_ports = 4; break;
    case 1:
    case 2:
    default: n_ports = 1; break;
  }
```

with

```c
  /* From the enumerator's own table, so a row added there cannot get the wrong port count here: rows
   * 3 and 5 (2 and 4 ports) were generated with only port 0 cleared. */
  const int n_ports = nr_csirs_blind_row_ports(c->row);
  if (n_ports <= 0 || n_ports > NR_CSIRS_BLIND_RT_MAX_PORTS)
    return;
```

- [ ] **Step 4: Run the whole suite**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 test_nr_csirs_blind_search nr-uesoftmodem 2>&1 | tail -2 && ./test_nr_csirs_blind_search 2>&1 | grep -E 'PASSED|FAILED|PARTIAL-BAND'"`
Expected: `[  PASSED  ]` for all tests, 0 failed; the `PARTIAL-BAND` line shows whole-band z < 2.5. If z ≥ 2.5, stop and report the value (do not change the threshold).

- [ ] **Step 5: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.h openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.c openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_search_test.cc && git commit -F -" <<'EOF'
Blind CSI-RS: take the port count from the enumerator's table; fix two stale tests

Rows 3 and 5 (2 and 4 ports) were enumerated but generated with n_ports=1, so their extra port
buffers were never cleared between candidates. The unused kPorts table is now the single source.
The enumeration test still expected rows {1,2,4}; the partial-band test zero-filled REs the
correlator now skips by design, so it models unrelated REs as noise instead. Suite green again.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

