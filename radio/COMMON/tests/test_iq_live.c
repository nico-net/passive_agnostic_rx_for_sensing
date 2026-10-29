/* MPCI-LIVE (A12) tests: the live-change state machine (iq_live) with recording hooks and with a real feed (iq_feed + iq_chan + fake radio), the slot thread parked
 * outside the radio gate on a paused ring (ru_park + the real shm_iq device), the CHAN demand following the running entries, the tuning mailbox and the per-cell
 * quiesce registry. Usage: test_iq_live <path to libshm_iq.so>. */
#define _GNU_SOURCE
#include "../iq_ddc.h"
#include "../iq_feed.h"
#include "../iq_live.h"
#include "../iq_span.h"
#include "../ru_park.h"
#include "common_lib.h"
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)
#define NCH 4
#define RATE 122.88e6
#define BLOCK 16384
#define CELL_RATE 30.72e6 /* 51 PRB at 30 kHz */

void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s %s %d\n", file, line, function, s, assert);
  exit(3);
}

/* ---------------------------------------------------------------- recording hooks */
typedef struct {
  char seq[1024]; /* "P1 T1 U1 R1 W" ... */
  int recentre_rc, recentres;
  double recentre_to;
  int fail_ue;
  char logs[16384];
} rec_t;
static void add(rec_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void add(rec_t *r, const char *fmt, ...)
{
  char b[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  if (r->seq[0]) strncat(r->seq, " ", sizeof r->seq - strlen(r->seq) - 1);
  strncat(r->seq, b, sizeof r->seq - strlen(r->seq) - 1);
}
static int h_pause(void *c, int s) { add(c, "P%d", s); return 0; }
static int h_resume(void *c, int s) { add(c, "R%d", s); return 0; }
static int h_chan(void *c, int s, double f) { (void)f; add(c, "T%d", s); return 0; }
static int h_ue(void *c, int s, const iq_live_tune_t *t) { (void)t; rec_t *r = c; add(r, "U%d", s); return r->fail_ue ? -1 : 0; }
static void h_wake(void *c) { add(c, "W"); }
static int h_recentre(void *c, double f) { rec_t *r = c; add(r, "C"); r->recentre_to = f; if (!r->recentre_rc) r->recentres++; return r->recentre_rc; }
static void h_span(void *c, const iq_span_plan_t *p) { (void)p; add(c, "S"); }
static void h_log(void *c, const char *l) { rec_t *r = c; strncat(r->logs, l, sizeof r->logs - strlen(r->logs) - 2); strcat(r->logs, "\n"); }
static iq_live_ops_t ops_for(rec_t *r)
{
  return (iq_live_ops_t){.chan_pause = h_pause, .chan_resume = h_resume, .chan_retune = h_chan, .ue_retune = h_ue, .wake = h_wake, .radio_recentre = h_recentre,
                         .span_changed = h_span, .log = h_log, .ctx = r};
}

/* ---------------------------------------------------------------- the span fixture: 51 PRB cells (30.72 MS/s), device rates 122.88 / 245.76 MS/s */
#define BASE 3669945000.0
static const iq_span_dev_t DEV = {.n_ranges = 2, .range = {{122.88e6, 122.88e6, 0}, {245.76e6, 245.76e6, 0}}, .max_rx = 4};
static iq_span_cell_t cell(const char *id, double centre) { return (iq_span_cell_t){id, centre, 51, 30e3, CELL_RATE, 4}; }
static iq_live_tune_t geom(void) { return (iq_live_tune_t){.n_rb = 51, .scs_hz = 30e3, .n_rx = 4}; }
static iq_live_tune_t tune(double centre, double cfo, int ssb) { iq_live_tune_t t = geom(); t.centre_hz = centre; t.cfo_hz = cfo; t.ssb = ssb; return t; }

/* c0 at BASE-30 MHz, c1 at BASE+45 MHz: span 93.4 MHz -> R = 122.88 MS/s, centre BASE+7.5 MHz; slots 2 and 3 are spares */
static void setup(iq_live_t *L, rec_t *r, iq_span_plan_t *plan)
{
  const iq_span_cell_t c[2] = {cell("A", BASE - 30e6), cell("B", BASE + 45e6)};
  iq_span_plan(&DEV, c, 2, plan);
  const iq_live_ops_t ops = ops_for(r);
  iq_live_init(L, &DEV, plan, &ops);
  const iq_live_tune_t g = geom();
  const iq_live_tune_t t0 = tune(c[0].centre_hz, 0, 100), t1 = tune(c[1].centre_hz, 50, 100);
  iq_live_slot_prepare(L, 0, "A", &g, &t0, CELL_RATE, 1);
  iq_live_slot_prepare(L, 1, "B", &g, &t1, CELL_RATE, 1);
  const iq_live_tune_t t2 = tune(BASE, 0, 100);
  iq_live_slot_prepare(L, 2, "spare1", &g, &t2, CELL_RATE, 0);
  iq_live_slot_prepare(L, 3, "spare2", &g, &t2, CELL_RATE, 0);
}

static int test_state_machine(void)
{
  iq_live_t L;
  rec_t r;
  iq_span_plan_t plan;
  memset(&r, 0, sizeof r);
  setup(&L, &r, &plan);
  CHECK(plan.ok && fabs(plan.rate - RATE) < 1 && fabs(plan.centre_hz - (BASE + 7.5e6)) < 1, "fixture plan rate %.0f centre %.0f", plan.rate, plan.centre_hz);
  CHECK(iq_live_state(&L, 0) == IQ_LIVE_SERVED && iq_live_state(&L, 1) == IQ_LIVE_SERVED && iq_live_state(&L, 2) == IQ_LIVE_EMPTY, "initial states");

  /* remove -> re-add the same cell into the same slot: pause; resume only (same tuning: no channelizer retune, no UE retune), one wake */
  CHECK(iq_live_remove(&L, 1, "removed") == IQ_LIVE_PARKED && !strcmp(r.seq, "P1"), "remove: %s", r.seq);
  CHECK(iq_live_served(&L) == 1, "one served");
  r.seq[0] = 0;
  const iq_live_tune_t tb = tune(BASE + 45e6, 50, 100);
  CHECK(iq_live_add(&L, 1, "B", &tb) == IQ_LIVE_SERVED && !strcmp(r.seq, "R1 W"), "re-add same slot: %s", r.seq);

  /* add into a prepared (empty) slot inside the capture: retune of the paused entry, UE tuning, resume */
  r.seq[0] = 0;
  const iq_live_tune_t tn = tune(BASE + 20e6, 120, 77);
  CHECK(iq_live_add(&L, 2, "N", &tn) == IQ_LIVE_SERVED && !strcmp(r.seq, "T2 U2 R2 W"), "add into spare: %s", r.seq);
  CHECK(r.recentres == 0, "no radio event");

  /* retune of a served cell: centre moved -> pause, channelizer, UE, resume; CFO only -> pause, UE, resume (no channelizer retune) */
  r.seq[0] = 0;
  const iq_live_tune_t tr = tune(BASE + 22e6, 120, 77);
  CHECK(iq_live_retune(&L, 2, &tr) == IQ_LIVE_SERVED && !strcmp(r.seq, "P2 T2 U2 R2 W"), "retune centre: %s", r.seq);
  r.seq[0] = 0;
  const iq_live_tune_t tc = tune(BASE + 22e6, 300, 77);
  CHECK(iq_live_retune(&L, 2, &tc) == IQ_LIVE_SERVED && !strcmp(r.seq, "P2 U2 R2 W"), "retune cfo only: %s", r.seq);
  r.seq[0] = 0;
  CHECK(iq_live_retune(&L, 2, &tc) == IQ_LIVE_SERVED && r.seq[0] == 0, "no change = no action: %s", r.seq);

  /* out of the span, needing another capture rate (a cell 150 MHz away): unserved, radio untouched, the other cells keep running */
  r.seq[0] = 0;
  const iq_live_tune_t tf = tune(BASE + 90e6, 0, 100);
  CHECK(iq_live_add(&L, 3, "far", &tf) == IQ_LIVE_UNSERVED && L.slot[3].reason == IQ_LIVE_REASON_RATE, "far cell: state %d reason %d", iq_live_state(&L, 3), L.slot[3].reason);
  CHECK(strchr(r.seq, 'C') == NULL && r.recentres == 0 && iq_live_state(&L, 0) == IQ_LIVE_SERVED, "radio untouched: %s", r.seq);
  CHECK(strstr(r.logs, "unserved reason=capture_rate_change"), "logged: %s", r.logs);

  /* geometry: a 106-PRB cell cannot take a 51-PRB slot */
  iq_live_tune_t tg = tune(BASE, 0, 100);
  tg.n_rb = 106;
  r.seq[0] = 0;
  iq_live_remove(&L, 3, "x");
  CHECK(iq_live_add(&L, 3, "wide", &tg) == IQ_LIVE_UNSERVED && L.slot[3].reason == IQ_LIVE_REASON_GEOMETRY, "geometry");
  CHECK(strstr(r.logs, "reason=geometry"), "geometry logged");
  return 0;
}

static int test_replan(void)
{
  iq_live_t L;
  rec_t r;
  iq_span_plan_t plan;
  memset(&r, 0, sizeof r);
  setup(&L, &r, &plan);
  /* 4 MHz below A: the span grows from 93.4 to 97.4 MHz (still one 122.88 MS/s capture) but the cell no longer fits the LO in force -> accepted re-plan, ONE radio retune */
  const iq_live_tune_t t = tune(BASE - 34e6, 0, 100);
  r.seq[0] = 0;
  CHECK(iq_live_add(&L, 2, "N", &t) == IQ_LIVE_SERVED, "replan accepted: state %d, %s", iq_live_state(&L, 2), r.logs);
  CHECK(!strcmp(r.seq, "C S T2 U2 R2 W") && r.recentres == 1, "order: %s", r.seq);
  CHECK(fabs(r.recentre_to - (BASE + 5.5e6)) < 1.0 && fabs(L.plan.centre_hz - r.recentre_to) < 1.0 && fabs(L.plan.rate - RATE) < 1, "new centre %.0f", r.recentre_to);
  CHECK(strstr(r.logs, "span_replan") && strstr(r.logs, "radio recentre"), "logged");
  /* the radio refuses: the cell stays unserved, the capture is the old one */
  iq_live_t L2;
  rec_t r2;
  memset(&r2, 0, sizeof r2);
  setup(&L2, &r2, &plan);
  r2.recentre_rc = -1;
  CHECK(iq_live_add(&L2, 2, "N", &t) == IQ_LIVE_UNSERVED && L2.slot[2].reason == IQ_LIVE_REASON_RADIO, "radio failure");
  CHECK(fabs(L2.plan.centre_hz - (BASE + 7.5e6)) < 1.0 && strchr(r2.seq, 'S') == NULL, "capture unchanged: %s", r2.seq);
  /* a served cell retuned out of the span: accepted when the re-plan keeps the rate, else parked-unserved and its entry is paused */
  iq_live_t L3;
  rec_t r3;
  memset(&r3, 0, sizeof r3);
  setup(&L3, &r3, &plan);
  const iq_live_tune_t far = tune(BASE + 150e6, 0, 100);
  CHECK(iq_live_retune(&L3, 1, &far) == IQ_LIVE_UNSERVED, "retune out of reach");
  CHECK(!strncmp(r3.seq, "P1", 2) && strchr(r3.seq, 'C') == NULL, "paused, radio untouched: %s", r3.seq);
  /* B leaves, A stays: the retry serves what was refused for the rate (now the single-cell span fits) */
  iq_live_remove(&L3, 1, "removed");
  const iq_live_tune_t near = tune(BASE - 5e6, 0, 100);
  r3.seq[0] = 0;
  CHECK(iq_live_add(&L3, 1, "B", &far) == IQ_LIVE_UNSERVED, "still far");
  CHECK(iq_live_retune(&L3, 1, &near) == IQ_LIVE_SERVED, "near fits (retry rule): state %d", iq_live_state(&L3, 1));
  /* a failing UE hook: unserved, hook_failed */
  iq_live_t L4;
  rec_t r4;
  memset(&r4, 0, sizeof r4);
  setup(&L4, &r4, &plan);
  r4.fail_ue = 1;
  const iq_live_tune_t ok = tune(BASE + 1e6, 0, 100);
  CHECK(iq_live_add(&L4, 2, "N", &ok) == IQ_LIVE_UNSERVED && L4.slot[2].reason == IQ_LIVE_REASON_FAIL, "hook failure");
  return 0;
}

/* ---------------------------------------------------------------- a real feed driven by iq_live */
typedef struct { uint64_t pos; int pace_us; } fake_t;
static int fake_read(void *ctx, int64_t *ts, void **buf, int n, int nch)
{
  fake_t *f = ctx;
  for (int a = 0; a < nch; ++a) {
    int16_t *d = buf[a];
    uint32_t r = 0x9e3779b9u * (uint32_t)(f->pos + (uint64_t)a + 1);
    for (int i = 0; i < 2 * n; ++i) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; d[i] = (int16_t)((int)(r % 2001) - 1000); }
  }
  *ts = 7000000003LL + (int64_t)f->pos;
  f->pos += (uint64_t)n;
  if (f->pace_us) usleep((useconds_t)f->pace_us);
  return n;
}
typedef struct { iq_feed_t *f; int entry[4]; double capture_centre; int recentred; } real_t;
static int g_pause(void *c, int s) { real_t *x = c; return iq_feed_cell_pause(x->f, x->entry[s]); }
static int g_resume(void *c, int s) { real_t *x = c; return iq_feed_cell_resume(x->f, x->entry[s]); }
static int g_chan(void *c, int s, double f) { real_t *x = c; return iq_feed_cell_retune(x->f, x->entry[s], f); }
static int g_ue(void *c, int s, const iq_live_tune_t *t) { (void)c; (void)s; (void)t; return 0; }
static int g_recentre(void *c, double f) { real_t *x = c; x->recentred++; return iq_feed_set_centre(x->f, f); }
static void g_log(void *c, const char *l) { (void)c; fprintf(stderr, "    %s\n", l); }
static void run(iq_feed_t *f, int reads)
{
  for (int i = 0; i < reads; ++i) {
    iq_feed_step(f);
    while (iq_chan_step(iq_feed_chan(f)) == 1) {}
  }
}
static uint64_t head(real_t *x, int s) { return atomic_load(&iq_feed_cell_ring(x->f, x->entry[s])->hdr->head); }
static double ring_centre(real_t *x, int s) { return shm_iq_ring_center(iq_feed_cell_ring(x->f, x->entry[s])); }

static int test_real_feed(void)
{
  iq_live_t L;
  rec_t r;
  iq_span_plan_t plan;
  memset(&r, 0, sizeof r);
  setup(&L, &r, &plan);
  fake_t fk = {0};
  iq_feed_cfg_t cfg = {.read = fake_read, .read_ctx = &fk, .nch = NCH, .rate = RATE, .centre_hz = plan.centre_hz, .name = "livefeed", .wide_capacity = 1 << 20, .block = BLOCK,
                       .backend = &iq_chan_backend_cpu, .radio_id = "fake"};
  real_t x = {.f = iq_feed_create(&cfg)};
  CHECK(x.f, "feed");
  const char *tag[4] = {"A", "B", "spare1", "spare2"};
  for (int k = 0; k < 4; ++k) {
    const int served = k < 2;
    const double centre = k == 0 ? BASE - 30e6 : k == 1 ? BASE + 45e6 : BASE;
    x.entry[k] = iq_feed_cell_add(x.f, tag[k], centre, 4, iq_ddc_cell_passband(51, 30e3, CELL_RATE, 0), !served);
    CHECK(x.entry[k] >= 0, "entry %d", k);
  }
  rec_t lr;
  memset(&lr, 0, sizeof lr);
  iq_live_ops_t ops = {.chan_pause = g_pause, .chan_resume = g_resume, .chan_retune = g_chan, .ue_retune = g_ue, .radio_recentre = g_recentre, .log = g_log, .ctx = &x};
  L.ops = ops;
  iq_chan_seek(iq_feed_chan(x.f), 0);
  run(x.f, 8);
  CHECK(head(&x, 0) > 0 && head(&x, 1) > 0 && head(&x, 2) == 0, "A and B flow, the spare does not");
  /* remove B: only B stops */
  iq_live_remove(&L, 1, "removed");
  run(x.f, 2);
  const uint64_t hA = head(&x, 0), hB = head(&x, 1);
  run(x.f, 8);
  CHECK(head(&x, 0) > hA && head(&x, 1) == hB, "A continues, B paused");
  CHECK(atomic_load(&iq_feed_cell_ring(x.f, x.entry[1])->hdr->dwell_until_ns) >= SHM_IQ_AWAY_PAUSED, "B marked paused for its consumer");
  /* the spare takes a new cell at another centre: its ring centre follows, data flows, A never stopped */
  const iq_live_tune_t tn = tune(BASE + 20e6, 0, 100);
  CHECK(iq_live_add(&L, 2, "N", &tn) == IQ_LIVE_SERVED, "add");
  run(x.f, 8);
  CHECK(head(&x, 2) > 0 && fabs(ring_centre(&x, 2) - (BASE + 20e6)) < 1.0, "spare ring tuned to the new cell: centre %.0f", ring_centre(&x, 2));
  /* retune A (centre moved by 1 MHz): its ring centre follows */
  const iq_live_tune_t ta = tune(BASE - 29e6, 0, 100);
  CHECK(iq_live_retune(&L, 0, &ta) == IQ_LIVE_SERVED, "retune A");
  run(x.f, 8);
  CHECK(fabs(ring_centre(&x, 0) - (BASE - 29e6)) < 1.0 && head(&x, 0) > hA, "A's ring tuned");
  /* re-activate the span with a re-plan: the wide ring takes the new centre, every running entry bumps its epoch (one resync of every cell) */
  const uint64_t e0 = atomic_load(&iq_feed_cell_ring(x.f, x.entry[0])->hdr->rf_epoch);
  const iq_live_tune_t tw = tune(BASE - 33e6, 0, 100); /* 4 MHz below A's old tuning: re-plan accepted (see test_replan) */
  iq_live_remove(&L, 2, "x");
  CHECK(iq_live_add(&L, 3, "W", &tw) == IQ_LIVE_SERVED && x.recentred == 1, "replan + recentre (%d)", x.recentred);
  run(x.f, 8);
  CHECK(fabs(iq_feed_centre(x.f) - L.plan.centre_hz) < 1.0 && atomic_load(&iq_feed_cell_ring(x.f, x.entry[0])->hdr->rf_epoch) > e0, "wide ring centre + epochs");
  iq_feed_destroy(x.f);
  (void)lr;
  return 0;
}

/* ---------------------------------------------------------------- the slot thread parked outside the radio gate */
static pthread_rwlock_t g_gate = PTHREAD_RWLOCK_INITIALIZER;
static int ge(void *c, int ex) { (void)c; if (ex) pthread_rwlock_wrlock(&g_gate); else pthread_rwlock_rdlock(&g_gate); return 1; }
static void gl(void *c) { (void)c; pthread_rwlock_unlock(&g_gate); }
static int gs(void *c) { (void)c; return 0; }
typedef struct { openair0_device_t *d; int ret; _Atomic int done; } rd_t;
static int read_once(void *p)
{
  rd_t *x = p;
  static int16_t b[NCH][2048 * 2];
  void *bp[NCH] = {b[0], b[1], b[2], b[3]};
  openair0_timestamp_t t;
  return x->d->trx_read_func(x->d, &t, bp, 1024, NCH);
}
static void *park_thread(void *p)
{
  rd_t *x = p;
  static const ru_park_gate_t gate = {.enter = ge, .leave = gl, .stopping = gs};
  x->ret = ru_park_read(&gate, 1, read_once, x);
  atomic_store(&x->done, 1);
  return NULL;
}
static int test_park(const char *lib)
{
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  int (*init)(openair0_device_t *, openair0_config_t *) = (int (*)(openair0_device_t *, openair0_config_t *))dlsym(h, "device_init");
  CHECK(init, "device_init");
  ru_park_reset();
  fake_t fk = {0};
  iq_feed_cfg_t cfg = {.read = fake_read, .read_ctx = &fk, .nch = NCH, .rate = RATE, .centre_hz = BASE, .name = "parkfeed", .wide_capacity = 1 << 20, .block = BLOCK,
                       .backend = &iq_chan_backend_cpu, .radio_id = "fake"};
  iq_feed_t *f = iq_feed_create(&cfg);
  CHECK(f, "feed");
  const int a = iq_feed_cell_add(f, "cellA", BASE, 4, iq_ddc_cell_passband(51, 30e3, CELL_RATE, 0), 0);
  const int b = iq_feed_cell_add(f, "cellB", BASE + 20e6, 4, iq_ddc_cell_passband(51, 30e3, CELL_RATE, 0), 0);
  CHECK(a >= 0 && b >= 0, "entries");
  iq_chan_seek(iq_feed_chan(f), 0);
  run(f, 6);
  openair0_config_t c;
  memset(&c, 0, sizeof c);
  c.sdr_addrs = (char *)"parkfeed_cellB"; c.sample_rate = CELL_RATE; c.rx_num_channels = NCH; c.num_rb_dl = 51;
  for (int i = 0; i < NCH; ++i) c.rx_freq[i] = BASE + 20e6;
  openair0_device_t dev;
  memset(&dev, 0, sizeof dev);
  CHECK(init(&dev, &c) == 0 && dev.trx_start_func(&dev) == 0, "RU of cell B");
  rd_t x = {.d = &dev, .ret = 99};
  /* a live ring: the park read is one ordinary read */
  pthread_t th;
  pthread_create(&th, NULL, park_thread, &x);
  for (int i = 0; i < 500 && !atomic_load(&x.done); ++i) { run(f, 1); usleep(500); }
  pthread_join(th, NULL);
  CHECK(x.ret == 1024 && ru_park_waits(1) == 0, "live read %d waits %lu", x.ret, ru_park_waits(1));
  /* pause B: the next read parks; while it is parked the EXCLUSIVE gate (restart / reinit / stop) is obtained at once, and nobody polls */
  iq_feed_cell_pause(f, b);
  { /* the backlog written before the pause is still read (nothing is lost); then the ring says PAUSED */
    int guard = 0, r;
    while ((r = read_once(&x)) > 0 && ++guard < 100000) {}
    CHECK(r == SHM_IQ_READ_PAUSED, "backlog drained, then PAUSED (got %d)", r);
  }
  atomic_store(&x.done, 0);
  x.ret = 99;
  pthread_create(&th, NULL, park_thread, &x);
  for (int i = 0; i < 500 && !ru_park_parked(1); ++i) usleep(1000);
  CHECK(ru_park_parked(1), "slot thread parked");
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  CHECK(pthread_rwlock_wrlock(&g_gate) == 0, "exclusive gate");
  pthread_rwlock_unlock(&g_gate);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  const double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-6;
  CHECK(ms < 5.0, "the exclusive gate was free while the read was parked (%.2f ms)", ms);
  const unsigned long w0 = ru_park_waits(1);
  usleep(300000);
  const unsigned long w1 = ru_park_waits(1);
  CHECK(w1 - w0 <= 300 / RU_PARK_POLL_MS + 2, "no busy loop: %lu retries in 300 ms (poll %d ms)", w1 - w0, RU_PARK_POLL_MS);
  CHECK(!atomic_load(&x.done), "still parked");
  /* resume: data flows again, the parked read returns, one forward gap in the ring */
  iq_feed_cell_resume(f, b);
  ru_park_wake();
  run(f, 6);
  for (int i = 0; i < 500 && !atomic_load(&x.done); ++i) { usleep(1000); run(f, 1); }
  pthread_join(th, NULL);
  CHECK(x.ret == 1024 && !ru_park_parked(1), "read completed after the resume: %d", x.ret);
  dev.trx_end_func(&dev);
  iq_feed_destroy(f);
  /* the tuning mailbox: one pending request per cell, taken once */
  ru_tune_t t = {.centre_hz = 1, .cfo_hz = 2, .ssb = 3}, g;
  CHECK(!ru_tune_take(1, &g), "empty");
  ru_tune_post(1, &t);
  t.ssb = 4;
  ru_tune_post(1, &t);
  CHECK(ru_tune_take(1, &g) && g.ssb == 4 && !ru_tune_take(1, &g) && !ru_tune_take(0, &g), "mailbox");
  return 0;
}

/* ---------------------------------------------------------------- CHAN demand follows the running entries */
static int g_demand, g_demand_calls;
static int l_acq(void *c, int b) { (void)c; (void)b; return 0; }
static void l_rel(void *c) { (void)c; }
static void l_dem(void *c, int t) { (void)c; g_demand = t; g_demand_calls++; }
static int l_max(void *c) { (void)c; return 6; }
static int test_demand(void)
{
  CHECK(iq_chan_demand_for(4, 2, 2, 4) == 4 && iq_chan_demand_for(4, 2, 1, 4) == 2 && iq_chan_demand_for(4, 2, 0, 4) == 0, "pure rule");
  CHECK(iq_chan_demand_for(3, 3, 1, 3) == 1 && iq_chan_demand_for(1, 2, 1, 1) == 1 && iq_chan_demand_for(4, 2, 3, 4) == 4 && iq_chan_demand_for(0, 2, 2, 4) == 0, "pure rule 2");
  static const iq_chan_lease_t lease = {.acquire = l_acq, .release = l_rel, .demand = l_dem, .max_tokens = l_max};
  iq_chan_set_lease(&lease);
  fake_t fk = {.pace_us = 250};
  iq_feed_cfg_t cfg = {.read = fake_read, .read_ctx = &fk, .nch = NCH, .rate = RATE, .centre_hz = BASE, .name = "demfeed", .wide_capacity = 1 << 22, .block = BLOCK,
                       .backend = &iq_chan_backend_cpu, .radio_id = "fake"};
  iq_feed_t *f = iq_feed_create(&cfg);
  CHECK(f, "feed");
  int e[3];
  for (int k = 0; k < 3; ++k) e[k] = iq_feed_cell_add(f, k == 0 ? "d0" : k == 1 ? "d1" : "d2", BASE + 10e6 * k, 4, iq_ddc_cell_passband(51, 30e3, CELL_RATE, 0), 0);
  CHECK(e[0] >= 0 && e[1] >= 0 && e[2] >= 0, "entries");
  CHECK(iq_feed_start(f, -1, 0, NULL, 0) == 0, "feed start");
  const int d0 = iq_chan_demand(iq_feed_chan(f));
  CHECK(d0 >= 1 && g_demand == d0, "demand registered at start: %d", d0);
  iq_feed_cell_pause(f, e[2]);
  iq_feed_cell_pause(f, e[1]);
  const int d1 = iq_chan_demand(iq_feed_chan(f));
  CHECK(d1 >= 1 && d1 <= d0 && d1 == iq_chan_demand_for(d0, 3, 1, d0) && g_demand == d1, "one running entry: %d of %d", d1, d0);
  iq_feed_cell_pause(f, e[0]);
  CHECK(iq_chan_demand(iq_feed_chan(f)) == 0 && g_demand == 0, "every entry paused: the tokens go back to the other classes");
  iq_feed_cell_resume(f, e[0]);
  iq_feed_cell_resume(f, e[1]);
  iq_feed_cell_resume(f, e[2]);
  CHECK(iq_chan_demand(iq_feed_chan(f)) == d0 && g_demand == d0, "resumed: back to %d (now %d)", d0, g_demand);
  iq_feed_stop(f);
  iq_feed_destroy(f);
  iq_chan_set_lease(NULL);
  printf("  CHAN demand %d -> %d -> 0 -> %d\n", d0, d1, d0);
  return 0;
}


/* mpci B6: a backend other than the CPU one (the GPU backend registers its one-token CPU part itself) also gives the reserved token back while no entry runs, and takes it
 * again on the first resume. The fake backend is the CPU backend under another address, so the channelizer takes the non-CPU branch of iq_chan_start. */
static iq_chan_backend_t g_fake_gpu_be;
static int test_demand_gpu_style(void)
{
  CHECK(iq_chan_demand_for(1, 2, 2, 1) == 1 && iq_chan_demand_for(1, 2, 1, 1) == 1 && iq_chan_demand_for(1, 2, 0, 1) == 0 && iq_chan_demand_for(1, 0, 1, 1) == 1, "pure rule, one token");
  static const iq_chan_lease_t lease = {.acquire = l_acq, .release = l_rel, .demand = l_dem, .max_tokens = l_max};
  iq_chan_set_lease(&lease);
  g_fake_gpu_be = iq_chan_backend_cpu;
  g_fake_gpu_be.name = "fake-gpu";
  g_demand = 0;
  g_demand_calls = 0;
  fake_t fk = {.pace_us = 250};
  iq_feed_cfg_t cfg = {.read = fake_read, .read_ctx = &fk, .nch = NCH, .rate = RATE, .centre_hz = BASE, .name = "demgpu", .wide_capacity = 1 << 22, .block = BLOCK,
                       .backend = &g_fake_gpu_be, .radio_id = "fake"};
  iq_feed_t *f = iq_feed_create(&cfg);
  CHECK(f, "feed");
  int e[2];
  for (int k = 0; k < 2; ++k) e[k] = iq_feed_cell_add(f, k == 0 ? "g0" : "g1", BASE + 10e6 * k, 4, iq_ddc_cell_passband(51, 30e3, CELL_RATE, 0), 0);
  CHECK(e[0] >= 0 && e[1] >= 0, "entries");
  CHECK(iq_feed_start(f, -1, 0, NULL, 0) == 0, "feed start");
  CHECK(iq_chan_demand(iq_feed_chan(f)) == 1, "one token while entries run: %d", iq_chan_demand(iq_feed_chan(f)));
  iq_feed_cell_pause(f, e[1]);
  CHECK(iq_chan_demand(iq_feed_chan(f)) == 1, "one entry still runs");
  iq_feed_cell_pause(f, e[0]);
  CHECK(iq_chan_demand(iq_feed_chan(f)) == 0 && g_demand == 0 && g_demand_calls >= 1, "every entry paused: the token goes back to the cells (calls %d)", g_demand_calls);
  iq_feed_cell_resume(f, e[1]);
  CHECK(iq_chan_demand(iq_feed_chan(f)) == 1 && g_demand == 1, "first resume takes it again: %d", g_demand);
  iq_feed_stop(f);
  iq_feed_destroy(f);
  iq_chan_set_lease(NULL);
  printf("  CHAN demand (non-CPU backend) 1 -> 0 -> 1\n");
  return 0;
}

/* ---------------------------------------------------------------- per-cell quiesce registry */
static int q_n[2], r_n[2];
static void q1(void *c, int cell) { (void)c; q_n[0] += 1 + cell * 10; }
static void r1(void *c, int cell) { (void)c; r_n[0] += 1 + cell * 10; }
static void q2(void *c, int cell) { (void)c; q_n[1] += 1 + cell * 10; }
static int test_hooks(void)
{
  ru_cell_hook_t h1 = {.name = "sense", .quiesce = q1, .resume = r1}, h2 = {.name = "prach", .quiesce = q2};
  CHECK(!ru_cell_hook_register(&h1) && !ru_cell_hook_register(&h2), "register");
  ru_cell_quiesce(2);
  ru_cell_resume(2);
  CHECK(q_n[0] == 21 && q_n[1] == 21 && r_n[0] == 21, "hooks run for the cell: %d %d %d", q_n[0], q_n[1], r_n[0]);
  return 0;
}

int main(int argc, char **argv)
{
  struct { const char *n; int (*f)(void); } t[] = {{"state machine", test_state_machine}, {"replan accept/refuse", test_replan}, {"real feed", test_real_feed},
                                                    {"CHAN demand", test_demand}, {"CHAN demand non-CPU backend", test_demand_gpu_style},
                                                    {"cell hooks", test_hooks}};
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); ++i) {
    printf("test_iq_live: %s\n", t[i].n);
    if (t[i].f()) return 1;
  }
  if (argc < 2) { fprintf(stderr, "no shm_iq path: park test skipped\n"); return 77; }
  printf("test_iq_live: slot thread parked outside the radio gate\n");
  if (test_park(argv[1])) return 1;
  printf("test_iq_live: ALL OK\n");
  return 0;
}
