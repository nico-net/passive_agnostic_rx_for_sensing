// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// mpci B6: a full DM-RS scan runs under one DISC lease of the core pool and must hand the token to a SCAN waiter within a slice of work, not after the whole chunk.
// Real pool (the site pool is injected), real scan; the assertion is ordinal (the SCAN waiter is granted while the chunk is still running) plus the pool's own yield
// counter, not a time threshold.
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <chrono>
#include <random>
#include <thread>
#include <vector>
#include <unistd.h>
extern "C" {
#include "nr_core_pool.h"
#include "nr_pdsch_dmrs_scan.h"
}

namespace {
constexpr int N = 4096, NRB = 217, FCO = N - NRB * 12 / 2, NANT = 4;
struct Holder {
  nr_pool_t *p;
  int prio;
  std::atomic<int> state{0};
  std::atomic<bool> go{false};
  std::thread th;
  Holder(nr_pool_t *p_, int pr, bool hold) : p(p_), prio(pr)
  {
    go = !hold;
    th = std::thread([this] {
      nr_pool_acquire(p, prio, 0, 0);
      state = 1;
      while (!go) usleep(200);
      nr_pool_release(p);
    });
  }
  ~Holder() { go = true; if (th.joinable()) th.join(); }
};
}

TEST(DmrsScanSlice, ScanWaiterIsServedInsideTheChunk)
{
  int cores[6] = {0, 1, 2, 3, 4, 5};
  nr_pool_cfg_t cfg = nr_pool_cfg_default_n(6);
  cfg.pin = false;
  nr_pool_t *p = nr_pool_create(cores, nullptr, 6, &cfg);
  nr_pool_site_set_for_test(p);
  ASSERT_TRUE(nr_pool_site_float_merged()) << "6 tokens: floats lease the pool";
  std::mt19937 rng(3);
  std::normal_distribution<double> nd(0, 20);
  std::vector<std::vector<int16_t>> g(NANT, std::vector<int16_t>(2 * N));
  for (auto &a : g) for (auto &v : a) v = (int16_t)nd(rng);
  std::vector<const int16_t *> rx;
  for (auto &a : g) rx.push_back(a.data());
  // 4 SCAN leases and one UL decode keep the other five tokens; the sixth is the scan's DISC lease
  std::vector<std::unique_ptr<Holder>> fill;
  for (int i = 0; i < 4; i++) fill.emplace_back(new Holder(p, NR_POOL_SCAN, true));
  fill.emplace_back(new Holder(p, NR_POOL_DECODE | NR_POOL_F_UL, true));
  for (auto &h : fill) for (int i = 0; i < 20000 && h->state != 1; i++) usleep(500);
  std::atomic<bool> chunk_done{false}, started{false};
  std::thread scan([&] {
    nr_pool_site_float_acquire(NR_POOL_DISC, 0, 0);
    started = true;
    nr_dmrs_scan_hit_t h;
    nr_dmrs_scan_chunk(rx.data(), NANT, N, FCO, 6, 2, 14, 100, 16, &h); // one full 16-RB chunk: 65536 identities
    nr_pool_site_float_release();
    chunk_done = true;
  });
  while (!started) usleep(100);
  usleep(1000);
  ASSERT_FALSE(chunk_done) << "the chunk is longer than a millisecond";
  std::atomic<bool> scan_granted_inside{false}, granted{false};
  std::thread waiter([&] {
    nr_pool_acquire(p, NR_POOL_SCAN, 1, 0);
    scan_granted_inside = !chunk_done;
    granted = true;
    nr_pool_release(p);
  });
  waiter.join();
  EXPECT_TRUE(granted);
  EXPECT_TRUE(scan_granted_inside) << "the SCAN waiter had to wait for the end of the chunk: the scan does not offer its token";
  scan.join();
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  EXPECT_GE(st.yields, 1u);
  for (auto &h : fill) h->go = true;
  fill.clear();
  nr_pool_site_set_for_test(nullptr);
  nr_pool_destroy(p);
}
