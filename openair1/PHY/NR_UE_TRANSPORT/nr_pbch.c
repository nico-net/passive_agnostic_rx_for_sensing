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
