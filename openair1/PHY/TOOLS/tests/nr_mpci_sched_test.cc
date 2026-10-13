// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// MPCI M3 arbiter. Handles in one process share a real shm segment; fork() covers the cross-process cases.
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
extern "C" {
#include "nr_mpci_sched.h"
#include "nr_gpu_budget.h"
}
static const uint64_t T0 = 100000000;  // fake clock origin (us)
static std::string seg(const char *t) { return std::string("mpci_t_") + t + "_" + std::to_string(getpid()); }
struct Seg {
  std::string n;
  explicit Seg(const char *t) : n(seg(t)) { shm_unlink(("/" + n).c_str()); }
  ~Seg() { shm_unlink(("/" + n).c_str()); }
};

// Greedy cell: asks every 6 ms, charges 5 ms per admit. Returns total admitted us over `ms` ms.
static uint64_t run(nr_mpci_t *c, uint64_t &now, int ms, int acq = 0) {
  uint64_t used = 0;
  for (int i = 0; i < ms / 6; i++, now += 6000)
    if (nr_mpci_discovery_admit(c, now, acq)) { nr_mpci_discovery_charge(c, now, 5000); used += 5000; }
  return used;
}
// Two greedy cells interleaved, so both are "demanding".
static void run2(nr_mpci_t *a, nr_mpci_t *b, uint64_t &now, int ms, uint64_t &ua, uint64_t &ub, int acqa = 0, int acqb = 0) {
  for (int i = 0; i < ms / 6; i++, now += 6000) {
    if (nr_mpci_discovery_admit(a, now, acqa)) { nr_mpci_discovery_charge(a, now, 5000); ua += 5000; }
    if (nr_mpci_discovery_admit(b, now, acqb)) { nr_mpci_discovery_charge(b, now, 5000); ub += 5000; }
  }
}

TEST(MpciSched, ShareSplitAndBorrowing) {
  Seg s("share");
  nr_mpci_t *a = nr_mpci_attach(s.n.c_str(), "A", 300, 40), *b = nr_mpci_attach(s.n.c_str(), "B", 100, 99);
  ASSERT_TRUE(a && b);
  uint64_t now = T0, ua = 0, ub = 0;
  run2(a, b, now, 4000, ua, ub);  // steady state over 4 s; global budget 40% (creator wins, B's 99 ignored)
  double tot = (ua + ub) / 4e6;
  EXPECT_LE(tot, 0.41);
  EXPECT_GE(tot, 0.36);
  EXPECT_NEAR((double)ua / (ua + ub), 0.75, 0.08);  // 3:1 shares
  // B goes idle: A borrows the whole budget (work conserving).
  now += 1000000;
  uint64_t ua2 = run(a, now, 3000);
  EXPECT_GE(ua2 / 3e6, 0.36);
  EXPECT_LE(ua2 / 3e6, 0.41);
  nr_mpci_detach(a); nr_mpci_detach(b);
}

TEST(MpciSched, RtInOtherCellBlocksDiscovery) {
  Seg s("rt");
  nr_mpci_t *a = nr_mpci_attach(s.n.c_str(), "A", 100, 50);
  ASSERT_TRUE(a);
  int up[2], down[2];
  ASSERT_EQ(0, pipe(up)); ASSERT_EQ(0, pipe(down));
  pid_t p = fork();
  if (p == 0) {
    nr_mpci_t *b = nr_mpci_attach(s.n.c_str(), "B", 100, 50);
    if (!b) _exit(1);
    nr_mpci_rt_begin(b, T0);
    char c = 1; write(up[1], &c, 1);
    read(down[0], &c, 1);  // parent says: release
    nr_mpci_rt_end(b);
    write(up[1], &c, 1);
    read(down[0], &c, 1);
    _exit(0);
  }
  char c;
  ASSERT_EQ(1, read(up[0], &c, 1));
  EXPECT_EQ(0, nr_mpci_discovery_admit(a, T0 + 1000, 0));
  EXPECT_EQ(0, nr_mpci_discovery_admit(a, T0 + 1000, 1));  // ACQUIRE does not bypass RT
  // A hung cell (no heartbeat for 2 s) with RT stuck must not block forever.
  EXPECT_EQ(1, nr_mpci_discovery_admit(a, T0 + 3000000, 0));
  write(down[1], &c, 1);
  ASSERT_EQ(1, read(up[0], &c, 1));
  EXPECT_EQ(1, nr_mpci_discovery_admit(a, T0 + 3001000, 0));
  write(down[1], &c, 1);
  waitpid(p, nullptr, 0);
  nr_mpci_detach(a);
}

TEST(MpciSched, KilledCellSlotReclaimed) {
  Seg s("kill");
  nr_mpci_t *a = nr_mpci_attach(s.n.c_str(), "A", 100, 50);
  ASSERT_TRUE(a);
  int up[2]; ASSERT_EQ(0, pipe(up));
  pid_t p = fork();
  if (p == 0) {
    nr_mpci_t *b = nr_mpci_attach(s.n.c_str(), "B", 100, 50);
    if (!b) _exit(1);
    nr_mpci_rt_begin(b, T0);
    char c = 1; write(up[1], &c, 1);
    pause();
    _exit(0);
  }
  char c;
  ASSERT_EQ(1, read(up[0], &c, 1));
  nr_mpci_cell_stat_t st[NR_MPCI_MAX_CELLS];
  EXPECT_EQ(2, nr_mpci_snapshot(a, T0, st, NR_MPCI_MAX_CELLS));
  kill(p, SIGKILL);
  waitpid(p, nullptr, 0);
  EXPECT_EQ(1, nr_mpci_snapshot(a, T0 + 10, st, NR_MPCI_MAX_CELLS));  // reaped by pid check
  EXPECT_EQ(1, nr_mpci_discovery_admit(a, T0 + 20, 0));               // dead cell's stuck RT is gone
  nr_mpci_t *b2 = nr_mpci_attach(s.n.c_str(), "B2", 100, 50);          // slot reusable
  ASSERT_TRUE(b2);
  EXPECT_EQ(2, nr_mpci_snapshot(a, T0 + 30, st, NR_MPCI_MAX_CELLS));
  nr_mpci_detach(a); nr_mpci_detach(b2);
}

TEST(MpciSched, AcquireBeatsMaintain) {
  Seg s("acq");
  nr_mpci_t *a = nr_mpci_attach(s.n.c_str(), "ACQ", 100, 40), *b = nr_mpci_attach(s.n.c_str(), "MNT", 100, 40);
  ASSERT_TRUE(a && b);
  uint64_t now = T0, ua = 0, ub = 0;
  run2(a, b, now, 4000, ua, ub, /*acqa=*/1, /*acqb=*/0);
  EXPECT_GT(ua, 2 * ub);  // equal shares, yet the ACQUIRE cell takes the lion's share
  EXPECT_GT(ub, 0u);      // MAINTAIN is deprioritised, not starved
  EXPECT_LE((ua + ub) / 4e6, 0.41);
  nr_mpci_detach(a); nr_mpci_detach(b);
}

TEST(MpciSched, DropsPublishedAndOffIsNoop) {
  EXPECT_EQ(nullptr, nr_mpci_default(30));  // ISAC_MPCI_SCHED unset
  EXPECT_EQ(1, nr_mpci_discovery_admit(nullptr, T0, 1));
  nr_mpci_discovery_charge(nullptr, T0, 10); nr_mpci_rt_begin(nullptr, T0); nr_mpci_rt_end(nullptr);
  nr_mpci_drops_t d = {1, 2, 3, 4, 5, 6, 7};
  nr_mpci_publish_drops(nullptr, &d);
  Seg s("drops");
  nr_mpci_t *a = nr_mpci_attach(s.n.c_str(), "A", 100, 30);
  ASSERT_TRUE(a);
  nr_mpci_publish_drops(a, &d);
  nr_mpci_t *ro = nr_mpci_attach_ro(s.n.c_str());
  ASSERT_TRUE(ro);
  nr_mpci_cell_stat_t st[NR_MPCI_MAX_CELLS];
  ASSERT_EQ(1, nr_mpci_snapshot(ro, T0, st, NR_MPCI_MAX_CELLS));
  EXPECT_EQ(7u, st[0].drops.pdschq_narrow);
  EXPECT_EQ(3u, st[0].drops.scanq_epoch);
  nr_mpci_detach(ro); nr_mpci_detach(a);
}

// nr_gpu_budget.c hooks, in a fresh process (its init is process-once).
TEST(MpciSched, BudgetHookYieldsOnForeignRt) {
  Seg s("hook");
  nr_mpci_t *b = nr_mpci_attach(s.n.c_str(), "other", 100, 30);
  ASSERT_TRUE(b);
  pid_t p = fork();
  if (p == 0) {
    setenv("ISAC_GPU_BUDGET", "1", 1); setenv("ISAC_MPCI_SCHED", s.n.c_str(), 1);
    nr_gpu_budget_test_reset(30);
    uint64_t now = nr_gpu_budget_now_us();
    nr_mpci_rt_begin(b, now);  // same pid: slot of "other" holds RT, ours is a second slot
    int r1 = nr_gpu_budget_discovery_begin(now);
    nr_mpci_rt_end(b);
    int r2 = nr_gpu_budget_discovery_begin(now + 10);
    if (r2 == 1) nr_gpu_budget_discovery_end(now + 20, 1000);
    nr_gpu_budget_metrics_t m; nr_gpu_budget_metrics(&m);
    _exit(r1 == 0 && r2 == 1 && m.discovery_yields == 1 ? 0 : 1);
  }
  int st = 0; waitpid(p, &st, 0);
  EXPECT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
  nr_mpci_detach(b);
}
