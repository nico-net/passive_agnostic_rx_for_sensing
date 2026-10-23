/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* pub_step with a fake recv: in-place path, first-write copy path, overflow gap, ring wrap, error counting, error after a partial
 * write into the slots, uncommitted reserve vs a consumer's copy, pub_tick, multi-streamer barrier (pub_mc_*) with per-channel fake sources. */
#include "shm_iq_uhd_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
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

/* multi-streamer: one fake source per channel, each fills only its own slot, like one recv thread per channel */
typedef struct { int64_t next; int len; pub_rx_status_t st; } chsrc_t;
static void mc_chunk(pub_t *p, chsrc_t *s, int nch, int want, int *pubd, pub_mc_act_t *act)
{
  int16_t *slots[SHM_IQ_MAX_CH];
  pub_chres_t res[SHM_IQ_MAX_CH];
  const int n = pub_mc_reserve(p, want, slots);
  for (int c = 0; c < nch; ++c) {
    const int w = pub_mc_want(p, (unsigned)c, n); /* a channel with carried samples receives that many fewer */
    res[c].st = s[c].st;
    res[c].tick = s[c].next;
    res[c].got = s[c].len < w ? s[c].len : w;
    if (s[c].st == PUB_RX_OK) {
      for (int i = 0; i < res[c].got; ++i) { slots[c][2 * i] = (int16_t)((s[c].next + i) & 0x7fff); slots[c][2 * i + 1] = (int16_t)c; }
      s[c].next += res[c].got;
    }
  }
  *pubd = pub_mc_finish(p, res, slots, act);
}

static void test_multi(void)
{
  char name[64];
  snprintf(name, sizeof(name), "uhdmc_%d", (int)getpid());
  pub_t *pb = pub_open(name, 2, 4096, 1e6, 3.5e9, "test", SHM_IQ_TS_FREE);
  CHECK(pb);
  shm_iq_ring_t *r = pub_ring(pb), c;
  const pub_stats_t *sp = pub_stats(pb);
  chsrc_t s[2] = {{5000, 700, PUB_RX_OK}, {5000, 700, PUB_RX_OK}};
  int pd;
  pub_mc_act_t a;
  for (int k = 0; k < 20; ++k) { mc_chunk(pb, s, 2, 1000, &pd, &a); CHECK(pd > 0 && a == PUB_MC_OK); } /* aligned, wraps 4096 several times */
  CHECK(sp->copied == 1 && sp->inplace == 19 && sp->resyncs == 0 && atomic_load(&r->hdr->gaps) == 0);
  CHECK(shm_iq_ring_attach(&c, name) == 0);
  uint64_t head = atomic_load(&c.hdr->head);
  int16_t b0[1000], b1[1000], *dst[2] = {b0, b1};
  CHECK(shm_iq_ring_copy(&c, head - 400, 400, 2, dst) == 0);
  for (int i = 0; i < 400; ++i) CHECK(b0[2 * i] == (int16_t)((5000 + (int64_t)(head - 400) + i) & 0x7fff) && b1[2 * i + 1] == 1);
  /* one channel short: nothing published, resync counted */
  s[1].len = 300;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 0 && a == PUB_MC_MISMATCH && sp->resyncs == 1 && atomic_load(&c.hdr->head) == head);
  CHECK(shm_iq_ring_copy(&c, head, 1, 2, dst) == 1); /* the refused, half-written slots are never readable */
  /* refused chunk, then the streams simply continue (no re-arm): its samples are lost -> one forward gap, exact tick mapping */
  s[1].len = 700;
  s[1].next = s[0].next;
  const uint64_t g0 = atomic_load(&r->hdr->gaps);
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 700 && a == PUB_MC_OK && atomic_load(&r->hdr->gaps) == g0 + 1);
  head = atomic_load(&c.hdr->head);
  CHECK(atomic_load(&c.hdr->tick_offset) + (int64_t)head == s[0].next && shm_iq_ring_copy(&c, head - 400, 400, 2, dst) == 0); /* b0/b1 hold 500 samples */
  for (int i = 0; i < 400; ++i) CHECK(b0[2 * i] == (int16_t)((s[0].next - 400 + i) & 0x7fff) && b1[2 * i + 1] == 1);
  /* one channel overflows (the other delivered): OVERFLOW, nothing published; restart on a new common tick -> one gap */
  s[0].st = PUB_RX_OVERFLOW;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 0 && a == PUB_MC_OVERFLOW && sp->overflows == 1 && atomic_load(&c.hdr->head) == head);
  s[0].st = PUB_RX_OK;
  s[0].next = 90000; s[1].next = 90000 + 64; /* restarted but not on the same tick: still refused */
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 0 && a == PUB_MC_MISMATCH && sp->resyncs == 2);
  s[0].next = s[1].next = 90000; /* common tick after the re-arm */
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd > 0 && a == PUB_MC_OK && sp->copied == 3 && atomic_load(&r->hdr->gaps) == 2);
  head = atomic_load(&c.hdr->head);
  CHECK(atomic_load(&c.hdr->tick_offset) + (int64_t)head == s[0].next);
  /* partial chunk: both channels deliver fewer samples than reserved, equal */
  s[0].len = s[1].len = 123;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 123 && a == PUB_MC_OK);
  /* a timeout on one channel only is a mismatch; all channels timing out is idle */
  s[0].len = s[1].len = 700;
  s[1].st = PUB_RX_TIMEOUT;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 0 && a == PUB_MC_MISMATCH && sp->consecutive_timeouts == 0);
  s[0].st = PUB_RX_TIMEOUT;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 0 && a == PUB_MC_IDLE && sp->consecutive_timeouts == 1 && sp->timeouts == 1);
  shm_iq_ring_detach(&c);
  pub_close(pb);
  char path[80];
  snprintf(path, sizeof(path), "/%s", name);
  shm_unlink(path);
}


/* a harmless count mismatch (same start tick, counts differ by < chunk/4) publishes the shorter count and carries the excess:
 * no restart, no gap, every channel stays aligned, including across the ring end (staging path) */
static void test_multi_carry(void)
{
  char name[64];
  snprintf(name, sizeof(name), "uhdmk_%d", (int)getpid());
  pub_t *pb = pub_open(name, 2, 4096, 1e6, 3.5e9, "test", SHM_IQ_TS_FREE);
  CHECK(pb);
  shm_iq_ring_t *r = pub_ring(pb), c;
  const pub_stats_t *sp = pub_stats(pb);
  chsrc_t s[2] = {{7000, 700, PUB_RX_OK}, {7000, 700, PUB_RX_OK}};
  int pd;
  pub_mc_act_t a;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 700 && a == PUB_MC_OK);
  CHECK(shm_iq_ring_attach(&c, name) == 0);
  int64_t expect = 7000 + 700;
  /* ch0 delivers 700, ch1 only 600: publish 600, ch0 keeps 100 */
  s[1].len = 600;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 600 && a == PUB_MC_OK && sp->shortened == 1 && sp->resyncs == 0);
  expect += 600;
  CHECK(atomic_load(&c.hdr->tick_offset) + (int64_t)atomic_load(&c.hdr->head) == expect);
  /* many chunks with alternating shorter channel, through several ring wraps (4096) */
  uint64_t g0 = atomic_load(&r->hdr->gaps);
  for (int k = 0; k < 60; ++k) {
    s[k & 1].len = 700; s[1 - (k & 1)].len = 640 + (k % 7) * 10; /* difference <= 60.. never above 250 */
    mc_chunk(pb, s, 2, 1000, &pd, &a);
    CHECK(pd > 0 && a == PUB_MC_OK && sp->resyncs == 0);
    expect += pd;
    CHECK(atomic_load(&c.hdr->tick_offset) + (int64_t)atomic_load(&c.hdr->head) == expect);
  }
  CHECK(atomic_load(&r->hdr->gaps) == g0 && sp->shortened > 20 && sp->staged > 0);
  uint64_t head = atomic_load(&c.hdr->head);
  int16_t b0[2000], b1[2000], *dst[2] = {b0, b1};
  CHECK(shm_iq_ring_copy(&c, head - 1000, 1000, 2, dst) == 0);
  for (int i = 0; i < 1000; ++i) {
    const int16_t v = (int16_t)((expect - 1000 + i) & 0x7fff);
    CHECK(b0[2 * i] == v && b1[2 * i] == v && b0[2 * i + 1] == 0 && b1[2 * i + 1] == 1); /* both channels hold the same sample numbers */
  }
  /* a difference above a quarter of the chunk is still a mismatch and drops the carry (restart) */
  s[0].len = 700; s[1].len = 300;
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 0 && a == PUB_MC_MISMATCH && sp->resyncs == 1);
  s[0].next = s[1].next = 123456; s[0].len = s[1].len = 700; /* re-armed on a common tick */
  mc_chunk(pb, s, 2, 1000, &pd, &a);
  CHECK(pd == 700 && a == PUB_MC_OK);
  shm_iq_ring_detach(&c);
  pub_close(pb);
  char path[80];
  snprintf(path, sizeof(path), "/%s", name);
  shm_unlink(path);
}

/* radio side channel: append / poll / rotation keeps the RFRING header, CHPOW numbers */
static void test_radio(void)
{
  char name[64];
  snprintf(name, sizeof(name), "uhdrad_%d", (int)getpid());
  pub_t *pb = pub_open(name, 2, 4096, 1e6, 3.5e9, "test", SHM_IQ_TS_FREE);
  CHECK(pb);
  pub_radio_reset(pb);
  pub_radio_line(pb, "RFRING ch0 port=RX1 gain=38.00 freq_hz=3590190000 rate=122880000 bw_hz=122880000");
  pub_radio_line(pb, "SENSING: RFSTALL SHM_PUB reason=overflow overflows=1 timeouts=0 resyncs=0");
  char buf[4096];
  uint64_t off = 0;
  CHECK(shm_iq_radio_poll(name, &off, buf, sizeof(buf)) > 0 && strstr(buf, "RFRING ch0") && strstr(buf, "RFSTALL SHM_PUB"));
  CHECK(shm_iq_radio_poll(name, &off, buf, sizeof(buf)) == 0); /* nothing new */
  /* full-scale-ish tone of amplitude A on ch0 (power A^2 / 32768^2) and half of it on ch1: CHPOW = 20 log10(A/32768) */
  int16_t *slots[SHM_IQ_MAX_CH];
  const int n = shm_iq_ring_reserve(pub_ring(pb), 1000, slots);
  for (int i = 0; i < n; ++i) { slots[0][2 * i] = 3277; slots[0][2 * i + 1] = 0; slots[1][2 * i] = 0; slots[1][2 * i + 1] = 1638; }
  CHECK(shm_iq_ring_commit(pub_ring(pb), 10, n) == -1); /* first write: copy path */
  void *bufs[2] = {slots[0], slots[1]};
  shm_iq_ring_write(pub_ring(pb), 10, bufs, n, 2);
  pub_radio_chpow(pb, 512);
  CHECK(shm_iq_radio_poll(name, &off, buf, sizeof(buf)) > 0);
  double p0 = 0, p1 = 0;
  const char *c0 = strstr(buf, "ch0="), *c1 = strstr(buf, "ch1=");
  CHECK(strstr(buf, "SENSING: CHPOW sample=") && c0 && c1 && sscanf(c0, "ch0=%lf", &p0) == 1 && sscanf(c1, "ch1=%lf", &p1) == 1);
  CHECK(fabs(p0 - 20 * log10(3277.0 / 32768.0)) < 0.01 && fabs(p1 - 20 * log10(1638.0 / 32768.0)) < 0.01);
  /* rotation: far more than 64 KiB keeps the header and a reader that was ahead restarts from 0 */
  for (int i = 0; i < 3000; ++i) pub_radio_line(pb, "SENSING: CHPOW sample=1 dBFS ch0=-40.00 ch1=-40.00 ch2=-40.00 ch3=-40.00 padpadpadpadpadpadpadpad");
  uint64_t off2 = 1ull << 40;
  CHECK(shm_iq_radio_poll(name, &off2, buf, sizeof(buf)) > 0 && off2 < 4096 && strstr(buf, "RFRING ch0"));
  pub_close(pb); /* removes the file */
  CHECK(shm_iq_radio_poll(name, &off, buf, sizeof(buf)) == 0);
  char path[80];
  snprintf(path, sizeof(path), "/%s", name);
  shm_unlink(path);
}

static void test_default_capacity(void)
{
  const uint64_t G = 1ull << 30;
  CHECK(pub_default_capacity(4, 100 * G, 16 * G) == (1ull << 27)); /* 2 GB ring, 84 GB spare */
  CHECK(pub_default_capacity(8, 100 * G, 16 * G) == (1ull << 27)); /* 4 GB */
  CHECK(pub_default_capacity(4, 20 * G, 16 * G) == (1ull << 26));  /* 4 GB spare: the ring may use half of it */
  CHECK(pub_default_capacity(4, 17 * G, 16 * G) == (1ull << 24));
  CHECK(pub_default_capacity(4, 10 * G, 16 * G) == (1ull << 23));  /* below the floor: the smallest ring */
}

/* usrp_lib X4x0 rxshift: arithmetic >> 2 (floor, = simde_mm256_srai_epi16), applied in place; shift 0 leaves the data alone */
static void test_rx_shift(void)
{
  int16_t x[8] = {7, -7, 4, -4, 32767, -32768, 1, -1}, y[8];
  for (int i = 0; i < 8; ++i) y[i] = x[i];
  pub_rx_shift(y, 4, 0);
  for (int i = 0; i < 8; ++i) CHECK(y[i] == x[i]);
  pub_rx_shift(y, 4, 2);
  const int16_t want[8] = {1, -2, 1, -1, 8191, -8192, 0, -1};
  for (int i = 0; i < 8; ++i) CHECK(y[i] == want[i]);
}

/* frozen timestamps: every packet stamped with the start tick -> ticks from the sample count; a normal stream is untouched */
static void test_tick_track(void)
{
  pub_tick_track_t t = {-1, -1, 0};
  pub_tick_reset_run(&t);
  CHECK(pub_tick_track(&t, 1000, 100) == 1000 && pub_tick_track(&t, 1100, 100) == 1100 && !t.frozen);
  CHECK(pub_tick_track(&t, 1300, 100) == 1300 && !t.frozen); /* a real gap stays a gap */
  pub_tick_reset_run(&t);
  CHECK(pub_tick_track(&t, 5000, 50) == 5000);
  CHECK(pub_tick_track(&t, 5000, 50) == 5050 && t.frozen);
  CHECK(pub_tick_track(&t, 5000, 70) == 5100 && pub_tick_track(&t, 5000, 10) == 5170);
  pub_tick_track_t u = {-1, -1, 0};                         /* on-air pattern 2026-10-15: stamp 68 ticks EARLIER each chunk */
  CHECK(pub_tick_track(&u, 342286654, 65536) == 342286654 && pub_tick_track(&u, 342286586, 65536) == 342352190 && u.frozen);
  pub_tick_reset_run(&t);                                   /* restart: first packet of the new run is taken as is */
  CHECK(pub_tick_track(&t, 9000, 10) == 9000 && pub_tick_track(&t, 9000, 10) == 9010);
}

int main(void)
{
  test_rx_shift();
  test_tick_track();
  test_multi();
  test_multi_carry();
  test_default_capacity();
  test_radio();
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
