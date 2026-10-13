/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "shm_iq_uhd_core.h"
#include <stdlib.h>
#include <string.h>

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

int pub_step(pub_t *p, pub_recv_fn recv, void *ctx, int want)
{
  shm_iq_ring_t *ring = &p->ring;
  pub_stats_t *st = &p->st;
  int16_t *slots[SHM_IQ_MAX_CH];
  const unsigned nch = ring->hdr->channels;
  const int n = shm_iq_ring_reserve(ring, want, slots);
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

