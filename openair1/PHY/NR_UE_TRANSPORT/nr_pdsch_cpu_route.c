/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_pdsch_cpu_route.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include "common/utils/LOG/log.h"

#define NCLS 16
static __thread bool t_cpu;
static int g_small = -1, g_small_nl = 2, g_batch = 0, g_slots = 3;
static _Atomic int g_inflight;
static _Atomic uint64_t g_ew[2][NCLS]; /* EWMA body ns: [0] GPU path, [1] CPU path */
static _Atomic uint32_t g_n[2][NCLS];
static _Atomic uint64_t g_calls, g_to_cpu, g_waits, g_wait_ns, g_wait_cpu, g_wait_go;

static inline uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void init(void)
{
  const char *e = getenv("ISAC_PDSCH_CPU_SMALL");
  g_small = e ? atoi(e) : 0;
  if ((e = getenv("ISAC_PDSCH_CPU_SMALL_NL")) && atoi(e) > 0)
    g_small_nl = atoi(e);
  g_batch = (e = getenv("ISAC_PDSCH_GPU_BATCH")) && atoi(e) == 1;
  if ((e = getenv("ISAC_PDSCH_GPU_BATCH_SLOTS")) && atoi(e) > 0)
    g_slots = atoi(e);
}
bool nr_pdsch_route_cpu(void) { return t_cpu; }

nr_pdsch_route_t nr_pdsch_route_begin(int nprb, int layers, bool probe)
{
  nr_pdsch_route_t r = {0, -1, 0, 0};
  t_cpu = false;
  if (g_small < 0)
    init();
  if (g_small <= 0 && !g_batch)
    return r;
  const uint64_t n = atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed) + 1;
  if (layers < 1)
    layers = 1;
  int c = 0;
  for (unsigned u = (unsigned)(nprb * layers); u > 1 && c < NCLS - 1; u >>= 1)
    c++; /* size class = floor(log2(PRB x layers)) */
  r.cls = (int8_t)c;
  const bool cand = g_small > 0 && nprb <= g_small && layers <= g_small_nl;
  uint64_t cpu_est = 0;
  if (cand) {
    const uint32_t ng = atomic_load_explicit(&g_n[0][c], memory_order_relaxed), nc = atomic_load_explicit(&g_n[1][c], memory_order_relaxed);
    const uint64_t eg = atomic_load_explicit(&g_ew[0][c], memory_order_relaxed), ec = atomic_load_explicit(&g_ew[1][c], memory_order_relaxed);
    cpu_est = nc ? ec : 0;
    bool cpu;
    if (nc < 4 && ng >= 4)
      cpu = (n % 8) == 0; /* explore the CPU arm */
    else if (ng < 4)
      cpu = false;        /* GPU arm first */
    else {
      cpu = ec < eg;
      if ((n % 64) == 0)
        cpu = !cpu;       /* refresh the losing arm */
    }
    if (cpu) {
      r.cpu = 1;
      t_cpu = true;
      atomic_fetch_add_explicit(&g_to_cpu, 1, memory_order_relaxed);
    }
  }
  if (!r.cpu && g_batch) {
    const int lim = probe && g_slots > 1 ? g_slots - 1 : g_slots;
    const uint64_t budget = cpu_est ? cpu_est : 3000000ull, w0 = now_ns();
    bool waited = false;
    for (;;) {
      int cur = atomic_load_explicit(&g_inflight, memory_order_relaxed);
      if (cur < lim && atomic_compare_exchange_weak(&g_inflight, &cur, cur + 1)) {
        r.held = 1;
        break;
      }
      waited = true;
      if (now_ns() - w0 > budget) { /* waited as long as the CPU path would take, or 3 ms: stop queueing */
        if (cpu_est) {
          r.cpu = 1;
          t_cpu = true;
          atomic_fetch_add_explicit(&g_wait_cpu, 1, memory_order_relaxed);
        } else {
          atomic_fetch_add(&g_inflight, 1);
          r.held = 1;
          atomic_fetch_add_explicit(&g_wait_go, 1, memory_order_relaxed);
        }
        break;
      }
      sched_yield();
    }
    if (waited) {
      atomic_fetch_add_explicit(&g_waits, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_wait_ns, now_ns() - w0, memory_order_relaxed);
    }
  }
  r.t0 = now_ns(); /* body time excludes the gate wait: the gate cost is reported separately */
  if (n % 4000 == 0)
    LOG_I(PHY, "SENSING: CPUROUTE calls=%lu cpu=%lu gate_waits=%lu wait_mean_us=%.1f wait_to_cpu=%lu wait_forced=%lu\n", (unsigned long)n,
          (unsigned long)atomic_load(&g_to_cpu), (unsigned long)atomic_load(&g_waits),
          atomic_load(&g_waits) ? (double)atomic_load(&g_wait_ns) / (double)atomic_load(&g_waits) / 1000.0 : 0.0,
          (unsigned long)atomic_load(&g_wait_cpu), (unsigned long)atomic_load(&g_wait_go));
  if (n % 4000 == 0 && g_small > 0) {
    char b[512];
    int u = 0;
    for (int k = 0; k < NCLS && u < 440; k++)
      if (atomic_load(&g_n[0][k]) + atomic_load(&g_n[1][k]))
        u += snprintf(b + u, sizeof(b) - u, "c%d[gpu %u/%.0fus cpu %u/%.0fus] ", k, (unsigned)atomic_load(&g_n[0][k]),
                      (double)atomic_load(&g_ew[0][k]) / 1e3, (unsigned)atomic_load(&g_n[1][k]), (double)atomic_load(&g_ew[1][k]) / 1e3);
    LOG_I(PHY, "SENSING: CPUROUTE classes(log2 PRBxlayers) %s\n", b);
  }
  return r;
}

void nr_pdsch_route_end(nr_pdsch_route_t *r)
{
  t_cpu = false;
  if (r->held)
    atomic_fetch_sub_explicit(&g_inflight, 1, memory_order_release);
  if (r->cls < 0 || g_small <= 0)
    return;
  uint64_t d = now_ns() - r->t0;
  if (d > 8000000ull) /* process-wide stalls (30-300 ms) hit both paths alike: clip so one cannot flip the choice */
    d = 8000000ull;
  _Atomic uint64_t *e = &g_ew[r->cpu][r->cls];
  const uint32_t k = atomic_fetch_add_explicit(&g_n[r->cpu][r->cls], 1, memory_order_relaxed);
  const uint64_t o = atomic_load_explicit(e, memory_order_relaxed);
  atomic_store_explicit(e, k ? o - o / 8 + d / 8 : d, memory_order_relaxed); /* racy EWMA: an estimate only */
}
