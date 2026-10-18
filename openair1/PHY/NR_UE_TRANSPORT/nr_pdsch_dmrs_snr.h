/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-grant DM-RS SNR from the chest's own output (EVMGRANT dmrs_snr= / dmrs_pw= / dmrs_ref=). Header-only, shared by the CPU chest, the GPU exact chest
 * and the GPU graph pass: every path hands over the same two things, the smoothed estimate row of one (DM-RS symbol, layer, antenna) in the grant and the
 * chest's noise nres = mean |LS - smoothed|^2 (nr_dl_chest_nvar_ant), so the SNR is the same function everywhere and needs no new GPU reduction.
 *
 * Model. The chest's LS value is the mean of two adjacent pilots, so its noise is N_ls = sigma^2 / 2 (sigma^2 = noise per RE). The FIR smoother (noise gain
 * g, own weight w) leaves a residual mean |LS - h|^2 = c * N_ls with c = 1 - 2w + g, and the smoothed estimate carries S + g * N_ls. Both ratios measured on the
 * real filters (nr_pdsch_dmrs_snr_test.cc, flat channel, interior of a 100 PRB grant): type 1 c = 0.72, g = 0.22; type 2 c = 0.59, g = 0.33. Then
 *   N_ls = nres / c,   S = mean|h|^2 - g * N_ls,   SNR = S / sigma^2 = S / (2 * N_ls)  (per RE, per antenna, before any combining).
 * S is clamped at 1 % of sigma^2 (-20 dB) so a grant with no signal reads strongly negative, not at a floor. Units of |h|^2 are the chest's (LS = rx * pilot, the
 * pilot has magnitude 2^14 / beta_dmrs / 2^15 of the rx scale; the caller rescales dmrs_pw). */
#ifndef NR_PDSCH_DMRS_SNR_H
#define NR_PDSCH_DMRS_SNR_H
#include <stdint.h>
#include <math.h>

typedef struct {
  float snr_db[4]; /* per RX antenna, -99 = no estimate */
  float comb_db;   /* MRC: 10 log10(sum of the linear per-antenna SNRs) */
  float pw_db;     /* mean RE power of the DM-RS in the grant on rx0 (S + sigma^2, rx scale), dB re LSB^2 */
  float ref_db;    /* mean RE power on the same symbol's carrier REs outside the grant on rx0, -99 = not available */
} nr_dmrs_snr_t;

static inline void nr_dmrs_snr_init(nr_dmrs_snr_t *s)
{
  for (int a = 0; a < 4; a++) s->snr_db[a] = -99.f;
  s->comb_db = s->pw_db = s->ref_db = -99.f;
}

/* h: interleaved int16 re,im of n REs. Returns the linear SNR (< 0 = no estimate); *pw_ls = S + sigma^2 in LS units (0 if none). */
static inline double nr_dmrs_snr_ant(const int16_t *h, int n, uint32_t nres, int dmrs_type, double *pw_ls)
{
  static const double c_res[3] = {1.0, 0.72, 0.59}, g_nz[3] = {0.0, 0.22, 0.33};
  *pw_ls = 0.0;
  if (!h || n <= 0 || nres == 0) return -1.0;
  const int t = dmrs_type == 2 ? 2 : 1;
  int64_t e = 0;
  for (int i = 0; i < 2 * n; i++) e += (int32_t)h[i] * h[i];
  const double nls = (double)nres / c_res[t];
  double s = (double)e / n - g_nz[t] * nls;
  if (s < 0.01 * 2.0 * nls) s = 0.01 * 2.0 * nls;
  *pw_ls = s + 2.0 * nls;
  return s / (2.0 * nls);
}

static inline float nr_dmrs_db(double lin) { return lin > 0.0 ? (float)(10.0 * log10(lin)) : -99.f; }

/* rows[a]: smoothed estimate of antenna a (NULL = none), nres[a]: nr_dl_chest_nvar_ant[a]. pw_ls_to_rx: LS-power -> rx-power factor. */
static inline void nr_dmrs_snr_fill(nr_dmrs_snr_t *s, const int16_t *const rows[4], int nrx, int n, const uint32_t nres[4], int dmrs_type, double pw_ls_to_rx)
{
  double sum = 0.0, pw0 = 0.0;
  for (int a = 0; a < nrx && a < 4; a++) {
    double pw;
    const double snr = nr_dmrs_snr_ant(rows[a], n, nres[a], dmrs_type, &pw);
    if (snr < 0.0) continue;
    s->snr_db[a] = nr_dmrs_db(snr);
    sum += snr;
    if (a == 0) pw0 = pw;
  }
  s->comb_db = nr_dmrs_db(sum);
  s->pw_db = nr_dmrs_db(pw0 * pw_ls_to_rx);
}

/* mean |rx|^2 (dB) over the carrier REs [car_k0, car_k0 + car_n) mod N that lie outside the grant [g_k0, g_k0 + g_n) mod N; -99 if the grant is not inside the
 * carrier or nothing is left. rx: interleaved int16 of one symbol (N REs). */
static inline float nr_dmrs_ref_db(const int16_t *rx, int N, int car_k0, int car_n, int g_k0, int g_n)
{
  if (!rx || car_n <= 0 || g_n <= 0) return -99.f;
  const int gs = ((g_k0 - car_k0) % N + N) % N;
  if (gs + g_n > car_n) return -99.f;
  int64_t e = 0;
  int cnt = 0;
  for (int pass = 0; pass < 2; pass++) {
    const int a = pass ? gs + g_n : 0, b = pass ? car_n : gs;
    int sc = (car_k0 + a) % N;
    for (int j = a; j < b; j++, cnt++) {
      e += (int64_t)rx[2 * sc] * rx[2 * sc] + (int64_t)rx[2 * sc + 1] * rx[2 * sc + 1];
      if (++sc == N) sc = 0;
    }
  }
  return cnt > 0 ? nr_dmrs_db((double)e / cnt) : -99.f;
}
#endif
