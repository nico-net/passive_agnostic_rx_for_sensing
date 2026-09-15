#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
#include "nr_passive_sample_lifetime.h"
/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c
 * \brief Consumer pool for the deferred passive PDSCH decode. See the header for the measurements
 * that forced this design.
 *
 * TWO CHANGES over the original single-consumer SPSC version, both forced by measurement at
 * ~1500 offered grants/s (2026-08-24):
 *
 * 1. N CONSUMERS. One consumer sustained 789 decodes/s against ~1570 offered, so 80 % of jobs were
 *    dropped on a full ring (`dropped[full=51647]` of 64835 queued). The PHY receive thread cannot
 *    be parallelised -- it is a sequential read-FEP-decode loop -- but the PDSCH decode behind it
 *    can be, and this box has eight idle cores (the thread pool holds 0,1,4,5,6,7 of 14).
 *
 * 2. DROP-OLDEST, not drop-newest. This is the change that matters for correctness, not throughput.
 *    Under overload a FIFO hands the consumer the OLDEST job -- which is precisely the one whose
 *    raw samples in ue->common_vars.rxdata are most likely to have been overwritten already. The
 *    measured consequence was decoded=58384 with crc_ok=0: the queue was working hard and producing
 *    nothing but stale decodes. For sensing, completeness is worthless and freshness is everything
 *    (a dropped grant costs one CFR row; a stale one costs a wrong row), so the producer now evicts
 *    the oldest entry to make room for the newest.
 *
 * Locking: a plain mutex, not the previous lock-free SPSC indices. The critical section is a ~600 B
 * struct copy; the work outside it is ~775 us per job. Contention is therefore negligible, and a
 * lock-free MULTI-consumer ring with eviction is far easier to get subtly wrong than to make fast.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_xoverhead.h"   // reject-only xOverhead elimination by TB CRC
#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h"  // blind DM-RS scrambling-identity estimate
#include "PHY/NR_REFSIG/dmrs_nr.h"                     // get_num_dmrs_re_per_rb
#include "common/utils/nr/nr_common.h"                // get_num_dmrs
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h" // Technique D scoring

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"
#include "common/utils/system.h" // threadCreate
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_data_aided.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "PHY/MODULATION/modulation_UE.h"

/* The RF producer's own position, published in executables/nr-ue.c immediately BEFORE nrue_ru_read()
 * fills that slot's region of rxdata. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

static nr_pdsch_passive_job_t g_ring[NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH];
static int  g_depth = 0;
static int  g_head  = 0; // next write slot
static int  g_tail  = 0; // next read slot
static int  g_count = 0; // entries currently held

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_slot_groups   = 0; ///< dequeues that took >1 grant of one slot
#define NR_PDSCH_PASSIVE_SLOT_GROUP_MAX 8
void nr_pdsch_passive_set_slot_share(int on, int rb_lo, int rb_n);
static _Atomic uint64_t g_decoded       = 0;
static _Atomic uint64_t g_crc_ok        = 0;
/* DL DM-RS identity estimate: one process-wide accumulator shared by every consumer, so evidence
 * from all of them adds. The 1024-candidate sweep runs under this lock on the consumer that
 * happens to hold it -- off the RT thread by construction, since this file IS the consumer. */
static nr_dmrs_id_state_t g_dl_dmrs_id;
static bool g_dl_dmrs_id_init;
static pthread_mutex_t g_dl_dmrs_id_lock = PTHREAD_MUTEX_INITIALIZER;
const nr_dmrs_id_state_t *nr_pdsch_passive_dl_dmrs_id(void) { return &g_dl_dmrs_id; }
static pthread_mutex_t g_dl_rank_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_rank_n[4], g_rank_low[4], g_rank_total; static double g_rank_sum[4];
static uint64_t g_cdm_empty[4]; static double g_cdm_sum[4];

/* ---- RANK / CDM-GROUP VERDICT  (agnosticity #4) ------------------------------------------------
 * Rank is NOT assumed here: cw->Nl comes from the DM-RS port count carried in the blindly decoded
 * DCI 1_1 antenna-ports field, and the decoder separates up to nb_antennas_rx layers. So the layer
 * count is DERIVED. What was missing is a stated verdict that cross-checks that derivation against
 * an INDEPENDENT physical observable, which is what the probes below already measure:
 *   - port-pair coherence ~1 => the DM-RS pair carries one symbol => ONE port; collapsed (<0.5) =>
 *     at least TWO ports are active. Honest bound: a PAIR test says "more than one", never how many,
 *     so it corroborates Nl>1 but cannot by itself distinguish 2 from 4. Nl does that; this checks it.
 *   - other-comb energy ratio ~1 => that comb carries PDSCH => numDmrsCdmGrpsNoData = 1; ~0 => 2.
 * Counted over every ACCEPTED grant, not CRC-OK ones: Nl is read out of the DCI, and the DCI's own
 * RNTI-masked CRC is what proves the grant belongs to this cell -- the TB CRC says nothing about how
 * many layers the scheduler declared. Gating this on TB CRC made the verdict unable to speak in
 * exactly the case it is most needed (rank high, decode failing), which is a defect, not caution. */
static _Atomic uint64_t g_nl_ok[8];
static _Atomic int      g_rank_verdict_logged;
#define RANK_VERDICT_MIN_OK 200
void nr_pdsch_passive_rank_verdict(void)
{
  if (atomic_load_explicit(&g_rank_verdict_logged, memory_order_relaxed))
    return;
  uint64_t tot = 0, best_n = 0; int best = 0;
  for (int i = 1; i < 8; i++) {
    const uint64_t v = atomic_load_explicit(&g_nl_ok[i], memory_order_relaxed);
    tot += v;
    if (v > best_n) { best_n = v; best = i; }
  }
  if (tot < RANK_VERDICT_MIN_OK)
    return;
  /* Independent corroboration from the coherence probe, CRC-OK buckets only (k=1 small, k=3 big). */
  double coh_low_frac = -1.0; uint64_t coh_n = 0;
  if (pthread_mutex_trylock(&g_dl_rank_lock) == 0) {
    /* CRC-OK buckets are the clean ones, but they are empty precisely when the decode is failing --
     * fall back to all buckets so the probe can still corroborate. The coherence test reads the DM-RS
     * directly and does not depend on the TB decoding. */
    coh_n = g_rank_n[1] + g_rank_n[3];
    if (coh_n) coh_low_frac = (double)(g_rank_low[1] + g_rank_low[3]) / (double)coh_n;
    else {
      coh_n = g_rank_n[0] + g_rank_n[2];
      if (coh_n) coh_low_frac = (double)(g_rank_low[0] + g_rank_low[2]) / (double)coh_n;
    }
    pthread_mutex_unlock(&g_dl_rank_lock);
  }
  const char *agree = "unresolved";
  if (coh_low_frac >= 0) {
    const bool probe_multi = coh_low_frac >= 0.5;
    agree = (probe_multi == (best > 1)) ? "CORROBORATED" : "CONTRADICTED";
  }
  atomic_store_explicit(&g_rank_verdict_logged, 1, memory_order_relaxed);
  LOG_A(PHY, "SENSING: RANK IDENTIFIED from DCI DM-RS ports: modal_layers=%d over %llu accepted grants "
             "hist[1=%llu 2=%llu 3=%llu 4=%llu] | probe: pair-coherence low(<0.5)=%.0f%% of %llu => %s "
             "(pair test proves >1 port, not how many; Nl gives the count)\n",
        best, (unsigned long long)tot,
        (unsigned long long)atomic_load_explicit(&g_nl_ok[1], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_nl_ok[2], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_nl_ok[3], memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_nl_ok[4], memory_order_relaxed),
        coh_low_frac >= 0 ? coh_low_frac * 100.0 : -1.0, (unsigned long long)coh_n, agree);
}
/* Per-RNTI DL decode census: "DL converged" is a per-UE statement, and a cell-wide CRC rate can hide
 * one UE decoding at 80 % and the other at 0 %. Indexed by C-RNTI; printed with the PDSCHQ census. */
static _Atomic uint32_t g_rnti_dec[65536], g_rnti_ok[65536];
void nr_pdsch_passive_queue_rnti_census(char *buf, size_t n)
{
  size_t off = 0; int shown = 0;
  for (int r = 1; r < 65536 && off + 40 < n && shown < 6; ++r) {
    const uint32_t d = atomic_load_explicit(&g_rnti_dec[r], memory_order_relaxed);
    if (d < 50) continue;
    const uint32_t ok = atomic_load_explicit(&g_rnti_ok[r], memory_order_relaxed);
    off += snprintf(buf + off, n - off, " 0x%x:%u/%u(%.0f%%)", r, ok, d, 100.0 * ok / d);
    ++shown;
  }
  if (!shown) snprintf(buf, n, " (none>=50)");
}
static _Atomic uint64_t g_dropped_full  = 0; // evicted oldest to admit a newer job
static _Atomic uint64_t g_dropped_narrow = 0; // budget: narrow grant refused while the ring was nearly full
static _Atomic uint64_t g_dropped_stale = 0;
static _Atomic uint64_t g_max_lag       = 0;

static _Atomic int g_running   = 0;
static _Atomic int g_stop      = 0;
static int         g_nthreads  = 0;
static pthread_t   g_threads[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static PHY_VARS_NR_UE *g_ue = NULL;

/* One frequency-domain slot buffer PER CONSUMER: they decode concurrently, and
 * nr_pdsch_passive_decode() both fills this and hands the same samples to the data-aided submit.
 * ~917 kB each at 273 PRB x 4 antennas. Heap, not thread-local storage -- the AoA work in
 * PASSIVE_RX_ONLY_HANDOVER.md records a shifted __thread layout producing an AVX alignment fault,
 * and this is far larger than the buffer that did it. */
static c16_t *g_rxdataF[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];

typedef struct {
  int idx;
} consumer_arg_t;
static consumer_arg_t g_args[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];

static void *nr_pdsch_passive_queue_thread(void *arg)
{
  const int idx = ((consumer_arg_t *)arg)->idx;
  PHY_VARS_NR_UE *ue        = g_ue;
  NR_DL_FRAME_PARMS *fp     = &ue->frame_parms;
  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  const long slots_per_frame = fp->slots_per_frame;

  LOG_I(PHY, "SENSING: passive PDSCH decode consumer %d started\n", idx);

  while (1) {
    nr_pdsch_passive_job_t job;

    pthread_mutex_lock(&g_lock);
    while (g_count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) == 0) {
      /* Timed wait so a stop cannot be missed if it lands between the predicate and the wait. */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000; // 20 ms
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }
    if (g_count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) != 0) {
      pthread_mutex_unlock(&g_lock);
      break;
    }
    job    = g_ring[g_tail];
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    /* SLOT GROUP: also take every queued job of the SAME slot (the producer enqueues a slot's
     * grants contiguously), so this thread does that slot's FEP and channel estimate once. */
    nr_pdsch_passive_job_t more[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX];
    int n_more = 0;
    while (g_count > 0 && n_more < NR_PDSCH_PASSIVE_SLOT_GROUP_MAX
           && g_ring[g_tail].absolute_slot == job.absolute_slot) {
      more[n_more++] = g_ring[g_tail];
      g_tail = (g_tail + 1) % g_depth;
      g_count--;
    }
    pthread_mutex_unlock(&g_lock);
    /* Union of RBs over the group members whose DM-RS configuration matches the head job's. */
    int rb_lo = job.freq_alloc.first_rb, rb_hi = job.freq_alloc.first_rb + job.freq_alloc.num_rbs;
    for (int k = 0; k < n_more; k++) {
      const fapi_nr_dl_config_dlsch_pdu_rel15_t *a = &job.dlsch_pdu, *b = &more[k].dlsch_pdu;
      if (a->dlDmrsSymbPos != b->dlDmrsSymbPos || a->dmrsConfigType != b->dmrsConfigType
          || a->nscid != b->nscid || a->dmrs_ports != b->dmrs_ports || a->n_dmrs_cdm_groups != b->n_dmrs_cdm_groups
          || a->dlDmrsScramblingId != b->dlDmrsScramblingId)
        continue;
      const int lo = more[k].freq_alloc.first_rb, hi = lo + more[k].freq_alloc.num_rbs;
      if (lo < rb_lo) rb_lo = lo;
      if (hi > rb_hi) rb_hi = hi;
    }
    /* Wide first: the data-aided CFR rows come from what decodes, and a wide grant's row is
     * worth more to the sensing grid than a narrow one's if the samples go stale mid-group. */
    if (n_more > 0) {
      nr_pdsch_passive_job_t all[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
      all[0] = job;
      for (int k = 0; k < n_more; k++) all[k + 1] = more[k];
      for (int i = 1; i <= n_more; i++) {
        nr_pdsch_passive_job_t t = all[i];
        int j = i - 1;
        while (j >= 0 && all[j].freq_alloc.num_rbs < t.freq_alloc.num_rbs) { all[j + 1] = all[j]; j--; }
        all[j + 1] = t;
      }
      job = all[0];
      for (int k = 0; k < n_more; k++) more[k] = all[k + 1];
    }
    nr_pdsch_passive_set_slot_share(n_more > 0, rb_lo, rb_hi - rb_lo);
    if (n_more > 0) atomic_fetch_add_explicit(&g_slot_groups, 1, memory_order_relaxed);

    /* ---- STALENESS CHECK. A job's raw IQ lives in rxdata only until the producer reaches the SAME
     * slot index one frame later, so decoding after that reads the NEXT frame's samples: the CRC
     * fails and it is indistinguishable from a weak channel. Count it as a drop, never "decode
     * anyway and hope". With drop-oldest admission this should now be rare -- if dropped_stale stays
     * high, the consumer pool is still too small for the offered rate. ---- */
    const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    if (lag > (long)atomic_load_explicit(&g_max_lag, memory_order_relaxed)) {
      atomic_store_explicit(&g_max_lag, (uint64_t)(lag > 0 ? lag : 0), memory_order_relaxed);
    }
    if (!nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame)) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    for (int gi = -1; gi < n_more; gi++) {
    if (gi >= 0) job = more[gi];
    /* Only these three proc fields are read downstream -- verified by inspecting every proc->
     * reference in nr_dl_channel_estimation.c, nr_dlsch_demodulation.c and nr_pdsch_data_aided.c. */
    UE_nr_rxtx_proc_t proc = {0};
    proc.frame_rx   = job.frame_rx;
    proc.nr_slot_rx = job.nr_slot_rx;
    proc.gNB_id     = job.gNB_id;

    c16_t (*rxdataF)[rxdataF_sz] = (c16_t (*)[rxdataF_sz])g_rxdataF[idx];

    /* Replay the offset captured with these samples (see nr_slot_fep_fo_override_hz). */
    const double saved_fo = nr_slot_fep_fo_override_hz;
    nr_slot_fep_fo_override_hz = job.fo_hz;
    job.grant.check_sample_lifetime = true;
    job.grant.source_absolute_slot = job.absolute_slot;

    nr_pdsch_passive_decode_result_t dec;
    const nr_pdsch_passive_decode_status_t st =
        nr_pdsch_passive_decode(ue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, rxdataF, &dec);
    nr_passive_replay_dl(&job, &dec);
    nr_slot_fep_fo_override_hz = saved_fo;
    if (st == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED && !nr_passive_samples_valid(
            atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
            job.absolute_slot, slots_per_frame))
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);

    if (st != NR_PDSCH_PASSIVE_DECODE_ERROR && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
      atomic_fetch_add_explicit(&g_decoded, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_rnti_dec[job.rnti], 1, memory_order_relaxed);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) atomic_fetch_add_explicit(&g_rnti_ok[job.rnti], 1, memory_order_relaxed);
      {
        /* Two dedicated-parameter checks that need only what is in hand here, on the consumer.
         * xOverhead: every CRC-OK decode refutes each alternative whose TBS differs (reject-only,
         * zero extra trials). DM-RS identity: accumulated on CRC-OK decodes only, so a noise-RNTI
         * false accept (garbage allocation) cannot pollute the statistic. */
        const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu = &job.dlsch_pdu;
        const bool crc = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
        /* dataScramblingIdentityPDSCH is dedicated and assumed = PCI. A CRC-OK transport block IS the
         * proof: c_init = RNTI*2^15 + n_ID, so any other n_ID descrambles wrongly and LDPC fails.
         * State the verdict once instead of leaving it an assumption. */
        static bool s_dl_scr_confirmed;
        if (crc && !s_dl_scr_confirmed) {
          s_dl_scr_confirmed = true;
          LOG_A(PHY, "SENSING: DATA_SCRAMBLING_ID PDSCH CONFIRMED n_id=%u (assumed %u) by TB CRC, rnti=0x%x\n",
                (unsigned)pdu->dlDataScramblingId, (unsigned)ue->frame_parms.Nid_cell, job.rnti);
        }
        const uint8_t nb_re_dmrs = get_num_dmrs_re_per_rb(pdu->dmrsConfigType, pdu->n_dmrs_cdm_groups);
        const uint16_t dmrs_len = get_num_dmrs(pdu->dlDmrsSymbPos);
        nr_pdsch_xoverhead_observe(dec.cw.qamModOrder, dec.cw.targetCodeRate, job.freq_alloc.num_rbs,
                                   (uint16_t)pdu->number_symbols, (uint16_t)(nb_re_dmrs * dmrs_len),
                                   job.grant.nb_rb_oh, job.grant.tb_scaling, dec.cw.Nl, crc);
        if (dec.cw.Nl > 0 && dec.cw.Nl < 8)
          atomic_fetch_add_explicit(&g_nl_ok[dec.cw.Nl], 1, memory_order_relaxed);
        nr_pdsch_passive_rank_verdict();
        /* RANK PROBE (OTA 2026-09-12: full-band grants 0/10000 CRC, short grants 8/9, gNB has 4 DL
         * antennas and the decoder assumes one layer). Per-grant even/odd DM-RS pair coherence under
         * the assumed identity: ~1 single-layer, collapsed two-layer. Censused by size and CRC. */
        if (pdu->dmrsConfigType == 0 && pdu->dlDmrsSymbPos && pthread_mutex_trylock(&g_dl_rank_lock) == 0) {
          const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
          const int sym = __builtin_ctz((unsigned)pdu->dlDmrsSymbPos);
          const int rb_offset = job.freq_alloc.first_rb + (pdu->refPoint ? 0 : pdu->BWPStart);
          const int start_sc  = fp->first_carrier_offset + (pdu->BWPStart + job.freq_alloc.first_rb) * 12;
          const double coh = nr_dmrs_port_pair_coherence(&rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                                                         start_sc, rb_offset, job.freq_alloc.num_rbs, fp->N_RB_DL,
                                                         fp->symbols_per_slot, job.nr_slot_rx, sym, pdu->nscid,
                                                         pdu->dlDmrsScramblingId, fp->Ncp == NR_NORMAL);
          /* CDM-GROUP PROBE. In the DM-RS symbol the other comb (delta = 1: subcarriers 4n+1, 4n+3)
           * carries PDSCH data when numDmrsCdmGrpsNoData = 1 and is EMPTY when it is 2. The ratio of
           * its mean energy to the DM-RS comb's is ~1 in the first case and ~0 in the second. The
           * receiver derives the group count from the antenna-ports code point; this measures it. */
          double e_dmrs = 0, e_other = 0;
          {
            const c16_t *row = &rxdataF[0][sym * fp->ofdm_symbol_size];
            int re = ((start_sc % fp->ofdm_symbol_size) + fp->ofdm_symbol_size) % fp->ofdm_symbol_size;
            for (int m = 0; m < 12 * job.freq_alloc.num_rbs; ++m) {
              const double e = (double)row[re].r * row[re].r + (double)row[re].i * row[re].i;
              if (m & 1) e_other += e; else e_dmrs += e;
              re = (re + 1) % fp->ofdm_symbol_size;
            }
          }
          const double cdm_ratio = e_dmrs > 0 ? e_other / e_dmrs : -1.0;
          if (coh >= 0) {
            const int big = job.freq_alloc.num_rbs >= 100, k = big * 2 + crc; // 0 small/fail 1 small/ok 2 big/fail 3 big/ok
            g_rank_n[k]++; g_rank_sum[k] += coh; if (coh < 0.5) g_rank_low[k]++;
            g_cdm_sum[k] += cdm_ratio; if (cdm_ratio < 0.25) g_cdm_empty[k]++;
            if (++g_rank_total % 500 == 0)
              LOG_I(PHY, "SENSING: DL_RANK_PROBE n=%lu | small/crcfail n=%lu mean_coh=%.2f low=%lu | small/crcok n=%lu mean=%.2f low=%lu"
                         " | big/crcfail n=%lu mean=%.2f low=%lu | big/crcok n=%lu mean=%.2f low=%lu  (low = coherence<0.5 => 2 ports)\n",
                    (unsigned long)g_rank_total,
                    (unsigned long)g_rank_n[0], g_rank_n[0] ? g_rank_sum[0] / g_rank_n[0] : 0.0, (unsigned long)g_rank_low[0],
                    (unsigned long)g_rank_n[1], g_rank_n[1] ? g_rank_sum[1] / g_rank_n[1] : 0.0, (unsigned long)g_rank_low[1],
                    (unsigned long)g_rank_n[2], g_rank_n[2] ? g_rank_sum[2] / g_rank_n[2] : 0.0, (unsigned long)g_rank_low[2],
                    (unsigned long)g_rank_n[3], g_rank_n[3] ? g_rank_sum[3] / g_rank_n[3] : 0.0, (unsigned long)g_rank_low[3]);
            if (g_rank_total % 500 == 0)
              LOG_I(PHY, "SENSING: DL_CDM_PROBE other-comb/dmrs-comb energy ratio (1 = data there = 1 CDM group, 0 = empty = 2 groups): "
                         "small/crcfail mean=%.2f empty=%lu/%lu | small/crcok mean=%.2f empty=%lu/%lu | "
                         "big/crcfail mean=%.2f empty=%lu/%lu | big/crcok mean=%.2f empty=%lu/%lu | receiver assumes cdm=%u\n",
                    g_rank_n[0] ? g_cdm_sum[0] / g_rank_n[0] : 0.0, (unsigned long)g_cdm_empty[0], (unsigned long)g_rank_n[0],
                    g_rank_n[1] ? g_cdm_sum[1] / g_rank_n[1] : 0.0, (unsigned long)g_cdm_empty[1], (unsigned long)g_rank_n[1],
                    g_rank_n[2] ? g_cdm_sum[2] / g_rank_n[2] : 0.0, (unsigned long)g_cdm_empty[2], (unsigned long)g_rank_n[2],
                    g_rank_n[3] ? g_cdm_sum[3] / g_rank_n[3] : 0.0, (unsigned long)g_cdm_empty[3], (unsigned long)g_rank_n[3],
                    (unsigned)pdu->n_dmrs_cdm_groups);
          }
          pthread_mutex_unlock(&g_dl_rank_lock);
        }
        if (crc && pdu->dmrsConfigType == 0 && pdu->dlDmrsSymbPos && pthread_mutex_trylock(&g_dl_dmrs_id_lock) == 0) {
          if (!g_dl_dmrs_id_init) { nr_dmrs_id_init(&g_dl_dmrs_id, "PDSCH", pdu->dlDmrsScramblingId); g_dl_dmrs_id_init = true; }
          if (!g_dl_dmrs_id.decided) {
            const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
            const int sym = __builtin_ctz((unsigned)pdu->dlDmrsSymbPos);
            /* Same two quantities the estimator itself derives (nr_dl_channel_estimation.c). */
            const int rb_offset = job.freq_alloc.first_rb + (pdu->refPoint ? 0 : pdu->BWPStart);
            const int start_sc  = fp->first_carrier_offset + (pdu->BWPStart + job.freq_alloc.first_rb) * 12;
            if (nr_dmrs_id_accumulate(&g_dl_dmrs_id, &rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                                      start_sc, rb_offset, job.freq_alloc.num_rbs, fp->N_RB_DL, fp->symbols_per_slot,
                                      job.nr_slot_rx, sym, pdu->nscid, fp->Ncp == NR_NORMAL))
              nr_dmrs_id_decide(&g_dl_dmrs_id, 16, 10.0);
          }
          pthread_mutex_unlock(&g_dl_dmrs_id_lock);
        }
      }
      /* Technique D scoring: the TB CRC is the only oracle that can tell a right payload
       * interpretation from a wrong one, and this is the one place it is known. */
      nr_pdsch_cfg_hypothesis_t winner;
      if (nr_pdsch_config_sweep_feedback(&job.sweep_ticket, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, &winner))
        LOG_A(PHY, "SENSING: Technique D CONVERGED rnti=0x%x tda=%u S=%u L=%u mask=0x%x table=%u\n",
              job.sweep_ticket.rnti, job.sweep_ticket.tda_index, winner.tda_start, winner.tda_length,
              winner.dmrs_mask, winner.mcs_table);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) {
        atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
        if (job.want_data) {
          /* Publish THIS job's monotonic slot so the CPI grid indexes it correctly. Without this the
           * submit derives the index from proc->frame_rx, which wraps at 1024 -- harmless in order,
           * fatal once several consumers submit concurrently across a wrap. */
          nr_isac_abs_slot_override = (uint64_t)job.absolute_slot;
          nr_isac_pdsch_data_aided_submit(ue, &proc, &dec.cw, &job.dlsch_pdu, &job.freq_alloc, job.rnti,
                                          dec.tb, job.harq_pid_tag, rxdataF, (double)dec.nvar);
          nr_isac_abs_slot_override = 0;
        }
      }
    }
    } /* slot group */
  }

  LOG_I(PHY, "SENSING: passive PDSCH decode consumer %d exiting\n", idx);
  return NULL;
}

static void report_sweep(const nr_pdsch_sweep_report_t *r)
{
  if (r->invalidated) {
    LOG_W(PHY,"PDSCH_RELEARN reason=CRC_EVIDENCE_LOSS config=%lx rnti=0x%x tda=%u "
              "generation=%lu->%lu reacquisitions=%lu consecutive_failures=%lu "
              "reference_crc_lower=%.6f action=LOCAL_HYPOTHESIS_SEARCH state=UNRESOLVED\n",
          (unsigned long)r->configuration,r->rnti,r->tda,
          (unsigned long)r->previous_generation,(unsigned long)r->generation,
          (unsigned long)r->reacquisitions,(unsigned long)r->failure_streak,r->reference_crc_lower);
    return;
  }
  if (r->operational)

    LOG_I(PHY,"Technique D operational rnti=0x%x config=%lx tda=%u crc=%lu/%lu\n",
          r->rnti,(unsigned long)r->configuration,r->tda,(unsigned long)r->passes,(unsigned long)r->trials);
  else
    LOG_I(PHY,"Technique D evidence rnti=0x%x config=%lx tda=%u outcomes=%lu min_trials=%u "
              "best=%lu/%lu S=%u L=%u mask=0x%x table=%u winner=%d\n",
          r->rnti,(unsigned long)r->configuration,r->tda,(unsigned long)r->outcomes,r->minimum,
          (unsigned long)r->passes,(unsigned long)r->trials,r->hypothesis.tda_start,
          r->hypothesis.tda_length,r->hypothesis.dmrs_mask,r->hypothesis.mcs_table,r->winner);
}

bool nr_pdsch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity)
{
  if (atomic_load_explicit(&g_running, memory_order_acquire)) {
    return true;
  }
  if (ue == NULL) {
    return false;
  }
  /* The --cont-fo-comp refusal that stood here is gone: the producer now samples the
   * frequency offset on the receive thread and the consumer replays it through
   * nr_slot_fep_fo_override_hz, so the deferred FEP de-rotates with the offset that
   * belongs to its own samples. Falling back in-line was never benign -- it overran the
   * slot deadline and cost PBCH lock under load. */

  if (n_consumers < 1) n_consumers = 1;
  if (n_consumers > NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS;
  if (depth < 2) depth = 2;
  if (depth > NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH) depth = NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH;

  const uint32_t rxdataF_sz = ue->frame_parms.samples_per_slot_wCP;
  for (int i = 0; i < n_consumers; i++) {
    g_rxdataF[i] = (c16_t *)malloc16_clear((size_t)ue->frame_parms.nb_antennas_rx * rxdataF_sz * sizeof(c16_t));
    if (g_rxdataF[i] == NULL) {
      LOG_E(PHY, "SENSING: passive PDSCH queue: rxdataF allocation failed for consumer %d\n", i);
      for (int j = 0; j < i; j++) { free(g_rxdataF[j]); g_rxdataF[j] = NULL; }
      return false;
    }
  }

  nr_pdsch_config_sweep_set_reporter(report_sweep);
  g_ue       = ue;
  g_depth    = depth;
  g_head     = 0;
  g_tail     = 0;
  g_count    = 0;
  g_nthreads = n_consumers;
  atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
  atomic_store_explicit(&g_running, 1, memory_order_release);

  for (int i = 0; i < n_consumers; i++) {
    g_args[i].idx = i;
    char name[32];
    snprintf(name, sizeof(name), "passivePdsch%d", i);
    /* Priority deliberately BELOW the RT receive thread's 97: if a consumer ever competes with the
     * receive path it must lose -- losing here costs one deferred decode, losing there costs time
     * sync, which is the entire problem this module exists to fix. One core each from base_core;
     * <0 leaves them unpinned. */
    threadCreate(&g_threads[i], nr_pdsch_passive_queue_thread, &g_args[i], name,
                 (affinity >= 0) ? (affinity + i) : -1, 50);
  }
  LOG_I(PHY, "SENSING: passive PDSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1);
  return true;
}

bool nr_pdsch_passive_queue_running(void)
{
  return atomic_load_explicit(&g_running, memory_order_acquire) != 0;
}

static bool enqueue_one(const nr_pdsch_passive_job_t *job);

/* SLOT BATCHING (2026-09-15). The producer hands over one grant at a time and the consumers drain
 * the ring faster than a slot's grants arrive, so the consumer-side slot group never formed
 * (rfsim, 3 UEs: SLOTSHARE 0/15689). Jobs are held back on the producer thread until the slot
 * changes (or the occasion ends), then pushed contiguously under ONE lock and ONE wake-up, so the
 * consumer that takes the first grant of a slot finds its siblings right behind it. Costs one
 * slot of latency against a one-frame sample lifetime. */
static nr_pdsch_passive_job_t g_pending[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX];
static int g_n_pending = 0;
static _Atomic uint64_t g_batches = 0, g_batches_multi = 0; ///< producer flushes, and those with >1 grant

void nr_pdsch_passive_queue_flush(void)
{
  if (g_n_pending == 0) return;
  atomic_fetch_add_explicit(&g_batches, 1, memory_order_relaxed);
  if (g_n_pending > 1) atomic_fetch_add_explicit(&g_batches_multi, 1, memory_order_relaxed);
  {
    const uint64_t nb = atomic_load_explicit(&g_batches, memory_order_relaxed);
    if ((nb % 5000) == 0)
      LOG_I(PHY, "SENSING: PDSCHQ batches=%lu multi-grant=%lu groups_taken=%lu dropped_narrow=%lu\n",
            (unsigned long)nb, (unsigned long)atomic_load_explicit(&g_batches_multi, memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&g_slot_groups, memory_order_relaxed),
            (unsigned long)atomic_load_explicit(&g_dropped_narrow, memory_order_relaxed));
  }
  pthread_mutex_lock(&g_lock); /* ONE lock for the whole slot: no consumer can take the head between pushes */
  for (int i = 0; i < g_n_pending; i++)
    enqueue_one(&g_pending[i]);
  g_n_pending = 0;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_lock);
}

bool nr_pdsch_passive_queue_enqueue(const nr_pdsch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  if (g_n_pending > 0 && (g_pending[0].absolute_slot != job->absolute_slot
                          || g_n_pending == NR_PDSCH_PASSIVE_SLOT_GROUP_MAX))
    nr_pdsch_passive_queue_flush();
  g_pending[g_n_pending++] = *job;
  return true;
}

static bool enqueue_one(const nr_pdsch_passive_job_t *job) /* g_lock held by the caller */
{
  /* DECODE BUDGET BY SENSING VALUE (2026-09-15). Under overload the sensing pipeline wants the
   * slot's CFR coverage, so a grant narrower than 1/8 of the band is refused while the ring is
   * >= 90 % full rather than evicting an older, wider one. Narrow grants of a shared slot still
   * get in whenever there is room, so multi-UE slots are not hollowed out below the threshold.
   * ponytail: fixed 1/8 and 90 %; make them coverage-driven if drops stay high. */
  if (g_count * 10 >= g_depth * 9 && job->freq_alloc.num_rbs * 8 < g_ue->frame_parms.N_RB_DL) {
    atomic_fetch_add_explicit(&g_dropped_narrow, 1, memory_order_relaxed);
    return false;
  }
  if (g_count == g_depth) {
    /* DROP-OLDEST, PER-RNTI FAIR (2026-09-15). The evicted entry is the oldest job of the RNTI
     * holding the MOST ring entries, so one UE's burst cannot starve the others' contexts; with a
     * single RNTI this is exactly the old drop-oldest (the head), whose rationale stands: its
     * rxdata samples are the closest to being overwritten. Ring depth <= 64, so the scan is cheap
     * and only runs under overload. */
    uint16_t top_rnti = g_ring[g_tail].rnti; int top_n = 0;
    for (int i = 0; i < g_count; i++) {
      const uint16_t r = g_ring[(g_tail + i) % g_depth].rnti;
      int n = 0;
      for (int j = 0; j < g_count; j++) n += g_ring[(g_tail + j) % g_depth].rnti == r;
      if (n > top_n) { top_n = n; top_rnti = r; }
    }
    int victim = 0;
    while (victim < g_count && g_ring[(g_tail + victim) % g_depth].rnti != top_rnti) victim++;
    if (victim >= g_count) victim = 0;
    for (int i = victim; i > 0; i--) /* shift the entries older than the victim up by one */
      g_ring[(g_tail + i) % g_depth] = g_ring[(g_tail + i - 1) % g_depth];
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
  }
  g_ring[g_head] = *job;
  g_head         = (g_head + 1) % g_depth;
  g_count++;
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
  return true;
}

void nr_pdsch_passive_queue_get_stats(nr_pdsch_passive_queue_stats_t *out)
{
  if (out == NULL) {
    return;
  }
  out->queued        = atomic_load_explicit(&g_queued, memory_order_relaxed);
  out->decoded       = atomic_load_explicit(&g_decoded, memory_order_relaxed);
  out->crc_ok        = atomic_load_explicit(&g_crc_ok, memory_order_relaxed);
  out->dropped_full  = atomic_load_explicit(&g_dropped_full, memory_order_relaxed);
  out->dropped_narrow = atomic_load_explicit(&g_dropped_narrow, memory_order_relaxed);
  out->dropped_stale = atomic_load_explicit(&g_dropped_stale, memory_order_relaxed);
  out->max_lag_slots = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
  out->slot_groups   = atomic_load_explicit(&g_slot_groups, memory_order_relaxed);
  out->batches       = atomic_load_explicit(&g_batches, memory_order_relaxed);
  out->batches_multi = atomic_load_explicit(&g_batches_multi, memory_order_relaxed);
}

void nr_pdsch_passive_queue_stop(void)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return;
  }
  atomic_store_explicit(&g_stop, 1, memory_order_release);
  pthread_mutex_lock(&g_lock);
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_lock);
  for (int i = 0; i < g_nthreads; i++) {
    pthread_join(g_threads[i], NULL);
    free(g_rxdataF[i]);
    g_rxdataF[i] = NULL;
  }
  atomic_store_explicit(&g_running, 0, memory_order_release);
}
