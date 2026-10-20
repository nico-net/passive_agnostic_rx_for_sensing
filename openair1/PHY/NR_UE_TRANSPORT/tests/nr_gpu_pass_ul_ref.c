/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* GPUPASS Task 7 test reference: the CPU half of the PUSCH receive chain of the passive UL decoder for ONE layer, ported from nr_pusch_antenna_processing()
 * (nr_ul_channel_estimation.c, DM-RS type 1, chest_freq 0) and inner_rx() / nr_rx_pusch_group_internal() (nr_ulsch_demodulation.c). Everything below the port
 * is the REAL library code: nr_passive_est_delay (with the loaded DFT), nr_channel_level, nr_scale_channel, nr_channel_compensation, nr_compute_llr. The port exists
 * because the originals are static / need a PHY_VARS_gNB; the replay shadow check (ISAC_UL_GPU_PASS_CHECK) compares the pass with the production functions. */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "PHY/NR_REFSIG/dmrs_nr.h"
#include "PHY/TOOLS/tools_defs.h"
#include "PHY/NR_UE_ESTIMATION/filt16a_32.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/nr_phy_common/inc/nr_passive_delay.h"
#include "PHY/nr_phy_common/inc/nr_channel_compensation.h"
#include "PHY/nr_phy_common/inc/nr_compute_llr.h"
#include "nr_gpu_pass_ul_ref.h"

#define N_FFT 4096

static int delay_idx(int d, int max_comp)
{
  int i = max_comp + d;
  i = i < 0 ? 0 : i;
  return i > (max_comp << 1) ? (max_comp << 1) : i;
}

void nr_gpu_pass_ul_ref_chest(const c16_t *rx, const c16_t *pil, int k0, int nb_rb, int delta, const c16_t *delay_table, int dt_stride, int max_delay_comp, c16_t *ul_ch,
                              int out_delay[3], uint64_t *noise)
{
  static __thread c16_t ls[N_FFT] __attribute__((aligned(32)));
  static __thread c16_t tm[N_FFT] __attribute__((aligned(32)));
  memset(ls, 0, sizeof(ls));
  memset(ul_ch, 0, sizeof(c16_t) * N_FFT);
  const c16_t *p = pil;
  int pilot_cnt = 0;
  for (int n = 0; n < 3 * nb_rb; n++) {
    c32_t ch = {0};
    for (int k_line = 0; k_line <= 1; k_line++) {
      const int re_offset = (k0 + (n << 2) + (k_line << 1) + delta) % N_FFT;
      ch = c32x16maddShift(*p, rx[re_offset], ch, 16);
      p++;
    }
    const c16_t ch16 = {.r = (int16_t)ch.r, .i = (int16_t)ch.i};
    for (int k = pilot_cnt << 1; k < (pilot_cnt << 1) + 4; k++)
      ls[k] = ch16;
    pilot_cnt += 2;
  }
  delay_t delay = {0};
  nr_passive_est_delay(N_FFT, ls, tm, &delay);
  out_delay[0] = delay.est_delay;
  out_delay[1] = delay.delay_max_pos;
  out_delay[2] = delay.delay_max_val;
  const c16_t *fwd = delay_table + (size_t)delay_idx(delay.est_delay, max_delay_comp) * dt_stride;
  c16_t *o = ul_ch;
  pilot_cnt = 0;
  for (int n = 0; n < 3 * nb_rb; n++)
    for (int k_line = 0; k_line <= 1; k_line++) {
      const int k = pilot_cnt << 1;
      const c16_t ch16 = c16mulShift(ls[k], fwd[k], 8);
      if (pilot_cnt == 0) {
        c16multaddVectRealComplex(filt16_ul_p0, &ch16, o, 16);
      } else if (pilot_cnt == 1 || pilot_cnt == 2) {
        c16multaddVectRealComplex(filt16_ul_p1p2, &ch16, o, 16);
      } else if (pilot_cnt == (6 * nb_rb - 1)) {
        c16multaddVectRealComplex(filt16_ul_last, &ch16, o, 16);
      } else {
        c16multaddVectRealComplex(filt16_ul_middle, &ch16, o, 16);
        if (pilot_cnt % 2 == 0)
          o += 4;
      }
      pilot_cnt++;
    }
  const c16_t *inv = delay_table + (size_t)delay_idx(-delay.est_delay, max_delay_comp) * dt_stride;
  uint64_t noise_amp2 = 0;
  pilot_cnt = 0;
  for (int n = 0; n < 3 * nb_rb; n++)
    for (int k_line = 0; k_line <= 1; k_line++) {
      const int k = pilot_cnt << 1;
      ul_ch[k] = c16mulShift(ul_ch[k], inv[k], 8);
      ul_ch[k + 1] = c16mulShift(ul_ch[k + 1], inv[k + 1], 8);
      noise_amp2 += c16amp2(c16sub(ls[k], ul_ch[k]));
      noise_amp2 += c16amp2(c16sub(ls[k + 1], ul_ch[k + 1]));
      pilot_cnt++;
    }
  *noise = noise_amp2;
  if (delta != 0) {
    memmove(&ul_ch[delta], ul_ch, (nb_rb * 12 - delta) * sizeof(c16_t));
    for (int d = 0; d < delta; d++)
      ul_ch[d] = ul_ch[delta];
  }
}

/* nr_ulsch_extract_rbs() for one layer, one antenna (contiguous allocation) */
static void extract(const c16_t *rxdataF, const c16_t *chF, c16_t *rxFext, c16_t *chFext, int is_dmrs, int delta, int start_re, int nb_rb, int type2)
{
  (void)type2;
  const int nb_re_pusch = 12 * nb_rb;
  const c16_t *rxF = rxdataF;
  c16_t *rxF_ext = rxFext, *ul_ch0_ext = chFext;
  if (is_dmrs == 0) {
    if (start_re + nb_re_pusch <= N_FFT)
      memcpy(rxF_ext, &rxF[start_re], nb_re_pusch * sizeof(c16_t));
    else {
      const int neg_length = N_FFT - start_re, pos_length = nb_re_pusch - neg_length;
      memcpy(rxF_ext, &rxF[start_re], neg_length * sizeof(c16_t));
      memcpy(&rxF_ext[neg_length], rxF, pos_length * sizeof(c16_t));
    }
    memcpy(ul_ch0_ext, chF, nb_re_pusch * sizeof(c16_t));
  } else {
    const c16_t *rxF32 = &rxF[start_re];
    if (start_re + nb_re_pusch < N_FFT) {
      for (int idx = 1 - delta; idx < nb_re_pusch; idx += 2) {
        *rxF_ext++ = rxF32[idx];
        *ul_ch0_ext++ = chF[idx];
      }
    } else {
      const int neg_length = N_FFT - start_re, pos_length = nb_re_pusch - neg_length;
      int idx, idx2;
      for (idx = 1 - delta; idx < neg_length; idx += 2) {
        *rxF_ext++ = rxF32[idx];
        *ul_ch0_ext++ = chF[idx];
      }
      rxF32 = rxF;
      idx2 = idx;
      for (idx = 1 - delta; idx < pos_length; idx += 2, idx2 += 2) {
        *rxF_ext++ = rxF32[idx];
        *ul_ch0_ext++ = chF[idx2];
      }
    }
  }
}

static int nb_re_of(int nb_rb, int is_dmrs, int cdm) { return is_dmrs ? nb_rb * (12 - cdm * 6) : nb_rb * 12; }

int nr_gpu_pass_ul_ref_eq(int nbRx, int nb_rb, int qm, int keep, int delta, int start_symbol, int nsym, uint32_t dmrs_mask, int cdm, int start_re, const c16_t *rxF /* [nbRx][14][N] */,
                          const c16_t *est /* [nbRx][14][N], the chest of every DM-RS symbol (delta realigned) */, int16_t *llr /* not descrambled */, int *log2_maxh_out, int *avg_out)
{
  const int end = start_symbol + nsym;
  int meas = -1, nb_re_meas = 0;
  for (int m = start_symbol; m < end; m++)
    if ((nb_re_meas = nb_re_of(nb_rb, dmrs_mask >> m & 1, cdm)) > 0) {
      meas = m;
      break;
    }
  if (meas < 0)
    return -1;
  const int len16 = (nb_re_meas + 15) & ~15;
  /* channel level of the measurement symbol: nr_channel_level over the extraction padded with zeros to a multiple of 16 */
  const int size_est = len16 * 14;
  int(*ext)[size_est] = calloc((size_t)nbRx, sizeof(*ext));
  for (int a = 0; a < nbRx; a++) {
    c16_t scratch[N_FFT];
    extract(&rxF[((size_t)a * 14 + meas) * N_FFT], &est[((size_t)a * 14 + get_valid_dmrs_idx_for_channel_est((uint16_t)dmrs_mask, (uint8_t)meas)) * N_FFT], scratch,
            (c16_t *)&ext[a][meas * len16], dmrs_mask >> meas & 1, delta, start_re, nb_rb, 0);
  }
  int32_t avg[4] = {0};
  nr_channel_level(meas, size_est, (const c16_t(*)[size_est])ext, nbRx, 1, avg, len16);
  free(ext);
  int avgs = 0;
  for (int a = 0; a < nbRx; a++)
    if (keep < 0 || a == keep)
      avgs = avgs > avg[a] ? avgs : avg[a];
  int l2 = (log2_approx(avgs) >> 1) + 1 + log2_approx(nbRx >> 1);
  if (l2 < 0)
    l2 = 0;
  *log2_maxh_out = l2;
  for (int a = 0; a < nbRx; a++)
    avg_out[a] = (keep < 0 || a == keep) ? avg[a] : 0;
  /* per symbol: extraction, branch zeroing, compensation, LLRs (inner_rx) */
  uint32_t off = 0;
  const int buffer_length = ((nb_rb * 12) + 15) & ~15;
  for (int m = start_symbol; m < end; m++) {
    const int is_dmrs = dmrs_mask >> m & 1;
    const int nre = nb_re_of(nb_rb, is_dmrs, cdm);
    if (nre <= 0)
      continue;
    const int dm = get_valid_dmrs_idx_for_channel_est((uint16_t)dmrs_mask, (uint8_t)m);
    c16_t rxFext[nbRx][buffer_length] __attribute__((aligned(64)));
    c16_t chFext[1][nbRx][buffer_length] __attribute__((aligned(64)));
    c16_t maga[1][buffer_length] __attribute__((aligned(64))), magb[1][buffer_length] __attribute__((aligned(64))), magc[1][buffer_length] __attribute__((aligned(64)));
    c16_t compbuf[buffer_length] __attribute__((aligned(64)));
    memset(rxFext, 0, sizeof(rxFext));
    memset(chFext, 0, sizeof(chFext));
    memset(compbuf, 0, sizeof(compbuf));
    memset(maga, 0, sizeof(maga));
    memset(magb, 0, sizeof(magb));
    memset(magc, 0, sizeof(magc));
    for (int a = 0; a < nbRx; a++) {
      extract(&rxF[((size_t)a * 14 + m) * N_FFT], &est[((size_t)a * 14 + dm) * N_FFT], rxFext[a], chFext[0][a], is_dmrs, delta, start_re, nb_rb, 0);
      if (keep >= 0 && a != keep)
        memset(chFext[0][a], 0, sizeof(chFext[0][a]));
    }
    c16_t *rxComp[1] = {compbuf};
    nr_channel_compensation(buffer_length, buffer_length, nbRx, 1, rxFext, chFext, maga, magb, magc, rxComp, NULL, qm, 0, (uint32_t)l2);
    nr_compute_llr(compbuf, maga[0], magb[0], magc[0], llr + off, (uint32_t)nre, (uint8_t)m, (uint8_t)qm);
    off += (uint32_t)nre * qm;
  }
  return (int)off;
}
