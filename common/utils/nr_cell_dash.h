/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI-DASH (docs/superpowers/reports/csd/MPCI-DASH-20261027.md section 3.1): formatters of the per-cell log lines P2 CELL_GPU, P4 CELL_Q, P5 LDPC_ITER,
 * P6 CELL_TIMING, P7 CELL_BUDGET, P8 SLOT_CELL and P9 CELL_SUM. (P1 CELL_CPU lives beside the pool counters, common/utils/nr_core_pool.c; P3 CHAN_CELL beside
 * iq_feed, radio/COMMON/iq_feed.c.)
 *
 * Header-only and pure: no LOG, no cell lookup, no allocation, no global state. Every formatter takes the number of cells of the process (n_cells) and writes
 * NOTHING (returns 0) when n_cells <= 1, so a single-cell log is byte-identical to the receiver without these lines (plan R12: no flag, no env var; the gate is
 * nr_cell_count() > 1, passed by the caller). Every line names its cell as "cell=<k> pci=<pci>" explicitly (the LOG tag [c<k>] is the cell of the emitting thread;
 * the fields do not depend on it). Values are window deltas ("(D)" in the report) of cumulative counters: the caller keeps the previous snapshot, and the first
 * call of a cell (nr_dash_win_tick returning -1) only primes the window and prints nothing. Counters are bumped by the callers with relaxed adds; nothing here
 * runs on a per-slot / per-code-block path. All sizes (slot duration, bins, pool size) are arguments: nothing is a constant of a host. */
#ifndef NR_CELL_DASH_H
#define NR_CELL_DASH_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NR_DASH_IT_BINS 7    /* LDPC iteration classes: 1, 2, 3, 4-6, 7-10, 11+, cap (a code block hit the iteration cap) */
#define NR_DASH_SLOT_BINS 24 /* log2(us) histogram of the slot processing time: bin b = [2^(b-1), 2^b) us, bin 0 = < 1 us (as the pool wait histogram) */

static inline int nr_dash_on(int n_cells)
{
  return n_cells > 1;
}

/* One window per emitting site and cell (the caller owns it, e.g. an NR_CELL_STATIC). Returns the window length in ns, or -1 for the first call (primes) / a clock
 * that did not advance (re-primes). */
typedef struct {
  uint64_t t_ns;
  int primed;
} nr_dash_win_t;
static inline int64_t nr_dash_win_tick(nr_dash_win_t *w, uint64_t now_ns)
{
  const int was = w->primed;
  const uint64_t prev = w->t_ns;
  w->t_ns = now_ns;
  w->primed = 1;
  if (!was || now_ns <= prev)
    return -1;
  return (int64_t)(now_ns - prev);
}

/* append with truncation tracking: o = bytes wanted so far (may exceed n); finish with nr_dash_end */
static inline int nr_dash_cat(char *buf, size_t n, int o, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int r = 0;
  if (o >= 0 && (size_t)o < n)
    r = vsnprintf(buf + o, n - (size_t)o, fmt, ap);
  va_end(ap);
  return r < 0 ? o : o + r;
}
static inline int nr_dash_end(size_t n, int o)
{
  return (size_t)o < n ? o : (int)n - 1;
}
static inline unsigned long nr_dash_ul(uint64_t v)
{
  return (unsigned long)v;
}
static inline uint64_t nr_dash_d(uint64_t cur, uint64_t prev)
{
  return cur >= prev ? cur - prev : 0; /* a counter that went backwards (re-initialised cell) reads as 0, never as a huge delta */
}
static inline double nr_dash_pct(uint64_t num, uint64_t den)
{
  return den ? 100.0 * (double)num / (double)den : 0.0;
}
/* upper bound (us) of the q-quantile of a log2(us) histogram with `nbins` bins (bin 0: 1) */
static inline uint64_t nr_dash_hist_q(const uint64_t *h, int nbins, double q)
{
  uint64_t tot = 0, c = 0;
  for (int b = 0; b < nbins; b++)
    tot += h[b];
  if (!tot)
    return 0;
  for (int b = 0; b < nbins; b++) {
    c += h[b];
    if ((double)c >= q * (double)tot)
      return b == 0 ? 1 : (1ull << b);
  }
  return 1ull << (nbins - 1);
}
/* log2(us) bin of a duration in ns (the histogram above) */
static inline int nr_dash_us_bin(uint64_t ns, int nbins)
{
  uint64_t us = ns / 1000;
  int b = 0;
  while (us && b < nbins - 1)
    us >>= 1, b++;
  return b;
}

/* ---- P4 CELL_Q: backlog and drops of the cell's queues ------------------------------------------------------------------------------------------------------- */
typedef struct {
  int depth;                  /* entries held now, -1 = not available for this queue */
  int age_p50, age_p99;       /* lag bin upper bound in slots over the window (<= value), -1 = not available */
  uint64_t lag_win;           /* worst lag (slots) seen in the window (reset at read) */
  uint64_t drop_full, drop_stale, drop_epoch; /* cumulative */
} nr_dash_q1_t;
typedef struct {
  nr_dash_q1_t scan, pdsch, pusch;
} nr_dash_q_t;
static inline int nr_dash_q_block(char *buf, size_t n, int o, const char *name, const nr_dash_q1_t *c, const nr_dash_q1_t *p)
{
  o = nr_dash_cat(buf, n, o, " %s[", name);
  if (c->depth >= 0)
    o = nr_dash_cat(buf, n, o, "depth_now=%d ", c->depth);
  if (c->age_p50 >= 0)
    o = nr_dash_cat(buf, n, o, "age_p50_slots<=%d age_p99_slots<=%d ", c->age_p50, c->age_p99);
  return nr_dash_cat(buf, n, o, "max_lag_win=%lu drop_full=%lu drop_stale=%lu drop_epoch=%lu]", nr_dash_ul(c->lag_win), nr_dash_ul(nr_dash_d(c->drop_full, p->drop_full)),
                     nr_dash_ul(nr_dash_d(c->drop_stale, p->drop_stale)), nr_dash_ul(nr_dash_d(c->drop_epoch, p->drop_epoch)));
}
/* "SENSING: CELL_Q cell=<k> pci=<pci> win_ms=<ms> scan[depth_now= age_p50_slots<= age_p99_slots<= max_lag_win= drop_full= drop_stale= drop_epoch=] pdsch[..] pusch[..]"
 * (a field the queue cannot supply is left out, see the report) */
static inline int nr_dash_fmt_cell_q(char *buf, size_t n, int n_cells, int cell, int pci, int64_t win_ns, const nr_dash_q_t *cur, const nr_dash_q_t *prev)
{
  if (!nr_dash_on(n_cells) || win_ns <= 0 || !buf || !n)
    return 0;
  int o = nr_dash_cat(buf, n, 0, "SENSING: CELL_Q cell=%d pci=%d win_ms=%lu", cell, pci, nr_dash_ul((uint64_t)win_ns / 1000000ull));
  o = nr_dash_q_block(buf, n, o, "scan", &cur->scan, &prev->scan);
  o = nr_dash_q_block(buf, n, o, "pdsch", &cur->pdsch, &prev->pdsch);
  o = nr_dash_q_block(buf, n, o, "pusch", &cur->pusch, &prev->pusch);
  return nr_dash_end(n, o);
}

/* ---- P5 LDPC_ITER: iteration effort of the cell's decoded transport blocks -------------------------------------------------------------------------------------- */
typedef struct {
  uint64_t tb;                      /* decoded transport blocks (probes excluded) with an LDPC run */
  uint64_t cb_tried, cb_ok;         /* code blocks the decoder was started on / that passed their CRC */
  uint64_t it_sum;                  /* sum of the iteration counts of the code blocks that decoded (EVMGRANT its=) */
  uint64_t hist[NR_DASH_IT_BINS];   /* per TB, by the largest iteration count of its code blocks (EVMGRANT it=) */
} nr_dash_ldpc_t;
/* the class of a TB whose slowest code block needed it_max iterations under the cap `cap` (0 = unknown cap); it_max < 1 -> -1 (no LDPC ran) */
static inline int nr_dash_it_bin(int it_max, int cap)
{
  if (it_max < 1)
    return -1;
  if (cap > 0 && it_max >= cap)
    return 6;
  if (it_max <= 3)
    return it_max - 1;
  return it_max <= 6 ? 3 : it_max <= 10 ? 4 : 5;
}
/* "SENSING: LDPC_ITER cell=<k> pci=<pci> win_ms=<ms> tb=<n> cb=<cb_tried> it_hist[1,2,3,4-6,7-10,11+,cap]=a,b,c,d,e,f,g at_cap_pct=<TB with a block at the cap> cb_fail_pct=<> mean_it=<its per decoded block>" */
static inline int nr_dash_fmt_ldpc_iter(char *buf, size_t n, int n_cells, int cell, int pci, int64_t win_ns, const nr_dash_ldpc_t *c, const nr_dash_ldpc_t *p)
{
  if (!nr_dash_on(n_cells) || win_ns <= 0 || !buf || !n)
    return 0;
  uint64_t h[NR_DASH_IT_BINS], tb_hist = 0;
  for (int i = 0; i < NR_DASH_IT_BINS; i++)
    tb_hist += h[i] = nr_dash_d(c->hist[i], p->hist[i]);
  const uint64_t cbt = nr_dash_d(c->cb_tried, p->cb_tried), cbo = nr_dash_d(c->cb_ok, p->cb_ok), its = nr_dash_d(c->it_sum, p->it_sum);
  int o = nr_dash_cat(buf, n, 0, "SENSING: LDPC_ITER cell=%d pci=%d win_ms=%lu tb=%lu cb=%lu it_hist[1,2,3,4-6,7-10,11+,cap]=", cell, pci, nr_dash_ul((uint64_t)win_ns / 1000000ull),
                      nr_dash_ul(nr_dash_d(c->tb, p->tb)), nr_dash_ul(cbt));
  for (int i = 0; i < NR_DASH_IT_BINS; i++)
    o = nr_dash_cat(buf, n, o, "%s%lu", i ? "," : "", nr_dash_ul(h[i]));
  o = nr_dash_cat(buf, n, o, " at_cap_pct=%.1f cb_fail_pct=%.1f mean_it=%.2f", nr_dash_pct(h[NR_DASH_IT_BINS - 1], tb_hist), nr_dash_pct(cbt >= cbo ? cbt - cbo : 0, cbt),
                  cbo ? (double)its / (double)cbo : 0.0);
  return nr_dash_end(n, o);
}

/* ---- P6 CELL_TIMING: the timing / sync state of the cell ------------------------------------------------------------------------------------------------------- */
typedef struct {
  double sfo_eps_ppm;               /* published SFO estimate of the cell, ppm */
  int cfo_hz, max_pos_acc, shift_next;
  double sync_age_s;                /* seconds since the last "Got synch" of the cell, -1 = never */
  uint64_t discont, resync;         /* cumulative: RXDISCONT events, "Got synch" events */
} nr_dash_timing_t;
/* "SENSING: CELL_TIMING cell=<k> pci=<pci> win_ms=<ms> sfo_eps_ppm=<+x.xxx> cfo_hz=<> max_pos_acc=<> shift_next=<> sync_age_s=<> discont=<D> resync=<D>" */
static inline int nr_dash_fmt_cell_timing(char *buf, size_t n, int n_cells, int cell, int pci, int64_t win_ns, const nr_dash_timing_t *c, const nr_dash_timing_t *p)
{
  if (!nr_dash_on(n_cells) || win_ns <= 0 || !buf || !n)
    return 0;
  const int o = nr_dash_cat(buf, n, 0, "SENSING: CELL_TIMING cell=%d pci=%d win_ms=%lu sfo_eps_ppm=%+.3f cfo_hz=%d max_pos_acc=%d shift_next=%d sync_age_s=%.1f discont=%lu resync=%lu", cell,
                            pci, nr_dash_ul((uint64_t)win_ns / 1000000ull), c->sfo_eps_ppm, c->cfo_hz, c->max_pos_acc, c->shift_next, c->sync_age_s,
                            nr_dash_ul(nr_dash_d(c->discont, p->discont)), nr_dash_ul(nr_dash_d(c->resync, p->resync)));
  return nr_dash_end(n, o);
}

/* ---- P2 CELL_GPU: what the host side of the GPU pass can attribute to the cell (no device time: see the report) ------------------------------------------------- */
typedef struct {
  uint64_t calls, fallback;         /* cumulative: DL pass calls of the cell, of which refused on the host (the old CPU path took them) */
  uint64_t mem_dev_b, mem_mgd_b, mem_pin_b; /* current GPU memory booked to the cell (device / managed / pinned), bytes */
  int active;                       /* the GPU pass module is up */
} nr_dash_gpu_t;
/* "SENSING: CELL_GPU cell=<k> pci=<pci> win_ms=<ms> active=<0|1> calls=<D> fallback=<D> fallback_pct=<> gpu_mem_mb=<dev+managed> pin_mb=<>" */
static inline int nr_dash_fmt_cell_gpu(char *buf, size_t n, int n_cells, int cell, int pci, int64_t win_ns, const nr_dash_gpu_t *c, const nr_dash_gpu_t *p)
{
  if (!nr_dash_on(n_cells) || win_ns <= 0 || !buf || !n)
    return 0;
  const uint64_t calls = nr_dash_d(c->calls, p->calls), fb = nr_dash_d(c->fallback, p->fallback);
  const int o = nr_dash_cat(buf, n, 0, "SENSING: CELL_GPU cell=%d pci=%d win_ms=%lu active=%d calls=%lu fallback=%lu fallback_pct=%.1f gpu_mem_mb=%lu pin_mb=%lu", cell, pci,
                            nr_dash_ul((uint64_t)win_ns / 1000000ull), c->active, nr_dash_ul(calls), nr_dash_ul(fb), nr_dash_pct(fb, calls),
                            nr_dash_ul((c->mem_dev_b + c->mem_mgd_b) >> 20), nr_dash_ul(c->mem_pin_b >> 20));
  return nr_dash_end(n, o);
}

/* ---- P7 CELL_BUDGET: planned (admission) against booked / live memory ------------------------------------------------------------------------------------------ */
typedef struct {
  long long ram_admit_mb, gpu_admit_mb;   /* the admission plan's committed bytes of the cell (planned, MB; -1 unknown) */
  long long gpu_now_mb, pin_now_mb;       /* GPU memory booked to the cell now (device+managed / pinned), MB */
  double tokens_fair;                     /* pool tokens / cells */
} nr_dash_budget_t;
/* "SENSING: CELL_BUDGET scope=cell cell=<k> pci=<pci> ram_admit_mb= gpu_admit_mb= gpu_now_mb= pin_now_mb= tokens_fair=" */
static inline int nr_dash_fmt_cell_budget(char *buf, size_t n, int n_cells, int cell, int pci, const nr_dash_budget_t *c)
{
  if (!nr_dash_on(n_cells) || !buf || !n)
    return 0;
  const int o = nr_dash_cat(buf, n, 0, "SENSING: CELL_BUDGET scope=cell cell=%d pci=%d ram_admit_mb=%lld gpu_admit_mb=%lld gpu_now_mb=%lld pin_now_mb=%lld tokens_fair=%.2f", cell, pci,
                            c->ram_admit_mb, c->gpu_admit_mb, c->gpu_now_mb, c->pin_now_mb, c->tokens_fair);
  return nr_dash_end(n, o);
}
/* "SENSING: CELL_BUDGET scope=proc cells=<n> host_budget_mb= host_used_mb= headroom_mb= avail_now_mb= rss_mb=" (-1 = unknown / unlimited) */
static inline int nr_dash_fmt_budget_proc(char *buf, size_t n, int n_cells, long long budget_mb, long long used_mb, long long headroom_mb, long long avail_mb, long long rss_mb)
{
  if (!nr_dash_on(n_cells) || !buf || !n)
    return 0;
  const int o = nr_dash_cat(buf, n, 0, "SENSING: CELL_BUDGET scope=proc cells=%d host_budget_mb=%lld host_used_mb=%lld headroom_mb=%lld avail_now_mb=%lld rss_mb=%lld", n_cells, budget_mb,
                            used_mb, headroom_mb, avail_mb, rss_mb);
  return nr_dash_end(n, o);
}

/* ---- P8 SLOT_CELL: processing time of the cell's slot thread (after the radio read until the slot is handed on) ------------------------------------------------ */
typedef struct {
  uint64_t slots, late;                      /* cumulative: slots that completed the loop body, of which longer than one slot */
  uint64_t hist[NR_DASH_SLOT_BINS];          /* log2(us) of the processing time */
  uint64_t max_ns;                           /* window maximum (the caller resets it after the read) */
} nr_dash_slot_t;
/* "SENSING: SLOT_CELL cell=<k> pci=<pci> win_ms=<ms> slot_us=<> slots=<D> proc_us p50<=<> p99<=<> max=<> late_slots=<D> cpu=<core of the thread>" */
static inline int nr_dash_fmt_slot_cell(char *buf, size_t n, int n_cells, int cell, int pci, int64_t win_ns, uint64_t slot_ns, int cpu, const nr_dash_slot_t *c,
                                        const nr_dash_slot_t *p)
{
  if (!nr_dash_on(n_cells) || win_ns <= 0 || !buf || !n)
    return 0;
  uint64_t d[NR_DASH_SLOT_BINS];
  for (int i = 0; i < NR_DASH_SLOT_BINS; i++)
    d[i] = nr_dash_d(c->hist[i], p->hist[i]);
  const int o = nr_dash_cat(buf, n, 0, "SENSING: SLOT_CELL cell=%d pci=%d win_ms=%lu slot_us=%lu slots=%lu proc_us p50<=%lu p99<=%lu max=%lu late_slots=%lu cpu=%d", cell, pci,
                            nr_dash_ul((uint64_t)win_ns / 1000000ull), nr_dash_ul(slot_ns / 1000), nr_dash_ul(nr_dash_d(c->slots, p->slots)),
                            nr_dash_ul(nr_dash_hist_q(d, NR_DASH_SLOT_BINS, 0.5)), nr_dash_ul(nr_dash_hist_q(d, NR_DASH_SLOT_BINS, 0.99)), nr_dash_ul(c->max_ns / 1000),
                            nr_dash_ul(nr_dash_d(c->late, p->late)), cpu);
  return nr_dash_end(n, o);
}

/* ---- P9 CELL_SUM: one line per cell composed of the counters the other lines are built from ------------------------------------------------------------------- */
typedef struct {
  const char *state;                         /* ACQ_STATE name */
  uint64_t decoded, crc_ok, cb_tried, cb_ok; /* PDSCH queue of the cell, cumulative */
  uint64_t drops;                            /* scan + PDSCH queue drops (full + stale), cumulative */
  uint64_t discont;                          /* RXDISCONT events, cumulative */
  uint64_t hold_ns;                          /* core-pool token time of the cell, cumulative */
} nr_dash_sum_t;
/* "SENSING: CELL_SUM cell=<k> pci=<pci> win_ms=<ms> state=<ACQ_STATE> decode_ok_pct=<D> grants=<D> cb_crc_pct=<D> drops=<D> disc=<D> tokens_avg=<hold/window>" */
static inline int nr_dash_fmt_cell_sum(char *buf, size_t n, int n_cells, int cell, int pci, int64_t win_ns, const nr_dash_sum_t *c, const nr_dash_sum_t *p)
{
  if (!nr_dash_on(n_cells) || win_ns <= 0 || !buf || !n)
    return 0;
  const uint64_t dec = nr_dash_d(c->decoded, p->decoded), ok = nr_dash_d(c->crc_ok, p->crc_ok), cbt = nr_dash_d(c->cb_tried, p->cb_tried), cbo = nr_dash_d(c->cb_ok, p->cb_ok);
  const int o = nr_dash_cat(buf, n, 0, "SENSING: CELL_SUM cell=%d pci=%d win_ms=%lu state=%s decode_ok_pct=%.1f grants=%lu cb_crc_pct=%.1f drops=%lu disc=%lu tokens_avg=%.2f", cell, pci,
                            nr_dash_ul((uint64_t)win_ns / 1000000ull), c->state ? c->state : "-", nr_dash_pct(ok, dec), nr_dash_ul(dec), nr_dash_pct(cbo, cbt),
                            nr_dash_ul(nr_dash_d(c->drops, p->drops)), nr_dash_ul(nr_dash_d(c->discont, p->discont)),
                            (double)nr_dash_d(c->hold_ns, p->hold_ns) / (double)win_ns);
  return nr_dash_end(n, o);
}

#ifdef __cplusplus
}
#endif
#endif
