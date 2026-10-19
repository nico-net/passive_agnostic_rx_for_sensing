// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Fast lane ordering (nr_pdcch_fast_lane.h): partition properties, and a state-hash comparison of a model of the Phase-2 gates run in the
// current order versus the fast-lane order on synthetic slots. The model carries exactly the order-sensitive state the real Phase 2 touches
// (mismatch EMA, per-RNTI persistence, (slot,rnti,dir) dedupe, per-occasion decode cap, UL bookkeeping); see the report for the code reading
// that fixes which task classes touch which state.
#include <gtest/gtest.h>
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>
#include <vector>
#include "../nr_pdcch_fast_lane.h"

namespace {
struct T {
  nr_fl_task_t f;
  bool ok;        // CRC passed
  int rnti;       // CRC-recovered RNTI
  int mm;         // mismatched bits
  bool exempt;    // SI / TC / RA class: exempt from the persistence gate
  int id;
};
struct State {
  int thres = 0;
  std::map<int, std::deque<long>> seen;
  std::set<std::tuple<long, int, int>> dedupe;
  std::vector<std::tuple<long, int, int>> grants;  // enqueue order is NOT part of the hash: consumers take a slot's grants as a set
  long held_mm = 0, held_persist = 0, dup = 0, over_cap = 0, ul_seen = 0;
  uint64_t hash() const
  {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(thres);
    for (auto &kv : seen) { mix(kv.first); for (long s : kv.second) mix((uint64_t)s); }
    for (auto &d : dedupe) { mix(std::get<0>(d)); mix(std::get<1>(d)); mix(std::get<2>(d)); }
    std::vector<std::tuple<long, int, int>> g = grants;
    std::sort(g.begin(), g.end());
    for (auto &x : g) { mix(std::get<0>(x)); mix(std::get<1>(x)); mix(std::get<2>(x)); }
    mix(held_mm); mix(held_persist); mix(dup); mix(over_cap); mix(ul_seen);
    return h;
  }
};
const int kK = 2, kWindow = 40, kCap = 16;

void step(State &st, const T &t, long slot, int &decodes)
{
  if (!t.ok) return;
  if (t.f.ul_scan) { // UL path: own dedupe direction, nothing of the DL gates
    st.ul_seen++;
    st.dedupe.insert({slot, t.rnti, 1});
    return;
  }
  if (!t.f.dl_auto || t.f.fmt10) {  // Gate 1.5: EMA, and the hold
    st.thres = (st.thres + t.mm) / 2;
    if (!t.exempt && t.mm > st.thres + 30) { st.held_mm++; return; }
  }
  if (!t.exempt) {  // Gate 2: persistence
    auto &q = st.seen[t.rnti];
    while (!q.empty() && slot - q.front() > kWindow) q.pop_front();
    q.push_back(slot);
    if ((int)q.size() < kK) { st.held_persist++; return; }
  }
  if (!st.dedupe.insert({slot, t.rnti, 0}).second) { st.dup++; return; }  // accept_dup
  if (decodes >= kCap) { st.over_cap++; return; }
  decodes++;
  st.grants.push_back({slot, t.rnti, 0});
}
}  // namespace

TEST(FastLane, PredicateAndStablePartition)
{
  nr_fl_task_t known{.dl_auto = true, .rnti_min = 0x4601, .rnti_max = 0x4601};
  nr_fl_task_t open = known; open.rnti_min = 1; open.rnti_max = 0xfff3; open.open_rnti = true;
  nr_fl_task_t f10 = known; f10.fmt10 = true;
  nr_fl_task_t ul = known; ul.ul_scan = true;
  nr_fl_task_t la = known; la.is_lookahead = true;
  nr_fl_task_t probe = known; probe.bwp_probe = true;
  nr_fl_task_t otherbwp = known; otherbwp.bwp_entry = 1;
  nr_fl_task_t k2 = known; k2.rnti_min = k2.rnti_max = 0x4602;
  EXPECT_TRUE(nr_fl_is_fast(&known, false));
  EXPECT_FALSE(nr_fl_is_fast(&open, false));
  EXPECT_FALSE(nr_fl_is_fast(&f10, false));
  EXPECT_FALSE(nr_fl_is_fast(&ul, false));
  EXPECT_FALSE(nr_fl_is_fast(&la, false));
  EXPECT_FALSE(nr_fl_is_fast(&probe, false));
  EXPECT_FALSE(nr_fl_is_fast(&otherbwp, false));
  // common-search-space occasion: every DL task, 1_0 and open range included; never UL / lookahead / probes
  EXPECT_TRUE(nr_fl_is_fast(&open, true));
  EXPECT_TRUE(nr_fl_is_fast(&f10, true));
  EXPECT_FALSE(nr_fl_is_fast(&ul, true));
  EXPECT_FALSE(nr_fl_is_fast(&la, true));
  const nr_fl_task_t ts[] = {open, known, ul, k2, f10, known, la};
  int ord[7];
  const int nf = nr_fl_partition(ts, 7, false, ord);
  EXPECT_EQ(nf, 3);
  const int want[] = {1, 3, 5, 0, 2, 4, 6};  // fast in order, then the rest in order
  for (int i = 0; i < 7; i++) EXPECT_EQ(ord[i], want[i]);
}

TEST(FastLane, PartitionIsAPermutationForRandomSlots)
{
  uint64_t r = 99;
  auto rnd = [&]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return (uint32_t)(r >> 8); };
  for (int it = 0; it < 2000; it++) {
    const int n = 1 + rnd() % 60;
    std::vector<nr_fl_task_t> t(n);
    for (auto &x : t) {
      x = {};
      x.dl_auto = rnd() & 1; x.open_rnti = !(rnd() % 4); x.fmt10 = !(rnd() % 5); x.ul_scan = !(rnd() % 4);
      x.is_lookahead = !(rnd() % 9); x.bwp_probe = !(rnd() % 11); x.bwp_entry = rnd() % 5 ? 0 : 1;
      x.rnti_min = rnd() % 3 ? 0x4600 + rnd() % 3 : 1; x.rnti_max = rnd() % 3 ? x.rnti_min : 0xfff3;
    }
    std::vector<int> ord(n);
    const bool css = rnd() & 1;
    const int nf = nr_fl_partition(t.data(), n, css, ord.data());
    std::vector<int> sorted = ord;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < n; i++) ASSERT_EQ(sorted[i], i);  // every task exactly once: nothing dropped, nothing duplicated
    for (int i = 0; i < n; i++) ASSERT_EQ(nr_fl_is_fast(&t[ord[i]], css), i < nf);
    for (int i = 1; i < nf; i++) ASSERT_LT(ord[i - 1], ord[i]);      // stable inside each class
    for (int i = nf + 1; i < n; i++) ASSERT_LT(ord[i - 1], ord[i]);
  }
}

// Same synthetic slots through the current order and the fast-lane order: identical state hash (EMA, persistence, dedupe, counters, the
// SET of enqueued grants). Includes the same RNTI decoded by a known-RNTI task and by an open-range task in one slot, cap pressure, UL.
TEST(FastLane, StateHashEqualsCurrentOrder)
{
  uint64_t r = 7;
  auto rnd = [&]() { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return (uint32_t)(r >> 8); };
  for (int trial = 0; trial < 60; trial++) {
    State a, b;
    for (long slot = 1; slot <= 400; slot++) {
      const bool css = !(rnd() % 6);
      std::vector<T> ts;
      const int n = 1 + rnd() % 40;
      for (int i = 0; i < n; i++) {
        T t{};
        t.id = i;
        t.f.dl_auto = css ? false : (rnd() % 8) != 0;
        t.f.open_rnti = !css && !(rnd() % 3);
        t.f.fmt10 = css ? true : !(rnd() % 7);
        t.f.ul_scan = !(rnd() % 5);
        t.f.rnti_min = t.f.open_rnti ? 1 : 0x4600 + rnd() % 6;
        t.f.rnti_max = t.f.open_rnti ? 0xfff3 : t.f.rnti_min;
        t.ok = (rnd() % 3) != 0;
        t.rnti = 0x4600 + rnd() % 6;
        t.mm = rnd() % 70;
        t.exempt = css && (rnd() % 2);
        ts.push_back(t);
      }
      int da = 0, db = 0;
      for (auto &t : ts) step(a, t, slot, da);
      std::vector<nr_fl_task_t> f;
      for (auto &t : ts) f.push_back(t.f);
      std::vector<int> ord(ts.size());
      nr_fl_partition(f.data(), (int)ts.size(), css, ord.data());
      for (int k : ord) step(b, ts[k], slot, db);
      ASSERT_EQ(a.thres, b.thres) << "trial " << trial << " slot " << slot;
    }
    ASSERT_EQ(a.hash(), b.hash()) << "trial " << trial;
  }
}
