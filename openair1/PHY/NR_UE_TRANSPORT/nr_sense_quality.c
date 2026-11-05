#include "nr_sense_quality.h"
#include <math.h>
#include <stdbool.h>
#include <string.h>

#define QMAXANT 8
#define QSYM 16
#define QKSPAN (NR_SENSE_MAX_PRB * 12)

static float pick(float *v, int n, int num, int den)
{
  for (int i = 1; i < n; i++) { /* insertion sort: n <= 275 */
    float x = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > x) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = x;
  }
  return v[(n - 1) * num / den];
}

void nr_sense_quality_compute(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                              const uint8_t *l_sym, float noise_var, nr_sense_quality_t *q)
{
  nr_sense_quality_compute_ex(n_ant, iq, ant_stride, n_re, k_abs, l_sym, noise_var, 0, q);
}

void nr_sense_quality_compute_ex(int n_ant, const float *iq, int ant_stride, int n_re, const uint16_t *k_abs,
                                 const uint8_t *l_sym, float noise_var, unsigned flags, nr_sense_quality_t *q)
{
  const bool sparse = (flags & NR_SENSE_Q_SPARSE) != 0;
  memset(q, 0, sizeof(*q));
  q->snr_db_p50 = q->snr_db_p10 = q->resid_db = q->sym_consist = q->recon_conf = NAN;
  if (n_ant < 1 || n_re < 1 || !iq || !k_abs || !l_sym)
    return;
  if (n_ant > QMAXANT)
    n_ant = QMAXANT;
  int pmin = 0xffff, kmin = 0xffff;
  for (int i = 0; i < n_re; i++) {
    if (k_abs[i] / 12 < pmin)
      pmin = k_abs[i] / 12;
    if (k_abs[i] < kmin)
      kmin = k_abs[i];
  }
  q->first_prb = (int16_t)pmin;

  uint16_t cnt[NR_SENSE_MAX_PRB][QSYM] = {{0}};
  uint16_t tot[NR_SENSE_MAX_PRB] = {0};
  uint32_t symcnt[QSYM] = {0};
  for (int i = 0; i < n_re; i++) {
    int p = k_abs[i] / 12 - pmin;
    if (p >= NR_SENSE_MAX_PRB)
      continue;
    tot[p]++;
    if (l_sym[i] < QSYM) {
      cnt[p][l_sym[i]]++;
      symcnt[l_sym[i]]++;
    }
  }
  uint8_t best[NR_SENSE_MAX_PRB];
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
    int b = 0;
    for (int s = 1; s < QSYM; s++)
      if (cnt[p][s] > cnt[p][b])
        b = s;
    best[p] = (uint8_t)b;
    if (tot[p])
      q->allocated_prbs++;
  }
  q->estimated_usable_prbs = q->allocated_prbs; /* shortcut: usable = allocated until producers report rate-matched PRBs */

  double pwr[NR_SENSE_MAX_PRB] = {0}, sse[NR_SENSE_MAX_PRB] = {0}, sig[NR_SENSE_MAX_PRB] = {0};
  for (int a = 0; a < n_ant; a++) {
    double n[NR_SENSE_MAX_PRB] = {0}, sx[NR_SENSE_MAX_PRB] = {0}, sxx[NR_SENSE_MAX_PRB] = {0};
    double shr[NR_SENSE_MAX_PRB] = {0}, shi[NR_SENSE_MAX_PRB] = {0}, sxhr[NR_SENSE_MAX_PRB] = {0};
    double sxhi[NR_SENSE_MAX_PRB] = {0}, shh[NR_SENSE_MAX_PRB] = {0};
    const float *h = iq + 2 * (size_t)a * ant_stride;
    for (int i = 0; i < n_re; i++) {
      int p = k_abs[i] / 12 - pmin;
      if (p >= NR_SENSE_MAX_PRB)
        continue;
      double hr = h[2 * i], hi = h[2 * i + 1], e = hr * hr + hi * hi;
      pwr[p] += e;
      if (l_sym[i] != best[p])
        continue;
      double x = (k_abs[i] % 12) - 5.5;
      n[p]++, sx[p] += x, sxx[p] += x * x, shr[p] += hr, shi[p] += hi;
      sxhr[p] += x * hr, sxhi[p] += x * hi, shh[p] += e;
    }
    for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
      if (n[p] < 3)
        continue;
      double mx = sx[p] / n[p], sxxc = sxx[p] - sx[p] * mx;
      double cr = sxhr[p] - mx * shr[p], ci = sxhi[p] - mx * shi[p];
      double r = shh[p] - (shr[p] * shr[p] + shi[p] * shi[p]) / n[p] - (sxxc > 1e-9 ? (cr * cr + ci * ci) / sxxc : 0);
      sse[p] += r > 0 ? r : 0;
      sig[p] += shh[p];
    }
  }

  float snr[NR_SENSE_MAX_PRB], res[NR_SENSE_MAX_PRB];
  int nv = 0;
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
    if (sparse) { /* one RE per PRB is enough: there is no local fit, so the producer's noise estimate is required and the residual test is not applied */
      if (tot[p] < 1 || !isfinite(pwr[p]) || !(pwr[p] > 0) || !(noise_var > 0))
        continue;
    } else if (tot[p] < 3 || cnt[p][best[p]] < 3 || !(sig[p] > 0) || !isfinite(pwr[p]) || !isfinite(sse[p]))
      continue;
    const double mean_pwr = pwr[p] / ((double)tot[p] * n_ant);
    /* noise of this PRB: the producer's estimate; none (<= 0 or NaN) -> the PRB's own local-fit residual. The fit removes one complex constant and one
     * complex slope from cnt REs per antenna, so for white noise E[sse] = (cnt - 2) n_ant noise_var. */
    double npw = noise_var > 0 ? noise_var : sse[p] / ((double)(cnt[p][best[p]] - 2) * n_ant); /* (sparse: noise_var > 0 here) */
    /* floor: no receiver sees an effective SNR above ~30 dB (NR_SENSE_RECON_IMPL_LOSS: transmitter EVM, phase noise, ADC), so a noise estimate
     * below that fraction of the PRB power (a noiseless simulation gives exactly 0) is raised to it: high SNR, never "invalid" */
    if (npw < NR_SENSE_RECON_IMPL_LOSS * mean_pwr)
      npw = NR_SENSE_RECON_IMPL_LOSS * mean_pwr;
    if (!(npw > 0))
      continue;
    /* channel SNR (spec 4.2a): E[mean|H|^2] = S + N, so subtract the noise; no signal left -> -100 dB (invalid) */
    double sp = mean_pwr - npw;
    float s = sp > 0 ? (float)(10.0 * log10(sp / npw)) : -100.0f;
    float r = sparse ? 0.f : (float)(sse[p] / sig[p]);
    if (s >= NR_SENSE_PRB_SNR_MIN_DB && r <= NR_SENSE_PRB_RESID_MAX) {
      q->valid_mask[p >> 3] |= (uint8_t)(1u << (p & 7));
      snr[nv] = s;
      res[nv++] = r;
    }
  }
  q->valid_cfr_prbs = (uint16_t)nv;
  if (nv) {
    q->snr_db_p10 = pick(snr, nv, 1, 10);
    q->snr_db_p50 = pick(snr, nv, 1, 2);
    float m = pick(res, nv, 1, 2);
    q->resid_db = sparse ? NAN : 10.0f * log10f(m > 1e-12f ? m : 1e-12f);
  }

  /* symbol consistency: two most populated symbols, over shared k, per-antenna complex correlation */
  int sa = -1, sb = -1;
  for (int s = 0; s < QSYM; s++) {
    if (!symcnt[s])
      continue;
    if (sa < 0 || symcnt[s] > symcnt[sa])
      sb = sa, sa = s;
    else if (sb < 0 || symcnt[s] > symcnt[sb])
      sb = s;
  }
  if (sb < 0)
    return;
  int16_t idx[QKSPAN];
  for (int i = 0; i < QKSPAN; i++)
    idx[i] = -1;
  for (int i = 0; i < n_re; i++)
    if (l_sym[i] == sa && k_abs[i] - kmin < QKSPAN)
      idx[k_abs[i] - kmin] = (int16_t)i;
  double cr = 0, ci = 0, pa = 0, pb = 0;
  for (int a = 0; a < n_ant; a++) {
    const float *h = iq + 2 * (size_t)a * ant_stride;
    for (int j = 0; j < n_re; j++) {
      if (l_sym[j] != sb || k_abs[j] - kmin >= QKSPAN || idx[k_abs[j] - kmin] < 0)
        continue;
      int i = idx[k_abs[j] - kmin];
      double ar = h[2 * i], ai = h[2 * i + 1], br = h[2 * j], bi = h[2 * j + 1];
      cr += ar * br + ai * bi, ci += ar * bi - ai * br;
      pa += ar * ar + ai * ai, pb += br * br + bi * bi;
    }
  }
  if (pa > 0 && pb > 0)
    q->sym_consist = (float)(sqrt(cr * cr + ci * ci) / sqrt(pa * pb));
  /* sparse source with two or more symbols: a failed symbol consistency invalidates the record's PRBs (nothing else checks them) */
  if (sparse && q->sym_consist == q->sym_consist && q->sym_consist < NR_SENSE_SPARSE_SYM_MIN) {
    memset(q->valid_mask, 0, sizeof q->valid_mask);
    q->valid_cfr_prbs = 0;
    q->snr_db_p10 = q->snr_db_p50 = NAN;
  }
}

void nr_sense_quality_apply_recon(nr_sense_quality_t *q, const uint8_t recon_ok_abs[(NR_SENSE_MAX_PRB + 7) / 8])
{
  int n = 0;
  for (int p = 0; p < NR_SENSE_MAX_PRB; p++) {
    const int a = q->first_prb + p;
    const bool ok = a >= 0 && a < NR_SENSE_MAX_PRB && (recon_ok_abs[a >> 3] >> (a & 7) & 1);
    if (q->valid_mask[p >> 3] >> (p & 7) & 1) {
      if (ok)
        n++;
      else
        q->valid_mask[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
  }
  q->valid_cfr_prbs = (uint16_t)n;
}
