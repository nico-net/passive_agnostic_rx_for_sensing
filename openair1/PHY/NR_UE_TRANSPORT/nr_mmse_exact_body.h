/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* The arithmetic of nr_mmse_exact.h, instantiated once per precision: included with NR_MMSE_R (double | float), NR_MMSE_NM(x)
 * (x | x_f), NR_MMSE_SQRT and NR_MMSE_LROUND defined. No include guard on purpose. Constants are cast to NR_MMSE_R so that the
 * float instantiation never promotes to double (RTX 4070: FP64 at 1/64 rate). The double instantiation is the original text. */
NR_MMSE_FN int NR_MMSE_NM(nr_mmse_exact_re)(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                   const int16_t yim[4], NR_MMSE_R sc, NR_MMSE_R s2, NR_MMSE_R xre[4], NR_MMSE_R xim[4], NR_MMSE_R ainv[4],
                                   NR_MMSE_R *out_res)
{
  NR_MMSE_R ar[4][4], ai[4][4], br[4], bi[4]; /* lower triangle of A (i >= j), b */
  for (int i = 0; i < nl; i++) {
    for (int j = 0; j <= i; j++) { /* A[i][j] = sum_a conj(h_i) h_j */
      NR_MMSE_R gr = ((NR_MMSE_R)0.0), gi = ((NR_MMSE_R)0.0);
      for (int a = 0; a < nrx; a++) {
        gr += (NR_MMSE_R)hre[i][a] * hre[j][a] + (NR_MMSE_R)him[i][a] * him[j][a];
        gi += (NR_MMSE_R)hre[i][a] * him[j][a] - (NR_MMSE_R)him[i][a] * hre[j][a];
      }
      ar[i][j] = gr * sc + (i == j ? s2 : ((NR_MMSE_R)0.0));
      ai[i][j] = gi * sc;
    }
    NR_MMSE_R ur = ((NR_MMSE_R)0.0), ui = ((NR_MMSE_R)0.0); /* conj(h_i) y */
    for (int a = 0; a < nrx; a++) {
      ur += (NR_MMSE_R)hre[i][a] * yre[a] + (NR_MMSE_R)him[i][a] * yim[a];
      ui += (NR_MMSE_R)hre[i][a] * yim[a] - (NR_MMSE_R)him[i][a] * yre[a];
    }
    br[i] = ur * sc;
    bi[i] = ui * sc;
  }
  /* Cholesky: L[i][j] (i > j), real diagonal d[j] */
  NR_MMSE_R d[4], lr[4][4], li[4][4];
  for (int j = 0; j < nl; j++) {
    NR_MMSE_R s = ar[j][j];
    for (int k = 0; k < j; k++)
      s -= lr[j][k] * lr[j][k] + li[j][k] * li[j][k];
    if (!(s > ((NR_MMSE_R)1e-12) * (ar[j][j] > ((NR_MMSE_R)1.0) ? ar[j][j] : ((NR_MMSE_R)1.0))))
      return 0;
    d[j] = NR_MMSE_SQRT(s);
    for (int i = j + 1; i < nl; i++) { /* (A[i][j] - sum_k L[i][k] conj(L[j][k])) / d_j */
      NR_MMSE_R tr = ar[i][j], ti = ai[i][j];
      for (int k = 0; k < j; k++) {
        tr -= lr[i][k] * lr[j][k] + li[i][k] * li[j][k];
        ti -= li[i][k] * lr[j][k] - lr[i][k] * li[j][k];
      }
      lr[i][j] = tr / d[j];
      li[i][j] = ti / d[j];
    }
  }
  /* L u = b */
  NR_MMSE_R ur[4], ui[4], uu = ((NR_MMSE_R)0.0);
  for (int i = 0; i < nl; i++) {
    NR_MMSE_R tr = br[i], ti = bi[i];
    for (int k = 0; k < i; k++) {
      tr -= lr[i][k] * ur[k] - li[i][k] * ui[k];
      ti -= lr[i][k] * ui[k] + li[i][k] * ur[k];
    }
    ur[i] = tr / d[i];
    ui[i] = ti / d[i];
    uu += ur[i] * ur[i] + ui[i] * ui[i];
  }
  /* L^H x = u */
  for (int i = nl - 1; i >= 0; i--) {
    NR_MMSE_R tr = ur[i], ti = ui[i];
    for (int k = i + 1; k < nl; k++) { /* conj(L[k][i]) x_k */
      tr -= lr[k][i] * xre[k] + li[k][i] * xim[k];
      ti -= lr[k][i] * xim[k] - li[k][i] * xre[k];
    }
    xre[i] = tr / d[i];
    xim[i] = ti / d[i];
  }
  /* M = L^-1 (lower): [A^-1]_ll = sum_{k >= l} |M[k][l]|^2 */
  for (int l = 0; l < nl; l++) {
    NR_MMSE_R mr[4], mi[4];
    mr[l] = ((NR_MMSE_R)1.0) / d[l];
    mi[l] = ((NR_MMSE_R)0.0);
    NR_MMSE_R n = mr[l] * mr[l];
    for (int i = l + 1; i < nl; i++) {
      NR_MMSE_R tr = ((NR_MMSE_R)0.0), ti = ((NR_MMSE_R)0.0);
      for (int k = l; k < i; k++) {
        tr += lr[i][k] * mr[k] - li[i][k] * mi[k];
        ti += lr[i][k] * mi[k] + li[i][k] * mr[k];
      }
      mr[i] = -tr / d[i];
      mi[i] = -ti / d[i];
      n += mr[i] * mr[i] + mi[i] * mi[i];
    }
    ainv[l] = n;
  }
  if (out_res) {
    NR_MMSE_R yy = ((NR_MMSE_R)0.0);
    for (int a = 0; a < nrx; a++)
      yy += (NR_MMSE_R)yre[a] * yre[a] + (NR_MMSE_R)yim[a] * yim[a];
    const NR_MMSE_R e = yy - uu / sc;
    *out_res = e > ((NR_MMSE_R)0.0) ? e : ((NR_MMSE_R)0.0);
  }
  return 1;
}

NR_MMSE_HD int16_t NR_MMSE_NM(nr_mmse3_s16)(NR_MMSE_R v)
{
  return (int16_t)(v > ((NR_MMSE_R)32767.0) ? 32767 : v < ((NR_MMSE_R)-32768.0) ? -32768 : NR_MMSE_LROUND(v));
}

/* MMSE3GPU: mode-3 output gain from the strongest layer's energy sum_{i,a} |h|^2 over the symbol's length REs: its mean
 * G_ll (2^-shift units, sc) lands at 2^10 LSB */
NR_MMSE_FN NR_MMSE_R NR_MMSE_NM(nr_mmse3_gain)(NR_MMSE_R emax, int length, NR_MMSE_R sc)
{
  const NR_MMSE_R gd = length > 0 ? emax * sc / length : ((NR_MMSE_R)0.0);
  return gd > ((NR_MMSE_R)0.0) ? ((NR_MMSE_R)1024.0) / gd : ((NR_MMSE_R)1.0);
}

/* MMSE3GPU: mode-3 output of one RE: z_l = x_l / [A^-1]_ll * gain and the per-layer thresholds w_l q / 32768,
 * w_l = (1 - s2 [A^-1]_ll) / [A^-1]_ll * gain (mu / [A^-1]_ll); a singular A or a vanishing [A^-1]_ll gives 0.
 * z[l] = {re, im}, mag[l] = {qa, qb, qr} thresholds. */
NR_MMSE_FN void NR_MMSE_NM(nr_mmse3_re)(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4], const int16_t yim[4],
                            NR_MMSE_R sc, NR_MMSE_R s2, NR_MMSE_R gain, int16_t qa, int16_t qb, int16_t qr, int16_t z[4][2], int16_t mag[4][3])
{
  NR_MMSE_R xre[4], xim[4], ainv[4];
  const int ok = NR_MMSE_NM(nr_mmse_exact_re)(nl, nrx, hre, him, yre, yim, sc, s2, xre, xim, ainv, 0);
  for (int r = 0; r < nl; r++) {
    const NR_MMSE_R ai = ok ? ainv[r] : ((NR_MMSE_R)0.0);
    const NR_MMSE_R w = ai > ((NR_MMSE_R)1e-12) ? (((NR_MMSE_R)1.0) - s2 * ai) / ai * gain : ((NR_MMSE_R)0.0);
    z[r][0] = NR_MMSE_NM(nr_mmse3_s16)(ai > ((NR_MMSE_R)1e-12) ? xre[r] / ai * gain : ((NR_MMSE_R)0.0));
    z[r][1] = NR_MMSE_NM(nr_mmse3_s16)(ai > ((NR_MMSE_R)1e-12) ? xim[r] / ai * gain : ((NR_MMSE_R)0.0));
    mag[r][0] = NR_MMSE_NM(nr_mmse3_s16)(w * qa / ((NR_MMSE_R)32768.0));
    mag[r][1] = NR_MMSE_NM(nr_mmse3_s16)(w * qb / ((NR_MMSE_R)32768.0));
    mag[r][2] = NR_MMSE_NM(nr_mmse3_s16)(w * qr / ((NR_MMSE_R)32768.0));
  }
}

