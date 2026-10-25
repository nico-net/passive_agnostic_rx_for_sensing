/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI M1/C1: see shm_iq_ring.h. Seqlock-style publication: the producer raises `reserve` before overwriting slots and
 * raises `head` after; a consumer copies, then re-reads `reserve` to know whether its slots were overwritten meanwhile.
 * The ring body is identical for both backings; only create/attach/detach differ. The anonymous path never calls shm_open (test_shm_iq counts the
 * calls with a linker --wrap); the POSIX path funnels every shm_open through ring_shm_open() below. */
#define _GNU_SOURCE
#include "shm_iq_ring.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <fcntl.h>
#include <pthread.h>
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

uint64_t shm_iq_ring_bytes(unsigned n_rx, uint64_t capacity) { return SHM_IQ_HDR_BYTES + (uint64_t)n_rx * capacity * 4; }

/* ---------------------------------------------------------------- capacity plan */
uint64_t shm_iq_ring_plan_capacity(size_t budget_bytes, double rate, unsigned n_rx, unsigned n_rings, double target_ms, double floor_ms)
{
  if (!(rate > 0) || n_rx < 1 || n_rx > SHM_IQ_MAX_CH || n_rings < 1 || !(target_ms > 0) || floor_ms < 0) return 0;
  const double want_d = rate * target_ms * 1e-3, floor_d = rate * floor_ms * 1e-3;
  if (want_d > (double)(1ull << 32)) return 0;
  uint64_t want = 1;
  while ((double)want < want_d) want <<= 1; /* ceil_pow2: the ring is never shorter than asked when the budget allows */
  if (want < 1024) want = 1024;
  uint64_t cap = want;
  if (budget_bytes != SIZE_MAX)
    while (cap > 1024 && (uint64_t)n_rings * shm_iq_ring_bytes(n_rx, cap) > (uint64_t)budget_bytes) cap >>= 1;
  if (budget_bytes != SIZE_MAX && (uint64_t)n_rings * shm_iq_ring_bytes(n_rx, cap) > (uint64_t)budget_bytes) return 0;
  if ((double)cap < floor_d) return 0;
  return cap;
}

/* ---------------------------------------------------------------- anonymous backing */
typedef struct {
  _Atomic int ref;
  void *map;
  uint64_t bytes;
  char name[64]; /* registry name, "" = unnamed */
} anon_t;

#define ANON_REG_MAX 64
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static anon_t *g_reg[ANON_REG_MAX];

static void anon_unref(anon_t *a)
{
  if (atomic_fetch_sub(&a->ref, 1) != 1) return;
  munmap(a->map, a->bytes);
  free(a);
}

static void anon_unregister(anon_t *a)
{
  pthread_mutex_lock(&g_reg_mu);
  for (int i = 0; i < ANON_REG_MAX; ++i)
    if (g_reg[i] == a) { g_reg[i] = NULL; atomic_fetch_sub(&a->ref, 1); } /* the registry's reference (not the last: the caller still holds one) */
  pthread_mutex_unlock(&g_reg_mu);
}

static void bind_anon(shm_iq_ring_t *r, anon_t *a, int owner)
{
  r->anon = a; r->hdr = a->map; r->map_bytes = a->bytes; r->backing = SHM_IQ_BACKING_ANON; r->owner = owner; r->writable = 1;
  snprintf(r->name, sizeof(r->name), "%s", a->name);
  map_channels(r);
}

/* the one place the POSIX backing calls shm_open */
static int ring_shm_open(const char *name, int flags, mode_t mode) { return shm_open(name, flags, mode); }

int shm_iq_ring_create(shm_iq_ring_t *r, shm_iq_backing_t backing, const char *name, unsigned n_rx, uint64_t capacity, double rate, double center_hz,
                       unsigned n_consumers)
{
  _Static_assert(sizeof(shm_iq_hdr_t) <= SHM_IQ_HDR_BYTES, "header too large");
  memset(r, 0, sizeof(*r));
  const int posix = backing == SHM_IQ_BACKING_POSIX_SHM;
  if ((backing != SHM_IQ_BACKING_ANON && !posix) || (posix && (!name || !*name)) || (name && strlen(name) >= sizeof(r->name) - 1) || n_rx < 1 || n_rx > SHM_IQ_MAX_CH
      || capacity < 1024 || capacity > (1ull << 32) || !(rate > 0) || n_consumers > SHM_IQ_MAX_CONS) {
    errno = EINVAL;
    return -1;
  }
  uint64_t cap = 1;
  while (cap < capacity) cap <<= 1;
  const uint64_t bytes = shm_iq_ring_bytes(n_rx, cap);
  void *p;
  if (posix) {
    shm_name(r->name, sizeof(r->name), name);
    shm_unlink(r->name); /* a previous producer's ring (maybe another geometry): its consumers see the heartbeat stop */
    int fd = ring_shm_open(r->name, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)bytes) != 0) { int e = errno; close(fd); shm_unlink(r->name); errno = e; return -1; }
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { int e = errno; shm_unlink(r->name); errno = e; return -1; }
    r->hdr = p; r->map_bytes = bytes; r->backing = SHM_IQ_BACKING_POSIX_SHM; r->owner = 1; r->writable = 1;
  } else {
    anon_t *a = calloc(1, sizeof(*a));
    if (!a) return -1;
    p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); /* zero-filled, private to the process: no file, no shm_open */
    if (p == MAP_FAILED) { int e = errno; free(a); errno = e; return -1; }
    a->map = p; a->bytes = bytes; atomic_init(&a->ref, 1);
    if (name && *name) {
      snprintf(a->name, sizeof(a->name), "%s", name);
      pthread_mutex_lock(&g_reg_mu);
      int slot = -1;
      for (int i = 0; i < ANON_REG_MAX; ++i) {
        if (g_reg[i] && !strcmp(g_reg[i]->name, a->name)) { slot = -2; break; }
        if (!g_reg[i] && slot == -1) slot = i;
      }
      if (slot >= 0) { g_reg[slot] = a; atomic_fetch_add(&a->ref, 1); }
      pthread_mutex_unlock(&g_reg_mu);
      if (slot < 0) { munmap(p, bytes); free(a); errno = slot == -2 ? EEXIST : ENOSPC; return -1; }
    }
    bind_anon(r, a, 1);
  }
  shm_iq_hdr_t *h = r->hdr;
  h->version = SHM_IQ_VERSION; h->channels = n_rx; h->capacity = cap; h->rate = rate;
  h->n_consumers = n_consumers; h->backing = (uint32_t)backing;
  h->base_tick = INT64_MIN; /* set by the first write */
  double c = center_hz; uint64_t bits; memcpy(&bits, &c, 8);
  atomic_store(&h->center_bits, bits);
  atomic_store(&h->producer_pid, (int32_t)getpid());
  atomic_store(&h->heartbeat_ns, shm_iq_now_ns());
  map_channels(r);
  __atomic_store_n(&h->magic, SHM_IQ_MAGIC, __ATOMIC_RELEASE); /* last: consumers attach only to a fully initialised header */
  return 0;
}

static int check_hdr(const shm_iq_ring_t *r)
{
  const shm_iq_hdr_t *h = r->hdr;
  return __atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == SHM_IQ_MAGIC && h->version >= 1 && h->version <= SHM_IQ_VERSION && h->channels >= 1
         && h->channels <= SHM_IQ_MAX_CH && shm_iq_ring_bytes(h->channels, h->capacity) == r->map_bytes;
}

int shm_iq_ring_view(shm_iq_ring_t *view, const shm_iq_ring_t *producer)
{
  memset(view, 0, sizeof(*view));
  if (!producer || producer->backing != SHM_IQ_BACKING_ANON || !producer->anon) { errno = EINVAL; return -1; }
  anon_t *a = producer->anon;
  atomic_fetch_add(&a->ref, 1);
  bind_anon(view, a, 0);
  return 0;
}

int shm_iq_ring_attach(shm_iq_ring_t *r, const char *name)
{
  memset(r, 0, sizeof(*r));
  if (!name || !*name || strlen(name) >= sizeof(r->name) - 1) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&g_reg_mu);
  anon_t *a = NULL;
  for (int i = 0; i < ANON_REG_MAX; ++i)
    if (g_reg[i] && !strcmp(g_reg[i]->name, name)) { a = g_reg[i]; atomic_fetch_add(&a->ref, 1); break; }
  pthread_mutex_unlock(&g_reg_mu);
  if (a) { bind_anon(r, a, 0); return 0; }
  shm_name(r->name, sizeof(r->name), name);
  int fd = ring_shm_open(r->name, O_RDWR, 0); /* RDWR: the consumer table is written by consumers; falls back to a read-only, untracked attach */
  r->writable = fd >= 0;
  if (fd < 0) fd = ring_shm_open(r->name, O_RDONLY, 0);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) || st.st_size < (off_t)SHM_IQ_HDR_BYTES) { close(fd); errno = EAGAIN; return -1; }
  void *p = mmap(NULL, (size_t)st.st_size, r->writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return -1;
  r->hdr = p; r->map_bytes = (uint64_t)st.st_size; r->backing = SHM_IQ_BACKING_POSIX_SHM;
  if (!check_hdr(r)) {
    munmap(p, r->map_bytes); memset(r, 0, sizeof(*r)); errno = EAGAIN; return -1;
  }
  if (r->hdr->version < 3) r->writable = 0; /* no consumer table in v1/v2 */
  map_channels(r);
  return 0;
}

void shm_iq_ring_detach(shm_iq_ring_t *r)
{
  if (r->backing == SHM_IQ_BACKING_ANON && r->anon) {
    anon_t *a = r->anon;
    if (r->owner && a->name[0]) anon_unregister(a);
    anon_unref(a);
  } else if (r->hdr) munmap(r->hdr, r->map_bytes);
  memset(r, 0, sizeof(*r));
}

void shm_iq_ring_prefault(shm_iq_ring_t *r)
{
  if (r->backing != SHM_IQ_BACKING_ANON || !r->hdr) return;
  const long pg = sysconf(_SC_PAGESIZE);
  volatile char *b = (volatile char *)r->hdr;
  for (uint64_t o = SHM_IQ_HDR_BYTES; o < r->map_bytes; o += (uint64_t)pg) b[o] = 0; /* data area is zero, so storing 0 changes nothing and faults the page in */
}

double shm_iq_ring_center(const shm_iq_ring_t *r)
{
  uint64_t bits = atomic_load(&r->hdr->center_bits);
  double c; memcpy(&c, &bits, 8);
  return c;
}

void shm_iq_ring_set_meta(shm_iq_ring_t *r, const char *radio_id, unsigned time_source)
{
  if (radio_id) snprintf(r->hdr->radio_id, sizeof(r->hdr->radio_id), "%s", radio_id);
  r->hdr->time_source = time_source;
}

void shm_iq_ring_set_away(shm_iq_ring_t *r, uint64_t until_ns)
{
  atomic_store(&r->hdr->dwell_until_ns, until_ns);
  atomic_store(&r->hdr->away, 1);
}

void shm_iq_ring_return(shm_iq_ring_t *r, double center_hz)
{
  /* away clears at the next write, after its fresh heartbeat (consumers read away first); the heartbeat is not
   * refreshed here: a backward tick at that write (device reload while away) sizes its jump from it */
  shm_iq_ring_new_epoch(r, center_hz);
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
  if (atomic_load_explicit(&h->away, memory_order_relaxed)) { atomic_store(&h->away, 0); atomic_store(&h->dwell_until_ns, 0); }
}

int shm_iq_ring_reserve(shm_iq_ring_t *r, int want, int16_t **slots)
{
  shm_iq_hdr_t *h = r->hdr;
  const uint64_t head = atomic_load_explicit(&h->head, memory_order_relaxed), i = head & (h->capacity - 1);
  const uint64_t room = h->capacity - i;
  const int n = (uint64_t)want < room ? want : (int)room;
  atomic_store_explicit(&h->reserve, head + (uint64_t)n, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  for (unsigned c = 0; c < h->channels; ++c) slots[c] = r->data[c] + 2 * i;
  return n;
}

int shm_iq_ring_commit(shm_iq_ring_t *r, int64_t tick, int got)
{
  shm_iq_hdr_t *h = r->hdr;
  const uint64_t head = atomic_load_explicit(&h->head, memory_order_relaxed);
  if (got <= 0) return 0;
  if (h->base_tick == INT64_MIN || atomic_load(&r->new_run)
      || tick != atomic_load_explicit(&h->tick_offset, memory_order_relaxed) + (int64_t)head)
    return -1;
  atomic_store_explicit(&h->head, head + (uint64_t)got, memory_order_release);
  atomic_store_explicit(&h->heartbeat_ns, shm_iq_now_ns(), memory_order_relaxed);
  if (atomic_load_explicit(&h->away, memory_order_relaxed)) { atomic_store(&h->away, 0); atomic_store(&h->dwell_until_ns, 0); }
  return 0;
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

/* ---------------------------------------------------------------- consumer table */
static shm_iq_cons_t *cons_at(const shm_iq_ring_t *r, int id)
{
  if (id < 0 || !r->hdr || r->hdr->version < 3 || (uint32_t)id >= r->hdr->n_consumers || (uint32_t)id >= SHM_IQ_MAX_CONS) return NULL;
  return &r->hdr->cons[id];
}

int shm_iq_ring_consumer_register(shm_iq_ring_t *r)
{
  if (!r->hdr || r->hdr->version < 3 || !r->hdr->n_consumers || !r->writable) { errno = ENOSYS; return -1; }
  for (uint32_t i = 0; i < r->hdr->n_consumers && i < SHM_IQ_MAX_CONS; ++i) {
    uint32_t z = 0;
    if (atomic_compare_exchange_strong(&r->hdr->cons[i].active, &z, 1u)) {
      atomic_store(&r->hdr->cons[i].pos, atomic_load(&r->hdr->head));
      atomic_store(&r->hdr->cons[i].overruns, 0);
      atomic_store(&r->hdr->cons[i].dropped, 0);
      return (int)i;
    }
  }
  errno = ENOSPC;
  return -1;
}

void shm_iq_ring_consumer_release(shm_iq_ring_t *r, int id)
{
  shm_iq_cons_t *c = r->writable ? cons_at(r, id) : NULL;
  if (c) atomic_store(&c->active, 0);
}

void shm_iq_ring_consumer_update(shm_iq_ring_t *r, int id, uint64_t pos)
{
  shm_iq_cons_t *c = r->writable ? cons_at(r, id) : NULL;
  if (c) atomic_store_explicit(&c->pos, pos, memory_order_relaxed);
}

void shm_iq_ring_consumer_overrun(shm_iq_ring_t *r, int id, uint64_t dropped, int overrun)
{
  shm_iq_cons_t *c = r->writable ? cons_at(r, id) : NULL;
  if (!c) return;
  atomic_fetch_add(&c->dropped, dropped);
  if (overrun) { atomic_fetch_add(&c->overruns, 1); atomic_fetch_add(&r->hdr->overruns, 1); }
}

uint64_t shm_iq_ring_overruns(const shm_iq_ring_t *r) { return r->hdr->version >= 3 ? atomic_load(&r->hdr->overruns) : 0; }
uint64_t shm_iq_ring_consumer_overruns(const shm_iq_ring_t *r, int id) { const shm_iq_cons_t *c = cons_at(r, id); return c ? atomic_load(&c->overruns) : 0; }
uint64_t shm_iq_ring_consumer_dropped(const shm_iq_ring_t *r, int id) { const shm_iq_cons_t *c = cons_at(r, id); return c ? atomic_load(&c->dropped) : 0; }

uint64_t shm_iq_ring_consumer_lag(const shm_iq_ring_t *r, int id)
{
  const shm_iq_cons_t *c = cons_at(r, id);
  if (!c) return 0;
  const uint64_t head = atomic_load(&r->hdr->head), pos = atomic_load(&c->pos);
  return head > pos ? head - pos : 0;
}

uint64_t shm_iq_ring_max_lag(const shm_iq_ring_t *r)
{
  uint64_t m = 0;
  if (r->hdr->version < 3) return 0;
  for (uint32_t i = 0; i < r->hdr->n_consumers && i < SHM_IQ_MAX_CONS; ++i)
    if (atomic_load(&r->hdr->cons[i].active)) { const uint64_t l = shm_iq_ring_consumer_lag(r, (int)i); if (l > m) m = l; }
  return m;
}

/* ---------------------------------------------------------------- radio side channel */
#define RADIO_MAX_BYTES 65536

static void radio_path(char *out, size_t len, const char *ring) { snprintf(out, len, "/dev/shm/%s.radio", ring); }

void shm_iq_radio_reset(const char *ring)
{
  char p[160];
  radio_path(p, sizeof(p), ring);
  const int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) close(fd);
}

void shm_iq_radio_remove(const char *ring)
{
  char p[160];
  radio_path(p, sizeof(p), ring);
  unlink(p);
}

void shm_iq_radio_append(const char *ring, const char *line)
{
  char p[160];
  radio_path(p, sizeof(p), ring);
  const int fd = open(p, O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  struct stat st;
  if (fstat(fd, &st) == 0 && st.st_size > RADIO_MAX_BYTES) { /* rotate: keep only the header lines (cells re-read from the start) */
    char *buf = malloc((size_t)st.st_size + 1), *out = malloc((size_t)st.st_size + 1);
    if (buf && out && pread(fd, buf, (size_t)st.st_size, 0) == st.st_size) {
      buf[st.st_size] = 0;
      size_t o = 0;
      for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n"))
        if (!strncmp(l, "RFRING ", 7)) o += (size_t)snprintf(out + o, (size_t)st.st_size + 1 - o, "%s\n", l);
      if (ftruncate(fd, 0) == 0 && o && pwrite(fd, out, o, 0) < 0) { /* best effort */ }
    }
    free(buf); free(out);
  }
  char l[512];
  const int n = snprintf(l, sizeof(l), "%s\n", line);
  if (n > 0 && write(fd, l, (size_t)(n < (int)sizeof(l) ? n : (int)sizeof(l) - 1)) < 0) { /* best effort */ }
  close(fd);
}

size_t shm_iq_radio_poll(const char *ring, uint64_t *off, char *buf, size_t cap)
{
  char p[160];
  radio_path(p, sizeof(p), ring);
  const int fd = open(p, O_RDONLY);
  if (fd < 0 || cap < 2) { if (fd >= 0) close(fd); return 0; }
  struct stat st;
  if (fstat(fd, &st) != 0) { close(fd); return 0; }
  if ((uint64_t)st.st_size < *off) *off = 0; /* rotated or recreated */
  const ssize_t k = pread(fd, buf, cap - 1, (off_t)*off);
  close(fd);
  if (k <= 0) { buf[0] = 0; return 0; }
  ssize_t e = k;
  while (e > 0 && buf[e - 1] != '\n') --e; /* complete lines only */
  buf[e] = 0;
  *off += (uint64_t)e;
  return (size_t)e;
}

int shm_iq_radio_fmt_chpow(char *out, size_t cap, uint64_t sample, const int16_t *const *ch, unsigned nch, int n)
{
  int o = snprintf(out, cap, "SENSING: CHPOW sample=%llu dBFS", (unsigned long long)sample);
  for (unsigned c = 0; c < nch && o > 0 && (size_t)o < cap; ++c) {
    double acc = 0;
    for (int k = 0; k < n; ++k) { const double re = ch[c][2 * k], im = ch[c][2 * k + 1]; acc += re * re + im * im; }
    const double pw = n ? acc / n / (32768.0 * 32768.0) : 0;
    o += snprintf(out + o, cap - (size_t)o, " ch%u=%.2f", c, pw > 0 ? 10 * log10(pw) : -200.0);
  }
  return o;
}
