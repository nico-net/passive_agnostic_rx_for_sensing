/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPU-DEMOD host side: ISAC_PDSCH_GPU_EXACT_DEMOD=1 (default off) runs nr_rx_pdsch()'s demodulation of a whole passive
 * PDSCH grant on the GPU (libpdsch_gpu.so, nr_pdsch_gpu_demod.cu), bit for bit.
 *   - Geometry: nr_dlsch_extract_geometry() runs the real nr_dlsch_extract_rbs() on index-coded inputs, so every
 *     DM-RS / CSI-RS / SSB / erase / rate-matching rule of the CPU extractor is the GPU's input, not a re-implementation.
 *   - Decision, mask, ISAC_RX_MRC_ALIGN, compensation, LLR and layer demapping: the GPU (integer models checked
 *     against the CPU functions by test_nr_pdsch_gpu_demod; the alignment follows GCC's FMA pattern).
 *   - ISAC_PDSCH_EVM_GRANT accumulation: the CPU, with a verbatim copy of nr_rx_pdsch()'s block, on the GPU's
 *     extraction buffers (same inputs, same compiler, same arithmetic).
 * Anything outside the supported subset (PT-RS, Nl >= 2, IRC, QAM AGC, forced branches, V2, ML, wide MRC, decode
 * diagnostics, SFO correction, L2MAXH peak/headroom ...) returns -1 and the caller runs the CPU loop unchanged.
 * ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK=1 runs the CPU loop on every grant as well (N > 1: every N-th grant), compares
 * every output (log2_maxh, dl_valid_re, LLRs, rxdataF_comp, mags, branch, the EVM accumulator) and disables the GPU
 * path on the first difference; the CPU results are the ones used. */
#include "nr_pdsch_gpu_demod_host.h"
#include "nr_pdsch_gpu_demod.h"
#include "nr_pdsch_evm_acc.h"
#include "nr_agnostic_v2.h"
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h"
#include "common/utils/LOG/log.h"
#include <complex.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_cpu_route.h" /* BATCHCPU */

uint32_t nr_dlsch_extract_geometry(const NR_DL_FRAME_PARMS *fp, int chest_time, c16_t *rx_idx, uint32_t est_sz, int32_t *est_idx,
                                   uint32_t ext_sz, c16_t *out_y, int32_t *out_h, unsigned char symbol,
                                   fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg, const freq_alloc_bitmap_t *fa, const nr_ssb_rm_mask_t *ssb_rm);
extern __thread int nr_dlsch_forced_branch, nr_dlsch_forced_mask, nr_dlsch_mrc_mode_override, nr_dlsch_irc_override,
    nr_dlsch_eq_override, nr_dlsch_qam_mode_override, nr_dlsch_used_branch, nr_dlsch_chest_per_symbol;
extern __thread bool nr_pdsch_decode_wide_mrc;

static nr_gpu_demod_init_fn g_init;
static nr_gpu_demod_run_fn g_run;
static nr_gpu_demod_release_fn g_release;
static nr_gpu_demod_evm_wait_fn g_evm_wait;
static int g_defer; /* ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_DEFER */
/* DECGPU EVM deferral: the job whose EVM chain is still running (context held), per consumer thread, lazily on the heap */
typedef struct {
  nr_gpu_demod_job_t j;
  int pending, nl, qm, nbrx, start, last;
} defer_t;
static __thread defer_t *t_def;
static __thread int t_defer_ok;
static _Atomic int g_on;
static int g_check;
static _Atomic uint64_t g_calls, g_chk_ok, g_chk_bad, g_ns_sum, g_ns_max, g_ns_gpu, g_ns_geo; /* geo: host geometry (index-coded extractor) */
static _Atomic uint64_t g_refuse[16];
static _Atomic uint64_t g_evm_close, g_evm_far, g_evm_maxrel; /* CHECK with the GPU EVM (rel. err of err x 1e18) */
/* GPUFUSE diagnostic ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES=1 (default off): per (Nl, Qm, RB class) count, wall and GPU ns */
static int g_shapes = -1;
static _Atomic uint64_t g_shape[4][4][4][3];
static int rb_class(int nrb) { return nrb <= 52 ? 0 : nrb <= 106 ? 1 : nrb <= 200 ? 2 : 3; }

static uint64_t now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int env_int(const char *n, int dflt)
{
  const char *e = getenv(n);
  return e ? atoi(e) : dflt;
}

int nr_pdsch_gpu_demod_warmup(const NR_DL_FRAME_PARMS *fp)
{
  if (env_int("ISAC_PDSCH_GPU_EXACT_DEMOD", 0) != 1 || !fp)
    return 0;
  if (fp->nb_antennas_rx > NR_GPU_DEMOD_MAX_RX || fp->symbols_per_slot != 14
      || ((fp->N_RB_DL * 12 + 15) & ~15) > NR_GPU_DEMOD_MAX_EXT || fp->ofdm_symbol_size > 32768) {
    LOG_W(PHY, "SENSING: GPU_EXACT_DEMOD needs <= 4 RX, 14 symbols, <= 275 PRB -- CPU demodulation\n");
    return 0;
  }
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  g_init = h ? (nr_gpu_demod_init_fn)dlsym(h, "nr_pdsch_gpu_demod_init") : NULL;
  g_run = h ? (nr_gpu_demod_run_fn)dlsym(h, "nr_pdsch_gpu_demod_run") : NULL;
  g_release = h ? (nr_gpu_demod_release_fn)dlsym(h, "nr_pdsch_gpu_demod_release") : NULL;
  g_evm_wait = h ? (nr_gpu_demod_evm_wait_fn)dlsym(h, "nr_pdsch_gpu_demod_evm_wait") : NULL;
  g_defer = env_int("ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_DEFER", 0) && g_evm_wait;
  if (!g_init || !g_run || !g_release || g_init(fp->nb_antennas_rx) != 0) {
    LOG_W(PHY, "SENSING: GPU_EXACT_DEMOD requested but libpdsch_gpu.so has no usable demod entry -- CPU demodulation\n");
    return 0;
  }
  g_check = env_int("ISAC_PDSCH_GPU_EXACT_DEMOD_CHECK", 0);
  atomic_store(&g_on, 1);
  LOG_A(PHY, "SENSING: GPU_EXACT_DEMOD enabled: %d RX, bit-exact nr_rx_pdsch() on the GPU; check=%d\n", fp->nb_antennas_rx, g_check);
  return 1;
}

/* ---- verbatim copy of nr_rx_pdsch()'s ISAC_PDSCH_EVM_GRANT block (nr_dlsch_demodulation.c), run per symbol on the
 * GPU's extraction buffers. decode_diag is refused upstream, so decode_ref_sign stays NULL. Keep in sync: the
 * CHECK mode compares the whole accumulator with the CPU's after every checked grant. */
static void evm_symbol(const NR_UE_DLSCH_t *dlsch, int nl, int nbRx, uint32_t nb_re_pdsch, uint32_t rx_size_symbol, uint32_t stride,
                       int mrc_rx_index, int mrc_nb_rx, unsigned char symbol, c16_t (*rxdataF_ext)[stride],
                       c16_t (*chFext)[nbRx][stride])
{
  const int decode_diag = 0;
  int8_t *decode_ref_sign = NULL;
  if (nr_pdsch_evm_acc.armed && nl >= 1 && nl <= 4 && nb_re_pdsch >= 12 && nb_re_pdsch <= rx_size_symbol) {
    nr_pdsch_evm_acc_t *ea = &nr_pdsch_evm_acc;
    const int qm = dlsch->cw_info.qamModOrder;
    const int lmax = (1 << (qm / 2)) - 1;
    double ms = 0.0;
    int nlev = 0;
    for (int l = 1; l <= lmax; l += 2) { ms += (double)l * l; nlev++; }
    const double ideal_pow = 2.0 * ms / nlev;
    const int nb = nbRx < NR_PDSCH_EVM_MAX_RX ? nbRx : NR_PDSCH_EVM_MAX_RX;
    const int a0 = nl == 1 ? mrc_rx_index : 0, a1 = nl == 1 ? mrc_rx_index + mrc_nb_rx : nbRx;
    double (*xr)[rx_size_symbol] = malloc(sizeof(double) * 2 * nl * rx_size_symbol);
    double (*xi)[rx_size_symbol] = xr ? (void *)((double *)xr + nl * rx_size_symbol) : NULL;
    double pw[4] = {0};
    uint32_t nv = 0;
    for (uint32_t i = 0; xr && i < nb_re_pdsch; i++) {
      /* G = H^H H (nl x nl), b = H^H y; solve G x = b by Gaussian elimination (complex, nl <= 4) */
      double complex G[4][5];
      for (int l = 0; l < nl; l++) {
        for (int k = 0; k < nl; k++) G[l][k] = 0;
        G[l][nl] = 0;
      }
      for (int a = a0; a < a1; a++) {
        const c16_t y = rxdataF_ext[a][i];
        const double complex yc = y.r + I * y.i;
        double complex h[4];
        for (int l = 0; l < nl; l++) { const c16_t hh = chFext[l][a][i]; h[l] = hh.r + I * hh.i; }
        for (int l = 0; l < nl; l++) {
          for (int k = 0; k < nl; k++) G[l][k] += conj(h[l]) * h[k];
          G[l][nl] += conj(h[l]) * yc;
        }
      }
      bool ok = true;
      if (nr_dlsch_csi_erase_n > 0) { /* CSI-FULL: an erased RE (all antennas zeroed) is not EVM evidence */
        bool z = true;
        for (int a = a0; a < a1 && z; a++) z = rxdataF_ext[a][i].r == 0 && rxdataF_ext[a][i].i == 0;
        ok = !z;
      }
      for (int c = 0; c < nl && ok; c++) {
        int piv = c;
        for (int r = c + 1; r < nl; r++) if (cabs(G[r][c]) > cabs(G[piv][c])) piv = r;
        if (cabs(G[piv][c]) <= 1e-9) { ok = false; break; }
        if (piv != c) for (int k = 0; k <= nl; k++) { const double complex t = G[c][k]; G[c][k] = G[piv][k]; G[piv][k] = t; }
        for (int r = 0; r < nl; r++) {
          if (r == c) continue;
          const double complex f = G[r][c] / G[c][c];
          for (int k = c; k <= nl; k++) G[r][k] -= f * G[c][k];
        }
      }
      for (int l = 0; l < nl; l++) {
        if (!ok) { xr[l][i] = xi[l][i] = NAN; continue; }
        const double complex x = G[l][nl] / G[l][l];
        xr[l][i] = creal(x); xi[l][i] = cimag(x);
        pw[l] += xr[l][i] * xr[l][i] + xi[l][i] * xi[l][i];
      }
      nv += ok;
    }
    bool good = xr && nv >= 12;
    for (int l = 0; l < nl && good; l++) good = pw[l] > 0.0;
    if (good) {
      if (decode_diag && qm == 2)
        decode_ref_sign = calloc((size_t)nl * nb_re_pdsch * 2, sizeof(*decode_ref_sign));
      double scale[4];
      for (int l = 0; l < nl; l++) scale[l] = sqrt(ideal_pow / (pw[l] / nv));
      double cr[NR_PDSCH_EVM_MAX_RX] = {0}, ci[NR_PDSCH_EVM_MAX_RX] = {0}, P[NR_PDSCH_EVM_MAX_RX] = {0}, Y[NR_PDSCH_EVM_MAX_RX] = {0};
      double qr[4] = {0}, qi[4] = {0}, qd[4] = {0}, qx[4] = {0}; /* per-layer LS fit x = beta d (CPE diagnostic) */
      for (uint32_t i = 0; i < nb_re_pdsch; i++) {
        if (isnan(xr[0][i])) continue;
        double sr[4], sq_[4];
        for (int l = 0; l < nl; l++) {
          const double vi = xr[l][i] * scale[l], vq = xi[l][i] * scale[l];
          if (decode_ref_sign) {
            decode_ref_sign[2 * (l * nb_re_pdsch + i)] = vi < 0 ? -1 : 1;
            decode_ref_sign[2 * (l * nb_re_pdsch + i) + 1] = vq < 0 ? -1 : 1;
          }
          double si = 2.0 * floor(vi / 2.0) + 1.0, sq = 2.0 * floor(vq / 2.0) + 1.0;
          si = si > lmax ? lmax : si < -lmax ? -lmax : si;
          sq = sq > lmax ? lmax : sq < -lmax ? -lmax : sq;
          ea->err += (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
          ea->ref += ideal_pow;
          ea->err_layer[l] += (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
          ea->ref_layer[l] += ideal_pow;
          if (symbol < 14) {
            const double e2 = (vi - si) * (vi - si) + (vq - sq) * (vq - sq);
            ea->err_sym[symbol] += e2;
            ea->ref_sym[symbol] += ideal_pow;
            ea->err_k[symbol][i % 12] += (float)(e2 / ideal_pow);
            ea->n_k[symbol][i % 12]++;
            if ((symbol == 0 || symbol == 1 || symbol == 3) && l == 0 && i / 12 < 276) { /* DECODE-RES per-RB map */
              const int row = symbol == 0 ? 0 : symbol == 1 ? 1 : 2;
              ea->err_rb[row][i / 12] += (float)(e2 / ideal_pow);
              ea->n_rb[row][i / 12]++;
            }
          }
          qr[l] += vi * si + vq * sq; qi[l] += vq * si - vi * sq; qd[l] += si * si + sq * sq; qx[l] += vi * vi + vq * vq;
          sr[l] = si / scale[l]; sq_[l] = sq / scale[l]; /* back in the channel's own units: one alpha per branch */
        }
        for (int a = 0; a < nb; a++) {
          const c16_t y = rxdataF_ext[a][i];
          double hsr = 0.0, hsi = 0.0;
          for (int l = 0; l < nl; l++) {
            const c16_t h = chFext[l][a][i];
            hsr += h.r * sr[l] - h.i * sq_[l];
            hsi += h.r * sq_[l] + h.i * sr[l];
          }
          cr[a] += y.r * hsr + y.i * hsi;
          ci[a] += y.i * hsr - y.r * hsi;
          P[a] += hsr * hsr + hsi * hsi;
          Y[a] += (double)y.r * y.r + (double)y.i * y.i;
        }
      }
      for (int l = 0; l < nl; l++) /* residual after the best per-symbol complex scale: |x|^2 - |<x,d>|^2/|d|^2 */
        if (qd[l] > 0.0) {
          const double r = qx[l] - (qr[l] * qr[l] + qi[l] * qi[l]) / qd[l];
          ea->err_cpe += r > 0.0 ? r : 0.0;
        }
      for (int a = 0; a < nb; a++) {
        if (P[a] <= 0.0) continue;
        const double sig = (cr[a] * cr[a] + ci[a] * ci[a]) / P[a];
        ea->sig[a] += sig;
        ea->res[a] += Y[a] > sig ? Y[a] - sig : 0.0;
        ea->alpha_abs[a] += sqrt(cr[a] * cr[a] + ci[a] * ci[a]) / P[a]; /* |alpha_a|: 1 = unbiased channel estimate */
        ea->alpha_n[a]++;
      }
      { /* T26d: cross-branch correlation of the LS residuals e_a = y_a - alpha_a h_a s (white noise: ~0) */
        double complex ar[NR_PDSCH_EVM_MAX_RX];
        for (int a = 0; a < nb; a++) ar[a] = P[a] > 0.0 ? (cr[a] + I * ci[a]) / P[a] : 0;
        double complex C[NR_PDSCH_EVM_MAX_RX][NR_PDSCH_EVM_MAX_RX] = {{0}};
        for (uint32_t i = 0; i < nb_re_pdsch; i++) {
          if (isnan(xr[0][i])) continue;
          double sr0[4], sq0[4];
          for (int l = 0; l < nl; l++) {
            const double vi = xr[l][i] * scale[l], vq = xi[l][i] * scale[l];
            double si = 2.0 * floor(vi / 2.0) + 1.0, sq = 2.0 * floor(vq / 2.0) + 1.0;
            si = si > lmax ? lmax : si < -lmax ? -lmax : si; sq = sq > lmax ? lmax : sq < -lmax ? -lmax : sq;
            sr0[l] = si / scale[l]; sq0[l] = sq / scale[l];
          }
          double complex e[NR_PDSCH_EVM_MAX_RX];
          for (int a = 0; a < nb; a++) {
            double complex hs = 0;
            for (int l = 0; l < nl; l++) hs += (chFext[l][a][i].r + I * chFext[l][a][i].i) * (sr0[l] + I * sq0[l]);
            e[a] = (rxdataF_ext[a][i].r + I * rxdataF_ext[a][i].i) - ar[a] * hs;
          }
          for (int a = 0; a < nb; a++)
            for (int b = a; b < nb; b++) C[a][b] += e[a] * conj(e[b]);
        }
        double rs = 0.0; int rn = 0;
        for (int a = 0; a < nb; a++)
          for (int b = a + 1; b < nb; b++)
            if (creal(C[a][a]) > 0 && creal(C[b][b]) > 0) { rs += cabs(C[a][b]) / sqrt(creal(C[a][a]) * creal(C[b][b])); rn++; }
        if (rn) { ea->rho_sum += rs / rn; ea->rho_n++; }
      }
      ea->n_re += nv;
      ea->n_sym++;
      ea->qm = qm;
      ea->nbrx = nb;
    }
    free(xr);
  }
  (void)decode_ref_sign;
}

/* The caller's symbol loop (nr_pdsch_passive_decode.c), same arguments and the same probe horizon. */
static int cpu_loop(const nr_pdsch_gpu_demod_call_t *c, int16_t *llr, uint32_t *dvr, int32_t *l2, c16_t *comp, c16_t *mag,
                    c16_t *magb, c16_t *magr)
{
  const NR_DL_FRAME_PARMS *fp = &c->ue->frame_parms;
  const uint32_t buf = c->buf;
  const int last = c->cfg->start_symbol + c->cfg->number_symbols - 1;
  for (int m = c->cfg->start_symbol; m <= last; m++) {
    if (c->probe_last_sym >= 0 && m > c->probe_last_sym && m != last)
      continue;
    if (nr_rx_pdsch(c->ue, c->proc, c->dlsch, c->freq_alloc, c->cfg, c->harq, (unsigned char)m, m == c->first_symbol,
                    (unsigned char)c->cfg->harq_process_nbr, c->est_size, (int32_t(*)[c->est_size])c->est, llr, dvr,
                    (c16_t(*)[fp->samples_per_slot_wCP])c->rxdataF, l2, buf, fp->nb_antennas_rx,
                    (c16_t(*)[NR_MAX_NB_LAYERS][buf])comp, (c16_t(*)[NR_MAX_NB_LAYERS][buf])mag,
                    (c16_t(*)[NR_MAX_NB_LAYERS][buf])magb, (c16_t(*)[NR_MAX_NB_LAYERS][buf])magr,
                    (c16_t(*)[NR_SYMBOLS_PER_SLOT])c->ptrs_phase, (int32_t(*)[NR_SYMBOLS_PER_SLOT])c->ptrs_re, c->nvar, c->scope,
                    (c16_t(*)[NR_MAX_NB_LAYERS * NR_MAX_NB_LAYERS][buf])c->rho, c->ssb_rm)
        < 0)
      return -1;
  }
  return 0;
}

/* Reasons the GPU path is refused (index into g_refuse, logged with the stats). */
enum { RF_OFF, RF_NL, RF_QM, RF_PTRS, RF_FORCED, RF_ENV, RF_OVERRIDE, RF_MODE, RF_MASK, RF_GEOM, RF_NRE0, RF_CAP, RF_BUSY, RF_N };

static __thread int t_qmode; /* refuse() -> gpu_grant(): the resolved multi-layer qam_mode */
static int refuse(const nr_pdsch_gpu_demod_call_t *c, int *mode, double *thr_coef, int *align, int *l2d)
{
  static int s_env = -1, s_mode = 0, s_align = 0, s_l2d = 0, s_l2d2 = 0;
  static double s_min_db = 12.0;
  if (s_env < 0) {
    /* every knob nr_rx_pdsch() reads whose path the GPU does not reproduce */
    const char *bad[] = {"ISAC_RX_BEST_BY_SNR", "ISAC_L2MAXH_PEAK", "ISAC_L2MAXH_HEADROOM", "ISAC_RX_IRC", "ISAC_RX_QAM_AGC",
                         "ISAC_RX_ML_2L", "ISAC_PDSCH_DECODE_DIAG", "ISAC_SUBSET_DIAG", "ISAC_RX_BRANCH", "ISAC_MMSE_FLOAT"};
    int any = 0;
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
      const char *e = getenv(bad[k]);
      if (e && atoi(e) != 0)
        any = 1;
    }
    if (nr_agnostic_v2())
      any = 1;
    s_mode = env_int("ISAC_RX_MRC_MODE", 0);
    if (getenv("ISAC_RX_BRANCH_MIN_DB"))
      s_min_db = atof(getenv("ISAC_RX_BRANCH_MIN_DB"));
    s_align = env_int("ISAC_RX_MRC_ALIGN", 0);
    s_l2d = env_int("ISAC_L2MAXH_DELTA", 0);
    s_l2d2 = env_int("ISAC_L2MAXH_DELTA_NL2", 0); /* nl >= 2 only, as the CPU path */
    s_env = any;
  }
  if (!atomic_load_explicit(&g_on, memory_order_relaxed))
    return RF_OFF;
  if (s_env)
    return RF_ENV;
  const int nl = c->dlsch->cw_info.Nl, qm = c->dlsch->cw_info.qamModOrder;
  if (nl >= 2) { /* nr_rx_pdsch()'s qam_mode resolution */
    static int s_qm = -1, s_nl34 = -1;
    if (s_qm < 0) {
      s_qm = env_int("ISAC_PDSCH_MMSE_MODE", 0);
      s_nl34 = env_int("ISAC_PDSCH_GPU_EXACT_DEMOD_NL34", 0); /* DECGPU: 3 and 4 layers (mode 2, and the stock MMSE) */
    }
    int qam_mode = nr_dlsch_qam_mode_override >= 0 ? nr_dlsch_qam_mode_override : s_qm;
    if (qam_mode < 0 || qam_mode > 2)
      qam_mode = 0;
    if (nl >= 3 && (qam_mode == 1 || qm == 2))
      qam_mode = 0;
    if (nl == 2 ? qam_mode != 2 : (nl > 4 || !s_nl34 || qam_mode == 1))
      return RF_NL;
    t_qmode = qam_mode;
  }
  if (qm != 2 && qm != 4 && qm != 6 && qm != 8)
    return RF_QM;
  if (c->harq->status != NR_ACTIVE)
    return RF_OVERRIDE;
  if ((c->cfg->pduBitmap & 0x1) && c->dlsch->rnti_type == TYPE_C_RNTI_)
    return RF_PTRS;
  if (nr_dlsch_forced_mask >= 0 || nr_dlsch_forced_branch >= 0)
    return RF_FORCED;
  if (nr_dlsch_irc_override > 0 || nr_dlsch_eq_override >= 0 || c->rho || !c->ssb_rm
      || c->scope->copy_chanest_to_scope || c->scope->copy_rxdataF_to_scope || c->ue->phy_sim_pdsch_rxdataF_ext
      || c->ue->phy_sim_pdsch_rxdataF_comp || c->ue->phy_sim_pdsch_dl_ch_estimates_ext || c->ue->do_ml)
    return RF_OVERRIDE;
  *mode = nr_dlsch_mrc_mode_override >= 0 ? nr_dlsch_mrc_mode_override : s_mode;
  if (*mode < 0 || *mode > 3)
    return RF_MODE;
  *thr_coef = pow(10.0, -s_min_db / 10.0);
  *align = s_align;
  *l2d = s_l2d + (nl >= 2 ? s_l2d2 : 0);
  return -1;
}

typedef struct {
  c16_t *rx_idx;
  int32_t *est_idx;
  uint32_t est_cap;
  c16_t *geo_y;
  int32_t *geo_h;
} geo_tls_t;

/* Index-coded inputs, built once per thread (rx_idx 14 x N, est_idx est_size) */
static geo_tls_t *geo_tls(const NR_DL_FRAME_PARMS *fp, uint32_t est_size)
{
  static __thread geo_tls_t g;
  const uint32_t N = fp->ofdm_symbol_size;
  if (!g.rx_idx) {
    g.rx_idx = malloc(sizeof(c16_t) * fp->samples_per_slot_wCP);
    g.geo_y = malloc(sizeof(c16_t) * NR_GPU_DEMOD_NSYM * NR_GPU_DEMOD_MAX_EXT);
    g.geo_h = malloc(sizeof(int32_t) * NR_GPU_DEMOD_NSYM * NR_GPU_DEMOD_MAX_EXT);
    if (!g.rx_idx || !g.geo_y || !g.geo_h)
      return NULL;
    for (uint32_t s = 0; s < fp->samples_per_slot_wCP; s++)
      g.rx_idx[s] = (c16_t){(int16_t)(s % N), 1};
  }
  if (g.est_cap < est_size) {
    free(g.est_idx);
    g.est_idx = malloc(sizeof(int32_t) * est_size);
    if (!g.est_idx)
      return NULL;
    g.est_cap = est_size;
    for (uint32_t s = 0; s < est_size; s++)
      ((c16_t *)g.est_idx)[s] = (c16_t){(int16_t)(s % N), (int16_t)(s / N + 1)};
  }
  return &g;
}

typedef struct {
  int16_t *llr;
  uint32_t llr_cap;
  c16_t *comp, *mag, *magb, *magr;
  size_t buf_n;
} chk_tls_t;

static void stats_log(uint64_t n)
{
  if (!(n == 1 || n == 10 || n == 100 || n == 1000 || n % 2000 == 0))
    return;
  char rf[256];
  int u = 0;
  for (int k = 0; k < RF_N; k++)
    u += snprintf(rf + u, sizeof(rf) - u, "%s%lu", k ? "," : "", (unsigned long)atomic_load(&g_refuse[k]));
  LOG_A(PHY, "SENSING: GPU_EXACT_DEMOD calls=%lu check_ok=%lu check_bad=%lu evm_close=%lu evm_far=%lu evm_maxrel=%.2e mean=%.0f us (gpu %.0f geo %.0f) max=%.0f us refused[off,nl,qm,ptrs,"
        "forced,env,override,mode,mask,geom,nre0,cap,busy]=[%s]\n",
        (unsigned long)n, (unsigned long)atomic_load(&g_chk_ok), (unsigned long)atomic_load(&g_chk_bad),
        (unsigned long)atomic_load(&g_evm_close), (unsigned long)atomic_load(&g_evm_far), atomic_load(&g_evm_maxrel) / 1e18,
        atomic_load(&g_ns_sum) / 1e3 / n, atomic_load(&g_ns_gpu) / 1e3 / n, atomic_load(&g_ns_geo) / 1e3 / n, atomic_load(&g_ns_max) / 1e3, rf);
  if (g_shapes == 1) {
    char sh[2048];
    int v = 0;
    static const char *rbn[4] = {"<=52", "<=106", "<=200", ">200"};
    for (int l = 0; l < 4; l++)
      for (int q = 0; q < 4; q++)
        for (int r = 0; r < 4; r++) {
          const uint64_t c = atomic_load(&g_shape[l][q][r][0]);
          if (c && v < (int)sizeof(sh) - 64)
            v += snprintf(sh + v, sizeof(sh) - v, " nl%d/qm%d/rb%s:n=%lu,mean=%.0f,gpu=%.0f", l + 1, 2 * (q + 1), rbn[r], (unsigned long)c,
                          atomic_load(&g_shape[l][q][r][1]) / 1e3 / c, atomic_load(&g_shape[l][q][r][2]) / 1e3 / c);
        }
    LOG_A(PHY, "SENSING: GPU_EXACT_DEMOD_SHAPES%s\n", sh);
  }
}

/* Folds one symbol's GPU EVM partial sums into nr_pdsch_evm_acc, in the field order of nr_rx_pdsch()'s block. */
static void evm_fold(const nr_gpu_evm_sym_t *E, int nl, int qm, int m)
{
  nr_pdsch_evm_acc_t *ea = &nr_pdsch_evm_acc;
  if (!E->valid)
    return;
  const double ip = E->ideal_pow, refn = ip * (double)E->nv;
  for (int l = 0; l < nl; l++) {
    ea->err += E->err_l[l];
    ea->ref += refn;
    ea->err_layer[l] += E->err_l[l];
    ea->ref_layer[l] += refn;
    ea->err_sym[m] += E->err_l[l];
    ea->ref_sym[m] += refn;
  }
  for (int k = 0; k < 12; k++) {
    ea->err_k[m][k] += E->err_k[k];
    ea->n_k[m][k] += (uint16_t)E->n_k[k];
  }
  if (E->rbmap)
    for (int rb = 0; rb < 276; rb++)
      if (E->n_rb[rb]) {
        ea->err_rb[E->row][rb] += E->err_rb[rb];
        ea->n_rb[E->row][rb] += E->n_rb[rb];
      }
  for (int l = 0; l < nl; l++)
    if (E->qd[l] > 0.0) {
      const double r = E->qx[l] - (E->qr[l] * E->qr[l] + E->qi[l] * E->qi[l]) / E->qd[l];
      ea->err_cpe += r > 0.0 ? r : 0.0;
    }
  for (int a = 0; a < E->nb; a++) {
    if (E->P[a] <= 0.0)
      continue;
    const double sig = (E->cr[a] * E->cr[a] + E->ci[a] * E->ci[a]) / E->P[a];
    ea->sig[a] += sig;
    ea->res[a] += E->Y[a] > sig ? E->Y[a] - sig : 0.0;
    ea->alpha_abs[a] += sqrt(E->cr[a] * E->cr[a] + E->ci[a] * E->ci[a]) / E->P[a];
    ea->alpha_n[a]++;
  }
  double rs = 0.0;
  int rn = 0;
  for (int a = 0; a < E->nb; a++)
    for (int b = a + 1; b < E->nb; b++)
      if (E->Cr[a][a] > 0 && E->Cr[b][b] > 0) {
        rs += hypot(E->Cr[a][b], E->Ci[a][b]) / sqrt(E->Cr[a][a] * E->Cr[b][b]);
        rn++;
      }
  if (rn) {
    ea->rho_sum += rs / rn;
    ea->rho_n++;
  }
  ea->n_re += E->nv;
  ea->n_sym++;
  ea->qm = qm;
  ea->nbrx = E->nb;
}

static int g_gpu_evm = -1;

/* EVM partial sums and kappa of every demodulated symbol into nr_pdsch_evm_acc, in nr_rx_pdsch()'s symbol order */
static void post_fold(const nr_gpu_demod_job_t *jp, const NR_UE_DLSCH_t *dlsch, int nl, int qm, int nbRx, int start, int last, uint32_t rx_size_symbol)
{
  for (int m = start; m <= last; m++) {
    if (!(jp->sym_mask >> m & 1))
      continue;
    c16_t(*y)[NR_GPU_DEMOD_MAX_EXT] = (c16_t(*)[NR_GPU_DEMOD_MAX_EXT])((c16_t *)jp->y_ext + (size_t)m * NR_GPU_DEMOD_MAX_RX * NR_GPU_DEMOD_MAX_EXT);
    c16_t(*h)[nbRx][NR_GPU_DEMOD_MAX_EXT] = (c16_t(*)[nbRx][NR_GPU_DEMOD_MAX_EXT])((c16_t *)jp->h_ext + (size_t)m * 16 * NR_GPU_DEMOD_MAX_EXT);
    if (jp->want_evm)
      evm_fold(&jp->evm[m], nl, qm, m);
    else
      evm_symbol(dlsch, nl, nbRx, jp->nre[m], rx_size_symbol, NR_GPU_DEMOD_MAX_EXT, jp->mrc_rx_index, jp->mrc_nb_rx, (unsigned char)m, y, h);
    if (nl == 1 && qm > 2 && jp->nre[m] >= 24) { /* nr_rx_pdsch(): ISAC_RX_QAM_AGC block, 1 layer */
      const double sz = (double)jp->kappa_sz[m][0], sm = (double)jp->kappa_sm[m][0];
      const double kappa = sm > 0.0 ? sz / sm : 1.0;
      nr_pdsch_evm_acc.kappa_sum += kappa;
      nr_pdsch_evm_acc.kappa_n++;
    }
    if (nl >= 2 && qm > 2 && jp->nre[m] >= 24) /* nr_rx_pdsch(): QAM-FIX multi-layer kappa */
      for (int l = 0; l < nl; l++) {
        const double sz = (double)jp->kappa_sz[m][l], sm = (double)jp->kappa_sm[m][l];
        if (sm > 0.0) {
          nr_pdsch_evm_acc.kappa_sum += sz / sm;
          nr_pdsch_evm_acc.kappa_n++;
        }
      }
  }
}

/* GPU demodulation of the grant into the given buffers; 0 = done. EVM accumulates into nr_pdsch_evm_acc (if armed). */
static int gpu_grant(const nr_pdsch_gpu_demod_call_t *c, int mode, double thr_coef, int align, int l2d, int16_t *llr, uint32_t llr_cap,
                     uint32_t *dvr, int32_t *l2, c16_t *comp, c16_t *mag, c16_t *magb, c16_t *magr, uint64_t *gpu_ns)
{
  const NR_DL_FRAME_PARMS *fp = &c->ue->frame_parms;
  const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg = c->cfg;
  const int nl = c->dlsch->cw_info.Nl, qm = c->dlsch->cw_info.qamModOrder, nbRx = fp->nb_antennas_rx;
  const uint32_t ext = (c->freq_alloc->num_rbs * NR_NB_SC_PER_RB + 15) & ~15;
  if (ext > NR_GPU_DEMOD_MAX_EXT || c->buf < ext)
    return RF_GEOM;
  geo_tls_t *g = geo_tls(fp, c->est_size);
  if (!g)
    return RF_GEOM;
  nr_gpu_demod_job_t j = {0};
  j.nbRx = nbRx;
  j.nl = nl;
  j.qm = qm;
  j.N = fp->ofdm_symbol_size;
  j.start_symbol = cfg->start_symbol;
  j.nb_symbols = cfg->number_symbols;
  j.first_symbol = c->first_symbol;
  const int last = cfg->start_symbol + cfg->number_symbols - 1;
  for (int m = cfg->start_symbol; m <= last && m < NR_GPU_DEMOD_NSYM; m++)
    if (!(c->probe_last_sym >= 0 && m > c->probe_last_sym && m != last))
      j.sym_mask |= (uint16_t)(1u << m);
  if (last >= NR_GPU_DEMOD_NSYM || c->first_symbol < cfg->start_symbol || c->first_symbol > last || !(j.sym_mask >> c->first_symbol & 1))
    return RF_MASK;
  uint32_t off = 0;
  const uint64_t tg = now_ns();
  for (int m = cfg->start_symbol; m <= last; m++) {
    j.llr_off[m] = off;
    if (!(j.sym_mask >> m & 1))
      continue;
    const uint32_t n = nr_dlsch_extract_geometry(fp, c->ue->chest_time, g->rx_idx, c->est_size, g->est_idx, ext, g->geo_y + (size_t)m * ext,
                                                 g->geo_h + (size_t)m * ext, (unsigned char)m, c->cfg, c->freq_alloc, c->ssb_rm);
    if (n > ext)
      return RF_GEOM;
    j.nre[m] = (uint16_t)n;
    off += n * (uint32_t)qm * (uint32_t)nl;
  }
  atomic_fetch_add_explicit(&g_ns_geo, now_ns() - tg, memory_order_relaxed);
  if (j.nre[c->first_symbol] == 0)
    return RF_NRE0;
  if (off > llr_cap)
    return RF_CAP;
  j.rx_stride = fp->samples_per_slot_wCP;
  j.est_stride = c->est_size;
  j.buf = c->buf;
  j.ext = ext;
  j.rxdataF = (const int16_t *)c->rxdataF;
  j.est = (const int16_t *)c->est;
  j.geo_y = (const int16_t *)g->geo_y;
  j.geo_h = (const int16_t *)g->geo_h;
  j.comp = (int16_t *)comp;
  j.mag = (int16_t *)mag;
  j.magb = (int16_t *)magb;
  j.magr = (int16_t *)magr;
  j.llr = llr;
  j.mode = mode;
  j.thr_coef = thr_coef;
  j.l2_delta = l2d;
  j.align = align;
  j.qam_mode = nl >= 2 ? t_qmode : 0;
  j.nvar = c->nvar;
  j.mag_per_layer = nl >= 2 && j.qam_mode == 2; /* mode 2 only: per-layer thresholds */
  j.wide_mrc = nr_pdsch_decode_wide_mrc;
  j.want_kappa = nr_pdsch_evm_acc.armed;
  if (g_gpu_evm < 0)
    g_gpu_evm = env_int("ISAC_PDSCH_GPU_EXACT_DEMOD_EVM", 1); /* 1: GPU float, 2: GPU double, 0: CPU copy (exact, slow) */
  static int s_evm34 = -1; /* GPUFUSE: ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_NL34=1 (default off): GPU EVM for Nl = 3, 4 too (else the CPU copy) */
  if (s_evm34 < 0)
    s_evm34 = env_int("ISAC_PDSCH_GPU_EXACT_DEMOD_EVM_NL34", 0);
  j.want_evm = nr_pdsch_evm_acc.armed && g_gpu_evm && (nl <= 2 || s_evm34 == 1);
  j.evm_fp64 = g_gpu_evm == 2;
  j.evm_erase = nr_dlsch_csi_erase_n > 0;
  j.evm_defer = t_defer_ok && j.want_evm && t_def;
  const uint64_t t0 = now_ns();
  if (g_run(&j) != 0)
    return RF_BUSY;
  *gpu_ns = now_ns() - t0;
  *l2 = j.log2_maxh;
  for (int m = cfg->start_symbol; m <= last; m++)
    if (j.sym_mask >> m & 1)
      dvr[m] = j.nre[m];
  if (j.used_branch_set)
    nr_dlsch_used_branch = j.used_branch;
  if (nr_pdsch_evm_acc.armed) {
    if (j.evm_pending && t_def) { /* DECGPU: fold after the EVM chain (nr_pdsch_gpu_demod_evm_finish) */
      memcpy(&t_def->j, &j, offsetof(nr_gpu_demod_job_t, evm));
      t_def->j.ctx = j.ctx;
      t_def->j.evm_pending = 1;
      t_def->nl = nl;
      t_def->qm = qm;
      t_def->nbrx = nbRx;
      t_def->start = cfg->start_symbol;
      t_def->last = last;
      t_def->pending = 1;
      return -1;
    }
    post_fold(&j, c->dlsch, nl, qm, nbRx, cfg->start_symbol, last, (c->freq_alloc->num_rbs * NR_NB_SC_PER_RB + 15) & ~15);
  }
  g_release(j.ctx);
  return -1;
}

static int compare(const nr_pdsch_gpu_demod_call_t *c, const chk_tls_t *k, const uint32_t *dvr_g, int32_t l2_g, int used_g,
                   const nr_pdsch_evm_acc_t *acc_g, int used_c, const nr_pdsch_evm_acc_t *acc_c, char *why, size_t wn)
{
  const fapi_nr_dl_config_dlsch_pdu_rel15_t *cfg = c->cfg;
  const int nl = c->dlsch->cw_info.Nl, qm = c->dlsch->cw_info.qamModOrder;
  const int last = cfg->start_symbol + cfg->number_symbols - 1;
  if (l2_g != *c->log2_maxh) {
    snprintf(why, wn, "log2_maxh gpu %d cpu %d", l2_g, *c->log2_maxh);
    return 0;
  }
  uint32_t tot = 0;
  for (int m = cfg->start_symbol; m <= last; m++) {
    if (dvr_g[m] != c->dl_valid_re[m]) {
      snprintf(why, wn, "dl_valid_re[%d] gpu %u cpu %u", m, dvr_g[m], c->dl_valid_re[m]);
      return 0;
    }
    tot += c->dl_valid_re[m] * (uint32_t)qm * (uint32_t)nl;
  }
  for (uint32_t i = 0; i < tot; i++)
    if (k->llr[i] != c->llr[i]) {
      snprintf(why, wn, "llr[%u] of %u gpu %d cpu %d", i, tot, k->llr[i], c->llr[i]);
      return 0;
    }
  for (int m = cfg->start_symbol; m <= last; m++)
    for (int l = 0; l < nl; l++) {
      const size_t row = ((size_t)m * NR_MAX_NB_LAYERS + l) * c->buf;
      for (uint32_t i = 0; i < c->dl_valid_re[m]; i++) {
        const c16_t a = k->comp[row + i], b = c->comp[row + i];
        if (a.r != b.r || a.i != b.i) {
          snprintf(why, wn, "comp sym %d layer %d re %u gpu (%d,%d) cpu (%d,%d)", m, l, i, a.r, a.i, b.r, b.i);
          return 0;
        }
        const c16_t *gm[3] = {k->mag, k->magb, k->magr}, *cm[3] = {c->mag, c->magb, c->magr};
        for (int q = 0; q < 3 && qm > 2 * (q + 1); q++)
          if (gm[q][row + i].r != cm[q][row + i].r || gm[q][row + i].i != cm[q][row + i].i) {
            snprintf(why, wn, "mag%d sym %d layer %d re %u gpu %d cpu %d", q, m, l, i, gm[q][row + i].r, cm[q][row + i].r);
            return 0;
          }
      }
    }
  if (used_g != used_c) {
    snprintf(why, wn, "used_branch gpu %d cpu %d", used_g, used_c);
    return 0;
  }
  if (g_gpu_evm > 0) { /* GPU EVM: block-reduced sums, compared to rounding; counted, never disables the path */
    const double rel = acc_c->err > 0 ? fabs(acc_g->err - acc_c->err) / acc_c->err : fabs(acc_g->err);
    const double tol = g_gpu_evm == 2 ? 1e-9 : 1e-4; /* float per-RE math: relative error of the sums ~1e-6 */
    const int same = acc_g->n_re == acc_c->n_re && acc_g->n_sym == acc_c->n_sym && acc_g->kappa_n == acc_c->kappa_n
                     && acc_g->kappa_sum == acc_c->kappa_sum && memcmp(acc_g->n_rb, acc_c->n_rb, sizeof(acc_g->n_rb)) == 0
                     && memcmp(acc_g->n_k, acc_c->n_k, sizeof(acc_g->n_k)) == 0 && rel < tol;
    atomic_fetch_add_explicit(same ? &g_evm_close : &g_evm_far, 1, memory_order_relaxed);
    uint64_t q = (uint64_t)(rel * 1e18), mx = atomic_load(&g_evm_maxrel);
    while (q > mx && !atomic_compare_exchange_weak(&g_evm_maxrel, &mx, q)) {}
    if (!same && atomic_load(&g_evm_far) <= 5)
      LOG_W(PHY, "SENSING: GPU_EXACT_DEMOD EVM differs: err %.17g/%.17g n_re %u/%u n_sym %u/%u kappa %.17g/%.17g\n", acc_g->err,
            acc_c->err, acc_g->n_re, acc_c->n_re, acc_g->n_sym, acc_c->n_sym, acc_g->kappa_sum, acc_c->kappa_sum);
    return 1;
  }
  if (memcmp(acc_g, acc_c, sizeof(*acc_g)) != 0) {
    snprintf(why, wn, "EVM accumulator (err %.17g/%.17g n_re %u/%u kappa %.17g/%.17g)", acc_g->err, acc_c->err, acc_g->n_re,
             acc_c->n_re, acc_g->kappa_sum, acc_c->kappa_sum);
    return 0;
  }
  return 1;
}

int nr_pdsch_gpu_demod_evm_deferred(void)
{
  return t_def && t_def->pending;
}

int nr_pdsch_gpu_demod_evm_finish(int fold)
{
  if (!t_def || !t_def->pending)
    return -1;
  const int ok = g_evm_wait(&t_def->j) == 0;
  if (ok && fold) /* want_evm is set on every deferred job: the CPU EVM copy (dlsch, rx_size) is not used */
    post_fold(&t_def->j, NULL, t_def->nl, t_def->qm, t_def->nbrx, t_def->start, t_def->last, 0);
  g_release(t_def->j.ctx);
  t_def->pending = 0;
  return ok ? 0 : -1;
}

int nr_pdsch_gpu_demod_grant(const nr_pdsch_gpu_demod_call_t *c)
{
  if (t_def && t_def->pending) /* a decode that returned before its deferred EVM was folded: drop it, free the context */
    nr_pdsch_gpu_demod_evm_finish(0);
  if (g_defer && !t_def)
    t_def = calloc(1, sizeof(*t_def));
  int mode = 0, align = 0, l2d = 0;
  double thr_coef = 0.0;
  if (nr_pdsch_route_cpu()) /* BATCHCPU */
    return -1;
  const int rf = refuse(c, &mode, &thr_coef, &align, &l2d);
  if (rf >= 0) {
    if (rf != RF_OFF)
      atomic_fetch_add_explicit(&g_refuse[rf], 1, memory_order_relaxed);
    return -1;
  }
  const uint64_t t0 = now_ns();
  uint64_t gns = 0;
  const uint64_t n = atomic_fetch_add_explicit(&g_calls, 1, memory_order_relaxed) + 1;
  const int check = g_check == 1 || (g_check > 1 && n % (uint64_t)g_check == 0);
  if (!check) {
    t_defer_ok = g_defer;
    const int r = gpu_grant(c, mode, thr_coef, align, l2d, c->llr, c->llr_cap, c->dl_valid_re, c->log2_maxh, c->comp, c->mag, c->magb,
                            c->magr, &gns);
    t_defer_ok = 0;
    if (r >= 0) {
      atomic_fetch_add_explicit(&g_refuse[r], 1, memory_order_relaxed);
      atomic_fetch_sub_explicit(&g_calls, 1, memory_order_relaxed);
      return -1; /* nothing was written that the CPU loop does not overwrite: it writes every output again */
    }
  } else {
    static __thread chk_tls_t k;
    const size_t bn = (size_t)NR_SYMBOLS_PER_SLOT * NR_MAX_NB_LAYERS * c->buf;
    if (k.buf_n < bn) {
      free(k.comp);
      free(k.mag);
      free(k.magb);
      free(k.magr);
      k.comp = calloc(bn, sizeof(c16_t));
      k.mag = calloc(bn, sizeof(c16_t));
      k.magb = calloc(bn, sizeof(c16_t));
      k.magr = calloc(bn, sizeof(c16_t));
      k.buf_n = (k.comp && k.mag && k.magb && k.magr) ? bn : 0;
    }
    if (k.llr_cap < c->llr_cap) {
      free(k.llr);
      k.llr = calloc(c->llr_cap, sizeof(int16_t));
      k.llr_cap = k.llr ? c->llr_cap : 0;
    }
    if (!k.buf_n || !k.llr_cap) {
      atomic_fetch_sub_explicit(&g_calls, 1, memory_order_relaxed);
      return -1;
    }
    const nr_pdsch_evm_acc_t acc0 = nr_pdsch_evm_acc;
    const int used0 = nr_dlsch_used_branch;
    if (cpu_loop(c, c->llr, c->dl_valid_re, c->log2_maxh, c->comp, c->mag, c->magb, c->magr) != 0) {
      atomic_fetch_sub_explicit(&g_calls, 1, memory_order_relaxed);
      return -1;
    }
    const nr_pdsch_evm_acc_t acc_c = nr_pdsch_evm_acc;
    const int used_c = nr_dlsch_used_branch;
    nr_pdsch_evm_acc = acc0;
    nr_dlsch_used_branch = used0;
    uint32_t dvr_g[NR_SYMBOLS_PER_SLOT] = {0};
    int32_t l2_g = 0;
    memset(k.llr, 0, sizeof(int16_t) * c->llr_cap);
    const int r = gpu_grant(c, mode, thr_coef, align, l2d, k.llr, c->llr_cap, dvr_g, &l2_g, k.comp, k.mag, k.magb, k.magr, &gns);
    const nr_pdsch_evm_acc_t acc_g = nr_pdsch_evm_acc;
    const int used_g = nr_dlsch_used_branch;
    nr_pdsch_evm_acc = acc_c;
    nr_dlsch_used_branch = used_c;
    if (r >= 0) { /* refused after the CPU ran: the CPU results stand */
      atomic_fetch_add_explicit(&g_refuse[r], 1, memory_order_relaxed);
      atomic_fetch_sub_explicit(&g_calls, 1, memory_order_relaxed);
      return 0;
    }
    char why[256] = "";
    if (compare(c, &k, dvr_g, l2_g, used_g, &acc_g, used_c, &acc_c, why, sizeof(why))) {
      atomic_fetch_add_explicit(&g_chk_ok, 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&g_chk_bad, 1, memory_order_relaxed);
      atomic_store(&g_on, 0);
      LOG_E(PHY, "SENSING: GPU_EXACT_DEMOD MISMATCH rnti 0x%x rb %d+%d sym %d+%d Qm %d Nl %d: %s -- GPU demodulation disabled\n",
            c->dlsch->rnti, c->freq_alloc->first_rb, c->freq_alloc->num_rbs, c->cfg->start_symbol, c->cfg->number_symbols,
            c->dlsch->cw_info.qamModOrder, c->dlsch->cw_info.Nl, why);
    }
  }
  const uint64_t dt = now_ns() - t0;
  if (g_shapes < 0)
    g_shapes = env_int("ISAC_PDSCH_GPU_EXACT_DEMOD_SHAPES", 0);
  if (g_shapes == 1) {
    const int l = (c->dlsch->cw_info.Nl - 1) & 3, q = (c->dlsch->cw_info.qamModOrder / 2 - 1) & 3, r = rb_class(c->freq_alloc->num_rbs);
    atomic_fetch_add_explicit(&g_shape[l][q][r][0], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_shape[l][q][r][1], dt, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_shape[l][q][r][2], gns, memory_order_relaxed);
  }
  atomic_fetch_add_explicit(&g_ns_sum, dt, memory_order_relaxed);
  atomic_fetch_add_explicit(&g_ns_gpu, gns, memory_order_relaxed);
  uint64_t mx = atomic_load(&g_ns_max);
  while (dt > mx && !atomic_compare_exchange_weak(&g_ns_max, &mx, dt)) {}
  stats_log(n);
  return 0;
}
