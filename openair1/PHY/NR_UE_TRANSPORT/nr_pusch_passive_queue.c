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

/* The RF producer's own position, published in executables/nr-ue.c immediately BEFORE nrue_ru_read()
 * fills that slot's region of rxdata. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

static nr_pusch_passive_job_t g_ring[NR_PUSCH_PASSIVE_QUEUE_MAX_DEPTH];
static nr_pusch_queue_cursor_t g_cursor;

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

  while (1) {
    nr_pusch_passive_job_t job;

    pthread_mutex_lock(&g_lock);
    while (g_cursor.count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) == 0) {
      /* Timed wait so a stop cannot be missed if it lands between the predicate and the wait. */
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000; // 20 ms
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    }
    if (g_cursor.count == 0 && atomic_load_explicit(&g_stop, memory_order_relaxed) != 0) {
      pthread_mutex_unlock(&g_lock);
      break;
    }
    job = g_ring[nr_pusch_queue_pop(&g_cursor)];
    pthread_mutex_unlock(&g_lock);

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
    if (!(nr_pusch_passive_pipeline_enabled() ?
          nr_pusch_queue_snapshot_fresh(prod, job.absolute_slot, slots_per_frame) :
          nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame))) {
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    const bool pipeline = nr_pusch_passive_pipeline_enabled();
    struct timespec grant_cpu_start={0},grant_cpu_end={0};
    if (nr_ul_chain_on()) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&grant_cpu_start);
    if (pipeline && !nr_pusch_passive_decode_capture(ue, idx, (uint8_t)job.nr_slot_rx,
                                                     (uint64_t)job.absolute_slot)) {
      atomic_fetch_add_explicit(&g_dropped_snapshot, 1, memory_order_relaxed);
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
      const nr_pdcch_blind_monitor_cfg_t *cfg=nr_pdcch_blind_monitor_get_cfg();
      /* nr_pdcch_blind_ul_dmrs_mask takes the RRC enum (pos2=0, pos3=1),
       * not the corresponding OFDM symbol number (2 or 3). */
      const int l0=cfg && cfg->dmrs_typeA_position==1 ? 1 : 0;
      const int nv=nr_pusch_solver_plan(&job.ul_ticket,rd,nrd,
          (uint64_t)job.absolute_slot-job.grant.k2,(uint64_t)job.absolute_slot,
          l0,ids,nids,var,2);
      for (int k=0;k<nv;++k) {
        struct timespec p0,p1;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&p0);
        nr_pusch_passive_out_t probe_out;
        if (var[k].probe)
          nr_pusch_passive_decode_probe(ue,idx,(uint32_t)job.frame_rx,(uint8_t)job.nr_slot_rx,
                &var[k].grant,job.ta_offset_samples,(uint64_t)job.absolute_slot,job.fo_hz,&probe_out);
        else
          nr_pusch_passive_decode(ue,idx,(uint32_t)job.frame_rx,(uint8_t)job.nr_slot_rx,
                &var[k].grant,job.ta_offset_samples,(uint64_t)job.absolute_slot,false,job.fo_hz,&probe_out);
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
      if (pipeline) nr_pusch_passive_decode_capture_release(idx);
      continue;
    }
    nr_pusch_passive_out_t out;
    nr_pusch_passive_decode(ue, idx, (uint32_t)job.frame_rx, (uint8_t)job.nr_slot_rx, &job.grant,
                            job.ta_offset_samples, (uint64_t)job.absolute_slot, job.cfr_only, job.fo_hz, &out);
    if (nr_ul_chain_on()) clock_gettime(CLOCK_THREAD_CPUTIME_ID,&grant_cpu_end);
    if (pipeline) nr_pusch_passive_decode_capture_release(idx);
    if (!nr_cfg_epoch_work_current()) {
      continue;
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
      nr_pusch_passive_ul_ids_confirm(&job.ul_ticket,out.nscid,out.dmrs_id,out.data_id);
    if (job.ul_ticket_valid && out.status==NR_PUSCH_PASSIVE_CRC_FAIL)
      nr_pusch_passive_ul_ids_failed(&job.ul_ticket);
    if(!job.cfr_only && (out.status==NR_PUSCH_PASSIVE_OK ||
        out.status==NR_PUSCH_PASSIVE_CRC_FAIL || out.status==NR_PUSCH_PASSIVE_ZERO_TB)) {
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
    n_consumers = 1;
    if (!nr_pusch_passive_decode_prewarminit(ue)) return false;
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
  atomic_store_explicit(&g_stop, 0, memory_order_relaxed);
  atomic_store_explicit(&g_running, 1, memory_order_release);

  for (int i = 0; i < n_consumers; i++) {
    g_args[i].idx = i;
    char name[32];
    snprintf(name, sizeof(name), "passivePusch%d", i);
    /* Priority BELOW the RT receive thread's 97: if a consumer ever competes with the receive path
     * it must lose. Losing here costs one deferred decode; losing there costs time sync, which is
     * the whole problem this module exists to fix. */
    threadCreate(&g_threads[i], nr_pusch_passive_queue_thread, &g_args[i], name,
                 nr_core_plan_cpu(NR_CP_PUSCH, i, (affinity >= 0) ? (affinity + i) : -1), 50);
  }
  LOG_I(PHY, "SENSING: passive PUSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1);
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
  const int index=nr_pusch_queue_push(&g_cursor,&evicted);
  /* DROP-OLDEST: the evicted entry is closest to IQ overwrite. */
  if(evicted>=0) atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
  g_ring[index] = *job;
  g_ring[index].config_epoch = nr_passive_job_epoch_stamp(nr_cfg_reconf_enabled(), nr_cfg_epoch_work_stamp);
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_lock);
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
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
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_lock);
  for (int i = 0; i < g_nthreads; i++) {
    pthread_join(g_threads[i], NULL);
  }
  atomic_store_explicit(&g_running, 0, memory_order_release);
  pthread_mutex_unlock(&g_lifecycle_lock);
}
