// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Early start (nr_early_start.h): trigger timing for CORESETs at symbols 0, 7, 11; not-yet-discovered CORESETs; reconfiguration; no PDSCH
// start before its data; and the RX-thread cost of the early-start machinery per slot.
#include <gtest/gtest.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstdio>
#include "../nr_early_start.h"

namespace {
// numerology 1 (30 kHz), 61.44 Msps-like: fft 2048, normal CP 144, first symbol of each half-subframe 160 (OAI nb_prefix_samples0 = cp + 16)
struct Geom {
  int fft = 2048, cp = 144, cp0 = 160, spf_sub = 2;  // slots per subframe
  uint32_t sym(int slot, int l) const { const int idx = (slot % spf_sub) * 14 + l; return fft + ((idx % (7 * spf_sub)) == 0 ? cp0 : cp); }
  void cum(int slot, uint32_t *c) const { c[0] = 0; for (int k = 1; k <= 14; k++) c[k] = c[k - 1] + sym(slot, k - 1); }
};
}  // namespace

TEST(EarlyStart, TriggerTimingAtSymbols0_7_11)
{
  Geom g;
  uint32_t cum[15];
  g.cum(3, cum);
  const uint32_t first = g.sym(3, 0);
  nr_es_point_t p[NR_ES_MAX_POINTS];
  {  // CORESET at symbols 0-2: fires after symbol 2 (3 symbols complete), ~11 symbols before the slot ends
    const nr_es_cs_t cs[] = {{0, 3}};
    ASSERT_EQ(nr_es_plan(cs, 1, 14, cum, first, p, 8), 1);
    EXPECT_EQ(p[0].sym_end, 3);
    EXPECT_EQ(p[0].sample_off, (int)(cum[3] - first));
    EXPECT_EQ(p[0].mask, 1u);
    EXPECT_GE(cum[14] - cum[3], 11u * 2048u);  // lead of >= 11 symbols
  }
  {  // single symbol 0: its samples came with the previous read: fires at the start of this read
    const nr_es_cs_t cs[] = {{0, 1}};
    ASSERT_EQ(nr_es_plan(cs, 1, 14, cum, first, p, 8), 1);
    EXPECT_EQ(p[0].sym_end, 1);
    EXPECT_EQ(p[0].sample_off, 0);
  }
  {  // mid-slot CORESET at symbol 7 (1 symbol): fires mid-slot
    const nr_es_cs_t cs[] = {{7, 1}};
    ASSERT_EQ(nr_es_plan(cs, 1, 14, cum, first, p, 8), 1);
    EXPECT_EQ(p[0].sym_end, 8);
    EXPECT_EQ(p[0].sample_off, (int)(cum[8] - first));
  }
  {  // end-of-slot CORESET (symbols 11-13): fires at slot end, i.e. not an early point
    const nr_es_cs_t cs[] = {{11, 3}};
    EXPECT_EQ(nr_es_plan(cs, 1, 14, cum, first, p, 8), 0);
  }
  {  // symbol 11 alone: fires after symbol 11, two symbols before the end
    const nr_es_cs_t cs[] = {{11, 1}};
    ASSERT_EQ(nr_es_plan(cs, 1, 14, cum, first, p, 8), 1);
    EXPECT_EQ(p[0].sym_end, 12);
  }
  {  // three CORESETs (0, 7, 11): ascending points, equal ends merged
    const nr_es_cs_t cs[] = {{11, 1}, {0, 2}, {7, 2}, {0, 2}};
    ASSERT_EQ(nr_es_plan(cs, 4, 14, cum, first, p, 8), 3);
    EXPECT_EQ(p[0].sym_end, 2); EXPECT_EQ(p[0].mask, (1u << 1) | (1u << 3));
    EXPECT_EQ(p[1].sym_end, 9); EXPECT_EQ(p[1].mask, 1u << 2);
    EXPECT_EQ(p[2].sym_end, 12); EXPECT_EQ(p[2].mask, 1u << 0);
    EXPECT_LT(p[0].sample_off, p[1].sample_off);
    EXPECT_LT(p[1].sample_off, p[2].sample_off);
  }
  for (int slot : {0, 1, 7, 19}) {  // half-subframe boundary slots: cumulative offsets stay consistent with the per-slot symbol sizes
    g.cum(slot, cum);
    const nr_es_cs_t cs[] = {{2, 1}};
    ASSERT_EQ(nr_es_plan(cs, 1, 14, cum, g.sym(slot, 0), p, 8), 1);
    EXPECT_EQ(p[0].sample_off, (int)(cum[3] - g.sym(slot, 0)));
  }
}

TEST(EarlyStart, NotYetDiscoveredStaysOffTheEarlyPath)
{
  nr_es_banks_t b{};
  nr_es_cs_t cs[NR_ES_MAX_BANKS];
  int n = 0;
  ASSERT_TRUE(nr_es_banks_read(&b, cs, &n));
  EXPECT_EQ(n, 0);  // nothing discovered: no entry, no trigger; the slot-end job scans as today
  Geom g; uint32_t cum[15]; g.cum(2, cum);
  nr_es_point_t p[NR_ES_MAX_POINTS];
  EXPECT_EQ(nr_es_plan(cs, n, 14, cum, g.sym(2, 0), p, 8), 0);
  const nr_es_cs_t found[] = {{0, 2}};  // discovery verifies a CORESET: it joins the triggers
  EXPECT_TRUE(nr_es_banks_publish(&b, found, 1));
  ASSERT_TRUE(nr_es_banks_read(&b, cs, &n));
  ASSERT_EQ(n, 1);
  EXPECT_EQ(nr_es_plan(cs, n, 14, cum, g.sym(2, 0), p, 8), 1);
  const nr_es_cs_t unusable[] = {{0, 0}, {5, 2}};  // a bank entry that is early-ineligible (duration 0) never triggers
  EXPECT_TRUE(nr_es_banks_publish(&b, unusable, 2));
  ASSERT_TRUE(nr_es_banks_read(&b, cs, &n));
  ASSERT_EQ(nr_es_plan(cs, n, 14, cum, g.sym(2, 0), p, 8), 1);
  EXPECT_EQ(p[0].mask, 2u);
}

TEST(EarlyStart, ReconfigurationMovesTheTriggers)
{
  nr_es_banks_t b{};
  Geom g; uint32_t cum[15]; g.cum(4, cum);
  nr_es_cs_t cs[NR_ES_MAX_BANKS]; int n = 0;
  nr_es_point_t p[NR_ES_MAX_POINTS];
  const nr_es_cs_t a[] = {{0, 3}};
  EXPECT_TRUE(nr_es_banks_publish(&b, a, 1));
  const uint32_t gen1 = atomic_load(&b.gen);
  EXPECT_FALSE(nr_es_banks_publish(&b, a, 1));  // republishing the same geometry writes nothing (RX seqlock untouched)
  EXPECT_EQ(atomic_load(&b.gen), gen1);
  ASSERT_TRUE(nr_es_banks_read(&b, cs, &n));
  ASSERT_EQ(nr_es_plan(cs, n, 14, cum, g.sym(4, 0), p, 8), 1);
  EXPECT_EQ(p[0].sym_end, 3);
  const nr_es_cs_t moved[] = {{6, 2}};            // the CORESET moves (reconfiguration rewrites the bank): next publish moves the trigger
  EXPECT_TRUE(nr_es_banks_publish(&b, moved, 1));
  ASSERT_TRUE(nr_es_banks_read(&b, cs, &n));
  ASSERT_EQ(nr_es_plan(cs, n, 14, cum, g.sym(4, 0), p, 8), 1);
  EXPECT_EQ(p[0].sym_end, 8);
  EXPECT_EQ(atomic_load(&b.gen), gen1 + 1);
  EXPECT_TRUE(nr_es_banks_publish(&b, nullptr, 0));  // removed
  ASSERT_TRUE(nr_es_banks_read(&b, cs, &n));
  EXPECT_EQ(n, 0);
}

TEST(EarlyStart, SeqlockReadersNeverSeeATornGeometry)
{
  nr_es_banks_t b{};
  std::atomic<bool> stop{false};
  std::thread w([&]() {
    for (int i = 0; !stop; i++) {
      const int8_t f = (int8_t)(i % 10);
      nr_es_cs_t cs[2] = {{f, 1}, {f, 1}};  // both entries always equal: a torn read shows as different entries
      nr_es_banks_publish(&b, cs, 2);
    }
  });
  long reads = 0;
  for (int i = 0; i < 300000; i++) {
    nr_es_cs_t cs[NR_ES_MAX_BANKS]; int n;
    if (!nr_es_banks_read(&b, cs, &n)) continue;
    reads++;
    if (n == 2) ASSERT_EQ(cs[0].first_symbol, cs[1].first_symbol);
  }
  stop = true;
  w.join();
  EXPECT_GT(reads, 1000);
}

TEST(EarlyStart, ProgressAndDoneMarks)
{
  nr_es_progress_t pr{};
  EXPECT_EQ(nr_es_progress_symbols(&pr, 100, 14), 0);
  nr_es_progress_publish(&pr, 100, 3);
  EXPECT_EQ(nr_es_progress_symbols(&pr, 100, 14), 3);
  EXPECT_EQ(nr_es_progress_symbols(&pr, 99, 14), 14);   // an older slot is complete
  EXPECT_EQ(nr_es_progress_symbols(&pr, 101, 14), 0);   // a newer one has not started
  nr_es_done_t d{};
  EXPECT_FALSE(nr_es_done_is(&d, 0, 0));
  nr_es_done_mark(&d, 0, 0);
  EXPECT_TRUE(nr_es_done_is(&d, 0, 0));
  EXPECT_FALSE(nr_es_done_is(&d, 0, 1));
  EXPECT_FALSE(nr_es_done_is(&d, 1, 0));
}

// "No PDSCH start before its data": the consumer of a gated grant returns from the wait only after the RX thread completed the slot.
TEST(EarlyStart, PdschConsumerNeverStartsBeforeTheSlotIsComplete)
{
  nr_slot_gate_t g;
  nr_slot_gate_init(&g);
  nr_slot_gate_complete(&g, 99);
  EXPECT_TRUE(nr_slot_gate_wait(&g, 99, 10));   // an old slot never waits
  std::atomic<long> completed_ns{0}, started_ns{0};
  auto now = []() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long)t.tv_sec * 1000000000L + t.tv_nsec; };
  std::thread consumer([&]() {
    EXPECT_TRUE(nr_slot_gate_wait(&g, 100, 2000));
    started_ns = now();
  });
  usleep(20000);
  EXPECT_EQ(started_ns.load(), 0);                // data not complete: still waiting
  completed_ns = now();
  nr_slot_gate_complete(&g, 100);
  consumer.join();
  EXPECT_GE(started_ns.load(), completed_ns.load());
  EXPECT_FALSE(nr_slot_gate_wait(&g, 101, 20));   // the slot never arrives: timeout, the caller drops the job
  // a thousand slots, consumers racing the producer: never an early return
  std::atomic<long> done{-1};
  std::thread prod([&]() { for (long s = 200; s < 1200; s++) { done = s; nr_slot_gate_complete(&g, s); } });
  for (long s = 200; s < 1200; s++) {
    ASSERT_TRUE(nr_slot_gate_wait(&g, s, 2000));
    ASSERT_GE(done.load(), s);
  }
  prod.join();
}

TEST(EarlyStart, GateCostsNothingWhenNobodyWaits)
{
  nr_slot_gate_t g;
  nr_slot_gate_init(&g);
  timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  const int N = 1000000;
  for (int i = 0; i < N; i++) nr_slot_gate_complete(&g, i);
  clock_gettime(CLOCK_MONOTONIC, &b);
  const double ns = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / N;
  printf("slot_gate_complete (no waiter): %.1f ns per slot\n", ns);
  EXPECT_LT(ns, 1000.0);
}

// RX-thread cost of the early-start machinery for one slot, as the RX loop runs it: seqlock read, cumulative symbol offsets, plan, and per
// point a progress publish; plus the slot-end progress/gate publish. (The extra nrue_ru_read calls and the early-job enqueue are not here:
// they need the device layer and the scan queue; the report states what was and was not measured.)
TEST(EarlyStartBench, RxThreadCostPerSlot)
{
  Geom g;
  nr_es_banks_t b{};
  const nr_es_cs_t cs3[] = {{0, 2}, {7, 1}, {11, 1}};
  nr_es_banks_publish(&b, cs3, 3);
  nr_es_progress_t pr{};
  nr_slot_gate_t gate;
  nr_slot_gate_init(&gate);
  const int N = 2000000;
  timespec a, e;
  uint64_t sink = 0;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (int i = 0; i < N; i++) {
    nr_es_cs_t cs[NR_ES_MAX_BANKS]; int n = 0; uint32_t cum[15];
    nr_es_point_t p[NR_ES_MAX_POINTS];
    nr_es_banks_read(&b, cs, &n);
    cum[0] = 0;
    for (int k = 1; k <= 14; k++) cum[k] = cum[k - 1] + g.sym(i, k - 1);
    const int np = nr_es_plan(cs, n, 14, cum, g.sym(i, 0), p, NR_ES_MAX_POINTS);
    for (int j = 0; j < np; j++) { nr_es_progress_publish(&pr, i, p[j].sym_end); sink += (uint64_t)p[j].sample_off; }
    nr_es_progress_publish(&pr, i, 14);
    nr_slot_gate_complete(&gate, i);
  }
  clock_gettime(CLOCK_MONOTONIC, &e);
  const double ns = ((e.tv_sec - a.tv_sec) * 1e9 + (e.tv_nsec - a.tv_nsec)) / N;
  printf("early-start RX bookkeeping, 3 CORESETs: %.1f ns per slot = %.4f %% of a 500 us slot (%llu)\n", ns, 100.0 * ns / 500000.0, (unsigned long long)(sink & 1));
  EXPECT_LT(ns, 5000.0);  // < 1 % of a 500 us slot
}
