/* Per-unit cost accounting (plan Task S5, spec section 5.2). See nr_sense_unit.h.
 *
 * Table: per cell, NR_UNIT_ROWS RNTI rows x NR_UNIT_SRC_N sources. Each unit keeps a ring of SENSE_WIN_BUCKETS buckets of SENSE_BUCKET_NS, two 64-bit words each:
 *   A = tag(24) | cpu_ns(40)                      B = tag(24) | wait_max_us(20) | messages(10) | wideband_messages(10)
 * tag = (t / bucket) mod 2^24 names the bucket a slot currently holds; a writer that finds an older tag replaces the word (CAS), one that finds the same tag
 * adds. Both words are updated by CAS loops, so any number of threads write one unit without a lock and without losing a sample; a write that arrives for a
 * bucket older than the one the slot already holds (a late thread) is dropped. The snapshot reads racily: each bucket is read as one word, consistent per word.
 *
 * Row lookup is a scan of 64 keys (rnti+1; 0 = free). Creating a row (rare: a new RNTI) takes a per-cell try-lock; if the lock is busy that sample is dropped,
 * so no thread ever waits. The victim is the row with the oldest last message. A writer that located a row just before its eviction can add one sample to the
 * RNTI that replaces it: bounded to the instant of the eviction, memory-safe (all fields are atomics), and statistically irrelevant to a 16 s window. */
#include "nr_sense_unit.h"
#include "nr_sense_sched.h"
#include "common/utils/nr_cell.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define NB ((int)SENSE_WIN_BUCKETS)
#define TAG_MASK 0xFFFFFFull
#define CPU_MASK ((1ull << 40) - 1)
#define WAIT_MAX 0xFFFFFu
#define CNT_MAX 1023u

typedef struct {
  _Atomic uint64_t a, b;
} ubucket_t;
typedef struct {
  ubucket_t bk[NB];
  _Atomic uint64_t last_msg_ns, last_valid_wb_ns;
  _Atomic uint32_t late, viol, skips, live, disabled, est_last, n_cfr, valid_bits, snr_bits;
} unit_t;
typedef struct {
  _Atomic uint32_t key[NR_UNIT_ROWS];
  _Atomic uint64_t row_last[NR_UNIT_ROWS];
  atomic_flag create_lock;
  unit_t u[NR_UNIT_ROWS][NR_UNIT_SRC_N];
} unit_tab_t;
NR_CELL_STATIC(unit_tab_t, unit_tab)

static inline unit_tab_t *tab_of(int cell) { return &unit_tab_cells[cell]; }

static inline uint32_t tag_of(uint64_t b) { return (uint32_t)(b & TAG_MASK); }
/* signed distance a - b in the 24-bit tag space */
static inline int32_t tag_diff(uint32_t a, uint32_t b) { return (int32_t)((a - b) << 8) >> 8; }

static void reset_unit(unit_t *u)
{
  for (int i = 0; i < NB; i++) {
    atomic_store_explicit(&u->bk[i].a, 0, memory_order_relaxed);
    atomic_store_explicit(&u->bk[i].b, 0, memory_order_relaxed);
  }
  atomic_store_explicit(&u->last_msg_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&u->last_valid_wb_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&u->late, 0, memory_order_relaxed);
  atomic_store_explicit(&u->viol, 0, memory_order_relaxed);
  atomic_store_explicit(&u->skips, 0, memory_order_relaxed);
  atomic_store_explicit(&u->live, 0, memory_order_relaxed);
  atomic_store_explicit(&u->disabled, 0, memory_order_relaxed);
  atomic_store_explicit(&u->est_last, 0, memory_order_relaxed);
  atomic_store_explicit(&u->n_cfr, 0, memory_order_relaxed);
  atomic_store_explicit(&u->valid_bits, 0, memory_order_relaxed);
  atomic_store_explicit(&u->snr_bits, 0, memory_order_relaxed);
}

static int row_find(unit_tab_t *T, uint16_t rnti)
{
  const uint32_t k = (uint32_t)rnti + 1u;
  for (int i = 0; i < NR_UNIT_ROWS; i++)
    if (atomic_load_explicit(&T->key[i], memory_order_acquire) == k)
      return i;
  return -1;
}

static int row_get(unit_tab_t *T, uint16_t rnti, bool create, uint64_t t_ns)
{
  int r = row_find(T, rnti);
  if (r >= 0 || !create)
    return r;
  if (atomic_flag_test_and_set_explicit(&T->create_lock, memory_order_acquire))
    return -1; /* another thread is creating a row: this one sample is dropped, never waited for */
  r = row_find(T, rnti);                       /* it may have been created while we took the lock */
  if (r < 0) {
    int v = 0;
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < NR_UNIT_ROWS; i++) {
      if (atomic_load_explicit(&T->key[i], memory_order_relaxed) == 0) { v = i; break; }
      const uint64_t l = atomic_load_explicit(&T->row_last[i], memory_order_relaxed);
      if (l < best) { best = l; v = i; }
    }
    atomic_store_explicit(&T->key[v], 0, memory_order_release); /* readers stop finding the old RNTI before its history is cleared */
    for (int s = 0; s < NR_UNIT_SRC_N; s++) reset_unit(&T->u[v][s]);
    atomic_store_explicit(&T->row_last[v], t_ns, memory_order_relaxed);
    atomic_store_explicit(&T->key[v], (uint32_t)rnti + 1u, memory_order_release);
    r = v;
  }
  atomic_flag_clear_explicit(&T->create_lock, memory_order_release);
  return r;
}

static inline void touch(unit_tab_t *T, int r, unit_t *u, uint64_t t_ns)
{
  atomic_store_explicit(&u->live, 1, memory_order_relaxed);
  uint64_t o = atomic_load_explicit(&u->last_msg_ns, memory_order_relaxed);
  while (o < t_ns && !atomic_compare_exchange_weak_explicit(&u->last_msg_ns, &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
  o = atomic_load_explicit(&T->row_last[r], memory_order_relaxed);
  while (o < t_ns && !atomic_compare_exchange_weak_explicit(&T->row_last[r], &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
}

static inline void bucket_add(unit_t *u, uint64_t t_ns, uint64_t cpu, uint64_t wait_ns, unsigned msgs, unsigned wb)
{
  const uint64_t bidx = t_ns / SENSE_BUCKET_NS;
  ubucket_t *bk = &u->bk[bidx % NB];
  const uint32_t tag = tag_of(bidx);
  if (cpu) {
    if (cpu > CPU_MASK) cpu = CPU_MASK;
    uint64_t w = atomic_load_explicit(&bk->a, memory_order_relaxed), n;
    do {
      const int32_t d = tag_diff(tag, (uint32_t)(w >> 40));
      if (w && d < 0 && d > -NB) goto word_b; /* a late thread: the slot already holds a newer bucket */
      uint64_t c = (w && d == 0) ? (w & CPU_MASK) + cpu : cpu;
      if (c > CPU_MASK) c = CPU_MASK;
      n = ((uint64_t)tag << 40) | c;
    } while (!atomic_compare_exchange_weak_explicit(&bk->a, &w, n, memory_order_relaxed, memory_order_relaxed));
  }
word_b:
  if (wait_ns || msgs) {
    uint64_t wu = wait_ns / 1000;
    if (wu > WAIT_MAX) wu = WAIT_MAX;
    uint64_t w = atomic_load_explicit(&bk->b, memory_order_relaxed), n;
    do {
      const int32_t d = tag_diff(tag, (uint32_t)(w >> 40));
      if (w && d < 0 && d > -NB) return;
      uint64_t wm = wu, m = msgs, q = wb;
      if (w && d == 0) {
        const uint64_t ow = (w >> 20) & WAIT_MAX, om = (w >> 10) & CNT_MAX, oq = w & CNT_MAX;
        if (ow > wm) wm = ow;
        m += om;
        q += oq;
      }
      if (m > CNT_MAX) m = CNT_MAX;
      if (q > CNT_MAX) q = CNT_MAX;
      n = ((uint64_t)tag << 40) | (wm << 20) | (m << 10) | q;
    } while (!atomic_compare_exchange_weak_explicit(&bk->b, &w, n, memory_order_relaxed, memory_order_relaxed));
  }
}

static inline unit_t *unit_get(unit_tab_t *T, nr_unit_src_t s, uint16_t rnti, uint64_t t_ns, int *row)
{
  if ((unsigned)s >= NR_UNIT_SRC_N || nr_isac_mode() != NR_ISAC_MODE_SENSING)
    return NULL;
  const int r = row_get(T, rnti, true, t_ns);
  if (r < 0)
    return NULL;
  *row = r;
  return &T->u[r][s];
}

static inline uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void nr_sense_unit_job(nr_unit_src_t s, uint16_t rnti, uint64_t cpu_ns, uint64_t wait_ns, bool late, uint16_t est_usable_prbs, uint64_t t_ns)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  int r;
  unit_t *u = unit_get(T, s, rnti, t_ns, &r);
  if (!u)
    return;
  touch(T, r, u, t_ns);
  if (late)
    atomic_fetch_add_explicit(&u->late, 1, memory_order_relaxed);
  if (est_usable_prbs)
    atomic_store_explicit(&u->est_last, est_usable_prbs, memory_order_relaxed);
  bucket_add(u, t_ns, cpu_ns, wait_ns, est_usable_prbs ? 1 : 0, est_usable_prbs >= (unsigned)nr_isac_sense_min_prb() ? 1 : 0);
}

static inline float bits_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t f_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
#define EWMA_A 0.25f /* a record weighs 1/4: about the last 4-8 records of the unit */

void nr_sense_unit_cfr(nr_unit_src_t s, uint16_t rnti, uint16_t valid_prbs, float snr_db, uint64_t t_ns)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  int r;
  unit_t *u = unit_get(T, s, rnti, t_ns, &r);
  if (!u)
    return;
  touch(T, r, u, t_ns);
  uint32_t den = atomic_load_explicit(&u->est_last, memory_order_relaxed);
  if (den < valid_prbs) den = valid_prbs; /* the estimate of usable PRBs is a floor of the valid count */
  const float ratio = den ? (float)valid_prbs / (float)den : 0.f;
  const float snr = snr_db == snr_db ? snr_db : 0.f;
  /* single writer per record in practice (the sense writer); a lost update between two writers only skews one EWMA step */
  if (atomic_fetch_add_explicit(&u->n_cfr, 1, memory_order_relaxed) == 0) {
    atomic_store_explicit(&u->valid_bits, f_bits(ratio), memory_order_relaxed);
    atomic_store_explicit(&u->snr_bits, f_bits(snr), memory_order_relaxed);
  } else {
    const float v = bits_f(atomic_load_explicit(&u->valid_bits, memory_order_relaxed));
    const float q = bits_f(atomic_load_explicit(&u->snr_bits, memory_order_relaxed));
    atomic_store_explicit(&u->valid_bits, f_bits(v + EWMA_A * (ratio - v)), memory_order_relaxed);
    if (snr_db == snr_db)
      atomic_store_explicit(&u->snr_bits, f_bits(q + EWMA_A * (snr - q)), memory_order_relaxed);
  }
  if (valid_prbs >= (unsigned)nr_isac_sense_min_prb()) {
    uint64_t o = atomic_load_explicit(&u->last_valid_wb_ns, memory_order_relaxed);
    while (o < t_ns && !atomic_compare_exchange_weak_explicit(&u->last_valid_wb_ns, &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
  }
}

void nr_sense_unit_violation(nr_unit_src_t s, uint16_t rnti)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  int r;
  const uint64_t t = now_ns();
  unit_t *u = unit_get(T, s, rnti, t, &r);
  if (!u)
    return;
  touch(T, r, u, t);
  atomic_fetch_add_explicit(&u->viol, 1, memory_order_relaxed);
}

void nr_sense_unit_skip(nr_unit_src_t s, uint16_t rnti)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  int r;
  const uint64_t t = now_ns();
  unit_t *u = unit_get(T, s, rnti, t, &r);
  if (!u)
    return;
  touch(T, r, u, t);
  atomic_fetch_add_explicit(&u->skips, 1, memory_order_relaxed);
  /* a gated message is still a message of the unit: its rate is what lets the scheduler value it while it is off */
  bucket_add(u, t, 0, 0, 1, atomic_load_explicit(&u->est_last, memory_order_relaxed) >= (unsigned)nr_isac_sense_min_prb() ? 1 : 0);
}

void nr_sense_unit_set_enabled(int cell, uint16_t rnti, nr_unit_src_t s, bool enabled)
{
  if (cell < 0 || cell >= NR_CELL_MAX || (unsigned)s >= NR_UNIT_SRC_N)
    return;
  unit_tab_t *T = tab_of(cell);
  const int r = row_get(T, rnti, true, now_ns());
  if (r < 0)
    return;
  unit_t *u = &T->u[r][s];
  atomic_store_explicit(&u->live, 1, memory_order_relaxed);
  atomic_store_explicit(&u->disabled, enabled ? 0 : 1, memory_order_relaxed);
}

static int cmp_u64(const void *a, const void *b)
{
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}
/* nearest rank: the ceil(pct/100 * n)-th smallest of n sorted values */
static uint64_t nrank(const uint64_t *sorted, int n, int pct)
{
  if (n <= 0)
    return 0;
  int rank = (pct * n + 99) / 100;
  if (rank < 1) rank = 1;
  return sorted[rank - 1];
}

int nr_sense_unit_snapshot_at(int cell, uint64_t now, nr_sense_unit_view_t *out, int max)
{
  if (cell < 0 || cell >= NR_CELL_MAX || !out || max <= 0)
    return 0;
  unit_tab_t *T = tab_of(cell);
  const uint64_t bn = now / SENSE_BUCKET_NS; /* the window is the NB completed buckets before the one holding `now` */
  int n = 0;
  for (int r = 0; r < NR_UNIT_ROWS && n < max; r++) {
    const uint32_t key = atomic_load_explicit(&T->key[r], memory_order_acquire);
    if (key == 0 || key == 0xFFFFFFFFu)
      continue;
    for (int s = 0; s < NR_UNIT_SRC_N && n < max; s++) {
      unit_t *u = &T->u[r][s];
      if (!atomic_load_explicit(&u->live, memory_order_relaxed))
        continue;
      uint64_t cpu[NB], wt[NB];
      int nw = 0;
      uint64_t msgs = 0, wbs = 0;
      for (int i = 0; i < NB; i++) {
        const uint64_t b = bn - NB + (uint64_t)i;
        const ubucket_t *bk = &u->bk[b % NB];
        const uint64_t a = atomic_load_explicit(&bk->a, memory_order_relaxed), w = atomic_load_explicit(&bk->b, memory_order_relaxed);
        cpu[i] = (a >> 40) == tag_of(b) ? (a & CPU_MASK) : 0;
        if ((w >> 40) == tag_of(b)) {
          msgs += (w >> 10) & CNT_MAX;
          wbs += w & CNT_MAX;
          const uint64_t wm = (w >> 20) & WAIT_MAX;
          if (wm || ((w >> 10) & CNT_MAX)) wt[nw++] = wm * 1000ull;
        }
      }
      qsort(cpu, NB, sizeof cpu[0], cmp_u64);
      qsort(wt, (size_t)nw, sizeof wt[0], cmp_u64);
      const double win_s = (double)NB * (double)SENSE_BUCKET_NS * 1e-9;
      nr_sense_unit_view_t *v = &out[n++];
      memset(v, 0, sizeof *v);
      v->rnti = (uint16_t)(key - 1u);
      v->src = (nr_unit_src_t)s;
      v->enabled = !atomic_load_explicit(&u->disabled, memory_order_relaxed);
      v->cpu_p95_ns = (float)nrank(cpu, NB, 95);
      v->cpu_p99_ns = (float)nrank(cpu, NB, 99);
      v->wait_p99_ns = (float)nrank(wt, nw, 99);
      v->late = atomic_load_explicit(&u->late, memory_order_relaxed);
      v->violations = atomic_load_explicit(&u->viol, memory_order_relaxed);
      v->skips = atomic_load_explicit(&u->skips, memory_order_relaxed);
      v->rate_hz = (float)((double)msgs / win_s);
      v->wb_rate_hz = (float)((double)wbs / win_s);
      v->valid_ratio = atomic_load_explicit(&u->n_cfr, memory_order_relaxed) ? bits_f(atomic_load_explicit(&u->valid_bits, memory_order_relaxed)) : 0.f;
      v->snr_db = atomic_load_explicit(&u->n_cfr, memory_order_relaxed) ? bits_f(atomic_load_explicit(&u->snr_bits, memory_order_relaxed)) : 0.f;
      v->last_valid_wb_ns = atomic_load_explicit(&u->last_valid_wb_ns, memory_order_relaxed);
      v->last_msg_ns = atomic_load_explicit(&u->last_msg_ns, memory_order_relaxed);
    }
  }
  return n;
}

int nr_sense_unit_snapshot(int cell, nr_sense_unit_view_t *out, int max) { return nr_sense_unit_snapshot_at(cell, now_ns(), out, max); }

uint64_t nr_sense_unit_bucket_cpu(int cell, uint16_t rnti, nr_unit_src_t s, uint64_t t_ns)
{
  if (cell < 0 || cell >= NR_CELL_MAX || (unsigned)s >= NR_UNIT_SRC_N)
    return 0;
  unit_tab_t *T = tab_of(cell);
  const int r = row_find(T, rnti);
  if (r < 0)
    return 0;
  const uint64_t b = t_ns / SENSE_BUCKET_NS;
  const uint64_t a = atomic_load_explicit(&T->u[r][s].bk[b % NB].a, memory_order_relaxed);
  return (a >> 40) == tag_of(b) ? (a & CPU_MASK) : 0;
}

void nr_sense_unit_reset(int cell)
{
  if (cell < 0 || cell >= NR_CELL_MAX)
    return;
  unit_tab_t *T = tab_of(cell);
  for (int r = 0; r < NR_UNIT_ROWS; r++) {
    atomic_store_explicit(&T->key[r], 0, memory_order_release);
    atomic_store_explicit(&T->row_last[r], 0, memory_order_relaxed);
    for (int s = 0; s < NR_UNIT_SRC_N; s++) reset_unit(&T->u[r][s]);
  }
}
