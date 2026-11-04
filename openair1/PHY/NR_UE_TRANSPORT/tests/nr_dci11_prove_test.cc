// E1: CRC-free proof of the DCI 1_1 pre_ant split. Synthetic scheduler streams: a known RRC config (widths), HARQ pids, a counter DAI per
// HARQ-ACK window, K1 from a list, TDD pattern DDDSU (slot 3 = special = MIXED = UL-capable, slot 4 = UL).
#include "nr_dci11_prove.h"
#include "nr_traffic_dci_fields.h"
#include "common/utils/nr_cell.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

static bool g_known = true;
static int g_ulmask = 0x18;   // residues 3 (special, MIXED) and 4 (UL) of DDDSU; 0x1C adds the FLEXIBLE slot 2
static bool ul_dddsu(int64_t s, int *known)
{
  *known = g_known ? 1 : 0;
  return (g_ulmask >> (int)(((s % 5) + 5) % 5)) & 1;
}
static const nr_dci11_feed_t FEED = {1, 1000000000, 1000000000};
static nr_dci11_class_t cls_of(const nr_dci11_layout_t &l, const nr_dci11_offsets_t &o) { nr_dci11_class_t c; c.l = l; c.o = o; return c; }

struct Cfg {
  int tb2 = 0, harq = 4, dai = 2, tpc2 = 0, k1 = 2, ext = 0;   // truth widths (ext = total extra bits, never read)
  std::vector<int> L = {1, 2, 3, 4};                            // K1 list (size 2^k1, or 1)
  int k0_hi = 0;                                                // K0 of TDRA indices >= 2 (0 = all cross-slot offsets zero)
  bool random_fb = false;                                       // feedback on any UL-capable slot (default: the next full UL slot)
  bool random_pid = false;                                      // arbitrary HARQ pids (no +1..3 habit)
  int pre_ant() const { return tb2 + harq + dai + 2 + tpc2 + 3 + k1 + ext; }
};

struct Dci { nr_dci_bits_t p; int64_t slot; };

static void put(nr_dci_bits_t &p, int len, int pos, int w, unsigned v)
{
  for (int i = 0; i < w; i++) {
    const int bit = len - 1 - (pos + i);
    if ((v >> (w - 1 - i)) & 1) p.w[bit / 64] |= 1ULL << (bit % 64); else p.w[bit / 64] &= ~(1ULL << (bit % 64));
  }
}

static void make_layout(const Cfg &c, nr_dci11_layout_t *l, nr_dci11_offsets_t *o)
{
  *l = nr_dci11_layout_t{};
  l->pre_ant = (uint8_t)c.pre_ant(); l->ant_ports = 4; l->fdra_mode = NR_FDRA_TYPE1;
  ASSERT_TRUE(nr_dci11_layout_offsets(l, 11, 4, o));
}

// One RNTI's stream of `n` DCIs. UEs share the generator state only through the rng.
static std::vector<Dci> gen_stream(const Cfg &c, int n, std::mt19937 &rng, int64_t first_slot = 1000)
{
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const int base = o.ant_ports - l.pre_ant, len = o.total;
  std::vector<Dci> out;
  int pid = 0, ndi[32] = {0};
  std::map<int64_t, int> win;   // feedback slot -> DCIs so far (counter DAI)
  for (int64_t s = first_slot; (int)out.size() < n; s++) {
    if (s % 5 == 4 || (rng() % 10) >= 5) continue;                  // UL slot, or not scheduled this slot
    const int tda = (int)(rng() % 4);
    const int64_t m = s + (tda >= 2 ? c.k0_hi : 0);                 // PDSCH slot
    std::vector<int> ok;
    for (size_t i = 0; i < c.L.size(); i++) { const int r = (int)((m + c.L[i]) % 5); if (r == 3 || r == 4) ok.push_back((int)i); }
    if (ok.empty()) continue;
    int idx = ok[rng() % ok.size()];
    if (!c.random_fb)
      for (int i : ok) if ((m + c.L[i]) % 5 == 4) idx = i;   // the PUCCH of the period sits on its UL slot
    const int64_t f = m + c.L[idx];
    nr_dci_bits_t p{};
    for (int i = 0; i < len; i++) put(p, len, i, 1, rng() & 1);     // every unread bit is random
    const unsigned r = (unsigned)(rng() % 10);
    const int step = c.random_pid ? (int)(rng() % (1u << c.harq)) : (r < 1 ? 0 : r < 3 ? 2 : 1);
    pid = (pid + step) & ((1 << c.harq) - 1);
    if (step != 0 || c.random_pid) ndi[pid] ^= 1;
    put(p, len, o.tda, o.tda_bits, (unsigned)tda);
    put(p, len, o.mcs, 5, (unsigned)(rng() % 28)); put(p, len, o.mcs + 5, 1, (unsigned)ndi[pid]);
    int pos = base + c.tb2;
    put(p, len, pos, c.harq, (unsigned)pid); pos += c.harq;
    if (c.dai) { put(p, len, pos, 2, (unsigned)((win[f]++) & 3)); }          // low bits (total DAI) stay random
    pos += c.dai + 2 + c.tpc2 + 3;
    if (c.k1) put(p, len, pos, c.k1, (unsigned)idx);
    out.push_back({p, s});
  }
  return out;
}

static std::vector<nr_dci11_obs_t> obs_of(const std::vector<Dci> &v, int len)
{
  std::vector<nr_dci11_obs_t> b;
  for (const Dci &d : v) b.push_back({d.p, (uint16_t)len, d.slot});
  return b;
}

struct Res { std::vector<nr_dci11_split_t> alive; std::vector<double> p; };
static Res run(const Cfg &c, const std::vector<Dci> &v, nr_ul_slot_fn ul = ul_dddsu, const Cfg *layout_of = nullptr)
{
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(layout_of ? *layout_of : c, &l, &o);
  auto b = obs_of(v, o.total);
  Res r;
  r.alive.resize(NR_DCI11_CORE_MAX); r.p.resize(NR_DCI11_CORE_MAX);
  const int n = nr_dci11_prove(&l, &o, b.data(), (int)b.size(), ul, r.alive.data(), r.p.data(), NR_DCI11_CORE_MAX);
  EXPECT_GE(n, 0);
  r.alive.resize(std::max(n, 0)); r.p.resize(std::max(n, 0));
  return r;
}

static std::string dump(const Res &r)
{
  std::string o;
  for (size_t i = 0; i < r.alive.size(); i++) {
    const auto &s = r.alive[i];
    char b[160];
    snprintf(b, sizeof b, "[tb2 %d harq %d dai %d tpc2 %d k1 %d p=%.3g] ", s.tb2, s.harq, s.dai, s.tpc2, s.k1, r.p[i]);
    o += b;
  }
  return o;
}

static bool is_truth(const nr_dci11_split_t &s, const Cfg &c)
{
  return s.tb2 == c.tb2 && s.harq == c.harq && s.dai == c.dai && s.tpc2 == c.tpc2 && s.k1 == c.k1 && s.tpc == 2 && s.pri == 3;
}

struct Real {
  Cfg c;
  double loss = 0.1, p10 = 0.2, corrupt = 0.01, sps = 0.005;
  bool lowest_free = false;
  int pucch_mask = 0x10;   // residues the gNB puts PUCCH on (subset of the UL-capable ones)
};

static std::vector<Dci> gen_real(const Real &R, int n_kept, std::mt19937 &rng, int64_t first_slot = 1000);

TEST(Dci11Prove, EnumeratesSpecSplits)
{
  auto count = [](int pre_ant, std::vector<nr_dci11_split_t> *out = nullptr) {
    nr_dci11_layout_t l{}; l.pre_ant = (uint8_t)pre_ant;
    std::vector<nr_dci11_split_t> v(20000);
    const int n = nr_dci11_splits(&l, v.data(), (int)v.size());
    v.resize(std::max(n, 0));
    if (out) *out = v;
    return n;
  };
  EXPECT_EQ(count(9), 1);    // harq 4 + TPC 2 + PRI 3
  EXPECT_EQ(count(38), 1);   // 8 + 5 + 6 + 2 + 2 + 3 + 3 + extras 1+1+2+1+1+3
  EXPECT_EQ(count(8), 0);
  EXPECT_EQ(count(39), 0);
  // pre_ant 10 = 9 + one more bit. Hand count: HARQ pid 5 bits (1), K1 1 bit (1), or one extra bit in one of six extras (one-shot, group idx, NFI,
  // n groups, retx, enhanced type 3) = 6. TB2 (8), DAI (2), 2nd TPC (2) cannot fit one bit. Total 8.
  std::vector<nr_dci11_split_t> v;
  ASSERT_EQ(count(10, &v), 8);
  int h5 = 0, k1 = 0, ext = 0;
  for (const auto &s : v) {
    EXPECT_EQ(s.tb2 + s.harq + s.dai + s.tpc + s.tpc2 + s.pri + s.k1 + s.ext[0] + s.ext[1] + s.ext[2] + s.ext[3] + s.ext[4] + s.ext[5], 10);
    EXPECT_EQ(s.tpc, 2); EXPECT_EQ(s.pri, 3);
    if (s.harq == 5) h5++; else if (s.k1 == 1) k1++; else ext++;
  }
  EXPECT_EQ(h5, 1); EXPECT_EQ(k1, 1); EXPECT_EQ(ext, 6);
  // an ambiguous mid sum has many splits; every one sums right and none is repeated
  const int n13 = count(13, &v);
  EXPECT_GT(n13, 8);
  for (size_t i = 0; i < v.size(); i++) {
    int sum = v[i].tb2 + v[i].harq + v[i].dai + v[i].tpc + v[i].tpc2 + v[i].pri + v[i].k1;
    for (int e : v[i].ext) sum += e;
    EXPECT_EQ(sum, 13);
    for (size_t j = i + 1; j < v.size(); j++) EXPECT_NE(0, memcmp(&v[i], &v[j], sizeof v[i]));
  }
  nr_dci11_layout_t l{};
  l.pre_ant = 13;
  EXPECT_EQ(nr_dci11_splits(&l, nullptr, 0), n13);   // count only
  EXPECT_EQ(nr_dci11_splits(nullptr, nullptr, 0), -1);
}

TEST(Dci11Prove, ProvesTrueSplit)
{
  std::mt19937 rng(1);
  Cfg c;   // harq 4, dai 2, tpc2 0, k1 2 (4 entries), pre_ant 13
  for (int ue = 0; ue < 8; ue++) {
    const auto v = gen_stream(c, NR_DCI11_BANK, rng);   // 8 UEs x 64 DCIs
    const Res r = run(c, v);
    ASSERT_EQ(r.alive.size(), 1u) << "ue " << ue << " " << dump(r);
    EXPECT_TRUE(is_truth(r.alive[0], c)) << "ue " << ue;
    EXPECT_LT(r.p[0], 1e-3) << "ue " << ue;
  }
}

TEST(Dci11Prove, ProvesAlsoWithCrossSlotK0)
{
  // K0 differs per TDRA index (the prover never sees K0): the (tda, k1) keys absorb it, the truth must survive.
  std::mt19937 rng(2);
  Cfg c; c.k0_hi = 1;
  for (int ue = 0; ue < 4; ue++) {
    const auto v = gen_stream(c, NR_DCI11_BANK, rng);
    const Res r = run(c, v);
    bool truth = false;
    for (const auto &s : r.alive) truth |= is_truth(s, c);
    EXPECT_TRUE(truth) << "ue " << ue;
  }
}

TEST(Dci11Prove, DaiRefutesWrongWidth)
{
  // Same sum 13: truth = DAI 2 + no 2nd TPC; the wrong reading calls the same two bits a 2nd TPC (DAI 0). Every other test reads the same bits, so only
  // the counter DAI separates them.
  std::mt19937 rng(3);
  Cfg c;
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  const Res r = run(c, v);
  ASSERT_EQ(r.alive.size(), 1u) << dump(r);
  EXPECT_TRUE(is_truth(r.alive[0], c));
  Cfg w = c; w.dai = 0; w.tpc2 = 2;
  // the wrong split is ALIVE when the DAI test cannot run (unknown TDD): that is what the DAI test removes
  g_known = false;
  const Res r0 = run(c, v);
  g_known = true;
  bool wrong_alive = false;
  for (const auto &s : r0.alive) wrong_alive |= (s.dai == 0 && s.tpc2 == 2 && s.harq == 4 && s.k1 == 2);
  EXPECT_TRUE(wrong_alive);
  EXPECT_GE(r0.alive.size(), 2u);
}

TEST(Dci11Prove, K1RefutesOffUl)
{
  std::mt19937 rng(4);
  Cfg c;
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const auto b = obs_of(v, o.total);
  auto split = [&](int dai, int tpc2, int k1) {
    nr_dci11_split_t s{}; s.tb2 = 0; s.harq = 4; s.dai = (uint8_t)dai; s.tpc = 2; s.tpc2 = (uint8_t)tpc2; s.pri = 3; s.k1 = (uint8_t)k1; return s;
  };
  // K1 width 0 (one offset for every DCI of a TDRA index): the DCIs sit in 4 slot phases, no single offset serves them all
  const nr_dci11_split_t w0 = split(4, 0, 0);
  bool any_empty = false;
  for (int t = 0; t < 4; t++) any_empty |= nr_dci11_k1_feasible(&l, &o, b.data(), (int)b.size(), ul_dddsu, &w0, t, 0) == 0;
  EXPECT_TRUE(any_empty);
  const Res r = run(c, v);
  for (const auto &s : r.alive) EXPECT_NE(s.k1, 0);
  // the truth keeps a non-empty set per key and it contains the true offset
  const nr_dci11_split_t tr = split(2, 0, 2);
  for (int t = 0; t < 4; t++)
    for (int i = 0; i < 4; i++) {
      const uint64_t m = nr_dci11_k1_feasible(&l, &o, b.data(), (int)b.size(), ul_dddsu, &tr, t, i);
      EXPECT_NE(m, 0u) << t << "," << i;
      if (m != UINT64_MAX) { EXPECT_TRUE((m >> c.L[i]) & 1) << t << "," << i << " L=" << c.L[i]; }
    }
  // a pattern with no UL slot at all refutes every split (every DCI would land on DL-only slots)
  auto all_dl = [](int64_t, int *known) { *known = 1; return false; };
  EXPECT_EQ(run(c, v, all_dl).alive.size(), 0u);
}

TEST(Dci11Prove, HarqStepRefutesShiftedPid)
{
  // Only the HARQ pid test runs (TDD unknown: K1 and DAI abstain). pre_ant 17 = harq 4 + DAI 4 + 2nd TPC 2 + K1 2 + fixed 5 (truth, TB2 absent);
  // a TB2 reading (8 bits) puts the pid on the bits behind it, where nothing steps by 1..3.
  std::mt19937 rng(12);
  Cfg c; c.dai = 4; c.tpc2 = 2; c.k1 = 2;
  ASSERT_EQ(c.pre_ant(), 17);
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  g_known = false;
  const Res r = run(c, v);
  g_known = true;
  bool truth = false;
  ASSERT_GT(r.alive.size(), 1u);   // no K1/DAI: several readings with the pid in the right place stay
  for (const auto &s : r.alive) { EXPECT_EQ(s.tb2, 0) << dump(r); truth |= is_truth(s, c); }
  EXPECT_TRUE(truth);
}

TEST(Dci11Prove, K1ToleratesOutliersButNotAWrongTable)
{
  // a few DCIs that follow no table (CRC false alarm, SPS release, reused RNTI) must not refute the truth; a split that mis-reads K1 is still refuted
  for (int seed = 0; seed < 20; seed++) {
    std::mt19937 rng(500 + seed);
    Cfg c;
    auto v = gen_stream(c, NR_DCI11_BANK, rng);
    nr_dci11_layout_t l; nr_dci11_offsets_t o;
    make_layout(c, &l, &o);
    const int base = o.ant_ports - l.pre_ant;
    for (int k : {5, 30}) put(v[k].p, o.total, base + 11, 2, (v[k].p.w[0] >> 3) & 3);   // two payloads with arbitrary K1 indices
    const Res r = run(c, v);
    bool truth = false;
    for (const auto &s : r.alive) truth |= is_truth(s, c);
    EXPECT_TRUE(truth) << seed << " " << dump(r);
  }
  // all-DL pattern: every DCI infeasible, tolerance does not save anyone
  std::mt19937 rng(77);
  Cfg c;
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  auto all_dl = [](int64_t, int *known) { *known = 1; return false; };
  EXPECT_EQ(run(c, v, all_dl).alive.size(), 0u);
}

TEST(Dci11Prove, FlexibleSlotsCarryPucch)
{
  // PUCCH on the FLEXIBLE slot 2 of D D F S U: with FLEXIBLE counted as UL-capable (mask 0x1C) the truth stays; a pattern calling it DL-only would refute it
  std::mt19937 rng(41);
  Cfg c;
  Real R; R.c = c; R.loss = 0; R.p10 = 0; R.corrupt = 0; R.sps = 0; R.pucch_mask = 0x1C;   // the gNB takes the first of slots 2 (FLEXIBLE), 3, 4
  g_ulmask = 0x1C;
  auto v = gen_real(R, NR_DCI11_BANK, rng);
  Res r = run(c, v);
  bool truth = false;
  for (const auto &s : r.alive) truth |= is_truth(s, c);
  EXPECT_TRUE(truth) << dump(r);
  g_ulmask = 0x18;   // the same stream under a pattern where slot 2 is DL: the K1 test refutes (this is the pre-fix behaviour)
  r = run(c, v);
  truth = false;
  for (const auto &s : r.alive) truth |= is_truth(s, c);
  EXPECT_FALSE(truth);
  g_ulmask = 0x18;
}

TEST(Dci11Prove, UnknownTddAbstains)
{
  std::mt19937 rng(5);
  Cfg c;
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  g_known = false;
  const Res known0 = run(c, v, ul_dddsu);
  g_known = true;
  const Res none = run(c, v, nullptr);   // no pattern at all
  ASSERT_EQ(known0.alive.size(), none.alive.size());
  for (size_t i = 0; i < none.alive.size(); i++) {
    EXPECT_EQ(0, memcmp(&known0.alive[i], &none.alive[i], sizeof(nr_dci11_split_t)));
    EXPECT_EQ(known0.p[i], none.p[i]);
  }
  // K1 and DAI refuted nothing: the K1-width-0 split is still alive, and the stream's K1 feasibility abstains
  bool w0 = false;
  for (const auto &s : known0.alive) w0 |= s.k1 == 0;
  EXPECT_TRUE(w0);
  EXPECT_GT(known0.alive.size(), 1u);
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const auto b = obs_of(v, o.total);
  nr_dci11_split_t s{}; s.harq = 4; s.dai = 4; s.tpc = 2; s.pri = 3;
  g_known = false;
  EXPECT_EQ(UINT64_MAX, nr_dci11_k1_feasible(&l, &o, b.data(), (int)b.size(), ul_dddsu, &s, 0, 0));
  g_known = true;
}

TEST(Dci11Prove, IndistinguishableStaysCandidate)
{
  // truth: 4-bit DAI field (counter + total). The other reading: 2-bit DAI + 2nd TPC. Same counter bits, same K1, same HARQ: no test separates them.
  std::mt19937 rng(6);
  Cfg c; c.dai = 4; c.k1 = 2;
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  const Res r = run(c, v);
  ASSERT_EQ(r.alive.size(), 2u) << dump(r);
  bool a = false, b = false;
  for (const auto &s : r.alive) { a |= (s.dai == 4 && s.tpc2 == 0); b |= (s.dai == 2 && s.tpc2 == 2); EXPECT_EQ(s.harq, 4); EXPECT_EQ(s.k1, 2); }
  EXPECT_TRUE(a && b);
  // not proven: two survivors
}

// ---- per-cell banks ----
static void feed_all(uint16_t rnti, const std::vector<Dci> &v, int len)
{
  for (const Dci &d : v) nr_dci11_bank_feed(rnti, d.p, (uint16_t)len, d.slot, &FEED);
}

TEST(Dci11Prove, BankAliveCountForTriage)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(11);
  Cfg a;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const uint16_t rnti = 0x4602;
  EXPECT_EQ(nr_dci11_bank_alive(rnti), -1);                            // never looked at
  feed_all(rnti, gen_stream(a, 64, rng), o.total);
  EXPECT_EQ(nr_dci11_bank_alive(rnti), -1);                            // fed, not proved yet
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  EXPECT_EQ(nr_dci11_bank_alive(rnti), 1);                             // exactly the proven split
  nr_dci11_bank_alive_clear(rnti);                                      // solver reopen / layout change
  EXPECT_EQ(nr_dci11_bank_alive(rnti), -1);
}

TEST(Dci11Prove, ReopensOnViolations)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(7);
  Cfg a;                                                              // DAI 2 + K1 2
  Cfg b; b.dai = 0; b.k1 = 3; b.ext = 1; b.L = {1, 2, 3, 4, 6, 7, 8, 9};   // same sum (13), a different RRC config: 3-bit K1, one-shot bit
  ASSERT_EQ(a.pre_ant(), b.pre_ant());
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const uint16_t rnti = 0x4601;
  feed_all(rnti, gen_stream(a, 64, rng), o.total);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));      // precondition: a proof exists
  nr_dci11_split_t s{};
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  EXPECT_EQ(s.dai, 2); EXPECT_EQ(s.k1, 2);
  // 64 DCIs of the other config replace the ring: the old reading violates the K1 table and is refuted (hysteresis ends)
  feed_all(rnti, gen_stream(b, 64, rng, 5000), o.total);
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));     // the state changed
  nr_dci11_split_t s2{};
  const bool still = nr_dci11_bank_proven(rnti, &l, &o, &s2);
  EXPECT_FALSE(still && s2.dai == 2 && s2.k1 == 2);
  nr_dci11_bank_reset();
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s2));
}

TEST(Dci11Prove, BankConsistentWithAClassEvenFromOneDci)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(12);
  Cfg a;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const uint16_t rnti = 0x4607;
  EXPECT_TRUE(nr_dci11_bank_consistent(rnti, &kc, ul_dddsu)) << "no bank: nothing refuted";
  const auto v = gen_stream(a, 64, rng);
  nr_dci11_bank_feed(rnti, v[0].p, (uint16_t)o.total, v[0].slot, &FEED);
  EXPECT_TRUE(nr_dci11_bank_consistent(rnti, &kc, ul_dddsu)) << "one DCI refutes nothing";
  feed_all(rnti, v, o.total);
  EXPECT_TRUE(nr_dci11_bank_consistent(rnti, &kc, ul_dddsu));
  auto all_dl = [](int64_t, int *known) { *known = 1; return false; };   // a TDD pattern with no UL slot refutes every split
  EXPECT_FALSE(nr_dci11_bank_consistent(rnti, &kc, all_dl));
  EXPECT_FALSE(nr_dci11_bank_consistent(rnti, nullptr, ul_dddsu));
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, ProofStandsOnMoreOfTheSameAndEndsAtARival)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(21);
  Cfg a;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const uint16_t rnti = 0x4603;
  auto first = gen_stream(a, 64, rng);
  feed_all(rnti, first, o.total);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  nr_dci11_split_t s{};
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  // 16 more DCIs of the same config: nothing changes
  feed_all(rnti, gen_stream(a, 16, rng, 20000), o.total);
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  EXPECT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  // the next 64 DCIs (the whole ring) have arbitrary pids AND arbitrary DAI bits: nothing in them refutes the old reading, but many readings survive
  // there, a RIVAL. A reading that is no longer the only one is not a proof (M2): it ends.
  Cfg w = a; w.random_pid = true;
  auto weak = gen_stream(w, 64, rng, 30000);
  const int base = o.ant_ports - l.pre_ant;
  for (auto &d : weak) put(d.p, o.total, base + 4, 2, (unsigned)(rng() & 3));
  ASSERT_GT(run(w, weak).alive.size(), 1u);   // precondition: on its own the weak window proves nothing and leaves rivals
  feed_all(rnti, weak, o.total);
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));   // the state changed
  nr_dci11_split_t s2{};
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s2));
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, UnexaminableClassIsAmbiguousNotRefuted)
{
  // a class the prover cannot examine (pre_ant longer than the offsets allow: ant_ports < pre_ant, prove() == -1) must not count as refuted: with a good class
  // proving the truth, adding it makes the round ambiguous, so no proof
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(25);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  nr_dci11_class_t bad = cls_of(l, o);
  bad.o.ant_ports = 3;                 // < pre_ant
  bad.o.mcs += 1;                      // a distinct class
  const nr_dci11_class_t good = cls_of(l, o);
  nr_dci11_split_t al[NR_DCI11_CORE_MAX]; double cp[NR_DCI11_CORE_MAX];
  ASSERT_EQ(nr_dci11_prove(&bad.l, &bad.o, nullptr, 0, ul_dddsu, al, cp, NR_DCI11_CORE_MAX), -1);
  const auto v = gen_stream(c, 64, rng);
  const uint16_t rnti = 0x4606;
  feed_all(rnti, v, o.total);
  nr_dci11_class_t two[2] = {good, bad};
  nr_dci11_bank_maybe_prove(rnti, two, 2, ul_dddsu);
  nr_dci11_split_t s{};
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  nr_dci11_bank_reset();
  feed_all(rnti, v, o.total);
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &good, 1, ul_dddsu));   // the good class alone does prove
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, ClassSetGatesTheStoredReading)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(26);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  r->n_hyp = 1; r->n_alive = 1; r->hyp[0] = l; r->off[0] = o; r->alive[0] = true;
  nr_dci11_class_t cl[NR_DCI11_CLASS_MAX];
  ASSERT_EQ(nr_dci11_class_set(r, cl, false, nullptr), NR_DCI11_CLS_NONE);   // never scans on the receive thread
  ASSERT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 1);
  ASSERT_EQ(nr_dci11_class_set(r, nullptr, false, nullptr), 1);               // cached: no scan needed
  const uint16_t rnti = 0x4607;
  feed_all(rnti, gen_stream(c, 64, rng), o.total);
  ASSERT_TRUE(nr_dci11_bank_due(rnti));
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, cl, 1, ul_dddsu));
  nr_dci11_class_t pc; nr_dci11_split_t ps;
  ASSERT_TRUE(nr_dci11_bank_proven_any(rnti, &pc, &ps));
  EXPECT_FALSE(nr_dci11_bank_due(rnti));
  // N4: the resolver loses every layout: no class set; the stored proof is dropped and does not come back when a layout reappears
  r->alive[0] = false; r->n_alive = 0;
  EXPECT_EQ(nr_dci11_class_set(r, nullptr, false, nullptr), NR_DCI11_CLS_NONE);   // changed set not yet examined: no reading
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, nullptr), NR_DCI11_CLS_NONE);
  EXPECT_FALSE(nr_dci11_bank_proven_any(rnti, &pc, &ps));
  r->alive[0] = true; r->n_alive = 1;
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 1);
  EXPECT_FALSE(nr_dci11_bank_proven_any(rnti, &pc, &ps));                 // needs a fresh proof
  feed_all(rnti, gen_stream(c, 64, rng, 40000), o.total);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, cl, 1, ul_dddsu));
  ASSERT_TRUE(nr_dci11_bank_proven_any(rnti, &pc, &ps));
  // N2: an incomplete resolver set (layouts not stored) abstains: no classes, proofs dropped, and a skipped round proves nothing
  r->n_truncated = 5;
  EXPECT_EQ(nr_dci11_classes(r, cl, NR_DCI11_CLASS_MAX), -2);
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, nullptr), NR_DCI11_CLS_INCOMPLETE);
  EXPECT_FALSE(nr_dci11_bank_proven_any(rnti, &pc, &ps));
  feed_all(rnti, gen_stream(c, 64, rng, 50000), o.total);
  ASSERT_TRUE(nr_dci11_bank_due(rnti));
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, nullptr, 0, ul_dddsu));    // skipped round: counted by the caller, look again after 16 more
  EXPECT_FALSE(nr_dci11_bank_due(rnti));
  EXPECT_FALSE(nr_dci11_bank_proven_any(rnti, &pc, &ps));
  r->n_truncated = 0;
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 1);
  free(r);
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, SolvedRntiProvesInItsOwnClassWhateverTheResolverHolds)
{
  // 20 alive classes (over the cap): an UNSOLVED RNTI abstains (counted, histogram), a CRC-SOLVED one proves within its known layout's single class
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(27);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  for (int i = 0; i < 20; i++) { r->hyp[i] = l; r->off[i] = o; r->off[i].mcs = (uint16_t)(o.mcs + i); r->alive[i] = true; }
  r->n_hyp = 20; r->n_alive = 20;
  nr_dci11_class_t cl[NR_DCI11_CLASS_MAX];
  int tot = 0;
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, &tot), NR_DCI11_CLS_TOO_MANY);
  EXPECT_EQ(tot, 20);                                             // histogram bucket 17-64
  const uint16_t rnti = 0x4608;
  feed_all(rnti, gen_stream(c, 64, rng), o.total);
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, nullptr, 0, ul_dddsu));   // the unsolved path: skipped round
  nr_dci11_split_t s{};
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  feed_all(rnti, gen_stream(c, 64, rng, 7000), o.total);
  const nr_dci11_class_t own = cls_of(l, o);                      // the solver's representative layout
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &own, 1, ul_dddsu));
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  EXPECT_EQ(s.dai, 2); EXPECT_EQ(s.k1, 2);
  // more than 64 classes: the histogram says so
  for (int i = 20; i < 70; i++) { r->hyp[i] = l; r->off[i] = o; r->off[i].mcs = (uint16_t)(o.mcs + i); r->alive[i] = true; }
  r->n_hyp = 70; r->n_alive = 70;
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, &tot), NR_DCI11_CLS_TOO_MANY);
  EXPECT_EQ(tot, NR_DCI11_CLASS_COUNT_MAX + 1);
  free(r);
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, EmitRuleScopedUnscopedAndLength)
{
  nr_dci11_layout_t la, lb; nr_dci11_offsets_t oa, ob;
  Cfg c13, c14; c14.k1 = 3;
  make_layout(c13, &la, &oa); make_layout(c14, &lb, &ob);
  const nr_dci11_class_t pc = cls_of(la, oa);
  const uint16_t len = oa.total;
  const nr_dci11_class_t S1[1] = {pc}, SB[1] = {cls_of(lb, ob)};   // current cached class set: this class / another one
  // unsolved (no rep): scoped never, unscoped only with a usable current set
  EXPECT_FALSE(nr_dci11_emit_ok(true, true, len, &pc, nullptr, nullptr, S1, 1));       // D1: solve -> prove -> unsolve -> no emission
  EXPECT_TRUE(nr_dci11_emit_ok(true, false, len, &pc, nullptr, nullptr, S1, 1));
  EXPECT_FALSE(nr_dci11_emit_ok(true, false, len, &pc, nullptr, nullptr, nullptr, 0));
  // solved: scoped only while the rep still has the stored class; unscoped also needs the same class under a rep
  EXPECT_TRUE(nr_dci11_emit_ok(true, true, len, &pc, &la, &oa, nullptr, 0));
  EXPECT_FALSE(nr_dci11_emit_ok(true, true, len, &pc, &lb, &ob, S1, 1));
  EXPECT_FALSE(nr_dci11_emit_ok(true, false, len, &pc, &lb, &ob, S1, 1));
  EXPECT_TRUE(nr_dci11_emit_ok(true, false, len, &pc, &la, &oa, S1, 1));
  // D4: a DCI of another length is never read with this class
  EXPECT_FALSE(nr_dci11_emit_ok(true, true, (uint16_t)(len + 1), &pc, &la, &oa, S1, 1));
  EXPECT_FALSE(nr_dci11_emit_ok(true, false, (uint16_t)(len - 1), &pc, nullptr, nullptr, S1, 1));
  EXPECT_FALSE(nr_dci11_emit_ok(false, false, len, &pc, &la, &oa, S1, 1));
  // F2: an unscoped proof whose class is no longer in the current cached set is not emitted (with or without a rep); a scoped one does not depend on the set
  EXPECT_FALSE(nr_dci11_emit_ok(true, false, len, &pc, nullptr, nullptr, SB, 1));
  EXPECT_FALSE(nr_dci11_emit_ok(true, false, len, &pc, &la, &oa, SB, 1));
  EXPECT_TRUE(nr_dci11_emit_ok(true, true, len, &pc, &la, &oa, SB, 1));
}

TEST(Dci11Prove, SolvedProofSurvivesTheUnsolvedClassSetChurn)
{
  // D3: an unsolved look that finds no usable class set drops the UNSCOPED proofs only; the solved RNTI keeps its own-class proof
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_dci11_budget_reset();
  std::mt19937 rng(28);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const nr_dci11_class_t own = cls_of(l, o);
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  r->hyp[0] = l; r->off[0] = o; r->alive[0] = true; r->n_hyp = 1; r->n_alive = 1;
  const uint16_t solved = 0x4701, unsolved = 0x4702, other = 0x4703;
  feed_all(solved, gen_stream(c, 64, rng), o.total);
  feed_all(unsolved, gen_stream(c, 64, rng), o.total);
  int tot = 0;
  EXPECT_TRUE(nr_dci11_bank_service(solved, r, &own, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);
  EXPECT_TRUE(nr_dci11_bank_service(unsolved, r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);
  nr_dci11_class_t pc; nr_dci11_split_t ps; bool scoped = false;
  ASSERT_TRUE(nr_dci11_bank_proven_scope(solved, &pc, &ps, &scoped)); EXPECT_TRUE(scoped);
  ASSERT_TRUE(nr_dci11_bank_proven_scope(unsolved, &pc, &ps, &scoped)); EXPECT_FALSE(scoped);
  // the resolver widens to 20 classes and keeps narrowing: every unsolved look finds TOO_MANY
  for (int i = 1; i < 21; i++) { r->hyp[i] = l; r->off[i] = o; r->off[i].mcs = (uint16_t)(o.mcs + i); r->alive[i] = true; }
  r->n_hyp = 21; r->n_alive = 21;
  for (int k = 0; k < 3; k++) {
    feed_all(other, gen_stream(c, 16, rng, 100000 + 1000 * k), o.total);
    const unsigned fl = nr_dci11_bank_service(other, r, nullptr, ul_dddsu, &tot);
    EXPECT_TRUE(fl & NR_DCI11_SVC_SKIPPED);
    EXPECT_GE(tot, 16);                                            // histogram input: more than 16 classes
    r->alive[20 - k] = false; r->n_alive--;                       // n_alive changes between looks
  }
  EXPECT_TRUE(nr_dci11_bank_proven_scope(solved, &pc, &ps, &scoped));    // still there
  EXPECT_FALSE(nr_dci11_bank_proven_scope(unsolved, &pc, &ps, &scoped)); // the unscoped one went with the set
  // a scoped proof is replaced, not carried, by an unscoped look of the same RNTI that cannot prove (set unusable): it drops at that look
  free(r);
  nr_dci11_bank_reset();
}

static uint64_t g_fake_t = 0, g_fake_step = 15000000ull;
static uint64_t fake_clock(void) { const uint64_t v = g_fake_t; g_fake_t += g_fake_step; return v; }   // every reading advances by g_fake_step

TEST(Dci11Prove, ProveBudgetDefersAndIsCounted)
{
  // scoped sub-budget 10 ms per second of wall time per cell: each service reads the clock twice (start, end) and is charged the difference (15 ms here), so one
  // look fits and the second is held back; it stays due and is counted once (also as a scoped deferral); a new window lets it run
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_dci11_budget_reset();
  g_fake_t = 1000000000ull; g_fake_step = 15000000ull;
  nr_dci11_set_clock(fake_clock);
  std::mt19937 rng(29);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const nr_dci11_class_t own = cls_of(l, o);
  uint16_t rn[2] = {0x4711, 0x4712};
  for (uint16_t r : rn) feed_all(r, gen_stream(c, 64, rng), o.total);
  int tot;
  EXPECT_TRUE(nr_dci11_bank_service(rn[0], nullptr, &own, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);   // spent 15 ms (a look that starts under budget may overshoot)
  unsigned fl = nr_dci11_bank_service(rn[1], nullptr, &own, ul_dddsu, &tot);
  EXPECT_EQ(fl, (unsigned)(NR_DCI11_SVC_DEFERRED | NR_DCI11_SVC_DEFERRED_SCOPED));   // over budget: held back, counted
  EXPECT_TRUE(nr_dci11_bank_due(rn[1]));                                // and still due
  EXPECT_EQ(nr_dci11_bank_service(rn[1], nullptr, &own, ul_dddsu, &tot), 0u);   // counted once per episode, not per accept
  EXPECT_TRUE(nr_dci11_bank_due(rn[1]));
  g_fake_t += 2000000000ull;                                            // two seconds later: a fresh window
  EXPECT_TRUE(nr_dci11_bank_service(rn[1], nullptr, &own, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);
  EXPECT_FALSE(nr_dci11_bank_due(rn[1]));
  nr_dci11_set_clock(nullptr);
  nr_dci11_budget_reset();
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, UnscopedDemandNeverDelaysAScopedLook)
{
  // F1(a): the unscoped demand is far above its 10 ms/s; a scoped look of the same window still runs (reserved sub-budget), and a second scoped one is bounded by
  // the scoped sub-budget alone. Merging the two sub-budgets (one index) fails here.
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_dci11_budget_reset();
  g_fake_t = 1000000000ull; g_fake_step = 12000000ull;
  nr_dci11_set_clock(fake_clock);
  std::mt19937 rng(31);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const nr_dci11_class_t own = cls_of(l, o);
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  r->hyp[0] = l; r->off[0] = o; r->alive[0] = true; r->n_hyp = 1; r->n_alive = 1;
  const uint16_t un[5] = {0x4721, 0x4722, 0x4723, 0x4724, 0x4725}, sc[2] = {0x4731, 0x4732};
  for (uint16_t x : un) feed_all(x, gen_stream(c, 64, rng), o.total);
  for (uint16_t x : sc) feed_all(x, gen_stream(c, 64, rng), o.total);
  int tot;
  EXPECT_TRUE(nr_dci11_bank_service(un[0], r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);   // 12 ms: the unscoped share is gone
  for (int i = 1; i < 5; i++) EXPECT_EQ(nr_dci11_bank_service(un[i], r, nullptr, ul_dddsu, &tot), (unsigned)NR_DCI11_SVC_DEFERRED);   // demand above budget
  EXPECT_TRUE(nr_dci11_bank_service(sc[0], nullptr, &own, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);   // the scoped look is not delayed
  EXPECT_EQ(nr_dci11_bank_service(sc[1], nullptr, &own, ul_dddsu, &tot), (unsigned)(NR_DCI11_SVC_DEFERRED | NR_DCI11_SVC_DEFERRED_SCOPED));   // its own share is gone
  free(r);
  nr_dci11_set_clock(nullptr);
  nr_dci11_budget_reset();
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, OldestDueIsServedFirstWithinTheSubBudget)
{
  // F1(b): C became due after B; in the next window C's accept comes first but B (waiting longer) goes first; C follows in the window after
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_dci11_budget_reset();
  g_fake_t = 1000000000ull; g_fake_step = 12000000ull;
  nr_dci11_set_clock(fake_clock);
  std::mt19937 rng(32);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  r->hyp[0] = l; r->off[0] = o; r->alive[0] = true; r->n_hyp = 1; r->n_alive = 1;
  const uint16_t A = 0x4741, B = 0x4742, C = 0x4743;
  for (uint16_t x : {A, B, C}) feed_all(x, gen_stream(c, 64, rng), o.total);
  int tot;
  EXPECT_TRUE(nr_dci11_bank_service(A, r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);
  EXPECT_TRUE(nr_dci11_bank_service(B, r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_DEFERRED);       // B waits first
  EXPECT_TRUE(nr_dci11_bank_service(C, r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_DEFERRED);       // C waits second
  g_fake_t += 1100000000ull;                                            // a fresh window
  EXPECT_EQ(nr_dci11_bank_service(C, r, nullptr, ul_dddsu, &tot), 0u);   // budget is there, but B has waited longer
  EXPECT_TRUE(nr_dci11_bank_due(C));
  EXPECT_TRUE(nr_dci11_bank_service(B, r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);
  EXPECT_FALSE(nr_dci11_bank_due(B));
  EXPECT_EQ(nr_dci11_bank_service(C, r, nullptr, ul_dddsu, &tot), 0u);   // B used the window's share
  EXPECT_TRUE(nr_dci11_bank_due(C));
  g_fake_t += 1100000000ull;
  EXPECT_TRUE(nr_dci11_bank_service(C, r, nullptr, ul_dddsu, &tot) & NR_DCI11_SVC_CHANGED);
  EXPECT_FALSE(nr_dci11_bank_due(C));
  free(r);
  nr_dci11_set_clock(nullptr);
  nr_dci11_budget_reset();
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, ChangedClassSetIsRecomputedOnlyWhenItChanges)
{
  // the cache key is (resolver, n_hyp, n_alive, n_truncated): another alive count is a fresh scan, the same one is not
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  Cfg c13, c14; c14.k1 = 3;
  nr_dci11_layout_t l[2]; nr_dci11_offsets_t o[2];
  make_layout(c13, &l[0], &o[0]); make_layout(c14, &l[1], &o[1]);
  r->n_hyp = 2; r->n_alive = 2;
  for (int i = 0; i < 2; i++) { r->hyp[i] = l[i]; r->off[i] = o[i]; r->alive[i] = true; }
  nr_dci11_class_t cl[NR_DCI11_CLASS_MAX];
  ASSERT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 2);
  r->alive[1] = false;   // n_alive NOT updated: the cache (keyed on the counts) still answers 2, as the resolver always moves n_alive with alive[]
  EXPECT_EQ(nr_dci11_class_set(r, cl, false, nullptr), 2);
  r->n_alive = 1;
  EXPECT_EQ(nr_dci11_class_set(r, cl, false, nullptr), NR_DCI11_CLS_NONE);   // the key moved: not examined yet
  EXPECT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 1);
  free(r);
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, DuplicateSlotsAreIgnored)
{
  // the same DCI seen twice in one slot (two passes / candidates) is one observation: a doubled bank proves exactly what the plain one does
  std::mt19937 rng(24);
  Cfg c;
  const auto v = gen_stream(c, 32, rng);
  std::vector<Dci> dbl;
  for (const Dci &d : v) { dbl.push_back(d); dbl.push_back(d); }
  const Res r1 = run(c, v), r2 = run(c, dbl);
  ASSERT_EQ(r1.alive.size(), r2.alive.size());
  for (size_t i = 0; i < r1.alive.size(); i++) { EXPECT_EQ(0, memcmp(&r1.alive[i], &r2.alive[i], sizeof(nr_dci11_split_t))); EXPECT_EQ(r1.p[i], r2.p[i]); }
}

TEST(Dci11Prove, ProvesOnlyAfterSixteenMore)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(8);
  Cfg a;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const auto v = gen_stream(a, 90, rng);
  // precondition: this stream IS provable (direct prove of the first 64 DCIs)
  {
    std::vector<Dci> v64(v.begin(), v.begin() + 64);
    ASSERT_EQ(run(a, v64).alive.size(), 1u);
  }
  const uint16_t rnti = 0x4602;
  for (int i = 0; i < 15; i++) nr_dci11_bank_feed(rnti, v[i].p, (uint16_t)o.total, v[i].slot, &FEED);
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));   // 15 < 16: not due
  nr_dci11_split_t s{};
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  nr_dci11_bank_feed(rnti, v[15].p, (uint16_t)o.total, v[15].slot, &FEED);
  for (int i = 16; i < 64; i++) nr_dci11_bank_feed(rnti, v[i].p, (uint16_t)o.total, v[i].slot, &FEED);
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));    // due (>= 16 since the start): the proof exists now
  EXPECT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));   // nothing new since: not due, no change
  for (int i = 64; i < 79; i++) nr_dci11_bank_feed(rnti, v[i].p, (uint16_t)o.total, v[i].slot, &FEED);
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));   // 15 more: not due, still proven
  EXPECT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, PerCellBanks)
{
  std::mt19937 rng(9);
  Cfg a;                                    // cell 0: DAI 2 + K1 2 (4 entries)
  Cfg b; b.dai = 0; b.tpc2 = 2; b.k1 = 2;   // cell 1: same sum, DAI 0 + 2nd TPC 2
  ASSERT_EQ(a.pre_ant(), b.pre_ant());
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const uint16_t rnti = 0x4601;
  nr_cell_bind(0); nr_dci11_bank_reset();
  nr_cell_bind(1); nr_dci11_bank_reset();
  nr_cell_bind(0); feed_all(rnti, gen_stream(a, 64, rng), o.total); ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  nr_cell_bind(1); feed_all(rnti, gen_stream(b, 64, rng), o.total);
  nr_cell_bind(0);
  nr_dci11_split_t s0{}, s1{};
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s0));
  EXPECT_EQ(s0.dai, 2); EXPECT_EQ(s0.tpc2, 0);
  nr_cell_bind(1);
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s1));   // cell 1 never proved: cell 0's proof must not leak
  nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu);
  // the cell-1 stream (DAI 0, 2nd TPC 2) either proves its own split or stays open, but never cell 0's
  if (nr_dci11_bank_proven(rnti, &l, &o, &s1)) { EXPECT_EQ(s1.dai, 0); EXPECT_EQ(s1.tpc2, 2); }
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_cell_bind(1);
  nr_dci11_bank_reset();
  nr_cell_bind(0);
}

TEST(Dci11Prove, IdentityChangeAndSilenceDropTheRing)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(22);
  Cfg a;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const nr_dci11_class_t kc = cls_of(l, o);
  const uint16_t rnti = 0x4604;
  const auto v = gen_stream(a, 64, rng);
  nr_dci11_split_t s{};
  feed_all(rnti, v, o.total);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  // another cell identity: the next feed wipes every bank and proof of the cell (and nothing is due in the fresh banks)
  const nr_dci11_feed_t other = {2, 1000000000, 1000000000};
  nr_dci11_bank_feed(0x4999, v[0].p, (uint16_t)o.total, v[0].slot, &other);
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  EXPECT_FALSE(nr_dci11_bank_due(rnti));
  // silence longer than max_gap drops the ring of that RNTI (and its proof); the new DCI starts a fresh ring of 1
  nr_dci11_bank_reset();
  const nr_dci11_feed_t gap = {1, 100, 50};
  for (const Dci &d : v) nr_dci11_bank_feed(rnti, d.p, (uint16_t)o.total, d.slot, &FEED);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  nr_dci11_bank_feed(rnti, v[0].p, (uint16_t)o.total, v.back().slot + 1000, &gap);   // 1000 > 100 slots of silence
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  for (int i = 1; i < 16; i++) nr_dci11_bank_feed(rnti, v[i].p, (uint16_t)o.total, v.back().slot + 1000 + i, &gap);
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));   // only 16 in the new ring and nothing to prove yet
  // a backwards jump (more than max_back) also drops it; a small reorder (across scan consumers) does not
  const nr_dci11_feed_t back = {1, 1000000000, 50};
  nr_dci11_bank_reset();
  for (const Dci &d : v) nr_dci11_bank_feed(rnti, d.p, (uint16_t)o.total, d.slot, &back);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, &kc, 1, ul_dddsu));
  nr_dci11_bank_feed(rnti, v[0].p, (uint16_t)o.total, v.back().slot - 10, &back);
  EXPECT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  nr_dci11_bank_feed(rnti, v[0].p, (uint16_t)o.total, v.back().slot - 500, &back);
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, TddSlotUnwrapsFrames)
{
  const int spf = 20;   // 30 kHz
  const int64_t w = 1024 * spf;
  EXPECT_EQ(nr_dci11_tdd_slot(5000, 250, 0, spf), 5000);           // frame 250 slot 0 = 5000: already in the pattern's numbering
  EXPECT_EQ(nr_dci11_tdd_slot(5007, 250, 0, spf), 5000);            // mono ahead of the frame count by 7 slots (d > w/2): back, not a whole wrap forward
  EXPECT_EQ(nr_dci11_tdd_slot(5000 + w, 250, 0, spf), 5000 + w);   // one wrap later the monotonic value stays next to mono, same residue
  EXPECT_EQ(nr_dci11_tdd_slot(5003 + 3 * w, 250, 3, spf) % 5, 5003 % 5);
  // mono offset by a non-multiple of the wrap (counter started mid-run): result is the value congruent to frame*spf+slot nearest to mono
  const int64_t r = nr_dci11_tdd_slot(123457, 100, 7, spf);
  EXPECT_EQ((r - (100 * spf + 7)) % w, 0);
  EXPECT_LE(std::llabs(r - 123457), w / 2);
  // differences between two DCIs are those of mono
  EXPECT_EQ(nr_dci11_tdd_slot(123460, 100, 10, spf) - r, 3);
  // wrap of the frame counter between two DCIs (1023 -> 0): still +1 slot
  const int64_t a = nr_dci11_tdd_slot(w * 7 + 1023 * spf + 19, 1023, 19, spf), b = nr_dci11_tdd_slot(w * 7 + 1024 * spf, 0, 0, spf);
  EXPECT_EQ(b - a, 1);
  EXPECT_EQ(nr_dci11_tdd_slot(-1, 0, 0, spf), -1);
  EXPECT_EQ(nr_dci11_tdd_slot(10, 1024, 0, spf), -1);
  EXPECT_EQ(nr_dci11_tdd_slot(10, 0, 20, spf), -1);
  EXPECT_EQ(nr_dci11_tdd_slot(10, 0, 0, 0), -1);
}

TEST(Dci11Prove, ClassesAreDistinctReadings)
{
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  nr_dci11_layout_t l[3]; nr_dci11_offsets_t o[3];
  Cfg c13; Cfg c14; c14.k1 = 3;
  make_layout(c13, &l[0], &o[0]);
  l[1] = l[0]; o[1] = o[0]; l[1].tail = 1; o[1].total = o[0].total;   // differs only where the prover does not read: same class
  make_layout(c14, &l[2], &o[2]);
  r->n_hyp = 3;
  for (int i = 0; i < 3; i++) { r->hyp[i] = l[i]; r->off[i] = o[i]; r->alive[i] = true; }
  nr_dci11_class_t out[4];
  ASSERT_EQ(nr_dci11_classes(r, out, 4), 2);
  r->alive[2] = false;
  EXPECT_EQ(nr_dci11_classes(r, out, 4), 1);
  r->alive[0] = r->alive[1] = false;
  EXPECT_EQ(nr_dci11_classes(r, out, 4), 0);
  r->alive[0] = r->alive[2] = true;
  EXPECT_EQ(nr_dci11_classes(r, out, 1), -1);   // more classes than the cap: the caller skips the round
  EXPECT_EQ(nr_dci11_classes(nullptr, out, 1), -1);
  free(r);
}

TEST(Dci11Prove, UnsolvedRntiProvenOverSeveralClasses)
{
  // the resolver still holds two classes of one DCI length: TDRA 4 bits + no VRB bit, or TDRA 3 bits + one pre-MCS bit (same MCS offset). Both read the
  // same HARQ-ACK bits: one distinct answer over both classes, proven. A third class with another pre_ant and the same length disagrees: no proof.
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(23);
  Cfg c13;
  nr_dci11_layout_t la, lb, lc; nr_dci11_offsets_t oa, ob, oc;
  make_layout(c13, &la, &oa);
  lb = la; lb.pre_mcs = 1;
  ASSERT_TRUE(nr_dci11_layout_offsets(&lb, 11, 3, &ob));
  ASSERT_EQ(ob.total, oa.total);
  lc = la; lc.pre_ant = 14; lc.tail = 0; lc.pre_mcs = 0;
  ASSERT_TRUE(nr_dci11_layout_offsets(&lc, 11, 3, &oc));   // one bit longer pre_ant, one bit shorter TDRA
  ASSERT_EQ(oc.total, oa.total);
  const uint16_t rnti = 0x4605;
  const auto v = gen_stream(c13, 64, rng);
  nr_dci11_class_t two[2] = {cls_of(la, oa), cls_of(lb, ob)}, three[3] = {two[0], two[1], cls_of(lc, oc)};
  nr_dci11_class_t pc; nr_dci11_split_t ps;
  for (const Dci &d : v) nr_dci11_bank_feed(rnti, d.p, (uint16_t)oa.total, d.slot, &FEED);
  ASSERT_TRUE(nr_dci11_bank_maybe_prove(rnti, two, 2, ul_dddsu));
  ASSERT_TRUE(nr_dci11_bank_proven_any(rnti, &pc, &ps));
  EXPECT_EQ(ps.dai, 2); EXPECT_EQ(ps.k1, 2); EXPECT_EQ(ps.harq, 4);
  // the proof belongs to its class: asked for under a layout of another class (pre_ant 14) it is not returned
  nr_dci11_split_t other{};
  EXPECT_TRUE(nr_dci11_bank_proven(rnti, &pc.l, &pc.o, &other));
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &lc, &oc, &other));
  nr_dci11_bank_reset();
  for (const Dci &d : v) nr_dci11_bank_feed(rnti, d.p, (uint16_t)oa.total, d.slot, &FEED);
  nr_dci11_bank_maybe_prove(rnti, three, 3, ul_dddsu);
  // class c reads bits one position off (pre_ant 14 starts a bit earlier): whatever it keeps alive must be a different reading, so no proof
  const bool p3 = nr_dci11_bank_proven_any(rnti, &pc, &ps);
  if (p3) { EXPECT_EQ(ps.dai, 2); EXPECT_EQ(ps.k1, 2); EXPECT_TRUE(nr_dci11_class_same(&pc.l, &pc.o, &la, &oa) || nr_dci11_class_same(&pc.l, &pc.o, &lb, &ob)); }
  nr_dci11_bank_reset();
}

TEST(Dci11Prove, ProvenSplitFeedsFieldExtraction)
{
  // pre_ant 13 is ambiguous for the unique-split rule (false, all -1); a proven split reads the fields
  std::mt19937 rng(10);
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  const Res r = run(c, v);
  ASSERT_EQ(r.alive.size(), 1u);
  nr_dci_harq_fields_t f;
  EXPECT_FALSE(nr_dci11_harq_fields(v[0].p, o.total, &l, &o, &f));
  EXPECT_EQ(f.dai, -1); EXPECT_EQ(f.k1, -1);
  const int base = o.ant_ports - l.pre_ant;
  for (size_t i = 0; i < 8; i++) {
    ASSERT_TRUE(nr_dci11_harq_fields_proven(v[i].p, o.total, &l, &o, &r.alive[0], &f));
    EXPECT_EQ(f.dai, (int)nr_dci_bits_field(&v[i].p, o.total, base + 4, 2));
    EXPECT_EQ(f.tpc, (int)nr_dci_bits_field(&v[i].p, o.total, base + 6, 2));
    EXPECT_EQ(f.pri, (int)nr_dci_bits_field(&v[i].p, o.total, base + 8, 3));
    EXPECT_EQ(f.k1, (int)nr_dci_bits_field(&v[i].p, o.total, base + 11, 2));
  }
  // a split that does not add up to this layout is refused
  nr_dci11_split_t bad = r.alive[0]; bad.k1 = 3;
  EXPECT_FALSE(nr_dci11_harq_fields_proven(v[0].p, o.total, &l, &o, &bad, &f));
  // NULL proven = the unique-split rule, as before
  EXPECT_EQ(nr_dci11_harq_fields_proven(v[0].p, o.total, &l, &o, nullptr, &f), nr_dci11_harq_fields(v[0].p, o.total, &l, &o, &f));
}

TEST(Dci11Prove, Mcs28ExcludesQam256)
{
  Cfg c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  nr_dci11_split_t s{}; s.harq = 4; s.dai = 2; s.tpc = 2; s.pri = 3; s.k1 = 2;
  const int base = o.ant_ports - l.pre_ant;
  auto mk = [&](int pid, int ndi, int mcs, int64_t slot) {
    nr_dci_bits_t p{};
    put(p, o.total, o.mcs, 5, (unsigned)mcs); put(p, o.total, o.mcs + 5, 1, (unsigned)ndi); put(p, o.total, base, 4, (unsigned)pid);
    return nr_dci11_obs_t{p, (uint16_t)o.total, slot};
  };
  // pid 3: NDI 0 (first sighting), NDI 1 with MCS 28 = new TB with MCS 28 -> excludes table 2
  std::vector<nr_dci11_obs_t> b = {mk(3, 0, 10, 100), mk(3, 1, 28, 101)};
  EXPECT_EQ(nr_dci11_mcs28_new_tb(&l, &o, b.data(), 2, &s), 1);
  // same NDI = retransmission (28 is legal there for table 2): no evidence
  b = {mk(3, 0, 10, 100), mk(3, 0, 28, 101)};
  EXPECT_EQ(nr_dci11_mcs28_new_tb(&l, &o, b.data(), 2, &s), 0);
  // toggled but MCS 27: valid in table 2, no evidence
  b = {mk(3, 0, 10, 100), mk(3, 1, 27, 101)};
  EXPECT_EQ(nr_dci11_mcs28_new_tb(&l, &o, b.data(), 2, &s), 0);
  // first sighting of a pid is not a toggle; another pid's NDI is not compared
  b = {mk(3, 0, 10, 100), mk(5, 1, 28, 101)};
  EXPECT_EQ(nr_dci11_mcs28_new_tb(&l, &o, b.data(), 2, &s), 0);
  // out-of-order bank: slot order decides
  b = {mk(3, 1, 28, 101), mk(3, 0, 10, 100)};
  EXPECT_EQ(nr_dci11_mcs28_new_tb(&l, &o, b.data(), 2, &s), 1);
  EXPECT_EQ(nr_dci11_mcs28_new_tb(nullptr, &o, b.data(), 2, &s), -1);
}

TEST(Dci11Prove, ArbitraryPidsRefuteNobody)
{
  // a scheduler without the +1..3 pid habit: the HARQ test is uninformative and must not refute the truth
  std::mt19937 rng(11);
  Cfg c; c.random_pid = true;
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  const Res r = run(c, v);
  bool truth = false;
  for (const auto &s : r.alive) truth |= is_truth(s, c);
  EXPECT_TRUE(truth);
}

// Soundness sweep: random configs and schedulers. The truth must never be refuted (only a missing signal may leave several alive), and
// a "proven" verdict (one survivor, chance_p < alpha) must always be the truth. Also reports how often the proof is reached.
TEST(Dci11Prove, SweepNeverProvesWrongSplit)
{
  int cases = 0, proven = 0, truth_lost = 0, wrong = 0, cases0 = 0, proven0 = 0;
  for (int seed = 100; seed < 400; seed++) {
    std::mt19937 rng(seed);
    Cfg c;
    c.tb2 = (rng() % 4 == 0) ? 8 : 0;
    c.harq = (rng() % 4 == 0) ? 5 : 4;
    const int dw[] = {0, 2, 4, 6};
    c.dai = dw[rng() % 4];
    c.tpc2 = (rng() % 3 == 0) ? 2 : 0;
    c.k1 = (int)(rng() % 4);
    c.ext = (int)(rng() % 4);
    static const std::vector<int> Ls[] = {{4}, {4, 3}, {1, 2, 3, 4}, {1, 2, 3, 4, 6, 7, 8, 9}};
    c.L = Ls[c.k1];
    c.k0_hi = (int)(rng() % 3);
    c.random_pid = rng() % 5 == 0;
    c.random_fb = rng() % 3 == 0;
    if (c.pre_ant() > 63) continue;
    const int n = 16 + (int)(rng() % 49);
    const auto v = gen_stream(c, n, rng);
    const Res r = run(c, v);
    cases++;
    cases0 += c.ext == 0;
    bool truth = false;
    for (const auto &s : r.alive) truth |= is_truth(s, c);
    if (!truth) { truth_lost++; ADD_FAILURE() << "seed " << seed << " truth refuted: " << dump(r); }
    if (r.alive.size() == 1 && r.p[0] < 1e-3) { proven++; proven0 += c.ext == 0; if (!is_truth(r.alive[0], c)) { wrong++; ADD_FAILURE() << "seed " << seed << " WRONG proof " << dump(r); } }
  }
  printf("sweep: %d cases, %d proven (ext 0: %d of %d), %d truth refuted, %d wrong proofs\n", cases, proven, proven0, cases0, truth_lost, wrong);
  EXPECT_EQ(truth_lost, 0);
  EXPECT_EQ(wrong, 0);
  EXPECT_GT(proven0, 0);
}

// ---- realistic sweep: what a passive receiver really sees ----
// Per-DCI loss 0-30 %, DCI 1_0 of the same RNTI interleaved (they take counter-DAI values and HARQ pids but are not in the bank), a lowest-free-pid
// scheduler or a round-robin one, 1 % corrupted payloads, 0.5 % SPS-release-like DCIs (pid 0, K1 field arbitrary), FLEXIBLE slots as PUCCH slots.
static std::vector<Dci> gen_real(const Real &R, int n_kept, std::mt19937 &rng, int64_t first_slot)
{
  const Cfg &c = R.c;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const int base = o.ant_ports - l.pre_ant, len = o.total;
  std::uniform_real_distribution<double> U(0.0, 1.0);
  std::vector<Dci> out;
  int pid = 0, ndi[32] = {0};
  int64_t busy[32] = {0};
  std::map<int64_t, int> win;
  for (int64_t s = first_slot; (int)out.size() < n_kept && s < first_slot + 200000; s++) {
    if (s % 5 == 4 || U(rng) > 0.5) continue;
    const bool is10 = U(rng) < R.p10;
    const int tda = (int)(rng() % 4);
    const int64_t m = s + (tda >= 2 ? c.k0_hi : 0);
    int bi = -1, bc = 99;
    for (size_t i = 0; i < c.L.size(); i++) {
      const int r = (int)((m + c.L[i]) % 5);
      if (((R.pucch_mask >> r) & 1) && c.L[i] < bc) { bc = c.L[i]; bi = (int)i; }
    }
    if (bi < 0) continue;
    const int64_t f = m + bc;
    int p;
    if (R.lowest_free) {
      p = -1;
      for (int q = 0; q < (1 << c.harq); q++) if (busy[q] <= s) { p = q; break; }
      if (p < 0) continue;
      busy[p] = f + 2;
    } else {
      pid = (pid + 1) & ((1 << c.harq) - 1);
      p = pid;
    }
    ndi[p] ^= 1;
    const unsigned dai = (unsigned)((win[f]++) & 3);   // every DCI of the UE in this window, 1_0 and 1_1, lost or not
    if (is10 || U(rng) < R.loss) continue;
    nr_dci_bits_t pl{};
    for (int i = 0; i < len; i++) put(pl, len, i, 1, rng() & 1);
    const double x = U(rng);
    if (x < R.corrupt) { out.push_back({pl, s}); continue; }     // every bit random
    put(pl, len, o.tda, o.tda_bits, (unsigned)tda);
    put(pl, len, o.mcs, 5, (unsigned)(rng() % 28)); put(pl, len, o.mcs + 5, 1, (unsigned)ndi[p]);
    int pos = base + c.tb2;
    const bool sps = x < R.corrupt + R.sps;
    put(pl, len, pos, c.harq, sps ? 0u : (unsigned)p); pos += c.harq;
    if (c.dai) put(pl, len, pos, 2, dai);
    pos += c.dai + 2 + c.tpc2 + 3;
    if (c.k1) put(pl, len, pos, c.k1, sps ? (unsigned)(rng() % (1u << c.k1)) : (unsigned)bi);
    out.push_back({pl, s});
  }
  return out;
}

TEST(Dci11Prove, SweepRealistic)
{
  int cases = 0, proven = 0, truth_lost = 0, wrong = 0, cases0 = 0, proven0 = 0;
  const int N = 1200;
  for (int seed = 1000; seed < 1000 + N; seed++) {
    std::mt19937 rng(seed);
    Real R;
    Cfg &c = R.c;
    c.tb2 = (rng() % 4 == 0) ? 8 : 0;
    c.harq = (rng() % 4 == 0) ? 5 : 4;
    const int dw[] = {0, 2, 2, 4};
    c.dai = dw[rng() % 4];
    c.tpc2 = (rng() % 4 == 0) ? 2 : 0;
    c.k1 = 1 + (int)(rng() % 3);
    c.ext = (rng() % 3 == 0) ? (int)(rng() % 3) : 0;
    static const std::vector<int> Ls[] = {{4}, {4, 3}, {1, 2, 3, 4}, {1, 2, 3, 4, 6, 7, 8, 9}};
    c.L = Ls[c.k1];
    c.k0_hi = (int)(rng() % 3);
    R.loss = 0.3 * (double)(rng() % 1000) / 1000.0;
    R.p10 = 0.3 * (double)(rng() % 1000) / 1000.0;
    R.lowest_free = rng() % 2;
    // UL-capable residues: DDDSU (3,4), with the FLEXIBLE slot 2 (2,3,4), or the special slot absent (4); PUCCH on a subset of them
    const int masks[] = {0x18, 0x1C, 0x10};
    g_ulmask = masks[rng() % 3];
    R.pucch_mask = (rng() % 2) ? 0x10 : g_ulmask;
    if (c.pre_ant() > 63) continue;
    const int n = 16 + (int)(rng() % 49);
    const auto v = gen_real(R, n, rng);
    if ((int)v.size() < 16) { g_ulmask = 0x18; continue; }
    const Res r = run(c, v);
    g_ulmask = 0x18;
    cases++;
    cases0 += c.ext == 0;
    bool truth = false;
    for (const auto &s : r.alive) truth |= is_truth(s, c);
    if (!truth) { truth_lost++; ADD_FAILURE() << "seed " << seed << " truth refuted: " << dump(r); }
    if (r.alive.size() == 1 && r.p[0] < 1e-3) { proven++; proven0 += c.ext == 0; if (!is_truth(r.alive[0], c)) { wrong++; ADD_FAILURE() << "seed " << seed << " WRONG proof " << dump(r); } }
  }
  g_ulmask = 0x18;
  printf("realistic sweep: %d configs, %d proven (%.1f %%; ext 0: %d of %d), %d truth refuted, %d wrong proofs\n", cases, proven, 100.0 * proven / cases, proven0, cases0,
         truth_lost, wrong);
  EXPECT_GE(cases, 1000);
  EXPECT_EQ(truth_lost, 0);
  EXPECT_EQ(wrong, 0);
}

TEST(Dci11Prove, BenchProveCost)
{
  // CPU on the scan consumer (64 DCIs): (a) one class, (b) the 16-class cap through the bank, (c) per ACCEPTED 1_1 when no proof is due (feed + due test + cached
  // class-set test), (d) worst-case class scan: 65535 alive layouts, 16 classes / 1 class
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(31);
  Cfg c; c.dai = 4; c.k1 = 3; c.L = {1, 2, 3, 4, 6, 7, 8, 9};
  const auto v = gen_stream(c, NR_DCI11_BANK, rng);
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(c, &l, &o);
  const auto b = obs_of(v, o.total);
  nr_dci11_split_t al[NR_DCI11_CORE_MAX]; double cp[NR_DCI11_CORE_MAX];
  auto now = [] { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; };
  const int reps = 200;
  double t0 = now();
  int na = 0;
  for (int i = 0; i < reps; i++) na += nr_dci11_prove(&l, &o, b.data(), (int)b.size(), ul_dddsu, al, cp, NR_DCI11_CORE_MAX);
  const double one = (now() - t0) / reps;
  EXPECT_GT(na, 0);
  // (b) 16 classes
  nr_dci11_class_t cl[16];
  for (int i = 0; i < 16; i++) { cl[i] = cls_of(l, o); cl[i].o.mcs = (uint16_t)(o.mcs + i); }
  const uint16_t rnti = 0x4610;
  double sum16 = 0;
  const int r16 = 50;
  for (int i = 0; i < r16; i++) {
    nr_dci11_bank_reset();
    feed_all(rnti, v, o.total);
    const double a0 = now();
    nr_dci11_bank_maybe_prove(rnti, cl, 16, ul_dddsu);
    sum16 += now() - a0;
  }
  const double p16 = sum16 / r16;
  // (c) per accepted 1_1 when not due
  nr_dci11_resolver_t *r = (nr_dci11_resolver_t *)calloc(1, sizeof *r);
  const int N = 65535;
  for (int i = 0; i < N; i++) { r->hyp[i] = l; r->off[i] = o; r->off[i].mcs = (uint16_t)(o.mcs + (i % 16)); r->alive[i] = true; }
  r->n_hyp = N; r->n_alive = N;
  const double s0 = now();
  ASSERT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 16);
  const double scan16 = now() - s0;
  for (int i = 0; i < N; i++) r->off[i].mcs = o.mcs;
  r->n_alive = N - 1;
  const double s1 = now();
  ASSERT_EQ(nr_dci11_class_set(r, cl, true, nullptr), 1);
  const double scan1 = now() - s1;
  nr_dci11_bank_reset();
  const nr_dci11_feed_t fd = {1, 1000000000, 1000000000};
  const int acc = 200000;
  const double c0 = now();
  int due = 0;
  for (int i = 0; i < acc; i++) {
    nr_dci11_bank_feed(rnti, v[i % 64].p, (uint16_t)o.total, 1000 + i, &fd);
    due += nr_dci11_bank_due(rnti);                      // true from the 16th feed on (nothing proves here): the test costs the same either way
    due += nr_dci11_class_set(r, nullptr, false, nullptr) > 0;
  }
  const double per_accept = (now() - c0) * 1000.0 / acc;
  printf("BENCH one class %.1f us | 16 classes via bank %.1f us | per accepted 1_1 (feed+due+cached class set) %.0f ns | class scan of 65535 alive layouts: 16 classes %.0f us, 1 class %.0f us (%d)\n",
         one, p16, per_accept, scan16, scan1, due > 0);
  free(r);
  nr_dci11_bank_reset();
  EXPECT_LT(one, 20000.0);
}
