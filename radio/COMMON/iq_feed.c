/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI G6: radio -> wideband ring -> channelizer -> per-cell rings, see iq_feed.h. */
#define _GNU_SOURCE
#include "iq_feed.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct iq_feed {
  iq_feed_cfg_t cfg;
  shm_iq_ring_t wide;
  iq_chan_t *chan;
  int block;
  int16_t *stage[SHM_IQ_MAX_CH]; /* reads that cannot go straight into the ring slots (ring end) land here first */
  /* timestamp tracking */
  int have_prev, frozen;
  int64_t prev_raw, expect;
  /* stats */
  _Atomic uint64_t reads, samples, short_reads, read_errors, ts_gaps, ts_frozen, staged;
  /* reader thread */
  pthread_t th;
  _Atomic int running, stop;
  int thread_started, chan_started;
  double live_ms;
  char tag[IQ_CHAN_MAX_CELLS][32];
  /* MPCI-DASH P3: window state of CHAN_CELL (one slot per entry id; touched only by the manager tick that formats the lines) */
  struct { uint64_t blocks, out, write_ns, ov; } dprev[IQ_CHAN_MAX_CELLS];
  uint64_t dprev_t_ns, dprev_run_ns;
  int dprimed;
};

static void *alloc_samples(size_t n)
{
  size_t bytes = (n * 4 + 63) & ~(size_t)63;
  return aligned_alloc(64, bytes ? bytes : 64);
}

iq_feed_t *iq_feed_create(const iq_feed_cfg_t *cfg)
{
  if (!cfg || !cfg->read || !cfg->nch || cfg->nch > SHM_IQ_MAX_CH || !(cfg->rate > 0) || !cfg->name || !cfg->name[0] || cfg->wide_capacity < 1024) {
    errno = EINVAL;
    return NULL;
  }
  iq_feed_t *f = calloc(1, sizeof(*f));
  if (!f) return NULL;
  f->cfg = *cfg;
  /* consumers of the wide ring: the channelizer (its own view) and up to one diagnostic tap */
  if (shm_iq_ring_create(&f->wide, SHM_IQ_BACKING_ANON, cfg->name, cfg->nch, cfg->wide_capacity, cfg->rate, cfg->centre_hz, 2)) goto fail;
  shm_iq_ring_set_meta(&f->wide, cfg->radio_id, SHM_IQ_TS_FREE);
  int block = cfg->block;
  if (block <= 0) {
    block = IQ_CHAN_DEFAULT_BLOCK;
    while (block > 4096 && (uint64_t)block > f->wide.hdr->capacity / 8) block /= 2;
  }
  if (block % 16 || (uint64_t)block > f->wide.hdr->capacity / 8) { errno = EINVAL; goto fail; }
  f->block = block;
  for (unsigned a = 0; a < cfg->nch; ++a)
    if (!(f->stage[a] = alloc_samples((size_t)block + 16))) { errno = ENOMEM; goto fail; }
  shm_iq_ring_prefault(&f->wide); /* page faults belong to start-up */
  iq_chan_cfg_t cc = {.in = &f->wide, .block = block, .backend = cfg->backend, .load_hint = cfg->load_hint,
                      .out_ring_budget_bytes = cfg->narrow_budget_bytes};
  if (!(f->chan = iq_chan_create(&cc))) goto fail;
  return f;
fail:
  {
    const int e = errno;
    for (unsigned a = 0; a < SHM_IQ_MAX_CH; ++a) free(f->stage[a]);
    if (f->wide.hdr) shm_iq_ring_detach(&f->wide);
    free(f);
    errno = e;
  }
  return NULL;
}

void iq_feed_stop(iq_feed_t *f)
{
  if (!f) return;
  atomic_store(&f->stop, 1);
  if (f->thread_started) { pthread_join(f->th, NULL); f->thread_started = 0; }
  if (f->chan_started) { iq_chan_stop(f->chan); f->chan_started = 0; }
}

void iq_feed_destroy(iq_feed_t *f)
{
  if (!f) return;
  iq_feed_stop(f);
  iq_chan_destroy(f->chan);
  for (unsigned a = 0; a < SHM_IQ_MAX_CH; ++a) free(f->stage[a]);
  shm_iq_ring_detach(&f->wide);
  free(f);
}

int iq_feed_cell_add(iq_feed_t *f, const char *tag, double centre_hz, int decim, double passband_hz, int paused)
{
  if (!f || !tag || !tag[0]) return -EINVAL;
  iq_chan_entry_t e = {.centre_hz = centre_hz, .decim = decim, .passband_hz = passband_hz};
  snprintf(e.tag, sizeof(e.tag), "%s", tag);
  const int id = iq_chan_cell_add(f->chan, &e);
  if (id >= 0 && id < IQ_CHAN_MAX_CELLS) snprintf(f->tag[id], sizeof(f->tag[id]), "%s", tag);
  if (id >= 0 && paused) iq_chan_cell_pause(f->chan, id);
  if (id >= 0) shm_iq_ring_prefault(iq_chan_cell_ring(f->chan, id));
  return id;
}
int iq_feed_cell_pause(iq_feed_t *f, int id) { return f ? iq_chan_cell_pause(f->chan, id) : -EINVAL; }
int iq_feed_cell_resume(iq_feed_t *f, int id) { return f ? iq_chan_cell_resume(f->chan, id) : -EINVAL; }
/* MPCI-LIVE: retune one entry to a new cell centre inside the running capture (shift = centre - the capture's centre); resets that entry's history, bumps its ring epoch */
int iq_feed_cell_retune(iq_feed_t *f, int id, double centre_hz)
{
  if (!f || !(centre_hz > 0)) return -EINVAL;
  return iq_chan_cell_retune(f->chan, id, centre_hz - shm_iq_ring_center(&f->wide), centre_hz);
}
/* MPCI-LIVE: the radio's LO moved to `centre_hz` (same rate; the caller retuned the device under its exclusive gate): the wideband ring starts a new run with the new
 * centre; the channelizer recomputes every entry's shift (entries are given by absolute centre), resets their histories and bumps every cell ring's epoch. */
int iq_feed_set_centre(iq_feed_t *f, double centre_hz)
{
  if (!f || !(centre_hz > 0)) return -EINVAL;
  shm_iq_ring_new_epoch(&f->wide, centre_hz);
  return 0;
}
double iq_feed_centre(const iq_feed_t *f) { return f ? shm_iq_ring_center(&f->wide) : 0; }
shm_iq_ring_t *iq_feed_cell_ring(iq_feed_t *f, int id) { return f ? iq_chan_cell_ring(f->chan, id) : NULL; }
shm_iq_ring_t *iq_feed_wide_ring(iq_feed_t *f) { return f ? &f->wide : NULL; }
iq_chan_t *iq_feed_chan(iq_feed_t *f) { return f ? f->chan : NULL; }

/* the tick of this read's first sample: frozen-stamp continuation and gap accounting; `raw` = what the device said */
static int64_t track_tick(iq_feed_t *f, int64_t raw)
{
  int64_t tick = raw;
  if (f->have_prev) {
    if (raw == f->prev_raw && raw != f->expect) { /* the device repeated the previous stamp: continue from the sample count */
      if (!f->frozen)
        fprintf(stderr, "IQ_FEED_TS_FROZEN the radio repeats the stream start timestamp %lld; timestamps continue from the sample count\n", (long long)raw);
      f->frozen = 1;
      atomic_fetch_add(&f->ts_frozen, 1);
      tick = f->expect;
    } else if (raw != f->expect) {
      atomic_fetch_add(&f->ts_gaps, 1);
      fprintf(stderr, "IQ_FEED_TS_GAP expected=%lld got=%lld delta=%+lld\n", (long long)f->expect, (long long)raw, (long long)(raw - f->expect));
    }
  }
  f->have_prev = 1;
  f->prev_raw = raw;
  return tick;
}

int iq_feed_step(iq_feed_t *f)
{
  const unsigned nch = f->cfg.nch;
  int16_t *slots[SHM_IQ_MAX_CH];
  const int room = shm_iq_ring_reserve(&f->wide, f->block, slots);
  /* straight into the ring when a whole block fits before the ring end (a radio driver may store whole 8-sample vectors: block is a multiple of 16) */
  const int direct = room == f->block;
  void *buf[SHM_IQ_MAX_CH];
  for (unsigned a = 0; a < nch; ++a) buf[a] = direct ? (void *)slots[a] : (void *)f->stage[a];
  int64_t ts = 0;
  int got = f->cfg.read(f->cfg.read_ctx, &ts, buf, f->block, (int)nch);
  if (got <= 0) {
    atomic_fetch_add(&f->read_errors, 1);
    return -1;
  }
  if (got > f->block) got = f->block;
  atomic_fetch_add(&f->reads, 1);
  atomic_fetch_add(&f->samples, (uint64_t)got);
  if (got < f->block) atomic_fetch_add(&f->short_reads, 1); /* published as it is: nothing is lost, nothing restarts */
  const int64_t tick = track_tick(f, ts);
  f->expect = tick + got;
  if (direct && shm_iq_ring_commit(&f->wide, tick, got) == 0) return 1;
  if (direct) /* first write / gap / new run: the ring decides the index, copy the slots out first (write() may move the head) */
    for (unsigned a = 0; a < nch; ++a) memcpy(f->stage[a], slots[a], (size_t)got * 4);
  else
    atomic_fetch_add(&f->staged, 1);
  void *src[SHM_IQ_MAX_CH];
  for (unsigned a = 0; a < nch; ++a) src[a] = f->stage[a];
  shm_iq_ring_write(&f->wide, tick, src, got, nch);
  return 1;
}

typedef struct {
  iq_feed_t *f;
  int cpu, prio;
} reader_arg_t;

static void *reader_main(void *p)
{
  reader_arg_t a = *(reader_arg_t *)p;
  free(p);
  iq_feed_t *f = a.f;
  pthread_setname_np(pthread_self(), "iqFeed");
  if (a.prio > 0) {
    struct sched_param sp = {.sched_priority = a.prio};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); /* best effort: needs CAP_SYS_NICE */
  }
  while (!atomic_load(&f->stop))
    if (iq_feed_step(f) < 0) {
      if (!atomic_load(&f->stop)) fprintf(stderr, "IQ_FEED_READ_ERROR the radio read failed: reader stops (the radio driver owns recovery and shutdown)\n");
      break;
    }
  atomic_store(&f->running, 0);
  return NULL;
}

static uint64_t mono_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* every running cell ring holds its first block (paused entries are away: live at once); polled, no lock held (see iq_feed.h, iq_feed_start) */
static int all_live(iq_feed_t *f)
{
  for (int id = 0; id < IQ_CHAN_MAX_CELLS; ++id) {
    const shm_iq_ring_t *r = iq_chan_cell_ring(f->chan, id);
    if (r && iq_chan_cell_paused(f->chan, id) != 1 && !shm_iq_ring_live(r)) return 0;
  }
  return 1;
}
static int wait_live(iq_feed_t *f)
{
  const uint64_t t0 = mono_ns();
  for (;;) {
    if (all_live(f)) {
      f->live_ms = (double)(mono_ns() - t0) * 1e-6;
      return 0;
    }
    if (!atomic_load(&f->running)) return -EIO;
    if (f->cfg.cancelled && f->cfg.cancelled()) return -ECANCELED;
    if (mono_ns() - t0 > (uint64_t)(IQ_FEED_LIVE_TIMEOUT_S * 1e9)) {
      fprintf(stderr, "IQ_FEED_NOT_LIVE no first block in every cell ring after %.0f s (reads=%llu): the feed is stopped\n", IQ_FEED_LIVE_TIMEOUT_S,
              (unsigned long long)atomic_load(&f->reads));
      return -ETIMEDOUT;
    }
    struct timespec w = {0, 1000000};
    nanosleep(&w, NULL);
  }
}

int iq_feed_start(iq_feed_t *f, int reader_cpu, int reader_prio, const int *chan_cpus, int n_chan_cpus)
{
  if (!f || f->thread_started) return -EINVAL;
  int rc = iq_chan_start(f->chan, chan_cpus, n_chan_cpus);
  if (rc) return rc;
  f->chan_started = 1;
  reader_arg_t *a = malloc(sizeof(*a));
  if (!a) return -ENOMEM;
  *a = (reader_arg_t){.f = f, .cpu = reader_cpu, .prio = reader_prio};
  pthread_attr_t at;
  pthread_attr_init(&at);
  if (reader_cpu >= 0 && reader_cpu < CPU_SETSIZE) {
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(reader_cpu, &s);
    pthread_attr_setaffinity_np(&at, sizeof(s), &s);
  }
  atomic_store(&f->stop, 0);
  atomic_store(&f->running, 1);
  rc = pthread_create(&f->th, &at, reader_main, a);
  pthread_attr_destroy(&at);
  if (rc) { free(a); atomic_store(&f->running, 0); return -rc; }
  f->thread_started = 1;
  rc = wait_live(f);
  if (rc) iq_feed_stop(f);
  return rc;
}

void iq_feed_stats(const iq_feed_t *f, iq_feed_stats_t *st)
{
  memset(st, 0, sizeof(*st));
  st->reads = atomic_load(&f->reads); st->samples = atomic_load(&f->samples); st->short_reads = atomic_load(&f->short_reads);
  st->read_errors = atomic_load(&f->read_errors); st->ts_gaps = atomic_load(&f->ts_gaps); st->ts_frozen = atomic_load(&f->ts_frozen);
  st->staged = atomic_load(&f->staged); st->frozen = f->frozen; st->running = atomic_load(&f->running); st->live_ms = f->live_ms;
  iq_chan_stats(f->chan, &st->chan);
}

int iq_feed_cell_stats(iq_feed_t *f, int id, iq_feed_cell_stats_t *st)
{
  shm_iq_ring_t *r = iq_feed_cell_ring(f, id);
  if (!r) return -1;
  memset(st, 0, sizeof(*st));
  snprintf(st->tag, sizeof(st->tag), "%s", f->tag[id]);
  st->paused = iq_chan_cell_paused(f->chan, id) == 1;
  st->head = atomic_load(&r->hdr->head);
  const shm_iq_hdr_t *h = r->hdr;
  for (unsigned i = 0; i < h->n_consumers && i < SHM_IQ_MAX_CONS; ++i)
    if (atomic_load(&h->cons[i].active)) {
      st->overruns += atomic_load(&h->cons[i].overruns);
      st->dropped += atomic_load(&h->cons[i].dropped);
      const uint64_t l = shm_iq_ring_consumer_lag(r, (int)i);
      if (l > st->lag) st->lag = l;
    }
  return 0;
}

int iq_feed_format(iq_feed_t *f, char *buf, size_t n)
{
  iq_feed_stats_t s;
  iq_feed_stats(f, &s);
  char ls[160] = ""; /* core-pool lease of the channelizer workers (CHANPOOL); empty without leases, so the line is unchanged then */
  if (s.chan.lease_blocks || s.chan.lease_denied)
    snprintf(ls, sizeof(ls), " chan_lease=%llu/denied=%llu/wait_max_us=%llu", (unsigned long long)s.chan.lease_blocks, (unsigned long long)s.chan.lease_denied,
             (unsigned long long)(s.chan.lease_wait_max_ns / 1000));
  /* G5: the channelizer's worst step gap and block work (where a full-ring overrun comes from) */
  char gs[64];
  snprintf(gs, sizeof(gs), " chan_gap_max_ms=%.1f chan_work_max_ms=%.1f", s.chan.step_gap_max_ns * 1e-6, s.chan.work_max_ns * 1e-6);
  if (strlen(ls) + strlen(gs) < sizeof(ls)) strcat(ls, gs);
  size_t o = (size_t)snprintf(buf, n,
                              "MPCI_FEED rate=%.0f centre_hz=%.0f reads=%llu samples=%llu short=%llu gaps=%llu frozen=%llu read_err=%llu chan_overruns=%llu chan_discont=%llu "
                              "chan_lag_max=%llu threads=%d/%d backend=%s%s cells=",
                              f->cfg.rate, f->cfg.centre_hz, (unsigned long long)s.reads, (unsigned long long)s.samples, (unsigned long long)s.short_reads,
                              (unsigned long long)s.ts_gaps, (unsigned long long)s.ts_frozen, (unsigned long long)s.read_errors,
                              (unsigned long long)s.chan.overruns, (unsigned long long)s.chan.discont, (unsigned long long)s.chan.lag_max, s.chan.threads,
                              s.chan.threads_needed, s.chan.backend ? s.chan.backend : "-", ls);
  int first = 1;
  for (int id = 0; id < IQ_CHAN_MAX_CELLS && o < n; ++id) {
    iq_feed_cell_stats_t c;
    if (iq_feed_cell_stats(f, id, &c)) continue;
    o += (size_t)snprintf(buf + o, n - o, "%s%s:ov=%llu,drop=%llu,lag=%llu%s", first ? "" : ";", c.tag, (unsigned long long)c.overruns, (unsigned long long)c.dropped,
                          (unsigned long long)c.lag, c.paused ? ",paused" : "");
    first = 0;
  }
  if (first && o < n) o += (size_t)snprintf(buf + o, n - o, "-");
  return (int)(o < n ? o : n - 1);
}

int iq_feed_format_chan_cells(iq_feed_t *f, uint64_t now_ns, const iq_feed_cell_ref_t *refs, int nrefs, char *buf, size_t cap)
{
  if (!f || !refs || !buf || !cap) return 0;
  buf[0] = 0;
  if (nrefs < 2) return 0; /* one cell: no line (and no window kept) */
  if (nrefs > IQ_CHAN_MAX_CELLS) nrefs = IQ_CHAN_MAX_CELLS;
  iq_feed_stats_t fs;
  iq_feed_stats(f, &fs);
  iq_chan_cell_stats_t cs[IQ_CHAN_MAX_CELLS];
  iq_feed_cell_stats_t ps[IQ_CHAN_MAX_CELLS];
  int ok[IQ_CHAN_MAX_CELLS];
  uint64_t out_tot = 0;
  const int primed = f->dprimed && now_ns > f->dprev_t_ns;
  for (int i = 0; i < nrefs; ++i) {
    ok[i] = refs[i].id >= 0 && refs[i].id < IQ_CHAN_MAX_CELLS && iq_chan_cell_stats(f->chan, refs[i].id, &cs[i]) == 0 && iq_feed_cell_stats(f, refs[i].id, &ps[i]) == 0;
    if (ok[i] && cs[i].out_samples >= f->dprev[refs[i].id].out) out_tot += cs[i].out_samples - f->dprev[refs[i].id].out;
  }
  const uint64_t win_ns = primed ? now_ns - f->dprev_t_ns : 0;
  const uint64_t run_d = fs.chan.run_ns >= f->dprev_run_ns ? fs.chan.run_ns - f->dprev_run_ns : 0;
  size_t o = 0;
  for (int i = 0; i < nrefs; ++i) {
    if (!ok[i]) continue;
    const int id = refs[i].id;
    typeof(f->dprev[0]) *p = &f->dprev[id];
    if (primed) {
      const uint64_t blk = cs[i].blocks >= p->blocks ? cs[i].blocks - p->blocks : 0, out = cs[i].out_samples >= p->out ? cs[i].out_samples - p->out : 0;
      const uint64_t wns = cs[i].write_ns >= p->write_ns ? cs[i].write_ns - p->write_ns : 0, ov = ps[i].overruns >= p->ov ? ps[i].overruns - p->ov : 0;
      const int w = snprintf(buf + o, cap - o, "%sSENSING: CHAN_CELL cell=%d pci=%d ring=%s win_ms=%llu blocks=%llu out_samples=%llu write_us_mean=%.1f write_us_max=%llu "
                             "run_us_attrib=%llu lag_now=%llu overruns_seen=%llu paused=%d", o ? "\n" : "", refs[i].cell, refs[i].pci, ps[i].tag,
                             (unsigned long long)(win_ns / 1000000ull), (unsigned long long)blk, (unsigned long long)out, blk ? (double)wns / (double)blk / 1000.0 : 0.0,
                             (unsigned long long)(cs[i].write_max_ns / 1000), (unsigned long long)(out_tot ? (uint64_t)((double)run_d * (double)out / (double)out_tot / 1000.0) : 0),
                             (unsigned long long)ps[i].lag, (unsigned long long)ov, ps[i].paused);
      if (w > 0) o = o + (size_t)w < cap ? o + (size_t)w : cap - 1;
    }
    p->blocks = cs[i].blocks; p->out = cs[i].out_samples; p->write_ns = cs[i].write_ns; p->ov = ps[i].overruns;
  }
  f->dprev_t_ns = now_ns;
  f->dprev_run_ns = fs.chan.run_ns;
  f->dprimed = 1;
  return (int)o;
}

iq_feed_mode_t iq_feed_input_mode(int n_cells, const char *device_name, int sim_or_replay)
{
  if (n_cells <= 1) return IQ_FEED_DIRECT;
  if (sim_or_replay) return IQ_FEED_STARTED_ONLY;
  if (device_name && !strcmp(device_name, "shm_iq")) return IQ_FEED_RINGS;
  return IQ_FEED_WIDEBAND;
}

const char *iq_feed_mode_name(iq_feed_mode_t m)
{
  switch (m) {
    case IQ_FEED_DIRECT: return "direct";
    case IQ_FEED_RINGS: return "rings";
    case IQ_FEED_WIDEBAND: return "wideband";
    default: return "started_only";
  }
}
