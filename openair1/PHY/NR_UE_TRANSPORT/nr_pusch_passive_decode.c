#include "nr_passive_sample_lifetime.h"
#include "nr_passive_uci_probe.h"
#include "nr_passive_uci_learn.h"
/* See nr_pusch_passive_decode.h for why this file constructs a gNB by hand. */

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "PHY/MODULATION/nr_modulation.h" // nr_symbol_fep_ul: the gNB uplink FEP
#include "PHY/nr_phy_common/inc/nr_phy_common.h" // nr_fo_compensation: the same de-rotation nr_slot_fep uses

#include "nr_pusch_passive_decode.h"
#include "nr_pusch_data_aided.h"

#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "PHY/defs_gNB.h"
#include "PHY/defs_RU.h"          // RU_RX_SLOT_DEPTH -- the gNB rxdataF ring depth
#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h" // blind UL DM-RS identity estimate (nr_dmrs_id_init/_accumulate/_decide/_set_range)
#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.h" // nr_pusch_passive_queue_running()
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_TRANSPORT/nr_ulsch.h"
#include "executables/softmodem-common.h"
#include "executables/nr-uesoftmodem.h" // get_nrUE_params -- the UE thread pool this reuses
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "PHY/NR_UE_ISAC/nr_isac.h" // UL CFR submission
#include "nr_pdcch_blind_monitor_rt.h" // nr_pdcch_blind_monitor_get_cfg: the UCI search parameters

/* nr_ulsch_decoding() has no declaration in any header this library exposes -- nr_transport_proto.h
 * declares nr_rx_pusch_group_tp() but not its decoder. Declared here against the definition read
 * from nr_ulsch_decoding.c rather than adding a prototype to a shared header, so that this passive
 * path cannot quietly change a signature the gNB build also depends on. */
/* The producer's un-wrapped slot counter. This is the SAME clock nr_pdsch_passive_queue.c stamps
 * onto pdsch_data rows via nr_isac_abs_slot_override, and the slow-time grid only makes sense if
 * every source shares one origin. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

extern int nr_ulsch_decoding(PHY_VARS_gNB *phy_vars_gNB,
                             NR_DL_FRAME_PARMS *frame_parms,
                             uint32_t frame,
                             uint8_t nr_tti_rx,
                             int *ULSCH_ids,
                             int nb_pusch);

/* ---- Two symbols the gNB PUSCH receive chain needs that live in files this library deliberately
 * does NOT compile. Providing them here costs four lines; pulling in their home files would drag
 * phy_init_nr_gNB() (with PRACH, SRS, PUCCH and the whole DLSCH side) and the entire gNB PHY
 * procedures TU into a passive receiver that calls none of it.
 *
 * Both are reproduced with their real semantics, not stubbed:
 *  - get_first_ant_idx() is the one-line expression from phy_procedures_nr_gNB.c:75 verbatim. With
 *    enable_analog_das = 0 (this receiver has no analog DAS) it returns fapi_start_port, which is
 *    the branch that would run anyway.
 *  - get_phy_stats() returns NULL, and that is CORRECT rather than a shortcut: its only caller here
 *    is nr_ulsch_decoding.c:142, which guards every use with . The structure it would
 *    otherwise hand back is the gNB's per-RNTI MAC-facing statistics array -- state a passive
 *    receiver has no business keeping, and which nothing in this path reads. This file keeps its
 *    own census instead (nr_pusch_passive_stats_dump).
 *
 * CONSTRAINT: these are global symbols, so this library must never be linked alongside PHY_NR.
 * It is not -- nr-uesoftmodem links PHY_NR_COMMON, PHY_NR_UE and this, and that is the whole
 * reason this library exists. */
uint16_t get_first_ant_idx(bool das, uint16_t num_ports_beams, uint16_t beam_id, uint16_t fapi_start_port)
{
  return ((das) ? (beam_id & 0x7fff) * num_ports_beams : fapi_start_port);
}

NR_gNB_PHY_STATS_t *get_phy_stats(PHY_VARS_gNB *gNB, uint16_t rnti)
{
  (void)gNB;
  (void)rnti;
  return NULL;
}

#define PASSIVE_UL_MAX_ANT 4
/* HARQ namespace. The DL path already uses 1000+ (attached UE), 2000+ (passive PDSCH decode) and
 * 3000+ (data-aided re-encode) on the SAME dlopen'd LDPC interface. A hardware accelerator keys its
 * internal state on this id, so an overlap would alias two unrelated transport blocks. */
#define PASSIVE_UL_HARQ_TAG_BASE 4000

/* One context per potential concurrent decoder. Every buffer the receive chain writes -- the
 * rxdataF ring, pusch_vars, the ULSCH HARQ, the tpool -- hangs off PHY_VARS_gNB, so sharing one
 * across threads would interleave two grants' intermediate state silently. Indexed rather than
 * thread-local: the queue already numbers its consumers, and __thread storage of this size is what
 * produced an AVX alignment fault in the AoA work. */
static PHY_VARS_gNB *g_gnb[NR_PUSCH_PASSIVE_MAX_CTX];
static int           g_gnb_nant;
static _Atomic uint64_t g_try, g_crc_ok, g_rej_unsup, g_rej_setup;
/* UL DM-RS identity estimate (CP-OFDM PUSCH, type 1, port 0): same sequence family as PDSCH
 * (TS 38.211 6.4.1.1.1.1 vs 7.4.1.1.1), reference point CRB 0, so the PDSCH estimator applies
 * unchanged. Accumulated on EVERY attempted grant, CRC-OK or not (review fix round 1, finding 2) --
 * see the accumulate call site's own comment, below nr_ulsch_decoding()'s CFR block, for why gating
 * this on CRC would be circular.
 *
 * The per-nSCID two-window DM-RS states/locks and the cell-wide data-ID sweep + CRC-stall
 * counter (Task 13) now live in nr_pusch_passive_ul_ids.{h,c} -- moved out of this file so
 * nr_pdcch_blind_monitor.c's read side does not pull PHY_NR_PASSIVE_UL into the offline
 * test_nr_pdcch_blind_monitor gtest binary; see that file's header comment and fix2-report.md. This
 * file still owns and calls the actual estimation (nr_dmrs_id_accumulate()/_decide(), which need live
 * IQ) through the accessors it exposes. */
static _Atomic uint64_t g_seg_fail, g_zero_tb;
/* Residual the channel estimator can absorb on its own: MAX_DELAY_COMP is 20 samples, so anything
 * beyond a comfortable fraction of that is worth re-placing the window for rather than hoping. */
#define PASSIVE_UL_DELAY_TOL 6
static _Atomic uint64_t g_ta_refined;
static _Atomic uint64_t g_uci_trials, g_uci_rescued;

/* The per-segment CRC verdict, the same expression nr_ulsch_decoding.c:289 calls `crcok`. Kept as a
 * helper because the UCI search needs it mid-function, not only at the end. */
static inline bool hp_crc_failed(const NR_gNB_ULSCH_t *u)
{
  const NR_UL_gNB_HARQ_t *h = u->harq_process;
  if (!h || !h->C || h->processedSegments!=h->C) return true;
  const uint32_t bits=h->ulsch_pdu.pusch_data.tb_size*8u;
  return h->C>1 && !check_crc(h->b,lenWithCrc(1,bits),crcType(1,bits));
}

/* ---- TIMING-ADVANCE SWEEP (ISAC_UL_TA_SWEEP="start:step:count", default off) ------------------
 * The applied advance is N_TA_offset, derived from the sample rate. What it CANNOT know is N_TA --
 * the per-UE advance the gNB commands, which appears in no DCI -- nor any fixed offset in this
 * receiver's own chain. After correcting the window's sign the estimator still measures a residual
 * of ~180-260 samples, an order beyond the +/-20 it can compensate, so the remaining offset has to
 * be found rather than derived.
 *
 * Swept INSIDE one capture, one value per grant round-robin, rather than one value per run. A run
 * on this rig costs ~4 minutes and mis-locks its CFO about half the time, so a 17-point sweep as
 * separate runs is two hours of wall time and a dozen VOID results; as a round-robin it is ~1100
 * grants per value in a single 200 s capture, and every value sees the SAME channel, traffic and
 * lock, which a sequence of runs could never guarantee. */
#define TA_SWEEP_MAX 33
static int      g_ta_sweep_n     = -1;
static int32_t  g_ta_sweep_start = 0;
static int32_t  g_ta_sweep_step  = 0;
static _Atomic uint32_t g_ta_sweep_seq;
static _Atomic uint64_t g_ta_try[TA_SWEEP_MAX];
static _Atomic uint64_t g_ta_ok[TA_SWEEP_MAX];

static int ta_sweep_points(void)
{
  if (g_ta_sweep_n < 0) {
    g_ta_sweep_n = 0;
    const char *e = getenv("ISAC_UL_TA_SWEEP");
    if (e != NULL) {
      int a = 0, b = 0, c = 0;
      if (sscanf(e, "%d:%d:%d", &a, &b, &c) == 3 && c > 0) {
        g_ta_sweep_start = a;
        g_ta_sweep_step  = b;
        g_ta_sweep_n     = (c > TA_SWEEP_MAX) ? TA_SWEEP_MAX : c;
      }
    }
  }
  return g_ta_sweep_n;
}

/* ---- UL RT-THREAD COST BREAKDOWN (ISAC_PUSCH_TIMING=1, default OFF) ---------------------------
 * The downlink has BTIM (nr_pdcch_blind_monitor_rt.c) and it is what turned "move the PDSCH decode
 * off the receive thread" from an assumption into a measurement. UTIM is the uplink equivalent,
 * same shape deliberately, so the two directions' numbers are read the same way.
 *
 * WHERE THIS RUNS. Corrected 2026-09-03: an earlier version of this comment said the uplink runs
 * "ENTIRELY IN-LINE ... there is no deferred queue", and that is false for every conf we actually
 * capture with. nr_pusch_passive_queue.{c,h} is the UL escape valve; nr_pusch_passive_monitor_rt.c
 * enqueues to it when pdcch_blind_monitor_ul_thread is non-zero and calls this decoder in-line only
 * as the fallback when it is not. The default IS 0 (in-line), which is what made the stale claim
 * plausible -- but tests/passive_rx sets "2:32:2" and the logs show "passive PUSCH decode consumer
 * 0/1 started". So the 1065 us/grant measured below is the cost of the WORK, not of blocking the
 * receive thread, unless you have deliberately turned the queue off.
 *
 * R7 applies: characterise with this on, score with it off. */
#define UTIM_FEP     0
#define UTIM_RXPUSCH 1
#define UTIM_CFR     2
#define UTIM_DECODE  3
#define UTIM_TOTAL   4
#define UTIM_N       5
static const char *const kUtimName[UTIM_N] = {"fep", "rx_pusch", "cfr", "decode", "TOTAL"};
static uint64_t g_utim_ns[UTIM_N]  = {0};
static uint64_t g_utim_n[UTIM_N]   = {0};
static uint64_t g_utim_max[UTIM_N] = {0};
/* Per-grant TOTAL, bucketed in microseconds: <50 <100 <200 <400 <800 <1600 <3200 >=3200. The mean
 * is not what starves a receive thread; the tail is. */
static uint64_t g_utim_hist[8]   = {0};
static uint64_t g_utim_over_slot = 0;
static int      g_utim_on        = -1;

/* One gate for every per-grant probe in this file, so they cannot drift apart. */
static inline int s_diag_on(void)
{
  static int v = -1;
  if (v < 0) {
    v = (getenv("ISAC_PUSCH_DIAG") != NULL) ? 1 : 0;
  }
  return v;
}

static inline int utim_enabled(void)
{
  if (g_utim_on < 0) {
    g_utim_on = (getenv("ISAC_PUSCH_TIMING") != NULL) ? 1 : 0;
  }
  return g_utim_on;
}

static inline uint64_t utim_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void utim_add(int k, uint64_t t0)
{
  const uint64_t d = utim_now() - t0;
  g_utim_ns[k] += d;
  g_utim_n[k]++;
  if (d > g_utim_max[k]) {
    g_utim_max[k] = d;
  }
}

static void utim_total(uint64_t d_ns, uint64_t slot_ns)
{
  g_utim_ns[UTIM_TOTAL] += d_ns;
  g_utim_n[UTIM_TOTAL]++;
  if (d_ns > g_utim_max[UTIM_TOTAL]) {
    g_utim_max[UTIM_TOTAL] = d_ns;
  }
  const uint64_t us = d_ns / 1000;
  int b = 0;
  if (us >= 3200) b = 7;
  else if (us >= 1600) b = 6;
  else if (us >= 800) b = 5;
  else if (us >= 400) b = 4;
  else if (us >= 200) b = 3;
  else if (us >= 100) b = 2;
  else if (us >= 50) b = 1;
  g_utim_hist[b]++;
  if (slot_ns > 0 && d_ns > slot_ns) {
    g_utim_over_slot++;
  }
}
static _Atomic uint64_t g_cfr_submits, g_cfr_re;
/* Per-antenna UL channel power, the uplink analogue of the DL RXBRANCH probe. Accumulated from the
 * PUSCH DM-RS channel ESTIMATE (|H|^2), not from raw time-domain power: raw power cannot separate
 * signal from interference, and the DL side has already been misled once by reading ANTPOW as an
 * antenna measure when ~70 % of its energy was uplink. Summed over whole transport blocks and
 * thousands of grants, so it does not carry the 10 dB per-TB swing the DL pw[] does. */
static _Atomic uint64_t g_ant_pw[PASSIVE_UL_MAX_ANT];
static _Atomic uint64_t g_ant_n;

/* ==============================================================================================
 * UCI ON PUSCH: reserve the REs the HARQ-ACK takes, so ULSCH rate matching is right.
 *
 * WHY THIS IS NEEDED. When the UE has HARQ-ACK to report and is scheduled a PUSCH in the same slot,
 * it multiplexes the ACK onto the PUSCH, and for O_ACK > 2 those coded bits are RATE-MATCHED AROUND
 * -- the ULSCH gets a smaller G. OAI's gNB receiver has no UCI-on-PUSCH support at all, so it
 * computes the full G and every rate-recovery offset is wrong on such a grant. Measured on this
 * cell: UCI rides 98.3 % of grants under bidirectional traffic and 32.7 % under uplink-only, which
 * is the difference between a passive receiver that works on a loaded cell and one that does not.
 *
 * WHY IT IS A SEARCH AND NOT A DERIVATION. O_ACK is not in the uplink DCI. With a dynamic codebook
 * the UL DAI carries (V_T_DAI - 1) mod 4, so the DCI pins O_ACK only MODULO 4 -- the receiver knows
 * the count's residue, not the count. A UE-attached receiver resolves it from its own downlink
 * assignment history; a passive one would have to reconstruct that UE's entire HARQ codebook state,
 * which is a much larger piece of work and fails silently whenever a DL assignment was missed.
 *
 * So the candidates consistent with the observed DAI are tried in order and the 24-bit transport
 * block CRC is the oracle. That is sound because the CRC is what decides acceptance anyway: a wrong
 * hypothesis produces a failed CRC, and a false accept across a handful of trials is ~n/2^24. The
 * cost is bounded and paid only on grants that failed without reservation, which is the honest
 * trade -- those grants currently yield nothing at all.
 *
 * The arithmetic mirrors nr_ulsch_ue.c's calc_rate_match_info_uci() rather than re-reading the
 * spec, so the transmitter this receiver is trying to invert and the receiver stay in step by
 * construction. TS 38.212 6.3.2.4.1.1.
 * ============================================================================================== */

/* TS 38.213 Table 9.3-1, betaOffsets for HARQ-ACK. Index straight from RRC. */
static double passive_beta_harq(uint8_t idx)
{
  /* Transcribed from nr_ulsch_ue.c's get_beta_offset_harq_ack() rather than from the spec table, so
   * the transmitter this inverts and this receiver cannot drift apart. Note indices 16-20 are the
   * sub-unity entries, not a continuation of the ramp -- writing them from memory got them wrong. */
  static const double v[21] = {1.000,  2.000,  2.500,  3.125,  4.000,  5.000,  6.250,
                               8.000,  10.000, 12.625, 15.875, 20.000, 31.000, 50.000,
                               80.000, 126.000, 0.600, 0.400, 0.200, 0.100, 0.050};
  return v[(idx < 21) ? idx : 11];
}

static double passive_alpha(uint8_t idx)
{
  switch (idx) {
    case 0: return 0.5;
    case 1: return 0.65;
    case 2: return 0.8;
    default: return 1.0;
  }
}

/* TS 38.212 6.3.1.2.1 */
static int passive_uci_crc(uint32_t o)
{
  return (o > 19) ? 11 : ((o > 11) ? 6 : 0);
}

/* Q'_ACK, TS 38.212 6.3.2.4.1.1. Identical shape to nr_ulsch_ue.c's get_Qd(). */
static uint32_t passive_qd_ack(uint32_t o_ack, double beta, double alpha,
                               uint32_t sum_kr, uint32_t s1, uint32_t s2)
{
  if (o_ack == 0 || sum_kr == 0) {
    return 0;
  }
  const uint32_t a = (uint32_t)ceil(((double)o_ack + passive_uci_crc(o_ack)) * beta * (double)s1 / (double)sum_kr);
  const uint32_t b = (uint32_t)ceil(alpha * (double)s2);
  return (a < b) ? a : b;
}

/* REs per layer the HARQ-ACK removes from the ULSCH, i.e. exactly nr_get_G()'s unav_res.
 * nr_get_G does `G -= unav_res * Qm * Nl` and the transmitter removes E_uci_ACK = Q'_ACK * Nl * Qm,
 * so the two are the same quantity -- checked against the units rather than assumed. */
static uint32_t passive_ul_unav_res(const nr_pdcch_blind_ul_result_t *g, uint32_t tbs_bits,
                                    uint8_t bg, uint32_t o_ack, uint8_t beta_idx, uint8_t alpha_idx)
{
  /* Below 3 bits the ACK PUNCTURES the ULSCH instead of being rate-matched around it
   * (TS 38.212 6.2.7, and nr_ulsch_ue.c only subtracts E_uci_ACK when O_ack > 2), so G is
   * unchanged and there is nothing to reserve. */
  if (o_ack <= 2) {
    return 0;
  }
  uint32_t C = 0, K = 0, Z = 0, F = 0;
  nr_segmentation(NULL, NULL, lenWithCrc(1, tbs_bits), &C, &K, &Z, &F, bg);
  const uint32_t sum_kr = K * C;

  const uint16_t mask     = g->ul_dmrs_symb_pos;
  const int      n_dmrs   = __builtin_popcount((unsigned)mask
                                               & (((1u << g->num_symbols) - 1u) << g->start_symbol));
  const int      nsc      = g->num_rb * NR_NB_SC_PER_RB;
  const uint32_t s1       = (uint32_t)(nsc * (g->num_symbols - n_dmrs));

  /* s2 counts only the non-DM-RS REs AFTER the first DM-RS symbol: the ACK is mapped starting from
   * there so the receiver has a channel estimate for it. */
  const int first_dmrs    = mask ? __builtin_ctz(mask) : g->start_symbol;
  const uint32_t range    = ((1u << g->num_symbols) - 1u) << g->start_symbol;
  const uint32_t post     = range & ~((1u << (first_dmrs + 1)) - 1u);
  const int n_post_nodmrs = __builtin_popcount(post & ~(uint32_t)mask);
  const uint32_t s2       = (uint32_t)(nsc * n_post_nodmrs);

  return passive_qd_ack(o_ack, passive_beta_harq(beta_idx), passive_alpha(alpha_idx), sum_kr, s1, s2);
}

/* ------------------------------------------------------------------------------------------
 * Minimal gNB context. Deliberately NOT phy_init_nr_gNB(): that allocates PRACH, SRS, PUCCH, the
 * DLSCH side and a set of queues, asserts on config this receiver has no business filling in, and
 * would drag most of PHY_NR into the link for buffers nothing here touches. Only the fields the
 * PUSCH receive chain actually reads are built, each one traceable to where it is read.
 * ------------------------------------------------------------------------------------------ */
static bool passive_gnb_prepare(PHY_VARS_NR_UE *ue, int ctx)
{
  if (ctx < 0 || ctx >= NR_PUSCH_PASSIVE_MAX_CTX) {
    return false;
  }
  if (g_gnb[ctx] != NULL) {
    return true;
  }
  const NR_DL_FRAME_PARMS *ufp = &ue->frame_parms;
  if (ufp->N_RB_UL < 1 || ufp->N_RB_UL > 275) return false;
  const int nant = (ufp->nb_antennas_rx < PASSIVE_UL_MAX_ANT) ? ufp->nb_antennas_rx : PASSIVE_UL_MAX_ANT;

  PHY_VARS_gNB *gnb = (PHY_VARS_gNB *)calloc(1, sizeof(PHY_VARS_gNB));
  if (gnb == NULL) {
    return false;
  }
  /* The two structs are the same type, so the uplink numerology, CP, FFT size and carrier offset
   * are inherited exactly from what the receiver is already synced to -- which is the point: a
   * passive receiver must demodulate the uplink on the SAME grid it demodulates the downlink on,
   * not on one derived independently. */
  gnb->frame_parms = *ufp;
  gnb->frame_parms.N_RB_UL = ufp->N_RB_UL;

  gnb->gNB_config.carrier_config.num_rx_ant.value = nant;
  gnb->gNB_config.cell_config.phy_cell_id.value   = ufp->Nid_cell;
  gnb->max_nb_pusch                = 1;
  gnb->max_ldpc_iterations         = 8;
  gnb->num_pusch_symbols_per_thread = 1;
  gnb->dmrs_num_antennas_per_thread = 1;
  gnb->chest_time                  = 0;
  gnb->chest_freq                  = 0;
  gnb->enable_analog_das           = 0;
  gnb->common_vars.num_beams_period = 1;
  gnb->pusch_thres                 = 0;
  /* The LDPC interface IS shared: it is a dlopen'd shared library and a second handle would not
   * share its internal state. The THREAD POOL is not -- tpool_t holds a pthread_barrier_t, so
   * copying the UE's by value would duplicate barrier state rather than share the pool. Give this
   * receiver its own small pool instead, which also keeps the decode off the UE's RT pool: this
   * tree has already measured blind PDSCH decoding on the receive thread costing PBCH lock. */
  gnb->nrLDPC_coding_interface     = ue->nrLDPC_coding_interface;
  /* ZERO worker threads, so pushTpool() runs every task INLINE (its own documented fallback).
   *
   * A 2-thread pool deadlocked the PHY receive thread on the first live grant: threadCreate gives
   * pool workers priority 97, the same as the receive thread, and this box already pins six Tpool
   * threads across the only cores the receiver has (--thread-pool 0,1,4,5,6,7). New RT-priority
   * workers on saturated cores may never be scheduled, and nr_rx_pusch_group_tp() then blocks
   * forever in join_task_ans() waiting for tasks that cannot run. Measured signature: the receiver
   * logged "passive PUSCH receiver ready" and then emitted nothing further for the remaining ~295 s
   * -- no RFCENSUS, no summary, no PUSCHDIAG -- while the process stayed alive.
   *
   * Inline costs the decode's full time on the receive thread. That is affordable ONLY because
   * uplink slots are otherwise completely idle here (2 slots in 10 doing no work at all), and it is
   * a stepping stone: the DL path already learned this lesson and moved its decode to a priority-50
   * consumer. Watch max_pos_acc and pbch_ok, and move this to a queue before any long capture. */
  initFloatingCoresTpool(0, &gnb->threadPool, false, "passiveUL-tpool");

  const int symsz = ufp->ofdm_symbol_size;
  const int sps   = ufp->symbols_per_slot;

  /* rxdataF is a RING of RU_RX_SLOT_DEPTH slots: the UL chest indexes it as
   * (Ns % RU_RX_SLOT_DEPTH) * symbols_per_slot * ofdm_symbol_size + symbol * ofdm_symbol_size. */
  gnb->common_vars.rxdataF = (c16_t **)malloc16_clear(nant * sizeof(c16_t *));
  if (gnb->common_vars.rxdataF == NULL) {
    free(gnb);
    return false;
  }
  for (int a = 0; a < nant; a++) {
    gnb->common_vars.rxdataF[a] = (c16_t *)malloc16_clear((size_t)RU_RX_SLOT_DEPTH * sps * symsz * sizeof(c16_t));
    if (gnb->common_vars.rxdataF[a] == NULL) {
      free(gnb);
      return false;
    }
  }

  const int max_layers = NR_MAX_NB_LAYERS;
  const int n_buf      = nant * max_layers;
  const int nb_re      = gnb->frame_parms.N_RB_UL * NR_NB_SC_PER_RB;
  const int nb_re2     = ((nb_re + 15) / 16) * 16;

  gnb->pusch_vars = (NR_gNB_PUSCH *)malloc16_clear(sizeof(NR_gNB_PUSCH));
  NR_gNB_PUSCH *pv = &gnb->pusch_vars[0];
  pv->ul_ch_estimates     = (int32_t **)malloc16_clear(n_buf * sizeof(int32_t *));
  pv->ptrs_phase_per_slot = (int32_t **)malloc16_clear(n_buf * sizeof(int32_t *));
  for (int i = 0; i < n_buf; i++) {
    pv->ul_ch_estimates[i]     = (int32_t *)malloc16_clear((size_t)symsz * sps * sizeof(int32_t));
    pv->ptrs_phase_per_slot[i] = (int32_t *)malloc16_clear(sps * sizeof(int32_t));
  }
  pv->rxdataF_comp = (c16_t **)malloc16_clear(max_layers * sizeof(c16_t *));
  for (int i = 0; i < max_layers; i++) {
    pv->rxdataF_comp[i] = (c16_t *)malloc16_clear((size_t)nb_re2 * sps * sizeof(c16_t));
  }
  /* Same size expression nr_init.c uses. It is not derived from anything here; copied deliberately
   * so the two cannot diverge. */
  pv->llr = (int16_t *)malloc16_clear(8 * ((3 * 8 * 6144) + 12) * sizeof(int16_t));
  pv->ul_valid_re_per_slot = (int16_t *)malloc16_clear(sps * sizeof(int16_t));

  gnb->ulsch = (NR_gNB_ULSCH_t *)malloc16_clear(sizeof(NR_gNB_ULSCH_t));
  gnb->ulsch[0] = new_gNB_ulsch(gnb->max_ldpc_iterations, gnb->frame_parms.N_RB_UL);

  g_gnb[ctx]  = gnb;
  g_gnb_nant  = nant;
  LOG_I(PHY,
        "SENSING: passive PUSCH receiver ready ctx=%d (N_RB_UL=%d ant=%d fft=%d sps=%d pci=%d)\n",
        ctx, gnb->frame_parms.N_RB_UL, nant, symsz, sps, ufp->Nid_cell);
  return true;
}

void nr_pusch_passive_decode_free(void)
{
  /* Deliberately a shallow teardown at process exit only: the buffers above are freed by the OS,
   * and free_gNB_ulsch()'s partner allocations are inside a struct this file did not fully build. */
  for (int c = 0; c < NR_PUSCH_PASSIVE_MAX_CTX; c++) {
    g_gnb[c] = NULL;
  }
}

/* Fill the FAPI PUSCH PDU from a recovered UL grant. Everything here either came from the DCI or
 * from the deployment config carried alongside it -- nothing is invented. */
static void fill_pusch_pdu(const nr_pdcch_blind_ul_result_t *g, int nant,
                           const NR_DL_FRAME_PARMS *fp, nfapi_nr_pusch_pdu_t *p)
{
  memset(p, 0, sizeof(*p));
  p->pdu_bit_map        = PUSCH_PDU_BITMAP_PUSCH_DATA;
  p->rnti               = g->rnti;
  p->bwp_start          = g->bwp_start;
  p->bwp_size           = g->bwp_size;
  p->subcarrier_spacing = fp->numerology_index;
  p->cyclic_prefix      = fp->Ncp;

  p->mcs_index          = g->mcs;
  p->mcs_table          = g->mcs_table;
  p->qam_mod_order      = nr_get_Qm_ul(g->mcs, g->mcs_table);
  p->target_code_rate   = nr_get_code_rate_ul(g->mcs, g->mcs_table);
  p->transform_precoding = g->transform_precoding ? 0 : 1; // enum: 0 = enabled, 1 = disabled
  p->data_scrambling_id = g->data_scrambling_id;
  p->nrOfLayers         = g->nrOfLayers;

  p->ul_dmrs_symb_pos   = g->ul_dmrs_symb_pos;
  p->dmrs_config_type   = g->dmrs_config_type;
  p->ul_dmrs_scrambling_id = g->ul_dmrs_scrambling_id;
  p->pusch_identity     = g->ul_dmrs_scrambling_id;
  p->scid               = g->nscid;
  p->num_dmrs_cdm_grps_no_data = g->n_dmrs_cdm_groups;
  p->dmrs_ports         = g->dmrs_ports;

  p->resource_alloc     = 1;   // type 1 -- the only type this deployment schedules
  p->rb_start           = g->start_rb;
  p->rb_size            = g->num_rb;
  p->vrb_to_prb_mapping = 0;
  p->frequency_hopping  = g->frequency_hopping;

  p->start_symbol_index = g->start_symbol;
  p->nr_of_symbols      = g->num_symbols;

  p->pusch_data.rv_index          = g->rv;
  p->pusch_data.harq_process_id   = g->harq_pid;
  p->pusch_data.new_data_indicator = g->ndi;

  p->maintenance_parms_v3.ldpcBaseGraph = 0; // filled below once the TBS is known

  /* THE RECEIVE ANTENNA COUNT FOR THE WHOLE UL CHAIN COMES FROM HERE, not from
   * frame_parms.nb_antennas_rx. nr_ul_channel_estimation.c:550 reads it as
   *     int nb_antennas_rx = pusch_pdu->param_v4.numSpatialStreamIndices;
   * and nr_ulsch_demodulation.c:239 does the same. Leaving it at 0 -- which is what a memset gives,
   * and what this function did -- is not merely "no MU-MIMO": it makes every per-antenna loop empty
   * and every antenna-dimensioned VLA zero-length.
   *
   * It also DEADLOCKS, silently. The channel estimator computes
   *     num_jobs = CEILIDIV(nb_antennas_rx, dmrs_num_antennas_per_thread)   -> 0
   *     init_task_ans(&ans, 0)                                             -> counter 0, sem 0
   *     for (job_id = 0; job_id < 0; ...)                                  -> pushes NOTHING
   *     join_task_ans(&ans)                                                -> sem_wait, forever
   * The semaphore is posted only when the counter reaches exactly zero THROUGH a completion, so a
   * count of zero jobs is never signalled. Measured live: the receiver logged that it was ready and
   * then produced nothing for the remaining ~295 s of the run -- no census, no summary -- with the
   * thread SLEEPING rather than spinning, which is exactly a sem_wait and is why it read as a hang
   * rather than a crash or a busy loop.
   *
   * spatialStreamIndices[] must be filled too: get_first_ant_idx() returns element 0 as the first
   * antenna index, and the chain then indexes rxdataF[aa_start + antenna]. */
  p->param_v4.numSpatialStreamIndices = (uint8_t)nant;
  for (int a = 0; a < nant && a < MAX_NUM_SPATIAL_STREAMS; a++) {
    p->param_v4.spatialStreamIndices[a] = (uint16_t)a;
  }
  p->beamforming.num_prgs               = 0;
  p->beamforming.dig_bf_interface       = nant;
}


/* ------------------------------------------------------------------------------------------
 * CONTINUOUS CFO, ON THE UPLINK FEP.
 *
 * With --cont-fo-comp the LO is deliberately NOT retuned (executables/nr-ue.c:244): the whole
 * offset lives in UE->freq_offset and is removed digitally, per OFDM symbol, in nr_slot_fep()
 * (slot_fep_nr.c:120). Every DL path inherits that for free -- including the passive PDSCH decode,
 * which calls nr_slot_fep().
 *
 * The passive UPLINK path does not: it calls nr_symbol_fep_ul(), the gNB's bare DFT, which has no
 * such hook. Left alone it would demodulate a grant that still carries the full carrier offset --
 * ~14 kHz here, half a subcarrier at 30 kHz SCS -- which is fatal.
 *
 * So do exactly what slot_fep_nr.c does, on the same window nr_symbol_fep_ul() would have read.
 * The offset arithmetic below is lifted verbatim from nr_symbol_fep_ul (slot_fep_nr.c:150-165) so
 * the compensated and uncompensated paths cannot drift apart; the DFT and rotation that follow are
 * the callers' own. With cont_fo_comp off the caller keeps the untouched nr_symbol_fep_ul() path,
 * so behaviour is bit-identical to before.
 * ------------------------------------------------------------------------------------------ */
static unsigned int passive_ul_fep_offset(const NR_DL_FRAME_PARMS *fp, unsigned char slot, unsigned char symbol,
                                          int sample_offset)
{
  uint32_t prefix_length = get_samples_symbol_duration(fp, slot, symbol, 1) - fp->ofdm_symbol_size;
  int64_t off = (int64_t)get_samples_slot_timestamp(fp, slot)
      + get_samples_symbol_timestamp(fp, slot, symbol) + prefix_length
      - fp->nb_prefix_samples / fp->ofdm_offset_divisor - (int64_t)sample_offset;
  /* Delay refinement can legitimately make the advance negative. Mixing it
   * with unsigned offsets previously made the wrap-copy length enormous. */
  off %= (int64_t)fp->samples_per_frame;
  if (off < 0) off += fp->samples_per_frame;
  return (unsigned int)off;
}

/* De-rotate one symbol's worth of samples into scratch, then DFT it. Mirrors nr_symbol_fep_ul()'s
 * wrap handling against samples_per_frame. */
void nr_pusch_passive_fep_symbol(const NR_DL_FRAME_PARMS *fp, const c16_t *rxdata, c16_t *rxdataF,
                              unsigned char symbol, unsigned char slot, int sample_offset, double fo_hz)
{
  const int N = fp->ofdm_symbol_size;
  const unsigned int off = passive_ul_fep_offset(fp, slot, symbol, sample_offset);

  c16_t win[N] __attribute__((aligned(32)));
  if (off + N > (unsigned int)fp->samples_per_frame) {
    const unsigned int first = fp->samples_per_frame - off;
    memcpy(&win[0], &rxdata[off], first * sizeof(c16_t));
    memcpy(&win[first], &rxdata[0], (N - first) * sizeof(c16_t));
  } else {
    memcpy(win, &rxdata[off], N * sizeof(c16_t));
  }

  c16_t rot[N] __attribute__((aligned(32)));
  /* sample_offset argument is the ABSOLUTE sample index, which is what keeps the de-rotation phase
   * continuous from symbol to symbol -- passing 0 here would restart the phase every symbol. */
  nr_fo_compensation(fo_hz, fp->samples_per_subframe, (int)off, win, rot, N);

  dft(get_dft(N), (int16_t *)rot, (int16_t *)rxdataF, 1);
}

/* The producer-timeline slow-time index for this uplink slot. Hoisted into a helper because BOTH
 * CFR sources need it and they MUST agree: a fused CPI mixing two slow-time origins has a
 * meaningless span (measured once as T_slot 67409 against a true ~256, collapsing the velocity axis
 * to zero). See the long comment at the DM-RS submission for why frame*slots_per_frame + slot is
 * the wrong clock. */
static uint32_t passive_ul_slow_time_idx(const NR_DL_FRAME_PARMS *fp,
                                         uint32_t frame,
                                         uint8_t slot,
                                         uint64_t abs_slot)
{
  if (abs_slot != 0) {
    return (uint32_t)abs_slot;
  }
  const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
  const long wrap = (long)fp->slots_per_frame * 1024;
  const long fd   = (long)frame * fp->slots_per_frame + (long)slot;
  const long lag  = ((prod - fd) % wrap + wrap) % wrap;
  return (uint32_t)(prod - lag);
}

static bool nr_pusch_passive_decode_inner(PHY_VARS_NR_UE *ue,
                                          int      ctx,
                                          uint32_t frame,
                                          uint8_t  slot,
                                          const nr_pdcch_blind_ul_result_t *g,
                                          int32_t  ta_offset_samples,
                                          uint64_t abs_slot,
                                          bool     cfr_only,
                                          double   fo_hz,
                                          nr_pusch_passive_out_t *out)
{
  memset(out, 0, sizeof(*out));
  out->status = NR_PUSCH_PASSIVE_ERROR;

  if (ue == NULL || g == NULL || !g->plausible) {
    out->reject_reason = "no plausible grant";
    return false;
  }
  /* Scope guards, each a case this receiver cannot do correctly rather than one it merely has not
   * been tested on. Silently attempting any of them would produce a confident wrong answer. */
  if (g->nrOfLayers != 1) {
    atomic_fetch_add_explicit(&g_rej_unsup, 1, memory_order_relaxed);
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "multi-layer PUSCH";
    return false;
  }
  if (g->transform_precoding) {
    atomic_fetch_add_explicit(&g_rej_unsup, 1, memory_order_relaxed);
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "DFT-s-OFDM (transform precoding) not wired";
    return false;
  }
  if (g->rv != 0) {
    /* No HARQ history: a passive receiver has no earlier redundancy version to combine with, so an
     * rv != 0 transmission carries only incremental parity and cannot be decoded standalone. The
     * DM-RS-based CFR path is unaffected -- it needs only the allocation. */
    atomic_fetch_add_explicit(&g_rej_unsup, 1, memory_order_relaxed);
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "rv != 0 with no HARQ history to combine";
    return false;
  }
  if (!passive_gnb_prepare(ue, ctx)) {
    atomic_fetch_add_explicit(&g_rej_setup, 1, memory_order_relaxed);
    out->reject_reason = "gNB context allocation failed";
    return false;
  }

  /* One-shot STAGE markers. The first live run entered the decode and never returned, and with no
   * gdb on this host and every thread sleeping rather than spinning, the log is the only instrument
   * that can say WHERE. Each prints once; the last one printed is the stage that blocked. */
  static int s_stage = 1;
#define PUSCH_STAGE(n, what) do { if (s_stage) { LOG_I(PHY, "SENSING: PUSCHSTAGE %d %s\n", (n), (what)); } } while (0)
  PUSCH_STAGE(1, "guards passed");
  const int      utim = utim_enabled();
  const uint64_t t_all = utim ? utim_now() : 0;
  uint64_t t_stage = t_all;
  PHY_VARS_gNB *gnb = g_gnb[ctx];
  NR_DL_FRAME_PARMS *fp = &gnb->frame_parms;
  const int nant = g_gnb_nant;
  const int symsz = fp->ofdm_symbol_size;
  const int sps   = fp->symbols_per_slot;

  /* ---- FEP the PUSCH's own symbols into the gNB grid, with the uplink timing advance. ----
   * nr_slot_fep()'s sample_offset is an unsigned quantity ADDED to the window position, but the
   * uplink arrives EARLY, so the correction is negative. Express it modulo the rxdata ring instead
   * of passing a negative number: the FEP wraps its reads against the same total, so an offset of
   * (total - advance) lands exactly where (-advance) would. */
  /* Round-robin over the sweep set when one is configured. The index is carried into the census
   * below so a value's outcome is attributable; without a sweep this is inert. */
  int ta_idx = -1;
  if (ta_sweep_points() > 0) {
    ta_idx = (int)(atomic_fetch_add_explicit(&g_ta_sweep_seq, 1, memory_order_relaxed)
                   % (uint32_t)ta_sweep_points());
    ta_offset_samples = g_ta_sweep_start + (int32_t)ta_idx * g_ta_sweep_step;
  }

  /* SIGN. nr_symbol_fep_ul() does `rxdata_offset -= sample_offset` (slot_fep_nr.c:158), so a
   * POSITIVE value reads EARLIER -- which is exactly what a timing advance is. The value is passed
   * through unchanged.
   *
   * Note this is the OPPOSITE of nr_slot_fep(), which does `rx_offset += sample_offset` (:85). The
   * two functions take an identically-named argument with inverted meaning. This path used to call
   * nr_slot_fep() and passed the advance straight through, so it read 1600 samples LATE where it
   * needed 1600 EARLY. */
  const int32_t ul_sample_offset = ta_offset_samples;

  const int slot_off = (slot % RU_RX_SLOT_DEPTH) * sps * symsz;
  /* ---- FEP the grant's symbols, EXACTLY as the gNB does -----------------------------------------
   * nr_symbol_fep_ul() (bare DFT, offset convention that SUBTRACTS) followed by
   * apply_nr_rotation_symbol_RX() with fp->symbol_rotation[link_type_ul] and fp->N_RB_UL. That pair
   * is nr_ofdm_demod_and_rx_rotation() (slot_fep_nr.c:219), which is what ulsim feeds
   * phy_procedures_gNB_uespec_RX() with -- so it, not the RU's nr_fep(), is the authority on what
   * nr_rx_pusch_group_tp() expects, and it expects ROTATED rxdataF.
   *
   * Writing straight into the gNB grid also avoids the ~917 kB scratch buffer and per-symbol memcpy
   * an intermediate layout would need.
   *
   * Factored out because it is run TWICE per grant -- see the delay refinement below. */
#define PASSIVE_UL_FEP(off_)                                                                       \
  do {                                                                                             \
    const double fo_hz_ = fo_hz;                                                                   \
    const int s0_ = g->start_symbol;                                                               \
    const int s1_ = g->start_symbol + g->num_symbols;                                              \
    for (int a_ = 0; a_ < nant; a_++) {                                                            \
      const c16_t *rx_ = (const c16_t *)ue->common_vars.rxdata[a_];                                \
      for (int sym_ = s0_; sym_ < s1_ && sym_ < sps; sym_++) {                                     \
        c16_t *dst_ = &gnb->common_vars.rxdataF[a_][slot_off + sym_ * symsz];                      \
        if (fo_hz_ != 0.0) {                                                                       \
          nr_pusch_passive_fep_symbol(fp, rx_, dst_, (unsigned char)sym_, (unsigned char)slot, (off_), fo_hz_);\
        } else {                                                                                   \
          nr_symbol_fep_ul(fp, rx_, dst_, (unsigned char)sym_, (unsigned char)slot, (off_));       \
        }                                                                                          \
        apply_nr_rotation_symbol_RX(fp->symbols_per_slot, fp->slots_per_subframe,                  \
                                    fp->timeshift_symbol_rotation, fp->first_carrier_offset,       \
                                    dst_, fp->symbol_rotation[link_type_ul], fp->N_RB_UL,          \
                                    slot, sym_);                                                   \
      }                                                                                            \
    }                                                                                              \
  } while (0)

  PASSIVE_UL_FEP(ul_sample_offset);
  if (abs_slot && !nr_passive_samples_valid(
      atomic_load_explicit(&nr_ue_diag_producer_absolute_slot,memory_order_relaxed),
      (long)abs_slot,fp->slots_per_frame)) {
    out->status=NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason="IQ expired during PUSCH FEP";
    return false;
  }


  /* ---- TBS, then the receive chain, exactly as the gNB runs it. ---- */
  if (utim) {
    utim_add(UTIM_FEP, t_stage);
    t_stage = utim_now();
  }
  PUSCH_STAGE(3, "FEP done");
  nfapi_nr_pusch_pdu_t pdu;
  fill_pusch_pdu(g, nant, fp, &pdu);
  /* The reused estimator takes its pilot seed from this private context.
   * Keep physical PCI in frame_parms separate from the grant's DM-RS identity. */
  gnb->gNB_config.cell_config.phy_cell_id.value = g->ul_dmrs_scrambling_id;

  const int n_dmrs_sym = __builtin_popcount((unsigned)g->ul_dmrs_symb_pos
                                            & (((1u << g->num_symbols) - 1u) << g->start_symbol));
  const int nb_dmrs_re_per_rb = ((g->dmrs_config_type == 0) ? 6 : 4) * g->n_dmrs_cdm_groups;
  const uint32_t tbs = nr_compute_tbs(pdu.qam_mod_order, pdu.target_code_rate, g->num_rb, g->num_symbols,
                                      nb_dmrs_re_per_rb * n_dmrs_sym, 0, 0, g->nrOfLayers);
  if (tbs == 0) {
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "TBS computed as zero";
    return false;
  }
  pdu.pusch_data.tb_size = tbs >> 3;
  pdu.maintenance_parms_v3.ldpcBaseGraph = get_BG(tbs, pdu.target_code_rate);

  NR_gNB_ULSCH_t *ulsch = &gnb->ulsch[0];
  ulsch->rnti     = g->rnti;
  ulsch->frame    = frame;
  ulsch->slot     = slot;
  ulsch->harq_pid = 0;
  ulsch->active   = true;
  ulsch->harq_process->ulsch_pdu = pdu;
  ulsch->harq_process->harq_to_be_cleared = true;
  ulsch->unav_res = 0;

  NR_gNB_PUSCH *pvp = &gnb->pusch_vars[0];
  const nfapi_nr_pusch_pdu_t *pdup = &ulsch->harq_process->ulsch_pdu;
  uint32_t *unavp = &ulsch->unav_res;
  if (!cfr_only) {
    atomic_fetch_add_explicit(&g_try, 1, memory_order_relaxed);
  }
  PUSCH_STAGE(4, "entering nr_rx_pusch_group_tp");
  nr_rx_pusch_group_tp(gnb, &pvp, &pdup, &unavp, 1, frame, slot);

  /* ---- PER-GRANT DELAY REFINEMENT ---------------------------------------------------------------
   * A FIXED timing advance cannot work here, and the reason is measurable rather than theoretical.
   * With ta pinned at the derived N_TA_offset, the residual delay this receiver measures per grant
   * walks a clean SAWTOOTH -- monotonically from about -387 to +433 samples over ~2 s, then wraps.
   * That is roughly 400 samples/s, ~3.3 ppm: sample-clock drift between a free-running receiver and
   * the gNB, which is exactly the condition this project targets (no shared hardware reference).
   *
   * nr_pusch_channel_estimation() compensates a delay only within +/-MAX_DELAY_COMP (20 samples),
   * so a grant decodes only when the ramp happens to pass near zero. Measured on the first capture
   * that decoded anything: the three successes had est_delay 8, 8 and 13, while the 58 failures of
   * the same shape were scattered across -387..+433.
   *
   * So the window is re-placed using the delay the estimator itself just measured, and the receive
   * chain re-run once. A positive est_delay means the CIR peak sits LATE of the window start, so the
   * window must start later, i.e. the ADVANCE must shrink: ta_new = ta - est_delay.
   *
   * One retry, not a loop: the correction is a direct measurement rather than a search, and a second
   * pass that still lands outside the compensation range means the estimate itself was unreliable
   * (nr_est_delay returns 0 unless the CIR peak clears PEAK_DETECT_THRESHOLD, so a poor grant simply
   * asks for no correction). Tracking the ramp across grants was considered and rejected: consecutive
   * grants are tens of ms apart, which is ~90 samples of drift, already far outside +/-20. */
  {
    const int d = pvp->delay.est_delay;
    if (d != 0 && (d > PASSIVE_UL_DELAY_TOL || d < -PASSIVE_UL_DELAY_TOL)) {
      atomic_fetch_add_explicit(&g_ta_refined, 1, memory_order_relaxed);
      PASSIVE_UL_FEP(ul_sample_offset - d);
  if (abs_slot && !nr_passive_samples_valid(
      atomic_load_explicit(&nr_ue_diag_producer_absolute_slot,memory_order_relaxed),
      (long)abs_slot,fp->slots_per_frame)) {
    out->status=NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason="IQ expired during PUSCH FEP";
    return false;
  }

      nr_rx_pusch_group_tp(gnb, &pvp, &pdup, &unavp, 1, frame, slot);
      out->est_delay_pre = d;
    }
  }

  if (utim) {
    utim_add(UTIM_RXPUSCH, t_stage);
    t_stage = utim_now();
  }
  PUSCH_STAGE(5, "rx_pusch returned, entering nr_ulsch_decoding");

  /* ---- UPLINK CFR from the PUSCH DM-RS ------------------------------------------------------
   * Placed HERE, after nr_rx_pusch_group_tp() has filled ul_ch_estimates and BEFORE the LDPC
   * decode, deliberately: the DM-RS channel estimate does not depend on the transport block, so
   * gating it on CRC would throw away a perfectly good measurement every time a decode fails. The
   * data-aided UL CFR is the one that needs a CRC-verified TB; this one does not.
   *
   * Layout: ul_ch_estimates[nl * num_sp_streams + antenna] is a per-symbol buffer indexed
   * [ofdm_symbol_size * symbol + k], with k an ABSOLUTE subcarrier. num_sp_streams is
   * param_v4.numSpatialStreamIndices -- the same field whose being zero deadlocked this function,
   * so it is read back from the PDU rather than assumed equal to nant.
   *
   * The antennas are kept SEPARATE (nof_ant > 1, ant_stride_re) rather than combined: the whole
   * point of a 4-element array is that the inter-element phase carries the bearing, and combining
   * before submission would destroy exactly that. */
  if (nr_isac_enabled() && nr_isac_source_enabled(NR_ISAC_SRC_PUSCH_DMRS)) {
    const uint32_t nof_ant_cfr = (uint32_t)nant;
    const int      num_sp      = pdu.param_v4.numSpatialStreamIndices;
    /* First DM-RS symbol inside the allocation. TS 38.211 puts the front-loaded one at l0, and it
     * is the strongest; the additional positions are used by the estimator but one symbol is what
     * a slow-time row wants. */
    int dmrs_sym = -1;
    for (int m = g->start_symbol; m < g->start_symbol + g->num_symbols; m++) {
      if (g->ul_dmrs_symb_pos & (1u << m)) {
        dmrs_sym = m;
        break;
      }
    }
    const int start_sc = ((g->bwp_start + g->start_rb) * NR_NB_SC_PER_RB + fp->first_carrier_offset)
                         % fp->ofdm_symbol_size;
    const int num_sc = g->num_rb * NR_NB_SC_PER_RB;
    if (dmrs_sym >= 0 && num_sp > 0 && num_sc > 0) {
      static __thread float    *ul_h = NULL;
      static __thread uint32_t *ul_k = NULL, *ul_l = NULL;
      static __thread uint32_t  ul_cap = 0;
      const uint32_t cap = (uint32_t)(273 * NR_NB_SC_PER_RB);
      if (ul_cap < cap) {
        free(ul_h); free(ul_k); free(ul_l);
        ul_h = (float *)malloc16_clear(sizeof(float) * 2 * PASSIVE_UL_MAX_ANT * cap);
        ul_k = (uint32_t *)malloc16_clear(sizeof(uint32_t) * cap);
        ul_l = (uint32_t *)malloc16_clear(sizeof(uint32_t) * cap);
        ul_cap = (ul_h && ul_k && ul_l) ? cap : 0;
      }
      /* ONE-SHOT INDEX AUDIT. The conventions above are read off nr_ul_channel_estimation.c, and a
       * prior version of this loop used the absolute index and produced numbers that LOOKED fine on
       * near-full-band grants. So the choice is measured here rather than trusted: print the energy
       * the estimate carries at the relative index against the absolute one for the first grant. If
       * relative is not the larger by a wide margin, this comment is wrong and so is the code. */
      {
        static __thread int s_audit = 1;
        if (s_audit) {
          s_audit = 0;
          const c16_t *h0 = (const c16_t *)&pvp->ul_ch_estimates[0][fp->ofdm_symbol_size * dmrs_sym];
          double e_rel = 0.0, e_abs = 0.0;
          for (int j = 0; j < num_sc; j++) {
            const int ka = (start_sc + j) % fp->ofdm_symbol_size;
            e_rel += (double)h0[j].r * h0[j].r + (double)h0[j].i * h0[j].i;
            e_abs += (double)h0[ka].r * h0[ka].r + (double)h0[ka].i * h0[ka].i;
          }
          LOG_I(PHY,
                "SENSING: ULCFRIDX dmrs_sym=%d start_sc=%d num_sc=%d num_sp=%d E_rel=%.3e E_abs=%.3e\n",
                dmrs_sym, start_sc, num_sc, num_sp, e_rel, e_abs);
        }
      }
      uint32_t nof_re = 0;
      double   pw = 0.0;
      double   ant_pw[PASSIVE_UL_MAX_ANT] = {0};
      /* TWO DIFFERENT SUBCARRIER CONVENTIONS, and mixing them is what this loop got wrong.
       *
       * nr_pusch_channel_estimation() writes ul_ch_estimates[..][symbolSize*symbol + n] with n
       * RELATIVE to the allocation, n = 0 .. rb_size*12-1: it starts each symbol's buffer at index
       * 0 and walks forward (nr_ul_channel_estimation.c:109 `ul_ch = &..[symbol_offset]`, then
       * `ul_ch += 4`, and the delta realignment at :246 shifts within `nb_rb_pusch*12`). Only
       * rxdataF is absolute there -- it is read as `rx[(k0 + n) % symbolSize]`.
       *
       * Reading it at the ABSOLUTE subcarrier was silently wrong in a way sized by the grant: a
       * near-full-band allocation still lands inside the written region and returns plausible
       * nonsense, while a small one reads untouched memory and returns EXACTLY ZERO. Measured both:
       * 256-PRB grants gave pw=[13333 579 58538 53579] (wrong, but believable) and 7-PRB grants
       * gave pw=[0 0 0 0] over 308628 REs, which is what made it visible at all.
       *
       * The REPORTED subcarrier (ul_k below) stays ABSOLUTE and CRB-referenced -- that is what the
       * CPI grid indexes on, and it was always correct. Only the read index was wrong. */
      for (int j = 0; j < num_sc && nof_re < ul_cap; j++) {
        const int k_abs = (start_sc + j) % fp->ofdm_symbol_size;
        for (uint32_t a = 0; a < nof_ant_cfr; a++) {
          /* layer 0 only: this receiver rejects multi-layer PUSCH upstream, and a second layer
           * would need its own submission rather than being folded into this one. */
          const c16_t *h = (const c16_t *)&pvp->ul_ch_estimates[0 * num_sp + (int)a][fp->ofdm_symbol_size * dmrs_sym];
          const size_t o = 2 * ((size_t)a * cap + nof_re);
          ul_h[o]     = (float)h[j].r;
          ul_h[o + 1] = (float)h[j].i;
          const double p2 = (double)h[j].r * h[j].r + (double)h[j].i * h[j].i;
          if (a == 0) {
            pw += p2;
          }
          if (a < PASSIVE_UL_MAX_ANT) {
            ant_pw[a] += p2;
          }
        }
        ul_k[nof_re] = (uint32_t)k_abs;
        ul_l[nof_re] = (uint32_t)dmrs_sym;
        nof_re++;
      }
      if (nof_re > 0) {
        /* ul_CarrierFreq, not dl_CarrierFreq: the range axis scales with the wavelength of the
         * signal actually observed, and on a TDD cell these are equal only by coincidence of the
         * duplex spacing being zero. */
        nr_isac_carrier_t carrier = {.nof_prb         = (uint32_t)fp->N_RB_UL,
                                     .scs_hz          = fp->subcarrier_spacing,
                                     .dl_center_hz    = fp->ul_CarrierFreq,
                                     .pci             = fp->Nid_cell,
                                     .slots_per_frame = fp->slots_per_frame};
        /* SLOW-TIME EPOCH. Must match what pdsch_data is indexed on, or a fused CPI mixes two
         * different origins and its span is meaningless -- measured: T_slot 67409 against a true
         * ~256, and the velocity axis collapsed to vel[res=0.000 max=0.0] with every "detection"
         * pinned at 0 m/s.
         *
         * pdsch_data uses the PRODUCER's monotonic counter (nr_pdsch_passive_queue.c stamps
         * job.absolute_slot through nr_isac_abs_slot_override). The obvious frame*slots_per_frame +
         * slot used here before is a DIFFERENT clock: it wraps at slots_per_frame*1024, so the two
         * sources drift apart by a whole wrap period and never share a timeline.
         *
         * This decode runs in-line in the uplink slot, but the producer counter still runs AHEAD of
         * the slot being processed by the pipeline depth. Align it to this slot's phase: both
         * counters advance one per slot, so subtracting the phase difference modulo the wrap yields
         * the producer-timeline value FOR THIS SLOT. Exact while the lag stays under one wrap
         * (~10 slots in practice against a 20480-slot wrap). */
        const uint32_t ul_slot_idx = passive_ul_slow_time_idx(fp, frame, slot, abs_slot);
        nr_isac_submit_cfr_multi(ul_slot_idx, 0.0f,
                                 NR_ISAC_SRC_PUSCH_DMRS, &carrier, ul_h, nof_ant_cfr, cap,
                                 ul_k, ul_l, nof_re, 1.0f);
        atomic_fetch_add_explicit(&g_cfr_re, nof_re, memory_order_relaxed);
        for (uint32_t a = 0; a < nof_ant_cfr && a < PASSIVE_UL_MAX_ANT; a++) {
          /* Accumulate the SUM and divide once at report time. Dividing per grant and casting to
           * uint64_t truncated to ZERO on small allocations -- measured pw=[0 0 0 0] on ~110-RE
           * HARQ-only grants, i.e. the probe went silently useless at exactly low UL load. */
          atomic_fetch_add_explicit(&g_ant_pw[a], (uint64_t)ant_pw[a], memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&g_ant_n, nof_re, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_cfr_submits, 1, memory_order_relaxed);
        out->snr_db = (pw > 0.0 && nof_re) ? (float)(10.0 * log10(pw / (double)nof_re)) : 0.0f;
      }
    }
  }

  /* ---- UL DM-RS SCRAMBLING IDENTITY (review fix round 1, finding 2) --------------------------
   * Placed HERE, same spot and same reasoning as the PUSCH_DMRS CFR block just above: the DM-RS
   * sequence is fixed by (nid, slot, symbol) regardless of whether the transport block later
   * decodes, so gating this on CRC (as it used to be, further down past nr_ulsch_decoding()) was
   * circular -- on a cell whose UL DM-RS id differs from the PCI the CRC never passes, so the id
   * that would explain the failures could never be measured. Mirrors the DL fix in
   * nr_pdsch_passive_queue.c exactly: one state per nSCID, staged range (0..1023 then the full
   * remaining space once stage 1 exhausts itself without deciding), stage-2 accumulate throttled
   * (same ~64x-cost argument, same throttle constant). */
  /* Final review I2/I5: DCI 0_1 grants only (a 0_0 uses N_ID^cell, see blind_ul_apply_scrambling_ids()),
   * and the two-window driver (stage 1 always on, stage 2 throttled + capped, all work stops once decided).
   * dmrs_config_type is no longer restricted to type 1: nr_dmrs_id_2stage_accumulate()/
   * nr_dmrs_id_accumulate() now take the type and generate the matching reference sequence/RE
   * pattern for both (dmrs_nr.c already supported type 2 generation; this estimator's own RE
   * stepping was the part that was type-1-only). transform_precoding stays excluded: low-PAPR
   * DM-RS uses a different sequence generator (nr_pusch_lowpaprtype1_dmrs_rx) this probe never
   * calls, a separate gap. */
  if (nr_pusch_passive_queue_running() && g->ul_dci_format == NR_BLIND_UL_DCI_FORMAT_0_1
      && !g->transform_precoding && nr_dmrs_id_2stage_decided(nr_pusch_passive_ul_dmrs_id(g->nscid)) < 0) {
    const int ul_ns = g->nscid & 1;
    bool was_init = false;
    nr_dmrs_id_2stage_t *dst = nr_pusch_passive_ul_dmrs_trylock(ul_ns, &was_init);
    if (dst) {
      if (!was_init)
        nr_dmrs_id_2stage_init(dst, "PUSCH", g->ul_dmrs_scrambling_id); /* undecided 0_1 => PCI or opts override */
      int dsym = -1;
      for (int m_ = g->start_symbol; m_ < g->start_symbol + g->num_symbols; m_++)
        if (g->ul_dmrs_symb_pos & (1u << m_)) { dsym = m_; break; }
      if (dsym >= 0) {
        /* Same slot_off/fp already computed for the FEP above; absolute subcarriers. */
        const c16_t *row = &gnb->common_vars.rxdataF[0][slot_off + dsym * fp->ofdm_symbol_size];
        const int start_sc = fp->first_carrier_offset + (g->bwp_start + g->start_rb) * NR_NB_SC_PER_RB;
        nr_dmrs_id_2stage_accumulate(dst, row, fp->ofdm_symbol_size, start_sc, g->bwp_start + g->start_rb, g->num_rb,
                                     fp->N_RB_UL, fp->symbols_per_slot, slot, dsym, g->nscid, fp->Ncp == NR_NORMAL,
                                     g->dmrs_config_type);
      }
      nr_pusch_passive_ul_dmrs_unlock(ul_ns);
    }
  }

  /* G is needed by the LLR probe below, which runs BEFORE nr_ulsch_decoding(). It used to be
   * assigned after it, so the probe read G = 0 and reported an empty LLR field on every grant --
   * a probe reporting its own initialisation order rather than the receiver. */
  out->G = nr_get_G(g->num_rb, g->num_symbols, nb_dmrs_re_per_rb, n_dmrs_sym, 0,
                    pdu.qam_mod_order, g->nrOfLayers);

  if (utim) {
    utim_add(UTIM_CFR, t_stage);
    t_stage = utim_now();
  }

  /* ---- TWO PROBES THAT SEPARATE THE TWO WAYS A DECODE CAN FAIL (ISAC_PUSCH_DIAG=1) -------------
   *
   * The transport-block parameters are already verified exact against the gNB's own log (same
   * rb_start, rb_size, symbols, MCS, TBS, and G = rb*12*11*Qm to the bit), and the gNB decodes the
   * same grants at 100 %. So the fault is in the SIGNAL path, and there are two candidates that a
   * CRC result alone cannot tell apart:
   *
   *   COHERENCE: the lag-1 correlation of the channel estimate across subcarriers. A real channel
   *   is smooth on a 15 kHz grid, so a correct DM-RS sequence at the right position gives |rho|
   *   near 1. A wrong sequence, a wrong scrambling identity or a wrong RE position correlates noise
   *   against noise and gives |rho| near 0 -- while still producing plenty of ENERGY, which is why
   *   energy alone (E_rel=5.6e6) proves nothing about correctness. Same discriminator the downlink
   *   work used to prove its DM-RS generator right (16.8 % vs a 1.0 % wrong-sequence control).
   *
   *   LLR OCCUPANCY: mean |LLR| and the fraction that are non-trivial. An LLR field that is empty
   *   or saturated means the demodulator never produced usable soft bits, and the LDPC failure is
   *   then downstream of a problem that has nothing to do with coding.
   *
   * Printed per grant so they can be correlated with allocation and with the gNB's own per-grant
   * verdict, not just averaged. */
  if (s_diag_on()) {
    const c16_t *h = (const c16_t *)&pvp->ul_ch_estimates[0][fp->ofdm_symbol_size * g->start_symbol];
    int dsym = -1;
    for (int m = g->start_symbol; m < g->start_symbol + g->num_symbols; m++) {
      if (g->ul_dmrs_symb_pos & (1u << m)) { dsym = m; break; }
    }
    if (dsym >= 0) {
      h = (const c16_t *)&pvp->ul_ch_estimates[0][fp->ofdm_symbol_size * dsym];
    }
    const int nsc = g->num_rb * NR_NB_SC_PER_RB;
    double acc_r = 0.0, acc_i = 0.0, e0 = 0.0;
    for (int j = 0; j + 1 < nsc; j++) {
      /* h[j] * conj(h[j+1]) summed: the magnitude of the sum over the sum of energies is the
       * normalised lag-1 coherence. */
      acc_r += (double)h[j].r * h[j + 1].r + (double)h[j].i * h[j + 1].i;
      acc_i += (double)h[j].i * h[j + 1].r - (double)h[j].r * h[j + 1].i;
      e0    += (double)h[j].r * h[j].r + (double)h[j].i * h[j].i;
    }
    const double rho = (e0 > 0.0) ? sqrt(acc_r * acc_r + acc_i * acc_i) / e0 : 0.0;

    const int16_t *llr = pvp->llr;
    const uint32_t nllr = out->G;
    double sum_abs = 0.0;
    uint32_t nz = 0;
    for (uint32_t q = 0; q < nllr; q++) {
      const int v = llr[q] < 0 ? -llr[q] : llr[q];
      sum_abs += v;
      if (v > 4) { nz++; }
    }
    LOG_I(PHY, "SENSING: ULSIG rho=%.3f e_chest=%.3e llr_n=%u llr_mean=%.1f llr_active=%.2f\n",
          rho, e0, nllr, nllr ? sum_abs / nllr : 0.0, nllr ? (double)nz / nllr : 0.0);
  }

  if (cfr_only) {
    /* The CFR is already submitted above. Everything past this point exists to produce a transport
     * block, which mode 2 does not want -- so stop here rather than paying for it and discarding
     * the result. Reported as UNSUPPORTED so it lands in a reject bucket rather than inflating
     * either the try or the CRC-failure count: a decode that was never attempted must not read as
     * a decode that failed. */
    out->status = NR_PUSCH_PASSIVE_UNSUPPORTED;
    out->reject_reason = "cfr_only mode: LDPC decode deliberately skipped";
    return false;
  }

  int ulsch_id = 0;
  int rc = nr_ulsch_decoding(gnb, fp, frame, slot, &ulsch_id, 1);

  /* Expensive, bounded OFFLINE diagnostic, never enabled by full_auto alone.
   * Preserve descrambled LLRs and remove actual interleaved UCI positions.
   * A CRC win identifies a footprint candidate, not O_ACK/CSI/RRC configuration. */
  if (getenv("ISAC_PASSIVE_REPLAY_INPUT") && getenv("ISAC_PASSIVE_REPLAY_UL_UCI")
      && rc==0 && hp_crc_failed(ulsch) && g->nrOfLayers==1) {
    const uint32_t full_bits=out->G;
    int16_t *original=malloc(full_bits*sizeof(*original));
    if (original) {
      memcpy(original,pvp->llr,full_bits*sizeof(*original));
      bool rescued=false;
      for(int csi=0;csi<2 && !rescued;++csi) {
        for(unsigned re=1;re<=1024u/pdu.qam_mod_order;++re) {
          const uint32_t data_bits=nr_passive_uci_probe_demux(original,full_bits,pvp->llr,
              g->num_rb,g->start_symbol,g->num_symbols,g->ul_dmrs_symb_pos,
              nb_dmrs_re_per_rb,pdu.qam_mod_order,re,csi);
          if(!data_bits) continue;
          ulsch->unav_res=re;
          ulsch->harq_process->harq_to_be_cleared=true;
          rc=nr_ulsch_decoding(gnb,fp,frame,slot,&ulsch_id,1);
          if(rc==0 && !hp_crc_failed(ulsch)) {
            rescued=true;
            out->G=data_bits;
            out->uci_ack_re=csi?0:re;
            LOG_I(PHY,"REPLAY-UCI source=%lu rnti=%04x kind=%s re=%u bits=%u tbs=%u CRC candidate\n",
                  (unsigned long)abs_slot,g->rnti,csi?"CSI":"ACK",re,full_bits-data_bits,tbs>>3);
            break;
          }
        }
      }
      if(!rescued) {
        memcpy(pvp->llr,original,full_bits*sizeof(*original));
        ulsch->unav_res=0;
        ulsch->harq_process->harq_to_be_cleared=true;
        rc=nr_ulsch_decoding(gnb,fp,frame,slot,&ulsch_id,1);
      }
      free(original);
    }
  }

  /* ---- LIVE UCI-ON-PUSCH RECOVERY ---------------------------------------------------------------
   * The previous reservation-only retry could not work and is gone: it changed `unav_res`, which
   * demodulation then overwrote, and it never removed the interleaved UCI positions from the LLR
   * stream. CSI Part 1 is ALWAYS rate-matched (G_ulsch = G - E_CSI1 - E_CSI2), so the LLRs must be
   * COMPACTED at the UCI positions, never merely truncated.
   *
   * nr_passive_uci_probe_demux() does that inversion. What it lacks is an affordable live policy --
   * its offline sweep tries every RE count up to 1024 coded bits per failed grant. The footprint is
   * fixed by the UE's report configuration and beta offsets, so it REPEATS: measured offline on this
   * cell only a few distinct values occur (ACK 17/22 RE, CSI 38/33 RE). So try what has worked for
   * this RNTI first, and pay for a wide sweep on only one failure in `explore_every`.
   *
   * A recovered RE count is an inferred FOOTPRINT, not a known O_ACK, CSI report size or beta
   * offset, and it is never written into configuration. Single-layer, no-PTRS scope, matching the
   * inverse it uses; combined ACK+CSI layouts and small-ACK puncturing are not covered. */
  const nr_pdcch_blind_monitor_cfg_t *ucfg = nr_pdcch_blind_monitor_get_cfg();
  /* Width hypotheses must be scored by the same decoder as settled grants.
   * Waiting for a width winner before recovering UCI is circular when UCI
   * prevents the correct width from passing. The existing per-identity rate
   * limiter bounds exploration; cache entries still require a verified TB CRC. */
  if (rc == 0 && hp_crc_failed(ulsch) && ucfg != NULL && ucfg->ul_uci_search > 0
      && g->nrOfLayers == 1 && out->G > 0) {
    const uint32_t full_bits = out->G;
    int16_t *original = malloc((size_t)full_bits * sizeof(*original));
    if (original == NULL) {
      out->status = NR_PUSCH_PASSIVE_ERROR;
      out->reject_reason = "UCI recovery scratch allocation failed";
      return false;
    }
    memcpy(original, pvp->llr, (size_t)full_bits * sizeof(*original));

    /* try(): one demux + one LDPC attempt at a candidate footprint. */
    bool rescued = false;
    uint32_t won_bits = 0; unsigned won_re = 0; int won_csi = 0;
    #define UCI_ATTEMPT(RE, CSI)                                                                   \
      do {                                                                                         \
        const uint32_t db = nr_passive_uci_probe_demux(original, full_bits, pvp->llr,               \
            g->num_rb, g->start_symbol, g->num_symbols, g->ul_dmrs_symb_pos,                        \
            nb_dmrs_re_per_rb, pdu.qam_mod_order, (RE), (CSI));                                     \
        if (db) {                                                                                   \
          atomic_fetch_add_explicit(&g_uci_trials, 1, memory_order_relaxed);                        \
          ulsch->unav_res = (RE);                                                                   \
          ulsch->harq_process->harq_to_be_cleared = true;                                           \
          rc = nr_ulsch_decoding(gnb, fp, frame, slot, &ulsch_id, 1);                               \
          if (rc == 0 && !hp_crc_failed(ulsch)) {                                                   \
            rescued = true; won_bits = db; won_re = (RE); won_csi = (CSI);                          \
          }                                                                                         \
        }                                                                                           \
      } while (0)

    /* 1. What already worked for this UE, most successful first. A couple of attempts. */
    nr_uci_footprint_t known[NR_UCI_LEARN_FOOTPRINTS];
    const int n_known = nr_passive_uci_learn_get(g->rnti, known, NR_UCI_LEARN_FOOTPRINTS);
    for (int i = 0; i < n_known && !rescued; i++) UCI_ATTEMPT(known[i].re, known[i].csi != 0);

    /* 2. Otherwise explore, rate-limited, within a bounded RE budget. */
    if (!rescued && nr_passive_uci_learn_should_explore(g->rnti, (unsigned)ucfg->ul_uci_explore_every)) {
      const unsigned cap = (unsigned)ucfg->ul_uci_search;
      for (int csi = 0; csi < 2 && !rescued; ++csi)
        for (unsigned re = 1; re <= cap && !rescued; ++re) UCI_ATTEMPT(re, csi);
    }
    #undef UCI_ATTEMPT

    if (rescued) {
      atomic_fetch_add_explicit(&g_uci_rescued, 1, memory_order_relaxed);
      nr_passive_uci_learn_record(g->rnti, (uint16_t)won_re, won_csi != 0);
      out->G = won_bits;
      out->uci_ack_re = (uint16_t)won_re;   /* inferred footprint; CSI vs ACK distinguished below */
      out->o_ack = 0;                        /* unknown: a footprint is not an O_ACK */
      static uint64_t logged;
      if (++logged <= 12 || (logged % 500) == 0)
        LOG_I(PHY, "SENSING: UCI recovered rnti=%04x kind=%s re=%u removed_bits=%u (learned)\n",
              g->rnti, won_csi ? "CSI" : "ACK", won_re, full_bits - won_bits);
    } else {
      /* Restore the untouched stream and re-run, so a failed search leaves no trace in the result. */
      memcpy(pvp->llr, original, (size_t)full_bits * sizeof(*original));
      ulsch->unav_res = 0;
      ulsch->harq_process->harq_to_be_cleared = true;
      rc = nr_ulsch_decoding(gnb, fp, frame, slot, &ulsch_id, 1);
    }
    free(original);
  }

  if (utim) {
    utim_add(UTIM_DECODE, t_stage);
    /* slots_per_frame is 10 * 2^mu; the slot budget is 10 ms / slots_per_frame. */
    utim_total(utim_now() - t_all,
               (fp->slots_per_frame > 0) ? (10000000ull / (uint64_t)fp->slots_per_frame) : 0);
  }
  PUSCH_STAGE(6, "ulsch_decoding returned");
  s_stage = 0;
#undef PUSCH_STAGE

  out->qam_mod_order = pdu.qam_mod_order;
  out->nb_rb         = g->num_rb;
  out->nb_symbols    = g->num_symbols;
  out->tbs_bytes     = tbs >> 3;
  out->est_delay     = pvp->delay.est_delay;
  if (pvp->ulsch_noise_power_tot > 0) {
    out->snr_db = 10.0f * log10f((float)pvp->ulsch_power_tot / (float)pvp->ulsch_noise_power_tot);
  }

  /* `rc` is the LDPC coding INTERFACE return, not the CRC verdict -- nr_ulsch_decoding() returns
   * whatever nrLDPC_coding_decoder() gave it and reports the actual per-segment CRC only through
   * harq_process->processedSegments (nr_ulsch_decoding.c:289 computes exactly this expression and
   * calls it `crcok`). Checking rc alone accepts a transport block whose segments failed, so the
   * two are checked separately here, and a segment failure is counted apart from the all-zero case
   * below: they have completely different causes. */
  NR_UL_gNB_HARQ_t *hp = ulsch->harq_process;
  out->n_segments  = (int)hp->C;
  out->segments_ok = (int)hp->processedSegments;
  if (rc != 0 || hp->b == NULL) {
    out->status = NR_PUSCH_PASSIVE_CRC_FAIL;
    out->reject_reason = "LDPC decoder interface error";
    return false;
  }
  if (ta_idx >= 0) {
    atomic_fetch_add_explicit(&g_ta_try[ta_idx], 1, memory_order_relaxed);
    if (hp->C > 0 && hp->processedSegments == hp->C) {
      atomic_fetch_add_explicit(&g_ta_ok[ta_idx], 1, memory_order_relaxed);
    }
  }
  if (hp_crc_failed(ulsch)) {
    atomic_fetch_add_explicit(&g_seg_fail, 1, memory_order_relaxed);
    nr_pusch_passive_ul_crc_note(g->rnti, g->ul_dci_format == NR_BLIND_UL_DCI_FORMAT_0_1, false);
    out->status = NR_PUSCH_PASSIVE_CRC_FAIL;
    out->reject_reason = "segment or final transport-block CRC failed";
    if (g->data_id_advance)
      nr_pusch_passive_data_id_feed(false);
    return false;
  }

  /* All-zero-payload guard, the uplink twin of nr_pdsch_passive_decode.c's. An all-zero transport
   * block carries an all-zero CRC24, so it passes the CRC by construction -- a false pass, not a
   * decode. On the downlink this population turned out to be REAL (the gNB emits PDSCH with no MAC
   * PDU when it has no HARQ buffer, 30.4 % of grants), so the same shape on the uplink is REPORTED
   * rather than silently dropped: counted, excluded from crc_ok, and never allowed to reach a
   * data-aided reconstruction where it would inject a constant X and hence a meaningless CFR.
   * `b` is malloc16_clear'd to a_segments*1056 bytes, past tbs, so reading the two CRC bytes just
   * beyond the payload is in bounds. */
  const uint32_t sz = (uint32_t)(tbs >> 3);
  if (sz > 0 && hp->b[sz] == 0 && hp->b[sz + 1] == 0) {
    uint32_t i = 0;
    while (i < sz && hp->b[i] == 0) {
      i++;
    }
    if (i == sz) {
      atomic_fetch_add_explicit(&g_zero_tb, 1, memory_order_relaxed);
      out->status = NR_PUSCH_PASSIVE_ZERO_TB;
      out->reject_reason = "all-zero transport block (CRC passes by construction)";
      return false;
    }
  }

  out->status = NR_PUSCH_PASSIVE_OK;
  out->tb     = hp->b;
  atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
  nr_pusch_passive_ul_crc_note(g->rnti, g->ul_dci_format == NR_BLIND_UL_DCI_FORMAT_0_1, true);
  if (g->data_id_advance)
    nr_pusch_passive_data_id_feed(true);
  {
    /* dataScramblingIdentityPUSCH, same argument as the DL: a CRC-OK TB under this n_ID is proof. */
    static _Atomic int s_ul_scr_confirmed;
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_ul_scr_confirmed, &expected, 1))
      LOG_A(PHY, "SENSING: DATA_SCRAMBLING_ID PUSCH CONFIRMED n_id=%d (assumed PCI %u) by TB CRC, rnti=0x%x\n",
            g->data_scrambling_id, (unsigned)fp->Nid_cell, g->rnti);
  }

  /* DM-RS identity: MOVED (review fix round 1, finding 2) to right after the delay-refined FEP,
   * alongside the PUSCH_DMRS CFR block above -- same reasoning that block's own comment already
   * gives: the DM-RS channel is fixed by (nid, slot, symbol) regardless of whether the payload
   * later decodes, so gating this on CRC was circular (a wrong id never lets the CRC pass, which is
   * exactly the situation this estimator exists to diagnose). See that insertion, above. */

  /* UPLINK DATA-AIDED CFR. Gated on o_ack == 0, which means this TB decoded on the FIRST attempt --
   * the no-UCI hypothesis -- so the codeword occupies every data RE and X is fully reconstructible.
   * A grant rescued by a UCI hypothesis has HARQ-ACK PUNCTURING the ULSCH at positions derived from
   * bits this receiver never decodes; reconstructing X there would be a guess, and a guess in the
   * numerator of Y/X is indistinguishable from a measurement downstream. Those grants keep
   * contributing through the DM-RS source, which does not depend on the payload at all. */
  if (out->uci_ack_re == 0) {
    nr_isac_pusch_data_aided_submit(ue, gnb, &pdu, g, hp->b,
                                    NR_PUSCH_PASSIVE_DA_TAG_BASE + (uint32_t)ctx,
                                    passive_ul_slow_time_idx(fp, frame, slot, abs_slot),
                                    (uint32_t)nant, slot);
  }
  return true;
}

/* The per-grant probe lives HERE, wrapping the decode, rather than in a caller: there are two
 * callers now (the in-line uplink hook and the deferred queue's consumer) and a probe that only one
 * of them carries goes silent exactly when the configuration changes -- which is when it is most
 * needed. Gated, and sampling nothing: at ~178 grants/s over a 95 s capture this is ~17 k lines,
 * well inside what this tree has found tolerable (a 55 k-line probe cost a run once). */
bool nr_pusch_passive_decode(PHY_VARS_NR_UE *ue,
                             int      ctx,
                             uint32_t frame,
                             uint8_t  slot,
                             const nr_pdcch_blind_ul_result_t *g,
                             int32_t  ta_offset_samples,
                             uint64_t abs_slot,
                             bool     cfr_only,
                             double   fo_hz,
                             nr_pusch_passive_out_t *out)
{
  const bool ok = nr_pusch_passive_decode_inner(ue, ctx, frame, slot, g, ta_offset_samples, abs_slot,
                                                cfr_only, fo_hz, out);

  if (s_diag_on() && g != NULL) {
    LOG_I(PHY,
          "SENSING: PUSCHDIAG %u.%u rnti=0x%x k2=%u prb=%u+%u sym=%u+%u mcs=%u/tbl%u rv=%u ta=%d "
          "tbs=%u G=%u Qm=%u snr=%.1f delay=%d seg=%d/%d status=%u %s\n",
          frame, (unsigned)slot, g->rnti, (unsigned)g->k2, (unsigned)g->start_rb, (unsigned)g->num_rb,
          (unsigned)g->start_symbol, (unsigned)g->num_symbols, (unsigned)g->mcs,
          (unsigned)g->mcs_table, (unsigned)g->rv, ta_offset_samples, out->tbs_bytes, out->G,
          (unsigned)out->qam_mod_order, out->snr_db, out->est_delay, out->segments_ok,
          out->n_segments, (unsigned)out->status, out->reject_reason ? out->reject_reason : "-");
  }
  return ok;
}

void nr_pusch_passive_stats_dump(void)
{
  if (ta_sweep_points() > 0) {
    char rep[1024];
    unsigned u = 0;
    for (int i = 0; i < ta_sweep_points() && u + 40 < sizeof(rep); i++) {
      const uint64_t t = atomic_load_explicit(&g_ta_try[i], memory_order_relaxed);
      const uint64_t k = atomic_load_explicit(&g_ta_ok[i], memory_order_relaxed);
      u += (unsigned)snprintf(rep + u, sizeof(rep) - u, "%d:%lu/%lu ",
                              g_ta_sweep_start + i * g_ta_sweep_step,
                              (unsigned long)k, (unsigned long)t);
    }
    LOG_I(PHY, "SENSING: TASWEEP (ta_samples:crc_ok/try) %s\n", rep);
  }
  const uint64_t t = atomic_load_explicit(&g_try, memory_order_relaxed);
  const uint64_t k = atomic_load_explicit(&g_crc_ok, memory_order_relaxed);
  /* Reported the way the downlink investigation had to learn to report it: crc_ok/try mixes two
   * independent things, decoder health and the empty-grant rate. `health` = ok/(ok+seg_fail) is the
   * one that says whether the receive chain works; crc_ok/try falls with the uplink's empty-TB
   * fraction even when nothing is wrong. Quoting only the latter misled the downlink work once. */
  const uint64_t sf = atomic_load_explicit(&g_seg_fail, memory_order_relaxed);
  const uint64_t zt = atomic_load_explicit(&g_zero_tb, memory_order_relaxed);
  LOG_I(PHY,
        "SENSING: pusch_passive[try=%lu crc_ok=%lu (%.1f%%) seg_fail=%lu zero_tb=%lu (%.1f%%) "
        "ta_refined=%lu uci[trials=%lu rescued=%lu] "
        "health=%.1f%% unsup=%lu setup_fail=%lu] ul_cfr[submits=%lu re=%lu]\n",
        (unsigned long)t, (unsigned long)k, t ? (100.0 * (double)k / (double)t) : 0.0,
        (unsigned long)sf, (unsigned long)zt, t ? (100.0 * (double)zt / (double)t) : 0.0,
        (unsigned long)atomic_load_explicit(&g_ta_refined, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_uci_trials, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_uci_rescued, memory_order_relaxed),
        (k + sf) ? (100.0 * (double)k / (double)(k + sf)) : 0.0,
        (unsigned long)atomic_load_explicit(&g_rej_unsup, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_rej_setup, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_cfr_submits, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&g_cfr_re, memory_order_relaxed));
  if (utim_enabled() && g_utim_n[UTIM_TOTAL] > 0) {
    char rep[512];
    unsigned u = 0;
    for (int k = 0; k < UTIM_N && u + 72 < sizeof(rep); k++) {
      u += (unsigned)snprintf(rep + u, sizeof(rep) - u, "%s[n=%lu mean=%.0fus max=%.0fus tot=%.2fs] ",
                              kUtimName[k], (unsigned long)g_utim_n[k],
                              g_utim_n[k] ? (double)g_utim_ns[k] / (double)g_utim_n[k] / 1000.0 : 0.0,
                              (double)g_utim_max[k] / 1000.0, (double)g_utim_ns[k] / 1e9);
    }
    LOG_I(PHY, "SENSING: UTIM %s\n", rep);
    LOG_I(PHY,
          "SENSING: UTIM grant_total_us_hist <50=%lu <100=%lu <200=%lu <400=%lu <800=%lu <1600=%lu "
          "<3200=%lu >=3200=%lu over_slot=%lu/%lu\n",
          (unsigned long)g_utim_hist[0], (unsigned long)g_utim_hist[1], (unsigned long)g_utim_hist[2],
          (unsigned long)g_utim_hist[3], (unsigned long)g_utim_hist[4], (unsigned long)g_utim_hist[5],
          (unsigned long)g_utim_hist[6], (unsigned long)g_utim_hist[7],
          (unsigned long)g_utim_over_slot, (unsigned long)g_utim_n[UTIM_TOTAL]);
  }
  {
    const uint64_t n = atomic_load_explicit(&g_ant_n, memory_order_relaxed);
    if (n > 0) {
      double p[PASSIVE_UL_MAX_ANT], mx = 0.0;
      for (int a = 0; a < PASSIVE_UL_MAX_ANT; a++) {
        p[a] = (double)atomic_load_explicit(&g_ant_pw[a], memory_order_relaxed) / (double)n;
        if (p[a] > mx) {
          mx = p[a];
        }
      }
      if (mx <= 0.0) {
        mx = 1.0;
      }
      /* 10log10: these are POWERS (|H|^2), unlike ANTPOW which accumulates an amplitude and needs
       * 20log10. Getting that wrong once made a -18 dB imbalance read as -9 dB on the DL side. */
      LOG_I(PHY,
            "SENSING: ULBRANCH pw=[%.0f %.0f %.0f %.0f] dB=[%.1f %.1f %.1f %.1f] n=%lu "
            "(PUSCH DM-RS |H|^2 per RX antenna)\n",
            p[0], p[1], p[2], p[3],
            10.0 * log10((p[0] > 0 ? p[0] : 1e-9) / mx), 10.0 * log10((p[1] > 0 ? p[1] : 1e-9) / mx),
            10.0 * log10((p[2] > 0 ? p[2] : 1e-9) / mx), 10.0 * log10((p[3] > 0 ? p[3] : 1e-9) / mx),
            (unsigned long)n);
    }
  }
}
