/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI-FEEDSTART: start-up of the multi-cell wideband path in the receiver's real order, without a radio, repeated N times:
 *   radio streaming (a fake device that delivers samples at the full capture rate from its stream start on, blocking like a radio, behind the receiver's
 *   radio lifetime gate exactly as nrue_feed_read does) -> iq_feed_start (reader thread + channelizer, CPU or GPU backend, CHAN leases of a real core pool
 *   through the receiver's lease hooks) -> "RU start": the exclusive radio gate (nr_shutdown_radio_enter(1), as nrue_ru_start) around the shm_iq ring
 *   devices' device_init + trx_start of every cell (as nrue_ru_start_impl for ring RUs).
 * Thread timing is varied on purpose (it is not under the receiver's control): the reader thread's first scheduling latency (0 .. 3 ms before it reaches
 * the gate) and the stream start latency (the radio's first sample 0 .. 20 ms after the start, the timed PPS start).
 * A start passes when every cell's device is up within START_LIMIT_S and the radio's backlog (samples the device holds that nobody read) never exceeds
 * BACKLOG_LIMIT_MS: a reader that stops draining at start fills the radio ring (the X410's 500 ms usrp ring, OTA mp28n 2026-10-28). On a stall the test
 * prints FAIL and exits at once: the gate holder is stuck in device_init, there is nothing to unwind (the receiver asserts after 30 s).
 *   test_iq_feed_start <libshm_iq.so> cpu|gpu [starts=50]
 * gpu: 77 when there is no CUDA device or no channelizer module. */
#define _GNU_SOURCE
#include "../iq_feed.h"
#include "common_lib.h"
#include "nr_core_pool.h"
#include "nr_shutdown.h"
#ifdef FEEDSTART_GPU
#include "nr_gpu_platform.h"
#include "shm_iq_chan_gpu.h"
#endif
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NCH 4
#define TICK0 7000000003LL
#define BACKLOG_LIMIT_MS 250.0 /* half the receiver's 500 ms radio ring */
#define START_LIMIT_S 5.0

/* the shm_iq module calls this through the executable (ENABLE_EXPORTS); an AssertFatal there ends the test */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s %s %d\n", file, line, function, s, assert);
  _exit(3);
}

static uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void sleep_until_ns(uint64_t t)
{
  struct timespec ts = {.tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* ---------------------------------------------------------------- fake radio: full rate from its stream start, behind the receiver's gate */
typedef struct {
  double rate;
  uint64_t t0_ns;           /* stream start: sample 0 exists at t0, sample k at t0 + k / rate (UINT64_MAX / 2: not streaming yet) */
  int first_delay_us;       /* the reader thread's first scheduling latency, before it reaches the gate */
  int calls;
  _Atomic uint64_t consumed; /* samples per channel the reader took */
} radio_t;

static int radio_read(void *ctx, int64_t *ts, void **buf, int n, int nch)
{
  (void)buf; (void)nch; /* sample values do not matter for start-up; the ring memory is what the reader reserved */
  radio_t *r = ctx;
  if (r->calls++ == 0 && r->first_delay_us) usleep((useconds_t)r->first_delay_us);
  if (!nr_shutdown_radio_enter(0)) /* as nrue_feed_read */
    return -1;
  const uint64_t pos = atomic_load(&r->consumed);
  while (__atomic_load_n(&r->t0_ns, __ATOMIC_ACQUIRE) >= UINT64_MAX / 2) usleep(100); /* not streaming yet */
  sleep_until_ns(__atomic_load_n(&r->t0_ns, __ATOMIC_ACQUIRE) + (uint64_t)llround(1e9 * (double)(pos + (uint64_t)n) / r->rate)); /* blocking: until the samples exist */
  *ts = TICK0 + (int64_t)pos;
  atomic_store(&r->consumed, pos + (uint64_t)n);
  nr_shutdown_radio_leave();
  return n;
}
static double backlog_ms(radio_t *r)
{
  const uint64_t t = now_ns(), t0 = __atomic_load_n(&r->t0_ns, __ATOMIC_ACQUIRE);
  if (t <= t0) return 0;
  return 1e3 * ((double)(t - t0) * 1e-9 - (double)atomic_load(&r->consumed) / r->rate);
}

/* ---------------------------------------------------------------- the receiver's CHAN lease hooks (executables/nr-uesoftmodem.c) on a real core pool */
static int chan_acquire(void *ctx, int blocking) { (void)ctx; return nr_pool_site_chan_acquire(blocking); }
static void chan_release(void *ctx) { (void)ctx; nr_pool_site_chan_release(); }
static void chan_demand(void *ctx, int tokens) { (void)ctx; nr_pool_site_chan_set_demand(tokens); }
static int chan_max(void *ctx) { (void)ctx; return nr_pool_site_chan_max(); }

/* ---------------------------------------------------------------- geometry: the two OTA pairs of 2026-10-27/28 */
typedef struct {
  const char *tag;
  double centre_hz;
  int n_rb, decim;
} cell_t;
typedef struct {
  double rate, centre_hz;
  int n;
  cell_t c[2];
} geom_t;
/* P: 415 + 923 at 245.76 MS/s (decim 2 / 2, -/+39.99 MHz) */
static const geom_t GEOM_P = {245.76e6, 3720000000.0, 2, {{"pci415", 3680010000.0, 217, 2}, {"pci923", 3759990000.0, 217, 2}}};
/* Q: 415 + 403 at 122.88 MS/s (decim 1 at +10.065 MHz, decim 4 at -39.945 MHz) */
static const geom_t GEOM_Q = {122.88e6, 3669945000.0, 2, {{"pci415", 3680010000.0, 217, 1}, {"pci403", 3630000000.0, 51, 4}}};

/* ---------------------------------------------------------------- "RU start": exclusive gate around the ring devices (nrue_ru_start + nrue_ru_start_impl) */
typedef int (*dev_init_fn)(openair0_device_t *, openair0_config_t *);
typedef struct {
  dev_init_fn init;
  int n;
  openair0_device_t dev[2];
  openair0_config_t cfg[2];
  char ring[2][64];
  _Atomic int done, rc;
} ru_t;

static void *ru_start(void *p)
{
  ru_t *u = p;
  int rc = -1;
  if (nr_shutdown_radio_enter(1)) {
    rc = 0;
    for (int k = 0; k < u->n && !rc; ++k) {
      if (u->init(&u->dev[k], &u->cfg[k]) < 0 || u->dev[k].trx_start_func(&u->dev[k]) != 0) rc = -1;
    }
    nr_shutdown_radio_leave();
  }
  atomic_store(&u->rc, rc);
  atomic_store(&u->done, 1);
  return NULL;
}

static const iq_chan_backend_t *g_backend;

/* iq_feed_start with the stream start placed after the feed threads exist (see one_start). The reader may already be blocked in its first read: it
 * waits until t0 is known (a radio before its first packet), so t0 is published atomically. */
static int iq_feed_start_t0(iq_feed_t *f, radio_t *r, int pps_delay_us)
{
  const int rc = iq_feed_start(f, -1, 0, NULL, 0);
  __atomic_store_n(&r->t0_ns, now_ns() + (uint64_t)pps_delay_us * 1000ull, __ATOMIC_RELEASE);
  return rc;
}

/* one start: 0 = pass; a stall exits the process */
static int one_start(dev_init_fn init, const geom_t *g, int i, int reader_delay_us, int pps_delay_us, double *ru_ms, double *bl_max)
{
  radio_t r = {.rate = g->rate, .first_delay_us = reader_delay_us};
  char name[32];
  snprintf(name, sizeof(name), "fs%d", i);
  const iq_feed_cfg_t fc = {.read = radio_read, .read_ctx = &r, .nch = NCH, .rate = g->rate, .centre_hz = g->centre_hz, .name = name,
                            .wide_capacity = 1u << 22, .narrow_budget_bytes = (size_t)200 << 20, .backend = g_backend, .radio_id = "fake"};
  iq_feed_t *f = iq_feed_create(&fc);
  if (!f) { fprintf(stderr, "FAIL start %d: iq_feed_create: %s\n", i, strerror(errno)); return 1; }
  for (int k = 0; k < g->n; ++k) {
    const double pb = iq_ddc_cell_passband(g->c[k].n_rb, 30e3, g->rate / g->c[k].decim, 0);
    if (iq_feed_cell_add(f, g->c[k].tag, g->c[k].centre_hz, g->c[k].decim, pb, 0) < 0) { fprintf(stderr, "FAIL start %d: cell add %s\n", i, g->c[k].tag); return 1; }
  }
  ru_t u = {.init = init, .n = g->n};
  for (int k = 0; k < g->n; ++k) {
    snprintf(u.ring[k], sizeof(u.ring[k]), "%s_%s", name, g->c[k].tag);
    openair0_config_t *c = &u.cfg[k];
    c->sdr_addrs = u.ring[k];
    c->sample_rate = g->rate / g->c[k].decim;
    c->rx_num_channels = NCH;
    c->num_rb_dl = g->c[k].n_rb;
    for (int a = 0; a < NCH; ++a) c->rx_freq[a] = g->c[k].centre_hz;
  }
  /* the receiver: radio started, then the feed, then the RUs. The radio's stream begins at its timed start (the X410: the next PPS + 1 s, i.e. after the
   * channelizer's start-up bench); the test puts sample 0 at the moment the feed threads exist + pps_delay, which is the tightest case the receiver meets.
   * Until then the fake device's read blocks (as a radio before its first packet). */
  r.t0_ns = UINT64_MAX / 2;
  const int rc = iq_feed_start_t0(f, &r, pps_delay_us);
  if (rc) { fprintf(stderr, "FAIL start %d: iq_feed_start %d\n", i, rc); return 1; }
  pthread_t th;
  const uint64_t t_ru = now_ns();
  pthread_create(&th, NULL, ru_start, &u);
  double bmax = 0;
  for (;;) {
    const double b = backlog_ms(&r);
    if (b > bmax) bmax = b;
    if (atomic_load(&u.done)) break;
    if (b > BACKLOG_LIMIT_MS || now_ns() - t_ru > (uint64_t)(START_LIMIT_S * 1e9)) {
      iq_feed_stats_t st;
      iq_feed_stats(f, &st);
      fprintf(stderr, "FAIL start %d (%s, reader late %d us, stream start +%d us): RU start not done after %.0f ms, radio backlog %.0f ms (reader reads=%llu, "
                      "consumed=%llu): the reader is not draining the radio while the RU start holds the radio gate\n", i, st.chan.backend, reader_delay_us, pps_delay_us,
              (now_ns() - t_ru) * 1e-6, b, (unsigned long long)st.reads, (unsigned long long)atomic_load(&r.consumed));
      fflush(stderr);
      _exit(1);
    }
    usleep(500);
  }
  pthread_join(th, NULL);
  *ru_ms = (now_ns() - t_ru) * 1e-6;
  if (atomic_load(&u.rc)) { fprintf(stderr, "FAIL start %d: RU start rc %d\n", i, atomic_load(&u.rc)); return 1; }
  /* the cells' RUs read their rings (the UE read path takes the shared gate) while the feed keeps draining */
  static int16_t buf[NCH][4096 * 2];
  void *bp[NCH] = {buf[0], buf[1], buf[2], buf[3]};
  for (int k = 0; k < g->n; ++k) {
    openair0_timestamp_t t = 0;
    for (int j = 0; j < 8; ++j) {
      if (!nr_shutdown_radio_enter(0)) return 1;
      const int got = u.dev[k].trx_read_func(&u.dev[k], &t, bp, 4096, NCH);
      nr_shutdown_radio_leave();
      if (got != 4096) { fprintf(stderr, "FAIL start %d: cell %s read %d\n", i, g->c[k].tag, got); return 1; }
      const double b = backlog_ms(&r);
      if (b > bmax) bmax = b;
    }
  }
  if (bmax > BACKLOG_LIMIT_MS) { fprintf(stderr, "FAIL start %d: radio backlog %.0f ms\n", i, bmax); return 1; }
  *bl_max = bmax;
  iq_feed_stop(f);
  for (int k = 0; k < g->n; ++k) u.dev[k].trx_end_func(&u.dev[k]);
  iq_feed_destroy(f);
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 3) { fprintf(stderr, "usage: %s <libshm_iq.so> cpu|gpu [starts]\n", argv[0]); return 2; }
  setvbuf(stdout, NULL, _IOLBF, 0);
  const int gpu = !strcmp(argv[2], "gpu");
  const int starts = argc > 3 ? atoi(argv[3]) : 50;
  void *h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "dlopen %s: %s\n", argv[1], dlerror()); return 2; }
  dev_init_fn init = (dev_init_fn)dlsym(h, "device_init");
  if (!init) return 2;
  const geom_t *g = gpu ? &GEOM_P : &GEOM_Q;
  g_backend = &iq_chan_backend_cpu;
#ifdef FEEDSTART_GPU
  if (gpu) {
    const nr_gpu_caps_t *caps = nr_gpu_platform_probe();
    const iq_chan_gpu_api_t *A = iq_chan_gpu_load();
    if (!caps || caps->model == NR_GPU_NONE || !A || !A->available()) { printf("SKIP no CUDA device / channelizer module\n"); return 77; }
    g_backend = A->backend;
  }
#else
  if (gpu) { printf("SKIP built without CUDA\n"); return 77; }
#endif
  /* a real core pool over the CPUs this process may use (derived), installed as the site pool the receiver's lease hooks reach */
  cpu_set_t m;
  int cores[CPU_SETSIZE], n = 0;
  if (sched_getaffinity(0, sizeof(m), &m)) return 2;
  for (int c = 0; c < CPU_SETSIZE && n < NR_POOL_MAX_TOKENS; ++c)
    if (CPU_ISSET(c, &m)) cores[n++] = c;
  nr_pool_t *pool = nr_pool_create(cores, NULL, n, NULL);
  if (!pool) return 2;
  nr_pool_site_set_for_test(pool);
  static const iq_chan_lease_t lease = {.acquire = chan_acquire, .release = chan_release, .demand = chan_demand, .max_tokens = chan_max};
  iq_chan_set_lease(&lease);
  printf("FEEDSTART %s backend, %.2f MS/s x %d ch, cells %s/d%d + %s/d%d, pool %d tokens (CHAN max %d), %d starts\n", g_backend->name, g->rate / 1e6, NCH, g->c[0].tag,
         g->c[0].decim, g->c[1].tag, g->c[1].decim, n, nr_pool_chan_cap(pool), starts);
  static const int reader_delay[] = {0, 200, 1000, 3000};
  static const int pps_delay[] = {0, 2000, 20000};
  double ru_max = 0, bl_max = 0;
  for (int i = 0; i < starts; ++i) {
    const int rd = reader_delay[i % 4], pd = pps_delay[(i / 4) % 3];
    double ru = 0, bl = 0;
    if (one_start(init, g, i, rd, pd, &ru, &bl)) { printf("FAILED at start %d\n", i); return 1; }
    if (ru > ru_max) ru_max = ru;
    if (bl > bl_max) bl_max = bl;
    printf("  start %2d ok: reader late %4d us, stream start +%5d us, RU start %.1f ms, radio backlog max %.1f ms\n", i, rd, pd, ru, bl);
  }
  iq_chan_set_lease(NULL);
  printf("ALL OK %d/%d starts (%s): RU start max %.1f ms, radio backlog max %.1f ms (limit %.0f)\n", starts, starts, g_backend->name, ru_max, bl_max, BACKLOG_LIMIT_MS);
  return 0;
}
