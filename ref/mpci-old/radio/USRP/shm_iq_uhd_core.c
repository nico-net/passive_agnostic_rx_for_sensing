/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "shm_iq_uhd_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(PUB_MAX_CH == SHM_IQ_MAX_CH, "PUB_MAX_CH");

struct pub {
  shm_iq_ring_t ring;
  char name[64];
  pub_stats_t st;
  int16_t *stage[SHM_IQ_MAX_CH]; /* copy-path buffers, grown on demand */
  int stage_n;
  /* multi-streamer: samples a channel received beyond the shortest channel of the last chunk (a harmless count mismatch); they open
   * the next chunk in that channel's slot, so every channel stays aligned without a restart */
  int16_t *carry[SHM_IQ_MAX_CH];
  int carry_n[SHM_IQ_MAX_CH], carry_cap, reserved;
  int16_t *base[SHM_IQ_MAX_CH]; /* ring slots of the current reservation (the caller's slots[] are advanced by the carry) */
  int16_t *mst[SHM_IQ_MAX_CH];  /* staging used instead of the ring slots when a carry would not fit before the ring end */
  int mst_cap, staged;
};

uint64_t pub_default_capacity(unsigned channels, uint64_t mem_avail_bytes, uint64_t keep_free_bytes)
{
  const uint64_t spare = mem_avail_bytes > keep_free_bytes ? mem_avail_bytes - keep_free_bytes : 0;
  uint64_t cap = 1ull << 27;
  while (cap > (1ull << 23) && 4096 + (uint64_t)channels * cap * 4 > spare / 2) cap >>= 1;
  return cap;
}

pub_t *pub_open(const char *ring, unsigned channels, uint64_t capacity, double rate, double center_hz, const char *radio_id, unsigned ts)
{
  pub_t *p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  if (shm_iq_ring_create(&p->ring, ring, channels, capacity, rate, center_hz)) { free(p); return NULL; }
  shm_iq_ring_set_meta(&p->ring, radio_id, ts);
  snprintf(p->name, sizeof(p->name), "%s", ring);
  return p;
}

void pub_close(pub_t *p)
{
  if (!p) return;
  for (unsigned c = 0; c < SHM_IQ_MAX_CH; ++c) { free(p->stage[c]); free(p->carry[c]); free(p->mst[c]); }
  shm_iq_radio_remove(p->name);
  shm_iq_ring_detach(&p->ring);
  free(p);
}

const pub_stats_t *pub_stats(pub_t *p) { p->st.head = atomic_load(&p->ring.hdr->head); return &p->st; }
uint64_t pub_capacity(const pub_t *p) { return p->ring.hdr->capacity; }
shm_iq_ring_t *pub_ring(pub_t *p) { return &p->ring; }

/* Publish `got` samples already sitting in the reserved slots: in place if the tick continues the run, else via the copy path. */
static int publish(pub_t *p, int16_t *const *slots, int64_t tick, int got)
{
  shm_iq_ring_t *ring = &p->ring;
  pub_stats_t *st = &p->st;
  const unsigned nch = ring->hdr->channels;
  if (shm_iq_ring_commit(ring, tick, got) == 0) st->inplace++;
  else {
    if (st->copied > 0 && st->copied <= 10) /* the first write is expected; log the first run breaks after it */
      fprintf(stderr, "SHMIQ_UHD_GAP n=%llu tick=%lld expected=%lld got=%d\n", (unsigned long long)st->copied, (long long)tick,
              (long long)(atomic_load(&ring->hdr->tick_offset) + (int64_t)atomic_load(&ring->hdr->head)), got);
    void *bufs[SHM_IQ_MAX_CH];
    if (got > p->stage_n) {
      for (unsigned c = 0; c < nch; ++c) {
        free(p->stage[c]);
        p->stage[c] = malloc((size_t)got * 4);
        if (!p->stage[c]) abort();
      }
      p->stage_n = got;
    }
    for (unsigned c = 0; c < nch; ++c) { memcpy(p->stage[c], slots[c], (size_t)got * 4); bufs[c] = p->stage[c]; }
    shm_iq_ring_write(ring, tick, bufs, got, nch);
    st->copied++;
  }
  st->samples += (uint64_t)got;
  return got;
}

int pub_step(pub_t *p, pub_recv_fn recv, void *ctx, int want)
{
  pub_stats_t *st = &p->st;
  int16_t *slots[SHM_IQ_MAX_CH];
  const int n = shm_iq_ring_reserve(&p->ring, want, slots);
  int64_t tick = 0;
  int got = 0;
  const pub_rx_status_t s = recv(ctx, slots, n, &tick, &got);
  if (s != PUB_RX_OK || got <= 0) {
    if (s == PUB_RX_OVERFLOW) { st->overflows++; st->consecutive_timeouts = 0; }
    else if (s == PUB_RX_TIMEOUT) { st->timeouts++; st->consecutive_timeouts++; }
    else { st->others++; st->consecutive_timeouts = 0; }
    return 0;
  }
  st->consecutive_timeouts = 0;
  return publish(p, slots, tick, got);
}

int pub_mc_reserve(pub_t *p, int want, int16_t **slots)
{
  const unsigned nch = p->ring.hdr->channels;
  int n = shm_iq_ring_reserve(&p->ring, want, p->base), maxc = 0;
  for (unsigned c = 0; c < nch; ++c) if (p->carry_n[c] > maxc) maxc = p->carry_n[c];
  p->staged = 0;
  if (maxc > 0 && n <= maxc + maxc / 4) { /* too close to the ring end for the carry: receive into staging, published by copy */
    if (p->mst_cap < want) {
      for (unsigned c = 0; c < nch; ++c) { free(p->mst[c]); p->mst[c] = malloc((size_t)want * 4); if (!p->mst[c]) abort(); }
      p->mst_cap = want;
    }
    for (unsigned c = 0; c < nch; ++c) p->base[c] = p->mst[c];
    n = want; p->staged = 1; p->st.staged++;
  }
  p->reserved = n;
  for (unsigned c = 0; c < nch; ++c) {
    if (p->carry_n[c] > 0) memcpy(p->base[c], p->carry[c], (size_t)p->carry_n[c] * 4);
    slots[c] = p->base[c] + 2 * p->carry_n[c];
  }
  return n;
}

int pub_mc_want(const pub_t *p, unsigned c, int n) { return n - p->carry_n[c]; }

/* a count mismatch is harmless (carried over) when every channel delivered and the shortest is within a quarter of the chunk */
#define PUB_MC_CARRY_MAX_DIV 4

int pub_mc_finish(pub_t *p, const pub_chres_t *res, int16_t *const *slots, pub_mc_act_t *act)
{
  (void)slots;
  pub_stats_t *st = &p->st;
  const unsigned nch = p->ring.hdr->channels;
  unsigned ok = 0, ovf = 0, oth = 0;
  for (unsigned c = 0; c < nch; ++c) {
    if (res[c].st == PUB_RX_OVERFLOW) ovf++;
    else if (res[c].st == PUB_RX_OTHER || (res[c].st == PUB_RX_OK && res[c].got <= 0)) oth++;
    else if (res[c].st == PUB_RX_OK) ok++;
  }
  *act = PUB_MC_OTHER;
  int pub_n = 0;
  if (ovf) { st->overflows++; st->consecutive_timeouts = 0; *act = PUB_MC_OVERFLOW; }
  else if (oth) { st->others++; st->consecutive_timeouts = 0; }
  else if (ok == 0) { st->timeouts++; st->consecutive_timeouts++; *act = PUB_MC_IDLE; }
  else {
    st->consecutive_timeouts = 0;
    /* effective chunk of channel c = its carry + what it received now, starting carry samples before the new ones */
    int same = ok == nch, mn = 0, mx = 0;
    int64_t t0 = 0;
    for (unsigned c = 0; same && c < nch; ++c) {
      const int eff = res[c].got + p->carry_n[c];
      const int64_t tk = res[c].tick - p->carry_n[c];
      if (c == 0) { t0 = tk; mn = mx = eff; }
      else { same = tk == t0; if (eff < mn) mn = eff; if (eff > mx) mx = eff; }
    }
    if (!same || mx - mn > p->reserved / PUB_MC_CARRY_MAX_DIV) { st->resyncs++; *act = PUB_MC_MISMATCH; }
    else {
      /* keep the excess of the longer channels, then publish the shortest count */
      if (mx > mn) {
        if (p->carry_cap < mx - mn) {
          for (unsigned c = 0; c < nch; ++c) { free(p->carry[c]); p->carry[c] = malloc((size_t)(mx - mn) * 4); if (!p->carry[c]) abort(); }
          p->carry_cap = mx - mn;
        }
        st->shortened++;
      }
      for (unsigned c = 0; c < nch; ++c) {
        const int eff = res[c].got + p->carry_n[c], ex = eff - mn;
        if (ex > 0) memcpy(p->carry[c], p->base[c] + 2 * (size_t)mn, (size_t)ex * 4);
        p->carry_n[c] = ex;
      }
      *act = PUB_MC_OK;
      if (p->staged) { /* near the ring end: copy path (shm_iq_ring_write wraps and starts a new run if the tick does not continue) */
        shm_iq_ring_write(&p->ring, t0, (void *const *)p->base, mn, nch);
        st->samples += (uint64_t)mn; pub_n = mn; /* not counted in copied: that counter flags run breaks */
      } else pub_n = publish(p, p->base, t0, mn);
    }
  }
  if (*act != PUB_MC_OK) memset(p->carry_n, 0, sizeof(p->carry_n)); /* the caller re-arms the streams: the next ticks jump */
  return pub_n;
}

int64_t pub_tick(int64_t mcr_ticks, int64_t decim, int64_t start_tick)
{
  const int64_t t = mcr_ticks / decim - (mcr_ticks % decim < 0);
  return t > start_tick ? t : start_tick;
}

void pub_rx_shift(int16_t *x, int n, int shift)
{
  if (shift <= 0) return;
  for (int i = 0; i < 2 * n; ++i) x[i] = (int16_t)(x[i] >> shift); /* arithmetic: floor, = srai */
}

void pub_tick_reset_run(pub_tick_track_t *t) { t->first = t->next = -1; }

int64_t pub_tick_track(pub_tick_track_t *t, int64_t tick, int got)
{
  if (t->next < 0) t->first = tick;                                       /* first packet of a run */
  else if (tick < t->next && !t->frozen) t->frozen = 1; /* stamp behind the samples already received: start-time/stale stamps */
  if (t->frozen && t->next >= 0) tick = t->next;
  t->next = tick + got;
  return tick;
}

void pub_radio_reset(pub_t *p) { shm_iq_radio_reset(p->name); }
void pub_radio_line(pub_t *p, const char *line) { shm_iq_radio_append(p->name, line); }

void pub_radio_chpow(pub_t *p, int n)
{
  shm_iq_hdr_t *h = p->ring.hdr;
  const uint64_t head = atomic_load(&h->head), cap = h->capacity;
  if (head == 0) return;
  if ((uint64_t)n > head) n = (int)head;
  if ((uint64_t)n > cap / 2) n = (int)(cap / 2);
  /* the newest n samples, as a contiguous run per channel (the wrap point is skipped by taking the part after it) */
  const uint64_t first = head - (uint64_t)n, i0 = first & (cap - 1);
  const int m = (uint64_t)n > cap - i0 ? (int)(cap - i0) : n;
  const int16_t *ch[SHM_IQ_MAX_CH];
  for (unsigned c = 0; c < h->channels; ++c) ch[c] = p->ring.data[c] + 2 * i0;
  char l[400];
  shm_iq_radio_fmt_chpow(l, sizeof(l), head, ch, h->channels, m);
  shm_iq_radio_append(p->name, l);
}
