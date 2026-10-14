#include "common/utils/nr_core_plan.h"
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
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/utils/LOG/log.h"
#include "common/utils/system.h" // threadCreate
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
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h" // nr_get_Qm_ul

/* The RF producer's own position, published in executables/nr-ue.c immediately BEFORE nrue_ru_read()
 * fills that slot's region of rxdata. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

static nr_pusch_passive_job_t g_ring[NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH];
static nr_pusch_queue_cursor_t g_cursor;
/* ULQ (all default off, read once at start):
 *  ISAC_UL_PRIO_QUEUE=1      layout probes and CRC-first trials go to their own ring (ISAC_UL_PROBE_DEPTH, default 8)
 *                            and are dequeued only when no real grant waits; a probe can no longer evict a real grant.
 *  ISAC_PUSCH_CONSUMERS=n    n consumers / decode contexts in pipeline mode (default 1), cores from ISAC_PUSCH_CORES.
 *  ISAC_UL_QUEUE_DEPTH=d     real-grant ring depth (default: the cfg ul_queue_depth, else 8).
 *  ISAC_UL_SNAP_POOL=1       one shared IQ snapshot per UL slot, copied by a capture thread right after booking
 *                            (ISAC_UL_SNAP_CORE / ISAC_UL_SNAP_PRIO), so a real grant stays decodable while it waits.
 *  ISAC_UL_SPARE_RESCUE=1    UCI explore search and UL_RETRY alternates run only while no real grant waits.
 *  ISAC_UL_CPU_BUDGET_PCT=p  each consumer idles so its CPU time stays <= p % of wall time (100 ms windows). */
static nr_pusch_passive_job_t g_pring[NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH];
static nr_pusch_queue_cursor_t g_pcursor;
static int g_prio, g_spare_rescue, g_budget_pct;
#define ULQ_POOL_MAX (NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH + NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS + 2)
typedef struct {
  c16_t *iq[4];
  long abs;
  uint32_t origin, len;
} ulq_snap_t;
static ulq_snap_t g_pool[ULQ_POOL_MAX];
static int g_pool_refs[ULQ_POOL_MAX]; /* under g_lock */
static int g_pool_n;
static pthread_t g_cap_thread;
static pthread_cond_t g_cap_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_q_real, g_q_probe, g_dec_probe, g_full_probe, g_snap_ok, g_snap_fail, g_snap_nobuf,
    g_snap_used, g_rescue_skipped, g_budget_sleep_us;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_decoded       = 0;
static _Atomic uint64_t g_crc_ok        = 0;
static _Atomic uint64_t g_dropped_full  = 0;
static _Atomic uint64_t g_dropped_stale = 0;
static _Atomic uint64_t g_dropped_epoch = 0;
static _Atomic uint64_t g_dropped_unready = 0;
static _Atomic uint64_t g_dropped_snapshot = 0;
static _Atomic uint64_t g_max_lag       = 0;

static _Atomic int g_running  = 0;
static _Atomic int g_stop     = 0;
static int         g_nthreads = 0;
static pthread_t   g_threads[NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static PHY_VARS_NR_UE *g_ue = NULL;

typedef struct {
  int idx;
} consumer_arg_t;
static consumer_arg_t g_args[NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS];

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

/* ISAC_UL_SOLVER_FALLTHROUGH=1 (default off): with ISAC_UL_PUSCH_SOLVER=1 every ticketed grant used to be
 * consumed by the solver branch, whose unsolved state only probes one-CB QPSK grants of <= 48 PRB
 * (one_cb_qpsk()). A grant outside that class (64/256QAM, e.g. the 3-PRB MCS 17-27 grants that are the only
 * detectable PUSCH at the DGX position) was therefore never decoded. With the flag such a grant also takes the
 * ordinary full-decode path below when the solver issued no full decode for it. */
/* ULQ: ISAC_UL_SOLVER_FALLTHROUGH=2 also falls through for the probe class (one-CB QPSK <= 48 PRB), so every real
 * grant the solver did not fully decode still gets one ordinary full decode. */
static int ul_solver_fallthrough(void)
{
  static int on = -1;
  if (on < 0) {
    const char *v = getenv("ISAC_UL_SOLVER_FALLTHROUGH");
    on = v ? atoi(v) : 0;
  }
  return on;
}

/* ISAC_UL_RETRY=1 (default off): UL twin of ISAC_PDSCH_DMRS_CDM_RETRY / ISAC_PDSCH_TDRA_RETRY. The DCI antenna-ports code
 * point fixes the number of CDM groups without data, but its field width is a hypothesis (U3 width splits), so the group
 * count can be wrong while everything else is right; likewise S=0/L=14 can alias S=1/L=13. A CRC-failed full decode whose
 * allocation is at most ISAC_UL_RETRY_MAX_RB PRB (default 12) and, if ISAC_UL_RETRY_SNR_DB is set, whose post-estimation SNR reaches it, is retried with each alternative. */
static bool ul_retry_enabled(void)
{
  static int on = -1;
  if (on < 0) {
    const char *v = getenv("ISAC_UL_RETRY");
    on = (v && !strcmp(v, "1")) ? 1 : 0;
  }
  return on == 1;
}
static float ul_retry_snr_db(void)
{
  static float t = -1000.0f;
  if (t < -999.0f) {
    const char *v = getenv("ISAC_UL_RETRY_SNR_DB");
    t = v ? (float)atof(v) : -500.0f; /* post-estimation snr_db is not populated by the PUSCH path (0.0): gate off by default */
  }
  return t;
}
/* Default size gate instead: only small allocations (the detectable class at this position) are retried. */
static int ul_retry_max_rb(void)
{
  static int m = -1;
  if (m < 0) {
    const char *v = getenv("ISAC_UL_RETRY_MAX_RB");
    m = v ? atoi(v) : 12;
  }
  return m;
}

bool nr_pusch_passive_pipeline_enabled(void)
{
  const char *v = getenv("ISAC_UL_PIPELINE");
  return nr_ul_chain_on() || (v && strcmp(v, "1") == 0);
}

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
  PHY_VARS_NR_UE *ue = g_ue;
  const long slots_per_frame = ue->frame_parms.slots_per_frame;
  struct timespec wall_start={0},cpu_start={0};
  if(nr_pusch_passive_pipeline_enabled()) {
    clock_gettime(CLOCK_MONOTONIC,&wall_start);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_start);
  }

  LOG_I(PHY, "SENSING: passive PUSCH decode consumer %d started\n", idx);

  int held = -1; /* ULQ shared snapshot this consumer holds a reference on */
  struct timespec bw0 = {0}, bc0 = {0};
  if (g_budget_pct) {
    clock_gettime(CLOCK_MONOTONIC, &bw0);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &bc0);
  }
  while (1) {
    nr_pusch_passive_job_t job;
    if (held >= 0) {
      pthread_mutex_lock(&g_lock);
      g_pool_refs[held]--;
      pthread_cond_signal(&g_cap_cv);
      pthread_mutex_unlock(&g_lock);
      held = -1;
    }
    if (g_budget_pct) {
      struct timespec w, c;
      clock_gettime(CLOCK_MONOTONIC, &w);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
      const long wall = (w.tv_sec - bw0.tv_sec) * 1000000000L + (w.tv_nsec - bw0.tv_nsec);
      const long cpu = (c.tv_sec - bc0.tv_sec) * 1000000000L + (c.tv_nsec - bc0.tv_nsec);
      const long idle = nr_pusch_budget_idle_ns(cpu, wall, g_budget_pct);
      if (idle > 0) {
        const struct timespec d = {.tv_sec = idle / 1000000000L, .tv_nsec = idle % 1000000000L};
        nanosleep(&d, NULL);
        atomic_fetch_add_explicit(&g_budget_sleep_us, (uint64_t)(idle / 1000), memory_order_relaxed);
      }
      if (wall > 100000000L) { bw0 = w; bc0 = c; } /* 100 ms windows */
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
    held = job.snap_idx; /* the queue's reference moves to this consumer */
    pthread_mutex_unlock(&g_lock);
    if (pick == 1) atomic_fetch_add_explicit(&g_dec_probe, 1, memory_order_relaxed);

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
    if (held < 0 && !(nr_pusch_passive_pipeline_enabled() ?
          nr_pusch_queue_snapshot_fresh(prod, job.absolute_slot, slots_per_frame) :
          nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame))) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    const bool pipeline = nr_pusch_passive_pipeline_enabled();
    nr_pusch_passive_harq_arm(!nr_ul_layout_is_probe(&job.grant)); /* first decode of this job may combine */
    struct timespec grant_cpu_start={0},grant_cpu_end={0};
    if (nr_ul_chain_on()) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&grant_cpu_start);
    if (held >= 0) {
      nr_pusch_passive_decode_attach(idx, g_pool[held].iq, g_pool[held].origin, g_pool[held].len);
      atomic_fetch_add_explicit(&g_snap_used, 1, memory_order_relaxed);
    } else if (pipeline && !nr_pusch_passive_decode_capture(ue, idx, (uint8_t)job.nr_slot_rx,
                                                     (uint64_t)job.absolute_slot)) {
      atomic_fetch_add_explicit(&g_dropped_snapshot, 1, memory_order_relaxed);
      continue;
    }
    if (nr_ul_layout_is_probe(&job.grant)) {
      /* UL-LAYOUT probe (ISAC_UL_LAYOUT_LOCK=1): DM-RS evidence for every UL BWP hypothesis, never a decode. */
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
    if (nr_pusch_solver_enabled() && job.ul_ticket_valid && !job.cfr_only) {
      /* Review fix (REVIEW1 s2 defects 1+2): reset the solver once per broad invalidation
       * (HARD epoch / identity change / dedicated-change-suspected), not on every SOFT epoch
       * such as an RX-recovery restart, and only from the one consumer that wins the CAS. */
      static _Atomic uint64_t solver_gen;
      const uint64_t gen = ((uint64_t)nr_cfg_epoch_identity_gen() << 32) | nr_cfg_epoch_dedicated_current();
      uint64_t prev_gen = atomic_load_explicit(&solver_gen, memory_order_acquire);
      if (prev_gen != gen &&
          atomic_compare_exchange_strong_explicit(&solver_gen, &prev_gen, gen, memory_order_acq_rel,
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
                                               probe_out.dmrs_id,probe_out.data_id);
            }
          }
        }
      }
      const bool not_probe_class = nr_get_Qm_ul(job.grant.mcs, job.grant.mcs_table) != 2 || job.grant.num_rb > 48;
      if (!(ul_solver_fallthrough() && !ran_full && (not_probe_class || ul_solver_fallthrough() == 2))) {
        if (pipeline) nr_pusch_passive_decode_capture_release(idx);
        continue;
      }
      /* fall through to the ordinary full decode of the DCI's own reading */
    }
    nr_pusch_passive_out_t out;
    nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &job.grant,
                            job.ta_offset_samples, (uint64_t)job.absolute_slot, job.cfr_only, job.fo_hz, &out);
    if (ul_retry_enabled() && !job.cfr_only && out.status == NR_PUSCH_PASSIVE_CRC_FAIL && out.snr_db >= ul_retry_snr_db() &&
        job.grant.num_rb <= ul_retry_max_rb() && nr_pusch_passive_rescue_allowed()) {
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
      for (int a = 0; a < na; ++a) {
        nr_pusch_passive_out_t out2;
        nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &alts[a],
                                job.ta_offset_samples, (uint64_t)job.absolute_slot, false, job.fo_hz, &out2);
        if (out2.status == NR_PUSCH_PASSIVE_OK) {
          out = out2;
          LOG_A(PHY, "SENSING: UL_RETRY rnti=0x%04x CRC OK with cdm_groups=%u S=%u L=%u (reading said cdm=%u S=%u L=%u)\n",
                alts[a].rnti, alts[a].n_dmrs_cdm_groups, alts[a].start_symbol, alts[a].num_symbols,
                job.grant.n_dmrs_cdm_groups, job.grant.start_symbol, job.grant.num_symbols);
          break;
        }
      }
    }
    if (nr_ul_chain_on()) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&grant_cpu_end);
    if (pipeline) nr_pusch_passive_decode_capture_release(idx);
    if (!nr_cfg_epoch_work_current()) {
      continue;
    }
    if (nr_ul_layout_is_tagged(&job.grant) && (out.status==NR_PUSCH_PASSIVE_OK || out.status==NR_PUSCH_PASSIVE_CRC_FAIL))
      nr_ul_layout_crc_verdict(&job.grant, out.status==NR_PUSCH_PASSIVE_OK, out.tbs_bytes*8u, out.n_segments, out.segments_ok,
                               job.absolute_slot); /* no-op without ISAC_UL_LOCK_CRC_FIRST */
    if (nr_ul_layout_is_tagged(&job.grant) && out.G > 0 && (out.status==NR_PUSCH_PASSIVE_OK || out.status==NR_PUSCH_PASSIVE_CRC_FAIL))
      nr_ul_layout_prior_note(&job.grant, out.status==NR_PUSCH_PASSIVE_OK, out.tbs_bytes*8u, out.n_segments, out.segments_ok,
                              job.absolute_slot, out.snr_db, (float)out.tbs_bytes*8.f*(float)out.qam_mod_order/(float)out.G); /* no-op without ISAC_UL_LOCK_PRIOR_SEARCH */
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_OK) {
      nr_ue_ul_update_t confirmed=job.ul_update;
      confirmed.ta_samples=out.ta_applied_samples;
      confirmed.dmrs_id=out.dmrs_id;
      confirmed.data_id=out.data_id;
      confirmed.nscid=out.nscid;
      nr_ue_ctx_ul_feedback(&job.ul_ticket,&confirmed,true);
    }
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_OK)
      nr_pusch_passive_ul_ids_confirm(&job.ul_ticket,out.nscid,out.dmrs_id,out.data_id);
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_CRC_FAIL)
      nr_pusch_passive_ul_ids_failed(&job.ul_ticket);
    if(!job.cfr_only && (out.status==NR_PUSCH_PASSIVE_OK ||
        out.status==NR_PUSCH_PASSIVE_CRC_FAIL || out.status==NR_PUSCH_PASSIVE_ZERO_TB)) {
      if (!nr_ul_layout_is_tagged(&job.grant)) /* UL-LAYOUT grants belong to no width/U3 search */
        nr_pdcch_ul_discovery_feedback(&job.grant,out.status==NR_PUSCH_PASSIVE_OK);
      nr_pdcch_dci01_fdra_feedback(&job.grant, out.status==NR_PUSCH_PASSIVE_OK);
    }
    if (nr_ul_chain_on()) {
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

static int ulq_env(const char *k, int d)
{
  const char *v = getenv(k);
  return v ? atoi(v) : d;
}

static bool ulq_is_probe(const nr_pdcch_blind_ul_result_t *g)
{
  return nr_ul_layout_is_probe(g) || nr_ul_layout_is_trial(g);
}

/* ULQ capture thread (ISAC_UL_SNAP_POOL=1): copies each booked UL slot ONCE, oldest real grant first, into a free
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
  (void)arg;
  PHY_VARS_NR_UE *ue = g_ue;
  const uint32_t cap = nr_pusch_passive_snapshot_len(&ue->frame_parms);
  pthread_mutex_lock(&g_lock);
  while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
    long abs = -1;
    int slot = 0;
    for (int k = 0; k < g_cursor.count && abs < 0; k++) {
      const nr_pusch_passive_job_t *j = &g_ring[(g_cursor.tail + k) % g_cursor.depth];
      if (j->snap_idx == -1 && !ulq_is_probe(&j->grant)) {
        abs = j->absolute_slot;
        slot = j->nr_slot_rx;
      }
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
    pthread_mutex_unlock(&g_lock);
    uint32_t origin = 0, len = 0;
    const bool ok = nr_pusch_passive_snapshot_copy(ue, (uint8_t)slot, (uint64_t)abs, g_pool[b].iq, cap, &origin, &len);
    pthread_mutex_lock(&g_lock);
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
    n_consumers = ulq_env("ISAC_PUSCH_CONSUMERS", 1);
    if (n_consumers < 1) n_consumers = 1;
    if (n_consumers > NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS) n_consumers = NR_PUSCH_PASSIVE_QUEUE_MAX_CONSUMERS;
    for (int c = 0; c < n_consumers; c++)
      if (!nr_pusch_passive_decode_prewarminit(ue, c)) {
        if (c == 0) return false;
        n_consumers = c;
        break;
      }
    depth = ulq_env("ISAC_UL_QUEUE_DEPTH", depth);
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
  g_prio = ulq_env("ISAC_UL_PRIO_QUEUE", 0) == 1;
  int pdepth = ulq_env("ISAC_UL_PROBE_DEPTH", 8);
  if (pdepth < 2) pdepth = 2;
  if (pdepth > NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH) pdepth = NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH;
  g_pcursor = (nr_pusch_queue_cursor_t){.depth=pdepth};
  g_spare_rescue = ulq_env("ISAC_UL_SPARE_RESCUE", 0) == 1;
  g_budget_pct = ulq_env("ISAC_UL_CPU_BUDGET_PCT", 0);
  g_pool_n = 0;
  if (nr_pusch_passive_pipeline_enabled() && ulq_env("ISAC_UL_SNAP_POOL", 0) == 1) {
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
    char name[32];
    snprintf(name, sizeof(name), "passivePusch%d", i);
    /* Priority BELOW the RT receive thread's 97: if a consumer ever competes with the receive path
     * it must lose. Losing here costs one deferred decode; losing there costs time sync, which is
     * the whole problem this module exists to fix. */
    int core = nr_core_plan_cpu(NR_CP_PUSCH, i, (affinity >= 0) ? (affinity + i) : -1);
    int prio = 50;
    /* ISAC_PUSCH_CORES="17,18" / ISAC_PUSCH_PRIO=<0..49> (default off): the UL trial decodes (ISAC_UL_LOCK_CRC_FIRST) ran unpinned at
     * FIFO 50, above the PDSCH consumers (40), and inflated every PDSCH stage (DECQUEUE report). Pin them off the PDSCH cores, lower. */
    core = nr_env_list_nth("ISAC_PUSCH_CORES", i, core);
    if (getenv("ISAC_PUSCH_PRIO")) prio = atoi(getenv("ISAC_PUSCH_PRIO"));
    threadCreate(&g_threads[i], nr_pusch_passive_queue_thread, &g_args[i], name, core, prio);
  }
  if (g_pool_n > 0) {
    const int core = ulq_env("ISAC_UL_SNAP_CORE", nr_env_list_nth("ISAC_PUSCH_CORES", 0, -1));
    const int prio = ulq_env("ISAC_UL_SNAP_PRIO", (getenv("ISAC_PUSCH_PRIO") ? atoi(getenv("ISAC_PUSCH_PRIO")) : 50) + 1);
    threadCreate(&g_cap_thread, ulq_capture_thread, NULL, "passivePuschCap", core, prio);
  }
  LOG_I(PHY, "SENSING: passive PUSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest; ULQ prio=%d probe_depth=%d snap_pool=%d spare_rescue=%d budget_pct=%d\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1, g_prio, g_pcursor.depth, g_pool_n,
        g_spare_rescue, g_budget_pct);
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
  const bool probe = g_prio && ulq_is_probe(&job->grant);
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
  if (!g_prio) return 0;
  pthread_mutex_lock(&g_lock);
  const int n = g_cursor.count;
  pthread_mutex_unlock(&g_lock);
  return n;
}

bool nr_pusch_passive_rescue_allowed(void)
{
  if (!g_spare_rescue) return true;
  pthread_mutex_lock(&g_lock);
  const int n = g_cursor.count;
  pthread_mutex_unlock(&g_lock);
  if (n > 0) atomic_fetch_add_explicit(&g_rescue_skipped, 1, memory_order_relaxed);
  return n == 0;
}

void nr_pusch_passive_queue_ulq_dump(void)
{
  if (!g_prio && !g_pool_n && !g_spare_rescue && !g_budget_pct && g_nthreads <= 1) return;
  LOG_I(PHY, "SENSING: PUSCHQ2 consumers=%d real[queued=%lu full=%lu] probe[queued=%lu decoded=%lu full=%lu] "
             "snap[pool=%d ok=%lu fail=%lu nobuf=%lu used=%lu] rescue_skipped=%lu budget_sleep_ms=%lu\n",
        g_nthreads, (unsigned long)atomic_load(&g_q_real), (unsigned long)atomic_load(&g_dropped_full),
        (unsigned long)atomic_load(&g_q_probe), (unsigned long)atomic_load(&g_dec_probe),
        (unsigned long)atomic_load(&g_full_probe), g_pool_n, (unsigned long)atomic_load(&g_snap_ok),
        (unsigned long)atomic_load(&g_snap_fail), (unsigned long)atomic_load(&g_snap_nobuf),
        (unsigned long)atomic_load(&g_snap_used), (unsigned long)atomic_load(&g_rescue_skipped),
        (unsigned long)(atomic_load(&g_budget_sleep_us) / 1000));
}
