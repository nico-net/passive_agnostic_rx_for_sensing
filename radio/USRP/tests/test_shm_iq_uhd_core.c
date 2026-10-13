/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* pub_step with a fake recv: in-place path, first-write copy path, overflow gap, ring wrap, error counting, error after a partial
 * write into the slots, uncommitted reserve vs a consumer's copy, pub_tick. */
#include "shm_iq_uhd_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct { int64_t next, skip_at, skip; int fail, scribble; } fake_t;

static pub_rx_status_t fake_recv(void *ctx, int16_t *const *dst, int n, int64_t *tick, int *got)
{
  fake_t *f = ctx;
  if (f->fail > 0) { f->fail--; return f->fail == 1 ? PUB_RX_TIMEOUT : PUB_RX_OVERFLOW; }
  if (f->scribble) { /* UHD copied part of a buffer into the slots, then reported the error: nothing may be published */
    f->scribble = 0;
    for (int c = 0; c < 2; ++c) for (int i = 0; i < 2 * n; ++i) dst[c][i] = -1;
    return PUB_RX_OVERFLOW;
  }
  if (f->skip_at >= 0 && f->next >= f->skip_at) { f->next += f->skip; f->skip_at = -1; }
  *got = n > 700 ? 700 : n; /* short reads exercise the wrap handling */
  *tick = f->next;
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < *got; ++i) { dst[c][2 * i] = (int16_t)((f->next + i) & 0x7fff); dst[c][2 * i + 1] = (int16_t)c; }
  f->next += *got;
  return PUB_RX_OK;
}

int main(void)
{
  char name[64];
  snprintf(name, sizeof(name), "uhdcore_%d", (int)getpid());
  shm_iq_ring_t c;
  pub_t *pb = pub_open(name, 2, 4096, 1e6, 3.5e9, "test", SHM_IQ_TS_FREE);
  CHECK(pb);
  shm_iq_ring_t *r = pub_ring(pb);
  fake_t f = {.next = 1000, .skip_at = 1000 + 700 * 10, .skip = 5000};
  const pub_stats_t *sp = pub_stats(pb);
  for (int k = 0; k < 40; ++k) pub_step(pb, fake_recv, &f, 1000);
  CHECK(sp->copied == 2); /* first write + the overflow gap */
  CHECK(sp->inplace > 30);
  CHECK(atomic_load(&r->hdr->gaps) == 1); /* the first write is not a gap */
  CHECK(shm_iq_ring_attach(&c, name) == 0);
  const uint64_t head = atomic_load(&c.hdr->head);
  const int64_t off = atomic_load(&c.hdr->tick_offset);
  CHECK(off + (int64_t)head == f.next); /* index <-> tick mapping survived the gap */
  int16_t b0[2000], b1[2000], *dst[2] = {b0, b1};
  CHECK(shm_iq_ring_copy(&c, head - 1000, 1000, 2, dst) == 0);
  for (int i = 0; i < 1000; ++i) {
    CHECK(b0[2 * i] == (int16_t)((off + (int64_t)(head - 1000) + i) & 0x7fff));
    CHECK(b1[2 * i + 1] == 1);
  }
  CHECK(atomic_load(&c.hdr->valid_from) >= 7000); /* the gap started a new run */
  f.fail = 3;
  for (int k = 0; k < 3; ++k) CHECK(pub_step(pb, fake_recv, &f, 1000) == 0);
  CHECK(sp->overflows == 2 && sp->timeouts == 1 && sp->consecutive_timeouts == 0);
  /* error after a partial write: head unchanged, the next packet (same tick) overwrites the slots in place, no gap */
  const uint64_t h0 = atomic_load(&c.hdr->head), cp0 = sp->copied;
  f.scribble = 1;
  CHECK(pub_step(pb, fake_recv, &f, 1000) == 0 && atomic_load(&c.hdr->head) == h0);
  for (int k = 0; k < 3; ++k) CHECK(pub_step(pb, fake_recv, &f, 1000) > 0);
  CHECK(sp->copied == cp0 && atomic_load(&r->hdr->gaps) == 1);
  const uint64_t h1 = atomic_load(&c.hdr->head);
  CHECK(h1 - h0 >= 1000 && shm_iq_ring_copy(&c, h0, 1000, 2, dst) == 0);
  for (int i = 0; i < 1000; ++i) CHECK(b0[2 * i] == (int16_t)((off + (int64_t)h0 + i) & 0x7fff) && b1[2 * i + 1] == 1);
  /* seqlock: a reserve not yet committed (recv in flight) already invalidates the oldest slots it may overwrite */
  int16_t *slots[SHM_IQ_MAX_CH];
  const int nr = shm_iq_ring_reserve(r, 1000, slots);
  CHECK(nr > 0 && atomic_load(&c.hdr->head) == h1);
  CHECK(shm_iq_ring_copy(&c, h1 - 4096, 1, 2, dst) == -1);
  CHECK(shm_iq_ring_copy(&c, h1 - 4096 + (uint64_t)nr, 100, 2, dst) == 0);
  /* pub_tick: floor keeps odd-MCR-tick starts consistent; never before the scheduled START */
  CHECK(pub_tick(1001, 2, 0) == 500 && pub_tick(1003, 2, 0) == 501 && pub_tick(1001, 1, 0) == 1001);
  CHECK(pub_tick(10, 2, 400) == 400);
  shm_iq_ring_detach(&c);
  pub_close(pb);
  char path[80];
  snprintf(path, sizeof(path), "/%s", name);
  shm_unlink(path);
  puts("test_shm_iq_uhd_core OK");
  return 0;
}
