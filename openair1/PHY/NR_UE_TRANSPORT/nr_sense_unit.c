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
#define NBS (NB + 1) /* ring slots: the bucket being written never aliases the oldest bucket of the window (M-3) */
#define TAG_MASK 0xFFFFFFull
#define CPU_MASK ((1ull << 40) - 1)
#define WAIT_MAX 0xFFFFFu
#define CNT_MAX 1023u

typedef struct {
  _Atomic uint64_t a, b;
} ubucket_t;
#define CFR_EV_N 4096
typedef struct {
  ubucket_t bk[NBS];
  _Atomic uint64_t rg[NBS], dq[NBS]; /* writer (ring) cost and daBg cost per bucket: tag(24) | ns(40), as word A of bk */
  _Atomic uint64_t last_msg_ns, last_valid_wb_ns, last_cfr_ns, first_ns;
  _Atomic uint32_t late, viol, skips, live, disabled, est_last, n_cfr, valid_bits, snr_bits, pend_w;
} unit_t;
typedef struct {
  _Atomic uint32_t key[NR_UNIT_ROWS];
  _Atomic uint64_t row_last[NR_UNIT_ROWS];
  _Atomic uint32_t gen[NR_UNIT_ROWS];  /* bumped when the row is evicted (I-5) */
  _Atomic uint64_t unattributed_ns;    /* cost-only calls of an RNTI without a row (I-1) and the PDSCH decode stage (protected baseline) */
  _Atomic uint64_t dec_wait_max;       /* longest decode-queue wait since the last take (ns) */
  _Atomic uint64_t viol_total, skip_total, wb_valid, wb_valid_prbs, last_wb_ns, gap_max_ns; /* cell totals for the metrics (S7) */
  _Atomic uint64_t ev[CFR_EV_N];       /* S9: the cell's latest DCI arrivals, (t_ns / 1000) << 16 | grant PRBs; 0 = empty */
  _Atomic uint64_t ev_head;            /* records written so far (the next slot is ev_head % CFR_EV_N) */
  _Atomic uint64_t wbg_offered, wbg_admitted, wbg_taps, wbg_cost_ns, wbg_rec, wbg_ring_ns, wbg_refunded, wbg_overcap; /* grant-level wideband path (nr_sense_wbg_*) */
  _Atomic uint64_t wbg_tok;            /* token bucket: t_us(32, wrapping) << 32 | milli-tokens (signed 32); 0 = never used (full) */
  _Atomic uint32_t wbg_rate_m1;        /* allowed grants/s in milli-Hz + 1; 0 = the startup rate (no decision yet / cell reset) */
  _Atomic uint32_t wbg_widest;         /* widest allocation admitted since the last decision */
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
  for (int i = 0; i < NBS; i++) {
    atomic_store_explicit(&u->rg[i], 0, memory_order_relaxed);
    atomic_store_explicit(&u->dq[i], 0, memory_order_relaxed);
    atomic_store_explicit(&u->bk[i].a, 0, memory_order_relaxed);
    atomic_store_explicit(&u->bk[i].b, 0, memory_order_relaxed);
  }
  atomic_store_explicit(&u->last_msg_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&u->last_valid_wb_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&u->last_cfr_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&u->first_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&u->late, 0, memory_order_relaxed);
  atomic_store_explicit(&u->viol, 0, memory_order_relaxed);
  atomic_store_explicit(&u->skips, 0, memory_order_relaxed);
  atomic_store_explicit(&u->live, 0, memory_order_relaxed);
  atomic_store_explicit(&u->disabled, 0, memory_order_relaxed);
  atomic_store_explicit(&u->est_last, 0, memory_order_relaxed);
  atomic_store_explicit(&u->n_cfr, 0, memory_order_relaxed);
  atomic_store_explicit(&u->valid_bits, 0, memory_order_relaxed);
  atomic_store_explicit(&u->snr_bits, 0, memory_order_relaxed);
  atomic_store_explicit(&u->pend_w, 0, memory_order_relaxed);
}

/* g0 (optional) receives the row generation read BEFORE the key: an eviction clears the key, then bumps the generation, so a writer that saw the key
 * under generation g0 detects any later eviction by a changed generation (I-5). */
static int row_find(unit_tab_t *T, uint16_t rnti, uint32_t *g0)
{
  const uint32_t k = (uint32_t)rnti + 1u;
  for (int i = 0; i < NR_UNIT_ROWS; i++) {
    const uint32_t g = atomic_load_explicit(&T->gen[i], memory_order_acquire);
    if (atomic_load_explicit(&T->key[i], memory_order_acquire) == k) {
      if (g0) *g0 = g;
      return i;
    }
  }
  return -1;
}

static int row_get(unit_tab_t *T, uint16_t rnti, bool create, uint64_t t_ns, uint32_t *g0)
{
  int r = row_find(T, rnti, g0);
  if (r >= 0 || !create)
    return r;
  if (atomic_flag_test_and_set_explicit(&T->create_lock, memory_order_acquire))
    return -1; /* another thread is creating a row: this one sample is dropped, never waited for */
  r = row_find(T, rnti, g0);                   /* it may have been created while we took the lock */
  if (r < 0) {
    int v = 0;
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < NR_UNIT_ROWS; i++) {
      if (atomic_load_explicit(&T->key[i], memory_order_relaxed) == 0) { v = i; break; }
      const uint64_t l = atomic_load_explicit(&T->row_last[i], memory_order_relaxed);
      if (l < best) { best = l; v = i; }
    }
    atomic_store_explicit(&T->key[v], 0, memory_order_release); /* readers stop finding the old RNTI before its history is cleared */
    atomic_fetch_add_explicit(&T->gen[v], 1, memory_order_acq_rel); /* writers that located the old RNTI drop their update */
    for (int s = 0; s < NR_UNIT_SRC_N; s++) {
      reset_unit(&T->u[v][s]);
      atomic_store_explicit(&T->u[v][s].disabled, 1, memory_order_relaxed); /* a new RNTI starts disabled: the scheduler's cold-unit path enables it */
    }
    atomic_store_explicit(&T->row_last[v], t_ns, memory_order_relaxed);
    if (g0) *g0 = atomic_load_explicit(&T->gen[v], memory_order_relaxed);
    atomic_store_explicit(&T->key[v], (uint32_t)rnti + 1u, memory_order_release);
    r = v;
  }
  atomic_flag_clear_explicit(&T->create_lock, memory_order_release);
  return r;
}

static inline void touch(unit_tab_t *T, int r, unit_t *u, uint64_t t_ns)
{
  atomic_store_explicit(&u->live, 1, memory_order_relaxed);
  uint64_t z = 0;
  atomic_compare_exchange_strong_explicit(&u->first_ns, &z, t_ns, memory_order_relaxed, memory_order_relaxed);
  uint64_t o = atomic_load_explicit(&u->last_msg_ns, memory_order_relaxed);
  while (o < t_ns && !atomic_compare_exchange_weak_explicit(&u->last_msg_ns, &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
  o = atomic_load_explicit(&T->row_last[r], memory_order_relaxed);
  while (o < t_ns && !atomic_compare_exchange_weak_explicit(&T->row_last[r], &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
}

static inline void bucket_add(unit_t *u, uint64_t t_ns, uint64_t cpu, uint64_t wait_ns, unsigned msgs, unsigned wb)
{
  const uint64_t bidx = t_ns / SENSE_BUCKET_NS;
  ubucket_t *bk = &u->bk[bidx % NBS];
  const uint32_t tag = tag_of(bidx);
  if (cpu) {
    if (cpu > CPU_MASK) cpu = CPU_MASK;
    uint64_t w = atomic_load_explicit(&bk->a, memory_order_relaxed), n;
    do {
      const int32_t d = tag_diff(tag, (uint32_t)(w >> 40));
      if (w && d < 0) goto word_b; /* a late thread: the slot already holds a newer bucket */
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
      if (w && d < 0) return;
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

/* add ns to the bucket word of t_ns in a tag|ns stream (same rules as word A of bucket_add: add when the tag matches, replace an older one, drop a late writer) */
static void stream_add(_Atomic uint64_t *arr, uint64_t t_ns, uint64_t ns)
{
  if (!ns) return;
  const uint64_t bidx = t_ns / SENSE_BUCKET_NS;
  _Atomic uint64_t *wd = &arr[bidx % NBS];
  const uint32_t tag = tag_of(bidx);
  if (ns > CPU_MASK) ns = CPU_MASK;
  uint64_t w = atomic_load_explicit(wd, memory_order_relaxed), n;
  do {
    const int32_t d = tag_diff(tag, (uint32_t)(w >> 40));
    if (w && d < 0) return;
    uint64_t c = (w && d == 0) ? (w & CPU_MASK) + ns : ns;
    if (c > CPU_MASK) c = CPU_MASK;
    n = ((uint64_t)tag << 40) | c;
  } while (!atomic_compare_exchange_weak_explicit(wd, &w, n, memory_order_relaxed, memory_order_relaxed));
}

static inline uint64_t now_ns(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Test seam (I-5): called between locating a row and updating it, so a test can evict the row at exactly that point. Unset in production. */
static void (*_Atomic g_seam)(void);
void nr_sense_unit_test_seam(void (*f)(void)) { atomic_store_explicit(&g_seam, f, memory_order_relaxed); }

/* SRS is one unit per cell (the blind search has no RNTI): every SRS call lands on rnti 0 whatever the record's attribution. */
static inline uint16_t unit_rnti(nr_unit_src_t s, uint16_t rnti) { return s == NR_UNIT_SRS ? 0 : rnti; }

/* Locates the unit; create = this call may open a row (wideband-capable call, or the per-cell SRS unit). Returns NULL when the call is not for a live row
 * (outside sensing mode, no row and not creatable, or the row was evicted since it was located: the update would land on the successor RNTI). */
static inline unit_t *unit_get(unit_tab_t *T, nr_unit_src_t s, uint16_t rnti, bool create, uint64_t t_ns, int *row)
{
  if ((unsigned)s >= NR_UNIT_SRC_N || nr_isac_mode() != NR_ISAC_MODE_SENSING)
    return NULL;
  uint32_t g0 = 0;
  const int r = row_get(T, rnti, create || s == NR_UNIT_SRS, t_ns, &g0);
  if (r < 0)
    return NULL;
  void (*f)(void) = atomic_load_explicit(&g_seam, memory_order_relaxed);
  if (f) f();
  if (atomic_load_explicit(&T->gen[r], memory_order_acquire) != g0)
    return NULL;
  *row = r;
  return &T->u[r][s];
}

static inline bool wideband_capable(unsigned prbs) { return prbs >= (unsigned)nr_isac_sense_min_prb(); } /* the wideband metric: unchanged */
/* S9: a unit is opened, booked and gated for every message of >= T_cell PRBs (the calling cell's adaptive floor; sense_min_prb until the first decision), so a continuity grant narrower than
 * sense_min_prb is exported like a wideband one. The scheduler weights the same messages (wb_rate_hz counts them). */
static inline bool cont_capable(unsigned prbs) { return prbs >= (unsigned)nr_sense_cell_floor(nr_cell_idx()); }

void nr_sense_unit_job(nr_unit_src_t s, uint16_t rnti, uint64_t cpu_ns, uint64_t wait_ns, bool late, uint16_t est_usable_prbs, uint64_t t_ns)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  rnti = unit_rnti(s, rnti);
  int r;
  unit_t *u = unit_get(T, s, rnti, cont_capable(est_usable_prbs), t_ns, &r);
  if (!u) {
    /* no row and none to create (narrow or cost-only call): the cost stays in the cell's baseline, not lost */
    if (cpu_ns && nr_isac_mode() == NR_ISAC_MODE_SENSING)
      atomic_fetch_add_explicit(&T->unattributed_ns, cpu_ns, memory_order_relaxed);
    return;
  }
  touch(T, r, u, t_ns);
  if (late)
    atomic_fetch_add_explicit(&u->late, 1, memory_order_relaxed);
  if (est_usable_prbs)
    atomic_store_explicit(&u->est_last, est_usable_prbs, memory_order_relaxed);
  bucket_add(u, t_ns, cpu_ns, wait_ns, est_usable_prbs ? 1 : 0, cont_capable(est_usable_prbs) ? 1 : 0);
}

static inline float bits_f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t f_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
#define EWMA_A 0.25f /* a record weighs 1/4: about the last 4-8 records of the unit */

/* cell totals of a wideband record (any unit, or the grant-level path): count, valid PRBs, longest gap between two records */
static void cell_wb_note(unit_tab_t *T, uint16_t valid_prbs, uint64_t t_ns)
{
  atomic_fetch_add_explicit(&T->wb_valid, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&T->wb_valid_prbs, valid_prbs, memory_order_relaxed);
  uint64_t o = atomic_load_explicit(&T->last_wb_ns, memory_order_relaxed);
  while (o < t_ns && !atomic_compare_exchange_weak_explicit(&T->last_wb_ns, &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
  if (o && t_ns > o) { /* o = the previous record (the CAS reloads it on failure; on success it is what we replaced) */
    uint64_t g = atomic_load_explicit(&T->gap_max_ns, memory_order_relaxed);
    while (g < t_ns - o && !atomic_compare_exchange_weak_explicit(&T->gap_max_ns, &g, t_ns - o, memory_order_relaxed, memory_order_relaxed)) {}
  }
}

void nr_sense_unit_cfr(nr_unit_src_t s, uint16_t rnti, uint16_t valid_prbs, float snr_db, uint64_t t_ns)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  rnti = unit_rnti(s, rnti);
  int r;
  unit_t *u = unit_get(T, s, rnti, cont_capable(valid_prbs), t_ns, &r);
  if (!u)
    return;
  touch(T, r, u, t_ns);
  const uint32_t est = atomic_load_explicit(&u->est_last, memory_order_relaxed);
  /* ratio of real counts: valid / usable PRBs when the unit reports usable PRBs; otherwise (SRS) the record is 1 when it is wideband-valid, else 0 */
  float ratio;
  if (est) ratio = valid_prbs >= est ? 1.f : (float)valid_prbs / (float)est;
  else ratio = cont_capable(valid_prbs) ? 1.f : 0.f;
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
  {
    uint64_t o = atomic_load_explicit(&u->last_cfr_ns, memory_order_relaxed);
    while (o < t_ns && !atomic_compare_exchange_weak_explicit(&u->last_cfr_ns, &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
  }
  if (s == NR_UNIT_SRS) /* SRS has no daBg/queue job to count the message: the exported record is the message */
    bucket_add(u, t_ns, 0, 0, 1, cont_capable(valid_prbs) ? 1 : 0);
  if (wideband_capable(valid_prbs)) {
    cell_wb_note(T, valid_prbs, t_ns);
    uint64_t o = atomic_load_explicit(&u->last_valid_wb_ns, memory_order_relaxed);
    while (o < t_ns && !atomic_compare_exchange_weak_explicit(&u->last_valid_wb_ns, &o, t_ns, memory_order_relaxed, memory_order_relaxed)) {}
  }
}

/* ---- grant-level wideband path (spec 2026-11-06-wideband-grant-cfr-design.md) ----
 * Per cell, lock-free. A token bucket refilled at the rate the scheduler allows (nr_sense_wbg_set_rate); one tap per decision interval until its first decision. */
#define WBG_STARTUP_M (1000ull * 1000000000ull / SENSE_DECIDE_NS) /* milli-tokens/s: one tap per decision interval */
#define WBG_BURST_M 4000   /* bucket depth, milli-tokens (4 grants) */
#define WBG_DEBT_M (-2000) /* a grant wider than any admitted since the last decision may borrow down to here */
bool nr_sense_wbg_admit(uint16_t prbs, uint64_t t_ns)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING) return false;
  const int cell = nr_cell_idx();
  if (prbs < (unsigned)nr_isac_sense_min_prb()) return false; /* the wideband target, not the continuity floor T_cell */
  if ((unsigned)cell >= NR_CELL_MAX) return false;
  unit_tab_t *T = tab_of(cell);
  if ((unsigned)cell >= NR_SENSE_CELLS) { atomic_fetch_add_explicit(&T->wbg_overcap, 1, memory_order_relaxed); return false; } /* the scheduler decides for no such cell: no rate, so no tap; counted, the rt thread logs it once */
  atomic_fetch_add_explicit(&T->wbg_offered, 1, memory_order_relaxed);
  const uint32_t rm = atomic_load_explicit(&T->wbg_rate_m1, memory_order_relaxed);
  bool ok = false;
  {
    const uint32_t now_us = (uint32_t)(t_ns / 1000); /* wraps every 71 min; a modular difference below 2^31 is the elapsed time */
    const int64_t rate = rm ? (int64_t)rm - 1 : (int64_t)WBG_STARTUP_M; /* milli-tokens per second */
    uint64_t o = atomic_load_explicit(&T->wbg_tok, memory_order_relaxed), n;
    do {
      uint32_t t_us = (uint32_t)(o >> 32);
      int64_t tk = (int32_t)(uint32_t)o;
      if (!o) { t_us = now_us; tk = WBG_BURST_M; } /* never used: full */
      const uint32_t dt = now_us - t_us;
      if (dt > 0 && dt < 0x80000000u) { tk += rate * (int64_t)dt / 1000000; if (tk > WBG_BURST_M) tk = WBG_BURST_M; t_us = now_us; }
      const uint32_t wd = atomic_load_explicit(&T->wbg_widest, memory_order_relaxed);
      ok = tk >= 1000 || (prbs > wd && tk > WBG_DEBT_M); /* widest first: a wider grant than any this window may borrow */
      if (ok) tk -= 1000;
      n = ((uint64_t)t_us << 32) | (uint32_t)(int32_t)tk;
      if (!n) n = 1; /* 0 means "never used" */
    } while (!atomic_compare_exchange_weak_explicit(&T->wbg_tok, &o, n, memory_order_relaxed, memory_order_relaxed));
  }
  if (ok) {
    atomic_fetch_add_explicit(&T->wbg_admitted, 1, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&T->wbg_widest, memory_order_relaxed);
    while (w < prbs && !atomic_compare_exchange_weak_explicit(&T->wbg_widest, &w, prbs, memory_order_relaxed, memory_order_relaxed)) {}
  }
  return ok;
}
void nr_sense_wbg_cost(uint64_t cpu_ns)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING) return;
  unit_tab_t *T = tab_of(nr_cell_idx());
  atomic_fetch_add_explicit(&T->wbg_taps, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&T->wbg_cost_ns, cpu_ns, memory_order_relaxed);
}
void nr_sense_wbg_refund(void)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING) return;
  const int cell = nr_cell_idx();
  if ((unsigned)cell >= NR_SENSE_CELLS) return;
  unit_tab_t *T = tab_of(cell);
  atomic_fetch_add_explicit(&T->wbg_refunded, 1, memory_order_relaxed);
  uint64_t o = atomic_load_explicit(&T->wbg_tok, memory_order_relaxed), n;
  do {
    if (!o) return; /* never used: the bucket is full */
    int64_t tk = (int32_t)(uint32_t)o + 1000;
    if (tk > WBG_BURST_M) tk = WBG_BURST_M;
    n = (o & 0xFFFFFFFF00000000ull) | (uint32_t)(int32_t)tk;
  } while (!atomic_compare_exchange_weak_explicit(&T->wbg_tok, &o, n, memory_order_relaxed, memory_order_relaxed));
}
void nr_sense_wbg_wasted(uint64_t cpu_ns)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING) return;
  atomic_fetch_add_explicit(&tab_of(nr_cell_idx())->wbg_cost_ns, cpu_ns, memory_order_relaxed);
}
void nr_sense_wbg_ring_cost(uint64_t ns)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING) return;
  atomic_fetch_add_explicit(&tab_of(nr_cell_idx())->wbg_ring_ns, ns, memory_order_relaxed);
}
int nr_sense_wbg_after_gate(bool unit_open, bool site_ok, uint16_t prbs, uint64_t t_ns)
{
  if (unit_open) return 0;
  return site_ok && nr_sense_wbg_admit(prbs, t_ns) ? 1 : -1;
}
void nr_sense_wbg_cfr(uint16_t valid_prbs, uint64_t t_ns)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  atomic_fetch_add_explicit(&T->wbg_rec, 1, memory_order_relaxed);
  if (wideband_capable(valid_prbs)) cell_wb_note(T, valid_prbs, t_ns);
}
void nr_sense_wbg_set_rate(int cell, float rate_hz)
{
  if (cell < 0 || cell >= NR_CELL_MAX) return;
  unit_tab_t *T = tab_of(cell);
  atomic_store_explicit(&T->wbg_widest, 0, memory_order_relaxed);
  atomic_store_explicit(&T->wbg_rate_m1, !(rate_hz >= 0.f) ? 0u : (uint32_t)(rate_hz * 1000.f > 4e9f ? 4e9f : rate_hz * 1000.f) + 1u, memory_order_relaxed);
}
void nr_sense_wbg_stats(int cell, nr_sense_wbg_stats_t *out)
{
  memset(out, 0, sizeof *out);
  if (cell < 0 || cell >= NR_CELL_MAX) return;
  const unit_tab_t *T = tab_of(cell);
  out->offered = atomic_load_explicit(&T->wbg_offered, memory_order_relaxed);
  out->admitted = atomic_load_explicit(&T->wbg_admitted, memory_order_relaxed);
  out->taps = atomic_load_explicit(&T->wbg_taps, memory_order_relaxed);
  out->cost_ns = atomic_load_explicit(&T->wbg_cost_ns, memory_order_relaxed);
  out->records = atomic_load_explicit(&T->wbg_rec, memory_order_relaxed);
  out->ring_ns = atomic_load_explicit(&T->wbg_ring_ns, memory_order_relaxed);
  out->refunded = atomic_load_explicit(&T->wbg_refunded, memory_order_relaxed);
  out->overcap = atomic_load_explicit(&T->wbg_overcap, memory_order_relaxed);
  const uint32_t rm = atomic_load_explicit(&T->wbg_rate_m1, memory_order_relaxed);
  out->rate_hz = (rm ? (float)(rm - 1) : (float)WBG_STARTUP_M) * 1e-3f;
}

static void cost_stream(nr_unit_src_t s, uint16_t rnti, uint64_t ns, uint64_t t_ns, bool ring)
{
  if ((unsigned)s >= NR_UNIT_SRC_N || nr_isac_mode() != NR_ISAC_MODE_SENSING || !ns) return;
  unit_tab_t *T = tab_of(nr_cell_idx());
  int r;
  unit_t *u = unit_get(T, s, unit_rnti(s, rnti), false, t_ns, &r); /* never opens a row: the cost of a unit without history is not attributed */
  if (u) stream_add(ring ? u->rg : u->dq, t_ns, ns);
}
void nr_sense_unit_cost_ring(nr_unit_src_t s, uint16_t rnti, uint64_t ns, uint64_t t_ns) { cost_stream(s, rnti, ns, t_ns, true); }
void nr_sense_unit_cost_da(nr_unit_src_t s, uint16_t rnti, uint64_t ns, uint64_t t_ns) { cost_stream(s, rnti, ns, t_ns, false); }

void nr_sense_unit_violation(nr_unit_src_t s, uint16_t rnti)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING)
    return; /* legacy and traffic pay no clock read on a drop */
  unit_tab_t *T = tab_of(nr_cell_idx());
  rnti = unit_rnti(s, rnti);
  int r;
  const uint64_t t = now_ns();
  unit_t *u = unit_get(T, s, rnti, false, t, &r); /* a drop never opens a row */
  if (!u)
    return;
  if (atomic_load_explicit(&u->disabled, memory_order_relaxed))
    return; /* a drop in a disabled unit is intentional shedding, not a violation */
  touch(T, r, u, t);
  atomic_fetch_add_explicit(&u->viol, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&T->viol_total, 1, memory_order_relaxed);
}

void nr_sense_unit_skip(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs, uint64_t t_ns) { nr_sense_unit_skip_w(s, rnti, est_usable_prbs, t_ns, 1); }

void nr_sense_unit_skip_w(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs, uint64_t t_ns, unsigned weight)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  rnti = unit_rnti(s, rnti);
  int r;
  unit_t *u = unit_get(T, s, rnti, cont_capable(est_usable_prbs), t_ns, &r);
  if (!u)
    return;
  touch(T, r, u, t_ns);
  atomic_fetch_add_explicit(&u->skips, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&T->skip_total, 1, memory_order_relaxed);
  /* a gated message is still a message of the unit: its (wideband) rate is what lets the scheduler value the unit while it is off */
  if (est_usable_prbs)
    atomic_store_explicit(&u->est_last, est_usable_prbs, memory_order_relaxed);
  if (weight < 1) weight = 1;
  bucket_add(u, t_ns, 0, 0, est_usable_prbs ? weight : 0, cont_capable(est_usable_prbs) ? weight : 0);
}

void nr_sense_unit_set_enabled(int cell, uint16_t rnti, nr_unit_src_t s, bool enabled)
{
  if (cell < 0 || cell >= NR_CELL_MAX || (unsigned)s >= NR_UNIT_SRC_N)
    return;
  unit_tab_t *T = tab_of(cell);
  rnti = unit_rnti(s, rnti);
  uint32_t g0;
  const uint64_t t = now_ns();
  const int r = row_get(T, rnti, true, t, &g0);
  if (r < 0)
    return;
  unit_t *u = &T->u[r][s];
  atomic_store_explicit(&u->live, 1, memory_order_relaxed);
  atomic_store_explicit(&u->disabled, enabled ? 0 : 1, memory_order_relaxed);
  uint64_t z = 0;
  atomic_compare_exchange_strong_explicit(&u->first_ns, &z, t, memory_order_relaxed, memory_order_relaxed);
}

bool nr_sense_unit_enabled(nr_unit_src_t s, uint16_t rnti)
{
  if ((unsigned)s >= NR_UNIT_SRC_N || nr_isac_mode() != NR_ISAC_MODE_SENSING)
    return true;
  unit_tab_t *T = tab_of(nr_cell_idx());
  const uint32_t k = (uint32_t)unit_rnti(s, rnti) + 1u;
  for (int i = 0; i < NR_UNIT_ROWS; i++) /* keys only: the read needs no generation (a row evicted under us is a race of one message) */
    if (atomic_load_explicit(&T->key[i], memory_order_acquire) == k)
      return !atomic_load_explicit(&T->u[i][s].disabled, memory_order_relaxed);
  return false;
}

/* The grants a gated message stands for: the decode policy offers them (nr_sense_unit_offer) when it lets a sampled / checked decode through for a disabled unit, the next skip of
 * that unit takes them (1 when none were offered: every decoded TB then is one grant). So a disabled unit keeps a rate comparable to an enabled one's, whatever the sampling. */
static unsigned take_weight(nr_unit_src_t s, uint16_t rnti)
{
  unit_tab_t *T = tab_of(nr_cell_idx());
  const int r = row_find(T, unit_rnti(s, rnti), NULL);
  if (r < 0) return 1;
  const unsigned w = atomic_exchange_explicit(&T->u[r][s].pend_w, 0, memory_order_relaxed);
  return w ? w : 1;
}
void nr_sense_unit_offer(nr_unit_src_t s, uint16_t rnti, unsigned grants)
{
  if ((unsigned)s >= NR_UNIT_SRC_N || nr_isac_mode() != NR_ISAC_MODE_SENSING || !grants)
    return;
  unit_tab_t *T = tab_of(nr_cell_idx());
  const int r = row_find(T, unit_rnti(s, rnti), NULL); /* never opens a row */
  if (r >= 0 && atomic_load_explicit(&T->u[r][s].pend_w, memory_order_relaxed) < 1023u)
    atomic_fetch_add_explicit(&T->u[r][s].pend_w, grants, memory_order_relaxed);
}

bool nr_sense_unit_gate(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs)
{
  if (nr_sense_unit_enabled(s, rnti))
    return true;
  if (s == NR_UNIT_SRS && !est_usable_prbs)
    est_usable_prbs = (uint16_t)nr_isac_sense_min_prb();
  nr_sense_unit_skip_w(s, rnti, est_usable_prbs, now_ns(), take_weight(s, rnti));
  return false;
}

/* Deferred accounting for a site that holds a long lock (g_phase2_mu): the read-only check runs there, the skip is queued per thread and booked by
 * nr_sense_unit_flush_pending() after the unlock (also when the queue is full: then inline, rarely). */
#define PEND_N 16
static __thread struct { nr_unit_src_t s; uint16_t rnti, est; uint64_t t; } tl_pend[PEND_N];
static __thread int tl_pend_n;
void nr_sense_unit_flush_pending(void)
{
  for (int i = 0; i < tl_pend_n; i++)
    nr_sense_unit_skip_w(tl_pend[i].s, tl_pend[i].rnti, tl_pend[i].est, tl_pend[i].t, take_weight(tl_pend[i].s, tl_pend[i].rnti));
  tl_pend_n = 0;
}
bool nr_sense_unit_gate_deferred(nr_unit_src_t s, uint16_t rnti, uint16_t est_usable_prbs)
{
  if (nr_sense_unit_enabled(s, rnti))
    return true;
  if (s == NR_UNIT_SRS && !est_usable_prbs)
    est_usable_prbs = (uint16_t)nr_isac_sense_min_prb();
  if (tl_pend_n == PEND_N)
    nr_sense_unit_flush_pending();
  tl_pend[tl_pend_n].s = s; tl_pend[tl_pend_n].rnti = rnti; tl_pend[tl_pend_n].est = est_usable_prbs; tl_pend[tl_pend_n].t = now_ns();
  tl_pend_n++;
  return false;
}

static int cmp_u64(const void *a, const void *b);

void nr_sense_dci_arrival(uint16_t prbs, uint64_t t_ns)
{
  if (!prbs || t_ns < 1000 || nr_isac_mode() != NR_ISAC_MODE_SENSING) return;
  unit_tab_t *T = tab_of(nr_cell_idx());
  const uint64_t i = atomic_fetch_add_explicit(&T->ev_head, 1, memory_order_relaxed);
  atomic_store_explicit(&T->ev[i % CFR_EV_N], ((t_ns / 1000) << 16) | prbs, memory_order_relaxed);
}

void nr_sense_unit_arrival_gaps(int cell, uint64_t now_ns, uint64_t window_ns, const int *thr, int n_thr, nr_sense_gapstat_t *out)
{
  for (int i = 0; i < n_thr; i++) out[i] = (nr_sense_gapstat_t){0};
  if (cell < 0 || cell >= NR_CELL_MAX || !window_ns || now_ns < window_ns) return;
  unit_tab_t *T = tab_of(cell);
  const uint64_t head = atomic_load_explicit(&T->ev_head, memory_order_acquire);
  const uint64_t cnt = head < CFR_EV_N ? head : CFR_EV_N, t0 = now_ns - window_ns;
  uint64_t ts[CFR_EV_N + 1], tt[CFR_EV_N]; /* ~64 KB on the stack of the scheduler thread (2 MB): times of the events passing one threshold / all events */
  uint32_t pr[CFR_EV_N];
  uint64_t start = t0;
  for (uint64_t k = 0; k < cnt; k++) {
    const uint64_t e = atomic_load_explicit(&T->ev[(head - 1 - k) % CFR_EV_N], memory_order_relaxed);
    tt[k] = (e >> 16) * 1000; pr[k] = (uint32_t)(e & 0xFFFF);
  }
  if (cnt == CFR_EV_N) { /* the ring wrapped: it only covers the time back to its oldest record (unsorted by a few writers, so take the minimum) */
    uint64_t mn = tt[0];
    for (uint64_t k = 1; k < cnt; k++) if (tt[k] < mn) mn = tt[k];
    if (mn > start) start = mn;
  }
  for (int i = 0; i < n_thr; i++) {
    uint64_t n = 0;
    for (uint64_t k = 0; k < cnt; k++)
      if (tt[k] > start && tt[k] <= now_ns && pr[k] >= (uint32_t)thr[i]) ts[n++] = tt[k];
    qsort(ts, n, sizeof ts[0], cmp_u64);
    uint64_t *g = ts; /* gaps in place: ts[j] becomes the gap that ends at event j; the last gap runs to now */
    uint64_t prev = start;
    for (uint64_t j = 0; j < n; j++) { const uint64_t t = ts[j]; g[j] = t - prev; prev = t; }
    g[n] = now_ns - prev;
    qsort(g, n + 1, sizeof g[0], cmp_u64);
    const uint64_t rank = ((n + 1) * 95 + 99) / 100; /* nearest rank, 1-based */
    out[i].p95_gap_ns = g[rank - 1];
    out[i].n = (uint32_t)n;
    out[i].rate_hz = now_ns > start ? (float)((double)n * 1e9 / (double)(now_ns - start)) : 0.f;
  }
}

void nr_sense_unit_cell_stats(int cell, nr_sense_unit_cell_stats_t *out)
{
  memset(out, 0, sizeof *out);
  if (cell < 0 || cell >= NR_CELL_MAX)
    return;
  unit_tab_t *T = tab_of(cell);
  out->violations = atomic_load_explicit(&T->viol_total, memory_order_relaxed);
  out->skips = atomic_load_explicit(&T->skip_total, memory_order_relaxed);
  out->wb_valid = atomic_load_explicit(&T->wb_valid, memory_order_relaxed);
  out->wb_valid_prbs = atomic_load_explicit(&T->wb_valid_prbs, memory_order_relaxed);
  out->gap_max_ns = atomic_load_explicit(&T->gap_max_ns, memory_order_relaxed);
  for (int r = 0; r < NR_UNIT_ROWS; r++) {
    const uint32_t k = atomic_load_explicit(&T->key[r], memory_order_acquire);
    if (k == 0 || k == 0xFFFFFFFFu)
      continue;
    for (int s = 0; s < NR_UNIT_SRC_N; s++)
      if (atomic_load_explicit(&T->u[r][s].live, memory_order_relaxed) && !atomic_load_explicit(&T->u[r][s].disabled, memory_order_relaxed))
        out->units_enabled++;
  }
}

void nr_sense_unit_decode_job(uint64_t cpu_ns, uint64_t wait_ns, uint64_t t_ns)
{
  (void)t_ns;
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING) return;
  unit_tab_t *T = tab_of(nr_cell_idx());
  if (cpu_ns) atomic_fetch_add_explicit(&T->unattributed_ns, cpu_ns, memory_order_relaxed);
  uint64_t o = atomic_load_explicit(&T->dec_wait_max, memory_order_relaxed);
  while (o < wait_ns && !atomic_compare_exchange_weak_explicit(&T->dec_wait_max, &o, wait_ns, memory_order_relaxed, memory_order_relaxed)) {}
}
uint64_t nr_sense_unit_take_decode_wait_max(int cell)
{
  return cell < 0 || cell >= NR_CELL_MAX ? 0 : atomic_exchange_explicit(&tab_of(cell)->dec_wait_max, 0, memory_order_relaxed);
}

uint64_t nr_sense_unit_unattributed_ns(int cell)
{
  return cell < 0 || cell >= NR_CELL_MAX ? 0 : atomic_load_explicit(&tab_of(cell)->unattributed_ns, memory_order_relaxed);
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
    const uint32_t g0 = atomic_load_explicit(&T->gen[r], memory_order_acquire);
    const uint32_t key = atomic_load_explicit(&T->key[r], memory_order_acquire);
    if (key == 0 || key == 0xFFFFFFFFu)
      continue;
    for (int s = 0; s < NR_UNIT_SRC_N && n < max; s++) {
      unit_t *u = &T->u[r][s];
      if (!atomic_load_explicit(&u->live, memory_order_relaxed))
        continue;
      uint64_t cpu[NB], wt[NB], rgn[NB], dqn[NB];
      int nw = 0;
      uint64_t msgs = 0, wbs = 0;
      for (int i = 0; i < NB; i++) {
        const uint64_t b = bn - NB + (uint64_t)i;
        const ubucket_t *bk = &u->bk[b % NBS];
        const uint64_t a = atomic_load_explicit(&bk->a, memory_order_relaxed), w = atomic_load_explicit(&bk->b, memory_order_relaxed);
        cpu[i] = (a >> 40) == tag_of(b) ? (a & CPU_MASK) : 0;
        { const uint64_t x = atomic_load_explicit(&u->rg[b % NBS], memory_order_relaxed), y = atomic_load_explicit(&u->dq[b % NBS], memory_order_relaxed);
          rgn[i] = (x >> 40) == tag_of(b) ? (x & CPU_MASK) : 0; dqn[i] = (y >> 40) == tag_of(b) ? (y & CPU_MASK) : 0; }
        if ((w >> 40) == tag_of(b)) {
          msgs += (w >> 10) & CNT_MAX;
          wbs += w & CNT_MAX;
          const uint64_t wm = (w >> 20) & WAIT_MAX;
          if (wm || ((w >> 10) & CNT_MAX)) wt[nw++] = wm * 1000ull;
        }
      }
      qsort(cpu, NB, sizeof cpu[0], cmp_u64);
      qsort(rgn, NB, sizeof rgn[0], cmp_u64);
      qsort(dqn, NB, sizeof dqn[0], cmp_u64);
      qsort(wt, (size_t)nw, sizeof wt[0], cmp_u64);
      const uint64_t first = atomic_load_explicit(&u->first_ns, memory_order_relaxed);
      double win_s = (double)NB * (double)SENSE_BUCKET_NS * 1e-9; /* a unit younger than the window is divided by its age (at least one bucket) */
      if (first && now > first && (double)(now - first) * 1e-9 < win_s) win_s = (double)(now - first) * 1e-9;
      if (win_s < SENSE_BUCKET_NS * 1e-9) win_s = SENSE_BUCKET_NS * 1e-9;
      nr_sense_unit_view_t *v = &out[n++];
      memset(v, 0, sizeof *v);
      v->rnti = (uint16_t)(key - 1u);
      v->src = (nr_unit_src_t)s;
      v->enabled = !atomic_load_explicit(&u->disabled, memory_order_relaxed);
      v->cpu_p95_ns = (float)nrank(cpu, NB, 95);
      v->cpu_p99_ns = (float)nrank(cpu, NB, 99);
      v->cpu_max_ns = (float)cpu[NB - 1];
      v->ring_p99_ns = (float)nrank(rgn, NB, 99);
      v->da_p99_ns = (float)nrank(dqn, NB, 99);
      v->gen = g0;
      v->last_cfr_ns = atomic_load_explicit(&u->last_cfr_ns, memory_order_relaxed);
      v->n_cfr = atomic_load_explicit(&u->n_cfr, memory_order_relaxed);
      v->est_usable_prbs = (uint16_t)atomic_load_explicit(&u->est_last, memory_order_relaxed);
      v->first_ns = first;
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
      /* the row was evicted while it was being read: its history is the successor's zeros under the old RNTI, discard the view (M-2) */
      if (atomic_load_explicit(&T->key[r], memory_order_acquire) != key || atomic_load_explicit(&T->gen[r], memory_order_acquire) != g0) n--;
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
  const int r = row_find(T, unit_rnti(s, rnti), NULL);
  if (r < 0)
    return 0;
  const uint64_t b = t_ns / SENSE_BUCKET_NS;
  const uint64_t a = atomic_load_explicit(&T->u[r][s].bk[b % NBS].a, memory_order_relaxed);
  return (a >> 40) == tag_of(b) ? (a & CPU_MASK) : 0;
}

void nr_sense_unit_reset(int cell)
{
  if (cell < 0 || cell >= NR_CELL_MAX)
    return;
  unit_tab_t *T = tab_of(cell);
  for (int r = 0; r < NR_UNIT_ROWS; r++) {
    atomic_store_explicit(&T->key[r], 0, memory_order_release);
    atomic_fetch_add_explicit(&T->gen[r], 1, memory_order_acq_rel);
    atomic_store_explicit(&T->row_last[r], 0, memory_order_relaxed);
    for (int s = 0; s < NR_UNIT_SRC_N; s++) reset_unit(&T->u[r][s]);
  }
  atomic_store_explicit(&T->unattributed_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&T->dec_wait_max, 0, memory_order_relaxed);
  atomic_store_explicit(&T->viol_total, 0, memory_order_relaxed);
  atomic_store_explicit(&T->skip_total, 0, memory_order_relaxed);
  atomic_store_explicit(&T->wb_valid, 0, memory_order_relaxed);
  atomic_store_explicit(&T->wb_valid_prbs, 0, memory_order_relaxed);
  atomic_store_explicit(&T->last_wb_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&T->gap_max_ns, 0, memory_order_relaxed);
  atomic_store_explicit(&T->ev_head, 0, memory_order_relaxed);
  atomic_store_explicit(&T->wbg_offered, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_admitted, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_taps, 0, memory_order_relaxed);
  atomic_store_explicit(&T->wbg_cost_ns, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_rec, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_ring_ns, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_refunded, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_overcap, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_tok, 0, memory_order_relaxed);
  atomic_store_explicit(&T->wbg_rate_m1, 0, memory_order_relaxed); atomic_store_explicit(&T->wbg_widest, 0, memory_order_relaxed);
  nr_sense_sched_cell_reset(cell); /* a new unit table = a new cell: its T_cell restarts at sense_min_prb */
}
void nr_sense_unit_cell_resync(int cell)
{
  if (nr_isac_mode() == NR_ISAC_MODE_SENSING) nr_sense_unit_reset(cell); /* legacy and traffic: untouched */
}
