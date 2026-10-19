// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// PDCCH-REDESIGN-20261019 micro-benchmark 2: thread hand-off latency and CPU cost on the DGX Grace, plus dft4096 cost.
//   ./handoff_bench <producer_cpu> <consumer_cpu>      e.g. 1 2  (both in 0-4 = Cortex-A725), then 6 7 (Cortex-X925)
// Producer fires every 500 us (slot cadence). latency = consumer running (first instruction after wake) minus producer stamp
// taken immediately before the wake call. Consumer is SCHED_OTHER (no root); production scan consumers are prio 50.
// Variants: cond (mutex+condvar, signal after unlock, as nr_pdcch_passive_queue.c), futex, eventfd, yieldspin (bounded
// sched_yield loop), spin (busy poll), hybrid (timer wake d us before the deadline, then spin), samecore (cond, both on one cpu).
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <algorithm>
#include <vector>
#include <sys/syscall.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <linux/futex.h>

extern "C" {
void dft4096(int16_t *x, int16_t *y, unsigned char scale);
/* stubs for the OAI logging symbols oai_dfts_neon.c references (only on error paths) */
void *g_log = nullptr;
void logRecord_mt(const char *, const char *, int, int, int, const char *, ...) {}
void exit_function(const char *, const char *, int, const char *, int) { abort(); }
void write_file_matlab(const char *, const char *, void *, int, int, int) {}
}

static inline uint64_t ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static void pin(int cpu) { cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); sched_setaffinity(0, sizeof s, &s); }
static void sleep_until(uint64_t t) { timespec ts{(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)}; clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr); }

static const int N = 12000;
static const uint64_t PERIOD = 500000;
static int pcpu = 1, ccpu = 2;
static std::atomic<uint64_t> stamp{0};
static std::atomic<int> seq{0};
static std::atomic<bool> fin{false};
#define WAITING (seq.load() == seen && !fin.load())
static std::vector<double> lat;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int efd;
static int hyb_lead_us = 40;
static std::atomic<uint64_t> busy_ns{0}; // consumer on-cpu time estimate (spin loops)

static void report(const char *name, uint64_t cpu_ns)
{
  std::vector<double> v(lat.begin() + 200, lat.end()); // drop warm-up
  std::sort(v.begin(), v.end());
  auto q = [&](double p) { return v[(size_t)(p * (v.size() - 1))]; };
  printf("%-22s p50 %6.1f  p90 %6.1f  p99 %6.1f  max %7.1f us | consumer spin/busy %.0f%% of one core\n", name, q(.5), q(.9), q(.99), v.back(),
         100.0 * cpu_ns / ((double)N * PERIOD));
}

struct Arg { int mode; };

static void *consumer(void *p)
{
  const int mode = ((Arg *)p)->mode;
  pin(ccpu);
  prctl(PR_SET_TIMERSLACK, 1);
  int seen = 0;
  uint64_t spun = 0;
  for (int i = 0;; i++) {
    const uint64_t w0 = ns();
    if (fin.load() && seq.load() == seen) break;
    switch (mode) {
      case 0: // cond
      case 6:
        pthread_mutex_lock(&mu);
        while (WAITING) pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        break;
      case 1: // futex
        while (WAITING) syscall(SYS_futex, &seq, FUTEX_WAIT_PRIVATE, seen, nullptr, nullptr, 0);
        break;
      case 2: { // eventfd
        uint64_t v;
        while (WAITING) if (read(efd, &v, 8) < 0) break;
        break;
      }
      case 3: // bounded sched_yield spin (up to 300 us), then cond wait
      {
        const uint64_t lim = ns() + 300000;
        while (WAITING && ns() < lim) sched_yield();
        spun += ns() - w0;
        if (WAITING) {
          pthread_mutex_lock(&mu);
          while (WAITING) pthread_cond_wait(&cv, &mu);
          pthread_mutex_unlock(&mu);
        }
        break;
      }
      case 4: // busy spin
        while (WAITING) asm volatile("yield");
        spun += ns() - w0;
        break;
      case 5: { // hybrid: timer wake lead us before the next deadline, then spin
        const uint64_t dl = stamp.load() ? 0 : 0; (void)dl;
        // next deadline = known cadence: derived from the previous stamp
        extern uint64_t g_next_deadline;
        const uint64_t tw = g_next_deadline - (uint64_t)hyb_lead_us * 1000;
        sleep_until(tw);
        const uint64_t s0 = ns();
        while (WAITING) asm volatile("yield");
        spun += ns() - s0;
        break;
      }
    }
    const uint64_t t1 = ns();
    const int cur = seq.load();
    if (cur == seen + 1) lat.push_back((t1 - stamp.load()) / 1000.0);
    seen = cur;
  }
  busy_ns = spun;
  return nullptr;
}
uint64_t g_next_deadline = 0;

static void run(const char *name, int mode, int pc, int cc)
{
  pcpu = pc; ccpu = cc; fin = false; lat.clear(); lat.reserve(N + 10); seq = 0; stamp = 0; busy_ns = 0;
  Arg a{mode};
  pthread_t th; pthread_create(&th, nullptr, consumer, &a);
  pin(pcpu);
  prctl(PR_SET_TIMERSLACK, 1);
  usleep(20000);
  uint64_t dl = ns() + 5000000;
  for (int i = 0; i < N; i++) {
    g_next_deadline = dl;
    // consumer (hybrid) needs the deadline before it starts waiting: set one period ahead
    sleep_until(dl - 30000);
    while (ns() < dl) asm volatile("yield");
    stamp.store(ns());
    switch (mode) {
      case 0: case 3: case 6:
        pthread_mutex_lock(&mu); seq.fetch_add(1); pthread_mutex_unlock(&mu); pthread_cond_signal(&cv);
        break;
      case 1: seq.fetch_add(1); syscall(SYS_futex, &seq, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0); break;
      case 2: { seq.fetch_add(1); uint64_t one = 1; if (write(efd, &one, 8) < 0) {} break; }
      case 4: case 5: seq.store(seq.load() + 1, std::memory_order_release); break;
    }
    dl += PERIOD;
    g_next_deadline = dl;
  }
  fin.store(true);
  pthread_mutex_lock(&mu); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
  syscall(SYS_futex, &seq, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
  { uint64_t one = 1; if (write(efd, &one, 8) < 0) {} }
  pthread_join(th, nullptr);
  report(name, busy_ns.load());
}

// consumer timer wake lateness (what a "sleep until just before the slot" strategy sees), slack 50 us vs 1 ns
static void timer_lateness(int cpu, long slack_ns)
{
  pin(cpu); prctl(PR_SET_TIMERSLACK, slack_ns);
  std::vector<double> v;
  uint64_t dl = ns() + 2000000;
  for (int i = 0; i < 10000; i++) { sleep_until(dl); v.push_back((ns() - dl) / 1000.0); dl += PERIOD; }
  std::sort(v.begin(), v.end());
  printf("timer wake lateness  slack=%-6ldns p50 %6.1f p90 %6.1f p99 %6.1f max %7.1f us\n", slack_ns, v[v.size() / 2], v[(size_t)(v.size() * .9)], v[(size_t)(v.size() * .99)], v.back());
}

static void fft_bench(int cpu)
{
  pin(cpu);
  alignas(32) static int16_t x[4][2 * 4096 + 64], y[4][2 * 4096 + 64];
  for (int a = 0; a < 4; a++) for (int i = 0; i < 2 * 4096; i++) x[a][i] = (int16_t)(rand() & 0x3fff);
  for (int w = 0; w < 200; w++) dft4096(x[0], y[0], 1);
  const int R = 5000;
  uint64_t t0 = ns();
  for (int r = 0; r < R; r++) dft4096(x[0], y[0], 1);
  const double one = (ns() - t0) / 1000.0 / R;
  t0 = ns();
  for (int r = 0; r < R; r++) for (int a = 0; a < 4; a++) dft4096(x[a], y[a], 1);
  const double four = (ns() - t0) / 1000.0 / R;
  printf("dft4096 (OAI NEON) on cpu %d: 1 antenna %.2f us, 4 antennas back-to-back %.2f us\n", cpu, one, four);
}

int main(int argc, char **argv)
{
  const int pc = argc > 1 ? atoi(argv[1]) : 1, cc = argc > 2 ? atoi(argv[2]) : 2;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  efd = eventfd(0, EFD_SEMAPHORE);
  printf("# hand-off bench, producer cpu %d, consumer cpu %d, period 500 us, %d iterations/variant\n", pc, cc, N);
  fft_bench(cc);
  timer_lateness(cc, 50000);
  timer_lateness(cc, 1);
  run("cond (as queue.c)", 0, pc, cc);
  run("futex", 1, pc, cc);
  run("eventfd", 2, pc, cc);
  run("yield-spin<=300us+cond", 3, pc, cc);
  run("busy spin (yield insn)", 4, pc, cc);
  hyb_lead_us = 40; run("hybrid timer-40us+spin", 5, pc, cc);
  hyb_lead_us = 15; run("hybrid timer-15us+spin", 5, pc, cc);
  run("cond, same cpu", 6, pc, pc);
  return 0;
}
