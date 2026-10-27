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
  char tag[IQ_CHAN_MAX_CELLS][32];
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
  return 0;
}

void iq_feed_stats(const iq_feed_t *f, iq_feed_stats_t *st)
{
  memset(st, 0, sizeof(*st));
  st->reads = atomic_load(&f->reads); st->samples = atomic_load(&f->samples); st->short_reads = atomic_load(&f->short_reads);
  st->read_errors = atomic_load(&f->read_errors); st->ts_gaps = atomic_load(&f->ts_gaps); st->ts_frozen = atomic_load(&f->ts_frozen);
  st->staged = atomic_load(&f->staged); st->frozen = f->frozen; st->running = atomic_load(&f->running);
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
