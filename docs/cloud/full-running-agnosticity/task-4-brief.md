### Task 4: AL1 cover module (pure)

At AL1 the candidate is one CCE = 6 REGs, and many CCE-to-REG mappings produce identical AL1 candidates (with bundle size 6 the interleaver only renames CCEs). Measured offline (`scratchpad/al1_cover.py`, same rules as `nr_pdcch_map_candidates()`):

| Shape | Mappings | Distinct AL1 REG sets | Covering mappings |
|---|---|---|---|
| 48 RB, D=1 | 81 | 88 | 11 |
| 270 RB, D=1 | 181 | 90 | 2 |
| 270 RB, D=2 | 1,081 | 990 | 11 |
| 270 RB, D=3 | 946 | 1,080 | 10 |
| 216 RB, D=2 | 865 | 792 | 11 |

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.h`, `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c`
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc`
- Modify: `CMakeLists.txt` (library source + test target)

**Interfaces:**
- Consumes: nothing (standalone; its own mapping struct so it needs no PHY headers).
- Produces:
  ```c
  typedef struct { uint8_t bundle; uint8_t interleaver; uint16_t shift; } nr_pdcch_al1_map_t; /* bundle 0 = non-interleaved */
  #define NR_PDCCH_AL1_MAX_MAPS  1200
  #define NR_PDCCH_AL1_MAX_COVER 32
  int nr_pdcch_al1_enumerate(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out);
  int nr_pdcch_al1_regset(int span_rb, int duration, nr_pdcch_al1_map_t m, int cce, uint16_t out[6]);
  int nr_pdcch_al1_cover(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out);
  int nr_pdcch_al1_narrow(int span_rb, int duration, const uint16_t (*obs)[6], int n_obs,
                          nr_pdcch_al1_map_t *cand, int n);
  int nr_pdcch_al1_family_count(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n);
  ```

- [ ] **Step 1: Write the failing tests**

Create `tests/nr_pdcch_al1_map_test.cc`:

```cpp
// Offline tests for the AL1 cover. Expected counts come from an independent Python enumeration of the
// TS 38.211 7.3.2.2 rules (session 2026-09-25), not from this module.
#include <chrono>
#include <cstdio>
#include <set>
#include <vector>
#include <gtest/gtest.h>
extern "C" {
#include "nr_pdcch_al1_map.h"
}

namespace {
struct Shape { int rb, d, maps, distinct, cover; };
const Shape kShapes[] = {{48, 1, 81, 88, 11}, {270, 1, 181, 90, 2}, {270, 2, 1081, 990, 11},
                         {270, 3, 946, 1080, 10}, {216, 2, 865, 792, 11}};

std::set<std::vector<uint16_t>> family(int rb, int d, nr_pdcch_al1_map_t m)
{
  std::set<std::vector<uint16_t>> f;
  for (int j = 0; j < rb * d / 6; j++) {
    uint16_t r[6];
    EXPECT_EQ(nr_pdcch_al1_regset(rb, d, m, j, r), 6);
    f.insert(std::vector<uint16_t>(r, r + 6));
  }
  return f;
}
}  // namespace

TEST(Al1Map, EnumerationMatchesTheSpecCount) {
  std::vector<nr_pdcch_al1_map_t> m(NR_PDCCH_AL1_MAX_MAPS);
  for (const auto &s : kShapes)
    EXPECT_EQ(nr_pdcch_al1_enumerate(s.rb, s.d, m.data(), (int)m.size()), s.maps) << s.rb << "x" << s.d;
}

TEST(Al1Map, RegsetIsSortedAndInRange) {
  uint16_t r[6];
  ASSERT_EQ(nr_pdcch_al1_regset(270, 2, {2, 3, 17}, 11, r), 6);
  for (int k = 0; k < 6; k++) {
    EXPECT_LT(r[k], 540);
    if (k) EXPECT_LT(r[k - 1], r[k]);
  }
  EXPECT_EQ(nr_pdcch_al1_regset(270, 2, {2, 3, 17}, 90, r), 0);  // CCE out of range
  EXPECT_EQ(nr_pdcch_al1_regset(270, 3, {2, 2, 0}, 0, r), 0);    // L=2 illegal at D=3
}

TEST(Al1Map, CoverUnionEqualsFullUnion) {
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS), cov(NR_PDCCH_AL1_MAX_COVER);
  for (const auto &s : kShapes) {
    const int n = nr_pdcch_al1_enumerate(s.rb, s.d, all.data(), (int)all.size());
    std::set<std::vector<uint16_t>> full, covered;
    for (int i = 0; i < n; i++)
      for (const auto &r : family(s.rb, s.d, all[i])) full.insert(r);
    const int nc = nr_pdcch_al1_cover(s.rb, s.d, cov.data(), (int)cov.size());
    for (int i = 0; i < nc; i++)
      for (const auto &r : family(s.rb, s.d, cov[i])) covered.insert(r);
    EXPECT_EQ((int)full.size(), s.distinct) << s.rb << "x" << s.d;
    EXPECT_EQ(covered, full) << s.rb << "x" << s.d;
    EXPECT_EQ(nc, s.cover) << s.rb << "x" << s.d;
    EXPECT_EQ(cov[0].bundle, 0) << "non-interleaved must lead the cover";
  }
}

TEST(Al1Map, CoverFitsTheLaneBoundForEveryLegalShape) {
  std::vector<nr_pdcch_al1_map_t> cov(NR_PDCCH_AL1_MAX_COVER);
  for (int d = 1; d <= 3; d++)
    for (int rb = 6; rb <= 270; rb += 6) {
      const int nc = nr_pdcch_al1_cover(rb, d, cov.data(), (int)cov.size());
      EXPECT_GT(nc, 0) << rb << "x" << d;
      EXPECT_LT(nc, NR_PDCCH_AL1_MAX_COVER) << rb << "x" << d << " would hit the cap";
    }
}

TEST(Al1Map, CoverIsFastEnoughForTheLanePath) {
  std::vector<nr_pdcch_al1_map_t> cov(NR_PDCCH_AL1_MAX_COVER);
  auto t0 = std::chrono::steady_clock::now();
  nr_pdcch_al1_cover(270, 2, cov.data(), (int)cov.size());
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  printf("AL1 cover 270x2: %.2f ms\n", ms);
  EXPECT_LT(ms, 20.0);
}

TEST(Al1Map, NarrowKeepsTheTruthAndCollapsesToItsFamily) {
  const nr_pdcch_al1_map_t truth = {2, 3, 101};
  std::vector<nr_pdcch_al1_map_t> cand(NR_PDCCH_AL1_MAX_MAPS);
  int n = nr_pdcch_al1_enumerate(270, 2, cand.data(), (int)cand.size());
  uint16_t obs[3][6];
  for (int k = 0; k < 3; k++) ASSERT_EQ(nr_pdcch_al1_regset(270, 2, truth, 5 + 29 * k, obs[k]), 6);
  const int n1 = nr_pdcch_al1_narrow(270, 2, obs, 1, cand.data(), n);
  bool kept = false;
  for (int i = 0; i < n1; i++)
    kept |= cand[i].bundle == truth.bundle && cand[i].interleaver == truth.interleaver && cand[i].shift == truth.shift;
  EXPECT_TRUE(kept);
  const int n3 = nr_pdcch_al1_narrow(270, 2, obs, 3, cand.data(), n1);
  EXPECT_LE(n3, n1);
  EXPECT_EQ(nr_pdcch_al1_family_count(270, 2, cand.data(), n3), 1);
}

TEST(Al1Map, EveryBundle6MappingSharesTheNonInterleavedFamily) {
  std::vector<nr_pdcch_al1_map_t> all(NR_PDCCH_AL1_MAX_MAPS);
  const int n = nr_pdcch_al1_enumerate(270, 1, all.data(), (int)all.size());
  std::vector<nr_pdcch_al1_map_t> l6 = {{0, 0, 0}};
  for (int i = 0; i < n; i++)
    if (all[i].bundle == 6) l6.push_back(all[i]);
  EXPECT_GT(l6.size(), 1u);
  EXPECT_EQ(nr_pdcch_al1_family_count(270, 1, l6.data(), (int)l6.size()), 1);
}

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
```

Register it in `CMakeLists.txt` right after the `test_nr_pdcch_joint_live` block:

```cmake
  add_executable(test_nr_pdcch_al1_map ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc
                                       ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c)
  target_include_directories(test_nr_pdcch_al1_map PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_pdcch_al1_map PRIVATE GTest::gtest)
  add_dependencies(tests test_nr_pdcch_al1_map)
  add_test(NAME test_nr_pdcch_al1_map COMMAND ./test_nr_pdcch_al1_map)
```

and append `${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c` to the `add_library(nr_pdcch_blind_monitor …)` source list after `nr_pdcch_joint_live.c`.

- [ ] **Step 2: Run to verify it fails**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && cmake . >/dev/null && make test_nr_pdcch_al1_map 2>&1 | grep -m1 -E 'error|No such file'"`
Expected: `nr_pdcch_al1_map.h: No such file or directory`.

- [ ] **Step 3: Implement**

Create `nr_pdcch_al1_map.h`:

```c
#ifndef NR_PDCCH_AL1_MAP_H
#define NR_PDCCH_AL1_MAP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* AL1 CANDIDATE GEOMETRY, independent of the CORESET's CCE-to-REG mapping (TS 38.211 7.3.2.2).
 * An AL1 PDCCH candidate is one CCE = 6 REGs. Many mappings produce the SAME set of AL1 candidates:
 * with bundle size 6 the interleaver only renames which CCE a bundle is. So AL1 discovery needs only a
 * small COVER of the mapping catalogue (2-11 mappings for 81-1081, measured), and one verified AL1
 * decode pins the true mapping to the mappings whose AL1 family contains its REG set. */

typedef struct { uint8_t bundle; uint8_t interleaver; uint16_t shift; } nr_pdcch_al1_map_t; /* bundle 0 = non-interleaved */

#define NR_PDCCH_AL1_MAX_MAPS  1200 /* > 1081, the largest legal catalogue (270 RB x 2 symbols) */
#define NR_PDCCH_AL1_MAX_COVER 32   /* lane-loop bound for the cover lap; every legal shape stays below it */

/** Every legal mapping of a span_rb x duration CORESET, in nr_pdcch_map_candidates()' rule order:
 *  non-interleaved, then L in {2,6} (D=1,2) / {3,6} (D=3), R in {2,3,6} with N_REG % (L*R) == 0, every
 *  shift 0..N_REG/L-1. Returns the count (capped by max_out); 0 for an illegal shape. */
int nr_pdcch_al1_enumerate(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out);
/** The 6 REG indices (REGs numbered time-first: REG n = RB n/D, symbol n%D), ascending, of AL1 CCE
 *  `cce` under mapping m. Returns 6, or 0 when m or cce is illegal for the shape. */
int nr_pdcch_al1_regset(int span_rb, int duration, nr_pdcch_al1_map_t m, int cce, uint16_t out[6]);
/** Greedy minimal subset of nr_pdcch_al1_enumerate() whose AL1 families together contain every
 *  distinct AL1 REG set of the whole catalogue. Deterministic; non-interleaved first. */
int nr_pdcch_al1_cover(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out);
/** In place: keep the mappings of cand[0..n) whose AL1 family contains every observed REG set.
 *  Returns the survivor count. */
int nr_pdcch_al1_narrow(int span_rb, int duration, const uint16_t (*obs)[6], int n_obs,
                        nr_pdcch_al1_map_t *cand, int n);
/** Number of distinct AL1 families among cand[0..n) (1 = AL1 decoding is exact with any of them). */
int nr_pdcch_al1_family_count(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n);

#ifdef __cplusplus
}
#endif
#endif
```

Create `nr_pdcch_al1_map.c`:

```c
#include "nr_pdcch_al1_map.h"
#include <stdlib.h>
#include <string.h>

#define AL1_MAX_CCE 135 /* 270 RB x 3 symbols / 6 */

static int shape_ok(int span_rb, int d)
{
  return span_rb > 0 && span_rb % 6 == 0 && d >= 1 && d <= 3 && span_rb * d / 6 <= AL1_MAX_CCE;
}

int nr_pdcch_al1_enumerate(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out)
{
  if (!shape_ok(span_rb, duration) || out == NULL || max_out <= 0)
    return 0;
  int n = 0;
  out[n++] = (nr_pdcch_al1_map_t){0, 0, 0};
  const int N = span_rb * duration;
  const int Ls[2] = {duration == 3 ? 3 : 2, 6};
  static const int Rs[3] = {2, 3, 6};
  for (int li = 0; li < 2; li++) {
    const int L = Ls[li];
    if (L % duration)
      continue;
    const int nb = N / L;
    for (int ri = 0; ri < 3; ri++) {
      if (N % (L * Rs[ri]))
        continue;
      for (int s = 0; s < nb && n < max_out; s++)
        out[n++] = (nr_pdcch_al1_map_t){(uint8_t)L, (uint8_t)Rs[ri], (uint16_t)s};
    }
  }
  return n;
}

int nr_pdcch_al1_regset(int span_rb, int duration, nr_pdcch_al1_map_t m, int cce, uint16_t out[6])
{
  if (!shape_ok(span_rb, duration) || out == NULL)
    return 0;
  const int N = span_rb * duration;
  if (cce < 0 || cce >= N / 6)
    return 0;
  if (m.bundle == 0) {
    for (int k = 0; k < 6; k++)
      out[k] = (uint16_t)(6 * cce + k);
    return 6;
  }
  const int L = m.bundle, R = m.interleaver;
  if (L <= 0 || 6 % L || L % duration || R <= 0 || N % (L * R))
    return 0;
  const int nb = N / L, C = nb / R, per = 6 / L;
  if (m.shift >= nb)
    return 0;
  int o = 0;
  for (int k = 0; k < per; k++) {
    const int x = per * cce + k;
    const int f = ((x % R) * C + x / R + m.shift) % nb;
    for (int r = 0; r < L; r++)
      out[o++] = (uint16_t)(f * L + r);
  }
  for (int i = 1; i < 6; i++) { /* insertion sort: canonical order */
    const uint16_t v = out[i];
    int j = i - 1;
    while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
    out[j + 1] = v;
  }
  return 6;
}

/* A REG set as one 64-bit key: 6 sorted 10-bit indices (N_REG <= 810 < 1024). */
static uint64_t key6(const uint16_t r[6])
{
  uint64_t k = 0;
  for (int i = 0; i < 6; i++)
    k = (k << 10) | r[i];
  return k;
}

static int family_keys(int span_rb, int d, nr_pdcch_al1_map_t m, uint64_t *keys)
{
  const int ncce = span_rb * d / 6;
  uint16_t r[6];
  for (int j = 0; j < ncce; j++) {
    if (nr_pdcch_al1_regset(span_rb, d, m, j, r) != 6)
      return 0;
    keys[j] = key6(r);
  }
  return ncce;
}

/* Open-addressing set of keys -> dense index. Size is a power of two >= 4x the largest union (1080). */
#define HSZ 8192
typedef struct { uint64_t key[HSZ]; int idx[HSZ]; int n; } kset_t;
static int kset_find(const kset_t *s, uint64_t k)
{
  for (uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 51) & (HSZ - 1);; h = (h + 1) & (HSZ - 1)) {
    if (s->idx[h] < 0) return -1;
    if (s->key[h] == k) return s->idx[h];
  }
}
static int kset_add(kset_t *s, uint64_t k)
{
  for (uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ULL) >> 51) & (HSZ - 1);; h = (h + 1) & (HSZ - 1)) {
    if (s->idx[h] < 0) { s->key[h] = k; s->idx[h] = s->n; return s->n++; }
    if (s->key[h] == k) return s->idx[h];
  }
}

int nr_pdcch_al1_cover(int span_rb, int duration, nr_pdcch_al1_map_t *out, int max_out)
{
  if (!shape_ok(span_rb, duration) || out == NULL || max_out <= 0)
    return 0;
  nr_pdcch_al1_map_t *maps = malloc(sizeof(*maps) * NR_PDCCH_AL1_MAX_MAPS);
  const int ncce = span_rb * duration / 6;
  int *fam = malloc(sizeof(int) * (size_t)NR_PDCCH_AL1_MAX_MAPS * ncce); /* dense key index per (map, cce) */
  kset_t *set = malloc(sizeof(*set));
  uint8_t *covered = calloc(HSZ, 1);
  int nout = 0;
  if (maps && fam && set && covered) {
    memset(set->idx, 0xff, sizeof(set->idx));
    set->n = 0;
    const int nm = nr_pdcch_al1_enumerate(span_rb, duration, maps, NR_PDCCH_AL1_MAX_MAPS);
    uint64_t keys[AL1_MAX_CCE];
    for (int m = 0; m < nm; m++) {
      family_keys(span_rb, duration, maps[m], keys);
      for (int j = 0; j < ncce; j++)
        fam[m * ncce + j] = kset_add(set, keys[j]);
    }
    int left = set->n;
    while (left > 0 && nout < max_out) {
      int best = -1, best_gain = 0;
      for (int m = 0; m < nm; m++) {
        int gain = 0;
        for (int j = 0; j < ncce; j++)
          gain += !covered[fam[m * ncce + j]];
        if (gain > best_gain) { best_gain = gain; best = m; } /* strict: first maximum wins */
      }
      if (best < 0)
        break;
      for (int j = 0; j < ncce; j++)
        if (!covered[fam[best * ncce + j]]) { covered[fam[best * ncce + j]] = 1; left--; }
      out[nout++] = maps[best];
    }
  }
  free(maps); free(fam); free(set); free(covered);
  return nout;
}

int nr_pdcch_al1_narrow(int span_rb, int duration, const uint16_t (*obs)[6], int n_obs,
                        nr_pdcch_al1_map_t *cand, int n)
{
  if (!shape_ok(span_rb, duration) || cand == NULL || n <= 0 || n_obs <= 0 || obs == NULL)
    return n > 0 ? n : 0;
  uint64_t want[16];
  const int nw = n_obs < 16 ? n_obs : 16;
  for (int k = 0; k < nw; k++)
    want[k] = key6(obs[k]);
  uint64_t keys[AL1_MAX_CCE];
  int kept = 0;
  for (int i = 0; i < n; i++) {
    const int nk = family_keys(span_rb, duration, cand[i], keys);
    int all = nk > 0;
    for (int k = 0; k < nw && all; k++) {
      int hit = 0;
      for (int j = 0; j < nk && !hit; j++)
        hit = keys[j] == want[k];
      all = hit;
    }
    if (all)
      cand[kept++] = cand[i];
  }
  return kept;
}

static int cmp_u64(const void *a, const void *b)
{
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

int nr_pdcch_al1_family_count(int span_rb, int duration, const nr_pdcch_al1_map_t *cand, int n)
{
  if (!shape_ok(span_rb, duration) || cand == NULL || n <= 0)
    return 0;
  uint64_t *fp = malloc(sizeof(uint64_t) * (size_t)n);
  if (fp == NULL)
    return 0;
  uint64_t keys[AL1_MAX_CCE];
  for (int i = 0; i < n; i++) {
    const int nk = family_keys(span_rb, duration, cand[i], keys);
    qsort(keys, (size_t)nk, sizeof(keys[0]), cmp_u64);
    uint64_t h = 1469598103934665603ULL; /* FNV-1a over the sorted family = family fingerprint */
    for (int j = 0; j < nk; j++)
      h = (h ^ keys[j]) * 1099511628211ULL;
    fp[i] = h;
  }
  qsort(fp, (size_t)n, sizeof(fp[0]), cmp_u64);
  int d = 1;
  for (int i = 1; i < n; i++)
    d += fp[i] != fp[i - 1];
  free(fp);
  return d;
}
```

- [ ] **Step 4: Run the tests**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 test_nr_pdcch_al1_map 2>&1 | tail -1 && ./test_nr_pdcch_al1_map 2>&1 | grep -E 'AL1 cover|PASSED|FAILED'"`
Expected: `[  PASSED  ] 7 tests`, and the `AL1 cover 270x2` time printed under 20 ms. If a count mismatches the table, stop and report (the table was produced independently; a mismatch means one of the two enumerations is wrong).

- [ ] **Step 5: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_al1_map.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc CMakeLists.txt && git commit -F -" <<'EOF'
Blind PDCCH: AL1 cover of the CCE-to-REG mapping catalogue (pure module)

An AL1 candidate is one CCE; with bundle size 6 the interleaver only renames CCEs, so many mappings
produce identical AL1 candidates. A greedy cover of 2-11 mappings reproduces every distinct AL1 REG
set of catalogues of 81-1081 mappings (counts checked against an independent enumeration). Also
narrowing by observed REG sets and an AL1-family count, for verification.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

