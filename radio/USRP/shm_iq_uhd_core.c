/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "shm_iq_uhd_core.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(PUB_MAX_CH == SHM_IQ_MAX_CH, "PUB_MAX_CH");

struct pub {
  shm_iq_ring_t ring;
  pub_stats_t st;
  int16_t *stage[SHM_IQ_MAX_CH]; /* copy-path buffers, grown on demand */
  int stage_n;
};

pub_t *pub_open(const char *ring, unsigned channels, uint64_t capacity, double rate, double center_hz, const char *radio_id, unsigned ts)
{
  pub_t *p = calloc(1, sizeof(*p));
  if (!p) return NULL;
  if (shm_iq_ring_create(&p->ring, ring, channels, capacity, rate, center_hz)) { free(p); return NULL; }
  shm_iq_ring_set_meta(&p->ring, radio_id, ts);
  return p;
}

void pub_close(pub_t *p)
{
  if (!p) return;
  for (unsigned c = 0; c < SHM_IQ_MAX_CH; ++c) free(p->stage[c]);
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

int pub_mc_reserve(pub_t *p, int want, int16_t **slots) { return shm_iq_ring_reserve(&p->ring, want, slots); }

int pub_mc_finish(pub_t *p, const pub_chres_t *res, int16_t *const *slots, pub_mc_act_t *act)
{
  pub_stats_t *st = &p->st;
  const unsigned nch = p->ring.hdr->channels;
  unsigned ok = 0, ovf = 0, oth = 0;
  for (unsigned c = 0; c < nch; ++c) {
    if (res[c].st == PUB_RX_OVERFLOW) ovf++;
    else if (res[c].st == PUB_RX_OTHER || (res[c].st == PUB_RX_OK && res[c].got <= 0)) oth++;
    else if (res[c].st == PUB_RX_OK) ok++;
  }
  *act = PUB_MC_OTHER;
  if (ovf) { st->overflows++; st->consecutive_timeouts = 0; *act = PUB_MC_OVERFLOW; return 0; }
  if (oth) { st->others++; st->consecutive_timeouts = 0; return 0; }
  if (ok == 0) { st->timeouts++; st->consecutive_timeouts++; *act = PUB_MC_IDLE; return 0; }
  st->consecutive_timeouts = 0;
  int same = ok == nch;
  for (unsigned c = 1; same && c < nch; ++c) same = res[c].tick == res[0].tick && res[c].got == res[0].got;
  if (!same) { st->resyncs++; *act = PUB_MC_MISMATCH; return 0; }
  *act = PUB_MC_OK;
  return publish(p, slots, res[0].tick, res[0].got);
}

int64_t pub_tick(int64_t mcr_ticks, int64_t decim, int64_t start_tick)
{
  const int64_t t = mcr_ticks / decim - (mcr_ticks % decim < 0);
  return t > start_tick ? t : start_tick;
}
