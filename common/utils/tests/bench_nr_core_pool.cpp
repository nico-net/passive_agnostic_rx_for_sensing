// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Job latency of the shared core pool under a synthetic mix shaped like the measured receiver (run via taskset; pool cores = first..first+n-1):
//   scan occasion 225 us every slot (500 us), DL decode 600 us at 1.2/slot, UL decode 400 us at 0.4/slot, retry 1500 us at 0.1/slot,
//   discovery 2 threads x 200 us chunks at ~45 % duty. Offered load at scale 1 = 3.4 cores. Latency = arrival -> job done.
// Float-lane mode (env BENCH_FLOAT=sep|merged, BENCH_LANE=<cpu list>, BENCH_PROD=<cpu>, BENCH_POOL=<cpu list> overrides first/n): the
//   discovery/background threads of the receiver's float lane (duty cycles from SYSAN-20261015 / CORE-PLAN-20261018, see FLOAT-LANE-POOL-20261021.md).
//   sep = status quo: unleased threads on the lane cpus (CFS). merged = they lease class DISC/HOUSE of the same pool (lane cpus = the home core(s)
//   of the unleasable threads only). "ulBgDisc" is one 150 ms job; BENCH_ULBG_CHUNK_MS=0 keeps it unchunked (one lease), else yields every N ms.
// Cell dimension (B1, mpci-main plan B.4): the compile-time macro BENCH_NCELLS=<k> (default 1 = the single-cell bench, output unchanged) runs k cells, each with the
//   full mix above and its own consumer and discovery threads, leasing with its cell index; BENCH_CELL_SCALE_LIST={s0,s1,...} (integers) multiplies the offered load of
//   each cell (default all 1; {3,1} = cell 0 bursts at 3x while cell 1 stays quiet). Build-time constants, no runtime flag. The per-cell rows are named "c<k> scan" etc. and the CORE_POOL line carries one block per cell.
// B5 (float lane per cell, mpci-main plan B5): every cell runs its own copy of the 8 float threads (class DISC / HOUSE, leasing with its cell index).
//   NO FLAGS: the float mode and the layout are bench COMMAND-LINE arguments after the four positional ones (a bench may take arguments; the receiver may not):
//     float=none|sep|merged   lane=<cpu list>  prod=<cpu>  pool=<cpu list>  noage  ulbg_chunk_ms=<ms>  cells=<k>  cellscale=<a,b,..>  cfs
//   The old BENCH_FLOAT / BENCH_LANE / BENCH_PROD / BENCH_POOL / BENCH_NOAGE / BENCH_ULBG_CHUNK_MS environment variables are still read when the argument is absent.
// CHANPOOL (channelizer workers as class CHAN): arguments chan=<k> (workers; 0 = none, the default) chan_msps=<capture MS/s per channel> chan_nch=<channels>
//   chan_per_thread_msps=<measured single-thread backend rate, MS/s per channel (test_iq_chan --bench)> chan_block=<samples, default 65536> chan_demand (word: register the
//   demand = k with the pool, i.e. the reserved-tokens policy; without it CHAN leases are just the top class). Model: every block period, each of the k workers leases
//   CHAN (blocking), spins its share of the block work (work per block = block x nch / (per_thread_rate x nch) / k per worker), releases; a block is LATE when the last worker
//   finished more than one period after the block arrived (the channelizer shortfall: the wideband ring backs up). Offered CHAN load is reported.
// usage: bench_nr_core_pool <first_core> <n_cores> <load_scale> <seconds> [cfs] [float=..] [lane=..] [prod=..] [pool=..] [noage] [ulbg_chunk_ms=..] [cells=..] [cellscale=..]
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
#include <memory>
#include <vector>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
extern "C" {
#include "nr_core_pool.h"
}

#ifndef BENCH_NCELLS
#define BENCH_NCELLS 1
#endif
#ifndef BENCH_CELL_SCALE_LIST
#define BENCH_CELL_SCALE_LIST {}
#endif
struct Job { uint64_t arr, svc; };
struct Q {
  std::mutex m;
  std::condition_variable cv;
  std::deque<Job> d;
  int prio, cell = 0;
  double cscale = 1.0; // per-cell load multiplier
  const char *name;
  std::vector<uint64_t> lat, svc, wt;
  uint64_t dl_ns; // deadline: latency above it counts as a miss
  std::atomic<uint64_t> done{0}, pushed{0};
  std::mutex lm;
};
static std::atomic<bool> g_stop{false};
static std::atomic<uint64_t> g_t_end{0}; // after this + 200 ms the consumers abandon their backlog (counted as drop)
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
  std::vector<uint64_t> lat, svc, wt;
  if (g_cfs) pin_rr();
  for (;;) {
    Job j;
    {
      std::unique_lock<std::mutex> l(q->m);
      q->cv.wait(l, [&] { return !q->d.empty() || g_stop; });
      if (q->d.empty() || (g_stop && nr_pool_now_ns() > g_t_end + 200000000ull)) break;
      j = q->d.front();
      q->d.pop_front();
    }
    if (!g_cfs) nr_pool_acquire(p, q->prio, q->cell, j.arr);
    const uint64_t t0 = nr_pool_now_ns();
    spin_for(j.svc);
    if (!g_cfs) nr_pool_release(p);
    const uint64_t t1 = nr_pool_now_ns();
    lat.push_back(t1 - j.arr);
    svc.push_back(t1 - t0);
    wt.push_back(t0 - j.arr);
    q->done++;
  }
  std::lock_guard<std::mutex> g(q->lm);
  q->lat.insert(q->lat.end(), lat.begin(), lat.end());
  q->svc.insert(q->svc.end(), svc.begin(), svc.end());
  q->wt.insert(q->wt.end(), wt.begin(), wt.end());
}

static void disc(nr_pool_t *p, double scale, unsigned seed, std::atomic<uint64_t> *chunks, int cell)
{
  std::mt19937 rng(seed);
  if (g_cfs) pin_rr();
  while (!g_stop) {
    if (!g_cfs) nr_pool_acquire(p, NR_POOL_DISC, cell, 0);
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


// ---- float lane model ----
static std::vector<int> parse_list(const char *e)
{
  std::vector<int> v;
  if (!e) return v;
  char buf[256]; snprintf(buf, sizeof(buf), "%s", e);
  char *sv = nullptr;
  for (char *t = strtok_r(buf, ",", &sv); t; t = strtok_r(nullptr, ",", &sv)) {
    int a = -1, b = -1;
    if (sscanf(t, "%d-%d", &a, &b) != 2) b = a = atoi(t);
    for (int k = a; k <= b; k++) v.push_back(k);
  }
  return v;
}
static void pin_list(const std::vector<int> &l)
{
  if (l.empty()) return;
  cpu_set_t cs; CPU_ZERO(&cs);
  for (int c : l) CPU_SET(c, &cs);
  sched_setaffinity(0, sizeof(cs), &cs);
}
struct FSpec { const char *name; int prio; double duty; double job_ms; int nice; };
// duty = fraction of one core in the replay thread map; ulBgDisc: 150 ms job (113-287 ms measured)
static const FSpec kF[] = {
  {"janon", NR_POOL_DISC, 0.30, 2.0, 0},   {"nidgpu", NR_POOL_DISC, 0.30, 3.0, 0}, {"uss", NR_POOL_DISC, 0.25, 1.0, 0},
  {"dmrsScan", NR_POOL_DISC, 0.15, 2.0, 0}, {"dmrsIdBg", NR_POOL_DISC, 0.30, 1.0, 19}, {"csirsDisc", NR_POOL_DISC, 0.05, 0.5, 0},
  {"ulBgDisc", NR_POOL_HOUSE, 0.45, 150.0, 10}, {"ulNidBg", NR_POOL_HOUSE, 0.03, 2.0, 19},
};
struct FStat { std::vector<uint64_t> wait, str; double cpu_ns = 0; uint64_t jobs = 0; std::mutex m; };
static void float_thread(nr_pool_t *p, const FSpec *f, FStat *st, bool merged, const std::vector<int> *lane, double chunk_ms, unsigned seed, int cell)
{
  std::mt19937 rng(seed);
  if (!merged) { pin_list(*lane); }
  if (f->nice) setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), f->nice);
  const uint64_t job = (uint64_t)(f->job_ms * 1e6);
  const uint64_t idle = (uint64_t)((double)job * (1.0 - f->duty) / f->duty);
  const uint64_t chunk = chunk_ms > 0 && f->prio == NR_POOL_HOUSE ? (uint64_t)(chunk_ms * 1e6) : (f->job_ms > 5 ? 5000000ull : job);
  std::vector<uint64_t> w, s;
  double cpu = 0;
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::nanoseconds(idle / 2 + rng() % (idle + 1)));
    if (g_stop) break;
    const uint64_t arr = nr_pool_now_ns();
    uint64_t left = job, first_start = 0;
    const bool lease_whole = !(chunk_ms > 0) && f->prio == NR_POOL_HOUSE;
    if (merged) nr_pool_acquire(p, f->prio, cell, arr);
    first_start = nr_pool_now_ns();
    while (left) {
      const uint64_t c = std::min(left, lease_whole ? left : chunk);
      spin_for(c);
      left -= c;
      cpu += (double)c;
      if (merged && left) nr_pool_yield(p);
    }
    if (merged) nr_pool_release(p);
    w.push_back(first_start - arr);
    s.push_back(nr_pool_now_ns() - arr);
  }
  std::lock_guard<std::mutex> g(st->m);
  st->wait = w; st->str = s; st->cpu_ns = cpu; st->jobs = w.size();
}

struct ChanStat { std::mutex m; std::vector<uint64_t> wait; uint64_t blocks = 0, late = 0; std::vector<uint64_t> done; };
static void chan_worker(nr_pool_t *p, int idx, int k, uint64_t period_ns, uint64_t work_ns, uint64_t t_begin, ChanStat *cs)
{
  std::vector<uint64_t> w, d;
  uint64_t late = 0, blocks = 0;
  for (uint64_t arr = t_begin + period_ns; !g_stop; arr += period_ns) {
    timespec ts{(time_t)(arr / 1000000000ull), (long)(arr % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    if (g_stop) break;
    nr_pool_acquire(p, NR_POOL_CHAN, 0, arr);
    const uint64_t t0 = nr_pool_now_ns();
    spin_for(work_ns / (uint64_t)k);
    nr_pool_release(p);
    const uint64_t t1 = nr_pool_now_ns();
    w.push_back(t0 - arr);
    d.push_back(t1 - arr);
    blocks++;
    late += t1 - arr > period_ns;
    (void)idx;
  }
  std::lock_guard<std::mutex> g(cs->m);
  cs->wait.insert(cs->wait.end(), w.begin(), w.end());
  cs->done.insert(cs->done.end(), d.begin(), d.end());
  cs->blocks += blocks;
  cs->late += late;
}

// wait = arrival -> job start (queue + token); lat = arrival -> done; miss = lat > deadline; drop = never started by the end of the run
static void report(Q *Qp)
{
  auto &v = Qp->lat; auto &w = Qp->wt; auto &s = Qp->svc;
  if (v.empty()) return;
  uint64_t miss = 0;
  for (uint64_t x : v) miss += x > Qp->dl_ns;
  miss += Qp->pushed - Qp->done; // drops count as misses
  std::sort(v.begin(), v.end()); std::sort(w.begin(), w.end()); std::sort(s.begin(), s.end());
  auto q = [](std::vector<uint64_t> &x, double f) { return x[std::min(x.size() - 1, (size_t)(f * (double)x.size()))] / 1000.0; };
  printf("  %-6s n=%-6zu wait_us p50=%7.0f p99=%8.0f max=%8.0f | lat_us p50=%7.0f p99=%8.0f max=%8.0f | svc_p50=%5.0f miss=%lu/%lu drop=%lu\n", Qp->name,
         v.size(), q(w, .5), q(w, .99), w.back() / 1000.0, q(v, .5), q(v, .99), v.back() / 1000.0, s[s.size() / 2] / 1000.0, (unsigned long)miss, (unsigned long)Qp->pushed,
         (unsigned long)(Qp->pushed - Qp->done));
}

int main(int argc, char **argv)
{
  if (argc < 5) { fprintf(stderr, "usage: %s first_core n_tokens load_scale seconds [reserve]\n", argv[0]); return 2; }
  int first = atoi(argv[1]), n = atoi(argv[2]);
  // optional arguments after the four positional ones (key=value or a bare word); the legacy BENCH_* environment variables are the fallback
  auto arg_of = [&](const char *key) -> const char * {
    const size_t kl = strlen(key);
    for (int i = 5; i < argc; i++)
      if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') return argv[i] + kl + 1;
    return nullptr;
  };
  auto has_word = [&](const char *w) { for (int i = 5; i < argc; i++) if (!strcmp(argv[i], w)) return true; return false; };
  auto opt = [&](const char *key, const char *envname) -> const char * { const char *a = arg_of(key); return a ? a : getenv(envname); };
  std::vector<int> pool_l = parse_list(opt("pool", "BENCH_POOL")), lane_l = parse_list(opt("lane", "BENCH_LANE"));
  const char *fm = opt("float", "BENCH_FLOAT");
  const bool f_sep = fm && !strcmp(fm, "sep"), f_merged = fm && !strcmp(fm, "merged");
  const char *uc = opt("ulbg_chunk_ms", "BENCH_ULBG_CHUNK_MS");
  const double ulbg_chunk = uc ? atof(uc) : 5.0;
  if (!pool_l.empty()) n = (int)pool_l.size();
  if (opt("prod", "BENCH_PROD")) pin_list(parse_list(opt("prod", "BENCH_PROD")));
  const double scale = atof(argv[3]);
  const int secs = atoi(argv[4]);
  int cores[NR_POOL_MAX_TOKENS];
  for (int i = 0; i < n; i++) cores[i] = pool_l.empty() ? first + i : pool_l[(size_t)i];
  nr_pool_cfg_t cfg = nr_pool_cfg_default();
  if (has_word("noage") || getenv("BENCH_NOAGE")) cfg.age_ns[NR_POOL_DISC] = cfg.age_ns[NR_POOL_HOUSE] = 0; // small-host policy: low classes are never promoted into the reserve
  g_cfs = has_word("cfs");
  g_first = first;
  g_n = n;
  nr_pool_t *p = nr_pool_create(cores, nullptr, n, &cfg);

  int ncell = arg_of("cells") ? atoi(arg_of("cells")) : BENCH_NCELLS;
  if (ncell < 1 || ncell > NR_POOL_MAX_CELLS) ncell = 1;
  const std::vector<int> cs_l = arg_of("cellscale") ? parse_list(arg_of("cellscale")) : std::vector<int>(BENCH_CELL_SCALE_LIST); // integer multipliers
  struct CQ { Q scan, dl, ul, rt; char nm[4][16]; };
  std::vector<std::unique_ptr<CQ>> cq;
  std::vector<std::thread> th;
  std::atomic<uint64_t> chunks{0};
  std::vector<std::thread> dt;
  for (int c = 0; c < ncell; c++) {
    cq.emplace_back(new CQ);
    CQ &x = *cq.back();
    const char *pre = ncell > 1 ? "c%d " : "";
    const double cs = (size_t)c < cs_l.size() ? cs_l[(size_t)c] : 1.0;
    Q *qs[4] = {&x.scan, &x.dl, &x.ul, &x.rt};
    const char *nn[4] = {"scan", "dl", "ul", "retry"};
    const int pr[4] = {NR_POOL_SCAN, NR_POOL_DECODE, NR_POOL_DECODE | NR_POOL_F_UL, NR_POOL_RETRY};
    const uint64_t dls[4] = {500000, 2000000, 2000000, 10000000};
    for (int i = 0; i < 4; i++) {
      char f[16];
      snprintf(f, sizeof(f), pre, c);
      snprintf(x.nm[i], sizeof(x.nm[i]), "%s%s", f, nn[i]);
      qs[i]->prio = pr[i], qs[i]->name = x.nm[i], qs[i]->dl_ns = dls[i], qs[i]->cell = c, qs[i]->cscale = cs;
    }
    for (int i = 0; i < 2; i++) th.emplace_back(consumer, p, &x.scan);
    for (int i = 0; i < 5; i++) th.emplace_back(consumer, p, &x.dl);
    for (int i = 0; i < 2; i++) th.emplace_back(consumer, p, &x.ul);
    th.emplace_back(consumer, p, &x.rt);
    for (int i = 0; i < 2; i++) dt.emplace_back(disc, p, scale * cs, 7u + (unsigned)i + 16u * (unsigned)c, &chunks, c);
  }
  auto all_q = [&] { std::vector<Q *> v; for (auto &x : cq) for (Q *q : {&x->scan, &x->dl, &x->ul, &x->rt}) v.push_back(q); return v; };

  const size_t nf0 = sizeof(kF) / sizeof(kF[0]), nf = nf0 * (size_t)ncell; // float thread k = cell k / nf0, spec k % nf0
  std::vector<FStat> fst(nf);
  std::vector<std::thread> ft;
  if (f_sep || f_merged)
    for (size_t i = 0; i < nf; i++) ft.emplace_back(float_thread, p, &kF[i % nf0], &fst[i], f_merged, &lane_l, ulbg_chunk, 100u + (unsigned)i, (int)(i / nf0));
  std::atomic<uint64_t> hbusy{0}; // unleasable threads (actors, CUDA helpers, log, ITTI): 0.25 core in 200 us bursts on the lane / home cpus, both modes
  std::thread home;
  if (f_sep || f_merged) home = std::thread([&] {
    pin_list(lane_l);
    std::mt19937 r(77);
    while (!g_stop) { spin_for(200000); hbusy += 200000; std::this_thread::sleep_for(std::chrono::nanoseconds(400000 + r() % 1200000)); }
  });
  std::mt19937 rng(1);
  const int chan_k = arg_of("chan") ? atoi(arg_of("chan")) : 0;
  ChanStat cst;
  std::vector<std::thread> cth;
  uint64_t chan_period = 0, chan_work = 0;
  if (chan_k > 0) {
    const double msps = arg_of("chan_msps") ? atof(arg_of("chan_msps")) : 245.76, pt = arg_of("chan_per_thread_msps") ? atof(arg_of("chan_per_thread_msps")) : 133.0;
    const double block = arg_of("chan_block") ? atof(arg_of("chan_block")) : 65536.0;
    chan_period = (uint64_t)(block / (msps * 1e6) * 1e9);
    chan_work = (uint64_t)(block / (pt * 1e6) * 1e9); // one thread's time for one block (all channels and cells of the load)
    if (has_word("chan_demand")) nr_pool_set_chan_demand(p, chan_k);
    for (int i = 0; i < chan_k; i++) cth.emplace_back(chan_worker, p, i, chan_k, chan_period, chan_work, nr_pool_now_ns(), &cst);
  }
  auto push = [&](Q &q, uint64_t svc) {
    const double j = 0.8 + 0.4 * (double)(rng() % 1000) / 1000.0;
    { std::lock_guard<std::mutex> l(q.m); q.d.push_back({nr_pool_now_ns(), (uint64_t)((double)svc * j)}); }
    q.pushed++;
    q.cv.notify_one();
  };
  auto poisson = [&](double lam) { return std::poisson_distribution<int>(lam)(rng); };
  const uint64_t t_begin = nr_pool_now_ns(), t_end = t_begin + (uint64_t)secs * 1000000000ull;
  g_t_end = t_end;
  uint64_t next = t_begin;
  while (next < t_end) {
    next += 500000;
    timespec ts{(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    for (auto &x : cq) {
      const double sc = scale * x->scan.cscale;
      if (x->scan.cscale >= 1.0) for (int k = 0; k < (int)x->scan.cscale; k++) push(x->scan, 225000); else push(x->scan, 225000);
      for (int k = poisson(1.2 * sc); k > 0; k--) push(x->dl, 600000);
      for (int k = poisson(0.4 * sc); k > 0; k--) push(x->ul, 400000);
      for (int k = poisson(0.1 * sc); k > 0; k--) push(x->rt, 1500000);
    }
  }
  g_stop = true;
  for (auto &t : cth) t.join();
  for (Q *q : all_q()) { std::lock_guard<std::mutex> l(q->m); q->cv.notify_all(); }
  for (auto &t : th) t.join();
  for (auto &t : dt) t.join();
  for (auto &t : ft) t.join();
  if (home.joinable()) home.join();
  nr_pool_stats_t st;
  nr_pool_stats(p, &st);
  printf("%s%s N=%d first_core=%d scale=%.2f  busy_cores=%.2f disc_chunks/s=%.0f yields=%lu nested=%lu\n", g_cfs ? "CFS" : "POOL", f_sep ? "+FLOAT-SEP" : f_merged ? "+FLOAT-MERGED" : "", n, first, scale,
         (double)st.busy_ns / (double)(nr_pool_now_ns() - t_begin), (double)chunks / secs, (unsigned long)st.yields, (unsigned long)st.nested);
  for (Q *q : all_q()) report(q);
  for (size_t i = 0; (f_sep || f_merged) && i < nf; i++) {
    auto &w = fst[i].wait, &x = fst[i].str;
    const FSpec &sp = kF[i % nf0];
    char fn[24];
    if (ncell > 1) snprintf(fn, sizeof(fn), "c%zu %s", i / nf0, sp.name);
    else snprintf(fn, sizeof(fn), "%s", sp.name);
    if (w.empty()) { printf("  F %-9s no jobs\n", fn); continue; }
    std::sort(w.begin(), w.end()); std::sort(x.begin(), x.end());
    auto q = [](std::vector<uint64_t> &v, double f) { return v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))] / 1000.0; };
    printf("  F %-9s duty_target=%.2f duty_got=%.2f jobs=%zu start_wait_us p50=%.0f p99=%.0f max=%.0f | done_us p50=%.0f p99=%.0f max=%.0f (job %.1f ms)\n", fn,
           sp.duty, fst[i].cpu_ns / 1e9 / secs, w.size(), q(w, .5), q(w, .99), w.back() / 1000.0, q(x, .5), q(x, .99), x.back() / 1000.0, sp.job_ms);
  }
  if (f_sep || f_merged) { // float CPU got / offered, all cells
    double got = 0, off = 0;
    for (size_t i = 0; i < nf; i++) got += fst[i].cpu_ns / 1e9 / secs, off += kF[i % nf0].duty;
    printf("  FLOAT cells=%d cpu_got=%.2f cpu_offered=%.2f ratio=%.2f\n", ncell, got, off, off > 0 ? got / off : 0.0);
  }
  if (chan_k > 0) {
    auto &w = cst.wait; auto &x = cst.done;
    std::sort(w.begin(), w.end()); std::sort(x.begin(), x.end());
    auto q = [](std::vector<uint64_t> &v, double f) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, (size_t)(f * (double)v.size()))] / 1000.0; };
    printf("  CHAN k=%d demand=%s period_us=%.1f work_us_per_block=%.1f offered_cores=%.2f leases=%zu lease_wait_us p50=%.0f p99=%.0f max=%.0f | done_us p50=%.0f p99=%.0f max=%.0f | late_worker_blocks=%lu/%lu (shortfall %.4f%%)\n",
           chan_k, has_word("chan_demand") ? "k" : "none", (double)chan_period / 1000.0, (double)chan_work / 1000.0, (double)chan_work / (double)chan_period, w.size(), q(w, .5), q(w, .99),
           w.empty() ? 0.0 : w.back() / 1000.0, q(x, .5), q(x, .99), x.empty() ? 0.0 : x.back() / 1000.0, (unsigned long)cst.late, (unsigned long)cst.blocks,
           cst.blocks ? 100.0 * (double)cst.late / (double)cst.blocks : 0.0);
  }
  char line[1024];
  nr_pool_stats_line(p, line, sizeof(line));
  printf("  %s\n", line);
  nr_pool_destroy(p);
  return 0;
}
