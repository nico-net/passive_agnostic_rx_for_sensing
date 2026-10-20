// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Job latency of the shared core pool under a synthetic mix shaped like the measured receiver (cores 0-4 only; run via taskset -c 0-4):
//   scan occasion 225 us every slot (500 us), DL decode 600 us at 1.2/slot, UL decode 400 us at 0.4/slot, retry 1500 us at 0.1/slot,
//   discovery 2 threads x 200 us chunks at ~45 % duty. Offered load at scale 1 = 3.4 cores. Latency = arrival -> job done.
// usage: bench_nr_core_pool <first_core> <n_cores> <load_scale> <seconds> [cfs]
//   default: the pool with n_cores tokens. cfs: no pool, every consumer thread pinned round-robin to the n_cores cores (time-sharing, SCHED_OTHER).
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sched.h>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <vector>
#include <time.h>
extern "C" {
#include "nr_core_pool.h"
}

struct Job { uint64_t arr, svc; };
struct Q {
  std::mutex m;
  std::condition_variable cv;
  std::deque<Job> d;
  int prio;
  const char *name;
  std::vector<uint64_t> lat, svc;
  std::mutex lm;
};
static std::atomic<bool> g_stop{false};
static bool g_cfs;
static int g_first, g_n;
static std::atomic<int> g_pin_rr{0};
static void pin_rr()
{
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(g_first + g_pin_rr++ % g_n, &cs);
  sched_setaffinity(0, sizeof(cs), &cs);
}
static void spin_for(uint64_t ns) { const uint64_t e = nr_pool_now_ns() + ns; while (nr_pool_now_ns() < e) {} }

static void consumer(nr_pool_t *p, Q *q)
{
  std::vector<uint64_t> lat, svc;
  if (g_cfs) pin_rr();
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> l(q->m);
      q->cv.wait(l, [&] { return !q->d.empty() || g_stop; });
      if (q->d.empty()) break;
      j = q->d.front();
      q->d.pop_front();
    }
    if (!g_cfs) nr_pool_acquire(p, q->prio, 0, j.arr);
    const uint64_t t0 = nr_pool_now_ns();
    spin_for(j.svc);
    if (!g_cfs) nr_pool_release(p);
    const uint64_t t1 = nr_pool_now_ns();
    lat.push_back(t1 - j.arr);
    svc.push_back(t1 - t0);
  }
  std::lock_guard<std::mutex> g(q->lm);
  q->lat.insert(q->lat.end(), lat.begin(), lat.end());
  q->svc.insert(q->svc.end(), svc.begin(), svc.end());
}

static void disc(nr_pool_t *p, double scale, unsigned seed, std::atomic<uint64_t> *chunks)
{
  std::mt19937 rng(seed);
  if (g_cfs) pin_rr();
  while (!g_stop) {
    if (!g_cfs) nr_pool_acquire(p, NR_POOL_DISC, 0, 0);
    for (int k = 0; k < 5; k++) { // one 1 ms job = 5 chunks of 200 us
      spin_for(200000);
      (*chunks)++;
      if (!g_cfs) nr_pool_yield(p);
    }
    if (!g_cfs) nr_pool_release(p);
    const double duty = std::min(0.95, 0.45 * scale); // per thread; two threads = 0.9 cores at scale 1
    const uint64_t idle = (uint64_t)(1000000.0 * (1.0 - duty) / duty);
    std::this_thread::sleep_for(std::chrono::nanoseconds(idle / 2 + rng() % (idle + 1)));
  }
}

static void report(const char *name, std::vector<uint64_t> &v, std::vector<uint64_t> &s)
{
  if (v.empty()) return;
  std::sort(v.begin(), v.end());
  std::sort(s.begin(), s.end());
  auto q = [&](double f) { return v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))] / 1000.0; };
  printf("  %-7s n=%-6zu lat_us p50=%7.0f p90=%7.0f p99=%7.0f p99.9=%7.0f max=%7.0f | svc_us p50=%6.0f\n", name, v.size(), q(0.5), q(0.9), q(0.99),
         q(0.999), v.back() / 1000.0, s[s.size() / 2] / 1000.0);
}

int main(int argc, char **argv)
{
  if (argc < 5) { fprintf(stderr, "usage: %s first_core n_tokens load_scale seconds [reserve]\n", argv[0]); return 2; }
  const int first = atoi(argv[1]), n = atoi(argv[2]);
  const double scale = atof(argv[3]);
  const int secs = atoi(argv[4]);
  int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = first + i;
  nr_pool_cfg_t cfg = nr_pool_cfg_default();
  g_cfs = argc > 5 && !strcmp(argv[5], "cfs");
  g_first = first;
  g_n = n;
  nr_pool_t *p = nr_pool_create(cores, nullptr, n, &cfg);

  Q scan, dl, ul, rt;
  scan.prio = NR_POOL_SCAN; scan.name = "scan";
  dl.prio = NR_POOL_DECODE; dl.name = "dl";
  ul.prio = NR_POOL_DECODE; ul.name = "ul";
  rt.prio = NR_POOL_RETRY; rt.name = "retry";
  std::vector<std::thread> th;
  for (int i = 0; i < 2; i++) th.emplace_back(consumer, p, &scan);
  for (int i = 0; i < 5; i++) th.emplace_back(consumer, p, &dl);
  for (int i = 0; i < 2; i++) th.emplace_back(consumer, p, &ul);
  th.emplace_back(consumer, p, &rt);
  std::atomic<uint64_t> chunks{0};
  std::vector<std::thread> dt;
  for (int i = 0; i < 2; i++) dt.emplace_back(disc, p, scale, 7u + (unsigned)i, &chunks);

  std::mt19937 rng(1);
  auto push = [&](Q &q, uint64_t svc) {
    const double j = 0.8 + 0.4 * (double)(rng() % 1000) / 1000.0;
    { std::lock_guard<std::mutex> l(q.m); q.d.push_back({nr_pool_now_ns(), (uint64_t)((double)svc * j)}); }
    q.cv.notify_one();
  };
  auto poisson = [&](double lam) { return std::poisson_distribution<int>(lam)(rng); };
  const uint64_t t_begin = nr_pool_now_ns(), t_end = t_begin + (uint64_t)secs * 1000000000ull;
  uint64_t next = t_begin;
  while (next < t_end) {
    next += 500000;
    timespec ts{(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    push(scan, 225000);
    for (int k = poisson(1.2 * scale); k > 0; k--) push(dl, 600000);
    for (int k = poisson(0.4 * scale); k > 0; k--) push(ul, 400000);
    for (int k = poisson(0.1 * scale); k > 0; k--) push(rt, 1500000);
  }
  g_stop = true;
  for (Q *q : {&scan, &dl, &ul, &rt}) { std::lock_guard<std::mutex> l(q->m); q->cv.notify_all(); }
  for (auto &t : th) t.join();
  for (auto &t : dt) t.join();
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  printf("%s N=%d first_core=%d scale=%.2f  busy_cores=%.2f disc_chunks/s=%.0f yields=%lu\n", g_cfs ? "CFS" : "POOL", n, first, scale,
         (double)st.busy_ns / (double)(nr_pool_now_ns() - t_begin), (double)chunks / secs, (unsigned long)st.yields);
  for (Q *q : {&scan, &dl, &ul, &rt}) report(q->name, q->lat, q->svc);
  char line[1024];
  nr_pool_stats_line(p, line, sizeof(line));
  printf("  %s\n", line);
  nr_pool_destroy(p);
  return 0;
}
