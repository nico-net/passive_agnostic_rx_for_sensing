/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* The arithmetic of nr_mmse_exact.h, instantiated once per precision: included with NR_MMSE_R (double | float), NR_MMSE_NM(x)
 * (x | x_f), NR_MMSE_SQRT and NR_MMSE_LROUND defined. No include guard on purpose. Constants are cast to NR_MMSE_R so that the
 * float instantiation never promotes to double (RTX 4070: FP64 at 1/64 rate). The double instantiation is the original text. */
NR_MMSE_FN int NR_MMSE_NM(nr_mmse_exact_core)(int nl, int nrx, const NR_MMSE_R hre[4][4], const NR_MMSE_R him[4][4], const NR_MMSE_R yre[4],
                                   const NR_MMSE_R yim[4], NR_MMSE_R sc, NR_MMSE_R s2, NR_MMSE_R xre[4], NR_MMSE_R xim[4], NR_MMSE_R ainv[4],
                                   NR_MMSE_R *out_res)
{
  NR_MMSE_R ar[4][4], ai[4][4], br[4], bi[4]; /* lower triangle of A (i >= j), b */
  for (int i = 0; i < nl; i++) {
    for (int j = 0; j <= i; j++) { /* A[i][j] = sum_a conj(h_i) h_j */
      NR_MMSE_R gr = ((NR_MMSE_R)0.0), gi = ((NR_MMSE_R)0.0);
      for (int a = 0; a < nrx; a++) {
        gr += hre[i][a] * hre[j][a] + him[i][a] * him[j][a];
        gi += hre[i][a] * him[j][a] - him[i][a] * hre[j][a];
      }
      ar[i][j] = gr * sc + (i == j ? s2 : ((NR_MMSE_R)0.0));
      ai[i][j] = gi * sc;
    }
    NR_MMSE_R ur = ((NR_MMSE_R)0.0), ui = ((NR_MMSE_R)0.0); /* conj(h_i) y */
    for (int a = 0; a < nrx; a++) {
      ur += hre[i][a] * yre[a] + him[i][a] * yim[a];
      ui += hre[i][a] * yim[a] - him[i][a] * yre[a];
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
      yy += yre[a] * yre[a] + yim[a] * yim[a];
    const NR_MMSE_R e = yy - uu / sc;
    *out_res = e > ((NR_MMSE_R)0.0) ? e : ((NR_MMSE_R)0.0);
  }
  return 1;
}


/* the int16 entry point (nr_mmse_exact.h's contract): the conversions are exact, so this is the original arithmetic */
NR_MMSE_FN int NR_MMSE_NM(nr_mmse_exact_re)(int nl, int nrx, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                   const int16_t yim[4], NR_MMSE_R sc, NR_MMSE_R s2, NR_MMSE_R xre[4], NR_MMSE_R xim[4], NR_MMSE_R ainv[4],
                                   NR_MMSE_R *out_res)
{
  NR_MMSE_R hr[4][4], hi[4][4], yr[4], yi[4];
  for (int a = 0; a < nrx; a++) {
    yr[a] = (NR_MMSE_R)yre[a];
    yi[a] = (NR_MMSE_R)yim[a];
    for (int i = 0; i < nl; i++) {
      hr[i][a] = (NR_MMSE_R)hre[i][a];
      hi[i][a] = (NR_MMSE_R)him[i][a];
    }
  }
  return NR_MMSE_NM(nr_mmse_exact_core)(nl, nrx, hr, hi, yr, yi, sc, s2, xre, xim, ainv, out_res);
}

NR_MMSE_HD int16_t NR_MMSE_NM(nr_mmse3_s16)(NR_MMSE_R v)
{
  return (int16_t)(v > ((NR_MMSE_R)32767.0) ? 32767 : v < ((NR_MMSE_R)-32768.0) ? -32768 : NR_MMSE_LROUND(v));
}

/* MMSE3GPU: mode-3 output gain from the strongest layer's energy sum_{i,a} |h|^2 over the symbol's length REs: its mean
 * G_ll (2^-shift units, sc) lands at 2^12 LSB (was 2^10). RANK34 2026-10-31: the unit amplitude of a layer is mu_l * gain, so a layer 20-25 dB under the strongest one
 * (4x4, RX correlation 0.95) had 2-4 LSB per constellation unit and the int16 rounding of z and of the thresholds was an SNR-independent error floor (4 layers 16QAM:
 * 28.4 dB for 10 % TB BLER against 24.1 dB for the unrounded MMSE; 2^12: 24.7 dB; 2^14 no better) [MEASURED DGX GB10 test, nr_linklevel_bench ml]. 2^12 leaves 8x headroom
 * over the strongest layer's mean for fades; no change within noise on i.i.d. Rayleigh and RX correlation 0.8. */
NR_MMSE_FN NR_MMSE_R NR_MMSE_NM(nr_mmse3_gain)(NR_MMSE_R emax, int length, NR_MMSE_R sc)
{
  const NR_MMSE_R gd = length > 0 ? emax * sc / length : ((NR_MMSE_R)0.0);
  return gd > ((NR_MMSE_R)0.0) ? ((NR_MMSE_R)4096.0) / gd : ((NR_MMSE_R)1.0);
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


/* ---- D4 IRC (2026-10-31): spatial whitening of the DM-RS interference + noise covariance, in front of the mode-3 MMSE -------------------------------------------
 * Per RB group g (NR_IRC_GRP_PRB PRBs of the grant) the covariance of the DM-RS least-squares residual across the RX branches, S = sum e e^H / n (nr_irc_sum_t, exact
 * int64 sums: the CPU chest and the GPU kernel add the same integers in any order), carries the spatial structure of interference + noise. The scale of S is not used
 * (the LS residual is a smoothed, pair-averaged quantity, ~20 dB under the data-domain noise, MMSE3CAL); only its shape, trace-normalised to nb. Shrinkage to the scalar
 * model follows Ledoit-Wolf with the closed-form estimation error of a complex Gaussian sample covariance, E||S - R||_F^2 = (tr R)^2 / n_eff:
 *   beta = min(1, (tr S)^2 / n_eff / ||S - mu I||_F^2),  R~ = (1 - beta) S + beta mu I
 * so white noise (S - mu I is estimation error only) gives beta ~ 1 and W ~ I, and a real interferer (||S - mu I|| >> its error) gives beta ~ 0. n_eff = n / 2: the LS values
 * of neighbouring pilot pairs share the FIR fit. A diagonal load eps bounds the amplification of quiet directions. W = scale L^-1 with R~ + eps I = L L^H, scale <= 1 so that
 * the whitened h never exceeds the raw one (the output gain of nr_mmse3_gain keeps its meaning and the int16 outputs their headroom). The model R = mu R~ gives the
 * whitened noise per dimension mu * noise_fac; the nvar-based s2 of a square channel (nrx == nl) is multiplied by it. Ill-conditioned (pivot spread above 400, a non-finite
 * pivot) or too few samples: identity, i.e. the scalar model unchanged. w[group * NR_IRC_WSTRIDE]: wr[10], wi[10] (lower triangle, row-major a (a + 1) / 2 + k), w[20] = 1 when
 * active, w[21] = noise_fac, w[22] = whitening gain estimate (linear). */
NR_MMSE_FN_INL void NR_MMSE_NM(nr_irc_final)(const nr_irc_sum_t *s, int nb, NR_MMSE_R *w)
{
  for (int t = 0; t < NR_IRC_WSTRIDE; t++)
    w[t] = ((NR_MMSE_R)0.0);
  for (int a = 0; a < 4; a++)
    w[NR_IRC_TRI(a, a)] = ((NR_MMSE_R)1.0);
  w[21] = ((NR_MMSE_R)1.0);
  w[22] = ((NR_MMSE_R)1.0);
  if (nb < 2 || nb > 4 || s->n < 2 * nb)
    return;
  const NR_MMSE_R n = (NR_MMSE_R)s->n;
  NR_MMSE_R sr[4][4], si[4][4], tr = ((NR_MMSE_R)0.0);
  for (int a = 0; a < nb; a++)
    for (int b = 0; b < nb; b++) {
      sr[a][b] = (NR_MMSE_R)s->re[a][b] / n;
      si[a][b] = (NR_MMSE_R)s->im[a][b] / n;
    }
  for (int a = 0; a < nb; a++)
    tr += sr[a][a];
  const NR_MMSE_R mu = tr / (NR_MMSE_R)nb;
  if (!(mu > ((NR_MMSE_R)0.0)))
    return;
  NR_MMSE_R d2 = ((NR_MMSE_R)0.0);
  for (int a = 0; a < nb; a++)
    for (int b = 0; b < nb; b++) {
      const NR_MMSE_R dr = sr[a][b] - (a == b ? mu : ((NR_MMSE_R)0.0));
      d2 += dr * dr + si[a][b] * si[a][b];
    }
  const NR_MMSE_R v = ((NR_MMSE_R)2.0) * tr * tr / n; /* (tr S)^2 / n_eff, n_eff = n / 2 */
  const NR_MMSE_R beta = d2 > v ? v / d2 : ((NR_MMSE_R)1.0);
  const NR_MMSE_R eps = ((NR_MMSE_R)0.05);
  NR_MMSE_R rr[4][4], ri[4][4]; /* R~ / mu + eps I */
  for (int a = 0; a < nb; a++)
    for (int b = 0; b < nb; b++) {
      rr[a][b] = (((NR_MMSE_R)1.0) - beta) * sr[a][b] / mu + (a == b ? beta + eps : ((NR_MMSE_R)0.0));
      ri[a][b] = (((NR_MMSE_R)1.0) - beta) * si[a][b] / mu;
    }
  NR_MMSE_R lr[4][4], li[4][4], dg[4], dmax = ((NR_MMSE_R)0.0), dmin = ((NR_MMSE_R)1e30);
  for (int j = 0; j < nb; j++) {
    NR_MMSE_R t = rr[j][j];
    for (int k = 0; k < j; k++)
      t -= lr[j][k] * lr[j][k] + li[j][k] * li[j][k];
    if (!(t > ((NR_MMSE_R)1e-6)))
      return;
    dg[j] = NR_MMSE_SQRT(t);
    dmax = t > dmax ? t : dmax;
    dmin = t < dmin ? t : dmin;
    for (int i = j + 1; i < nb; i++) { /* (R[i][j] - sum_k L[i][k] conj(L[j][k])) / d_j */
      NR_MMSE_R ur = rr[i][j], ui = ri[i][j];
      for (int k = 0; k < j; k++) {
        ur -= lr[i][k] * lr[j][k] + li[i][k] * li[j][k];
        ui -= li[i][k] * lr[j][k] - lr[i][k] * li[j][k];
      }
      lr[i][j] = ur / dg[j];
      li[i][j] = ui / dg[j];
    }
  }
  if (dmax > ((NR_MMSE_R)400.0) * dmin)
    return;
  /* W0 = L^-1 (lower): W0[i][i] = 1 / d_i, W0[i][j] = -(sum_{k = j..i-1} L[i][k] W0[k][j]) / d_i */
  NR_MMSE_R wr0[4][4], wi0[4][4], fro = ((NR_MMSE_R)0.0);
  for (int j = 0; j < nb; j++) {
    wr0[j][j] = ((NR_MMSE_R)1.0) / dg[j];
    wi0[j][j] = ((NR_MMSE_R)0.0);
    fro += wr0[j][j] * wr0[j][j];
    for (int i = j + 1; i < nb; i++) {
      NR_MMSE_R ur = ((NR_MMSE_R)0.0), ui = ((NR_MMSE_R)0.0);
      for (int k = j; k < i; k++) {
        ur += lr[i][k] * wr0[k][j] - li[i][k] * wi0[k][j];
        ui += lr[i][k] * wi0[k][j] + li[i][k] * wr0[k][j];
      }
      wr0[i][j] = -ur / dg[i];
      wi0[i][j] = -ui / dg[i];
      fro += wr0[i][j] * wr0[i][j] + wi0[i][j] * wi0[i][j];
    }
  }
  const NR_MMSE_R rms2 = fro / (NR_MMSE_R)nb; /* tr(R^-1) / nb */
  const NR_MMSE_R scale = rms2 > ((NR_MMSE_R)1.0) ? ((NR_MMSE_R)1.0) / NR_MMSE_SQRT(rms2) : ((NR_MMSE_R)1.0);
  for (int a = 0; a < nb; a++)
    for (int k = 0; k <= a; k++) {
      w[NR_IRC_TRI(a, k)] = wr0[a][k] * scale;
      w[10 + NR_IRC_TRI(a, k)] = wi0[a][k] * scale;
    }
  for (int a = nb; a < 4; a++) /* unused rows stay identity so a caller may run all four */
    w[NR_IRC_TRI(a, a)] = ((NR_MMSE_R)1.0);
  w[20] = ((NR_MMSE_R)1.0);
  const NR_MMSE_R nf = ((NR_MMSE_R)1.0) - eps * rms2;
  w[21] = scale * scale * (nf > ((NR_MMSE_R)0.5) ? nf : ((NR_MMSE_R)0.5));
  w[22] = rms2 * (((NR_MMSE_R)1.0) + eps);
}

/* y' = W y, h' = W h for one RE (nl layers, nrx branches) into the solver's arithmetic type; w == NULL or inactive: the conversion only */
NR_MMSE_FN void NR_MMSE_NM(nr_irc_whiten_re)(int nl, int nrx, const NR_MMSE_R *w, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                             const int16_t yim[4], NR_MMSE_R ohr[4][4], NR_MMSE_R ohi[4][4], NR_MMSE_R oyr[4], NR_MMSE_R oyi[4])
{
  if (!w || !(w[20] > ((NR_MMSE_R)0.5))) {
    for (int a = 0; a < nrx; a++) {
      oyr[a] = (NR_MMSE_R)yre[a];
      oyi[a] = (NR_MMSE_R)yim[a];
      for (int l = 0; l < nl; l++) {
        ohr[l][a] = (NR_MMSE_R)hre[l][a];
        ohi[l][a] = (NR_MMSE_R)him[l][a];
      }
    }
    return;
  }
  for (int a = 0; a < nrx; a++) {
    NR_MMSE_R yr = ((NR_MMSE_R)0.0), yi = ((NR_MMSE_R)0.0);
    NR_MMSE_R hr[4], hi[4];
    for (int l = 0; l < nl; l++)
      hr[l] = hi[l] = ((NR_MMSE_R)0.0);
    for (int k = 0; k <= a; k++) {
      const NR_MMSE_R cr = w[NR_IRC_TRI(a, k)], ci = w[10 + NR_IRC_TRI(a, k)];
      yr += cr * (NR_MMSE_R)yre[k] - ci * (NR_MMSE_R)yim[k];
      yi += cr * (NR_MMSE_R)yim[k] + ci * (NR_MMSE_R)yre[k];
      for (int l = 0; l < nl; l++) {
        hr[l] += cr * (NR_MMSE_R)hre[l][k] - ci * (NR_MMSE_R)him[l][k];
        hi[l] += cr * (NR_MMSE_R)him[l][k] + ci * (NR_MMSE_R)hre[l][k];
      }
    }
    oyr[a] = yr;
    oyi[a] = yi;
    for (int l = 0; l < nl; l++) {
      ohr[l][a] = hr[l];
      ohi[l][a] = hi[l];
    }
  }
}

/* the whitened ZF residual ||y' - H' x||^2 (decision-free noise energy in the nrx - nl dimensional complement) of one RE; 0 when singular */
NR_MMSE_FN int NR_MMSE_NM(nr_mmse_exact_re_w)(int nl, int nrx, const NR_MMSE_R *w, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                              const int16_t yim[4], NR_MMSE_R sc, NR_MMSE_R s2, NR_MMSE_R xre[4], NR_MMSE_R xim[4], NR_MMSE_R ainv[4],
                                              NR_MMSE_R *out_res)
{
  NR_MMSE_R hr[4][4], hi[4][4], yr[4], yi[4];
  NR_MMSE_NM(nr_irc_whiten_re)(nl, nrx, w, hre, him, yre, yim, hr, hi, yr, yi);
  return NR_MMSE_NM(nr_mmse_exact_core)(nl, nrx, hr, hi, yr, yi, sc, s2, xre, xim, ainv, out_res);
}

/* nr_mmse3_re on the whitened RE (w == NULL: exactly nr_mmse3_re) */
NR_MMSE_FN void NR_MMSE_NM(nr_mmse3_re_w)(int nl, int nrx, const NR_MMSE_R *w, const int16_t hre[4][4], const int16_t him[4][4], const int16_t yre[4],
                                          const int16_t yim[4], NR_MMSE_R sc, NR_MMSE_R s2, NR_MMSE_R gain, int16_t qa, int16_t qb, int16_t qr,
                                          int16_t z[4][2], int16_t mag[4][3])
{
  NR_MMSE_R xre[4], xim[4], ainv[4];
  const int ok = NR_MMSE_NM(nr_mmse_exact_re_w)(nl, nrx, w, hre, him, yre, yim, sc, s2, xre, xim, ainv, 0);
  for (int r = 0; r < nl; r++) {
    const NR_MMSE_R ai = ok ? ainv[r] : ((NR_MMSE_R)0.0);
    const NR_MMSE_R wl = ai > ((NR_MMSE_R)1e-12) ? (((NR_MMSE_R)1.0) - s2 * ai) / ai * gain : ((NR_MMSE_R)0.0);
    z[r][0] = NR_MMSE_NM(nr_mmse3_s16)(ai > ((NR_MMSE_R)1e-12) ? xre[r] / ai * gain : ((NR_MMSE_R)0.0));
    z[r][1] = NR_MMSE_NM(nr_mmse3_s16)(ai > ((NR_MMSE_R)1e-12) ? xim[r] / ai * gain : ((NR_MMSE_R)0.0));
    mag[r][0] = NR_MMSE_NM(nr_mmse3_s16)(wl * qa / ((NR_MMSE_R)32768.0));
    mag[r][1] = NR_MMSE_NM(nr_mmse3_s16)(wl * qb / ((NR_MMSE_R)32768.0));
    mag[r][2] = NR_MMSE_NM(nr_mmse3_s16)(wl * qr / ((NR_MMSE_R)32768.0));
  }
}
