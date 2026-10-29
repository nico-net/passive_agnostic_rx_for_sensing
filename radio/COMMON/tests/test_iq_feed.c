/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G6: radio -> wideband ring -> channelizer -> per-cell rings (iq_feed), with a FAKE radio (synthetic wideband IQ, deterministic by absolute sample
 * index, timestamps as the radio gives them). Oracle = a fresh iq_ddc per (cell, channel) fed the same wideband stream in the channelizer's blocks
 * (iq_ddc == the frozen reference to 0 LSB, test_iq_ddc_equiv): every comparison is "0 differing samples".
 *   test_iq_feed <path of libshm_iq.so>
 * Cases: two cells bit-exact (122.88 MS/s capture, Vodafone 415 + Iliad 403 geometry of the planner); frozen device stamps; short reads; a radio gap;
 * live pause / resume of one cell while the other stays bit-exact; a cell created paused; the reader thread + channelizer thread end to end; the
 * shm_iq consumer device (the cell's RU) reading its cell ring (data, timestamps, a paused ring at attach, stop releases a blocked read);
 * the input-mode selection (N = 1 is always the direct radio path). */
#define _GNU_SOURCE
#include "../iq_feed.h"
#include "../iq_span.h"
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
#define CENTRE 3669945000.0
#define TICK0 7000000003LL /* radio clock of the first sample: deliberately not a multiple of any decimation */
#define BLOCK 16384

/* the shm_iq module calls these through the executable (ENABLE_EXPORTS) */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function %s:%d %s %s %d\n", file, line, function, s, assert);
  exit(3);
}

/* ---------------------------------------------------------------- deterministic wideband input: noise + two tones per channel, by absolute index */
static int16_t *g_tab[NCH];
static uint64_t g_T;
static void table_init(uint64_t T)
{
  g_T = T;
  for (int a = 0; a < NCH; ++a) {
    g_tab[a] = malloc((size_t)T * 4);
    uint32_t r = 0x51ed27u * (uint32_t)(a + 3);
    for (uint64_t i = 0; i < T; ++i) {
      r ^= r << 13; r ^= r >> 17; r ^= r << 5;
      const double t = (double)i / RATE;
      /* tones at the two cell centres (+10.065 / -39.945 MHz from the capture centre) plus noise */
      const double re = 2000 * cos(2 * M_PI * 10.065e6 * t) + 1500 * cos(2 * M_PI * -39.945e6 * t + a) + (double)((int)(r % 801) - 400);
      const double im = 2000 * sin(2 * M_PI * 10.065e6 * t) + 1500 * sin(2 * M_PI * -39.945e6 * t + a) + (double)((int)((r >> 11) % 801) - 400);
      g_tab[a][2 * i] = (int16_t)lrint(re);
      g_tab[a][2 * i + 1] = (int16_t)lrint(im);
    }
  }
}
static void fill(int ch, uint64_t pos, int n, int16_t *dst)
{
  uint64_t i = pos % g_T;
  int done = 0;
  while (done < n) {
    const uint64_t k = (uint64_t)(n - done) < g_T - i ? (uint64_t)(n - done) : g_T - i;
    memcpy(dst + 2 * done, g_tab[ch] + 2 * i, k * 4);
    done += (int)k; i = 0;
  }
}

/* ---------------------------------------------------------------- fake radio */
typedef struct {
  uint64_t pos;        /* next absolute sample index */
  int frozen;          /* every read returns the stream start timestamp (X410 34B2AF1) */
  int short_every;     /* every k-th read returns n - short_by samples */
  int short_by;
  int gap_at;          /* read number before which gap_len samples are lost (forward timestamp jump) */
  uint64_t gap_len;
  int fail_at;         /* read number that fails (-1) */
  int reads;
  _Atomic int stop;    /* a blocked "radio" read returns -1 */
  int pace_us;         /* thread test: sleep per read */
} fake_t;

static int fake_read(void *ctx, int64_t *ts, void **buf, int n, int nch)
{
  fake_t *f = ctx;
  if (atomic_load(&f->stop)) return -1;
  ++f->reads;
  if (f->fail_at && f->reads == f->fail_at) return -1;
  if (f->gap_at && f->reads == f->gap_at) f->pos += f->gap_len;
  int got = n;
  if (f->short_every && f->reads % f->short_every == 0) got = n - f->short_by;
  for (int a = 0; a < nch; ++a) fill(a, f->pos, got, buf[a]);
  *ts = f->frozen ? TICK0 : TICK0 + (int64_t)f->pos;
  f->pos += (uint64_t)got;
  if (f->pace_us) usleep((useconds_t)f->pace_us);
  return got;
}

/* ---------------------------------------------------------------- oracle */
typedef struct { iq_ddc_t d[NCH]; int decim; double shift; } oracle_t;
static int oracle_init(oracle_t *o, int decim, double passband, double shift)
{
  o->decim = decim; o->shift = shift;
  for (int a = 0; a < NCH; ++a) {
    if (iq_ddc_init(&o->d[a], RATE, decim, passband, IQ_CHAN_DEFAULT_ATTEN_DB)) return -1;
    iq_ddc_set_shift(&o->d[a], shift);
  }
  return 0;
}
static void oracle_free(oracle_t *o) { for (int a = 0; a < NCH; ++a) iq_ddc_free(&o->d[a]); }

/* compare `nb` channelizer blocks of the cell ring starting at cell ring index j0 with the oracle fed wideband blocks from absolute index p0 */
static long compare(oracle_t *o, shm_iq_ring_t *cell, uint64_t j0, uint64_t p0, int nb)
{
  static int16_t in[BLOCK * 2], ref[BLOCK * 2], got[NCH][BLOCK * 2];
  int16_t *dst[NCH] = {got[0], got[1], got[2], got[3]};
  const int m = BLOCK / o->decim;
  long bad = 0;
  for (int b = 0; b < nb; ++b) {
    if (shm_iq_ring_copy(cell, j0 + (uint64_t)b * m, m, NCH, dst) != 0) return -1;
    for (int a = 0; a < NCH; ++a) {
      fill(a, p0 + (uint64_t)b * BLOCK, BLOCK, in);
      if (o->decim == 1 && o->shift == 0) memcpy(ref, in, (size_t)BLOCK * 4);
      else iq_ddc_process(&o->d[a], in, BLOCK, ref);
      for (int i = 0; i < 2 * m; ++i) bad += ref[i] != got[a][i];
    }
  }
  return bad;
}

/* the planner's entries for Vodafone 415 (217 PRB) + Iliad 403 (51 PRB) in the 122.88 MS/s capture (test_iq_span) */
static void plan_pair(iq_span_plan_t *p)
{
  const iq_span_dev_t dev = {.n_ranges = 2, .range = {{122.88e6, 122.88e6, 0}, {245.76e6, 245.76e6, 0}}, .max_rx = 4};
  const iq_span_cell_t c[2] = {{"pci415", 3680010000.0, 217, 30e3, 122.88e6, 4}, {"pci403", 3630000000.0, 51, 30e3, 30.72e6, 4}};
  iq_span_plan(&dev, c, 2, p);
}

static iq_feed_t *make_feed(fake_t *fk, const char *name, uint64_t cap)
{
  iq_feed_cfg_t cfg = {.read = fake_read, .read_ctx = fk, .nch = NCH, .rate = RATE, .centre_hz = CENTRE, .name = name, .wide_capacity = cap, .block = BLOCK,
                       .backend = &iq_chan_backend_cpu, .radio_id = "fake"};
  return iq_feed_create(&cfg);
}

/* drive radio reads and channelizer blocks on this thread: `reads` radio reads, channelizer steps until it has consumed everything published */
static void run(iq_feed_t *f, int reads)
{
  for (int i = 0; i < reads; ++i) {
    iq_feed_step(f);
    while (iq_chan_step(iq_feed_chan(f)) == 1) {}
  }
}

static int test_two_cells(int frozen, int short_every)
{
  fake_t fk = {.frozen = frozen, .short_every = short_every, .short_by = 4104};
  char name[32];
  snprintf(name, sizeof(name), "g6t2_%d_%d", frozen, short_every);
  iq_feed_t *f = make_feed(&fk, name, 1 << 20);
  CHECK(f, "create");
  iq_span_plan_t p;
  plan_pair(&p);
  CHECK(p.ok && p.n_served == 2 && p.rate == RATE && fabs(p.centre_hz - CENTRE) < 1e-3, "plan");
  const int a = iq_feed_cell_add(f, "pci415", CENTRE + p.cell[0].shift_hz, p.cell[0].decim, p.cell[0].passband_hz, 0);
  const int b = iq_feed_cell_add(f, "pci403", CENTRE + p.cell[1].shift_hz, p.cell[1].decim, p.cell[1].passband_hz, 0);
  CHECK(a >= 0 && b >= 0, "add %d %d", a, b);
  char nm[96];
  snprintf(nm, sizeof(nm), "%s_pci403", name);
  shm_iq_ring_t view;
  CHECK(shm_iq_ring_attach(&view, nm) == 0, "cell ring registered as %s", nm);
  shm_iq_ring_detach(&view);
  iq_chan_seek(iq_feed_chan(f), 0);
  run(f, 40);
  iq_feed_stats_t st;
  iq_feed_stats(f, &st);
  CHECK(st.reads == 40 && st.read_errors == 0 && st.ts_gaps == 0, "reads %llu gaps %llu", (unsigned long long)st.reads, (unsigned long long)st.ts_gaps);
  CHECK(frozen ? st.frozen && st.ts_frozen == 39 : !st.frozen, "frozen %d %llu", st.frozen, (unsigned long long)st.ts_frozen);
  CHECK(short_every ? st.short_reads == 40 / (uint64_t)short_every : st.short_reads == 0, "short reads %llu", (unsigned long long)st.short_reads);
  CHECK(st.chan.overruns == 0 && st.chan.discont == 0, "chan ov %llu disc %llu", (unsigned long long)st.chan.overruns, (unsigned long long)st.chan.discont);
  shm_iq_ring_t *w = iq_feed_wide_ring(f);
  CHECK(w->hdr->base_tick == TICK0 && atomic_load(&w->hdr->gaps) == 0, "wide ring ticks continuous");
  CHECK(atomic_load(&w->hdr->head) == fk.pos, "every read sample published (head %llu pos %llu)", (unsigned long long)atomic_load(&w->hdr->head), (unsigned long long)fk.pos);
  const int nb = (int)(fk.pos / BLOCK);
  CHECK(nb >= 30, "blocks %d", nb);
  for (int k = 0; k < 2; ++k) {
    oracle_t o;
    CHECK(oracle_init(&o, p.cell[k].decim, p.cell[k].passband_hz, p.cell[k].shift_hz) == 0, "oracle");
    shm_iq_ring_t *r = iq_feed_cell_ring(f, k ? b : a);
    CHECK(r->hdr->base_tick == TICK0 / p.cell[k].decim, "cell %d base tick %lld", k, (long long)r->hdr->base_tick);
    CHECK(atomic_load(&r->hdr->gaps) == 0 && atomic_load(&r->hdr->head) == (uint64_t)nb * BLOCK / p.cell[k].decim, "cell %d ring continuous", k);
    const long bad = compare(&o, r, 0, 0, nb);
    oracle_free(&o);
    CHECK(bad == 0, "cell %d: %ld samples differ from the iq_ddc reference (frozen=%d short=%d)", k, bad, frozen, short_every);
  }
  iq_feed_destroy(f);
  printf("  two cells bit-exact over %d blocks (frozen=%d short_every=%d)\n", nb, frozen, short_every);
  return 0;
}

/* MPCI-DASH P3: CHAN_CELL lines. One line per cell with cell=/pci=/ring= named, window deltas, nothing at one cell, nothing on the first (window opening) call */
static int test_chan_cell(void)
{
  fake_t fk = {0};
  iq_feed_t *f = make_feed(&fk, "g6chc", 1 << 20);
  CHECK(f, "create");
  iq_span_plan_t p;
  plan_pair(&p);
  const int a = iq_feed_cell_add(f, "pci415", CENTRE + p.cell[0].shift_hz, p.cell[0].decim, p.cell[0].passband_hz, 0);
  const int b = iq_feed_cell_add(f, "pci403", CENTRE + p.cell[1].shift_hz, p.cell[1].decim, p.cell[1].passband_hz, 0);
  CHECK(a >= 0 && b >= 0, "add");
  iq_chan_seek(iq_feed_chan(f), 0);
  iq_feed_cell_ref_t refs[2] = {{a, 0, 415}, {b, 1, 403}};
  char buf[2048];
  memset(buf, 'X', sizeof(buf));
  CHECK(iq_feed_format_chan_cells(f, 1000000000ull, refs, 1, buf, sizeof(buf)) == 0 && buf[0] == 0, "one cell: no line");
  CHECK(iq_feed_format_chan_cells(f, 1000000000ull, refs, 2, buf, sizeof(buf)) == 0 && buf[0] == 0, "the first call only opens the window");
  run(f, 20);
  const int nb = (int)(fk.pos / BLOCK);
  const int n = iq_feed_format_chan_cells(f, 3000000000ull, refs, 2, buf, sizeof(buf));
  CHECK(n > 0 && n == (int)strlen(buf), "two cells: lines (%d)", n);
  char *nl = strchr(buf, '\n');
  CHECK(nl, "two lines");
  *nl = 0;
  char want[160];
  snprintf(want, sizeof(want), "SENSING: CHAN_CELL cell=0 pci=415 ring=pci415 win_ms=2000 blocks=%d out_samples=%d ", nb, nb * BLOCK / p.cell[0].decim);
  CHECK(!strncmp(buf, want, strlen(want)), "line 0: %s (want prefix %s)", buf, want);
  snprintf(want, sizeof(want), "SENSING: CHAN_CELL cell=1 pci=403 ring=pci403 win_ms=2000 blocks=%d out_samples=%d ", nb, nb * BLOCK / p.cell[1].decim);
  CHECK(!strncmp(nl + 1, want, strlen(want)), "line 1: %s (want prefix %s)", nl + 1, want);
  CHECK(strstr(buf, " write_us_mean=") && strstr(buf, " run_us_attrib=") && strstr(buf, " lag_now=") && strstr(buf, " overruns_seen=0 paused=0"), "fields: %s", buf);
  /* the next window starts from here: nothing processed, zero blocks */
  CHECK(iq_feed_format_chan_cells(f, 5000000000ull, refs, 2, buf, sizeof(buf)) > 0 && strstr(buf, " blocks=0 out_samples=0 "), "next window: %s", buf);
  iq_feed_destroy(f);
  printf("  CHAN_CELL lines: 2 cells, 1 cell none, priming call none\n");
  return 0;
}

static int test_gap_and_error(void)
{
  fake_t fk = {.gap_at = 11, .gap_len = 5 * BLOCK, .fail_at = 30};
  iq_feed_t *f = make_feed(&fk, "g6gap", 1 << 20);
  CHECK(f, "create");
  iq_span_plan_t p;
  plan_pair(&p);
  const int b = iq_feed_cell_add(f, "pci403", CENTRE + p.cell[1].shift_hz, p.cell[1].decim, p.cell[1].passband_hz, 0);
  CHECK(b >= 0, "add");
  iq_chan_seek(iq_feed_chan(f), 0);
  run(f, 10);
  shm_iq_ring_t *r = iq_feed_cell_ring(f, b);
  oracle_t o;
  /* before the gap: 10 blocks from index 0 (checked now: the gap moves the ring's valid_from past them) */
  oracle_init(&o, p.cell[1].decim, p.cell[1].passband_hz, p.cell[1].shift_hz);
  CHECK(compare(&o, r, 0, 0, 10) == 0, "pre-gap");
  oracle_free(&o);
  run(f, 19);
  iq_feed_stats_t st;
  iq_feed_stats(f, &st);
  CHECK(st.ts_gaps == 1 && st.chan.discont == 1 && st.chan.overruns == 0, "gap: feed %llu chan %llu", (unsigned long long)st.ts_gaps, (unsigned long long)st.chan.discont);
  CHECK(atomic_load(&r->hdr->gaps) == 1, "the cell ring carries one forward gap");
  /* after: a fresh filter from absolute 15 x BLOCK, cell index (15 x BLOCK) / decim */
  oracle_init(&o, p.cell[1].decim, p.cell[1].passband_hz, p.cell[1].shift_hz);
  CHECK(compare(&o, r, 15 * BLOCK / (uint64_t)p.cell[1].decim, 15 * BLOCK, 10) == 0, "post-gap bit-exact vs a fresh filter");
  oracle_free(&o);
  CHECK(iq_feed_step(f) == -1, "read error reported");
  iq_feed_stats(f, &st);
  CHECK(st.read_errors == 1, "read error counted");
  iq_feed_destroy(f);
  printf("  radio gap -> one discontinuity per ring, post-gap bit-exact; read error counted\n");
  return 0;
}

static int test_pause_resume(void)
{
  fake_t fk = {0};
  iq_feed_t *f = make_feed(&fk, "g6pr", 1 << 20);
  iq_span_plan_t p;
  plan_pair(&p);
  const int a = iq_feed_cell_add(f, "pci415", CENTRE + p.cell[0].shift_hz, p.cell[0].decim, p.cell[0].passband_hz, 0);
  const int b = iq_feed_cell_add(f, "pci403", CENTRE + p.cell[1].shift_hz, p.cell[1].decim, p.cell[1].passband_hz, 0);
  /* a third cell outside the capture span: created paused, never computed */
  const int c = iq_feed_cell_add(f, "far", 3300e6, 4, 9.18e6, 1);
  CHECK(a >= 0 && b >= 0 && c >= 0, "add");
  shm_iq_ring_t *rb = iq_feed_cell_ring(f, b), *rc = iq_feed_cell_ring(f, c);
  CHECK(atomic_load(&rc->hdr->away) && atomic_load(&rc->hdr->head) == 0, "created paused: away, no data");
  iq_chan_seek(iq_feed_chan(f), 0);
  run(f, 10);
  const uint64_t hb = atomic_load(&rb->hdr->head);
  CHECK(iq_feed_cell_pause(f, b) == 0 && atomic_load(&rb->hdr->away), "pause");
  run(f, 10);
  CHECK(atomic_load(&rb->hdr->head) == hb, "a paused cell gets no writes");
  iq_feed_cell_stats_t cs;
  CHECK(iq_feed_cell_stats(f, b, &cs) == 0 && cs.paused == 1, "stats paused");
  const uint64_t resume_at = iq_chan_pos(iq_feed_chan(f));
  CHECK(iq_feed_cell_resume(f, b) == 0, "resume");
  run(f, 10);
  CHECK(atomic_load(&rb->hdr->head) > hb && !atomic_load(&rb->hdr->away) && atomic_load(&rb->hdr->gaps) == 1, "resumed: data, away cleared, one forward gap");
  CHECK(atomic_load(&rc->hdr->head) == 0, "the out-of-span cell stays unserved");
  iq_feed_stats_t st;
  iq_feed_stats(f, &st);
  CHECK(st.chan.n_cells == 3 && st.chan.n_paused == 1 && st.chan.overruns == 0, "chan stats");
  oracle_t o;
  /* cell A bit-exact through the whole pause / resume of B */
  oracle_init(&o, p.cell[0].decim, p.cell[0].passband_hz, p.cell[0].shift_hz);
  const int nb = (int)(iq_chan_pos(iq_feed_chan(f)) / BLOCK);
  CHECK(compare(&o, iq_feed_cell_ring(f, a), 0, 0, nb) == 0, "cell A unaffected");
  oracle_free(&o);
  /* cell B after resume: fresh history at the resume position */
  oracle_init(&o, p.cell[1].decim, p.cell[1].passband_hz, p.cell[1].shift_hz);
  CHECK(compare(&o, rb, resume_at / (uint64_t)p.cell[1].decim, resume_at, (int)((iq_chan_pos(iq_feed_chan(f)) - resume_at) / BLOCK)) == 0, "cell B resumed bit-exact");
  oracle_free(&o);
  char line[1024];
  iq_feed_format(f, line, sizeof(line));
  printf("  %s\n", line);
  CHECK(strstr(line, "MPCI_FEED") && strstr(line, "far:") && strstr(line, "paused"), "format");
  iq_feed_destroy(f);
  printf("  live pause / resume: the radio and the other cell never stop; out-of-span cell kept unserved\n");
  return 0;
}

/* reader thread + channelizer thread with derived k, a consumer thread checking the cell ring is continuous */
static int test_threads(void)
{
  fake_t fk = {.pace_us = 1000};
  iq_feed_t *f = make_feed(&fk, "g6thr", 1 << 21);
  iq_span_plan_t p;
  plan_pair(&p);
  const int b = iq_feed_cell_add(f, "pci403", CENTRE + p.cell[1].shift_hz, p.cell[1].decim, p.cell[1].passband_hz, 0);
  CHECK(b >= 0, "add");
  CHECK(iq_feed_start(f, -1, 0, NULL, 0) == 0, "start");
  shm_iq_ring_t *r = iq_feed_cell_ring(f, b);
  for (int i = 0; i < 400 && atomic_load(&r->hdr->head) < 40 * BLOCK / 4; ++i) usleep(5000);
  atomic_store(&fk.stop, 1); /* the radio stops first (the receiver's shutdown stops the device), then the feed */
  iq_feed_stop(f);
  iq_feed_stats_t st;
  iq_feed_stats(f, &st);
  CHECK(!st.running && st.reads > 0 && st.read_errors <= 1, "reader left on the stopped radio (reads %llu err %llu)", (unsigned long long)st.reads, (unsigned long long)st.read_errors);
  CHECK(atomic_load(&r->hdr->head) >= 40 * BLOCK / 4 && st.chan.overruns == 0, "cell ring filled live (%llu)", (unsigned long long)atomic_load(&r->hdr->head));
  /* the channelizer started at the live edge (16-aligned): compare from its first block */
  const uint64_t start_pos = (uint64_t)(r->hdr->base_tick - TICK0 / p.cell[1].decim) * (uint64_t)p.cell[1].decim; /* positions are 16-aligned */
  oracle_t o;
  oracle_init(&o, p.cell[1].decim, p.cell[1].passband_hz, p.cell[1].shift_hz);
  CHECK(compare(&o, r, 0, start_pos, 20) == 0, "threaded bit-exact (start %llu)", (unsigned long long)start_pos);
  oracle_free(&o);
  iq_feed_destroy(f);
  printf("  reader + channelizer threads: %llu reads, threads=%d needed=%d\n", (unsigned long long)st.reads, st.chan.threads, st.chan.threads_needed);
  return 0;
}

/* the cell's RU: the shm_iq consumer device on its cell ring */
typedef struct { openair0_device_t *d; int ret; } rd_t;
static void *blocked_read(void *arg)
{
  rd_t *x = arg;
  static int16_t b[NCH][2048 * 2];
  void *bp[NCH] = {b[0], b[1], b[2], b[3]};
  openair0_timestamp_t t;
  x->ret = x->d->trx_read_func(x->d, &t, bp, 1024, NCH);
  return NULL;
}
static int test_device(const char *lib)
{
  void *h = dlopen(lib, RTLD_NOW);
  CHECK(h, "dlopen %s", dlerror());
  int (*init)(openair0_device_t *, openair0_config_t *) = (int (*)(openair0_device_t *, openair0_config_t *))dlsym(h, "device_init");
  CHECK(init, "device_init");
  fake_t fk = {0};
  iq_feed_t *f = make_feed(&fk, "nrcell", 1 << 20);
  iq_span_plan_t p;
  plan_pair(&p);
  const int b = iq_feed_cell_add(f, "pci403", CENTRE + p.cell[1].shift_hz, p.cell[1].decim, p.cell[1].passband_hz, 0);
  const int z = iq_feed_cell_add(f, "pci999", 3300e6, 4, 9.18e6, 1);
  CHECK(b >= 0 && z >= 0, "add");
  iq_chan_seek(iq_feed_chan(f), 0);
  run(f, 4);
  /* cell 403 as its UE instance opens it: ring nrcell_pci403, rate 30.72 MS/s, tuned to its own centre (the ring centre): a memcpy consumer */
  openair0_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.sdr_addrs = (char *)"nrcell_pci403"; cfg.sample_rate = 30.72e6; cfg.rx_num_channels = NCH; cfg.num_rb_dl = 51;
  for (int a = 0; a < NCH; ++a) cfg.rx_freq[a] = 3630000000.0;
  openair0_device_t dev;
  memset(&dev, 0, sizeof(dev));
  CHECK(init(&dev, &cfg) == 0, "device init on the in-process cell ring");
  CHECK(dev.trx_start_func(&dev) == 0, "start");
  run(f, 4);
  static int16_t got[NCH][4096 * 2], ref[NCH][4096 * 2];
  void *gp[NCH] = {got[0], got[1], got[2], got[3]};
  int16_t *rp[NCH] = {ref[0], ref[1], ref[2], ref[3]};
  shm_iq_ring_t *r = iq_feed_cell_ring(f, b);
  openair0_timestamp_t t = 0;
  CHECK(dev.trx_read_func(&dev, &t, gp, 4096, NCH) == 4096, "read");
  const uint64_t idx = (uint64_t)t - (uint64_t)r->hdr->base_tick;
  CHECK(shm_iq_ring_copy(r, idx, 4096, NCH, rp) == 0, "ring copy at the read timestamp");
  long bad = 0;
  for (int a = 0; a < NCH; ++a) for (int i = 0; i < 2 * 4096; ++i) bad += got[a][i] != ref[a][i];
  CHECK(bad == 0, "device output == cell ring (%ld differ)", bad);
  CHECK((int64_t)t == r->hdr->base_tick + (int64_t)idx && r->hdr->base_tick == TICK0 / 4, "timestamps on the cell clock");
  dev.trx_end_func(&dev);
  /* a cell created paused (outside the span): its device attaches at once, a read waits, the receiver's stop releases it */
  openair0_config_t cz = cfg;
  cz.sdr_addrs = (char *)"nrcell_pci999";
  for (int a = 0; a < NCH; ++a) cz.rx_freq[a] = 3300e6;
  openair0_device_t dz;
  memset(&dz, 0, sizeof(dz));
  CHECK(init(&dz, &cz) == 0 && dz.trx_start_func(&dz) == 0, "paused ring attach");
  rd_t x = {.d = &dz, .ret = 99};
  pthread_t th;
  pthread_create(&th, NULL, blocked_read, &x);
  pthread_join(th, NULL); /* MPCI-LIVE: a ring paused by its producer returns SHM_IQ_READ_PAUSED at once (no wait; the receiver parks the slot thread outside its radio gate) */
  CHECK(x.ret == SHM_IQ_READ_PAUSED, "read on a paused ring returns PAUSED, got %d", x.ret);
  dz.trx_end_func(&dz);
  iq_feed_destroy(f);
  printf("  shm_iq device reads its cell ring bit-exact on the cell clock; paused ring attach + PAUSED read OK\n");
  return 0;
}

static int test_mode(void)
{
  CHECK(iq_feed_input_mode(1, "", 0) == IQ_FEED_DIRECT, "N=1 radio");
  CHECK(iq_feed_input_mode(1, "shm_iq", 0) == IQ_FEED_DIRECT, "N=1 rings");
  CHECK(iq_feed_input_mode(1, NULL, 1) == IQ_FEED_DIRECT, "N=1 sim");
  CHECK(iq_feed_input_mode(0, "", 0) == IQ_FEED_DIRECT, "N=0");
  CHECK(iq_feed_input_mode(2, "shm_iq", 0) == IQ_FEED_RINGS, "rings");
  CHECK(iq_feed_input_mode(2, "", 0) == IQ_FEED_WIDEBAND && iq_feed_input_mode(3, NULL, 0) == IQ_FEED_WIDEBAND, "radio");
  CHECK(iq_feed_input_mode(2, "", 1) == IQ_FEED_STARTED_ONLY && iq_feed_input_mode(2, "shm_iq", 1) == IQ_FEED_STARTED_ONLY, "sim/replay");
  return 0;
}

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: test_iq_feed <libshm_iq.so>\n"); return 2; }
  table_init(1 << 21);
  int fail = 0;
  fail |= test_mode();
  fail |= test_two_cells(0, 0);
  fail |= test_two_cells(1, 0);
  fail |= test_two_cells(0, 3);
  fail |= test_two_cells(1, 4);
  fail |= test_chan_cell();
  fail |= test_gap_and_error();
  fail |= test_pause_resume();
  fail |= test_threads();
  fail |= test_device(argv[1]);
  printf("%s\n", fail ? "FAILED" : "ALL OK");
  return fail;
}
