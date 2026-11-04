// E1: CRC-free proof of the DCI 1_1 pre_ant split. Synthetic scheduler streams: a known RRC config (widths), HARQ pids, a counter DAI per
// HARQ-ACK window, K1 from a list, TDD pattern DDDSU (slot 3 = special = MIXED = UL-capable, slot 4 = UL).
#include "nr_dci11_prove.h"
#include "nr_traffic_dci_fields.h"
#include "common/utils/nr_cell.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

static bool g_known = true;
static bool ul_dddsu(int64_t s, int *known)
{
  *known = g_known ? 1 : 0;
  const int r = (int)(((s % 5) + 5) % 5);
  return r == 3 || r == 4;
}

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
  for (const Dci &d : v) nr_dci11_bank_feed(rnti, d.p, (uint16_t)len, d.slot);
}

TEST(Dci11Prove, ReopensOnViolations)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(7);
  Cfg a;                                   // DAI 2 + K1 2
  Cfg b; b.dai = 4; b.k1 = 0; b.L = {4};   // same sum (13), a different RRC config
  ASSERT_EQ(a.pre_ant(), b.pre_ant());
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const uint16_t rnti = 0x4601;
  feed_all(rnti, gen_stream(a, 64, rng), o.total);
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu));
  nr_dci11_split_t s{};
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s));
  EXPECT_EQ(s.dai, 2); EXPECT_EQ(s.k1, 2);
  // 64 DCIs of the other config replace the ring: re-proving refutes the old split
  feed_all(rnti, gen_stream(b, 64, rng, 5000), o.total);
  EXPECT_TRUE(nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu));
  nr_dci11_split_t s2{};
  if (nr_dci11_bank_proven(rnti, &l, &o, &s2)) { EXPECT_FALSE(s2.dai == 2 && s2.k1 == 2); }
  // reset forgets everything
  nr_dci11_bank_reset();
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s2));
}

TEST(Dci11Prove, ProvesOnlyAfterSixteenMore)
{
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  std::mt19937 rng(8);
  Cfg a;
  nr_dci11_layout_t l; nr_dci11_offsets_t o;
  make_layout(a, &l, &o);
  const auto v = gen_stream(a, 64, rng);
  const uint16_t rnti = 0x4602;
  for (int i = 0; i < 15; i++) nr_dci11_bank_feed(rnti, v[i].p, (uint16_t)o.total, v[i].slot);
  EXPECT_FALSE(nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu));   // 15 < 16: not due
  nr_dci11_bank_feed(rnti, v[15].p, (uint16_t)o.total, v[15].slot);
  nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu);                 // due now (16)
  const bool before = nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu);   // nothing new since
  EXPECT_FALSE(before);
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
  const uint16_t rnti = 0x4601;
  nr_cell_bind(0); nr_dci11_bank_reset();
  nr_cell_bind(1); nr_dci11_bank_reset();
  nr_cell_bind(0); feed_all(rnti, gen_stream(a, 64, rng), o.total); nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu);
  nr_cell_bind(1); feed_all(rnti, gen_stream(b, 64, rng), o.total);
  nr_cell_bind(0);
  nr_dci11_split_t s0{}, s1{};
  ASSERT_TRUE(nr_dci11_bank_proven(rnti, &l, &o, &s0));
  EXPECT_EQ(s0.dai, 2); EXPECT_EQ(s0.tpc2, 0);
  nr_cell_bind(1);
  EXPECT_FALSE(nr_dci11_bank_proven(rnti, &l, &o, &s1));   // cell 1 never proved: cell 0's proof must not leak
  nr_dci11_bank_maybe_prove(rnti, &l, &o, ul_dddsu);
  // the cell-1 stream (DAI 0, 2nd TPC 2) either proves its own split or stays open, but never cell 0's
  if (nr_dci11_bank_proven(rnti, &l, &o, &s1)) { EXPECT_EQ(s1.dai, 0); EXPECT_EQ(s1.tpc2, 2); }
  nr_cell_bind(0);
  nr_dci11_bank_reset();
  nr_cell_bind(1);
  nr_dci11_bank_reset();
  nr_cell_bind(0);
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
