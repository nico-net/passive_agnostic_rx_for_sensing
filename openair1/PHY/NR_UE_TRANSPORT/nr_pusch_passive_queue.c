#include "PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_reconf_rules.h"
#include "common/utils/nr_core_plan.h"
#include "PHY/TOOLS/nr_stage_split.h"
extern uint64_t *nr_ss_gpu_ptr(void);
#include "nr_passive_sample_lifetime.h"
#include "nr_pdcch_ul_discovery.h"
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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.c
 * \brief Deferred passive PUSCH decode. See the header for the measurement that motivates it.
 *
 * Structurally a copy of nr_pdsch_passive_queue.c -- deliberately, so the two behave identically
 * under overload and a reader who knows one knows the other. Same drop-oldest admission, same
 * staleness policing against the RF producer's published position, same below-RT consumer priority.
 * The differences are stated in the header: a per-consumer DECODE CONTEXT rather than a per-consumer
 * buffer, and one extra slot of staleness margin for the timing-advance backward read.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pusch_passive_queue.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h" // nr_pdcch_dci01_fdra_feedback

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "common/utils/LOG/log.h"
#include "common/utils/system.h" // threadCreate
#include "common/utils/nr_core_pool.h"
extern uint64_t nr_passive_slot_arrival_ns(long abs_slot);
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_job_epoch.h"
#include "nr_pusch_passive_ul_ids.h"
#include "nr_ul_first_pusch.h"
#include "nr_pusch_passive_monitor_rt.h"
#include "nr_pusch_passive_queue_cursor.h"
#include "nr_pusch_solver.h"
#include "nr_pdcch_ul_config_disc.h"
#include "nr_pdcch_ul_layout_lock.h"
#include "nr_pusch_passive_decode.h"
#include "nr_ul_nid_bg.h" // nr_ul_nid_bg_wants_next
#include <math.h>
#include "common/utils/nr_cell.h"
#include "nr_ul_cell_prealloc.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h" // nr_get_Qm_ul

/* The RF producer's own position, published in executables/nr-ue.c immediately BEFORE nrue_ru_read()
 * fills that slot's region of rxdata. */
#include "PHY/NR_UE_TRANSPORT/nr_ue_cell_clock.h" /* per-cell slot clock (A8): replaces the extern of the former process global */

typedef nr_pusch_passive_job_t g_ring_cell_t[NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH];
NR_CELL_STATIC(g_ring_cell_t, g_ring);
#define g_ring NR_CELL_REF(g_ring)
NR_CELL_STATIC(nr_pusch_queue_cursor_t, g_cursor);
#define g_cursor NR_CELL_REF(g_cursor)
/* ULQ: layout probes and CRC-first trials go to their own ring (depth 8) and are dequeued only when no real grant waits; a probe can
 * no longer evict a real grant. Pipeline mode: 2 consumers / decode contexts on ISAC_PUSCH_CORES, real-grant ring depth 32. One shared
 * IQ snapshot per UL slot, copied by a capture thread right after booking (ISAC_UL_SNAP_CORE), so a real grant stays decodable while it
 * waits. UCI explore search and UL_RETRY alternates run only while no real grant waits. A job holding a shared snapshot is dropped at
 * dequeue by the same age rule as a job without one (nr_pusch_queue_snapshot_fresh: one frame minus 5 slots), so a GPU/CPU stall costs
 * the grants older than that instead of draining tens of ms of backlog (VL2: 60 ms). The age is also taken in wall time (the same
 * number of slot durations since the slot's samples arrived), since the producer's slot counter itself lags when the host stalls.
 * Real-grant stale drops are printed as PUSCHQ2 real_stale= and scored as UL drops. */
typedef nr_pusch_passive_job_t g_pring_cell_t[NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH];
NR_CELL_STATIC(g_pring_cell_t, g_pring);
#define g_pring NR_CELL_REF(g_pring)
NR_CELL_STATIC(nr_pusch_queue_cursor_t, g_pcursor);
#define g_pcursor NR_CELL_REF(g_pcursor)
#define ULQ_POOL_MAX (NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH + NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS + 2)
typedef struct {
  c16_t *iq[4];
  long abs;
  uint32_t origin, len;
} ulq_snap_t;
typedef ulq_snap_t g_pool_cell_t[ULQ_POOL_MAX];
NR_CELL_STATIC(g_pool_cell_t, g_pool);
#define g_pool NR_CELL_REF(g_pool)
/* stage B of the n_ID search: the capture thread also copies the slot AFTER each booked slot (hypotheses with k2 + 1 read it); same index as g_pool */
typedef ulq_snap_t g_pool2_cell_t[ULQ_POOL_MAX];
NR_CELL_STATIC(g_pool2_cell_t, g_pool2);
#define g_pool2 NR_CELL_REF(g_pool2)
NR_CELL_STATIC(int, g_snap_next);
#define g_snap_next NR_CELL_REF(g_snap_next)
typedef int g_pool_refs_cell_t[ULQ_POOL_MAX];
NR_CELL_STATIC(g_pool_refs_cell_t, g_pool_refs); /* under g_lock */
#define g_pool_refs NR_CELL_REF(g_pool_refs)
NR_CELL_STATIC(int, g_pool_n);
#define g_pool_n NR_CELL_REF(g_pool_n)
NR_CELL_STATIC(pthread_t, g_cap_thread);
#define g_cap_thread NR_CELL_REF(g_cap_thread)
NR_CELL_STATIC_INIT(pthread_cond_t, g_cap_cv, PTHREAD_COND_INITIALIZER);
#define g_cap_cv NR_CELL_REF(g_cap_cv)
NR_CELL_STATIC(_Atomic uint64_t, g_q_real);
#define g_q_real NR_CELL_REF(g_q_real)
NR_CELL_STATIC(_Atomic uint64_t, g_q_probe);
#define g_q_probe NR_CELL_REF(g_q_probe)
NR_CELL_STATIC(_Atomic uint64_t, g_dec_probe);
#define g_dec_probe NR_CELL_REF(g_dec_probe)
NR_CELL_STATIC(_Atomic uint64_t, g_full_probe);
#define g_full_probe NR_CELL_REF(g_full_probe)
NR_CELL_STATIC(_Atomic uint64_t, g_snap_ok);
#define g_snap_ok NR_CELL_REF(g_snap_ok)
NR_CELL_STATIC(_Atomic uint64_t, g_snap_fail);
#define g_snap_fail NR_CELL_REF(g_snap_fail)
NR_CELL_STATIC(_Atomic uint64_t, g_snap_nobuf);
#define g_snap_nobuf NR_CELL_REF(g_snap_nobuf)
NR_CELL_STATIC(_Atomic uint64_t, g_snap_used);
#define g_snap_used NR_CELL_REF(g_snap_used)
NR_CELL_STATIC(_Atomic uint64_t, g_rescue_skipped);
#define g_rescue_skipped NR_CELL_REF(g_rescue_skipped)
NR_CELL_STATIC(_Atomic uint64_t, g_stale_real);
#define g_stale_real NR_CELL_REF(g_stale_real)

NR_CELL_STATIC_INIT(pthread_mutex_t, g_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_lock NR_CELL_REF(g_lock)
static bool ulq_is_probe(const nr_pdcch_blind_ul_result_t *g);
NR_CELL_STATIC_INIT(pthread_mutex_t, g_lifecycle_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_lifecycle_lock NR_CELL_REF(g_lifecycle_lock)
NR_CELL_STATIC_INIT(pthread_cond_t, g_cv, PTHREAD_COND_INITIALIZER);
#define g_cv NR_CELL_REF(g_cv)

NR_CELL_STATIC_INIT(_Atomic uint64_t, g_queued, 0);
#define g_queued NR_CELL_REF(g_queued)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_decoded, 0);
#define g_decoded NR_CELL_REF(g_decoded)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_crc_ok, 0);
#define g_crc_ok NR_CELL_REF(g_crc_ok)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_dropped_full, 0);
#define g_dropped_full NR_CELL_REF(g_dropped_full)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_dropped_stale, 0);
#define g_dropped_stale NR_CELL_REF(g_dropped_stale)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_dropped_epoch, 0);
#define g_dropped_epoch NR_CELL_REF(g_dropped_epoch)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_dropped_unready, 0);
#define g_dropped_unready NR_CELL_REF(g_dropped_unready)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_dropped_snapshot, 0);
#define g_dropped_snapshot NR_CELL_REF(g_dropped_snapshot)
NR_CELL_STATIC_INIT(_Atomic uint64_t, g_max_lag, 0);
#define g_max_lag NR_CELL_REF(g_max_lag)

NR_CELL_STATIC_INIT(_Atomic int, g_running, 0);
#define g_running NR_CELL_REF(g_running)
NR_CELL_STATIC_INIT(_Atomic int, g_stop, 0);
#define g_stop NR_CELL_REF(g_stop)
NR_CELL_STATIC_INIT(int, g_nthreads, 0);
#define g_nthreads NR_CELL_REF(g_nthreads)
typedef pthread_t g_threads_cell_t[NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
NR_CELL_STATIC(g_threads_cell_t, g_threads);
#define g_threads NR_CELL_REF(g_threads)
NR_CELL_STATIC_INIT(PHY_VARS_NR_UE *, g_ue, NULL);
#define g_ue NR_CELL_REF(g_ue)

typedef struct {
  int idx;
  int cell; /* the consumer / capture thread binds itself to this cell (A6: the queue is per cell) */
} consumer_arg_t;
typedef consumer_arg_t g_args_cell_t[NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
NR_CELL_STATIC(g_args_cell_t, g_args);
#define g_args NR_CELL_REF(g_args)
NR_CELL_STATIC(consumer_arg_t, g_cap_arg);
#define g_cap_arg NR_CELL_REF(g_cap_arg)

static bool same_reading(const nr_pdcch_blind_ul_result_t *a, const nr_pdcch_blind_ul_result_t *b)
{
  return a->bwp_start == b->bwp_start && a->bwp_size == b->bwp_size &&
         a->start_rb == b->start_rb && a->num_rb == b->num_rb &&
         a->ra_type0 == b->ra_type0 && a->rbg_bitmap == b->rbg_bitmap &&
         a->tda_index == b->tda_index && a->k2 == b->k2 &&
         a->start_symbol == b->start_symbol && a->num_symbols == b->num_symbols &&
         a->mapping_type == b->mapping_type && a->frequency_hopping == b->frequency_hopping &&
         a->mcs == b->mcs && a->mcs_table == b->mcs_table && a->rv == b->rv && a->ndi == b->ndi &&
         a->ul_dmrs_symb_pos == b->ul_dmrs_symb_pos &&
         a->dmrs_config_type == b->dmrs_config_type &&
         a->n_dmrs_cdm_groups == b->n_dmrs_cdm_groups && a->dmrs_ports == b->dmrs_ports &&
         a->nscid == b->nscid && a->data_scrambling_id == b->data_scrambling_id &&
         a->ul_dmrs_scrambling_id == b->ul_dmrs_scrambling_id &&
         a->transform_precoding == b->transform_precoding;
}

/* U3 supplies all layouts of the same accepted payload. Group layouts that
 * read this DCI identically, as the DL solver does. Different k2 values need
 * different IQ snapshots and cannot be tested from this queue job. */
static int solver_readings(const nr_pusch_passive_job_t *job,
                           nr_pusch_solver_reading_t *rd, int cap)
{
  nr_pdcch_blind_ul_opts_t opts[64];
  int selected = -1;
  const int nopts=nr_pdcch_ul_config_disc_options(&job->grant,opts,64,&selected);
  int n=0;
  if (selected >= 0 && selected < 64)
    rd[n++] = (nr_pusch_solver_reading_t){.grant=job->grant,.layouts=UINT64_C(1)<<selected};
  else
    rd[n++] = (nr_pusch_solver_reading_t){.grant=job->grant,.layouts=UINT64_C(1)};
  for (int i=0; i<nopts; ++i) {
    nr_pdcch_blind_ul_result_t g={0};
    if (i==selected || !nr_pdcch_blind_extract_01(job->grant.raw_payload,job->grant.dci_length,
                    job->grant.rnti,&opts[i],&g) || !g.plausible || g.k2!=job->grant.k2 ||
        g.carrier_indicator || g.ul_sul_indicator) continue;
    g.data_scrambling_id=job->grant.data_scrambling_id;
    g.ul_dmrs_scrambling_id=job->grant.ul_dmrs_scrambling_id;
    bool grouped=false;
    for (int j=0;j<n;++j) if (same_reading(&rd[j].grant,&g)) {
      rd[j].layouts |= UINT64_C(1)<<i;
      grouped=true; break;
    }
    if (!grouped && n<cap)
      rd[n++]=(nr_pusch_solver_reading_t){.grant=g,.layouts=UINT64_C(1)<<i};
  }
  return n;
}

/* A ticketed grant the solver did not fully decode (it only probes one-CB QPSK grants of <= 48 PRB, e.g. never the 3-PRB MCS 17-27
 * 64/256QAM grants that are the only detectable PUSCH at the DGX position) also takes the ordinary full-decode path, probe class included. */

/* UL twin of the PDSCH DM-RS CDM / TDRA retries. The DCI antenna-ports code point fixes the number of CDM groups without data, but its
 * field width is a hypothesis (U3 width splits), so the group count can be wrong while everything else is right; likewise S=0/L=14 can
 * alias S=1/L=13. A CRC-failed full decode of an allocation of at most 12 PRB (the detectable class at this position) is retried with
 * each alternative, if the DM-RS SNR reaches the Shannon bound of the grant's MCS minus 3 dB, 10 log10(2^(Qm R) - 1) - 3: the alternates
 * change the data REs / symbol 0, never the DM-RS, so below that bound no alternate can pass CRC. Without the SNR gate every CRC-failed
 * small grant paid 3 decode calls (the UL p99 tail on replays). */
static bool ul_retry_snr_ok(const nr_pdcch_blind_ul_result_t *g, float snr_db)
{
  return snr_db >= nr_pusch_passive_shannon_db(g->mcs, g->mcs_table) - 3.0f;
}

/* ULST: true when the slot's samples arrived more than (slots_per_frame - 5) slot durations ago (the
 * nr_pusch_queue_snapshot_fresh budget in wall time); false when the arrival is unknown */
static bool ulq_wall_stale(long abs_slot, long slots_per_frame)
{
  extern uint64_t nr_passive_slot_arrival_ns(long abs_slot);
  const uint64_t arr = nr_passive_slot_arrival_ns(abs_slot);
  if (!arr || slots_per_frame <= 5) return false;
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  const uint64_t now = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
  return now > arr && now - arr > (uint64_t)(slots_per_frame - 5) * (10000000ull / (uint64_t)slots_per_frame);
}

bool nr_pusch_passive_pipeline_enabled(void) { return true; }

void nr_pusch_passive_queue_note_unready(void)
{
  atomic_fetch_add_explicit(&g_dropped_unready, 1, memory_order_relaxed);
}

static void *nr_pusch_passive_queue_thread(void *arg)
{
  /* The consumer index IS the decode-context index. That identity is what keeps two consumers off
   * one PHY_VARS_gNB, so it is not an incidental convenience -- do not renumber one without the
   * other. */
  const int idx = ((consumer_arg_t *)arg)->idx;
  nr_cell_bind(((consumer_arg_t *)arg)->cell);
  PHY_VARS_NR_UE *ue = g_ue;
  const long slots_per_frame = ue->frame_parms.slots_per_frame;
  struct timespec wall_start={0},cpu_start={0};
  if(nr_pusch_passive_pipeline_enabled()) {
    clock_gettime(CLOCK_MONOTONIC,&wall_start);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_start);
  }

  LOG_I(PHY, "SENSING: passive PUSCH decode consumer %d started\n", idx);

  int held = -1; /* ULQ shared snapshot this consumer holds a reference on */
  while (1) {
    nr_pusch_passive_job_t job;
    nr_pool_site_release(); /* core pool: a lease never outlives one iteration */
    nr_pool_site_enforce_tick();
    if (held >= 0) {
      pthread_mutex_lock(&g_lock);
      g_pool_refs[held]--;
      pthread_cond_signal(&g_cap_cv);
      pthread_mutex_unlock(&g_lock);
      held = -1;
    }
    pthread_mutex_lock(&g_lock);
    while (nr_pusch_queue_pick(&g_cursor, &g_pcursor) < 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) == 0) {
      /* Timed wait so a stop cannot be missed if it lands between the predicate and the wait. */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000; // 20 ms
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }
    const int pick = nr_pusch_queue_pick(&g_cursor, &g_pcursor);
    if (pick < 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) != 0) {
      pthread_mutex_unlock(&g_lock);
      break;
    }
    job = pick == 0 ? g_ring[nr_pusch_queue_pop(&g_cursor)] : g_pring[nr_pusch_queue_pop(&g_pcursor)];
    extern int nr_passive_e2e_trace(void);
    extern uint64_t nr_passive_slot_arrival_ns(long abs_slot);
    struct timespec e2e_t0 = {0}; /* dequeue time */
    if (nr_passive_e2e_trace()) {
      clock_gettime(CLOCK_MONOTONIC, &e2e_t0);
      nr_ss_ns[NRSS_PREP] = 0;
      uint64_t *gp = nr_ss_gpu_ptr();
      nr_ss_ns[NRSS_LDPC] = 0;
      if (gp) gp[NRSS_LDPC] = 0;
    }
    held = job.snap_idx; /* the queue's reference moves to this consumer */
    pthread_mutex_unlock(&g_lock);
    if (pick == 1) atomic_fetch_add_explicit(&g_dec_probe, 1, memory_order_relaxed);
    /* core pool: DECODE class for the probes too: this consumer is serial, a probe that waited as RETRY would hold the real UL grants behind it */
    nr_pool_site_acquire(NR_POOL_DECODE | NR_POOL_F_UL, nr_cell_idx(), nr_passive_slot_arrival_ns(job.absolute_slot));

    NR_CFG_EPOCH_WORK(job.config_epoch, &g_dropped_epoch);
    if (!nr_cfg_epoch_work_current()) {
      continue;
    }

    /* ---- STALENESS. A job's raw IQ lives in rxdata only until the producer reaches the same slot
     * index one frame later; decoding after that reads the NEXT frame's samples and fails CRC
     * indistinguishably from a weak channel. Count it, never "decode anyway and hope". ---- */
    const long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    if (lag > (long)atomic_load_explicit(&g_max_lag, memory_order_relaxed)) {
      atomic_store_explicit(&g_max_lag, (uint64_t)(lag > 0 ? lag : 0), memory_order_relaxed);
    }
    if ((held < 0 && !nr_pusch_queue_snapshot_fresh(prod, job.absolute_slot, slots_per_frame))
        || (held >= 0 && (!nr_pusch_queue_snapshot_fresh(prod, job.absolute_slot, slots_per_frame)
                          || ulq_wall_stale(job.absolute_slot, slots_per_frame)))) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      if (pick == 0) atomic_fetch_add_explicit(&g_stale_real, 1, memory_order_relaxed);
      continue;
    }

    const bool pipeline = nr_pusch_passive_pipeline_enabled();
    /* first decode of a real-grant job may combine/store; probes and CRC-first trials (hypothesis reads) never do */
    nr_pusch_passive_harq_arm(!ulq_is_probe(&job.grant));
    struct timespec grant_cpu_start={0},grant_cpu_end={0};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&grant_cpu_start);
    if (held >= 0) {
      nr_pusch_passive_decode_attach(idx, g_pool[held].iq, g_pool[held].origin, g_pool[held].len);
      atomic_fetch_add_explicit(&g_snap_used, 1, memory_order_relaxed);
    } else if (pipeline && !nr_pusch_passive_decode_capture(ue, idx, (uint8_t)job.nr_slot_rx,
                                                     (uint64_t)job.absolute_slot)) {
      atomic_fetch_add_explicit(&g_dropped_snapshot, 1, memory_order_relaxed);
      continue;
    }
    if (nr_ul_layout_is_probe(&job.grant)) {
      /* UL-LAYOUT probe (layout lock): DM-RS evidence for every UL BWP hypothesis, never a decode. */
      uint16_t p0[NR_UL_LAYOUT_MAX_HYP], np[NR_UL_LAYOUT_MAX_HYP], nid = 0;
      uint8_t l0 = 2;
      const int nh = nr_ul_layout_probe_plan(&job.grant, p0, np, NR_UL_LAYOUT_MAX_HYP, &nid, &l0);
      nr_ul_layout_probe_res_t r;
      if (nh > 0 && nr_pusch_passive_layout_probe(ue, idx, (uint8_t)job.nr_slot_rx, job.ta_offset_samples, job.fo_hz, nh,
                                                  p0, np, nid, l0, &r))
        nr_ul_layout_probe_feed(&job.grant, job.absolute_slot, &r);
      if (pipeline) nr_pusch_passive_decode_capture_release(idx);
      continue;
    }
    if (job.ul_ticket_valid && !job.cfr_only) {
      /* Review fix (REVIEW1 s2 defects 1+2): reset the solver once per broad invalidation
       * (HARD epoch / identity change / dedicated-change-suspected), not on every SOFT epoch
       * such as an RX-recovery restart, and only from the one consumer that wins the CAS. */
      NR_CELL_STATIC(_Atomic uint64_t, ulq_solver_gen);
      #define ulq_solver_gen NR_CELL_REF(ulq_solver_gen)
      const uint64_t gen = ((uint64_t)nr_cfg_epoch_identity_gen() << 32) | nr_cfg_epoch_dedicated_current();
      uint64_t prev_gen = atomic_load_explicit(&ulq_solver_gen, memory_order_acquire);
      if (prev_gen != gen &&
          atomic_compare_exchange_strong_explicit(&ulq_solver_gen, &prev_gen, gen, memory_order_acq_rel,
                                                  memory_order_acquire))
        nr_pusch_solver_reset();
      nr_pusch_solver_reading_t rd[64];
      const int nrd=solver_readings(&job,rd,64);
      nr_pusch_solver_var_t var[2];
      uint16_t ids[4]={job.grant.data_scrambling_id};
      int nids=1;
      nr_ue_ctx_t dl={0};
      if (nr_ue_ctx_try_get(job.grant.rnti,&dl)) {
        /* The DL MCS family is a ranking prior, never a proof of the UL
         * table. The physical enumerator still tries the other tables. */
        const nr_ue_cfg_value_t *m=&dl.cfg[NR_UEP_MCS_TABLE];
        if (m->verif==NR_UEV_TRUSTED && m->source!=NR_UES_UNKNOWN &&
            m->value>=0 && m->value<=2)
          for (int i=0;i<nrd;++i) rd[i].grant.mcs_table=(uint8_t)m->value;
        for (int k=0;k<dl.ul.n_id_priors && k<3;++k) {
          const uint16_t id=dl.ul.id_priors[k];
          bool seen=false;
          for (int h=0;h<nids;++h) seen |= ids[h]==id;
          if (!seen && nids<4) ids[nids++]=id;
        }
      }
      nr_pusch_solver_set_mu((int)ue->frame_parms.numerology_index);
      const nr_pdcch_blind_monitor_cfg_t *cfg=nr_pdcch_blind_monitor_get_cfg();
      /* nr_pdcch_blind_ul_dmrs_mask takes the RRC enum (pos2=0, pos3=1),
       * not the corresponding OFDM symbol number (2 or 3). */
      const int l0=cfg && cfg->dmrs_typeA_position==1 ? 1 : 0;
      const int nv=nr_pusch_solver_plan(&job.ul_ticket,rd,nrd,
          (uint64_t)job.absolute_slot-job.grant.k2,(uint64_t)job.absolute_slot,
          l0,ids,nids,var,2);
      bool ran_full = false;
      for (int k=0;k<nv;++k) {
        struct timespec p0,p1;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&p0);
        nr_pusch_passive_out_t probe_out;
        if (var[k].probe)
          nr_pusch_passive_decode_probe(ue,idx,(uint32_t)job.frame_rx,(uint8_t)job.nr_slot_rx,
                &var[k].grant,job.ta_offset_samples,(uint64_t)job.absolute_slot,job.fo_hz,&probe_out);
        else {
          ran_full = true;
          nr_pusch_passive_decode(ue,idx,(uint32_t)job.frame_rx,(uint8_t)job.nr_slot_rx,
                &var[k].grant,job.ta_offset_samples,(uint64_t)job.absolute_slot,false,job.fo_hz,&probe_out);
        }
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&p1);
        const uint64_t us=(uint64_t)(p1.tv_sec-p0.tv_sec)*1000000u+
                          (p1.tv_nsec-p0.tv_nsec)/1000;
        const bool credit=nr_cfg_epoch_work_current() &&
          probe_out.status!=NR_PUSCH_PASSIVE_UNSUPPORTED && probe_out.status!=NR_PUSCH_PASSIVE_ERROR;
        const bool ok=probe_out.status==NR_PUSCH_PASSIVE_OK && probe_out.tbs_bytes>0 &&
                      (!var[k].probe || probe_out.n_segments==1);
        nr_pusch_solver_feedback(&job.ul_ticket,&var[k],ok,credit,us);
        if (var[k].probe)
          LOG_I(PHY,"SENSING: UL_PUSCH_PROBE rnti=0x%04x dci=%lu target=%ld layouts=0x%016llx prb=%u+%u S=%u L=%u mcs=%u/t%u dmrs=0x%04x id=%u/%u ta=%d C=%d ok=%d status=%u cpu_us=%lu reason=%s\n",
                job.grant.rnti,(unsigned long)var[k].dci_slot,job.absolute_slot,
                (unsigned long long)var[k].layouts,
                var[k].grant.bwp_start+var[k].grant.start_rb,var[k].grant.num_rb,
                var[k].grant.start_symbol,var[k].grant.num_symbols,
                var[k].grant.mcs,var[k].grant.mcs_table,var[k].grant.ul_dmrs_symb_pos,
                var[k].grant.ul_dmrs_scrambling_id,var[k].grant.data_scrambling_id,
                probe_out.ta_applied_samples,probe_out.n_segments,ok,probe_out.status,
                (unsigned long)us,probe_out.reject_reason ? probe_out.reject_reason : "-");
        if (credit) {
          if (!var[k].probe) atomic_fetch_add_explicit(&g_decoded,1,memory_order_relaxed);
          if (ok) {
            if (!var[k].probe) {
              atomic_fetch_add_explicit(&g_crc_ok,1,memory_order_relaxed);
              nr_pusch_first_crc_result(&var[k].grant,&job.ul_ticket,true,&probe_out,
                                         job.absolute_slot,slots_per_frame,us);
            }
            if (!var[k].probe && nr_pusch_solver_is_solved(&job.ul_ticket)) {
              nr_ue_ul_update_t confirmed=job.ul_update;
              confirmed.bwp_start=var[k].grant.bwp_start;
              confirmed.bwp_size=var[k].grant.bwp_size;
              confirmed.tda=var[k].grant.tda_index;
              confirmed.ta_samples=probe_out.ta_applied_samples;
              confirmed.dmrs_id=probe_out.dmrs_id;
              confirmed.data_id=probe_out.data_id;
              confirmed.nscid=probe_out.nscid;
              nr_ue_ctx_ul_feedback(&job.ul_ticket,&confirmed,true);
              nr_pusch_passive_ul_ids_confirm(&job.ul_ticket,probe_out.nscid,
                                               probe_out.dmrs_id,probe_out.data_id,probe_out.tbs_bytes*8u,(int64_t)job.absolute_slot);
            }
          }
        }
      }
      if (ran_full) {
        if (pipeline) nr_pusch_passive_decode_capture_release(idx);
        continue;
      }
      /* fall through to the ordinary full decode of the DCI's own reading */
    }
    nr_pusch_passive_out_t out;
    nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &job.grant,
                            job.ta_offset_samples, (uint64_t)job.absolute_slot, job.cfr_only, job.fo_hz, &out);
    /* TAIL-FIX: the alternates of a failed grant (UL_RETRY below the verdict) run AFTER its verdict (UL_E2E / UL_PUSCH_CRC, first-decode time), on this same consumer,
     * and only while no real grant waits (re-checked before each alternate). A rescue is a late event, logged with its own arrival-to-rescue time. */
    const bool ul_retry_wanted = !job.cfr_only && !out.skip && out.status == NR_PUSCH_PASSIVE_CRC_FAIL &&
                                 ul_retry_snr_ok(&job.grant, out.snr_db) && job.grant.num_rb <= 12;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&grant_cpu_end);
    if (nr_ul_layout_is_trial(&job.grant)) /* CRC trials of the layout fit are optional work: they draw on the same per-second share as the UCI search */
      nr_pusch_passive_optional_charge((uint64_t)((grant_cpu_end.tv_sec-grant_cpu_start.tv_sec)*1000000000LL+(grant_cpu_end.tv_nsec-grant_cpu_start.tv_nsec)));
    if (!nr_cfg_epoch_work_current()) {
      if (pipeline) nr_pusch_passive_decode_capture_release(idx);
      continue;
    }
    if (nr_ul_layout_is_tagged(&job.grant) && (out.status==NR_PUSCH_PASSIVE_OK || out.status==NR_PUSCH_PASSIVE_CRC_FAIL))
      nr_ul_layout_crc_verdict(&job.grant, out.status==NR_PUSCH_PASSIVE_OK, out.tbs_bytes*8u, out.n_segments, out.segments_ok,
                               job.absolute_slot);
    if (nr_ul_layout_is_tagged(&job.grant) && out.G > 0 && (out.status==NR_PUSCH_PASSIVE_OK || out.status==NR_PUSCH_PASSIVE_CRC_FAIL))
      nr_ul_layout_prior_note(&job.grant, out.status==NR_PUSCH_PASSIVE_OK, out.tbs_bytes*8u, out.n_segments, out.segments_ok,
                              job.absolute_slot, out.snr_db, (float)out.tbs_bytes*8.f*(float)out.qam_mod_order/(float)out.G);
    if (out.status==NR_PUSCH_PASSIVE_OK && out.tb && out.tbs_bytes>=7) {
      /* Msg3 (TS 38.321 5.1.5): the UL CCCH SDU is the 48 bits the Msg4 Contention Resolution Identity must repeat. */
      uint8_t ccch[6];
      if (nr_reconf_ulsch_ccch(out.tb,out.tbs_bytes,ccch)) nr_ue_ctx_on_msg3(job.grant.rnti,nr_reconf_pack48(ccch),(int64_t)job.absolute_slot);
    }
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_OK) {
      nr_ue_ul_update_t confirmed=job.ul_update;
      confirmed.ta_samples=out.ta_applied_samples;
      confirmed.dmrs_id=out.dmrs_id;
      confirmed.data_id=out.data_id;
      confirmed.nscid=out.nscid;
      nr_ue_ctx_ul_feedback(&job.ul_ticket,&confirmed,true);
    }
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_OK)
      nr_pusch_passive_ul_ids_confirm(&job.ul_ticket,out.nscid,out.dmrs_id,out.data_id,out.tbs_bytes*8u,(int64_t)job.absolute_slot);
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_CRC_FAIL)
      nr_pusch_passive_ul_ids_failed(&job.ul_ticket);
    if(!job.cfr_only && (out.status==NR_PUSCH_PASSIVE_OK ||
        out.status==NR_PUSCH_PASSIVE_CRC_FAIL || out.status==NR_PUSCH_PASSIVE_ZERO_TB)) {
      if (!nr_ul_layout_is_tagged(&job.grant)) /* UL-LAYOUT grants belong to no width/U3 search */
        nr_pdcch_ul_discovery_feedback(&job.grant,out.status==NR_PUSCH_PASSIVE_OK);
      nr_pdcch_dci01_fdra_feedback(&job.grant, out.status==NR_PUSCH_PASSIVE_OK);
    }
    if (nr_passive_e2e_trace() && !job.cfr_only) {
      /* UL_E2E: per real-grant full decode (incl. UL_RETRY alternates): arr2deq = target-slot samples read -> dequeued,
       * wall = dequeued -> decoded (us), tot = arrival -> decoded; pick 1 = probe ring (layout search, not a locked real grant) */
      struct timespec e2e_t1;
      clock_gettime(CLOCK_MONOTONIC, &e2e_t1);
      const uint64_t t0 = (uint64_t)e2e_t0.tv_sec * 1000000000ull + e2e_t0.tv_nsec, t1 = (uint64_t)e2e_t1.tv_sec * 1000000000ull + e2e_t1.tv_nsec;
      const uint64_t arr = nr_passive_slot_arrival_ns(job.absolute_slot);
      LOG_A(PHY, "SENSING: UL_E2E rnti=0x%04x st=%u prb=%u mcs=%u pick=%d arr2deq=%ld wall=%lu tot=%ld cpu_us=%ld stg=ldpc:g%lu,prep:%lu probe=%d\n", job.grant.rnti,
            (unsigned)out.status, (unsigned)job.grant.num_rb, (unsigned)job.grant.mcs, pick, arr && t0 >= arr ? (long)((t0 - arr) / 1000) : -1L,
            (unsigned long)((t1 - t0) / 1000), arr && t1 >= arr ? (long)((t1 - arr) / 1000) : -1L,
            (long)((grant_cpu_end.tv_sec-grant_cpu_start.tv_sec)*1000000L+(grant_cpu_end.tv_nsec-grant_cpu_start.tv_nsec)/1000L),
            (unsigned long)(((nr_ss_gpu_ptr() && nr_ss_gpu_ptr() != nr_ss_ns ? nr_ss_gpu_ptr()[NRSS_LDPC] : 0) + nr_ss_ns[NRSS_LDPC]) / 1000), (unsigned long)(nr_ss_ns[NRSS_PREP] / 1000), (int)(pick == 1 || ulq_is_probe(&job.grant)));
    }
    {
      const uint64_t cpu_us=(uint64_t)((grant_cpu_end.tv_sec-grant_cpu_start.tv_sec)*1000000L+
                                      (grant_cpu_end.tv_nsec-grant_cpu_start.tv_nsec)/1000L);
      nr_pusch_first_crc_result(&job.grant,&job.ul_ticket,job.ul_ticket_valid,&out,
                                job.absolute_slot,slots_per_frame,cpu_us);
    }


    if (out.status != NR_PUSCH_PASSIVE_UNSUPPORTED && out.status != NR_PUSCH_PASSIVE_ERROR) {
      atomic_fetch_add_explicit(&g_decoded, 1, memory_order_relaxed);
      if (out.status == NR_PUSCH_PASSIVE_OK) {
        atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
      }
    }
    if (ul_retry_wanted) {
      /* Only grants with a measurable signal are retried (the extra LDPC runs cost CPU). Alternates, in order:
       * the other CDM-group count (data-RE set and G change), then the S=1/L=13 alias of S=0/L=14 (the DL TDRA
       * aliasing class: a symbol-0 mismatch leaves one wrong data symbol that LDPC cannot absorb at high MCS). */
      nr_pdcch_blind_ul_result_t alts[2];
      int na = 0;
      if ((job.grant.n_dmrs_cdm_groups == 1 || job.grant.n_dmrs_cdm_groups == 2) && !job.grant.transform_precoding) {
        alts[na] = job.grant;
        alts[na].n_dmrs_cdm_groups = (uint8_t)(3 - job.grant.n_dmrs_cdm_groups);
        na++;
      }
      if (job.grant.start_symbol == 0 && job.grant.num_symbols == 14) {
        alts[na] = job.grant;
        alts[na].start_symbol = 1;
        alts[na].num_symbols = 13;
        na++;
      }
      for (int a = 0; a < na && nr_pusch_passive_rescue_allowed(); ++a) {
        nr_pusch_passive_out_t out2;
        nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &alts[a],
                                job.ta_offset_samples, (uint64_t)job.absolute_slot, false, job.fo_hz, &out2);
        if (out2.status != NR_PUSCH_PASSIVE_OK) continue;
        struct timespec t_r;
        clock_gettime(CLOCK_MONOTONIC, &t_r);
        const uint64_t arr_r = nr_passive_slot_arrival_ns(job.absolute_slot), now_r = (uint64_t)t_r.tv_sec * 1000000000ull + t_r.tv_nsec;
        LOG_A(PHY, "SENSING: UL_RETRY rnti=0x%04x CRC OK with cdm_groups=%u S=%u L=%u (reading said cdm=%u S=%u L=%u) late tot_us=%ld\n",
              alts[a].rnti, alts[a].n_dmrs_cdm_groups, alts[a].start_symbol, alts[a].num_symbols,
              job.grant.n_dmrs_cdm_groups, job.grant.start_symbol, job.grant.num_symbols, arr_r && now_r >= arr_r ? (long)((now_r - arr_r) / 1000) : -1L);
        /* the updates the first pass withheld: CRC-OK counter and the CRC-OK feedback of the solver ticket, the ID store, the discovery / FDRA searches */
        atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
        if (job.ul_ticket_valid) {
          nr_ue_ul_update_t confirmed = job.ul_update;
          confirmed.ta_samples = out2.ta_applied_samples;
          confirmed.dmrs_id = out2.dmrs_id;
          confirmed.data_id = out2.data_id;
          confirmed.nscid = out2.nscid;
          nr_ue_ctx_ul_feedback(&job.ul_ticket, &confirmed, true);
          nr_pusch_passive_ul_ids_confirm(&job.ul_ticket, out2.nscid, out2.dmrs_id, out2.data_id, out2.tbs_bytes * 8u, (int64_t)job.absolute_slot);
        }
        if (!nr_ul_layout_is_tagged(&job.grant)) nr_pdcch_ul_discovery_feedback(&job.grant, true);
        nr_pdcch_dci01_fdra_feedback(&job.grant, true);
        break;
      }
    }
    if (pipeline) nr_pusch_passive_decode_capture_release(idx);
  }

  if (held >= 0) {
    pthread_mutex_lock(&g_lock);
    g_pool_refs[held]--;
    pthread_mutex_unlock(&g_lock);
  }
  if(nr_pusch_passive_pipeline_enabled()) {
    struct timespec wall_end,cpu_end;
    clock_gettime(CLOCK_MONOTONIC,&wall_end);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_end);
    const uint64_t wall_ns=(uint64_t)(wall_end.tv_sec-wall_start.tv_sec)*1000000000ull+
                           (uint64_t)(wall_end.tv_nsec-wall_start.tv_nsec);
    const uint64_t cpu_ns=(uint64_t)(cpu_end.tv_sec-cpu_start.tv_sec)*1000000000ull+
                          (uint64_t)(cpu_end.tv_nsec-cpu_start.tv_nsec);
    LOG_I(PHY,"SENSING: UL_PIPELINE worker_cpu_ms=%.3f wall_ms=%.3f core_fraction=%.4f\n",
          cpu_ns/1e6,wall_ns/1e6,wall_ns?(double)cpu_ns/wall_ns:0.0);
  }
  LOG_I(PHY, "SENSING: passive PUSCH decode consumer %d exiting\n", idx);
  return NULL;
}

static bool ulq_is_probe(const nr_pdcch_blind_ul_result_t *g)
{
  return nr_ul_layout_is_probe(g) || nr_ul_layout_is_trial(g);
}

/* ULQ capture thread: copies each booked UL slot ONCE, oldest real grant first, into a free
 * pool buffer and hands one reference to every queued job of that slot (probes of the same slot share it for free;
 * a probe alone never takes a buffer). A job it cannot serve (no free buffer, IQ already overwritten) gets
 * snap_idx -2 and takes the old per-consumer copy at dequeue, with the old freshness check. Under g_lock except
 * for the copy itself. */
static void ulq_assign(long abs, int b)
{
  for (int r = 0; r < 2; r++) {
    nr_pusch_queue_cursor_t *c = r ? &g_pcursor : &g_cursor;
    nr_pusch_passive_job_t *ring = r ? g_pring : g_ring;
    for (int k = 0; k < c->count; k++) {
      nr_pusch_passive_job_t *j = &ring[(c->tail + k) % c->depth];
      if (j->absolute_slot != abs || j->snap_idx != -1) continue;
      j->snap_idx = (int16_t)b;
      if (b >= 0) g_pool_refs[b]++;
    }
  }
}

static void *ulq_capture_thread(void *arg)
{
  nr_cell_bind(((consumer_arg_t *)arg)->cell);
  PHY_VARS_NR_UE *ue = g_ue;
  const uint32_t cap = nr_pusch_passive_snapshot_len(&ue->frame_parms);
  pthread_mutex_lock(&g_lock);
  while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
    long abs = -1;
    int slot = 0;
    bool want_next = false;
    for (int k = 0; k < g_cursor.count && abs < 0; k++) {
      const nr_pusch_passive_job_t *j = &g_ring[(g_cursor.tail + k) % g_cursor.depth];
      if (j->snap_idx == -1 && !ulq_is_probe(&j->grant)) {
        abs = j->absolute_slot;
        slot = j->nr_slot_rx;
      }
    }
    for (int k = 0; g_snap_next && abs >= 0 && k < g_cursor.count && !want_next; k++) { /* the slot after it, only if a job of this slot could become a stage-B job */
      const nr_pusch_passive_job_t *j = &g_ring[(g_cursor.tail + k) % g_cursor.depth];
      if (j->absolute_slot == abs && !ulq_is_probe(&j->grant)) want_next = nr_ul_nid_bg_wants_next(&j->grant);
    }
    const int b = abs >= 0 ? nr_pusch_snap_free(g_pool_refs, g_pool_n) : -1;
    if (abs >= 0 && b < 0) {
      ulq_assign(abs, -2);
      atomic_fetch_add_explicit(&g_snap_nobuf, 1, memory_order_relaxed);
    }
    if (b < 0) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cap_cv, &g_lock, &ts);
      continue;
    }
    g_pool_refs[b] = 1; /* this thread's hold during the copy */
    g_pool[b].abs = -1; /* not shareable by enqueue until the copy is complete */
    g_pool[b].len = 0;
    g_pool2[b].abs = -1;
    g_pool2[b].len = 0;
    pthread_mutex_unlock(&g_lock);
    uint32_t origin = 0, len = 0, origin2 = 0, len2 = 0;
    nr_pool_site_acquire(NR_POOL_DECODE | NR_POOL_F_UL, nr_cell_idx(), nr_passive_slot_arrival_ns(abs)); /* the copy gates the UL decode: a lease per copy, none across the wait below */
    const bool ok = nr_pusch_passive_snapshot_copy(ue, (uint8_t)slot, (uint64_t)abs, g_pool[b].iq, cap, &origin, &len);
    nr_pool_site_release();
    bool ok2 = false;
    for (int w = 0; ok && want_next && w < 40; w++) { /* the next slot is produced 0.5 ms after the target slot: wait for it (<= 4 ms), once it exists copy once */
      if (atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_acquire) < abs + 1) { usleep(100); continue; }
      nr_pool_site_acquire(NR_POOL_DECODE | NR_POOL_F_UL, nr_cell_idx(), nr_passive_slot_arrival_ns(abs));
      ok2 = nr_pusch_passive_snapshot_copy(ue, (uint8_t)((slot + 1) % ue->frame_parms.slots_per_frame), (uint64_t)(abs + 1), g_pool2[b].iq, cap, &origin2, &len2);
      nr_pool_site_release();
      break;
    }
    pthread_mutex_lock(&g_lock);
    if (ok2) { g_pool2[b].abs = abs + 1; g_pool2[b].origin = origin2; g_pool2[b].len = len2; }
    g_pool[b].abs = abs;
    g_pool[b].origin = origin;
    g_pool[b].len = len;
    ulq_assign(abs, ok ? b : -2);
    g_pool_refs[b]--;
    atomic_fetch_add_explicit(ok ? &g_snap_ok : &g_snap_fail, 1, memory_order_relaxed);
  }
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

/* n_ID search stage B: a private copy of the slot AFTER `abs` (hypotheses with k2 + 1), taken by the capture thread together with the pool snapshot of
 * `abs`. The copy runs without g_lock under a reference. false = not captured (or larger than cap). */
bool nr_pusch_passive_queue_pool_copy_next(long abs, int nant, c16_t *const *dst, uint32_t cap, uint32_t *origin, uint32_t *len)
{
  int b = -1;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; g_snap_next && i < g_pool_n; i++)
    if (g_pool[i].abs == abs && g_pool2[i].abs == abs + 1 && g_pool2[i].len > 0 && g_pool2[i].len <= cap) { b = i; break; }
  if (b >= 0) g_pool_refs[b]++;
  pthread_mutex_unlock(&g_lock);
  if (b < 0) return false;
  for (int a = 0; a < nant && a < 4; a++) memcpy(dst[a], g_pool2[b].iq[a], (size_t)g_pool2[b].len * sizeof(c16_t));
  *origin = g_pool2[b].origin;
  *len = g_pool2[b].len;
  pthread_mutex_lock(&g_lock);
  g_pool_refs[b]--;
  pthread_mutex_unlock(&g_lock);
  return true;
}

/* nr_pusch_passive_queue_start: starts the passive PUSCH decode pool: sizes the ring (depth clamped to [2, MAX]) and launches n_consumers consumer threads at SCHED_OTHER priority 50, below the RT receive thread.
 *   Idempotent: returns true at once when the pool already runs. Consumer cores come from nr_core_plan_cpu(NR_CP_PUSCH) or from affinity + index. The frequency offset is sampled by the producer and carried per job (fo_hz), so no --cont-fo-comp refusal is needed.
 *   Caller: nr_pusch_passive_monitor_rt.c (UL monitor start-up). Returns false only when ue is NULL.
 */
bool nr_pusch_passive_queue_start(PHY_VARS_NR_UE *ue, int depth, int n_consumers, int affinity)
{
  if (atomic_load_explicit(&g_running, memory_order_acquire)) {
    return true;
  }
  if (ue == NULL) {
    return false;
  }
  if (nr_pusch_passive_pipeline_enabled()) {
    /* One prewarmed context per consumer (the consumer index IS the context index). Default 1 as before. */
    n_consumers = 2;
    if (n_consumers < 1) n_consumers = 1;
    if (n_consumers > NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS;
    for (int c = 0; c < n_consumers; c++)
      if (!nr_pusch_passive_decode_prewarminit(ue, c)) {
        if (c == 0) return false;
        n_consumers = c;
        break;
      }
    depth = 32;
  }
  /* The --cont-fo-comp refusal that used to live here is GONE, and the hazard it named is fixed
   * rather than avoided. It read: a deferred nr_slot_fep() would de-rotate using
   * ue->dl_Doppler_shift + ue->freq_offset, both mutated by the RECEIVE thread, hence newer than
   * the job's own samples. True -- so the producer now SAMPLES that offset on the receive thread,
   * in the same call that captured the samples, and carries it in nr_pusch_passive_job_t::fo_hz;
   * the consumer passes it to nr_pusch_passive_decode() instead of reading ue->.
   *
   * Why this mattered enough to fix: the fallback was not benign. --cont-fo-comp is required for
   * CFO to converge at sensing grade, so every run had it on, so the queue never started, so the
   * decode (measured 1065 us mean against a 500 us slot) ran in-line and overran its deadline on
   * 99.5 % of grants. Under bidirectional iperf that timing runaway lost PBCH lock and the
   * supervisor killed the capture at 73 s. The two features were mutually exclusive by accident.
   */

  if (n_consumers < 1) n_consumers = 1;
  if (n_consumers > NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS;
  if (depth < 2) depth = 2;
  if (depth > NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH) depth = NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH;

  g_ue       = ue;
  g_cursor   = (nr_pusch_queue_cursor_t){.depth=depth};
  g_nthreads = n_consumers;
  g_pcursor = (nr_pusch_queue_cursor_t){.depth=8}; /* probe queue depth */
  g_pool_n = 0;
  g_snap_next = 1;
  {
    /* Distinct slots alive at once <= queued real jobs + one per consumer + the one being copied. */
    const uint32_t len = nr_pusch_passive_snapshot_len(&ue->frame_parms);
    const int nant = nr_pusch_passive_snapshot_nant();
    int want = depth + n_consumers + 2;
    if (want > ULQ_POOL_MAX) want = ULQ_POOL_MAX;
    for (int b = 0; b < want && len && nant > 0 && nant <= 4; b++) {
      bool okb = true;
      for (int a = 0; a < nant; a++) {
        if (!g_pool[b].iq[a]) g_pool[b].iq[a] = malloc16_clear((size_t)len * sizeof(c16_t));
        okb &= g_pool[b].iq[a] != NULL;
        if (g_snap_next) {
          if (!g_pool2[b].iq[a]) g_pool2[b].iq[a] = malloc16_clear((size_t)len * sizeof(c16_t));
          if (!g_pool2[b].iq[a]) g_snap_next = 0;
        }
      }
      if (!okb) break;
      g_pool_refs[b] = 0;
      g_pool_n = b + 1;
    }
  }
  atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
  atomic_store_explicit(&g_running, 1, memory_order_release);

  for (int i = 0; i < n_consumers; i++) {
    g_args[i].idx = i;
    g_args[i].cell = nr_cell_idx();
    char name[32];
    snprintf(name, sizeof(name), "passivePusch%d", i);
    /* Priority BELOW the RT receive thread's 97: if a consumer ever competes with the receive path
     * it must lose. Losing here costs one deferred decode; losing there costs time sync, which is
     * the whole problem this module exists to fix. */
    int core = nr_core_plan_cpu(NR_CP_PUSCH, i, (affinity >= 0) ? (affinity + i) : -1);
    /* ISAC_PUSCH_CORES="17,18": the UL trial decodes ran unpinned at FIFO 50, above the PDSCH consumers (40), and inflated every PDSCH
     * stage (DECQUEUE report). Pin them off the PDSCH cores, at the default priority. */
    core = nr_env_list_nth("ISAC_PUSCH_CORES", i, core);
    const int prio = 0;
    threadCreate(&g_threads[i], nr_pusch_passive_queue_thread, &g_args[i], name, nr_pool_site() ? -1 : core, prio);
  }
  if (g_pool_n > 0) {
    const char *ce = getenv("ISAC_UL_SNAP_CORE");
    const int core = ce && *ce ? atoi(ce) : 8;
    const int prio = 0;
    (void)core; /* the snapshot copy leases a pool token per copy (DECODE class); the thread itself sits on the home lane */
    g_cap_arg.idx = 0;
    g_cap_arg.cell = nr_cell_idx();
    threadCreate(&g_cap_thread, ulq_capture_thread, &g_cap_arg, "passivePuschCap", nr_pool_site() ? -1 : core, prio);
  }
  LOG_I(PHY, "SENSING: passive PUSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest; ULQ probe_depth=%d snap_pool=%d\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1, g_pcursor.depth, g_pool_n);
  return true;
}

bool nr_pusch_passive_queue_running(void)
{
  return atomic_load_explicit(&g_running, memory_order_acquire) != 0;
}

bool nr_pusch_passive_queue_enqueue(const nr_pusch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  pthread_mutex_lock(&g_lock);
  if (atomic_load_explicit(&g_stop, memory_order_relaxed)) {
    pthread_mutex_unlock(&g_lock);
    return false;
  }
  int evicted;
  const bool probe = ulq_is_probe(&job->grant);
  nr_pusch_queue_cursor_t *cur = probe ? &g_pcursor : &g_cursor;
  nr_pusch_passive_job_t *ring = probe ? g_pring : g_ring;
  const int index=nr_pusch_queue_push(cur,&evicted);
  /* DROP-OLDEST: the evicted entry is closest to IQ overwrite. A probe evicts only probes. */
  if(evicted>=0) {
    atomic_fetch_add_explicit(probe ? &g_full_probe : &g_dropped_full, 1, memory_order_relaxed);
    if (ring[evicted].snap_idx >= 0) g_pool_refs[ring[evicted].snap_idx]--;
  }
  ring[index] = *job;
  ring[index].snap_idx = -1;
  ring[index].config_epoch = nr_passive_job_epoch_stamp(nr_cfg_reconf_enabled(), nr_cfg_epoch_work_stamp);
  /* A probe of an already-copied slot shares that copy (its real grant may have been booked first). */
  if (g_pool_n > 0)
    for (int b = 0; b < g_pool_n; b++)
      if (g_pool_refs[b] > 0 && g_pool[b].abs == job->absolute_slot && g_pool[b].len) {
        ring[index].snap_idx = (int16_t)b;
        g_pool_refs[b]++;
        break;
      }
  pthread_cond_signal(&g_cv);
  if (g_pool_n > 0) pthread_cond_signal(&g_cap_cv);
  pthread_mutex_unlock(&g_lock);
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(probe ? &g_q_probe : &g_q_real, 1, memory_order_relaxed);
  return true;
}

void nr_pusch_passive_queue_get_stats(nr_pusch_passive_queue_stats_t *out)
{
  if (out == NULL) {
    return;
  }
  out->queued        = atomic_load_explicit(&g_queued, memory_order_relaxed);
  out->decoded       = atomic_load_explicit(&g_decoded, memory_order_relaxed);
  out->crc_ok        = atomic_load_explicit(&g_crc_ok, memory_order_relaxed);
  out->dropped_full  = atomic_load_explicit(&g_dropped_full, memory_order_relaxed);
  out->dropped_stale = atomic_load_explicit(&g_dropped_stale, memory_order_relaxed);
  out->dropped_epoch = atomic_load_explicit(&g_dropped_epoch, memory_order_relaxed);
  out->dropped_unready = atomic_load_explicit(&g_dropped_unready, memory_order_relaxed);
  out->dropped_snapshot = atomic_load_explicit(&g_dropped_snapshot, memory_order_relaxed);
  out->max_lag_slots = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
}

void nr_pusch_passive_queue_stop(void)
{
  pthread_mutex_lock(&g_lifecycle_lock);
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    pthread_mutex_unlock(&g_lifecycle_lock);
    return;
  }
  atomic_store_explicit(&g_stop, 1, memory_order_release);
  pthread_mutex_lock(&g_lock);
  atomic_fetch_add_explicit(&g_dropped_unready,nr_pusch_queue_discard_all(&g_cursor),memory_order_relaxed);
  atomic_fetch_add_explicit(&g_dropped_unready,nr_pusch_queue_discard_all(&g_pcursor),memory_order_relaxed);
  pthread_cond_broadcast(&g_cv);
  pthread_cond_broadcast(&g_cap_cv);
  pthread_mutex_unlock(&g_lock);
  for (int i = 0; i < g_nthreads; i++) {
    pthread_join(g_threads[i], NULL);
  }
  if (g_pool_n > 0) pthread_join(g_cap_thread, NULL);
  for (int b = 0; b < g_pool_n; b++) g_pool_refs[b] = 0; /* discarded jobs drop their references with the rings */
  atomic_store_explicit(&g_running, 0, memory_order_release);
  pthread_mutex_unlock(&g_lifecycle_lock);
}

int nr_pusch_passive_queue_real_pending(void)
{
  pthread_mutex_lock(&g_lock);
  const int n = g_cursor.count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

NR_CELL_STATIC(_Atomic uint64_t, g_opt_win_start_ns);
#define g_opt_win_start_ns NR_CELL_REF(g_opt_win_start_ns)
NR_CELL_STATIC(_Atomic uint64_t, g_opt_win_used_ns);
#define g_opt_win_used_ns NR_CELL_REF(g_opt_win_used_ns)
/* mpci A6: the optional-work window (g_opt_win_*) is PER CELL, the budget it is compared with is the cell's fair share.
 * The core pool (common/utils/nr_core_pool.h) serves the cells with equal weights (decayed token-time usage per cell) and is
 * work conserving; a process-wide window would let one cell's UCI explore / rescue work use up the share of another cell
 * (starving its optional work while the pool still grants it equal tokens), and the N=1 result of a cell would depend on its
 * neighbours. Per cell window + per cell share keeps each cell's optional work independent. The process total stays at the
 * single-cell value: without a dedicated core list the share is (1 s / online cpus) / active cells, so N cells together spend
 * what one cell spent. A dedicated list (ISAC_PUSCH_CORES, existing variable) keeps its full-second meaning per cell. N=1: unchanged. */
static uint64_t opt_share_ns_per_s(void)
{
  static _Atomic uint64_t share; /* PROCESS: cache of a host constant (online cpus), idempotent */
  static _Atomic int dedicated;
  uint64_t v = atomic_load_explicit(&share, memory_order_acquire);
  if (!v) {
    const char *c = getenv("ISAC_PUSCH_CORES");
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    v = (c && *c) ? 1000000000ull : 1000000000ull / (uint64_t)(ncpu > 0 ? ncpu : 1);
    atomic_store_explicit(&dedicated, (c && *c) ? 1 : 0, memory_order_relaxed);
    atomic_store_explicit(&share, v, memory_order_release);
  }
  if (!atomic_load_explicit(&dedicated, memory_order_relaxed)) {
    const int cells = nr_cell_count();
    if (cells > 1) v /= (uint64_t)cells;
  }
  return v;
}
bool nr_pusch_passive_optional_share_left(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, w0 = atomic_load_explicit(&g_opt_win_start_ns, memory_order_relaxed);
  if (now - w0 > 1000000000ull) {
    atomic_store_explicit(&g_opt_win_start_ns, now, memory_order_relaxed);
    atomic_store_explicit(&g_opt_win_used_ns, 0, memory_order_relaxed);
  }
  return atomic_load_explicit(&g_opt_win_used_ns, memory_order_relaxed) < opt_share_ns_per_s();
}
void nr_pusch_passive_optional_charge(uint64_t ns)
{
  atomic_fetch_add_explicit(&g_opt_win_used_ns, ns, memory_order_relaxed);
}

bool nr_pusch_passive_rescue_allowed(void)
{
  pthread_mutex_lock(&g_lock);
  const int n = g_cursor.count;
  pthread_mutex_unlock(&g_lock);
  if (n > 0) atomic_fetch_add_explicit(&g_rescue_skipped, 1, memory_order_relaxed);
  return n == 0;
}

void nr_pusch_passive_queue_ulq_dump(void)
{
  LOG_I(PHY, "SENSING: PUSCHQ2 consumers=%d real[queued=%lu full=%lu] real_stale=%lu probe[queued=%lu decoded=%lu full=%lu] "
             "snap[pool=%d ok=%lu fail=%lu nobuf=%lu used=%lu] rescue_skipped=%lu\n",
        g_nthreads, (unsigned long)atomic_load(&g_q_real), (unsigned long)atomic_load(&g_dropped_full),
        (unsigned long)atomic_load(&g_stale_real),
        (unsigned long)atomic_load(&g_q_probe), (unsigned long)atomic_load(&g_dec_probe),
        (unsigned long)atomic_load(&g_full_probe), g_pool_n, (unsigned long)atomic_load(&g_snap_ok),
        (unsigned long)atomic_load(&g_snap_fail), (unsigned long)atomic_load(&g_snap_nobuf),
        (unsigned long)atomic_load(&g_snap_used), (unsigned long)atomic_load(&g_rescue_skipped));
}
