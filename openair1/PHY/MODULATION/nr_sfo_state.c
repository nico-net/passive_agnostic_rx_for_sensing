/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Receiver SFO provider and FEP correction: see nr_sfo_state.h. */
#include "nr_sfo_state.h"
#include <math.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"

/* ALL the provider state in one struct, one instance per cell (NR_CELL_STATIC; multi-PCI design note in
 * docs/superpowers/reports/csd/FIX-SFO-RESYNC-20261026.md). The eps is relative to the clock of the gNB being tracked, so it is
 * meaningless across a cell change or a timing epoch. Threads that serve a cell are bound to it (nr_cell_idx()): the timing loop
 * (UE_thread), the FEP and the decode workers. N = 1: cell 0, as before. */
typedef struct {
  _Atomic int64_t eps_q;   /* published eps, 1e-12 */
  pthread_mutex_t mu;
  int win[NR_SFO_WINDOW];  /* per-frame extra samples, ring */
  int n, w;                /* frames in the window, write index */
  long sum;                /* sum of the window */
  int spf;                 /* samples per frame of the window */
  double dmrs_corr;        /* fine correction from the DM-RS residual, eps units */
  uint64_t frames, dmrs_n;
} nr_sfo_state_t;
NR_CELL_STATIC_INIT(nr_sfo_state_t, g_st, {.mu = PTHREAD_MUTEX_INITIALIZER})
#define g_st NR_CELL_REF(g_st)
/* thread-local pin: a value (what this job's FEP uses), not cell state */
static __thread int64_t t_pin = INT64_MIN;

static void reset_cell(nr_sfo_state_t *s)
{
  pthread_mutex_lock(&s->mu);
  s->n = s->w = 0;
  s->sum = 0;
  s->spf = 0;
  s->dmrs_corr = 0;
  s->frames = s->dmrs_n = 0;
  atomic_store(&s->eps_q, 0);
  pthread_mutex_unlock(&s->mu);
}

void nr_sfo_reset(void)
{
  reset_cell(&g_st);
}

/* tag: the cell is named in the line only when the caller is not that cell's own thread (otherwise the D2 [c<k>] prefix says it) */
static void resync_cell(int cell, const char *reason)
{
  nr_sfo_state_t *s = &g_st_cells[cell];
  const int64_t old = atomic_load_explicit(&s->eps_q, memory_order_relaxed);
  reset_cell(s);
  if (cell == nr_cell_idx())
    LOG_I(PHY, "SENSING: SFO reset (%s); previous eps %+.3f ppm\n", reason ? reason : "?", (double)old * 1e-6);
  else
    LOG_I(PHY, "SENSING: SFO reset (%s) of cell %d; previous eps %+.3f ppm\n", reason ? reason : "?", cell, (double)old * 1e-6);
}

void nr_sfo_resync(const char *reason)
{
  resync_cell(nr_cell_idx(), reason);
}

void nr_sfo_resync_cell(int cell, const char *reason)
{
  if (cell >= 0 && cell < NR_CELL_MAX)
    resync_cell(cell, reason);
}

void nr_sfo_resync_all(const char *reason)
{
  const int own = nr_cell_idx();
  const int n = nr_cell_count();
  resync_cell(own, reason);
  for (int c = 0; c < n && c < NR_CELL_MAX; c++)
    if (c != own)
      resync_cell(c, reason);
}

/* caller holds s->mu: publish the window mean plus the DM-RS fine term when it moved by more than the hysteresis */
static void publish_locked(nr_sfo_state_t *s)
{
  if (s->n < NR_SFO_WINDOW / 4 || s->spf <= 0)
    return;
  const double eps = (double)s->sum / ((double)s->n * s->spf) + s->dmrs_corr;
  if (!isfinite(eps) || fabs(eps) > NR_SFO_MAX_PPM * 1e-6)
    return;
  const int64_t q = (int64_t)llround(eps * 1e12);
  const int64_t old = atomic_load_explicit(&s->eps_q, memory_order_relaxed);
  if (llabs(q - old) <= NR_SFO_HYST_Q)
    return;
  atomic_store_explicit(&s->eps_q, q, memory_order_release);
  LOG_I(PHY, "SENSING: SFO eps=%+.3f ppm (timing loop %+.3f ppm over %d frames, DM-RS residual term %+.3f ppm, %lu DM-RS samples)\n", (double)q * 1e-6,
        (double)s->sum / ((double)s->n * s->spf) * 1e6, s->n, s->dmrs_corr * 1e6, (unsigned long)s->dmrs_n);
}

void nr_sfo_note_frame_read(int extra_samples, int samples_per_frame)
{
  if (samples_per_frame <= 0)
    return;
  /* a read longer or shorter than any oscillator can explain is a timing jump (rebase, resync), not drift */
  if (fabs((double)extra_samples) > NR_SFO_MAX_PPM * 1e-6 * samples_per_frame)
    return;
  nr_sfo_state_t *s = &g_st;
  pthread_mutex_lock(&s->mu);
  if (samples_per_frame != s->spf) { /* new numerology / bandwidth: start over */
    s->n = s->w = 0;
    s->sum = 0;
    s->spf = samples_per_frame;
  }
  if (s->n == NR_SFO_WINDOW)
    s->sum -= s->win[s->w];
  else
    s->n++;
  s->win[s->w] = extra_samples;
  s->sum += extra_samples;
  s->w = (s->w + 1) % NR_SFO_WINDOW;
  if ((++s->frames % 64) == 0)
    publish_locked(s);
  pthread_mutex_unlock(&s->mu);
}

void nr_sfo_note_dmrs_residual(double eps_residual, double weight)
{
  if (!isfinite(eps_residual) || !(weight > 0) || fabs(eps_residual) > 2e-6)
    return;
  if (weight > 1)
    weight = 1;
  nr_sfo_state_t *s = &g_st;
  pthread_mutex_lock(&s->mu);
  /* slow integrator: one grant moves the correction by at most 1 % of its residual */
  s->dmrs_corr += 0.01 * weight * eps_residual;
  if (s->dmrs_corr > 1e-6) s->dmrs_corr = 1e-6;
  if (s->dmrs_corr < -1e-6) s->dmrs_corr = -1e-6;
  if ((++s->dmrs_n % 256) == 0)
    publish_locked(s);
  pthread_mutex_unlock(&s->mu);
}

int64_t nr_sfo_eps_q(void)
{
  return atomic_load_explicit(&g_st.eps_q, memory_order_acquire);
}

double nr_sfo_eps(void)
{
  return (double)nr_sfo_eps_q() * 1e-12;
}

void nr_sfo_pin(int64_t eps_q)
{
  t_pin = eps_q;
}

int64_t nr_sfo_eps_q_for_fep(void)
{
  return t_pin != INT64_MIN ? t_pin : nr_sfo_eps_q();
}

int nr_sfo_symbol_offset(int N, int cp, int cp0, int numerology, int symbols_per_slot, int slot, int symbol)
{
  int n = 0;
  for (int l = 1; l <= symbol; l++) {
    const int abs_symb = slot * symbols_per_slot + l;
    n += N + ((abs_symb % (0x7 << numerology)) ? cp : cp0);
  }
  return n;
}

uint32_t nr_sfo_dphi(int64_t eps_q, int N, int n_l)
{
  if (eps_q == 0 || n_l == 0 || N <= 0)
    return 0;
  /* turns per subcarrier = eps * n_l / N; 2^32 turns-units, wrapped to 32 bits */
  const double turns = (double)eps_q * 1e-12 * (double)n_l / (double)N;
  const double u = turns * 4294967296.0;
  return (uint32_t)(int64_t)llround(u);
}

int nr_sfo_slot_dphi(int64_t eps_q, int N, int cp, int cp0, int numerology, int symbols_per_slot, int slot, uint32_t out[14])
{
  for (int l = 0; l < 14; l++)
    out[l] = l < symbols_per_slot ? nr_sfo_dphi(eps_q, N, nr_sfo_symbol_offset(N, cp, cp0, numerology, symbols_per_slot, slot, l)) : 0;
  return eps_q != 0;
}

void nr_sfo_rotate_symbol(int16_t *symb, int N, int fco, int nb_rb, uint32_t dphi, int k_ref)
{
  if (!dphi || !symb || N <= 0)
    return;
  /* the bins apply_nr_rotation_symbol_RX() touches */
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, start2 = (nb_rb & 1) ? fco - 6 : fco;
  /* per symbol: A[h] = exp(j 64 h dphi) for h in [-N/64, N/64), B[b] = exp(j b dphi) for b in [0, 64) -- the same integer values
   * nr_sfo_phasor() computes per RE, so this loop and a per-RE GPU kernel agree bit for bit */
  nr_sfo_c16_t B[64];
  for (int b = 0; b < 64; b++)
    B[b] = nr_sfo_sincos((uint32_t)b * dphi);
  const int hmin = -(N / 64), hn = N / 32; /* k - k_ref in [-N, N) */
  nr_sfo_c16_t A[hn > 0 ? hn : 1];
  for (int i = 0; i < hn; i++)
    A[i] = nr_sfo_sincos((uint32_t)(64 * (hmin + i)) * dphi);
  nr_sfo_c16_t *v = (nr_sfo_c16_t *)symb;
  for (int pass = 0; pass < 2; pass++) {
    const int a = pass ? start2 : 0, e = (pass ? start2 : 0) + len;
    for (int n = a; n < e && n < N; n++) {
      const int k = nr_sfo_bin_k(n, N) - k_ref;
      const int h = k >= 0 ? k / 64 : -((63 - k) / 64);
      v[n] = nr_sfo_cmul(v[n], nr_sfo_cmul(A[h - hmin], B[k - 64 * h]));
    }
  }
}
