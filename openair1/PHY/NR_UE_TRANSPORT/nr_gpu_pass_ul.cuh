/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 7: device kernels of the PUSCH pass (host side: nr_gpu_pass_ul.c, graph: nr_gpu_pass_graph.cu, contract: nr_gpu_pass.h). Two sections, each included by the
 * translation unit that owns what it needs:
 *   NR_GPU_PASS_UL_CHEST (nr_pdsch_gpu_fep.cu, after namespace nrch): k_ul_ls, the least-squares step of nr_pusch_antenna_processing() for the nr_gpu_chest_job_t type 3.
 *   NR_GPU_PASS_UL_EQ (nr_gpu_pass_graph.cu): k_ul_level / k_ul_decide / k_ul_eq / k_ul_llr, the equaliser of nr_rx_pusch_group_internal() + inner_rx() for ONE layer.
 * All of it is integer arithmetic of the CPU functions (nr_channel_level, nr_channel_compensation, nr_compute_llr), so it is bit-exact whatever the build. */
#ifdef NR_GPU_PASS_UL_CHEST
namespace nrch {
/* c32x16maddShift(pilot, rx, ch, 16) over the two pilots of a group of four REs, then the int16 cast of nr_pusch_antenna_processing() (type 1, chest_freq == 0):
 *   ch = (p0 * rx0 >> 16) + (p1 * rx1 >> 16) per component, ul_ls_est[4 q .. 4 q + 3] = (int16)ch. The DL estimator rounds differently (each product >> 15, then the pair >> 1). */
__device__ __forceinline__ int ul_mul_r(short2 a, short2 b) { return (int)((unsigned)((int)a.x * b.x) - (unsigned)((int)a.y * b.y)); }
__device__ __forceinline__ int ul_mul_i(short2 a, short2 b) { return (int)((unsigned)((int)a.x * b.y) + (unsigned)((int)a.y * b.x)); }
__global__ void k_ul_ls(const Par j, const nr_gpu_chest_row_t *rows, short2 *ls)
{
  const nr_gpu_chest_row_t R = rows[blockIdx.x];
  short2 *o = ls + (size_t)blockIdx.x * 4096;
  const short2 *rx = (const short2 *)R.rxF, *pil = (const short2 *)R.pil;
  const int N = j.N, idx = 12 * R.nb_rb;
  for (int k = threadIdx.x; k < N; k += blockDim.x) {
    short2 v = make_short2(0, 0);
    if (k < idx) {
      const int q = k >> 2, a = R.k0 + 4 * q, b = a + 2;
      const short2 x0 = rx[a % N], x1 = rx[b % N];
      const int r = (ul_mul_r(pil[2 * q], x0) >> 16) + (ul_mul_r(pil[2 * q + 1], x1) >> 16);
      const int i = (ul_mul_i(pil[2 * q], x0) >> 16) + (ul_mul_i(pil[2 * q + 1], x1) >> 16);
      v = make_short2((short)r, (short)i);
    }
    o[k] = v;
  }
}
} // namespace nrch
#endif

#ifdef NR_GPU_PASS_UL_EQ
#include "nr_pdsch_gpu_demod.cuh"
namespace nrul {
constexpr int NS = 14, NN = 4096;
/* what the host writes before every replay (device-accessible) */
struct UlBlk {
  int nrb, nre12, qm, nbRx, keep, delta, nd, meas_sym, meas_est, meas_len, ext;
  int dsym[4];
  int nre[NS], est_sym[NS], is_dmrs[NS];
  uint32_t llr_off[NS];
  int start_re;
  int gate_on; float gate_thr_db; /* Fix A+D: the no-chance gate (k_gate_ul) */
  const short2 *rx[4];  /* rxdataF row of symbol 0 of the slot per antenna */
  const short2 *est;    /* [antenna][14][N] */
  short2 *comp, *mag, *magb, *magc; /* [14][ext] */
  int16_t *llr;
  const uint32_t *scr;  /* descrambling words, bit set = flip */
};
struct UlOut { /* written by the kernels, read by the host after the pass */
  int32_t avg[4];
  int32_t log2_maxh;
  uint64_t hsum[16];
};
/* the estimate after the "align the channel estimates for the delta shift" memmove of nr_pusch_antenna_processing() */
__device__ __forceinline__ short2 est_at(const short2 *row, int idx, int delta) { return row[idx < delta ? 0 : idx - delta]; }
__device__ __forceinline__ int ext_idx(const UlBlk &b, int m, int j) { return b.is_dmrs[m] ? 1 - b.delta + 2 * j : j; }

/* blocks [0, nd * nbRx): sum |H|^2 of a chest row over 12 nrb estimates (the DM-RS SNR of nr_rx_pusch_group_internal); blocks [nd * nbRx, + nbRx): the channel level of
 * antenna a on the measurement symbol = nr_channel_level() over the extraction padded with zeros to a multiple of 16 */
__global__ void k_ul_level(const UlBlk *bp, UlOut *o)
{
  const UlBlk &b = *bp;
  __shared__ unsigned long long red[256];
  __shared__ uint32_t lane[256][4];
  const int t = threadIdx.x, nrows = b.nd * b.nbRx;
  if ((int)blockIdx.x < nrows) {
    const int k = blockIdx.x / b.nbRx, a = blockIdx.x % b.nbRx;
    const short2 *row = b.est + ((size_t)a * NS + b.dsym[k]) * NN;
    unsigned long long s = 0;
    for (int i = t; i < b.nre12; i += blockDim.x) {
      const short2 h = row[i];
      s += (unsigned long long)(uint32_t)((int)h.x * h.x + (int)h.y * h.y); /* c16amp2 is an int; the CPU adds it to a double: exact below 2^53 */
    }
    red[t] = s;
    __syncthreads();
    for (int q = blockDim.x / 2; q > 0; q >>= 1) {
      if (t < q) red[t] += red[t + q];
      __syncthreads();
    }
    if (t == 0) o->hsum[blockIdx.x] = red[0];
    return;
  }
  const int a = blockIdx.x - nrows;
  if (b.keep >= 0 && a != b.keep) {
    if (t == 0) o->avg[a] = 0;
    return;
  }
  const short2 *row = b.est + ((size_t)a * NS + b.meas_est) * NN;
  const int len = b.meas_len, x = nrd_factor2((unsigned)len), nre = b.nre[b.meas_sym];
  uint32_t l4[4] = {0, 0, 0, 0};
  for (int g = t; g < (len >> 2); g += blockDim.x)
    for (int q = 0; q < 4; q++) {
      const int j = 4 * g + q;
      nrd_c16 h = {0, 0};
      if (j < nre) {
        const short2 e = est_at(row, ext_idx(b, b.meas_sym, j), b.delta);
        h.r = e.x;
        h.i = e.y;
      }
      l4[q] += nrd_level_term(h, x);
    }
  for (int q = 0; q < 4; q++) lane[t][q] = l4[q];
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (t < s)
      for (int q = 0; q < 4; q++) lane[t][q] += lane[t + s][q];
    __syncthreads();
  }
  if (t == 0) o->avg[a] = nrd_level_finish(lane[0], (uint32_t)len);
}
/* log2_maxh of nr_rx_pusch_group_internal() for one layer */
__global__ void k_ul_decide(const UlBlk *bp, UlOut *o)
{
  const UlBlk &b = *bp;
  if (threadIdx.x != 0) return;
  int avgs = 0;
  for (int a = 0; a < b.nbRx; a++)
    if (b.keep < 0 || a == b.keep) avgs = avgs > o->avg[a] ? avgs : o->avg[a];
  int l2 = (nrd_log2_approx((uint32_t)avgs) >> 1) + 1 + nrd_log2_approx((uint32_t)(b.nbRx >> 1));
  o->log2_maxh = l2 < 0 ? 0 : l2;
}
/* extraction (nr_ulsch_extract_rbs) + nr_channel_compensation (MRC over the antennas; the branches the passive equaliser does not keep have a zero channel) */
__global__ void k_ul_eq(const UlBlk *bp, const UlOut *o)
{
  const UlBlk &b = *bp;
  const int m = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x, nre = b.nre[m];
  if (i >= nre) return;
  const int idx = ext_idx(b, m, i), shift = o->log2_maxh, qm = b.qm;
  const int rxi = (b.start_re + idx) % NN;
  const int16_t qa = qm == 4 ? NRD_QAM16_n1 : qm == 6 ? NRD_QAM64_n1 : qm == 8 ? NRD_QAM256_n1 : 0;
  const int16_t qb = qm == 6 ? NRD_QAM64_n2 : qm == 8 ? NRD_QAM256_n2 : 0;
  const int16_t qc = qm == 8 ? NRD_QAM256_n3 : 0;
  nrd_c16 acc = {0, 0};
  int16_t ma = 0, mb = 0, mc = 0;
  for (int a = 0; a < b.nbRx; a++) {
    nrd_c16 h = {0, 0};
    if (b.keep < 0 || a == b.keep) {
      const short2 e = est_at(b.est + ((size_t)a * NS + b.est_sym[m]) * NN, idx, b.delta);
      h.r = e.x;
      h.i = e.y;
    }
    const short2 yv = b.rx[a][(size_t)m * NN + rxi];
    nrd_c16 y;
    y.r = yv.x;
    y.i = yv.y;
    const nrd_c16 c = nrd_cpx_mult_conj(h, y, shift);
    if (a == 0) acc = c;
    else {
      acc.r = nrd_adds(acc.r, c.r);
      acc.i = nrd_adds(acc.i, c.i);
    }
    if (qm > 2) {
      const int16_t mg = nrd_mag16(h, shift);
      const int16_t ta = nrd_mulhrs(mg, qa), tb = nrd_mulhrs(mg, qb), tc = nrd_mulhrs(mg, qc);
      ma = a == 0 ? ta : nrd_adds(ma, ta);
      mb = a == 0 ? tb : nrd_adds(mb, tb);
      mc = a == 0 ? tc : nrd_adds(mc, tc);
    }
  }
  const size_t p = (size_t)m * b.ext + i;
  b.comp[p] = make_short2(acc.r, acc.i);
  if (qm > 2) {
    b.mag[p] = make_short2(ma, ma);
    if (qm > 4) b.magb[p] = make_short2(mb, mb);
    if (qm > 6) b.magc[p] = make_short2(mc, mc);
  }
}
/* nr_compute_llr (the last nre % 4 REs of a symbol take the scalar tail) and the unscrambling of nr_pusch_symbol_processing, straight into the decoder's LLR order */
__global__ void k_ul_llr(const UlBlk *bp)
{
  const UlBlk &b = *bp;
  const int m = blockIdx.y;
  const int i = blockIdx.x * blockDim.x + threadIdx.x, nre = b.nre[m];
  if (i >= nre) return;
  const size_t row = (size_t)m * b.ext;
  const uint32_t o = b.llr_off[m] + (uint32_t)i * b.qm;
  nrd_llr_re_out(b.qm, (uint32_t)i, (uint32_t)nre, (const nrd_c16 *)b.comp + row, (const nrd_c16 *)b.mag + row, (const nrd_c16 *)b.magb + row, (const nrd_c16 *)b.magc + row,
                 b.llr + o);
  for (int q = 0; q < b.qm; q++)
    if (b.scr[(o + q) >> 5] >> ((o + q) & 31) & 1u) b.llr[o + q] = (int16_t)(-b.llr[o + q]);
}
} // namespace nrul
#endif
