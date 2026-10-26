/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Receiver SFO provider and FEP correction: see nr_sfo_state.h. */
#include "nr_sfo_state.h"
#include <math.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
#include "common/utils/LOG/log.h"

/* ALL the provider state in one struct: one instance now (single cell); a per-cell array indexed by nr_cell_idx() later (see the
 * multi-PCI design note in docs/superpowers/reports/csd/FIX-SFO-RESYNC-20261026.md). The eps is relative to the clock of the gNB being
 * tracked, so it is meaningless across a cell change or a timing epoch. */
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
static nr_sfo_state_t g_st = {.mu = PTHREAD_MUTEX_INITIALIZER};
#define g_eps_q (g_st.eps_q)
#define g_mu (g_st.mu)
#define g_win (g_st.win)
#define g_n (g_st.n)
#define g_w (g_st.w)
#define g_sum (g_st.sum)
#define g_spf (g_st.spf)
#define g_dmrs_corr (g_st.dmrs_corr)
#define g_frames (g_st.frames)
#define g_dmrs_n (g_st.dmrs_n)
static __thread int64_t t_pin = INT64_MIN;

void nr_sfo_reset(void)
{
  pthread_mutex_lock(&g_mu);
  g_n = g_w = 0;
  g_sum = 0;
  g_spf = 0;
  g_dmrs_corr = 0;
  g_frames = g_dmrs_n = 0;
  atomic_store(&g_eps_q, 0);
  pthread_mutex_unlock(&g_mu);
}

void nr_sfo_resync(const char *reason)
{
  const int64_t old = atomic_load_explicit(&g_eps_q, memory_order_relaxed);
  nr_sfo_reset();
  LOG_I(PHY, "SENSING: SFO reset (%s); previous eps %+.3f ppm\n", reason ? reason : "?", (double)old * 1e-6);
}

/* caller holds g_mu: publish the window mean plus the DM-RS fine term when it moved by more than the hysteresis */
static void publish_locked(void)
{
  if (g_n < NR_SFO_WINDOW / 4 || g_spf <= 0)
    return;
  const double eps = (double)g_sum / ((double)g_n * g_spf) + g_dmrs_corr;
  if (!isfinite(eps) || fabs(eps) > NR_SFO_MAX_PPM * 1e-6)
    return;
  const int64_t q = (int64_t)llround(eps * 1e12);
  const int64_t old = atomic_load_explicit(&g_eps_q, memory_order_relaxed);
  if (llabs(q - old) <= NR_SFO_HYST_Q)
    return;
  atomic_store_explicit(&g_eps_q, q, memory_order_release);
  LOG_I(PHY, "SENSING: SFO eps=%+.3f ppm (timing loop %+.3f ppm over %d frames, DM-RS residual term %+.3f ppm, %lu DM-RS samples)\n", (double)q * 1e-6,
        (double)g_sum / ((double)g_n * g_spf) * 1e6, g_n, g_dmrs_corr * 1e6, (unsigned long)g_dmrs_n);
}

void nr_sfo_note_frame_read(int extra_samples, int samples_per_frame)
{
  if (samples_per_frame <= 0)
    return;
  /* a read longer or shorter than any oscillator can explain is a timing jump (rebase, resync), not drift */
  if (fabs((double)extra_samples) > NR_SFO_MAX_PPM * 1e-6 * samples_per_frame)
    return;
  pthread_mutex_lock(&g_mu);
  if (samples_per_frame != g_spf) { /* new numerology / bandwidth: start over */
    g_n = g_w = 0;
    g_sum = 0;
    g_spf = samples_per_frame;
  }
  if (g_n == NR_SFO_WINDOW)
    g_sum -= g_win[g_w];
  else
    g_n++;
  g_win[g_w] = extra_samples;
  g_sum += extra_samples;
  g_w = (g_w + 1) % NR_SFO_WINDOW;
  if ((++g_frames % 64) == 0)
    publish_locked();
  pthread_mutex_unlock(&g_mu);
}

void nr_sfo_note_dmrs_residual(double eps_residual, double weight)
{
  if (!isfinite(eps_residual) || !(weight > 0) || fabs(eps_residual) > 2e-6)
    return;
  if (weight > 1)
    weight = 1;
  pthread_mutex_lock(&g_mu);
  /* slow integrator: one grant moves the correction by at most 1 % of its residual */
  g_dmrs_corr += 0.01 * weight * eps_residual;
  if (g_dmrs_corr > 1e-6) g_dmrs_corr = 1e-6;
  if (g_dmrs_corr < -1e-6) g_dmrs_corr = -1e-6;
  if ((++g_dmrs_n % 256) == 0)
    publish_locked();
  pthread_mutex_unlock(&g_mu);
}

int64_t nr_sfo_eps_q(void)
{
  return atomic_load_explicit(&g_eps_q, memory_order_acquire);
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
