/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1: see shm_iq_ring.h. Seqlock-style publication: the producer raises `reserve` before overwriting slots and
 * raises `head` after; a consumer copies, then re-reads `reserve` to know whether its slots were overwritten meanwhile. */
#define _GNU_SOURCE
#include "shm_iq_ring.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t shm_iq_now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void shm_name(char *out, size_t len, const char *name) { snprintf(out, len, "/%s", name); }

static void map_channels(shm_iq_ring_t *r)
{
  char *base = (char *)r->hdr + SHM_IQ_HDR_BYTES;
  for (unsigned c = 0; c < r->hdr->channels; ++c)
    r->data[c] = (int16_t *)(base + (size_t)c * r->hdr->capacity * 4);
}

int shm_iq_ring_create(shm_iq_ring_t *r, const char *name, unsigned channels, uint64_t capacity, double rate, double center_hz)
{
  _Static_assert(sizeof(shm_iq_hdr_t) <= SHM_IQ_HDR_BYTES, "header too large");
  memset(r, 0, sizeof(*r));
  if (!name || !*name || strlen(name) >= sizeof(r->name) - 1 || channels < 1 || channels > SHM_IQ_MAX_CH || capacity < 1024
      || capacity > (1ull << 32) || !(rate > 0)) {
    errno = EINVAL;
    return -1;
  }
  uint64_t cap = 1;
  while (cap < capacity) cap <<= 1;
  shm_name(r->name, sizeof(r->name), name);
  shm_unlink(r->name); /* a previous producer's ring (maybe another geometry): its consumers see the heartbeat stop */
  int fd = shm_open(r->name, O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd < 0) return -1;
  r->map_bytes = SHM_IQ_HDR_BYTES + (uint64_t)channels * cap * 4;
  if (ftruncate(fd, (off_t)r->map_bytes) != 0) { int e = errno; close(fd); shm_unlink(r->name); errno = e; return -1; }
  void *p = mmap(NULL, r->map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) { int e = errno; shm_unlink(r->name); errno = e; return -1; }
  r->hdr = p;
  shm_iq_hdr_t *h = r->hdr;
  h->version = SHM_IQ_VERSION; h->channels = channels; h->capacity = cap; h->rate = rate;
  h->base_tick = INT64_MIN; /* set by the first write */
  double c = center_hz; uint64_t bits; memcpy(&bits, &c, 8);
  atomic_store(&h->center_bits, bits);
  atomic_store(&h->producer_pid, (int32_t)getpid());
  atomic_store(&h->heartbeat_ns, shm_iq_now_ns());
  map_channels(r);
  __atomic_store_n(&h->magic, SHM_IQ_MAGIC, __ATOMIC_RELEASE); /* last: consumers attach only to a fully initialised header */
  return 0;
}

int shm_iq_ring_attach(shm_iq_ring_t *r, const char *name)
{
  memset(r, 0, sizeof(*r));
  if (!name || !*name || strlen(name) >= sizeof(r->name) - 1) { errno = EINVAL; return -1; }
  shm_name(r->name, sizeof(r->name), name);
  int fd = shm_open(r->name, O_RDONLY, 0);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) || st.st_size < (off_t)SHM_IQ_HDR_BYTES) { close(fd); errno = EAGAIN; return -1; }
  void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return -1;
  r->hdr = p; r->map_bytes = (uint64_t)st.st_size;
  const shm_iq_hdr_t *h = r->hdr;
  if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != SHM_IQ_MAGIC || h->version != SHM_IQ_VERSION || h->channels < 1 || h->channels > SHM_IQ_MAX_CH
      || SHM_IQ_HDR_BYTES + (uint64_t)h->channels * h->capacity * 4 != r->map_bytes) {
    munmap(p, r->map_bytes); memset(r, 0, sizeof(*r)); errno = EAGAIN; return -1;
  }
  map_channels(r);
  return 0;
}

void shm_iq_ring_detach(shm_iq_ring_t *r)
{
  if (r->hdr) munmap(r->hdr, r->map_bytes);
  memset(r, 0, sizeof(*r));
}

double shm_iq_ring_center(const shm_iq_ring_t *r)
{
  uint64_t bits = atomic_load(&r->hdr->center_bits);
  double c; memcpy(&c, &bits, 8);
  return c;
}

void shm_iq_ring_new_epoch(shm_iq_ring_t *r, double center_hz)
{
  shm_iq_hdr_t *h = r->hdr;
  uint64_t bits; memcpy(&bits, &center_hz, 8);
  atomic_store(&h->center_bits, bits);
  atomic_store(&r->new_run, 1);
  atomic_fetch_add(&h->rf_epoch, 1);
}

void shm_iq_ring_write(shm_iq_ring_t *r, int64_t tick, void *const *bufs, int n, unsigned channels)
{
  shm_iq_hdr_t *h = r->hdr;
  if (n <= 0) return;
  if (channels > h->channels) channels = h->channels;
  uint64_t head = atomic_load_explicit(&h->head, memory_order_relaxed);
  int64_t off = atomic_load_explicit(&h->tick_offset, memory_order_relaxed);
  const int new_run = atomic_exchange(&r->new_run, 0);
  if (h->base_tick == INT64_MIN) { h->base_tick = tick; off = tick; atomic_store(&h->tick_offset, off); }
  if (tick > off + (int64_t)head) { /* forward gap: keep the tick mapping, skip the index */
    atomic_fetch_add(&h->gaps, 1);
    head = (uint64_t)(tick - off);
    atomic_store(&h->valid_from, head);
  } else if (tick < off + (int64_t)head) {
    /* backward (reload / clock reset): skip the index by the wall-clock gap so caught-up consumers see a timestamp jump */
    atomic_fetch_add(&h->gaps, 1);
    const double gap = (double)(shm_iq_now_ns() - atomic_load_explicit(&h->heartbeat_ns, memory_order_relaxed)) * 1e-9 * h->rate;
    head += gap > 1 ? (uint64_t)gap : 1;
    off = tick - (int64_t)head;
    atomic_store(&h->tick_offset, off);
    atomic_store(&h->valid_from, head);
  } else if (new_run) { /* retune on a continuous clock: lagging consumers drop the old-tuned samples */
    atomic_store(&h->valid_from, head);
  }
  const uint64_t cap = h->capacity, mask = cap - 1;
  atomic_store_explicit(&h->reserve, head + (uint64_t)n, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  for (unsigned c = 0; c < h->channels; ++c) {
    uint64_t pos = head;
    int left = n;
    const int16_t *src = c < channels ? bufs[c] : NULL;
    while (left > 0) {
      uint64_t i = pos & mask;
      int k = (int)((cap - i) < (uint64_t)left ? (cap - i) : (uint64_t)left);
      if (src) memcpy(r->data[c] + 2 * i, src + 2 * (pos - head), (size_t)k * 4);
      else memset(r->data[c] + 2 * i, 0, (size_t)k * 4);
      pos += k; left -= k;
    }
  }
  atomic_store_explicit(&h->head, head + (uint64_t)n, memory_order_release);
  atomic_store_explicit(&h->heartbeat_ns, shm_iq_now_ns(), memory_order_relaxed);
}

int shm_iq_ring_copy(const shm_iq_ring_t *r, uint64_t pos, int n, unsigned channels, int16_t *const *dst)
{
  shm_iq_hdr_t *h = r->hdr;
  const uint64_t cap = h->capacity, mask = cap - 1;
  uint64_t head = atomic_load_explicit(&h->head, memory_order_acquire);
  if (pos + (uint64_t)n > head) return 1;
  if (pos < atomic_load(&h->valid_from) || head - pos > cap) return -1;
  for (unsigned c = 0; c < channels; ++c) {
    uint64_t p = pos;
    int left = n;
    while (left > 0) {
      uint64_t i = p & mask;
      int k = (int)((cap - i) < (uint64_t)left ? (cap - i) : (uint64_t)left);
      memcpy(dst[c] + 2 * (p - pos), r->data[c] + 2 * i, (size_t)k * 4);
      p += k; left -= k;
    }
  }
  atomic_thread_fence(memory_order_acquire);
  uint64_t reserve = atomic_load_explicit(&h->reserve, memory_order_relaxed);
  if (reserve - pos > cap || pos < atomic_load(&h->valid_from)) return -1; /* overwritten while copying */
  return 0;
}
