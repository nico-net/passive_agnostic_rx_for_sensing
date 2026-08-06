/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief Top-level routines for generating and decoding  the PBCH/BCH physical/transport channel V8.6 2009-03
 */
#include "PHY/defs_nr_UE.h"
#include "PHY/CODING/coding_extern.h"
#include "PHY/sse_intrin.h"
#include "PHY/INIT/nr_phy_init.h"
#include "openair1/SCHED_NR_UE/defs.h"
#include <openair1/PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h>
#include <openair1/PHY/TOOLS/phy_scope_interface.h>
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "openair1/PHY/NR_REFSIG/nr_refsig.h"
#include "openair1/PHY/NR_UE_ESTIMATION/nr_estimation.h"
#include "bits.h"
#include "instrumentation.h"
//#define DEBUG_PBCH
//#define DEBUG_PBCH_ENCODING

#define PBCH_A 24
#define PBCH_MAX_RE (PBCH_MAX_RE_PER_SYMBOL*4)
#define print_shorts(s,x) printf("%s : %d,%d,%d,%d,%d,%d,%d,%d\n",s,((int16_t*)x)[0],((int16_t*)x)[1],((int16_t*)x)[2],((int16_t*)x)[3],((int16_t*)x)[4],((int16_t*)x)[5],((int16_t*)x)[6],((int16_t*)x)[7])

static uint16_t nr_pbch_extract(const NR_DL_FRAME_PARMS *frame_parms,
                                const c16_t rxdataF[][frame_parms->ofdm_symbol_size],
                                const c16_t dl_ch_estimates[][frame_parms->ofdm_symbol_size],
                                struct complex16 rxdataF_ext[][PBCH_MAX_RE_PER_SYMBOL],
                                struct complex16 dl_ch_estimates_ext[][PBCH_MAX_RE_PER_SYMBOL],
                                uint32_t symbol,
                                uint32_t s_offset,
                                int ssb_start_subcarrier,
                                int nid)
{
  uint16_t rb;
  uint8_t i, j, aarx;
  int nushiftmod4 = nid % 4;
  AssertFatal(symbol>=1 && symbol<5,
              "symbol %d illegal for PBCH extraction\n",
              symbol);

  for (aarx=0; aarx<frame_parms->nb_antennas_rx; aarx++) {
    unsigned int rx_offset = frame_parms->first_carrier_offset + ssb_start_subcarrier;
    rx_offset = (rx_offset)%(frame_parms->ofdm_symbol_size);
    const struct complex16 *rxF = rxdataF[aarx];
    struct complex16 *rxF_ext = rxdataF_ext[aarx];
#ifdef DEBUG_PBCH
    printf("extract_rbs (nushift %d): rx_offset=%d, symbol %u\n",
           nushiftmod4,
           (rx_offset + ((symbol+s_offset) * (frame_parms->ofdm_symbol_size))),
           symbol);
    int16_t *p = (int16_t *)rxF;

    for (int i =0; i<8; i++) {
      printf("rxF.r [%d]= %d rxF.i [%d]= %d\n", i, rxF[i].r, i, rxF[i].i);
      printf("pbch extract rxF  %d %d addr %p\n", p[2*i], p[2*i+1], &p[2*i]);
    }

#endif

    for (rb=0; rb<20; rb++) {
      j=0;

      if (symbol==1 || symbol==3) {
        for (i=0; i<12; i++) {
          if ((i!=nushiftmod4) &&
              (i!=(nushiftmod4+4)) &&
              (i!=(nushiftmod4+8))) {
            rxF_ext[j]=rxF[rx_offset];
#ifdef DEBUG_PBCH
            printf("rxF ext[%d] = (%d,%d) rxF [%u]= (%d,%d)\n",
		   (9 * rb) + j,
                   rxF_ext[j].r,
                   rxF_ext[j].i,
                   rx_offset,
                   rxF[rx_offset].r,
                   rxF[rx_offset].i);
#endif
            j++;
          }

          rx_offset=(rx_offset+1)%(frame_parms->ofdm_symbol_size);
          //rx_offset = (rx_offset >= frame_parms->ofdm_symbol_size) ? (rx_offset - frame_parms->ofdm_symbol_size + 1) : (rx_offset+1);
        }

        rxF_ext+=9;
      } else { //symbol 2
        if ((rb < 4) || (rb >15)) {
          for (i=0; i<12; i++) {
            if ((i!=nushiftmod4) &&
                (i!=(nushiftmod4+4)) &&
                (i!=(nushiftmod4+8))) {
              rxF_ext[j]=rxF[rx_offset];
#ifdef DEBUG_PBCH
              printf("rxF ext[%d] = (%d,%d) rxF [%u]= (%d,%d)\n",
                     (rb < 4) ? (9 * rb) + j : (9 * (rb - 12)) + j,
		     rxF_ext[j].r,
                     rxF_ext[j].i,
                     rx_offset,
		     rxF[rx_offset].r,
                     rxF[rx_offset].i);
#endif
              j++;
            }

            rx_offset=(rx_offset+1)%(frame_parms->ofdm_symbol_size);
            //rx_offset = (rx_offset >= frame_parms->ofdm_symbol_size) ? (rx_offset - frame_parms->ofdm_symbol_size + 1) : (rx_offset+1);
          }

          rxF_ext+=9;
        } else { //rx_offset = (rx_offset >= frame_parms->ofdm_symbol_size) ? (rx_offset - frame_parms->ofdm_symbol_size + 12) : (rx_offset+12);
          rx_offset = (rx_offset+12)%(frame_parms->ofdm_symbol_size);
        }
      }
    }

    const struct complex16 *dl_ch0 = dl_ch_estimates[aarx];

    //printf("dl_ch0 addr %p\n",dl_ch0);
    struct complex16 *dl_ch0_ext = dl_ch_estimates_ext[aarx];

    for (rb=0; rb<20; rb++) {
      j=0;

      if (symbol==1 || symbol==3) {
        for (i=0; i<12; i++) {
          if ((i!=nushiftmod4) &&
              (i!=(nushiftmod4+4)) &&
              (i!=(nushiftmod4+8))) {
            dl_ch0_ext[j]=dl_ch0[i];
#ifdef DEBUG_PBCH
            if ((rb == 0) && (i < 2))
              printf("dl ch0 ext[%d] = (%d,%d)  dl_ch0 [%d]= (%d,%d)\n",
                     j,
                     dl_ch0_ext[j].r,
                     dl_ch0_ext[j].i,
                     i,
                     dl_ch0[j].r,
                     dl_ch0[j].i);
#endif
            j++;
          }
        }

        dl_ch0+=12;
        dl_ch0_ext+=9;
      } else {
        if ((rb < 4) || (rb >15)) {
          for (i=0; i<12; i++) {
            if ((i!=nushiftmod4) &&
                (i!=(nushiftmod4+4)) &&
                (i!=(nushiftmod4+8))) {
              dl_ch0_ext[j]=dl_ch0[i];
#ifdef DEBUG_PBCH
              printf("dl ch0 ext[%d] = (%d,%d)  dl_ch0 [%d]= (%d,%d)\n",
                     j,
                     dl_ch0_ext[j].r,
                     dl_ch0_ext[j].i,
                     i,
                     dl_ch0[j].r,
                     dl_ch0[j].i);
#endif
              j++;
            }
          }

          dl_ch0_ext+=9;
        }

        dl_ch0+=12;
      }
    }
  }

  return(0);
}

void nr_pbch_channel_compensation(const struct complex16 rxdataF_ext[][PBCH_MAX_RE_PER_SYMBOL],
                                  const struct complex16 dl_ch_estimates_ext[][PBCH_MAX_RE_PER_SYMBOL],
                                  int nb_re,
                                  struct complex16 rxdataF_comp[][PBCH_MAX_RE_PER_SYMBOL],
                                  const NR_DL_FRAME_PARMS *frame_parms,
                                  uint8_t output_shift)
{
  for (int aarx=0; aarx<frame_parms->nb_antennas_rx; aarx++) {
    mult_cpx_conj_vector((c16_t *)dl_ch_estimates_ext[aarx],
                         (c16_t *)rxdataF_ext[aarx],
                         (c16_t *)rxdataF_comp[aarx],
                         nb_re,
                         output_shift);
  }
}

void nr_pbch_detection_mrc(NR_DL_FRAME_PARMS *frame_parms,
                           int **rxdataF_comp,
                           uint8_t symbol) {
  uint8_t symbol_mod;
  int i, nb_rb = 6;
  simde__m128i *rxdataF_comp128_0, *rxdataF_comp128_1;
  symbol_mod = (symbol>=(7-frame_parms->Ncp)) ? symbol-(7-frame_parms->Ncp) : symbol;

  if (frame_parms->nb_antennas_rx > 1) {
    rxdataF_comp128_0 = (simde__m128i *)&rxdataF_comp[0][symbol_mod * 6 * 12];
    rxdataF_comp128_1 = (simde__m128i *)&rxdataF_comp[1][symbol_mod * 6 * 12];

    // MRC on each re of rb, both on MF output and magnitude (for 16QAM/64QAM llr computation)
    for (i = 0; i < nb_rb * 3; i++) {
      rxdataF_comp128_0[i] =
          simde_mm_adds_epi16(simde_mm_srai_epi16(rxdataF_comp128_0[i], 1), simde_mm_srai_epi16(rxdataF_comp128_1[i], 1));
    }
  }

}

void nr_pbch_unscrambling(int16_t *demod_pbch_e,
                          uint16_t Nid,
                          uint8_t nushift,
                          uint16_t M,
                          uint16_t length,
                          uint8_t bitwise,
                          uint32_t unscrambling_mask,
                          uint32_t pbch_a_prime,
                          uint32_t *pbch_a_interleaved)
{
  uint32_t *seq = gold_cache(Nid, (nushift * M + length + 31) / 32); // this is c_init
  // The Gold sequence is shifted by nushift* M, so we skip (nushift*M /32) double words
  int idxGold = (nushift * M + 31) / 32 - 1;

  // Scrambling is now done with offset (nushift*M)%32
  int offset = (nushift * M) & 0x1f;
  uint8_t k = 0;
  for (int i = 0; i < length; i++) {
    if (bitwise) {
      if (((k + offset) & 0x1f) == 0 && (!((unscrambling_mask >> i) & 1)))
        idxGold++;
      *pbch_a_interleaved ^= ((unscrambling_mask >> i) & 1)
                                 ? ((pbch_a_prime >> i) & 1) << i
                                 : (((pbch_a_prime >> i) & 1) ^ ((seq[idxGold] >> ((k + offset) & 0x1f)) & 1)) << i;
      k += (!((unscrambling_mask >> i) & 1));
    } else {
      if (((i + offset) & 0x1f) == 0)
        idxGold++;

      if (seq[idxGold] & (1UL << ((i + offset) % 32)))
        demod_pbch_e[i] = -demod_pbch_e[i];

#ifdef DEBUG_PBCH_ENCODING

      if (i<8)
        printf("s %d demod_pbch_e[i] %d\n", ((s>>((i+offset)&0x1f))&1), demod_pbch_e[i]);

#endif
    }
  }
}

/* TEMPORARY DIAGNOSTIC (intermittent PBCH tracking failure, 2026-08-02).
 * Per-SSB-occasion state, recorded unconditionally in nr_generate_pbch_llr() and printed ONLY when
 * nr_pbch_decode() fails -- alongside the last SUCCESSFUL occasion's state, which is the
 * comparison that matters given the failure is intermittent rather than bandwidth-determined.
 * __thread because the DL actors are several threads and this must not race. */
typedef struct {
  int valid;
  int is_track;
  double log2_maxh;
  double rawmean;
  int rawmax;
  int presat;
  int nb;
  double hmean;
  int hmax;
  int nb_re;
} pbch_sym_diag_t;

static __thread pbch_sym_diag_t g_pbch_diag[4];
static __thread pbch_sym_diag_t g_pbch_last_good[4];
static __thread int g_pbch_have_good;

/* Cross-thread reference, deliberately NOT __thread: acquisition and tracking run on different
 * threads, and in a failing run tracking never succeeds, so the only "known good" decode available
 * to compare against is acquisition's. Benign last-writer-wins race, acceptable for a diagnostic. */
static pbch_sym_diag_t g_pbch_ref[4];
static volatile int g_pbch_have_ref;

void nr_pbch_diag_snapshot_reference(void)
{
  memcpy(g_pbch_ref, g_pbch_diag, sizeof(g_pbch_ref));
  g_pbch_have_ref = 1;
}

void nr_pbch_diag_report(int success, int frame, int slot, int ssbIndex)
{
  static __thread int nfail;
  if (success) {
    memcpy(g_pbch_last_good, g_pbch_diag, sizeof(g_pbch_last_good));
    g_pbch_have_good = 1;
    return;
  }
  if (nfail >= 6)
    return;
  nfail++;
  LOG_E(PHY, "PBCHFAIL #%d frame=%d slot=%d ssb=%d have_reference_success=%d\n", nfail, frame, slot, ssbIndex, g_pbch_have_good);
  for (int s = 1; s <= 3; s++) {
    const pbch_sym_diag_t *f = &g_pbch_diag[s];
    const pbch_sym_diag_t *g = &g_pbch_last_good[s];
    LOG_E(PHY,
          "  sym%d FAIL track=%d log2_maxh=%.1f |H|mean=%.1f |H|max=%d rawmean=%.1f rawmax=%d sat=%d/%d\n",
          s, f->is_track, f->log2_maxh, f->hmean, f->hmax, f->rawmean, f->rawmax, f->presat, f->nb);
    if (g_pbch_have_good && g->valid)
      LOG_E(PHY,
            "  sym%d GOOD track=%d log2_maxh=%.1f |H|mean=%.1f |H|max=%d rawmean=%.1f rawmax=%d sat=%d/%d\n",
            s, g->is_track, g->log2_maxh, g->hmean, g->hmax, g->rawmean, g->rawmax, g->presat, g->nb);
    const pbch_sym_diag_t *r = &g_pbch_ref[s];
    if (g_pbch_have_ref && r->valid)
      LOG_E(PHY,
            "  sym%d ACQREF track=%d log2_maxh=%.1f |H|mean=%.1f |H|max=%d rawmean=%.1f rawmax=%d sat=%d/%d\n",
            s, r->is_track, r->log2_maxh, r->hmean, r->hmax, r->rawmean, r->rawmax, r->presat, r->nb);
  }
}

/* ---- SHARED FRAME-WIDE SSB SEARCH (2026-08-06) -------------------------------------------------
 * Factored out of phy_procedures_nr_ue.c's tracking-side FRAMESCAN so the IDENTICAL scoring can be
 * run against acquisition's own known-good buffer. Reason this matters: FRAMESCAN's "no SSB
 * anywhere in the tracking buffer" was never validated against a buffer KNOWN to contain one, so a
 * bug in the search itself was indistinguishable from a genuinely empty buffer.
 *
 * It also sweeps a CFO derotation, because the two paths were NOT comparable as originally written:
 * nr_initial_sync.c calls compensate_freq_offset() on its private copy BEFORE measuring, so
 * acquisition's ~19 dB figure is from a CFO-CORRECTED buffer, while tracking's FRAMESCAN reads the
 * raw live buffer. At this cell's measured -15204 Hz that is 0.507 x the 30 kHz SCS -- worst-case
 * half-subcarrier straddle -- so an uncorrected buffer can score near-noise while holding a
 * perfectly good SSB. cfo_hz_span=0 disables the sweep (single pass at 0 Hz).
 */
void nr_isac_framescan(const c16_t *rxd,
                       unsigned int total_samples,
                       int N,
                       int start_bin,
                       int sym_stride,
                       int nsym,
                       long ref_offset,
                       double sampling_rate,
                       double cfo_hz_span,
                       double cfo_hz_step,
                       const char *label)
{
  if (!rxd || N <= 0 || sym_stride <= 0 || nsym <= 0 || total_samples == 0)
    return;

  const dft_size_idx_t dsz = get_dft(N);
  c16_t *win = (c16_t *)malloc16(sizeof(c16_t) * N);
  c16_t *spec = (c16_t *)malloc16(sizeof(c16_t) * N);
  if (!win || !spec) {
    if (win)
      free16(win, sizeof(c16_t) * N);
    if (spec)
      free16(spec, sizeof(c16_t) * N);
    return;
  }

  const int nsteps = (cfo_hz_span > 0.0 && cfo_hz_step > 0.0) ? (2 * (int)(cfo_hz_span / cfo_hz_step) + 1) : 1;
  double glob_best_ratio = -1.0;
  long glob_best_off = -1;
  double glob_best_cfo = 0.0;

  for (int c = 0; c < nsteps; c++) {
    const double cfo = (nsteps == 1) ? 0.0 : (-cfo_hz_span + c * cfo_hz_step);
    double best_ratio = -1.0;
    long best_off = -1;

    for (int k = 0; k < nsym; k++) {
      const long base = ((long)k * sym_stride) % (long)total_samples;
      for (int i = 0; i < N; i++) {
        const c16_t v = rxd[(unsigned int)((base + i) % (long)total_samples)];
        if (cfo == 0.0) {
          win[i] = v;
        } else {
          /* Derotate in time domain: the only place a CFO correction can remove ICI. Phase
           * referenced to this window's own start, matching what the FFT below assumes. */
          const double ph = -2.0 * M_PI * cfo * ((double)i / sampling_rate);
          const double cs = cos(ph), sn = sin(ph);
          win[i].r = (int16_t)lround(v.r * cs - v.i * sn);
          win[i].i = (int16_t)lround(v.r * sn + v.i * cs);
        }
      }
      dft(dsz, (int16_t *)win, (int16_t *)spec, 1);
      double in_p = 0.0, out_p = 0.0;
      for (int i = 0; i < 240; i++) {
        const c16_t v = spec[(start_bin + i) % N];
        in_p += (double)v.r * v.r + (double)v.i * v.i;
      }
      for (int i = 0; i < 240; i++) { /* local floor: 300 bins below the SSB band */
        const c16_t v = spec[(((start_bin - 300 + i) % N) + N) % N];
        out_p += (double)v.r * v.r + (double)v.i * v.i;
      }
      const double ratio = (out_p > 0.0) ? sqrt(in_p / out_p) : 0.0;
      if (ratio > best_ratio) {
        best_ratio = ratio;
        best_off = base;
      }
    }
    if (best_ratio > glob_best_ratio) {
      glob_best_ratio = best_ratio;
      glob_best_off = best_off;
      glob_best_cfo = cfo;
    }
  }

  LOG_W(PHY,
        "SENSING: FRAMESCAN2 label=%s nsym=%d sym_stride=%d start_bin=%d ref_offset=%ld "
        "best_off=%ld best_ratio=%.2f best_cfo_hz=%.0f delta_vs_ref=%ld cfo_steps=%d\n",
        label, nsym, sym_stride, start_bin, ref_offset, glob_best_off, glob_best_ratio,
        glob_best_cfo, glob_best_off - ref_offset, nsteps);

  free16(win, sizeof(c16_t) * N);
  free16(spec, sizeof(c16_t) * N);
}

/* ---- GOLDEN-BUFFER PBCH REPLAY (2026-08-06) ----------------------------------------------------
 * Runs the FULL PBCH chain (FFT -> nr_pbch_channel_estimation -> nr_generate_pbch_llr ->
 * nr_pbch_decode) against the exact samples acquisition just decoded successfully, sweeping the FFT
 * window offset. Purpose, and why this beats another live diagnostic: acquisition decodes and
 * tracking does not, so the decisive question is whether the difference is WHERE the FFT window is
 * placed or WHAT happens after it. Here the samples are held fixed and known-good, so:
 *   - delta=0 must decode. If it does not, the fault is in the chain (chest / LLR / decode
 *     parameters), NOT in sample selection, and every timing hypothesis is dead.
 *   - if it decodes only in a narrow delta window, that window IS the alignment tolerance, and
 *     tracking's actual offset can be compared against it directly.
 * Deliberately uses acquisition's own uniform-stride window (ofdm_symbol_size + nb_prefix_samples),
 * matching do_time_to_freq(), so delta is measured against a reference that is known to work.
 */
typedef struct {
  int valid;
  int nb_ant;
  int nsamp;
  int ssb_time_offset;
  int nid_cell;
  int i_ssb;
  int n_hf;
  int ssb_start_subcarrier;
  int ofdm_symbol_size;
  int nb_prefix_samples;
  c16_t *rxdata[4];
} pbch_snapshot_t;

static pbch_snapshot_t g_pbch_golden; /* acquisition: samples KNOWN to decode */
static pbch_snapshot_t g_pbch_live;   /* one live tracking occasion, captured verbatim */

void nr_pbch_golden_capture(int nb_ant,
                            int nsamp,
                            c16_t *const *rxdata,
                            int ssb_time_offset,
                            int nid_cell,
                            int i_ssb,
                            int n_hf,
                            int ssb_start_subcarrier,
                            int ofdm_symbol_size,
                            int nb_prefix_samples)
{
  if (g_pbch_golden.valid || nb_ant <= 0 || nb_ant > 4 || nsamp <= 0)
    return;
  for (int a = 0; a < nb_ant; a++) {
    g_pbch_golden.rxdata[a] = (c16_t *)malloc16(sizeof(c16_t) * nsamp);
    if (!g_pbch_golden.rxdata[a]) {
      for (int b = 0; b < a; b++)
        free16(g_pbch_golden.rxdata[b], sizeof(c16_t) * nsamp);
      return;
    }
    memcpy(g_pbch_golden.rxdata[a], rxdata[a], sizeof(c16_t) * nsamp);
  }
  g_pbch_golden.nb_ant = nb_ant;
  g_pbch_golden.nsamp = nsamp;
  g_pbch_golden.ssb_time_offset = ssb_time_offset;
  g_pbch_golden.nid_cell = nid_cell;
  g_pbch_golden.i_ssb = i_ssb;
  g_pbch_golden.n_hf = n_hf;
  g_pbch_golden.ssb_start_subcarrier = ssb_start_subcarrier;
  g_pbch_golden.ofdm_symbol_size = ofdm_symbol_size;
  g_pbch_golden.nb_prefix_samples = nb_prefix_samples;
  g_pbch_golden.valid = 1;
  LOG_W(PHY,
        "SENSING: PBCHGOLDEN captured nb_ant=%d nsamp=%d ssb_time_offset=%d nid=%d i_ssb=%d n_hf=%d "
        "ssb_sc=%d N=%d cp=%d\n",
        nb_ant, nsamp, ssb_time_offset, nid_cell, i_ssb, n_hf, ssb_start_subcarrier,
        ofdm_symbol_size, nb_prefix_samples);
}

/* Capture ONE live tracking occasion verbatim: the time-domain samples the live path is about to
 * process, plus the live state it will process them with. The live ssb_time_offset is expressed in
 * the SAME convention as the acquisition snapshot (start of SSB symbol 0), derived from the
 * rx_offset nr_slot_fep actually used for PBCH symbol 1, so one replay loop serves both. */
void nr_pbch_live_capture(int nb_ant,
                          int nsamp,
                          c16_t *const *rxdata,
                          int fep_rx_offset_sym1,
                          int nid_cell,
                          int i_ssb,
                          int n_hf,
                          int ssb_start_subcarrier,
                          int ofdm_symbol_size,
                          int nb_prefix_samples)
{
  if (g_pbch_live.valid || nb_ant <= 0 || nb_ant > 4 || nsamp <= 0)
    return;
  for (int a = 0; a < nb_ant; a++) {
    g_pbch_live.rxdata[a] = (c16_t *)malloc16(sizeof(c16_t) * nsamp);
    if (!g_pbch_live.rxdata[a]) {
      for (int b = 0; b < a; b++)
        free16(g_pbch_live.rxdata[b], sizeof(c16_t) * nsamp);
      return;
    }
    memcpy(g_pbch_live.rxdata[a], rxdata[a], sizeof(c16_t) * nsamp);
  }
  const int stride = ofdm_symbol_size + nb_prefix_samples;
  g_pbch_live.nb_ant = nb_ant;
  g_pbch_live.nsamp = nsamp;
  /* replay computes base = ssb_time_offset + s*stride + cp; make s=1 land on the live offset */
  g_pbch_live.ssb_time_offset = fep_rx_offset_sym1 - stride - nb_prefix_samples;
  g_pbch_live.nid_cell = nid_cell;
  g_pbch_live.i_ssb = i_ssb;
  g_pbch_live.n_hf = n_hf;
  g_pbch_live.ssb_start_subcarrier = ssb_start_subcarrier;
  g_pbch_live.ofdm_symbol_size = ofdm_symbol_size;
  g_pbch_live.nb_prefix_samples = nb_prefix_samples;
  g_pbch_live.valid = 1;
  LOG_W(PHY,
        "SENSING: PBCHLIVE captured nb_ant=%d nsamp=%d fep_rx_offset=%d -> ssb_time_offset=%d "
        "nid=%d i_ssb=%d n_hf=%d ssb_sc=%d N=%d cp=%d\n",
        nb_ant, nsamp, fep_rx_offset_sym1, g_pbch_live.ssb_time_offset, nid_cell, i_ssb, n_hf,
        ssb_start_subcarrier, ofdm_symbol_size, nb_prefix_samples);
}

static uint32_t fnv1a_c16(const c16_t *v, int n)
{
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) {
    const uint8_t *p = (const uint8_t *)&v[i];
    for (unsigned k = 0; k < sizeof(c16_t); k++) {
      h ^= p[k];
      h *= 16777619u;
    }
  }
  return h;
}

static double rms_c16(const c16_t *v, int n)
{
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += (double)v[i].r * v[i].r + (double)v[i].i * v[i].i;
  return (n > 0) ? sqrt(s / n) : 0.0;
}

/* ---- 2x2 CROSS-REPLAY: samples x parameters (2026-08-06) ---------------------------------------
 * Separates two claims that a timing measurement alone cannot: whether the LIVE SAMPLES carry a
 * decodable PBCH, and whether the LIVE STATE is correct. Runs one chain over all four crossings and
 * hashes every stage, so the FIRST differing stage is the bug boundary.
 *
 *   golden samples + golden params -> control, must pass
 *   golden samples + live params   -> isolates tracking metadata/state
 *   live samples   + golden params -> isolates the live samples
 *   live samples   + live params   -> reproduces the real occasion offline
 *
 * If the last one PASSES offline while the online occasion failed, the fault is not in samples or
 * parameters at all but in something only the live path has: a race, buffer reuse/aliasing, or
 * shared scratch state.
 */
static void pbch_replay_one(const NR_DL_FRAME_PARMS *fp,
                            const UE_nr_rxtx_proc_t *proc,
                            const pbch_snapshot_t *samples,
                            const pbch_snapshot_t *params,
                            const char *label)
{
  if (!samples->valid || !params->valid)
    return;
  const int N = params->ofdm_symbol_size;
  const int cp = params->nb_prefix_samples;
  const int stride = N + cp;
  const int nb_ant = samples->nb_ant < params->nb_ant ? samples->nb_ant : params->nb_ant;
  const dft_size_idx_t dsz = get_dft(N);

  int16_t pbch_e_rx[NR_POLAR_PBCH_E];
  double pbch_log2_maxh = -1.0;
  uint32_t h_time = 0, h_fft = 0, h_chest = 0;
  double r_time = 0.0, r_fft = 0.0, r_chest = 0.0;

  for (int s = 1; s <= 3; s++) {
    const long base = (long)params->ssb_time_offset + (long)s * stride + cp;
    if (base < 0 || base + N > samples->nsamp) {
      LOG_W(PHY, "SENSING: PBCH2X2 label=%s SKIPPED (base %ld out of range, nsamp=%d)\n", label, base,
            samples->nsamp);
      return;
    }
    __attribute__((aligned(32))) c16_t rxdataF[nb_ant][N];
    __attribute__((aligned(32))) c16_t dl_ch_estimates[nb_ant][N];
    for (int a = 0; a < nb_ant; a++)
      dft(dsz, (int16_t *)&samples->rxdata[a][base], (int16_t *)rxdataF[a], 1);

    for (int a = 0; a < nb_ant; a++)
      nr_pbch_channel_estimation(fp, NULL, dl_ch_estimates[a], proc, s - 1, params->i_ssb, params->n_hf,
                                 params->ssb_start_subcarrier, rxdataF[a], false, params->nid_cell);

    if (s == 1) { /* hash one representative symbol's stages */
      h_time = fnv1a_c16(&samples->rxdata[0][base], N);
      r_time = rms_c16(&samples->rxdata[0][base], N);
      h_fft = fnv1a_c16(rxdataF[0], N);
      r_fft = rms_c16(rxdataF[0], N);
      h_chest = fnv1a_c16(dl_ch_estimates[0], N);
      r_chest = rms_c16(dl_ch_estimates[0], N);
    }

    nr_generate_pbch_llr(NULL, proc, fp, s, params->i_ssb, params->nid_cell, params->ssb_start_subcarrier,
                         rxdataF, dl_ch_estimates, pbch_e_rx, &pbch_log2_maxh);
  }

  double llr_absmean = 0.0;
  for (int i = 0; i < NR_POLAR_PBCH_E; i++)
    llr_absmean += fabs((double)pbch_e_rx[i]);
  llr_absmean /= NR_POLAR_PBCH_E;
  uint32_t h_llr = 2166136261u;
  for (int i = 0; i < NR_POLAR_PBCH_E; i++) {
    h_llr ^= (uint8_t)(pbch_e_rx[i] & 0xff);
    h_llr *= 16777619u;
  }

  fapiPbch_t res;
  int hfb = 0, ssb_idx = 0, sym_off = 0;
  const int crc_ok =
      (0 == nr_pbch_decode(NULL, fp, proc, params->i_ssb, params->nid_cell, pbch_e_rx, &hfb, &ssb_idx, &sym_off, &res));

  LOG_W(PHY,
        "SENSING: PBCH2X2 label=%-22s crc_ok=%d llr_absmean=%.2f log2_maxh=%.2f | "
        "time[h=%08x rms=%.1f] fft[h=%08x rms=%.1f] chest[h=%08x rms=%.1f] llr[h=%08x]\n",
        label, crc_ok, llr_absmean, pbch_log2_maxh, h_time, r_time, h_fft, r_fft, h_chest, r_chest, h_llr);
}

/* ---- PER-ANTENNA MASKED DECODE (2026-08-06) ----------------------------------------------------
 * Decodes ONE immutable snapshot repeatedly using only the antennas selected by a bitmask, so the
 * contribution of each RF branch is measured rather than argued. Motivation: acquisition CHRMS
 * shows only channel 2 carries signal (ch0/1/3 sit at the noise floor), and acquisition succeeds
 * while tracking fails -- but three noise branches do NOT by themselves prove an MRC bug. A
 * correctly weighted combiner should suppress branches with weak channel estimates. This measures
 * which is true.
 *
 * A local copy of the frame params carries nb_antennas_rx = popcount(mask), and the selected
 * antennas are packed into the low indices, so every downstream function (channel estimation, the
 * extract/compensate/MRC chain inside nr_generate_pbch_llr) sees a consistent, genuinely smaller
 * antenna set rather than zeroed branches -- zeroing would still be accumulated and would test a
 * different thing.
 */
static int pbch_replay_mask(const NR_DL_FRAME_PARMS *fp,
                            const UE_nr_rxtx_proc_t *proc,
                            const pbch_snapshot_t *snap,
                            unsigned mask,
                            const char *label)
{
  if (!snap->valid)
    return -1;
  int sel[4], nsel = 0;
  for (int a = 0; a < snap->nb_ant && a < 4; a++)
    if (mask & (1u << a))
      sel[nsel++] = a;
  if (nsel == 0)
    return -1;

  NR_DL_FRAME_PARMS fp2 = *fp;
  fp2.nb_antennas_rx = nsel;

  const int N = snap->ofdm_symbol_size;
  const int cp = snap->nb_prefix_samples;
  const int stride = N + cp;
  const dft_size_idx_t dsz = get_dft(N);

  // Full per-mask reset: the LLR codeword, the shared channel-compensation shift, and the
  // per-antenna power record. nsel changes between masks, so any carry-over would silently
  // contaminate the next mask -- exactly the "results depend on mask order" failure mode.
  int16_t pbch_e_rx[NR_POLAR_PBCH_E];
  memset(pbch_e_rx, 0, sizeof(pbch_e_rx));
  double pbch_log2_maxh = -1.0;
  double chpow[4] = {0};

  for (int s = 1; s <= 3; s++) {
    const long base = (long)snap->ssb_time_offset + (long)s * stride + cp;
    if (base < 0 || base + N > snap->nsamp)
      return -1;
    __attribute__((aligned(32))) c16_t rxdataF[nsel][N];
    __attribute__((aligned(32))) c16_t dl_ch_estimates[nsel][N];
    memset(rxdataF, 0, sizeof(rxdataF));
    memset(dl_ch_estimates, 0, sizeof(dl_ch_estimates));
    for (int k = 0; k < nsel; k++)
      dft(dsz, (int16_t *)&snap->rxdata[sel[k]][base], (int16_t *)rxdataF[k], 1);
    for (int k = 0; k < nsel; k++) {
      nr_pbch_channel_estimation(&fp2, NULL, dl_ch_estimates[k], proc, s - 1, snap->i_ssb, snap->n_hf,
                                 snap->ssb_start_subcarrier, rxdataF[k], false, snap->nid_cell);
      if (s == 1)
        chpow[k] = rms_c16(dl_ch_estimates[k], N);
    }
    nr_generate_pbch_llr(NULL, proc, &fp2, s, snap->i_ssb, snap->nid_cell, snap->ssb_start_subcarrier,
                         rxdataF, dl_ch_estimates, pbch_e_rx, &pbch_log2_maxh);
  }

  double llr_absmean = 0.0;
  for (int i = 0; i < NR_POLAR_PBCH_E; i++)
    llr_absmean += fabs((double)pbch_e_rx[i]);
  llr_absmean /= NR_POLAR_PBCH_E;

  fapiPbch_t res;
  int hfb = 0, ssb_idx = 0, sym_off = 0;
  const int crc_ok =
      (0 == nr_pbch_decode(NULL, fp, proc, snap->i_ssb, snap->nid_cell, pbch_e_rx, &hfb, &ssb_idx, &sym_off, &res));

  char cb[128];
  int p = 0;
  for (int k = 0; k < nsel; k++)
    p += snprintf(cb + p, sizeof(cb) - p, " ch%d_chest_rms=%.1f", sel[k], chpow[k]);
  LOG_W(PHY, "SENSING: PBCHMASK %-14s mask=0x%x nant=%d crc_ok=%d llr_absmean=%.2f log2_maxh=%.2f |%s\n",
        label, mask, nsel, crc_ok, llr_absmean, pbch_log2_maxh, cb);
  return crc_ok;
}


/* Normalized DMRS-match metric. chest_rms only says the estimator produced energy; it cannot
 * distinguish a correct DMRS sequence from a wrong one, because Y*conj(X) has similar magnitude
 * either way. This measures whether the resulting channel is SMOOTH across subcarriers -- the
 * defining property of a real channel estimate, and what collapses when X is the wrong sequence.
 * |sum conj(H[k])H[k+1]| / sum |H[k]||H[k+1]| in [0,1]; ~1 = coherent, ~0 = random phase. */
static double chest_coherence(const c16_t *h, int n)
{
  double lr = 0.0, li = 0.0, den = 0.0;
  for (int k = 0; k + 1 < n; k++) {
    const double ar = h[k].r, ai = h[k].i, br = h[k + 1].r, bi = h[k + 1].i;
    lr += ar * br + ai * bi;
    li += ar * bi - ai * br;
    den += sqrt((ar * ar + ai * ai) * (br * br + bi * bi));
  }
  return (den > 0.0) ? sqrt(lr * lr + li * li) / den : 0.0;
}

/* ---- i_ssb / n_hf SWEEP (2026-08-06) ----------------------------------------------------------
 * Regenerates the ENTIRE candidate-dependent path per tuple -- PBCH DMRS, channel estimate,
 * log2_maxh/scaling, equalisation, LLRs, descrambling, polar decode, CRC mask. It deliberately does
 * NOT reuse one i_ssb=0 channel estimate and vary only the descrambling: PBCH DMRS generation is
 * itself a function of the SSB index, so reusing an estimate would test nothing.
 */
static void pbch_sweep_issb(const NR_DL_FRAME_PARMS *fp,
                            const UE_nr_rxtx_proc_t *proc,
                            const pbch_snapshot_t *snap,
                            unsigned mask,
                            const char *what)
{
  if (!snap->valid)
    return;
  int sel[4], nsel = 0;
  for (int a = 0; a < snap->nb_ant && a < 4; a++)
    if (mask & (1u << a))
      sel[nsel++] = a;
  if (nsel == 0)
    return;

  NR_DL_FRAME_PARMS fp2 = *fp;
  fp2.nb_antennas_rx = nsel;
  const int N = snap->ofdm_symbol_size;
  const int cp = snap->nb_prefix_samples;
  const int stride = N + cp;
  const dft_size_idx_t dsz = get_dft(N);
  const int n_hf_max = (fp->Lmax == 4) ? 2 : 1;

  LOG_W(PHY, "SENSING: PBCHSWEEP --- %s (mask=0x%x) Lmax=%d ---\n", what, mask, fp->Lmax);

  for (int i_ssb = 0; i_ssb < 8; i_ssb++) {
    for (int n_hf = 0; n_hf < n_hf_max; n_hf++) {
      int16_t pbch_e_rx[NR_POLAR_PBCH_E];
      memset(pbch_e_rx, 0, sizeof(pbch_e_rx));
      double pbch_log2_maxh = -1.0;
      double coh = 0.0, crms = 0.0;

      for (int s = 1; s <= 3; s++) {
        const long base = (long)snap->ssb_time_offset + (long)s * stride + cp;
        if (base < 0 || base + N > snap->nsamp)
          return;
        __attribute__((aligned(32))) c16_t rxdataF[nsel][N];
        __attribute__((aligned(32))) c16_t dl_ch[nsel][N];
        memset(rxdataF, 0, sizeof(rxdataF));
        memset(dl_ch, 0, sizeof(dl_ch));
        for (int k = 0; k < nsel; k++)
          dft(dsz, (int16_t *)&snap->rxdata[sel[k]][base], (int16_t *)rxdataF[k], 1);
        for (int k = 0; k < nsel; k++)
          nr_pbch_channel_estimation(&fp2, NULL, dl_ch[k], proc, s - 1, i_ssb, n_hf,
                                     snap->ssb_start_subcarrier, rxdataF[k], false, snap->nid_cell);
        if (s == 1) {
          coh = chest_coherence(dl_ch[0], N);
          crms = rms_c16(dl_ch[0], N);
        }
        nr_generate_pbch_llr(NULL, proc, &fp2, s, i_ssb, snap->nid_cell, snap->ssb_start_subcarrier,
                             rxdataF, dl_ch, pbch_e_rx, &pbch_log2_maxh);
      }

      double llr = 0.0;
      for (int i = 0; i < NR_POLAR_PBCH_E; i++)
        llr += fabs((double)pbch_e_rx[i]);
      llr /= NR_POLAR_PBCH_E;

      fapiPbch_t res;
      memset(&res, 0, sizeof(res));
      int hfb = 0, ssb_idx = 0, sym_off = 0;
      const int crc_ok = (0 == nr_pbch_decode(NULL, fp, proc, i_ssb, snap->nid_cell, pbch_e_rx, &hfb,
                                              &ssb_idx, &sym_off, &res));
      LOG_W(PHY,
            "SENSING: PBCHSWEEP %s i_ssb=%d n_hf=%d dmrs_coh=%.4f chest_rms=%.1f llr=%.2f "
            "log2_maxh=%.1f crc_ok=%d%s\n",
            what, i_ssb, n_hf, coh, crms, llr, pbch_log2_maxh, crc_ok,
            crc_ok ? "  <== PASS" : "");
    }
  }
}


/* ---- OFFLINE DMRS-COHERENCE LOCALISATION (2026-08-06) ------------------------------------------
 * Locates the PBCH DMRS signature in an immutable live capture, or proves it absent.
 *
 * Why this replaces the earlier occupied-band-ratio scan: on a loaded 273-PRB cell EVERY offset
 * carries band energy, so that scan locked onto a shallow, wandering maximum and was inconclusive.
 * Normalized DMRS coherence has a sharp calibrated target instead -- measured on this cell:
 *     correct DMRS 0.9999 | wrong i_ssb 0.81-0.89 | live floor 0.84-0.91
 * so a candidate near 0.90 is NOT a hit; only a sharp jump toward the golden signature counts.
 *
 * Runs on a DETACHED THREAD, never in the RT receive path.
 *
 * Requires STRUCTURE, not one high value: all three PBCH-bearing symbols must score, and the
 * candidate is scored by the MINIMUM across them (a single lucky symbol cannot carry a candidate).
 * A residual-CFO dimension is swept at the refine stage so "SSB absent" cannot be declared merely
 * because live and golden entered the harness with different CFO preprocessing.
 */
typedef struct {
  pbch_snapshot_t snap;
  NR_DL_FRAME_PARMS fp;
  UE_nr_rxtx_proc_t proc;
} pbch_scan_arg_t;

static double pbch_cand_score(const pbch_scan_arg_t *A, long off, int i_ssb, double cfo_hz, int ant,
                              double *per_sym)
{
  const pbch_snapshot_t *sn = &A->snap;
  const int N = sn->ofdm_symbol_size, cp = sn->nb_prefix_samples, stride = N + cp;
  const dft_size_idx_t dsz = get_dft(N);
  const double fs = (double)A->fp.samples_per_subframe * 1000.0;
  NR_DL_FRAME_PARMS fp1 = A->fp;
  fp1.nb_antennas_rx = 1;
  double worst = 1.0;
  for (int s = 1; s <= 3; s++) {
    const long base = off + (long)s * stride + cp;
    if (base < 0 || base + N > sn->nsamp)
      return -1.0;
    __attribute__((aligned(32))) c16_t win[N], rxF[N], H[N];
    for (int i = 0; i < N; i++) {
      const c16_t v = sn->rxdata[ant][base + i];
      if (cfo_hz == 0.0) {
        win[i] = v;
      } else {
        const double ph = -2.0 * M_PI * cfo_hz * ((double)i / fs);
        const double c = cos(ph), sn2 = sin(ph);
        win[i].r = (int16_t)lround(v.r * c - v.i * sn2);
        win[i].i = (int16_t)lround(v.r * sn2 + v.i * c);
      }
    }
    dft(dsz, (int16_t *)win, (int16_t *)rxF, 1);
    memset(H, 0, sizeof(H));
    nr_pbch_channel_estimation(&fp1, NULL, H, &A->proc, s - 1, i_ssb, sn->n_hf,
                               sn->ssb_start_subcarrier, rxF, false, sn->nid_cell);
    const double c = chest_coherence(H, N);
    if (per_sym)
      per_sym[s - 1] = c;
    if (c < worst)
      worst = c;
  }
  return worst;
}

static void *pbch_scan_thread(void *arg)
{
  pbch_scan_arg_t *A = (pbch_scan_arg_t *)arg;
  const pbch_snapshot_t *sn = &A->snap;
  const int stride = sn->ofdm_symbol_size + sn->nb_prefix_samples;
  const long span = sn->nsamp - 4L * stride;
  const int COARSE = 64;

  LOG_W(PHY, "SENSING: DMRSSCAN begin nsamp=%d span=%ld coarse_stride=%d ant=0..%d\n",
        sn->nsamp, span, COARSE, sn->nb_ant - 1);

  /* Stage 1: coarse, symbol-1 only, i_ssb=0, no CFO -- a cheap sieve, not a decision. */
  typedef struct { long off; double sc; } cand_t;
  cand_t best[8];
  int nb = 0;
  for (int a = 0; a < sn->nb_ant; a++) {
    double amax = -1.0;
    long amax_off = -1;
    for (long off = 0; off < span; off += COARSE) {
      double ps[3];
      const double sc = pbch_cand_score(A, off, 0, 0.0, a, ps);
      if (sc > amax) { amax = sc; amax_off = off; }
      if (sc > 0.95 && nb < 8) { best[nb].off = off; best[nb].sc = sc; nb++; }
    }
    LOG_W(PHY, "SENSING: DMRSSCAN coarse ant%d best_off=%ld min3_coh=%.4f\n", a, amax_off, amax);
    if (nb < 8 && amax_off >= 0) { best[nb].off = amax_off; best[nb].sc = amax; nb++; }
  }

  /* Stage 2: refine to 1 sample, sweep i_ssb and a residual-CFO grid, require all 3 symbols. */
  double gmax = -1.0;
  long goff = -1;
  int gi = -1, ga = -1;
  double gcfo = 0.0, gps[3] = {0};
  for (int k = 0; k < nb; k++) {
    for (int a = 0; a < sn->nb_ant; a++) {
      for (long off = best[k].off - 64; off <= best[k].off + 64; off++) {
        for (int i_ssb = 0; i_ssb < 8; i_ssb++) {
          for (double cfo = -25000.0; cfo <= 25000.0; cfo += 5000.0) {
            double ps[3];
            const double sc = pbch_cand_score(A, off, i_ssb, cfo, a, ps);
            if (sc > gmax) {
              gmax = sc; goff = off; gi = i_ssb; ga = a; gcfo = cfo;
              gps[0] = ps[0]; gps[1] = ps[1]; gps[2] = ps[2];
            }
          }
        }
      }
    }
  }

  LOG_W(PHY,
        "SENSING: DMRSSCAN RESULT best min3_coh=%.4f off=%ld i_ssb=%d ant=%d cfo_hz=%.0f "
        "sym_coh=[%.4f %.4f %.4f] live_ref_off=%d delta=%ld verdict=%s\n",
        gmax, goff, gi, ga, gcfo, gps[0], gps[1], gps[2], sn->ssb_time_offset,
        (goff >= 0) ? goff - sn->ssb_time_offset : 0,
        (gmax > 0.97) ? "SSB_FOUND" : "NO_DMRS_SIGNATURE_ANYWHERE");
  free(A);
  return NULL;
}

void nr_pbch_dmrs_scan_launch(const NR_DL_FRAME_PARMS *fp, const UE_nr_rxtx_proc_t *proc)
{
  static int launched = 0;
  if (launched || !g_pbch_live.valid)
    return;
  launched = 1;
  pbch_scan_arg_t *A = (pbch_scan_arg_t *)calloc(1, sizeof(*A));
  if (!A)
    return;
  A->snap = g_pbch_live; /* pointers shared; the snapshot is immutable after capture */
  A->fp = *fp;
  A->proc = *proc;
  pthread_t th;
  if (pthread_create(&th, NULL, pbch_scan_thread, A) == 0)
    pthread_detach(th);
  else
    free(A);
}


/* ---- PER-OCCASION DELTA MEASUREMENT (2026-08-06) -----------------------------------------------
 * The one-shot scan found the DMRS at +3620 in a single capture, and a verified +3620 FFT-window
 * bias (confirmed to move fep_rx_offset by exactly 3620) still did not decode. So the displacement
 * is not a fixed handover constant. This measures its DISTRIBUTION across occasions.
 *
 * Cheap by construction: a bounded +-4384 (one OFDM symbol) search around the position the live
 * path actually used, on a SMALL window copied per occasion (~26k samples/antenna, not the whole
 * 20 ms ring), scored by min-over-3-symbols DMRS coherence. Runs on the calling thread only after
 * the copy, and the copy is all that touches live buffers.
 */
typedef struct {
  int frame, slot;
  unsigned fep_rx_offset;
  long delta;
  double best_coh, coh_at_predicted;
  int i_ssb, ant;
} occ_delta_t;

void nr_pbch_measure_delta(const NR_DL_FRAME_PARMS *fp,
                           const UE_nr_rxtx_proc_t *proc,
                           c16_t *const *rxdata,
                           int nsamp_total,
                           unsigned fep_rx_offset_sym1)
{
  static int left = 24;
  if (left <= 0)
    return;
  static int en = -1;
  if (en < 0)
    en = (getenv("ISAC_DELTA_SCAN") && atoi(getenv("ISAC_DELTA_SCAN"))) ? 1 : 0;
  if (!en)
    return;
  /* One measurement per SSB occasion: this is called for each of the three PBCH symbols, but
   * `pred` is derived assuming the offset belongs to the FIRST of them, so later symbols would
   * yield a meaningless prediction. Dedupe on the frame rather than plumbing a counter in. */
  static int last_frame = -1;
  if (proc->frame_rx == last_frame)
    return;
  last_frame = proc->frame_rx;
  left--;

  const int N = fp->ofdm_symbol_size, cp = fp->nb_prefix_samples, stride = N + cp;
  const int HALF = stride; /* +-1 OFDM symbol */
  /* ssb symbol0 start implied by the live path, same convention the scanner uses */
  const long pred = (long)fep_rx_offset_sym1 - stride - cp;
  const long lo = pred - HALF, hi = pred + HALF;
  const long need = (hi + 4L * stride) - lo;
  if (lo < 0 || hi + 4L * stride > nsamp_total || need <= 0)
    return;

  const int nant = fp->nb_antennas_rx > 4 ? 4 : fp->nb_antennas_rx;
  pbch_snapshot_t w;
  memset(&w, 0, sizeof(w));
  for (int a = 0; a < nant; a++) {
    w.rxdata[a] = (c16_t *)malloc16(sizeof(c16_t) * need);
    if (!w.rxdata[a]) {
      for (int b = 0; b < a; b++)
        free16(w.rxdata[b], sizeof(c16_t) * need);
      return;
    }
    memcpy(w.rxdata[a], &rxdata[a][lo], sizeof(c16_t) * need);
  }
  w.valid = 1;
  w.nb_ant = nant;
  w.nsamp = (int)need;
  w.nid_cell = fp->Nid_cell;
  w.n_hf = fp->half_frame_bit;
  w.ssb_start_subcarrier = fp->ssb_start_subcarrier;
  w.ofdm_symbol_size = N;
  w.nb_prefix_samples = cp;

  pbch_scan_arg_t A;
  memset(&A, 0, sizeof(A));
  A.snap = w;
  A.fp = *fp;
  A.proc = *proc;

  double best = -1.0, at_pred = -1.0;
  long best_off = -1;
  int best_i = -1, best_a = -1;
  for (int a = 0; a < nant; a++) {
    for (long off = 0; off + 4L * stride <= need; off += 8) {
      double ps[3];
      const double sc = pbch_cand_score(&A, off, 0, 0.0, a, ps);
      if (sc > best) { best = sc; best_off = off; best_i = 0; best_a = a; }
      if (off == pred - lo && a == 0)
        at_pred = sc;
    }
  }
  LOG_W(PHY,
        "SENSING: DELTA frame=%d slot=%d fep_rx_offset=%u pred_ssb0=%ld best_coh=%.4f "
        "coh_at_predicted=%.4f delta=%+ld ant=%d hit=%s\n",
        proc->frame_rx, proc->nr_slot_rx, fep_rx_offset_sym1, pred, best,
        at_pred, (best_off >= 0) ? (lo + best_off) - pred : 0, best_a,
        (best > 0.97) ? "YES" : "no");

  for (int a = 0; a < nant; a++)
    free16(w.rxdata[a], sizeof(c16_t) * need);
}

void nr_pbch_replay_masks(const NR_DL_FRAME_PARMS *fp, const UE_nr_rxtx_proc_t *proc)
{
  static int done = 0;
  if (done)
    return;
  done = 1;

  // ---- MANDATORY CONTROLS ------------------------------------------------------------------
  // No mask result may be read unless the harness is proven working IN THIS RUN. A previous
  // version printed a full table from a run whose golden snapshot pointed at the wrong frame, so
  // every row was noise and the live numbers looked meaningful. Both controls must pass:
  //   (1) golden samples + golden params must decode;
  //   (2) the +-432 sweep must show its expected pass region (the tolerance already established).
  // If either fails the snapshot is bad and the experiment is aborted rather than reported.
  if (!g_pbch_golden.valid) {
    LOG_W(PHY, "SENSING: PBCHMASK ABORT -- no golden snapshot captured\n");
    return;
  }
  const int ctrl_crc = pbch_replay_mask(fp, proc, &g_pbch_golden, 0xF, "CONTROL_all4");
  int sweep_pass = 0;
  {
    const int cp = g_pbch_golden.nb_prefix_samples;
    for (int d = -432; d <= 432; d += 108) {
      pbch_snapshot_t shifted = g_pbch_golden;
      shifted.ssb_time_offset += d;
      if (pbch_replay_mask(fp, proc, &shifted, 0xF, "CONTROL_sweep") == 1)
        sweep_pass++;
    }
    (void)cp;
  }
  if (ctrl_crc != 1 || sweep_pass < 5) {
    LOG_W(PHY,
          "SENSING: PBCHMASK ABORT -- controls failed (golden_crc=%d sweep_pass=%d/9). "
          "Snapshot is not trustworthy; mask table suppressed.\n",
          ctrl_crc, sweep_pass);
    return;
  }
  LOG_W(PHY, "SENSING: PBCHMASK CONTROLS OK (golden_crc=1 sweep_pass=%d/9) -- table follows\n", sweep_pass);

  const struct {
    unsigned m;
    const char *n;
  } tests[] = {{0x1, "ch0"},      {0x2, "ch1"},      {0x4, "ch2"},      {0x8, "ch3"},
               {0x5, "ch2+ch0"},  {0x6, "ch2+ch1"},  {0xC, "ch2+ch3"},  {0xF, "all4"}};
  LOG_W(PHY, "SENSING: PBCHMASK --- GOLDEN (acquisition) snapshot ---\n");
  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
    pbch_replay_mask(fp, proc, &g_pbch_golden, tests[i].m, tests[i].n);
  if (g_pbch_live.valid) {
    LOG_W(PHY, "SENSING: PBCHMASK --- LIVE (tracking) snapshot ---\n");
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
      pbch_replay_mask(fp, proc, &g_pbch_live, tests[i].m, tests[i].n);
  }

  // Sweep validated on GOLDEN first: it must select the known acquisition tuple and only that one.
  // If it selects several, candidate-dependent state is not being fully regenerated and the live
  // result would be meaningless.
  pbch_sweep_issb(fp, proc, &g_pbch_golden, 0x4, "GOLDEN");
  if (g_pbch_live.valid)
    pbch_sweep_issb(fp, proc, &g_pbch_live, 0x4, "LIVE");
}

void nr_pbch_replay_2x2(const NR_DL_FRAME_PARMS *fp, const UE_nr_rxtx_proc_t *proc)
{
  static int done = 0;
  if (done || !g_pbch_golden.valid || !g_pbch_live.valid)
    return;
  done = 1;
  LOG_W(PHY, "SENSING: PBCH2X2 begin (golden ssb_off=%d, live ssb_off=%d)\n",
        g_pbch_golden.ssb_time_offset, g_pbch_live.ssb_time_offset);
  pbch_replay_one(fp, proc, &g_pbch_golden, &g_pbch_golden, "goldenSamp+goldenParm");
  pbch_replay_one(fp, proc, &g_pbch_golden, &g_pbch_live, "goldenSamp+liveParm");
  pbch_replay_one(fp, proc, &g_pbch_live, &g_pbch_golden, "liveSamp+goldenParm");
  pbch_replay_one(fp, proc, &g_pbch_live, &g_pbch_live, "liveSamp+liveParm");
  LOG_W(PHY, "SENSING: PBCH2X2 end\n");
}

void nr_pbch_replay_golden(const NR_DL_FRAME_PARMS *fp, const UE_nr_rxtx_proc_t *proc)
{
  if (!g_pbch_golden.valid)
    return;
  static int s_done = 0;
  if (s_done)
    return;
  s_done = 1;

  const int N = g_pbch_golden.ofdm_symbol_size;
  const int cp = g_pbch_golden.nb_prefix_samples;
  const int nb_ant = g_pbch_golden.nb_ant;
  const int stride = N + cp;
  const dft_size_idx_t dsz = get_dft(N);

  /* Sweep a little over one CP either side: a correct window may sit anywhere in the CP for a
   * flat channel, so the pass region's WIDTH is itself the result, not just its centre. */
  const int dmax = cp + (cp / 2);
  const int dstep = (cp / 16 > 0) ? cp / 16 : 1;

  LOG_W(PHY, "SENSING: PBCHREPLAY begin N=%d cp=%d stride=%d nb_ant=%d sweep=[%d..%d] step=%d\n",
        N, cp, stride, nb_ant, -dmax, dmax, dstep);

  for (int delta = -dmax; delta <= dmax; delta += dstep) {
    int16_t pbch_e_rx[NR_POLAR_PBCH_E];
    double pbch_log2_maxh = -1.0;
    double llr_absmean = 0.0;

    /* SSB symbols 1..3 carry PBCH (symbol 0 is PSS). Same loop shape as nr_initial_sync.c. */
    for (int s = 1; s <= 3; s++) {
      const long base = (long)g_pbch_golden.ssb_time_offset + (long)s * stride + cp + delta;
      if (base < 0 || base + N > g_pbch_golden.nsamp)
        goto next_delta;

      __attribute__((aligned(32))) c16_t rxdataF[nb_ant][N];
      __attribute__((aligned(32))) c16_t dl_ch_estimates[nb_ant][N];
      for (int a = 0; a < nb_ant; a++)
        dft(dsz, (int16_t *)&g_pbch_golden.rxdata[a][base], (int16_t *)rxdataF[a], 1);

      for (int a = 0; a < nb_ant; a++)
        nr_pbch_channel_estimation(fp,
                                   NULL,
                                   dl_ch_estimates[a],
                                   proc,
                                   s - 1,
                                   g_pbch_golden.i_ssb,
                                   g_pbch_golden.n_hf,
                                   g_pbch_golden.ssb_start_subcarrier,
                                   rxdataF[a],
                                   false,
                                   g_pbch_golden.nid_cell);

      nr_generate_pbch_llr(NULL,
                           proc,
                           fp,
                           s,
                           g_pbch_golden.i_ssb,
                           g_pbch_golden.nid_cell,
                           g_pbch_golden.ssb_start_subcarrier,
                           rxdataF,
                           dl_ch_estimates,
                           pbch_e_rx,
                           &pbch_log2_maxh);
    }

    for (int i = 0; i < NR_POLAR_PBCH_E; i++)
      llr_absmean += fabs((double)pbch_e_rx[i]);
    llr_absmean /= NR_POLAR_PBCH_E;

    {
      fapiPbch_t res;
      int hfb = 0, ssb_idx = 0, sym_off = 0;
      const int crc_ok = (0
                          == nr_pbch_decode(NULL,
                                            fp,
                                            proc,
                                            g_pbch_golden.i_ssb,
                                            g_pbch_golden.nid_cell,
                                            pbch_e_rx,
                                            &hfb,
                                            &ssb_idx,
                                            &sym_off,
                                            &res));
      LOG_W(PHY,
            "SENSING: PBCHREPLAY delta=%+d crc_ok=%d llr_absmean=%.2f log2_maxh=%.2f\n",
            delta, crc_ok, llr_absmean, pbch_log2_maxh);
    }
  next_delta:;
  }
  LOG_W(PHY, "SENSING: PBCHREPLAY end\n");
}

void nr_pbch_quantize(int16_t *pbch_llr8, const int16_t *pbch_llr, const uint16_t len)
{
  for (int i=0; i<len; i++) {
    if (pbch_llr[i]>31)
      pbch_llr8[i]=32;
    else if (pbch_llr[i]<-31)
      pbch_llr8[i]=-32;
    else
      pbch_llr8[i]=pbch_llr[i];
  }
}
/*
unsigned char sign(int8_t x) {
  return (unsigned char)x >> 7;
}
*/

const uint8_t pbch_deinterleaving_pattern[32] = {28, 0, 31, 30, 7,  29, 25, 27, 5,  8,  24, 9,  10, 11, 12, 13,
                                                 1,  4, 3,  14, 15, 16, 17, 2,  26, 18, 19, 20, 21, 22, 6,  23};

void nr_generate_pbch_llr(const PHY_VARS_NR_UE *ue,
                          const UE_nr_rxtx_proc_t *proc,
                          const NR_DL_FRAME_PARMS *frame_parms,
                          const int symbolSSB,
                          const int i_ssb,
                          const int nid,
                          const int ssb_start_subcarrier,
                          const c16_t rxdataF[frame_parms->nb_antennas_rx][frame_parms->ofdm_symbol_size],
                          const c16_t dl_ch_estimates[frame_parms->nb_antennas_rx][frame_parms->ofdm_symbol_size],
                          int16_t pbch_e_rx[NR_POLAR_PBCH_E],
                          double *log2_maxh_state)
{
  const int symbol_offset = nr_get_ssb_start_symbol(frame_parms, i_ssb) % (NR_SYMBOLS_PER_SLOT);
  const int nb_re = (symbolSSB == 2) ? 72 : 180;

  __attribute__((aligned(32))) struct complex16 rxdataF_ext[frame_parms->nb_antennas_rx][PBCH_MAX_RE_PER_SYMBOL];
  __attribute__((aligned(32))) struct complex16 dl_ch_estimates_ext[frame_parms->nb_antennas_rx][PBCH_MAX_RE_PER_SYMBOL];
  memset(dl_ch_estimates_ext, 0, sizeof dl_ch_estimates_ext);

  nr_pbch_extract(frame_parms,
                  rxdataF,
                  dl_ch_estimates,
                  rxdataF_ext,
                  dl_ch_estimates_ext,
                  symbolSSB,
                  symbol_offset,
                  ssb_start_subcarrier,
                  nid);

  // TEMPORARY DIAGNOSTIC (2026-08-05): nr_generate_pbch_llr() is the SHARED convergence point of
  // acquisition (ue==NULL) and tracking (ue!=NULL) -- see this function's other TEMPORARY diagnostic
  // block below and the caller comment at its call site. Logs a cheap FNV-1a checksum plus the key
  // scalar parameters for BOTH the raw extracted rxdataF (post-FFT, pre-channel-estimation) and the
  // extracted channel estimate (dl_ch_estimates_ext), from every call on either path, bounded so a
  // live run's log stays readable. Purpose: acquisition (273 PRB) is known to decode PBCH reliably
  // while tracking (same bandwidth, same FFT size, same nr_generate_pbch_llr call) fails -- this
  // pins down whether the two paths' INPUTS to this shared function already differ (checksums/means
  // disagree at matching symbolSSB/i_ssb/nid) or whether they agree here and the fault is further
  // downstream (log2_maxh derivation, LLR quantisation, or polar decode).
  //
  // EXTENDED 2026-08-05 with the metrics that actually discriminate a TIMING OFFSET from a
  // DECORRELATED estimate -- raw amplitude alone cannot, and the SSB's four symbols carry different
  // signals over different occupied-RE patterns (sym0 PSS/127 SC, sym1 PBCH/240 SC, sym2 SSS+PBCH
  // edges, sym3 PBCH/240 SC), so cross-SYMBOL amplitude comparison is meaningless anyway. Everything
  // below is measured over the SAME extracted RE set on BOTH paths (this function is the shared
  // convergence point), and keyed by symbolSSB so only like-for-like symbol TYPES are compared.
  //
  //  - slope_rad: mean phase increment per extracted RE, from the adjacent-RE product
  //      sum_i conj(H[i])*H[i+1]  ->  atan2(im, re)
  //    computed this way deliberately: no phase unwrapping, and noise averages down. A constant FFT
  //    window displacement dn shows up here as a CONSTANT non-zero slope (H_d[k] = H[k]e^{-j2pi k dn/N}).
  //  - slope_coh: |sum conj(H[i])H[i+1]| / sum |H[i]||H[i+1]|, in [0,1] -- how CONSISTENT that
  //    increment is across the band. This is the decisive discriminator:
  //      coh -> 1 with slope != 0 : clean linear phase ramp = pure timing offset (correctable)
  //      coh -> 0                 : estimate is decorrelated, NOT a timing offset (a window shift
  //                                 cannot explain it, and correcting the slope would not recover it)
  //  - resid: mean |H| after removing the fitted slope, relative to mean |H| -- residual spread once
  //    the linear-phase component is taken out.
  {
    static int s_pbchdiag_left = 60;
    static int s_pbchdiag_en = -1;
    if (s_pbchdiag_en < 0)
      s_pbchdiag_en = (getenv("ISAC_PHY_DIAG") && atoi(getenv("ISAC_PHY_DIAG"))) ? 1 : 0;
    if (s_pbchdiag_en && s_pbchdiag_left > 0) {
      uint64_t h_rxf = 1469598103934665603ULL, h_chest = 1469598103934665603ULL;
      double rxf_abs_sum = 0.0, chest_abs_sum = 0.0;
      const int16_t *rxf_p = (const int16_t *)&rxdataF_ext[0][0];
      const int16_t *chest_p = (const int16_t *)&dl_ch_estimates_ext[0][0];
      for (int i = 0; i < nb_re * 2; i++) {
        h_rxf = (h_rxf ^ (uint64_t)(uint16_t)rxf_p[i]) * 1099511628211ULL;
        h_chest = (h_chest ^ (uint64_t)(uint16_t)chest_p[i]) * 1099511628211ULL;
        rxf_abs_sum += (rxf_p[i] < 0) ? -rxf_p[i] : rxf_p[i];
        chest_abs_sum += (chest_p[i] < 0) ? -chest_p[i] : chest_p[i];
      }

      // Occupied-RE power (RMS), over exactly the extracted set -- comparable across paths for the
      // same symbolSSB, unlike a whole-FFT mean which is dominated by unoccupied subcarriers.
      double rxf_p2 = 0.0, chest_p2 = 0.0;
      for (int i = 0; i < nb_re; i++) {
        const double yr = rxdataF_ext[0][i].r, yi = rxdataF_ext[0][i].i;
        const double hr = dl_ch_estimates_ext[0][i].r, hi = dl_ch_estimates_ext[0][i].i;
        rxf_p2 += yr * yr + yi * yi;
        chest_p2 += hr * hr + hi * hi;
      }
      const double rxf_rms = sqrt(rxf_p2 / nb_re);
      const double chest_rms = sqrt(chest_p2 / nb_re);

      // Adjacent-RE phase increment + its consistency.
      double dr = 0.0, di = 0.0, dnorm = 0.0;
      for (int i = 0; i + 1 < nb_re; i++) {
        const double ar = dl_ch_estimates_ext[0][i].r, ai = dl_ch_estimates_ext[0][i].i;
        const double br = dl_ch_estimates_ext[0][i + 1].r, bi = dl_ch_estimates_ext[0][i + 1].i;
        dr += ar * br + ai * bi; // Re{conj(a)*b}
        di += ar * bi - ai * br; // Im{conj(a)*b}
        dnorm += sqrt(ar * ar + ai * ai) * sqrt(br * br + bi * bi);
      }
      const double slope_rad = atan2(di, dr);
      const double slope_coh = (dnorm > 0.0) ? sqrt(dr * dr + di * di) / dnorm : 0.0;
      // Implied FFT-window displacement in samples, if (and only if) slope_coh says the ramp is real.
      const double implied_dn = -slope_rad * (double)frame_parms->ofdm_symbol_size / (2.0 * M_PI);

      // Residual spread after de-rotating the fitted linear phase.
      double res_sum = 0.0, mag_sum = 0.0;
      double acc_r = 1.0, acc_i = 0.0; // running e^{-j*slope*i}
      const double cs = cos(-slope_rad), sn = sin(-slope_rad);
      double mr = 0.0, mi = 0.0;
      for (int i = 0; i < nb_re; i++) {
        const double hr = dl_ch_estimates_ext[0][i].r, hi = dl_ch_estimates_ext[0][i].i;
        const double dr2 = hr * acc_r - hi * acc_i;
        const double di2 = hr * acc_i + hi * acc_r;
        mr += dr2;
        mi += di2;
        mag_sum += sqrt(hr * hr + hi * hi);
        const double nacc_r = acc_r * cs - acc_i * sn;
        acc_i = acc_r * sn + acc_i * cs;
        acc_r = nacc_r;
      }
      mr /= nb_re;
      mi /= nb_re;
      // recompute deviation from that de-rotated mean
      acc_r = 1.0;
      acc_i = 0.0;
      for (int i = 0; i < nb_re; i++) {
        const double hr = dl_ch_estimates_ext[0][i].r, hi = dl_ch_estimates_ext[0][i].i;
        const double dr2 = hr * acc_r - hi * acc_i;
        const double di2 = hr * acc_i + hi * acc_r;
        res_sum += sqrt((dr2 - mr) * (dr2 - mr) + (di2 - mi) * (di2 - mi));
        const double nacc_r = acc_r * cs - acc_i * sn;
        acc_i = acc_r * sn + acc_i * cs;
        acc_r = nacc_r;
      }
      const double resid = (mag_sum > 0.0) ? res_sum / mag_sum : -1.0;

      LOG_W(PHY,
            "SENSING: PBCHDIAG path=%s frame=%d slot=%d symbolSSB=%d i_ssb=%d nid=%d ssb_sc=%d nb_re=%d "
            "rxf_hash=%016lx rxf_meanabs=%.2f rxf_rms=%.2f chest_hash=%016lx chest_meanabs=%.2f "
            "chest_rms=%.2f slope_rad=%+.5f slope_coh=%.4f implied_dn=%+.2f resid=%.4f log2maxh_st=%.2f\n",
            ue ? "tracking" : "acquisition", proc ? proc->frame_rx : -1, proc ? proc->nr_slot_rx : -1,
            symbolSSB, i_ssb, nid, ssb_start_subcarrier, nb_re,
            (unsigned long)h_rxf, rxf_abs_sum / (nb_re * 2), rxf_rms, (unsigned long)h_chest,
            chest_abs_sum / (nb_re * 2), chest_rms, slope_rad, slope_coh, implied_dn, resid,
            *log2_maxh_state);

      // ---- SPECTRUM / EXTRACTION-INDEX DIAGNOSTIC (2026-08-05) -------------------------------
      // Separates "a strong SSB peak exists but the extraction indices miss it" from "there is no
      // SSB peak anywhere near" and from "indices+peak align but extracted power is still low".
      // Uses the SAME start-bin formula nr_pbch_extract() itself uses, quoted from its own source
      // (rx_offset = (first_carrier_offset + ssb_start_subcarrier) % ofdm_symbol_size, then 240
      // subcarriers walked with mod-N wrap), so the reported window is what is actually read, not
      // an independent re-derivation that could drift from it.
      {
        const int N = frame_parms->ofdm_symbol_size;
        const int start_bin = (frame_parms->first_carrier_offset + ssb_start_subcarrier) % N;
        const c16_t *X = rxdataF[0];

        // Per-bin power, then a sliding 240-wide window over the WHOLE spectrum to find where the
        // SSB actually is -- if it sits somewhere else, that is the answer outright.
        double win_cfg = 0.0;
        for (int i = 0; i < 240; i++) {
          const c16_t v = X[(start_bin + i) % N];
          win_cfg += (double)v.r * v.r + (double)v.i * v.i;
        }
        double run = 0.0;
        for (int i = 0; i < 240; i++) {
          const c16_t v = X[i % N];
          run += (double)v.r * v.r + (double)v.i * v.i;
        }
        double best = run;
        int best_bin = 0;
        for (int s = 1; s < N; s++) {
          const c16_t out = X[(s - 1) % N];
          const c16_t in = X[(s + 239) % N];
          run -= (double)out.r * out.r + (double)out.i * out.i;
          run += (double)in.r * in.r + (double)in.i * in.i;
          if (run > best) {
            best = run;
            best_bin = s;
          }
        }
        // Neighbouring (nominally empty) guard regions, same width, for a reference floor.
        double win_lo = 0.0, win_hi = 0.0;
        for (int i = 0; i < 240; i++) {
          const c16_t a = X[((start_bin - 300 + i) % N + N) % N];
          const c16_t b = X[(start_bin + 300 + i) % N];
          win_lo += (double)a.r * a.r + (double)a.i * a.i;
          win_hi += (double)b.r * b.r + (double)b.i * b.i;
        }
        // Coarse profile: 14 blocks of 60 bins spanning [start-300, start+540).
        char prof[420];
        int pp = 0;
        for (int b = 0; b < 14 && pp < (int)sizeof(prof) - 12; b++) {
          double blk = 0.0;
          for (int i = 0; i < 60; i++) {
            const c16_t v = X[(((start_bin - 300 + b * 60 + i) % N) + N) % N];
            blk += (double)v.r * v.r + (double)v.i * v.i;
          }
          pp += snprintf(prof + pp, sizeof(prof) - pp, "%.0f ", sqrt(blk / 60.0));
        }
        LOG_W(PHY,
              "SENSING: SPECDIAG path=%s symbolSSB=%d N=%d fco=%d ssb_sc=%d start_bin=%d "
              "cfg_rms=%.2f best_bin=%d best_rms=%.2f delta_bins=%d lo_rms=%.2f hi_rms=%.2f "
              "prof[start-300,+60/blk]: %s\n",
              ue ? "tracking" : "acquisition", symbolSSB, N, frame_parms->first_carrier_offset,
              ssb_start_subcarrier, start_bin, sqrt(win_cfg / 240.0), best_bin, sqrt(best / 240.0),
              ((best_bin - start_bin + N / 2 + N) % N) - N / 2, sqrt(win_lo / 240.0),
              sqrt(win_hi / 240.0), prof);
      }
      s_pbchdiag_left--;
    }
  }
#ifdef DEBUG_PBCH
  LOG_I(PHY, "[PHY] PBCH Symbol %d ofdm size %d\n", symbolSSB, frame_parms->ofdm_symbol_size);
  LOG_I(PHY, "[PHY] PBCH starting channel_level\n");
#endif

  // Channel-compensation output shift: computed ONCE from symbol 1 and reused for symbols 2 and 3,
  // via caller-owned state in *log2_maxh_state.
  //
  // Before commit e4b2125f1e "Refactor PBCH & PSBCH UE procedures" this code was a loop over
  // symbols 1..3 with `log2_maxh` declared OUTSIDE the loop and computed under `if (symbol == 1)`,
  // so symbols 2 and 3 deliberately reused symbol 1's value. When the loop body became this
  // per-symbol function, log2_maxh became a local initialised to 0 while the `symbolSSB == 1` guard
  // was carried over verbatim -- so symbols 2 and 3 silently began compensating with a shift of 0,
  // i.e. no downscaling at all.
  //
  // MEASURED consequence (X410, 273 PRB, live cell): symbol 2 and symbol 3 LLRs came out 100%
  // saturated at nr_pbch_quantize()'s +-32 clamp (144/144 and 360/360), so two thirds of the polar
  // codeword carried only hard decisions and no soft information, while symbol 1 was properly scaled
  // (|LLR| mean ~9-15, ~15% saturated).
  //
  // The shift MUST be shared across the three symbols rather than recomputed per symbol, and that is
  // why the state is threaded through the caller instead of being a local. nr_pbch_decode() feeds
  // all three symbols' LLRs into ONE polar codeword, so their magnitudes have to stay on a common
  // scale: LLR ~ |H|^2 / 2^log2_maxh. With a single shared shift, a symbol whose channel is stronger
  // yields proportionally larger LLRs and is weighted more by the decoder, which is what soft
  // combining wants. Giving each symbol its own shift divides each by its own |H| instead, which
  // EQUALISES the symbols and throws away exactly that reliability weighting.
  double log2_maxh;
  if (symbolSSB == 1 || *log2_maxh_state < 0.0) {
    int avg[frame_parms->nb_antennas_rx];
    nr_channel_level(0, PBCH_MAX_RE_PER_SYMBOL, dl_ch_estimates_ext, frame_parms->nb_antennas_rx, 1, avg, nb_re);
    uint32_t max_h = avg[0];
    for (int i = 1; i < frame_parms->nb_antennas_rx; i++)
      max_h = cmax(avg[i], max_h);
    log2_maxh = 3 + (log2_approx(max_h) / 2);
    *log2_maxh_state = log2_maxh;
  } else {
    log2_maxh = *log2_maxh_state;
  }

#ifdef DEBUG_PBCH
  LOG_I(PHY, "[PHY] PBCH log2_maxh = %f\n", log2_maxh);
#endif
  __attribute__((aligned(32))) struct complex16 rxdataF_comp[frame_parms->nb_antennas_rx][PBCH_MAX_RE_PER_SYMBOL];
  nr_pbch_channel_compensation(rxdataF_ext, dl_ch_estimates_ext, nb_re, rxdataF_comp, frame_parms,
                               log2_maxh); // log2_maxh+I0_shift

  /*if (frame_parms->nb_antennas_rx > 1)
    pbch_detection_mrc(frame_parms,
                        rxdataF_comp,
                        symbol);*/

  /*
      if (mimo_mode == ALAMOUTI) {
        nr_pbch_alamouti(frame_parms,rxdataF_comp,symbol);
      } else if (mimo_mode != SISO) {
        LOG_I(PHY,"[PBCH][RX] Unsupported MIMO mode\n");
        return(-1);
      }
  */
  int pbch_e_rx_idx = 0;
  if (symbolSSB == 1) {
    pbch_e_rx_idx = 0;
  } else if (symbolSSB == 2) {
    pbch_e_rx_idx = 360;
  } else if (symbolSSB == 3) {
    pbch_e_rx_idx = 360 + 144;
  }

  if (ue) {
    metadata meta = {.slot = proc->nr_slot_rx, .frame = proc->frame_rx};
    UEscopeCopyWithMetadata(ue, pbchRxdataF_comp, rxdataF_comp[0], sizeof(c16_t), 1, nb_re, pbch_e_rx_idx / 2, &meta);
  }

  const int nb = (symbolSSB == 2) ? 144 : 360;

  // TEMPORARY DIAGNOSTIC (PBCH tracking-vs-acquisition root-cause hunt, 2026-08-02).
  // nr_generate_pbch_llr() is SHARED by both paths -- acquisition calls it with ue == NULL,
  // tracking with ue != NULL -- so the quantiser is common and any divergence must be in its
  // inputs (|H| via log2_maxh, and the compensated REs) rather than in the quantisation itself.
  // Logs log2_maxh, the pre-quantisation dynamic range, and the saturation fraction for each,
  // so the 273 PRB (broken) and 51 PRB (working) arms can be compared directly.
  // Record per-symbol state for the FAILURE-TRIGGERED dump (see nr_pbch_diag_report below).
  // Recording is unconditional and cheap; nothing is printed unless a decode actually fails, so a
  // failing occasion can be compared against the immediately preceding successful one.
  {
    const int16_t *raw = (const int16_t *)rxdataF_comp[0];
    long rawsum = 0;
    int rawmax = 0;
    int presat = 0;
    for (int i = 0; i < nb; i++) {
      const int a = abs(raw[i]);
      rawsum += a;
      if (a > rawmax)
        rawmax = a;
      if (a > 31)
        presat++;
    }
    // mean |H| over the extracted channel estimate, as a scale-independent health check
    long hsum = 0;
    int hmax = 0;
    for (int i = 0; i < nb_re; i++) {
      const int a = abs(dl_ch_estimates_ext[0][i].r) + abs(dl_ch_estimates_ext[0][i].i);
      hsum += a;
      if (a > hmax)
        hmax = a;
    }
    if (symbolSSB >= 1 && symbolSSB <= 3) {
      g_pbch_diag[symbolSSB] = (pbch_sym_diag_t){.valid = 1,
                                                 .is_track = (ue != NULL),
                                                 .log2_maxh = log2_maxh,
                                                 .rawmean = (double)rawsum / nb,
                                                 .rawmax = rawmax,
                                                 .presat = presat,
                                                 .nb = nb,
                                                 .hmean = (double)hsum / (nb_re ? nb_re : 1),
                                                 .hmax = hmax,
                                                 .nb_re = nb_re};
    }
  }

  nr_pbch_quantize(pbch_e_rx + pbch_e_rx_idx, (short *)rxdataF_comp[0], nb);
#ifdef DEBUG_PBCH
  char fname[50];
  sprintf(fname, "rxdataF_comp_%d.m", symbolSSB);
  write_output(fname, "rxFcomp", rxdataF[0], 240, 1, 1);

  for (int cnt = 0; cnt < 864  ; cnt++)
    printf("pbch rx llr %d\n", *(pbch_e_rx + cnt));

#endif
}

int nr_pbch_decode(PHY_VARS_NR_UE *ue,
                   const NR_DL_FRAME_PARMS *frame_parms,
                   const UE_nr_rxtx_proc_t *proc,
                   const int i_ssb,
                   const int Nid_cell,
                   int16_t pbch_e_rx[NR_POLAR_PBCH_E],
                   int *half_frame_bit,
                   int *ssb_index,
                   int *ret_symbol_offset,
                   fapiPbch_t *result)
{
  TracyCZone(ctx, true);
  if (ue) {
    UEscopeCopy(ue, pbchLlr, pbch_e_rx, sizeof(int16_t), frame_parms->nb_antennas_rx, NR_POLAR_PBCH_E, 0);
  }
  // un-scrambling
  const uint8_t Lmax = frame_parms->Lmax;
  const int unscrambling_mask = (Lmax == 64) ? 0x100006D : 0x1000041;
  unsigned int pbch_a_interleaved = 0;
  int pbch_a_prime = 0;
  int M = NR_POLAR_PBCH_E;
  int nushift = (Lmax == 4) ? i_ssb & 3 : i_ssb & 7;
  nr_pbch_unscrambling(pbch_e_rx, Nid_cell, nushift, M, NR_POLAR_PBCH_E, 0, 0, pbch_a_prime, &pbch_a_interleaved);
  //polar decoding de-rate matching
  uint64_t tmp = 0;
  const int decoderState = polar_decoder_int16(pbch_e_rx,
                                               (uint64_t *)&tmp,
                                               0,
                                               NR_POLAR_PBCH_MESSAGE_TYPE,
                                               NR_POLAR_PBCH_PAYLOAD_BITS,
                                               NR_POLAR_PBCH_AGGREGATION_LEVEL);
  pbch_a_prime = tmp;

  nr_downlink_indication_t dl_indication;
  fapi_nr_rx_indication_t rx_ind = {0};
  uint16_t number_pdus = 1;

  if (decoderState) {
    if (ue) { // decoding failed in synced state
      nr_fill_dl_indication(&dl_indication, NULL, &rx_ind, proc, ue, NULL);
      nr_fill_rx_indication(&rx_ind, FAPI_NR_RX_PDU_TYPE_SSB, ue, 0, 0, NULL, number_pdus, proc, NULL, NULL);
      if (ue->if_inst && ue->if_inst->dl_indication)
        ue->if_inst->dl_indication(&dl_indication);
    }
    return(decoderState);
  }
  //  printf("polar decoder output 0x%08x\n",pbch_a_prime);
  // Decoder reversal
  pbch_a_prime = (uint32_t)reverse_bits(pbch_a_prime, NR_POLAR_PBCH_PAYLOAD_BITS);

  //payload un-scrambling
  M = (Lmax == 64)? (NR_POLAR_PBCH_PAYLOAD_BITS - 6) : (NR_POLAR_PBCH_PAYLOAD_BITS - 3);
  nushift = ((pbch_a_prime>>24)&1) ^ (((pbch_a_prime>>6)&1)<<1);
  pbch_a_interleaved=0;
  nr_pbch_unscrambling(pbch_e_rx, Nid_cell, nushift, M, NR_POLAR_PBCH_PAYLOAD_BITS,
		       1, unscrambling_mask, pbch_a_prime, &pbch_a_interleaved);
  //printf("nushift %d sfn 3rd %d 2nd %d", nushift,((pbch_a_prime>>6)&1), ((pbch_a_prime>>24)&1) );
  //payload deinterleaving
  //uint32_t in=0;
  uint32_t out=0;

  for (int i=0; i<32; i++) {
    out |= ((pbch_a_interleaved>>i)&1)<<(pbch_deinterleaving_pattern[i]);
#ifdef DEBUG_PBCH
    printf("i %d in 0x%08x out 0x%08x ilv %d (in>>i)&1) 0x%08x\n", i, pbch_a_interleaved, out, pbch_deinterleaving_pattern[i], (pbch_a_interleaved>>i)&1);
#endif
  }

  result->xtra_byte = (out>>24)&0xff;

  const uint64_t payload = reverse_bits(out, NR_POLAR_PBCH_PAYLOAD_BITS);

  for (int i=0; i<3; i++)
    result->decoded_output[i] = (uint8_t)((payload>>((3-i)<<3))&0xff);

  *half_frame_bit = (result->xtra_byte >> 4) & 0x01; // computing the half frame index from the extra byte
  *ssb_index = i_ssb; // ssb index corresponds to i_ssb for Lmax = 4,8

  if (Lmax == 64) {   // for Lmax = 64 ssb index 4th,5th and 6th bits are in extra byte
    for (int i=0; i<3; i++)
      *ssb_index += (((result->xtra_byte >> (7 - i)) & 0x01) << (3 + i));
  }

  *ret_symbol_offset = nr_get_ssb_start_symbol(frame_parms, *ssb_index);

  if (*half_frame_bit)
    *ret_symbol_offset += (frame_parms->slots_per_frame >> 1) * frame_parms->symbols_per_slot;

#ifdef DEBUG_PBCH
  printf("xtra_byte %x payload %lx\n", result->xtra_byte, payload);

  for (int i=0; i<(NR_POLAR_PBCH_PAYLOAD_BITS>>3); i++) {
    //     printf("unscrambling pbch_a[%d] = %x \n", i,pbch_a[i]);
    printf("[PBCH] decoder payload[%d] = %x\n",i,result->decoded_output[i]);
  }

#endif

  if (ue) {
    nr_fill_dl_indication(&dl_indication, NULL, &rx_ind, proc, ue, NULL);
    nr_fill_rx_indication(&rx_ind, FAPI_NR_RX_PDU_TYPE_SSB, ue, 0, 0, NULL, number_pdus, proc, (void *)result, NULL);

    if (ue->if_inst && ue->if_inst->dl_indication)
      ue->if_inst->dl_indication(&dl_indication);
  }

  TracyCZoneEnd(ctx);
  return 0;
}

double nr_ue_pbch_freq_offset(const NR_DL_FRAME_PARMS *frame_parms,
                              const c16_t dl_ch_est_symb1[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB],
                              const c16_t dl_ch_est_symb3[NR_PBCH_NUM_RB * NR_NB_SC_PER_RB])
{
  const int nb_re = NR_PBCH_NUM_RB * NR_NB_SC_PER_RB;
  const c32_t dot_prod_res = dot_product(dl_ch_est_symb1, dl_ch_est_symb3, nb_re, 8);
  const double res_phase = atan2(dot_prod_res.i, dot_prod_res.r);
  const int samples_per_symbol = frame_parms->ofdm_symbol_size + frame_parms->nb_prefix_samples;
  const double t_ofdm = samples_per_symbol / (frame_parms->samples_per_subframe * 1000.0); // symbol duration in sec
  const double freq_offset = res_phase / (2 * M_PI * (3 - 1) * t_ofdm);

  return freq_offset;
}
