### Task 8: PRB-set helpers — RA type 0, dynamicSwitch, interleaved VRB→PRB, PRG segments (pure)

The receiver only knows contiguous type-1 allocations (`nr_pdcch_blind_monitor_rt.c:5686`: `resource_alloc = 1; // Type-1/RIV -- the only branch this module ever produces`). Resource-allocation type 0 (RBG bitmap), `dynamicSwitch`, and interleaved VRB-to-PRB mapping all produce NON-contiguous PRB sets, and PRB bundling (PRG) restricts channel-estimate interpolation to PRG-aligned segments. This task only adds the spec arithmetic; Tasks 9–12 wire it.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_prb_set_test.cc`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  ```c
  int nr_rbg_size(int bwp_size, int rbg_config2);                   /* P, TS 38.214 Table 5.1.2.2.1-1 (= UL Table 6.1.2.2.1-1) */
  int nr_rbg_count(int bwp_start, int bwp_size, int P);              /* N_RBG */
  int nr_ra_type0_prbs(uint32_t bitmap, int bwp_start, int bwp_size, int P, uint16_t *prb, int max);
  int nr_fdra_dynamic_split(uint32_t field, int n_rbg, int riv_bits, uint32_t *type0_bitmap, uint32_t *riv);
  int nr_vrb_to_prb_interleaved(int bwp_start, int bwp_size, int L, int vrb_start, int n_vrb, uint16_t *prb);
  typedef struct { uint16_t prb_start; uint16_t n_prb; uint16_t data_index; } nr_prb_seg_t;
  int nr_prb_segments(const uint16_t *prb, int n, int bwp_start, int prg, nr_prb_seg_t *seg, int max);
  #define NR_PRB_SET_MAX 275
  ```

- [ ] **Step 1: Write the failing tests**

Create `tests/nr_pdsch_prb_set_test.cc` (expected values hand-derived from the spec text, see comments):

```cpp
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdsch_prb_set.h"
}

TEST(PrbSet, RbgSizeTable) {  // 38.214 Table 5.1.2.2.1-1
  EXPECT_EQ(nr_rbg_size(36, 0), 2);  EXPECT_EQ(nr_rbg_size(36, 1), 4);
  EXPECT_EQ(nr_rbg_size(37, 0), 4);  EXPECT_EQ(nr_rbg_size(72, 1), 8);
  EXPECT_EQ(nr_rbg_size(73, 0), 8);  EXPECT_EQ(nr_rbg_size(144, 1), 16);
  EXPECT_EQ(nr_rbg_size(273, 0), 16); EXPECT_EQ(nr_rbg_size(273, 1), 16);
  EXPECT_EQ(nr_rbg_size(0, 0), 0);   EXPECT_EQ(nr_rbg_size(276, 0), 0);
}

TEST(PrbSet, Type0AlignedBwp) {
  // start 0, size 51, config1 -> P=4, N_RBG = 13, last RBG = 51 mod 4 = 3 RBs.
  EXPECT_EQ(nr_rbg_count(0, 51, 4), 13);
  uint16_t p[NR_PRB_SET_MAX];
  const int n = nr_ra_type0_prbs((1u << 12) | 1u, 0, 51, 4, p, NR_PRB_SET_MAX);  // RBG 0 (MSB) and RBG 12
  ASSERT_EQ(n, 7);
  const uint16_t want[7] = {0, 1, 2, 3, 48, 49, 50};
  for (int i = 0; i < 7; i++) EXPECT_EQ(p[i], want[i]);
}

TEST(PrbSet, Type0MisalignedBwpHasShortFirstAndLastRbg) {
  // start 3, size 20, P=4: N_RBG = ceil(23/4) = 6; RBG0 = {0} (4-3=1 RB), RBG5 = {17,18,19} ((3+20) mod 4 = 3).
  EXPECT_EQ(nr_rbg_count(3, 20, 4), 6);
  uint16_t p[NR_PRB_SET_MAX];
  ASSERT_EQ(nr_ra_type0_prbs(1u << 5, 3, 20, 4, p, NR_PRB_SET_MAX), 1);
  EXPECT_EQ(p[0], 0);
  ASSERT_EQ(nr_ra_type0_prbs(1u, 3, 20, 4, p, NR_PRB_SET_MAX), 3);
  EXPECT_EQ(p[0], 17); EXPECT_EQ(p[2], 19);
  ASSERT_EQ(nr_ra_type0_prbs((1u << 6) - 1, 3, 20, 4, p, NR_PRB_SET_MAX), 20);  // all RBGs = whole BWP
}

TEST(PrbSet, DynamicSwitchMsbSelectsType) {
  // width = 1 + max(N_RBG=13, riv_bits=11) = 14; MSB (bit 13) = 1 -> type 1.
  uint32_t t0 = 0, riv = 0;
  EXPECT_EQ(nr_fdra_dynamic_split((1u << 13) | 0x2AB, 13, 11, &t0, &riv), 1);
  EXPECT_EQ(riv, 0x2ABu);
  EXPECT_EQ(nr_fdra_dynamic_split(0x1801, 13, 11, &t0, &riv), 0);
  EXPECT_EQ(t0, 0x1801u);
}

TEST(PrbSet, InterleavedVrbEvenBundleCount) {
  // start 0, size 10, L=2: 5 bundles, C=2; f = {0,2,1,3,4} (last bundle fixed).
  uint16_t p[10];
  ASSERT_EQ(nr_vrb_to_prb_interleaved(0, 10, 2, 0, 10, p), 10);
  const uint16_t want[10] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9};
  for (int i = 0; i < 10; i++) EXPECT_EQ(p[i], want[i]) << i;
}

TEST(PrbSet, InterleavedVrbSevenBundles) {
  // start 0, size 14, L=2: 7 bundles, C=3; f = {0,3,1,4,2,5,6}.
  uint16_t p[14];
  ASSERT_EQ(nr_vrb_to_prb_interleaved(0, 14, 2, 0, 14, p), 14);
  const uint16_t want[14] = {0, 1, 6, 7, 2, 3, 8, 9, 4, 5, 10, 11, 12, 13};
  for (int i = 0; i < 14; i++) EXPECT_EQ(p[i], want[i]) << i;
}

TEST(PrbSet, InterleavedVrbMisalignedIsIdentityWithThreeBundles) {
  // start 1, size 10, L=4: bundles {0..2},{3..6},{7..9}; C=1 -> f = {0,1,2}.
  uint16_t p[10];
  ASSERT_EQ(nr_vrb_to_prb_interleaved(1, 10, 4, 0, 10, p), 10);
  for (int i = 0; i < 10; i++) EXPECT_EQ(p[i], i);
}

TEST(PrbSet, InterleavedVrbIsAPermutation) {
  for (int start : {0, 1, 2, 3, 5})
    for (int L : {2, 4})
      for (int size : {24, 51, 106, 273}) {
        std::vector<uint16_t> p(size);
        ASSERT_EQ(nr_vrb_to_prb_interleaved(start, size, L, 0, size, p.data()), size);
        std::vector<int> seen(size, 0);
        for (auto v : p) { ASSERT_LT(v, size); seen[v]++; }
        for (int i = 0; i < size; i++) EXPECT_EQ(seen[i], 1) << start << "/" << L << "/" << size;
      }
}

TEST(PrbSet, SegmentsFollowDataOrderAndPrgBoundaries) {
  const uint16_t il[10] = {0, 1, 4, 5, 2, 3, 6, 7, 8, 9};
  nr_prb_seg_t s[16];
  ASSERT_EQ(nr_prb_segments(il, 10, 0, 0, s, 16), 4);
  EXPECT_EQ(s[0].prb_start, 0); EXPECT_EQ(s[0].n_prb, 2); EXPECT_EQ(s[0].data_index, 0);
  EXPECT_EQ(s[1].prb_start, 4); EXPECT_EQ(s[1].data_index, 2);
  EXPECT_EQ(s[2].prb_start, 2); EXPECT_EQ(s[2].data_index, 4);
  EXPECT_EQ(s[3].prb_start, 6); EXPECT_EQ(s[3].n_prb, 4);
  uint16_t c[10];
  for (int i = 0; i < 10; i++) c[i] = (uint16_t)i;
  ASSERT_EQ(nr_prb_segments(c, 10, 0, 2, s, 16), 5);  // PRG 2 on CRB 0..9
  ASSERT_EQ(nr_prb_segments(c, 10, 1, 4, s, 16), 3);  // PRG 4 on CRB 1..10: breaks at CRB 4 and 8
  EXPECT_EQ(s[0].n_prb, 3); EXPECT_EQ(s[1].n_prb, 4); EXPECT_EQ(s[2].n_prb, 3);
  ASSERT_EQ(nr_prb_segments(c, 10, 0, 0, s, 16), 1);  // wideband contiguous = today's single segment
  EXPECT_EQ(nr_prb_segments(c, 10, 0, 2, s, 4), -1);  // does not fit
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

Register after the Task 6 block and add the source to the library:

```cmake
  add_executable(test_nr_pdsch_prb_set ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_prb_set_test.cc
                                       ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.c)
  target_include_directories(test_nr_pdsch_prb_set PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdsch_prb_set PRIVATE GTest::gtest)
  add_dependencies(tests test_nr_pdsch_prb_set)
  add_test(NAME test_nr_pdsch_prb_set COMMAND ./test_nr_pdsch_prb_set)
```

Append `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.c` to `add_library(nr_pdcch_blind_monitor …)`.

- [ ] **Step 2: Run to verify it fails**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && cmake . >/dev/null && make test_nr_pdsch_prb_set 2>&1 | grep -m1 -E 'error|No such file'"`
Expected: `nr_pdsch_prb_set.h: No such file or directory`.

- [ ] **Step 3: Implement**

`nr_pdsch_prb_set.h`:

```c
#ifndef NR_PDSCH_PRB_SET_H
#define NR_PDSCH_PRB_SET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Frequency-domain resource arithmetic for non-contiguous allocations. All PRB indices are
 * BWP-relative; bwp_start is the BWP's first CRB (RBG, VRB-bundle and PRG boundaries are aligned to
 * the COMMON RB grid, which is why it is needed). */

#define NR_PRB_SET_MAX 275

/** RBG size P (TS 38.214 Table 5.1.2.2.1-1; UL Table 6.1.2.2.1-1 is identical). 0 if out of range. */
int nr_rbg_size(int bwp_size, int rbg_config2);
/** N_RBG = ceil((N_size + (N_start mod P)) / P). */
int nr_rbg_count(int bwp_start, int bwp_size, int P);
/** PRBs of an RA type-0 bitmap (N_RBG bits, MSB = RBG 0), increasing. Returns the count. */
int nr_ra_type0_prbs(uint32_t bitmap, int bwp_start, int bwp_size, int P, uint16_t *prb, int max);
/** dynamicSwitch FDRA field (1 + max(N_RBG, riv_bits) bits): MSB 0 -> type 0, bitmap = N_RBG LSBs,
 *  returns 0; MSB 1 -> type 1, RIV = riv_bits LSBs, returns 1 (TS 38.212 7.3.1.2.2). */
int nr_fdra_dynamic_split(uint32_t field, int n_rbg, int riv_bits, uint32_t *type0_bitmap, uint32_t *riv);
/** Interleaved VRB-to-PRB mapping (TS 38.211 7.3.1.6), bundle size L in {2,4}. prb[i] is the PRB of
 *  VRB vrb_start+i, i.e. the output is in DATA order (PDSCH maps to VRBs in increasing order).
 *  For DCI 1_0 in a common search space pass bwp_start = 0, the initial-BWP size, L = 2. */
int nr_vrb_to_prb_interleaved(int bwp_start, int bwp_size, int L, int vrb_start, int n_vrb, uint16_t *prb);

/** One contiguous piece of an allocation, in data order: channel estimation and RE extraction run per
 *  segment and the segments' REs are concatenated in array order. */
typedef struct { uint16_t prb_start; uint16_t n_prb; uint16_t data_index; } nr_prb_seg_t;
/** Split a data-ordered PRB list into contiguous segments; prg > 0 also splits at CRB multiples of prg
 *  (PRB bundling: precoding may change there, so a channel estimate must not interpolate across it).
 *  prg = 0 = wideband. Returns the segment count, -1 if more than max. */
int nr_prb_segments(const uint16_t *prb, int n, int bwp_start, int prg, nr_prb_seg_t *seg, int max);

#ifdef __cplusplus
}
#endif
#endif
```

`nr_pdsch_prb_set.c`:

```c
#include "nr_pdsch_prb_set.h"
#include <stdbool.h>
#include <stddef.h>

int nr_rbg_size(int bwp_size, int rbg_config2)
{
  static const int lim[4] = {36, 72, 144, 275}, p1[4] = {2, 4, 8, 16}, p2[4] = {4, 8, 16, 16};
  for (int i = 0; i < 4; i++)
    if (bwp_size >= 1 && bwp_size <= lim[i])
      return rbg_config2 ? p2[i] : p1[i];
  return 0;
}

int nr_rbg_count(int bwp_start, int bwp_size, int P)
{
  if (P <= 0 || bwp_size <= 0 || bwp_start < 0)
    return 0;
  return (bwp_size + bwp_start % P + P - 1) / P;
}

int nr_ra_type0_prbs(uint32_t bitmap, int bwp_start, int bwp_size, int P, uint16_t *prb, int max)
{
  const int n_rbg = nr_rbg_count(bwp_start, bwp_size, P);
  if (n_rbg <= 0 || n_rbg > 32 || prb == NULL)
    return 0;
  const int off = bwp_start % P;
  int n = 0;
  for (int g = 0; g < n_rbg; g++) {
    if (!((bitmap >> (n_rbg - 1 - g)) & 1u))
      continue;
    const int first = g == 0 ? 0 : g * P - off;
    const int last = g == n_rbg - 1 ? bwp_size - 1 : (g + 1) * P - off - 1;
    for (int r = first; r <= last && n < max; r++)
      prb[n++] = (uint16_t)r;
  }
  return n;
}

int nr_fdra_dynamic_split(uint32_t field, int n_rbg, int riv_bits, uint32_t *type0_bitmap, uint32_t *riv)
{
  const int w = n_rbg > riv_bits ? n_rbg : riv_bits;
  if ((field >> w) & 1u) {
    if (riv)
      *riv = field & ((1u << riv_bits) - 1u);
    return 1;
  }
  if (type0_bitmap)
    *type0_bitmap = field & ((1u << n_rbg) - 1u);
  return 0;
}

int nr_vrb_to_prb_interleaved(int bwp_start, int bwp_size, int L, int vrb_start, int n_vrb, uint16_t *prb)
{
  if ((L != 2 && L != 4) || bwp_size <= 0 || bwp_start < 0 || vrb_start < 0 || n_vrb <= 0
      || vrb_start + n_vrb > bwp_size || prb == NULL)
    return 0;
  const int off = bwp_start % L;
  const int nb = (bwp_size + off + L - 1) / L, C = nb / 2;
  for (int i = 0; i < n_vrb; i++) {
    const int v = vrb_start + i;
    const int j = (v + off) / L;                            /* VRB bundle */
    const int f = (j == nb - 1) ? j : (j % 2) * C + j / 2;  /* PRB bundle: f(j) = rC + c, j = cR + r, R = 2 */
    const int vfirst = j == 0 ? 0 : j * L - off;
    const int pfirst = f == 0 ? 0 : f * L - off;
    prb[i] = (uint16_t)(pfirst + (v - vfirst));             /* bundles 0 and N-1 map to themselves, so sizes agree */
  }
  return n_vrb;
}

int nr_prb_segments(const uint16_t *prb, int n, int bwp_start, int prg, nr_prb_seg_t *seg, int max)
{
  if (prb == NULL || seg == NULL || n <= 0)
    return 0;
  int ns = 0;
  for (int i = 0; i < n; i++) {
    const bool cont = ns > 0 && prb[i] == prb[i - 1] + 1 && !(prg > 0 && (bwp_start + prb[i]) % prg == 0);
    if (cont) {
      seg[ns - 1].n_prb++;
      continue;
    }
    if (ns == max)
      return -1;
    seg[ns++] = (nr_prb_seg_t){prb[i], 1, (uint16_t)i};
  }
  return ns;
}
```

- [ ] **Step 4: Run**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_pdsch_prb_set 2>&1 | tail -1 && ./test_nr_pdsch_prb_set 2>&1 | grep -E 'PASSED|FAILED'"`
Expected: `[  PASSED  ] 9 tests`.

- [ ] **Step 5: Commit** — message `Passive PDSCH: PRB-set arithmetic for RA type 0, dynamicSwitch, interleaved VRB and PRG segments` + trailer; files: the three new files and `CMakeLists.txt`.

---

