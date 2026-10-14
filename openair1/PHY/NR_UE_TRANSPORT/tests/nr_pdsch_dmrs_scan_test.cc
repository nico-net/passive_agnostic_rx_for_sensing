/* TIM-PDSCH: blind per-UE PDSCH DM-RS identity scan (nr_pdsch_dmrs_scan.h). */
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>
extern "C" {
#include "nr_pdsch_dmrs_scan.h"
#include "nr_scancost.h"
}

namespace {
constexpr int N = 4096, NRB = 217, FCO = N - NRB * 12 / 2, NANT = 4, SLOT = 6, SYM = 2;

uint32_t cinit(int nid, int ns) { return (uint32_t)((((uint64_t)(14 * SLOT + SYM + 1) * (2ull * nid + 1)) << 17) + 2ull * nid + ns) % (1u << 31); }

/* plant a 2-port (OCC) type-1 CDM-group-0 DM-RS of identity nid on CRBs [rb0, rb0+nrb) */
void plant(std::vector<std::vector<int16_t>> &g, int nid, int ns, int rb0, int nrb, std::mt19937 &rng, double amp)
{
  const int nb = 12 * (rb0 + nrb);
  std::vector<uint8_t> c(nb);
  nr_dmrs_scan_gold_ref(cinit(nid, ns), 0, nb, c.data());
  std::normal_distribution<double> h(0, 1);
  for (int a = 0; a < NANT; a++) {
    const double h0r = h(rng), h0i = h(rng), h1r = 0.6 * h(rng), h1i = 0.6 * h(rng);
    for (int m = 6 * rb0; m < 6 * (rb0 + nrb); m++) {
      const double rr = (1 - 2 * c[2 * m]) / M_SQRT2, ri = (1 - 2 * c[2 * m + 1]) / M_SQRT2;
      const double w = (m % 2) ? -1 : 1; /* port 1001 OCC over k' */
      const double hr = h0r + w * h1r, hi = h0i + w * h1i;
      const int re = (FCO + 12 * (m / 6) + 2 * (m % 6)) % N;
      g[a][2 * re] = (int16_t)(amp * (hr * rr - hi * ri));
      g[a][2 * re + 1] = (int16_t)(amp * (hr * ri + hi * rr));
    }
  }
}

std::vector<std::vector<int16_t>> noise_grid(std::mt19937 &rng, double sigma)
{
  std::normal_distribution<double> n(0, sigma);
  std::vector<std::vector<int16_t>> g(NANT, std::vector<int16_t>(2 * N));
  for (auto &a : g) for (auto &v : a) v = (int16_t)n(rng);
  return g;
}
}  // namespace

TEST(PdschDmrsScan, GoldRefFirstBits)
{
  /* c_init = 0: x2 = 0, c(n) = x1(n + 1600); x1 from x1(0) = 1 is periodic with a fixed, nonzero window */
  std::vector<uint8_t> a(64), b(64);
  nr_dmrs_scan_gold_ref(0, 0, 64, a.data());
  nr_dmrs_scan_gold_ref(0, 32, 32, b.data());
  for (int i = 0; i < 32; i++) EXPECT_EQ(a[32 + i], b[i]);
  int ones = 0;
  for (int i = 0; i < 64; i++) ones += a[i];
  EXPECT_GT(ones, 10);
}

TEST(PdschDmrsScan, FindsTwoUesAboveTheOldRange)
{
  nr_dmrs_scan_reset();
  std::mt19937 rng(7);
  auto g = noise_grid(rng, 20);
  plant(g, 1677, 0, 160, 16, rng, 400);
  plant(g, 272, 0, 80, 16, rng, 300);
  std::vector<const int16_t *> rx;
  for (auto &a : g) rx.push_back(a.data());
  nr_dmrs_scan_hit_t hits[NR_DMRS_SCAN_MAX_HITS];
  const int n = nr_dmrs_scan_slot(rx.data(), NANT, N, FCO, NRB, SLOT, SYM, 14, 6, hits, NR_DMRS_SCAN_MAX_HITS);
  ASSERT_EQ(n, 2);
  bool f1677 = false, f272 = false;
  for (int i = 0; i < n; i++) {
    EXPECT_GT(hits[i].score, 0.9f);
    f1677 |= hits[i].nid == 1677 && hits[i].rb0 == 160;
    f272 |= hits[i].nid == 272 && hits[i].rb0 == 80;
  }
  EXPECT_TRUE(f1677);
  EXPECT_TRUE(f272);
  /* the pool now holds both: a later slot with UE 272 elsewhere is found without a full scan */
  auto g2 = noise_grid(rng, 20);
  plant(g2, 272, 0, 32, 16, rng, 300);
  std::vector<const int16_t *> rx2;
  for (auto &a : g2) rx2.push_back(a.data());
  const int n2 = nr_dmrs_scan_slot_pool(rx2.data(), NANT, N, FCO, NRB, SLOT, SYM, 14, 6, hits, NR_DMRS_SCAN_MAX_HITS);
  ASSERT_EQ(n2, 1);
  EXPECT_EQ(hits[0].nid, 272);
  EXPECT_EQ(hits[0].rb0, 32);
  std::vector<uint16_t> crb, wrong;
  for (int r = 160; r < 176; r++) crb.push_back((uint16_t)r);
  for (int r = 0; r < 16; r++) wrong.push_back((uint16_t)r);
  EXPECT_GT(nr_dmrs_scan_prb_score(rx.data(), NANT, N, FCO, SLOT, SYM, 14, 1677, 0, crb.data(), (int)crb.size()), 0.9f);
  EXPECT_LT(nr_dmrs_scan_prb_score(rx.data(), NANT, N, FCO, SLOT, SYM, 14, 272, 0, crb.data(), (int)crb.size()), 0.4f);
  EXPECT_LT(nr_dmrs_scan_prb_score(rx.data(), NANT, N, FCO, SLOT, SYM, 14, 1677, 0, wrong.data(), (int)wrong.size()), 0.4f);
  /* SCANCOST: one full-width Gold sequence per (id, nSCID) scores exactly like the per-call one */
  std::vector<uint8_t> gold(12 * 275);
  for (int nid : {1677, 272})
    for (int ns : {0, 1}) {
      nr_dmrs_scan_prb_gold(SLOT, SYM, 14, nid, ns, (int)gold.size(), gold.data());
      for (auto *l : {&crb, &wrong})
        EXPECT_EQ(nr_dmrs_scan_prb_score_gold(rx.data(), NANT, N, FCO, gold.data(), l->data(), (int)l->size()),
                  nr_dmrs_scan_prb_score(rx.data(), NANT, N, FCO, SLOT, SYM, 14, nid, ns, l->data(), (int)l->size()));
    }
}

/* SCANCOST: the hashed offset-key index returns what the solver's linear first-match search returned */
TEST(ScanCost, KeyHashMatchesLinearSearch)
{
  std::mt19937_64 rng(5);
  for (int round = 0; round < 50; round++) {
    uint64_t keys[256];
    int nk = 0;
    nr_keyhash_t h;
    nr_keyhash_init(&h);
    std::vector<uint64_t> pool(round % 2 ? 40 : 600);
    for (auto &k : pool) k = rng() >> (round % 3 ? 0 : 50); /* some rounds: small keys, many collisions in the low bits */
    for (int i = 0; i < 3000; i++) {
      const uint64_t key = pool[rng() % pool.size()];
      int lin = -1, pos = 0;
      for (int q = 0; q < nk && lin < 0; q++)
        if (keys[q] == key) lin = q;
      ASSERT_EQ(nr_keyhash_find(&h, keys, key, &pos), lin);
      if (lin < 0 && nk < 256) {
        keys[nk] = key;
        nr_keyhash_put(&h, pos, nk++);
      }
    }
  }
}

TEST(PdschDmrsScan, NscidOneAndNoiseOnly)
{
  std::mt19937 rng(11);
  auto g = noise_grid(rng, 20);
  plant(g, 40000, 1, 32, 8, rng, 400);
  std::vector<const int16_t *> rx;
  for (auto &a : g) rx.push_back(a.data());
  nr_dmrs_scan_hit_t hit;
  ASSERT_TRUE(nr_dmrs_scan_chunk(rx.data(), NANT, N, FCO, SLOT, SYM, 14, 32, 8, &hit));
  EXPECT_EQ(hit.nid, 40000);
  EXPECT_EQ(hit.nscid, 1);
  /* pure noise chunk: no hit */
  EXPECT_FALSE(nr_dmrs_scan_chunk(rx.data(), NANT, N, FCO, SLOT, SYM, 14, 120, 16, &hit));
}

TEST(PdschDmrsScan, VotesIsolateTheUesOwnIdentity)
{
  nr_dmrs_scan_reset();
  nr_dmrs_scan_hit_t a = {1677, 0, 160, 16, 0.95f, 0.4f}, b = {272, 0, 80, 16, 0.95f, 0.4f};
  nr_dmrs_scan_hit_t s1[2] = {a, b}, s2[1] = {b}, s3[2] = {b, a};
  nr_dmrs_scan_vote(0x5954, 100, s1, 2);
  nr_dmrs_scan_vote(0x5954, 100, s1, 2); /* same slot: counted once */
  int ids[2];
  EXPECT_EQ(nr_dmrs_scan_rnti_ids(0x5954, ids, 2), 0);
  nr_dmrs_scan_vote(0x5954, 103, s2, 1);
  ASSERT_EQ(nr_dmrs_scan_rnti_ids(0x5954, ids, 2), 1);
  EXPECT_EQ(ids[0], 272);
  nr_dmrs_scan_vote(0x5954, 107, s3, 2);
  ASSERT_EQ(nr_dmrs_scan_rnti_ids(0x5954, ids, 2), 2);
  EXPECT_EQ(ids[0], 272);
  EXPECT_EQ(ids[1], 1677);
  EXPECT_EQ(nr_dmrs_scan_rnti_slots(0x5954), 3);
  EXPECT_EQ(nr_dmrs_scan_rnti_ids(0x1234, ids, 2), 0);
}

/* SOLVERCOVER: a background identity in every slot (65) and a busy co-scheduled UE's identity (1337, 60 % of the slots)
 * outvote a quiet UE's own identity (638, detected on half of its slots). Raw votes rank 65/1337 first; the contrast
 * ranking (ISAC_PDSCH_DMRS_ID_CONTRAST=1) puts 638 first and never drops an identity. A UE alone in the cell keeps its id. */
static void VoteCell(void)
{
  std::mt19937 rng(7);
  for (int64_t s = 0; s < 2000; s++) {
    const bool busy = rng() % 10 < 6, quiet = rng() % 10 < 3, seen = rng() % 2 == 0;
    nr_dmrs_scan_hit_t h[3];
    int n = 0;
    h[n++] = {65, 0, 0, 16, 0.9f, 0.4f};
    if (busy) h[n++] = {1337, 0, 32, 16, 0.9f, 0.4f};
    if (quiet && seen) h[n++] = {638, 0, 64, 16, 0.9f, 0.4f};
    if (busy) nr_dmrs_scan_vote(0x5114, s, h, n);
    if (quiet) nr_dmrs_scan_vote(0x5098, s, h, n);
    if (!busy && !quiet) nr_dmrs_scan_vote(0x4000 + (int)(s % 5), s, h, n);
  }
}
TEST(PdschDmrsScan, ContrastRanksTheUesOwnIdentityAboveCellWideOnes)
{
  int ids[2];
  unsetenv("ISAC_PDSCH_DMRS_ID_CONTRAST");
  nr_dmrs_scan_reset();
  VoteCell();
  ASSERT_EQ(nr_dmrs_scan_rnti_ids(0x5098, ids, 2), 2);
  EXPECT_EQ(ids[0], 65); /* raw votes: the background id wins */
  EXPECT_NE(ids[1], 638);
  EXPECT_TRUE(nr_dmrs_scan_rnti_decided(0x4000)); /* raw rule: any id with 2 votes over 4 slots */
  setenv("ISAC_PDSCH_DMRS_ID_CONTRAST", "1", 1);
  nr_dmrs_scan_reset();
  VoteCell();
  ASSERT_EQ(nr_dmrs_scan_rnti_ids(0x5098, ids, 2), 2);
  EXPECT_EQ(ids[0], 638);
  ASSERT_EQ(nr_dmrs_scan_rnti_ids(0x5114, ids, 1), 1);
  EXPECT_EQ(ids[0], 1337); /* the busy UE keeps its own id first too */
  int all[8];
  EXPECT_EQ(nr_dmrs_scan_rnti_ids(0x5098, all, 8), 3); /* reordered, nothing dropped */
  EXPECT_TRUE(nr_dmrs_scan_rnti_decided(0x5098));
  EXPECT_FALSE(nr_dmrs_scan_rnti_decided(0x4000)); /* only the cell-wide id: keep full-scanning for its own */
  /* a single UE in every slot: no other slots, contrast = its own share, its id stays first */
  nr_dmrs_scan_reset();
  for (int64_t s = 0; s < 50; s++) {
    nr_dmrs_scan_hit_t h[2] = {{415, 0, 0, 16, 0.9f, 0.4f}, {442, 0, 16, 16, 0.9f, 0.4f}};
    nr_dmrs_scan_vote(0x7683, s, h, s % 3 ? 1 : 2);
  }
  ASSERT_EQ(nr_dmrs_scan_rnti_ids(0x7683, ids, 2), 2);
  EXPECT_EQ(ids[0], 415);
  unsetenv("ISAC_PDSCH_DMRS_ID_CONTRAST");
  nr_dmrs_scan_reset();
}
