#include "PHY/TOOLS/nr_gpu_budget.h"
#include "common/utils/nr_core_plan.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_replay_capture.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.h"
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
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_solver.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_tpool.h"         // T26b grant solver feedback
#include "PHY/NR_UE_TRANSPORT/nr_passive_mac_ta.h"    // MAC timing-advance parsers + this file's reporter
#include "PHY/NR_UE_TRANSPORT/nr_prach_passive.h"
void nr_passive_rrc_harvest(const uint8_t *tb, uint32_t tb_bytes); // openair2/LAYER2/NR_MAC_UE
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_xoverhead.h"   // reject-only xOverhead elimination by TB CRC
#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h"  // blind DM-RS scrambling-identity estimate
#include "PHY/NR_REFSIG/dmrs_nr.h"                     // get_num_dmrs_re_per_rb
#include "common/utils/nr/nr_common.h"                // get_num_dmrs
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h" // Technique D scoring
#include "PHY/NR_UE_TRANSPORT/nr_dci_history.h" // BC9 DL DCI history (certified-flag census, BC9d confirmed-DCI exclusions)
#include "PHY/NR_UE_TRANSPORT/nr_passive_acq_state.h" // BC9d: SIB1 TDD PDSCH last symbols
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.h" // Technique D Qm oracle
#include "PHY/NR_UE_TRANSPORT/nr_td_order.h" // BC12a SIB1 census
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h" // nr_pdcch_dci11_layout_feedback
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h" // nr_prb_segments (probe span of a PRB-list grant)
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h" // nr_pdsch_passive_alloc_normalise
#include "PHY/NR_UE_TRANSPORT/nr_passive_obs.h" // per-grant observation API (Task A3)
#include "PHY/NR_UE_TRANSPORT/nr_sense_records.h"
#include "PHY/NR_UE_TRANSPORT/nr_td_cb0_wire.h" // CB0 elimination channel (ISAC_TD_CB0_ELIM, default ON, 0 disables)
#include "nr_passive_cfg_epoch.h"
#include "nr_passive_job_epoch.h"
#include "nr_gpu_fep_hygiene.h"
#include "nr_pdsch_gpu_record.h"
#include "nr_pdsch_gpu_exact.h"
void nr_pdsch_gpu_record_cpu(PHY_VARS_NR_UE *ue, const nr_pdsch_passive_job_t *src,
                             const nr_pdsch_passive_decode_result_t *dec, const c16_t *rxdataF, uint32_t rxF_stride);
#include "nr_csirs_disc.h"

#include <limits.h>
#include <math.h>
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
#include "PHY/NR_UE_TRANSPORT/nr_transport_proto_ue.h" // nr_dlsch_unscrambling (GPU self-check)

/* T21: fixed-size consumer-owned IQ arenas, allocated before threads start. No
 * CUDA registration or allocation. Only a slot window is copied per group. */
static c16_t *g_owned_iq[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS][NB_ANTENNAS_RX];
static _Atomic uint64_t g_latency[NR_PDSCH_LAT_STAGES][NR_PDSCH_LAT_BINS];
static _Atomic uint64_t g_drop_reason[NR_PDSCH_DROP_REASONS];
/* latency_note: adds one sample (end - start, ns) to the queue-latency histogram of a pipeline stage; ignored when start is 0 or end < start.
 *   Lock-free atomic increment into g_latency[stage][bin]. Callers: gpu_fep_worker_thread(), nr_pdsch_passive_queue_thread() (queue wait, FEP).
 */
static void latency_note(int stage, uint64_t start, uint64_t end)
{
  if (start && end >= start)
    atomic_fetch_add_explicit(&g_latency[stage][nr_pdsch_latency_bin(end - start)], 1, memory_order_relaxed);
}

/* ---- GPU front end (NR_GPU_FEP=1): FEP + chest + MMSE + LLR for a whole slot group in one launch
 * set, LLRs handed to the decode through nr_pdsch_passive_set_llr_override(). ---- */
static const nr_gpu_fep_api_t * _Atomic g_gpu; /* NULL = CPU path; health gate can disable from a consumer */
static _Atomic uint64_t g_gpu_slots, g_gpu_jobs, g_gpu_fep_ns, g_gpu_llr_ns;
static _Atomic uint64_t g_gpu_chk_n, g_gpu_chk_crc_agree, g_gpu_chk_crc_gpu_ok, g_gpu_chk_crc_cpu_ok;
static _Atomic uint64_t g_gpu_stale_pre, g_gpu_stale_post, g_gpu_chk_fail, g_gpu_chk_skipped;
static _Atomic int g_gpu_disabled;
static _Atomic int g_gpu_chk_limit;
static _Atomic int g_gpu_chk_enforce;
static int (*g_gpu_admit)(const nr_gpu_pdsch_job_t *, int, int);
static int16_t *g_consumer_gpu_llr[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static int16_t *g_consumer_gpu_rxF[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
#define GPU_LLR_CAP (32u << 20) /* int16 per consumer: room for a full recorded multi-grant slot */

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
/* release_solver: drops a queued decode variant without decoding it: returns its pending solver entry with no credit, at once.
 *   Called when a job is evicted, refused or skipped, so the solver's FIFO does not keep it as a lost CRC. Caller: enqueue_one() and queue_thread().
 */
static void release_solver(const nr_pdsch_passive_job_t *j)
{
  if (j->solver_token) nr_pdsch_solver_feedback(j->solver_token, false, false);
}

/* ---- ONE dedicated GPU worker thread (2026-09-16), not "whichever consumer grabs the lock" -------
 * ROOT CAUSE of the live OTA drop spike (r4a_104200: 92 % dropped, GPU_FEP calls measured 13-31 ms
 * against the isolated 0.3-1.6 ms): cons6_ota.conf's `pdcch_blind_monitor_pdsch` pins the 6 passive-
 * PDSCH consumers to cores 6..11 (LOG line "cores 6..11") -- 2 of those (6,7) ARE `--thread-pool`
 * cores, and 4 (8-11) ARE the NIC's own dedicated MSI IRQ cores (preflight: "NIC irqs -> 8-13"), the
 * exact contention the 2026-09-02 NIC/softmodem core separation exists to prevent, now reintroduced
 * for this pool. Confirmed NOT a self-check or batching bug: ISAC_GPU_SELFCHECK never fired in that
 * run (0 GPU_SELFCHECK lines) and GPU_FEP slots==jobs (batch 1 throughout, as expected -- most
 * dequeues are single-slot, "slot_groups=37/641"). Reproduced clean (4 threads, real mutex, 3 ms of
 * CPU work/job, no core contention): p50 251 us / p99 273 us fep, 840 jobs/s aggregate -- mutex
 * contention ALONE does not explain it (scratchpad/gpu_contend_bench.cc).
 * MECHANISM: with the OLD design every one of the 6 consumers directly executed the blocking GPU
 * call (fep_slot+pdsch_llr) under one shared mutex. CPU-only decodes are fully independent, so a
 * consumer thread stalled on a contended core only slows itself; the GPU path serialises all 6
 * behind one lock, so ANY ONE of the 6 -- and on this conf 4/6 sit on the busiest IRQ cores in the
 * box -- stalling while it holds the lock stalls the other five. That is the drop-rate asymmetry.
 * FIX: one dedicated worker thread executes every GPU call; consumers hand off a request and block
 * on their OWN completion condvar (releasing the CPU, not spinning on the lock). This does not
 * raise the GPU's raw throughput ceiling (still one lane), but it shrinks the "who can stall
 * everyone" surface from 6 threads on 6 (partly contended) cores to exactly 1 thread on 1 core that
 * can be placed deliberately: core 3 by default -- `isolcpus=2,3` keeps the scheduler off it,
 * ISAC_UE_RT_CORE only pins the receive thread to core 2, and it is neither a `--thread-pool` core
 * nor a NIC IRQ core. ISAC_GPU_WORKER_CORE overrides; <0 = unpinned. */
typedef struct {
  nr_gpu_pdsch_job_t *jobs; int n_jobs;
  uint32_t config_epoch;
  long source_abs_slot, slots_per_frame;
  const int16_t *rx[4]; uint32_t ring_len, ring_off, abs_sample; double fo_hz; const int16_t *rot;
  int16_t *out; size_t out_cap;
  int16_t *rxdataF; int read_rc, ofdm_symbol_size;
  const nr_gpu_fep_api_t *api;
  uint64_t fep_end_ns, submit_ns;
  bool owned_iq;
  volatile int done; int64_t rc;
} gpu_fep_req_t;
#define GPU_WORKER_QUEUE_MAX 32
static gpu_fep_req_t *g_gpu_wq[GPU_WORKER_QUEUE_MAX];
static int g_gpu_wq_n;
static pthread_mutex_t g_gpu_wq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gpu_wq_work = PTHREAD_COND_INITIALIZER, g_gpu_wq_done = PTHREAD_COND_INITIALIZER;
static _Atomic int g_gpu_worker_running;
static pthread_t g_gpu_worker_thread;
extern _Atomic long nr_ue_diag_producer_absolute_slot;
/* This is the only call site for the GPU API. In particular, read_rxdataF must run before
 * the worker takes another slot and replaces the device-global resident rxdataF. */
static long gpu_req_producer(void *ctx)
{
  (void)ctx;
  return atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
}
static void gpu_req_stale(void *ctx, int stage)
{
  (void)ctx;
  atomic_fetch_add_explicit(stage == 0 ? &g_gpu_stale_pre : &g_gpu_stale_post, 1, memory_order_relaxed);
}
/* gpu_req_fep: GPU worker step 1: asks the GPU admission gate whether it can take the request, then runs the FEP for one slot on the device.
 *   Returns the fep_slot() return code (-1 when not admitted) and stamps r->fep_end_ns. Used directly for owned-IQ requests and as the FEP callback otherwise.
 *   Callers: gpu_fep_worker_thread(), gpu_fep_submit_and_wait().
 */
static int gpu_req_fep(void *ctx)
{
  gpu_fep_req_t *r = ctx;
  if (!g_gpu_admit || !g_gpu_admit(r->jobs, r->n_jobs, r->rxdataF != NULL)) return -1;
  const int rc = r->api->fep_slot(r->rx, r->ring_len, r->ring_off, r->abs_sample, r->fo_hz, r->rot);
  r->fep_end_ns = now_ns();
  return rc;
}
/* gpu_req_llr: GPU request callback run by the PDSCH queue: computes the LLRs for the jobs and reads back the rxdataF.
 *   How it works: Calls the GPU PDSCH LLR API. On success and when an rxdataF buffer is given, calls nr_pdsch_gpu_readback and keeps its result in read_rc.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:245, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:302.
 */
static int64_t gpu_req_llr(void *ctx)
{
  gpu_fep_req_t *r = ctx;
  const int64_t rc = r->api->pdsch_llr(r->jobs, r->n_jobs, r->out, r->out_cap);
  if (rc >= 0 && r->rxdataF)
    r->read_rc = nr_pdsch_gpu_readback(r->api, r->jobs, r->n_jobs, r->ofdm_symbol_size, r->rxdataF);
  return rc;
}

/* gpu_fep_worker_thread: dedicated GPU worker: takes batches of FEP+LLR requests from g_gpu_wq and runs them on the device, one request at a time.
 *   Waits on g_gpu_wq_work (20 ms timeout) while idle; a request whose config epoch is stale or whose API is gone completes with rc = -1.
 *   Completion: per-request done flag (with NR_CSIRS_DISC) or a batch-wide done flag, then broadcast on g_gpu_wq_done. Logs GPU_FEP stats every 1000 slots.
 *   Started by nr_pdsch_passive_gpu_warmup() on its own core (ISAC_GPU_WORKER_CORE, default 3). Stopped by nr_pdsch_passive_queue_stop().
 */
static void *gpu_fep_worker_thread(void *arg)
{
  (void)arg;
  LOG_I(PHY, "SENSING: GPU_FEP worker thread started\n");
  while (atomic_load_explicit(&g_gpu_worker_running, memory_order_relaxed)) {
    gpu_fep_req_t *take[GPU_WORKER_QUEUE_MAX];
    int nt;
    pthread_mutex_lock(&g_gpu_wq_lock);
    while (g_gpu_wq_n == 0 && atomic_load_explicit(&g_gpu_worker_running, memory_order_relaxed)) {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20 * 1000 * 1000;
      if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
      pthread_cond_timedwait(&g_gpu_wq_work, &g_gpu_wq_lock, &ts);
    }
    nt = g_gpu_wq_n;
    memcpy(take, g_gpu_wq, (size_t)nt * sizeof(*take));
    g_gpu_wq_n = 0;
    pthread_mutex_unlock(&g_gpu_wq_lock);
    for (int i = 0; i < nt; i++) {
      gpu_fep_req_t *r = take[i];
      r->api = atomic_load(&g_gpu);
      if (!r->api || nr_passive_job_epoch_old(nr_cfg_reconf_enabled(), r->config_epoch, nr_cfg_epoch_current)) {
        r->rc = -1;
        goto complete;
      }
      const uint64_t t0 = now_ns();
      if (r->owned_iq) latency_note(NR_PDSCH_LAT_GPU_WAIT, r->submit_ns, t0);
      /* Owned IQ passed the queue's pre/post-copy guards; device execution has
       * no remaining dependency on the live ring. Keep the borrowed API guarded. */
      int64_t rc = r->owned_iq ? (gpu_req_fep(r) == 0 ? gpu_req_llr(r) : -1)
          : nr_gpu_fep_run_checked(r->source_abs_slot, r->slots_per_frame, gpu_req_producer,
                                  gpu_req_stale, gpu_req_fep, gpu_req_llr, r);
      const uint64_t t1 = now_ns();
      r->rc = (atomic_load(&g_gpu) != r->api || nr_passive_job_epoch_old(nr_cfg_reconf_enabled(), r->config_epoch, nr_cfg_epoch_current)) ? -1 : rc;
      if (rc >= 0) {
        atomic_fetch_add(&g_gpu_slots, 1);
        atomic_fetch_add(&g_gpu_jobs, (uint64_t)r->n_jobs);
        atomic_fetch_add(&g_gpu_fep_ns, r->fep_end_ns - t0);
        atomic_fetch_add(&g_gpu_llr_ns, t1 - r->fep_end_ns);
        const uint64_t ns = atomic_load(&g_gpu_slots);
        if (ns == 50 || (ns % 1000) == 0)
          LOG_A(PHY, "SENSING: GPU_FEP slots=%lu jobs=%lu fep=%.0f us/slot chest+llr=%.0f us/slot (%.1f us/job) selfcheck n=%lu crc_agree=%lu gpu_ok=%lu cpu_ok=%lu\n",
                (unsigned long)ns, (unsigned long)atomic_load(&g_gpu_jobs), atomic_load(&g_gpu_fep_ns) / 1e3 / ns,
                atomic_load(&g_gpu_llr_ns) / 1e3 / ns, atomic_load(&g_gpu_llr_ns) / 1e3 / (double)atomic_load(&g_gpu_jobs),
                (unsigned long)atomic_load(&g_gpu_chk_n), (unsigned long)atomic_load(&g_gpu_chk_crc_agree),
                (unsigned long)atomic_load(&g_gpu_chk_crc_gpu_ok), (unsigned long)atomic_load(&g_gpu_chk_crc_cpu_ok));
      }
complete:
      /* Release completed requests immediately; another request's CUDA round trip
       * must not hold this consumer (or its snapshot) hostage. */
      if (nr_csirs_disc_enabled()) {
        pthread_mutex_lock(&g_gpu_wq_lock);
        r->done = 1;
        pthread_cond_broadcast(&g_gpu_wq_done);
        pthread_mutex_unlock(&g_gpu_wq_lock);
      }
    }
    pthread_mutex_lock(&g_gpu_wq_lock);
    if (!nr_csirs_disc_enabled())
      for (int i = 0; i < nt; i++) take[i]->done = 1;
    pthread_cond_broadcast(&g_gpu_wq_done);
    pthread_mutex_unlock(&g_gpu_wq_lock);
  }
  LOG_I(PHY, "SENSING: GPU_FEP worker thread exiting\n");
  return NULL;
}

/* gpu_fep_submit_and_wait: hands one FEP+LLR request to the GPU and blocks until it is done; returns the rc of the FEP/LLR call (-1 when refused).
 *   With the GPU budget gate (nr_gpu_budget_enabled) it runs the request directly under a nonblocking mutex; otherwise it queues it to the worker (bounded queue).
 *   Caller: queue_thread() on the GPU front-end path. Takes g_gpu_wq_lock while waiting on g_gpu_wq_done.
 */
static int64_t gpu_fep_submit_and_wait(gpu_fep_req_t *req)
{
  if(nr_gpu_budget_enabled()) {
    /* A stack-owned request cannot be abandoned on the old worker queue: the
     * worker could still write its output after CPU fallback. Run under one
     * nonblocking gate instead; CUDA modules quarantine their private buffers
     * on timeout. One deadline covers FEP, LLR and optional self-check readback. */
    static pthread_mutex_t direct=PTHREAD_MUTEX_INITIALIZER;
    if(pthread_mutex_trylock(&direct)) {nr_gpu_budget_rt_result(0,1);return -1;}
    req->submit_ns=now_ns();
    nr_gpu_budget_rt_job_start(req->submit_ns/1000);
    int64_t rc=-1;
    if(nr_gpu_budget_rt_begin()) {
      req->api=atomic_load(&g_gpu);
      if(req->api && !nr_passive_job_epoch_old(nr_cfg_reconf_enabled(),req->config_epoch,nr_cfg_epoch_current)) {
        rc=req->owned_iq ? (gpu_req_fep(req)==0?gpu_req_llr(req):-1)
          : nr_gpu_fep_run_checked(req->source_abs_slot,req->slots_per_frame,gpu_req_producer,
                                  gpu_req_stale,gpu_req_fep,gpu_req_llr,req);
        if(nr_passive_job_epoch_old(nr_cfg_reconf_enabled(),req->config_epoch,nr_cfg_epoch_current))rc=-1;
        if(rc>=0) {
          atomic_fetch_add(&g_gpu_slots,1);atomic_fetch_add(&g_gpu_jobs,req->n_jobs);
          atomic_fetch_add(&g_gpu_fep_ns,req->fep_end_ns-req->submit_ns);
          atomic_fetch_add(&g_gpu_llr_ns,now_ns()-req->fep_end_ns);
        }
      }
      nr_gpu_budget_rt_end();
    }
    nr_gpu_budget_rt_job_start(0);
    pthread_mutex_unlock(&direct);return rc;
  }
  req->done = 0;
  req->submit_ns = req->owned_iq ? now_ns() : 0;
  pthread_mutex_lock(&g_gpu_wq_lock);
  if (!atomic_load(&g_gpu_worker_running) || g_gpu_wq_n >= GPU_WORKER_QUEUE_MAX) {
    /* worker is badly backed up -- do not grow unbounded; caller falls back to CPU for this group */
    pthread_mutex_unlock(&g_gpu_wq_lock);
    return -1;
  }
  g_gpu_wq[g_gpu_wq_n++] = req;
  pthread_cond_signal(&g_gpu_wq_work);
  while (!req->done)
    pthread_cond_wait(&g_gpu_wq_done, &g_gpu_wq_lock);
  pthread_mutex_unlock(&g_gpu_wq_lock);
  return req->rc;
}

/* gpu_dump_record: ISAC_GPU_DUMP=<file>: appends one record per self-checked GPU job to the dump file (slot IQ window, job, CPU and GPU LLRs) for offline comparison.
 *   Opens the file once (path cached in a static). Writes nothing when the env var is unset. Caller: queue_thread() after the GPU self-check.
 */
static void gpu_dump_record(PHY_VARS_NR_UE *ue, const nr_pdsch_passive_job_t *job, const nr_gpu_pdsch_job_t *gj,
                            const int16_t *llr_cpu, const int16_t *llr_gpu, uint32_t G)
{
  static const char *path; static int tried;
  if (!tried) { tried = 1; path = getenv("ISAC_GPU_DUMP"); }
  if (!path) return;
  static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&lk);
  FILE *f = fopen(path, "ab");
  if (f) {
    const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
    const uint32_t ring_len = 2 * fp->samples_per_frame, off = get_samples_slot_timestamp(fp, job->nr_slot_rx);
    const uint32_t win = fp->samples_per_slot_wCP + fp->ofdm_symbol_size;
    const uint32_t hdr[8] = {0x47505544u /* "GPUD" */, (uint32_t)job->absolute_slot, (uint32_t)fp->nb_antennas_rx, win,
                             G, (uint32_t)job->layout_probe, (uint32_t)sizeof(*gj), (uint32_t)(job->fo_hz * 1000)};
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(gj, sizeof(*gj), 1, f);
    for (int a = 0; a < fp->nb_antennas_rx; a++)
      for (uint32_t i = 0; i < win; i++)
        fwrite(&nr_pdsch_passive_iq_for_slot(job->nr_slot_rx, ue->common_vars.rxdata)[a][(off + i) % ring_len], sizeof(c16_t), 1, f);
    fwrite(llr_cpu, sizeof(int16_t), G, f);
    fwrite(llr_gpu, sizeof(int16_t), G, f);
    fclose(f);
  }
  pthread_mutex_unlock(&lk);
}

/* The RF producer's completed position, published in executables/nr-ue.c after a
 * successful nrue_ru_read(). The two-slot lifetime margin also covers prefetched IQ. */
extern _Atomic long nr_ue_diag_producer_absolute_slot;

static nr_pdsch_passive_job_t g_ring[NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH];
static uint64_t g_ring_ns[NR_PDSCH_PASSIVE_QUEUE_MAX_DEPTH];
static int  g_depth = 0;
static int  g_head  = 0; // next write slot
static int  g_tail  = 0; // next read slot
static int  g_count = 0; // entries currently held

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv   = PTHREAD_COND_INITIALIZER;

static _Atomic uint64_t g_queued        = 0;
static _Atomic uint64_t g_slot_groups   = 0; ///< dequeues that took >1 grant of one slot
#define NR_PDSCH_PASSIVE_SLOT_GROUP_MAX 64
void nr_pdsch_passive_set_slot_share(int on, int rb_lo, int rb_n);
void nr_pdcch_bwp_probe_result(int entry, nr_dci_bits_t payload, const float *prb_coh); /* nr_pdcch_blind_monitor_rt.c */
void nr_pdcch_bwp_crc_result(int entry, bool crc_ok);
#include "nr_dmrs_id_estimate.h"
#include "nr_pdsch_dmrs_scan.h"
/* TIM-PDSCH ISAC_PDSCH_DMRS_ID_SCAN=<scans per second of signal time> (default off; 1 = 8/s): blind per-UE PDSCH DM-RS
 * identity scan over 0..65535 (nr_pdsch_dmrs_scan.h) on the DM-RS symbol of slots carrying an unsolved dedicated
 * grant; the identities found are voted for every C-RNTI whose PDSCH is in that slot. A slot is scanned once (cache). */
static int dmrs_scan_rate(void)
{
  static _Atomic int r = -1;
  int v = atomic_load(&r);
  if (v < 0) {
    const char *e = getenv("ISAC_PDSCH_DMRS_ID_SCAN");
    v = e ? atoi(e) : 0;
    if (v == 1) v = 8;
    if (v < 0) v = 0;
    atomic_store(&r, v);
  }
  return v;
}
typedef struct { int64_t abs; int n; nr_dmrs_scan_hit_t h[NR_DMRS_SCAN_MAX_HITS]; } dmrs_scan_slot_t;
static dmrs_scan_slot_t g_scan_cache[32];
static int64_t g_scan_last_abs = -1000000;
static uint64_t g_scan_n, g_scan_hits, g_scan_us, g_pool_n, g_pool_us;
static pthread_mutex_t g_scan_mu = PTHREAD_MUTEX_INITIALIZER;
/* vote, and log the RNTI's ranked identities whenever they change (the per-scan lines are rate limited) */
static void dmrs_scan_vote_log(uint16_t rnti, int64_t abs_slot, const nr_dmrs_scan_hit_t *h, int n)
{
  int a[2], b[2];
  const int na = nr_dmrs_scan_rnti_ids(rnti, a, 2);
  nr_dmrs_scan_vote(rnti, abs_slot, h, n);
  const int nb = nr_dmrs_scan_rnti_ids(rnti, b, 2);
  if (nb != na || (nb > 0 && a[0] != b[0]) || (nb > 1 && a[1] != b[1]))
    LOG_A(PHY, "SENSING: DMRS_SCAN ids rnti=0x%04x abs=%ld ids=%d (%d %d) slots=%d\n", rnti, (long)abs_slot, nb,
          nb > 0 ? b[0] : -1, nb > 1 ? b[1] : -1, nr_dmrs_scan_rnti_slots(rnti));
}
/* ISAC_PDSCH_DMRS_SCAN_ASYNC=1 (default off, DECQUEUE): a full scan costs ~65 ms and ran inline on a PDSCH consumer, i.e. one of the
 * decode cores was gone for 16 scans/s. With the flag the scan job (a copy of the DM-RS symbol) goes to ONE worker thread, pinned to
 * ISAC_DMRS_SCAN_CORE and run at SCHED_OTHER; the consumer returns at once. The votes then arrive a few slots later. A full queue drops the job. */
#define DSCAN_Q 4
#define DSCAN_MAXN 8192
typedef struct {
  uint16_t rnti; int64_t abs_slot; int nr_slot, sym, nant, N, fco, n_rb, sps; bool full;
  int16_t *buf; /* nant * N * 2 */
} dscan_job_t;
static dscan_job_t g_dq[DSCAN_Q];
static int g_dq_head, g_dq_n;
static bool g_dq_started;
static pthread_mutex_t g_dq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_dq_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_dq_thr;
static _Atomic uint64_t g_dscan_dropped;
static bool dmrs_scan_async(void)
{
  static _Atomic int v = -1;
  int r = atomic_load(&v);
  if (r < 0) { const char *e = getenv("ISAC_PDSCH_DMRS_SCAN_ASYNC"); r = e && atoi(e) != 0; atomic_store(&v, r); }
  return r;
}
/* dmrs_scan_run: the scan itself (cache fill, votes, log); rx = the DM-RS symbol per antenna. */
static void dmrs_scan_run(dmrs_scan_slot_t *c, uint16_t rnti, int64_t abs_slot, int nr_slot, int sym, bool full, int nant, int N, int fco,
                          int n_rb, int sps, const int16_t *const *rx)
{
  int ids[2];
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  nr_dmrs_scan_hit_t h[NR_DMRS_SCAN_MAX_HITS];
  const int n = (full ? nr_dmrs_scan_slot : nr_dmrs_scan_slot_pool)(rx, nant, N, fco, n_rb, nr_slot, sym, sps, 6, h, NR_DMRS_SCAN_MAX_HITS);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  pthread_mutex_lock(&g_scan_mu);
  if (c->abs == abs_slot) { c->n = n; memcpy(c->h, h, sizeof(h)); }
  const uint64_t us = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
  if (full) { g_scan_n++; g_scan_hits += (uint64_t)n; g_scan_us += us; }
  else { g_pool_n++; g_pool_us += us; }
  const uint64_t sn = g_scan_n, sh = g_scan_hits, su = g_scan_us, pn = g_pool_n, pu = g_pool_us;
  pthread_mutex_unlock(&g_scan_mu);
  dmrs_scan_vote_log(rnti, abs_slot, h, n);
  if (!full && (n == 0 || (pn % 50) != 1)) return; /* pool checks: log 1 in 50 */
  char b[256] = "";
  int u = 0;
  for (int i = 0; i < n && u < (int)sizeof(b) - 40; i++)
    u += snprintf(b + u, sizeof(b) - u, " %d/%d@%d+%d:%.2f(%.2f)", h[i].nid, h[i].nscid, h[i].rb0, h[i].nrb, h[i].score, h[i].second);
  const int nd = nr_dmrs_scan_rnti_ids(rnti, ids, 2);
  LOG_A(PHY, "SENSING: DMRS_SCAN %s abs=%ld rnti=0x%04x hits=%d%s | rnti ids=%d (%d %d) slots=%d | full scans=%lu hits=%lu "
             "mean=%lu us | pool checks=%lu mean=%lu us\n",
        full ? "full" : "pool", (long)abs_slot, rnti, n, b, nd, nd > 0 ? ids[0] : -1, nd > 1 ? ids[1] : -1,
        nr_dmrs_scan_rnti_slots(rnti), (unsigned long)sn, (unsigned long)sh, (unsigned long)(sn ? su / sn : 0),
        (unsigned long)pn, (unsigned long)(pn ? pu / pn : 0));
}
static void *dmrs_scan_worker(void *arg)
{
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&g_dq_mu);
    while (g_dq_n == 0) pthread_cond_wait(&g_dq_cv, &g_dq_mu);
    dscan_job_t j = g_dq[g_dq_head];
    pthread_mutex_unlock(&g_dq_mu);
    const int16_t *rx[8];
    for (int a = 0; a < j.nant; a++) rx[a] = j.buf + (size_t)a * j.N * 2;
    pthread_mutex_lock(&g_scan_mu);
    dmrs_scan_slot_t *c = &g_scan_cache[j.abs_slot & 31];
    pthread_mutex_unlock(&g_scan_mu);
    dmrs_scan_run(c, j.rnti, j.abs_slot, j.nr_slot, j.sym, j.full, j.nant, j.N, j.fco, j.n_rb, j.sps, rx);
    pthread_mutex_lock(&g_dq_mu); /* the slot stays ours until the scan is done: the producer cannot overwrite the copy */
    g_dq_head = (g_dq_head + 1) % DSCAN_Q;
    g_dq_n--;
    pthread_mutex_unlock(&g_dq_mu);
  }
  return NULL;
}
/* dmrs_scan_submit: copy the symbol into the worker ring. false = not queued (ring full, size out of range, no memory): caller runs inline or drops. */
static bool dmrs_scan_submit(uint16_t rnti, int64_t abs_slot, int nr_slot, int sym, bool full, int nant, int N, int fco, int n_rb, int sps,
                             const int16_t *const *rx)
{
  if (nant <= 0 || nant > 8 || N <= 0 || N > DSCAN_MAXN) return false;
  pthread_mutex_lock(&g_dq_mu);
  if (!g_dq_started) {
    for (int i = 0; i < DSCAN_Q; i++) g_dq[i].buf = malloc((size_t)8 * DSCAN_MAXN * 2 * sizeof(int16_t));
    for (int i = 0; i < DSCAN_Q; i++) if (!g_dq[i].buf) { pthread_mutex_unlock(&g_dq_mu); return false; }
    threadCreate(&g_dq_thr, dmrs_scan_worker, NULL, "dmrsScan", nr_env_list_nth("ISAC_DMRS_SCAN_CORE", 0, -1), 0);
    g_dq_started = true;
  }
  if (g_dq_n >= DSCAN_Q) { pthread_mutex_unlock(&g_dq_mu); atomic_fetch_add(&g_dscan_dropped, 1); return true; }
  const int idx = (g_dq_head + g_dq_n) % DSCAN_Q;
  dscan_job_t *j = &g_dq[idx];
  for (int a = 0; a < nant; a++) memcpy(j->buf + (size_t)a * N * 2, rx[a], (size_t)N * 2 * sizeof(int16_t));
  j->rnti = rnti; j->abs_slot = abs_slot; j->nr_slot = nr_slot; j->sym = sym; j->full = full; j->nant = nant; j->N = N; j->fco = fco;
  j->n_rb = n_rb; j->sps = sps;
  g_dq_n++;
  pthread_cond_signal(&g_dq_cv);
  pthread_mutex_unlock(&g_dq_mu);
  return true;
}
/* dmrs_scan_job: scan (rate-limited, once per slot) and vote for this job's RNTI. rx = the DM-RS symbol per antenna. */
static void dmrs_scan_job(const NR_DL_FRAME_PARMS *fp, uint16_t rnti, int64_t abs_slot, int nr_slot, int sym,
                          const int16_t *const *rx)
{
  const int rate = dmrs_scan_rate();
  if (rate <= 0 || abs_slot < 0) return;
  int ids[2];
  if (nr_pdsch_solver_rep(rnti) >= 0) return; /* solved: nothing left to learn */
  /* decided: no more full scans for it, but the cached/pool results keep voting, so an early wrong lead can be overtaken */
  const bool decided = nr_dmrs_scan_rnti_ids(rnti, ids, 2) >= 1 && nr_dmrs_scan_rnti_slots(rnti) >= 4;
  pthread_mutex_lock(&g_scan_mu);
  dmrs_scan_slot_t *c = &g_scan_cache[abs_slot & 31];
  if (c->abs == abs_slot) {
    dmrs_scan_slot_t copy = *c;
    pthread_mutex_unlock(&g_scan_mu);
    if (copy.n >= 0) dmrs_scan_vote_log(rnti, abs_slot, copy.h, copy.n);
    return;
  }
  /* a full 16-bit scan at most `rate` times per second of signal time; between them, only the identities full scans
   * already found (cell-wide pool) are re-tested, which costs a few ms and lets every slot of the RNTI vote */
  const int64_t gap = (int64_t)(1000 * fp->slots_per_subframe) / rate;
  const bool full = !decided && abs_slot - g_scan_last_abs >= gap;
  if (full) g_scan_last_abs = abs_slot;
  c->abs = abs_slot; c->n = -1; /* in progress: concurrent consumers of this slot do not vote */
  pthread_mutex_unlock(&g_scan_mu);
  if (dmrs_scan_async() && dmrs_scan_submit(rnti, abs_slot, nr_slot, sym, full, fp->nb_antennas_rx, fp->ofdm_symbol_size,
                                            fp->first_carrier_offset, fp->N_RB_DL, fp->symbols_per_slot, rx))
    return;
  dmrs_scan_run(c, rnti, abs_slot, nr_slot, sym, full, fp->nb_antennas_rx, fp->ofdm_symbol_size, fp->first_carrier_offset, fp->N_RB_DL,
                fp->symbols_per_slot, rx);
}
#include "PHY/MODULATION/modulation_UE.h" /* nr_slot_fep */
static _Atomic uint64_t g_decoded       = 0;
static _Atomic uint64_t g_crc_ok        = 0;
/* DL DM-RS identity estimate: one process-wide accumulator shared by every consumer, so evidence
 * from all of them adds. The 1024-candidate sweep runs under this lock on the consumer that
 * happens to hold it -- off the RT thread by construction, since this file IS the consumer. */
/* One state per nSCID (TS 38.211 scramblingID0 vs scramblingID1 are independent RRC fields, so a
 * cell that sets them differently needs two separate estimates -- a single shared state would
 * average two different true identities into neither). */
#define NR_DL_DMRS_NSCID 2
/* Two windows per nSCID, stage 1 always on, stage 2 throttled and capped: nr_dmrs_id_2stage_t
 * (final review I5; measured 170360 us per stage-2 accumulate vs ~2 ms for stage 1). */
/* T26c ISAC_PDSCH_DMRS_ID_PER_CORESET: one blind DM-RS identity estimator per discovered CORESET population */
typedef struct {
  int key, decided, top[3];
  bool used;
  pthread_mutex_t mu;
  nr_dmrs_id_2stage_t est;
  uint64_t last_slot;
  uint32_t failed_slots;
} coreset_dmrs_t;
static coreset_dmrs_t g_cs_dmrs[8];
static pthread_mutex_t g_cs_dmrs_mu = PTHREAD_MUTEX_INITIALIZER;
/* coreset_dmrs_refresh: returns ISAC_PDSCH_DMRS_ID_REFRESH (cached per thread, default 0): how often the per-CORESET DM-RS identity is re-estimated.
 *   Nonzero also makes the coreset_dmrs table readers take its mutex. Callers: nr_pdsch_passive_coreset_dmrs_top(), queue_thread().
 */
static int coreset_dmrs_refresh(void)
{
  static _Thread_local int n = -1;
  if (n < 0) {
    const char *e = getenv("ISAC_PDSCH_DMRS_ID_REFRESH");
    n = e ? atoi(e) : 0;
    if (n < 0) n = 0;
  }
  return n;
}
/* per_coreset_dmrs_on: returns ISAC_PDSCH_DMRS_ID_PER_CORESET (cached): 1 keeps a separate DM-RS identity estimate for each CORESET key.
 *   Off by default; when off the shared per-grant estimate is used. Caller: queue_thread() (after the solver feedback).
 */
static bool per_coreset_dmrs_on(void)
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_PDSCH_DMRS_ID_PER_CORESET") && atoi(getenv("ISAC_PDSCH_DMRS_ID_PER_CORESET")) ? 1 : 0;
  return en == 1;
}
/* coreset_dmrs_slot: finds or allocates the DM-RS identity estimator of one CORESET key (8 slots, under g_cs_dmrs_mu) and initialises it with the PCI.
 *   The estimator is a two-stage NID search restricted to stage 1 (s1_grants = UINT32_MAX). Returns NULL when all 8 slots are used.
 *   Caller: queue_thread() (per-CORESET DM-RS path).
 */
static coreset_dmrs_t *coreset_dmrs_slot(int key, int pci)
{
  pthread_mutex_lock(&g_cs_dmrs_mu);
  coreset_dmrs_t *r = NULL;
  for (int i = 0; i < 8 && !r; i++)
    if (g_cs_dmrs[i].used && g_cs_dmrs[i].key == key) r = &g_cs_dmrs[i];
  for (int i = 0; i < 8 && !r; i++)
    if (!g_cs_dmrs[i].used) {
      r = &g_cs_dmrs[i];
      r->used = true; r->key = key; r->decided = -1; r->top[0] = r->top[1] = r->top[2] = -1;
      pthread_mutex_init(&r->mu, NULL);
      char label[32];
      snprintf(label, sizeof(label), "PDSCH-cs%d", key);
      nr_dmrs_id_2stage_init(&r->est, strdup(label), pci);
      r->est.s1_grants = UINT32_MAX; /* stage 1 (0..1023: every PCI) only -- a stage-2 evaluation stalls a consumer ~170 ms */
    }
  pthread_mutex_unlock(&g_cs_dmrs_mu);
  return r;
}
/* nr_pdsch_passive_coreset_dmrs_top: returns the three best DM-RS identity candidates of a CORESET (out3[0..2]) once that CORESET has a decided identity; returns 3, else 0.
 *   Reads the table under the per-entry mutex only when ISAC_PDSCH_DMRS_ID_REFRESH is set. Caller: nr_pdcch_blind_monitor_rt.c (scrambling-id hypotheses).
 */
int nr_pdsch_passive_coreset_dmrs_top(int key, int *out3)
{
  for (int i = 0; i < 8; i++)
    if (g_cs_dmrs[i].used && g_cs_dmrs[i].key == key && __atomic_load_n(&g_cs_dmrs[i].decided, __ATOMIC_ACQUIRE) >= 0) {
      if (coreset_dmrs_refresh()) pthread_mutex_lock(&g_cs_dmrs[i].mu);
      for (int q = 0; q < 3; q++) out3[q] = g_cs_dmrs[i].top[q];
      if (coreset_dmrs_refresh()) pthread_mutex_unlock(&g_cs_dmrs[i].mu);
      return 3;
    }
  return 0;
}
/* nr_pdsch_passive_coreset_dmrs_id: returns the decided DM-RS identity of a CORESET key, or -1 when none has been decided yet (or the key is unknown).
 *   Lock-free acquire load of the decided field. Caller: nr_pdcch_blind_monitor_rt.c (scrambling id of the CORESET).
 */
int nr_pdsch_passive_coreset_dmrs_id(int key)
{
  for (int i = 0; i < 8; i++)
    if (g_cs_dmrs[i].used && g_cs_dmrs[i].key == key) return __atomic_load_n(&g_cs_dmrs[i].decided, __ATOMIC_ACQUIRE);
  return -1;
}
static nr_dmrs_id_2stage_t g_dl_dmrs_id[NR_DL_DMRS_NSCID];
static bool g_dl_dmrs_id_init[NR_DL_DMRS_NSCID];
static pthread_mutex_t g_dl_dmrs_id_lock[NR_DL_DMRS_NSCID] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
const nr_dmrs_id_2stage_t *nr_pdsch_passive_dl_dmrs_id(int nscid) { return &g_dl_dmrs_id[nscid & 1]; }
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
static _Atomic uint64_t g_stale_after_decode = 0; // K33
void nr_pdsch_passive_note_stale_after_decode(void)
{
  atomic_fetch_add_explicit(&g_stale_after_decode, 1, memory_order_relaxed);
}
static _Atomic uint64_t g_dropped_epoch = 0;
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
/* GrantWork-lite (ISAC_TD_GRANTWORK=1): g_rxdataF[i] is the FEP buffer the grant's GrantWork borrows; its
 * generation moves on every slot group, so a gw that outlives its group can no longer compute on it. */
static uint64_t g_fep_gen[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];
static bool g_rxdataF_gw; /* g_rxdataF[] came from nr_td_gw_alloc (ISAC_TD_GRANTWORK=1 only: flag off = unchanged) */

typedef struct {
  int idx;
} consumer_arg_t;
static consumer_arg_t g_args[NR_PDSCH_PASSIVE_QUEUE_MAX_CONSUMERS];


/* Timing advance out of an overheard, CRC-verified MAC PDU. The parsing itself is pure and lives in
 * nr_passive_mac_ta.c; this is only the reporting half, kept here so that file needs no log.h and
 * can be unit-tested standalone. A RAR gives an ABSOLUTE range to the UE it answers; a TA Command CE
 * gives a range delta for an already-connected one. Non-static and declared in nr_passive_mac_ta.h:
 * the in-line decode path in nr_pdcch_blind_monitor_rt.c reports through this same function, so the
 * two paths cannot drift apart in what they log. */
/* TC-RNTIs harvested from CRC-verified RARs, with the slot they were issued. A Msg4 addressed to one
 * of these is the ONLY DCI in CORESET#0 whose RNTI we know in advance, so its CRC match is exact
 * evidence and needs none of the noise gates (mismatch/persistence) that exist for guessed RNTIs.
 * Small ring, no eviction policy: a TC-RNTI is live for one RA procedure (~tens of ms), 32 entries
 * cover minutes at this cell's RAR rate. */
#define RAR_TC_RING 32
static struct { uint16_t rnti; uint32_t abs_slot; } g_rar_tc[RAR_TC_RING];
static unsigned g_rar_tc_head;
static uint32_t g_rar_tc_wrap;   /* slots per SFN cycle = 1024 * slots_per_frame, set on first RAR */
static pthread_mutex_t g_rar_tc_lock = PTHREAD_MUTEX_INITIALIZER;
/* CLOCK BUG, found before it produced a conclusion (2026-09-21): the ring used to be stamped with
 * job.absolute_slot -- the PRODUCER'S MONOTONIC counter -- while the PDCCH accept path asks with
 * frame_rx * slots_per_frame + nr_slot_rx. Two clocks with different origins, so the window test
 * was never true and a real Msg4 could not have been flagged however many arrived. Both sides now
 * use the FRAME-DERIVED slot, which wraps at SFN 1024, hence the modular age below. */
bool nr_passive_rar_tc_seen(uint16_t rnti, uint32_t now_abs_slot, uint32_t window_slots, uint32_t *age_out)
{
  bool hit = false;
  pthread_mutex_lock(&g_rar_tc_lock);
  for (int i = 0; i < RAR_TC_RING; i++) {
    if (g_rar_tc[i].rnti != rnti || rnti == 0 || g_rar_tc_wrap == 0) {
      continue;
    }
    const uint32_t age = (now_abs_slot >= g_rar_tc[i].abs_slot)
                             ? (now_abs_slot - g_rar_tc[i].abs_slot)
                             : (now_abs_slot + g_rar_tc_wrap - g_rar_tc[i].abs_slot);
    if (age <= window_slots) {
      if (age_out) *age_out = age;
      hit = true;
      break;
    }
  }
  pthread_mutex_unlock(&g_rar_tc_lock);
  return hit;
}

void nr_passive_mac_report_ta(uint16_t rnti, bool is_ra_rnti, int frame, int slot, int mu,
                              uint32_t abs_slot, const uint8_t *tb, uint32_t tb_bytes)
{
  if (!nr_cfg_epoch_work_current()) return;
  if (tb == NULL || tb_bytes == 0)
    return;
  if (is_ra_rnti) {
    uint8_t rapid = 0;
    uint16_t ta = 0, tc_rnti = 0;
    if (nr_passive_mac_rar_ta(tb, tb_bytes, &rapid, &ta, &tc_rnti)) {
      /* Use the decoded PDSCH slot in both inline and deferred paths: the
       * caller's abs_slot can still name the scheduling DCI when k0 > 0. */
      const uint64_t rar_slot = (uint64_t)frame * (10u << mu) + slot;
      nr_prach_passive_rar_pdu(rnti, rar_slot, tb, tb_bytes);
      LOG_A(PHY, "SENSING: MAC_TA RAR (%d.%d) ra_rnti=0x%x rapid=%u ta=%u -> %.1f m one-way, tc_rnti=0x%x\n",
            frame, slot, rnti, rapid, ta, nr_passive_mac_ta_metres(ta, mu), tc_rnti);
      /* THE SEED THE DEDICATED SEARCH NEEDS. Every other RNTI this receiver sees is a guess: the
       * CORESET search tests 271 unverified mapping hypotheses, so its accepts are random CRC
       * passes at random CCE locations (measured: ~20 RNTIs uniform over the 16-bit space, none
       * ever recurring). This one is not a guess. The RA-RNTI that scheduled this RAR is COMPUTED
       * from the PRACH occasion, the DCI carried 16 spec-fixed zero reserved bits, and the transport
       * block then passed its own CRC -- three independent checks. The TC-RNTI inside becomes that
       * UE's C-RNTI, so it will recur in the dedicated search space and can confirm.
       *
       * Recorded as TC: nr_pdcch_blind_rnti_bootstrap_record() admits C and TC only, and the two
       * share one hypothesis downstream. A zero TC-RNTI is not a UE and is dropped by the parser's
       * own field checks, but guard anyway -- this feeds a table the whole dedicated path keys on. */
      if (tc_rnti != 0) {
        /* TRUSTED: live at one sighting. See this file's header note and the RAR chain above --
         * three independent checks passed before this line, and a TC-RNTI cannot repeat. */
        nr_pdcch_blind_rnti_bootstrap_record_verified(tc_rnti, NR_BLIND_RNTI_CLASS_TC, abs_slot);
        if (nr_ue_ctx_enabled()) nr_ue_ctx_on_anchor(tc_rnti, NR_BLIND_RNTI_CLASS_TC, abs_slot);
        nr_ue_ctx_ul_ta_command(tc_rnti, ta, mu, true);
        /* FRAME-DERIVED, to match what the accept path asks with. slots_per_frame = 10 << mu,
         * never a hardcoded 10 (valid only at 15 kHz, and it has broken a slow-time axis here
         * before). */
        const uint32_t spf = 10u << (unsigned)mu;
        pthread_mutex_lock(&g_rar_tc_lock);
        g_rar_tc_wrap = 1024u * spf;
        g_rar_tc[g_rar_tc_head % RAR_TC_RING] =
            (typeof(g_rar_tc[0])){tc_rnti, (uint32_t)frame * spf + (uint32_t)slot};
        g_rar_tc_head++;
        pthread_mutex_unlock(&g_rar_tc_lock);
      }
    }
    return;
  }
  uint8_t tag = 0, cmd = 0;
  if (nr_passive_mac_dlsch_ta(tb, tb_bytes, &tag, &cmd)) {
    /* TS 38.213 4.2: a CE command is RELATIVE around 31, so 31 means hold. The ABSOLUTE range needs
     * that UE's RAR, which this receiver has only if it also overheard its Msg2. */
    const int step = (int)cmd - 31;
    const double delta_m = nr_passive_mac_ta_metres((uint16_t)(step < 0 ? -step : step), mu)
                           * (step < 0 ? -1.0 : 1.0);
    LOG_A(PHY, "SENSING: MAC_TA CE (%d.%d) rnti=0x%x tag=%u ta_cmd=%u -> %+.1f m range delta\n",
          frame, slot, rnti, tag, cmd, delta_m);
    nr_ue_ctx_ul_ta_command(rnti, step, mu, false);
  }
}

/* dmrs_oracle_measure: DM-RS symbol oracle: measures per-symbol DM-RS coherence over the grant's PRBs and returns the mask of DM-RS symbols and the last data symbol.
 *   Runs one FEP per symbol (CPU), computes the coherence metric per symbol, the energy profile and the lower median; a symbol is DM-RS when its metric exceeds 0.68 and med + 0.18.
 *   Outputs: *last_sym (last symbol above 1/4 of peak energy) and *med_out. Callers: nr_pdsch_passive_queue_thread() (grant, k0 and layout probes) and nr_pdsch_passive_bc9_confirm().
 */
static uint16_t dmrs_oracle_measure(PHY_VARS_NR_UE *ue, NR_DL_FRAME_PARMS *fp, uint32_t rxdataF_sz,
                                    c16_t rxdataF[][rxdataF_sz], int nr_slot, int rb0, int nrb, int nscid, int nid,
                                    int *last_sym, double prof[14], double *med_out)
{
  const int n_sym = fp->symbols_per_slot;
  float coh[275];
  double energy[14] = {0}; /* per symbol, over the grant's PRBs: the allocation END is where it stops */
  for (int sym = 0; sym < n_sym && sym < 14; sym++) {
    nr_slot_fep_ant(ue, fp, nr_slot, sym, 0, rxdataF, link_type_dl, 0, nr_pdsch_passive_iq_for_slot(nr_slot, ue->common_vars.rxdata));
    nr_dmrs_prb_coherence(&rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size, fp->first_carrier_offset,
                          fp->N_RB_DL < 275 ? fp->N_RB_DL : 275, n_sym, nr_slot, sym,
                          nscid, nid, fp->Ncp == NR_NORMAL, coh);
    double m = 0;
    for (int p = rb0; p < rb0 + nrb && p < fp->N_RB_DL; p++) m += coh[p];
    prof[sym] = m / nrb;
    const c16_t *sy = &rxdataF[0][sym * fp->ofdm_symbol_size];
    double e = 0;
    for (int p = rb0; p < rb0 + nrb && p < fp->N_RB_DL; p++)
      for (int r = 0; r < 12; r++) {
        const int k = (fp->first_carrier_offset + p * 12 + r) % fp->ofdm_symbol_size;
        e += (double)sy[k].r * sy[k].r + (double)sy[k].i * sy[k].i;
      }
    energy[sym] = e;
  }
  /* Last symbol of the allocation: the last one whose energy on these PRBs is above a quarter of
   * the strongest (data symbols are within a few dB of each other; an empty symbol is noise). */
  int last = -1;
  double emax = 0;
  for (int sym = 0; sym < n_sym && sym < 14; sym++) if (energy[sym] > emax) emax = energy[sym];
  for (int sym = 0; sym < n_sym && sym < 14; sym++) if (energy[sym] > 0.25 * emax) last = sym;
  /* The metric's floor on data symbols is ~0.5 (three random pair-products per PRB), a DM-RS
   * symbol reads ~0.9 (measured on the rank-4 bed: 0.92 vs 0.47-0.53). Symbols without energy
   * (the special slot's UL part) read 0 and are left out of the median. */
  double srt[14]; int ns = 0;
  for (int sym = 0; sym < n_sym; sym++) if (prof[sym] > 0.05) srt[ns++] = prof[sym];
  for (int a = 1; a < ns; a++) for (int b = a; b > 0 && srt[b] < srt[b - 1]; b--) { double t = srt[b]; srt[b] = srt[b - 1]; srt[b - 1] = t; }
  /* LOWER median: a short type-B allocation (L = 2) has two energetic symbols, and the upper median
   * of two IS the DM-RS symbol, which then can never clear med + 0.18. Identical to the old median for
   * an ODD count only; for an even count >= 4 it is the lower of the two middle values. */
  const double med = ns ? srt[(ns - 1) / 2] : 1.0;
  uint16_t mask = 0;
  for (int sym = 0; sym < n_sym; sym++)
    if (prof[sym] > 0.68 && prof[sym] > med + 0.18) mask |= (uint16_t)(1u << sym); /* OTA reads 0.75-0.82, floor 0.50 */
  *last_sym = last;
  *med_out = med;
  return mask;
}

static _Atomic uint64_t g_k0_probes, g_k0_probe_retained, g_k0_probe_retained_max, g_k0_probe_hits;

/* The DM-RS probes below (oracle gate, rank, CDM, DM-RS identity) read one CONTIGUOUS stretch of
 * subcarriers from first_rb. A PRB-list or PRG grant is not contiguous (and may change precoder at a
 * PRG edge), so they measure its LARGEST segment instead -- still real PRBs of this grant, which is
 * all they need; covering every segment would only add PRBs to a statistic, not correctness. A
 * contiguous grant keeps first_rb/num_rbs exactly. *nrb = 0 = nothing measurable. */
static int probe_span(const nr_pdsch_passive_job_t *j, int *nrb)
{
  const freq_alloc_bitmap_t *fa = &j->freq_alloc;
  *nrb = fa->num_rbs;
  if (fa->n_prb_list == 0 && fa->prg == 0)
    return fa->first_rb;
  uint16_t contig[NR_PRB_SET_MAX];
  const uint16_t *prb = fa->prb_list;
  int n = fa->n_prb_list;
  if (n == 0) {
    n = fa->num_rbs < NR_PRB_SET_MAX ? fa->num_rbs : NR_PRB_SET_MAX;
    for (int i = 0; i < n; i++)
      contig[i] = (uint16_t)(fa->first_rb + i);
    prb = contig;
  }
  nr_prb_seg_t seg[NR_PRB_SET_MAX];
  const int ns = nr_prb_segments(prb, n, j->dlsch_pdu.BWPStart, fa->prg, seg, NR_PRB_SET_MAX);
  if (ns <= 0) {
    *nrb = 0;
    return fa->first_rb;
  }
  int best = 0;
  for (int s = 1; s < ns; s++)
    if (seg[s].n_prb > seg[best].n_prb)
      best = s;
  *nrb = seg[best].n_prb;
  return seg[best].prb_start;
}

/* ---- BC9 certified-flag census (see the header) ---- */
static uint64_t bc9_row_k0(void *arg, uint64_t cfg, uint16_t rnti, uint8_t tda)
{
  (void)arg;
  return nr_pdsch_config_sweep_row_k0_allowed(cfg, rnti, tda);
}
static _Atomic uint64_t g_bc9_jobs, g_bc9_cert, g_bc9_cert_ok, g_bc9_nosib, g_bc9_nodci, g_bc9_ok, g_bc9_alarm;
static _Atomic uint64_t g_bc9_cert_ok_k0[5]; /* certified passes by the hypothesis' k0: 0, 1, 2, 3, >= 4 */
/* k0 values with a certified pass per (RNTI, configuration, TDA), checked against the winner on convergence (M3). */
#define BC9_CPK 64
static struct { uint64_t cfg, mask, touched; uint16_t rnti; uint8_t tda; } g_bc9_cpk[BC9_CPK];
static uint64_t g_bc9_cpk_clock;
static pthread_mutex_t g_bc9_cpk_lock = PTHREAD_MUTEX_INITIALIZER;
static int bc9_cpk_find(uint16_t rnti, uint64_t cfg, uint8_t tda, bool create)
{
  int lru = 0;
  for (int i = 0; i < BC9_CPK; i++) {
    if (g_bc9_cpk[i].rnti == rnti && g_bc9_cpk[i].cfg == cfg && g_bc9_cpk[i].tda == tda && g_bc9_cpk[i].rnti)
      return i;
    if (g_bc9_cpk[i].touched < g_bc9_cpk[lru].touched)
      lru = i;
  }
  if (!create)
    return -1;
  g_bc9_cpk[lru].rnti = rnti;
  g_bc9_cpk[lru].cfg = cfg;
  g_bc9_cpk[lru].tda = tda;
  g_bc9_cpk[lru].mask = 0;
  return lru;
}
int nr_pdsch_passive_bc12_census(const nr_pdsch_sweep_ticket_t *ticket, const nr_pdsch_cfg_hypothesis_t *winner, char *buf, size_t n)
{
  nr_td_tdra_t l[NR_TD_MAX_SIB1_TDRA];
  const int nl = nr_td_sib1_store_get(l);
  const nr_td_census_t s = nr_td_census_sib1(l, nl, ticket->tda_index, winner);
  const nr_td_census_t d = nr_td_census_deftab(ticket->typeA_pos, ticket->tda_index, winner);
  nr_td_census_count(ticket->dci_format, s);
  nr_td_census_count_deftab(ticket->dci_format, d);
  static const char *const sn[] = {"none", "match", "mismatch"};
  static const char *const dn[] = {"na", "match", "mismatch"};
  uint32_t er = 0, et = 0, ep = 0;
  nr_pdsch_config_sweep_excl_census(ticket->configuration, ticket->rnti, ticket->tda_index, &er, &et, &ep);
  return snprintf(buf, n, " k0=%u map=%c dci=%s sib1_row=%s deftab=%s excl_restarts=%u excl_truncs=%u dci_phases=%u",
                  (unsigned)winner->k0, winner->mapping_type ? 'B' : 'A',
                  ticket->dci_format == 10 ? "1_0" : ticket->dci_format == 11 ? "1_1" : "?", sn[s], dn[d], er, et, ep);
}
void nr_pdsch_passive_bc9_converged(const nr_pdsch_sweep_ticket_t *ticket, uint8_t winner_k0)
{
  if (!nr_cfg_epoch_work_current()) return;
  if (ticket == NULL)
    return;
  pthread_mutex_lock(&g_bc9_cpk_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_bc9_cpk_lock); return; }
  const int i = bc9_cpk_find(ticket->rnti, ticket->configuration, ticket->tda_index, false);
  const uint64_t other = i >= 0 ? g_bc9_cpk[i].mask & ~(UINT64_C(1) << (winner_k0 & 63)) : 0;
  if (i >= 0)
    g_bc9_cpk[i].rnti = 0;
  pthread_mutex_unlock(&g_bc9_cpk_lock);
  if (other) {
    atomic_fetch_add(&g_bc9_alarm, 1);
    LOG_W(PHY, "SENSING: BC9 DCIADJ_CERT ALARM rnti=0x%x tda=%u converged on k0=%u but certified passes were seen on k0 mask 0x%llx "
               "(A1/A3 violation or compatibility bug)\n", ticket->rnti, ticket->tda_index, winner_k0, (unsigned long long)other);
  }
}
void nr_pdsch_passive_bc9_note(const nr_pdsch_sweep_ticket_t *ticket, uint16_t rnti, uint32_t dci_abs_slot,
                               const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu, uint16_t xoh, bool crc_ok)
{
  if (!nr_cfg_epoch_work_current()) return;
  /* ISAC_TD_DCI_ADJ=0: the whole BC9 runtime path is off (A/B) */
  if (!nr_dci_hist_enabled() || ticket == NULL || pdu == NULL || ticket->generation == 0 || ticket->settled)
    return;
  nr_pdsch_cfg_hypothesis_t h;
  uint64_t sib = 0;
  uint8_t tables = 0;
  if (!nr_pdsch_config_sweep_ticket_siblings(ticket, &h, &sib, &tables))
    return;
  nr_dci_hist_t *dh = nr_dci_hist_global();
  nr_dci_hist_entry_t g[4];
  const int n = nr_dci_hist_at(dh, rnti, dci_abs_slot, g, 4);
  const nr_dci_hist_entry_t *gg = NULL;
  for (int i = 0; i < n && !gg; i++)
    if (g[i].dci11 && g[i].tda == ticket->tda_index && g[i].cfg == ticket->configuration) /* review M3: same key */
      gg = &g[i];
  const uint64_t jobs = atomic_fetch_add(&g_bc9_jobs, 1) + 1;
  atomic_fetch_add(&g_bc9_ok, crc_ok);
  if (gg == NULL) {
    atomic_fetch_add(&g_bc9_nodci, 1);
  } else {
    if (!(sib & ~(UINT64_C(1) << h.k0)))
      atomic_fetch_add(&g_bc9_nosib, 1);
    const nr_dci_geom_t geo = {.nb_symb = h.tda_length, .dmrs_mask = h.dmrs_mask,
                               .dmrs_type = (uint8_t)(pdu->dmrsConfigType == NFAPI_NR_DMRS_TYPE2),
                               .nl = (uint8_t)__builtin_popcount(pdu->dmrs_ports), .xoh = xoh};
    const nr_dci_row_k0_fn rk = nr_pdsch_config_sweep_rnti_constrained(rnti, gg->cfg) ? bc9_row_k0 : NULL; /* no certified row: skip */
    if (nr_dci_hist_k0_certified(dh, gg, h.k0, sib, tables, &geo, rk, NULL)) {
      atomic_fetch_add(&g_bc9_cert, 1);
      atomic_fetch_add(&g_bc9_cert_ok, crc_ok);
      if (crc_ok) {
        atomic_fetch_add(&g_bc9_cert_ok_k0[h.k0 < 4 ? h.k0 : 4], 1);
        pthread_mutex_lock(&g_bc9_cpk_lock);
        if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_bc9_cpk_lock); return; }
        const int ci = bc9_cpk_find(rnti, ticket->configuration, ticket->tda_index, true);
        g_bc9_cpk[ci].mask |= UINT64_C(1) << (h.k0 & 63);
        g_bc9_cpk[ci].touched = ++g_bc9_cpk_clock;
        pthread_mutex_unlock(&g_bc9_cpk_lock);
      }
    }
  }
  if ((jobs % 1000) == 0)
    LOG_A(PHY, "SENSING: BC9 DCIADJ_CERT trials=%llu certified=%llu (f_S=%.4f) certified_pass=%llu passes=%llu no_sibling=%llu "
               "no_dci_in_history=%llu certified_pass_by_k0[0,1,2,3,>=4]=%llu,%llu,%llu,%llu,%llu wrong_k0_alarms=%llu\n",
          (unsigned long long)jobs, (unsigned long long)atomic_load(&g_bc9_cert),
          (double)atomic_load(&g_bc9_cert) / (double)jobs, (unsigned long long)atomic_load(&g_bc9_cert_ok),
          (unsigned long long)atomic_load(&g_bc9_ok), (unsigned long long)atomic_load(&g_bc9_nosib),
          (unsigned long long)atomic_load(&g_bc9_nodci), (unsigned long long)atomic_load(&g_bc9_cert_ok_k0[0]),
          (unsigned long long)atomic_load(&g_bc9_cert_ok_k0[1]), (unsigned long long)atomic_load(&g_bc9_cert_ok_k0[2]),
          (unsigned long long)atomic_load(&g_bc9_cert_ok_k0[3]), (unsigned long long)atomic_load(&g_bc9_cert_ok_k0[4]),
          (unsigned long long)atomic_load(&g_bc9_alarm));
}

/* ---- BC9d: deterministic exclusions only from CONFIRMED DCIs (nr_dci_history.h) ----
 * Runs at feedback time on a TB CRC pass, AFTER the trial's KL feedback (an exclusion re-indexes the context, like the Qm
 * oracle), in the deferred consumer and the in-line path -- off the PDCCH thread (review M4 of BC9: the accept hook no longer
 * takes the sweep lock per DCI). */
_Static_assert(NR_DCI_HIST_K0_MAX == NR_TD_K0_MAX, "BC9d: one k0 range for the history and the sweep");
typedef struct {
  int mu;
} bc9d_arg_t;
static bool bc9d_tdd_last(void *arg, uint32_t abs_slot, int8_t *last)
{
  return nr_passive_acq_tdd_pdsch_last_symbols(abs_slot, ((const bc9d_arg_t *)arg)->mu, NR_TD_K0_MAX + 1, last);
}
static uint64_t bc9d_row_k0(void *arg, uint64_t cfg, uint16_t rnti, uint8_t tda)
{
  (void)arg;
  return nr_pdsch_config_sweep_row_k0_allowed(cfg, rnti, tda);
}
static bool bc9d_constrained(void *arg, uint64_t cfg, uint16_t rnti)
{
  (void)arg;
  return nr_pdsch_config_sweep_rnti_constrained(rnti, cfg);
}
static _Atomic uint64_t g_bc9d_confirms, g_bc9d_missed, g_bc9d_repeat, g_bc9d_tdd, g_bc9d_tdd_applied, g_bc9d_adj_rows,
    g_bc9d_adj_removed, g_bc9d_adj_refused;
/* Review M5 cache (moved from the accept hook): true when this exact TDD constraint is known applied (no g_lock); else applies
 * it and caches it when it persisted (the RNTI is tracked by the sweep). Valid while the sweep's constraint epoch is unchanged.
 * Direct-mapped, own small lock. */
#define BC9_TDD_CACHE 256
static struct { uint64_t cfg, epoch; uint16_t rnti; uint8_t tda; bool valid; nr_td_excl_t ex; } g_bc9_tdd_cache[BC9_TDD_CACHE];
static pthread_mutex_t g_bc9_tdd_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int bc9d_exclude(void *arg, uint64_t cfg, uint16_t rnti, uint8_t tda, const int8_t *last, bool tdd)
{
  if (!nr_cfg_epoch_work_current()) return 0;
  (void)arg;
  nr_td_excl_t ex;
  memcpy(ex.last, last, sizeof(ex.last));
  if (!tdd)
    return nr_pdsch_config_sweep_exclude_key(cfg, rnti, tda, &ex);
  atomic_fetch_add(&g_bc9d_tdd, 1);
  uint64_t hsh = cfg * UINT64_C(0x9E3779B97F4A7C15) ^ ((uint64_t)rnti << 8) ^ tda;
  for (int k = 0; k <= NR_TD_K0_MAX; k++)
    hsh = hsh * 31 + (uint8_t)ex.last[k];
  const int i = (int)((hsh ^ (hsh >> 29)) % BC9_TDD_CACHE);
  const uint64_t epoch = nr_pdsch_config_sweep_cert_epoch(); /* read BEFORE applying: a concurrent drop invalidates */
  pthread_mutex_lock(&g_bc9_tdd_cache_lock);
  const bool hit = g_bc9_tdd_cache[i].valid && g_bc9_tdd_cache[i].epoch == epoch && g_bc9_tdd_cache[i].rnti == rnti
                   && g_bc9_tdd_cache[i].cfg == cfg && g_bc9_tdd_cache[i].tda == tda
                   && !memcmp(g_bc9_tdd_cache[i].ex.last, ex.last, sizeof(ex.last));
  pthread_mutex_unlock(&g_bc9_tdd_cache_lock);
  if (hit)
    return 0;
  atomic_fetch_add(&g_bc9d_tdd_applied, 1);
  const int rm = nr_pdsch_config_sweep_exclude_key(cfg, rnti, tda, &ex);
  if (nr_pdsch_config_sweep_rnti_constrained(rnti, cfg)) { /* persisted: safe to skip next time */
    pthread_mutex_lock(&g_bc9_tdd_cache_lock);
    if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_bc9_tdd_cache_lock); return 0; }
    g_bc9_tdd_cache[i].valid = true;
    g_bc9_tdd_cache[i].epoch = epoch;
    g_bc9_tdd_cache[i].rnti = rnti;
    g_bc9_tdd_cache[i].cfg = cfg;
    g_bc9_tdd_cache[i].tda = tda;
    g_bc9_tdd_cache[i].ex = ex;
    pthread_mutex_unlock(&g_bc9_tdd_cache_lock);
  }
  return rm;
}
void nr_pdsch_passive_bc9_confirm(const nr_pdsch_sweep_ticket_t *ticket, uint16_t rnti, uint32_t dci_abs_slot, int mu, bool crc_ok)
{
  if (!nr_cfg_epoch_work_current()) return;
  if (!crc_ok || ticket == NULL || ticket->generation == 0 || !nr_dci_hist_enabled())
    return;
  bc9d_arg_t a = {.mu = mu};
  { /* TD_EXCL census: the phase of this confirmed DCI, recorded BEFORE its exclusions so a restart it causes is never ahead of it */
    uint32_t period = nr_passive_acq_tdd_period_slots(mu);
    if (period == 0)
      period = 10u << mu; /* slots per frame */
    nr_pdsch_config_sweep_note_dci_phase(ticket->configuration, rnti, (uint16_t)(dci_abs_slot % period));
  }
  const nr_dci_excl_ops_t ops = {bc9d_tdd_last, bc9d_exclude, bc9d_constrained, bc9d_row_k0, &a};
  nr_dci_confirm_out_t o;
  nr_dci_hist_on_confirm(nr_dci_hist_global(), rnti, dci_abs_slot, ticket->configuration, ticket->tda_index, &ops, &o);
  const uint64_t n = atomic_fetch_add(&g_bc9d_confirms, 1) + 1;
  atomic_fetch_add(&g_bc9d_missed, !o.found);
  atomic_fetch_add(&g_bc9d_repeat, o.found && o.newly == 0);
  atomic_fetch_add(&g_bc9d_adj_rows, (uint64_t)o.adj_rows);
  atomic_fetch_add(&g_bc9d_adj_removed, (uint64_t)o.adj_removed);
  atomic_fetch_add(&g_bc9d_adj_refused, (uint64_t)o.adj_refused);
  static _Atomic int s_log = 20;
  if ((o.adj_removed > 0 || o.adj_refused > 0) && atomic_fetch_sub(&s_log, 1) > 0)
    LOG_A(PHY, "SENSING: BC9 DCIADJ confirmed DCI rnti=0x%x slot=%u tda=%u -> rows=%d removed=%d refused=%d\n", rnti, dci_abs_slot,
          ticket->tda_index, o.adj_rows, o.adj_removed, o.adj_refused);
  if ((n % 2000) == 0) {
    uint64_t lt2 = 0, ge2 = 0, refused = 0;
    nr_pdsch_config_sweep_excl_stats(&lt2, &ge2, &refused);
    LOG_A(PHY, "SENSING: BC9 DCICONF confirms=%llu missed_lookup=%llu repeat=%llu tdd_known=%d tdd_dcis=%llu tdd_lock_calls=%llu "
               "adj_rows=%llu adj_removed=%llu adj_refused=%llu excl_removed[k0<2]=%llu excl_removed[k0>=2]=%llu excl_refused=%llu\n",
          (unsigned long long)n, (unsigned long long)atomic_load(&g_bc9d_missed), (unsigned long long)atomic_load(&g_bc9d_repeat),
          nr_passive_acq_tdd_known(), (unsigned long long)atomic_load(&g_bc9d_tdd),
          (unsigned long long)atomic_load(&g_bc9d_tdd_applied), (unsigned long long)atomic_load(&g_bc9d_adj_rows),
          (unsigned long long)atomic_load(&g_bc9d_adj_removed), (unsigned long long)atomic_load(&g_bc9d_adj_refused),
          (unsigned long long)lt2, (unsigned long long)ge2, (unsigned long long)refused);
  }
}

void nr_pdsch_passive_oracle_inline(PHY_VARS_NR_UE *ue, const nr_pdsch_sweep_ticket_t *ticket,
                                    const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu, const freq_alloc_bitmap_t *fa,
                                    int nr_slot, c16_t *scratch)
{
  if (ticket == NULL || ticket->settled || ticket->generation == 0)
    return;
  nr_pdsch_passive_job_t j;
  memset(&j, 0, sizeof(j));
  j.dlsch_pdu = *pdu;
  j.freq_alloc = *fa;
  int nrb;
  const int rb = probe_span(&j, &nrb);
  if (nrb < 4)
    return;
  NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint32_t sz = fp->samples_per_slot_wCP;
  double prof[14] = {0}, med = 1.0;
  int last_sym = -1;
  const uint16_t mask = dmrs_oracle_measure(ue, fp, sz, (c16_t(*)[sz])scratch, nr_slot, nr_dmrs_oracle_crb(pdu->BWPStart, rb),
                                            nrb, pdu->nscid, pdu->dlDmrsScramblingId, &last_sym, prof, &med);
  static _Atomic int s_log = 6;
  if (mask && atomic_fetch_sub(&s_log, 1) > 0)
    LOG_A(PHY, "SENSING: DMRS_ORACLE (in-line) slot=%d rb=%d+%d mask=0x%x last_sym=%d med=%.2f\n", nr_slot, rb, nrb, mask,
          last_sym, med);
  if (mask)
    nr_pdsch_config_sweep_observe(ticket, mask, last_sym, 0); /* K39: k0 = 0 is only PLAUSIBLE (mask/last symbol prune; k0 is never pinned by DM-RS presence) */
}

/* nr_pdsch_passive_queue_thread: consumer thread body of the passive PDSCH decode pool: takes a slot group of grants from the ring, decodes each grant, and feeds the results back.
 *   Per group: optional signal-time pacing (ISAC_PDSCHQ_SIGNAL_TIME, replay only), owned IQ copy, FEP+channel estimate on CPU or on the GPU (gpu_fep_submit_and_wait),
 *   DM-RS oracle and k0 hypotheses, GrantWork and CB0 elimination, then nr_pdsch_passive_decode() per grant; solver feedback (nr_pdsch_solver_feedback), RRC and TA harvest, Technique D and Qm-oracle updates.
 *   Flags read: ISAC_PDSCH_INLINE_POOL, ISAC_PDSCHQ_SIGNAL_TIME, ISAC_PROBE_ALL, ISAC_PDSCH_K0_PROBE, ISAC_TD_GRANTWORK, ISAC_TD_GW_PROBE, ISAC_TD_CB0_ELIM, ISAC_RX_EQ_AB, ISAC_CHEST_OCC_AB, ISAC_LDPC_ITERS_AB, ISAC_GPU_SELFCHECK, ISAC_PDSCH_EVM_GRANT.
 *   Started by nr_pdsch_passive_queue_start() (one per consumer, priority 40 below RT). Exits when g_stop is set; uses g_lock and g_cv for the ring.
 */
static void *nr_pdsch_passive_queue_thread(void *arg)
{
  const int idx = ((consumer_arg_t *)arg)->idx;
  PHY_VARS_NR_UE *ue        = g_ue;
  NR_DL_FRAME_PARMS *fp     = &ue->frame_parms;
  const uint32_t rxdataF_sz = fp->samples_per_slot_wCP;
  const long slots_per_frame = fp->slots_per_frame;

  LOG_I(PHY, "SENSING: passive PDSCH decode consumer %d started\n", idx);
  const char *slot_timing_env = getenv("ISAC_PDSCH_SLOT_TIMING");
  const bool slot_timing = slot_timing_env && atoi(slot_timing_env) != 0;
  {
    const char *e = getenv("ISAC_PDSCH_INLINE_POOL");
    nr_passive_tpool_inline = e && atoi(e) != 0;
    if (nr_passive_tpool_inline)
      LOG_A(PHY, "SENSING: passive PDSCH consumer %d runs FEP/chest/LDPC tasks inline (ISAC_PDSCH_INLINE_POOL)\n", idx);
  }

  /* ISAC_PDSCHQ_SIGNAL_TIME=1 (T26c, REPLAY ONLY): an offline replay runs at a fraction of real time, so the decode
   * pool looks ~1/f times faster than live. After each dequeued group the consumer waits until the producer has
   * advanced by the group's own wall time in SIGNAL slots -- the pool then serves the queue at its live rate in signal
   * time, and queue fill / staleness become representative of live load. Never enable on air. */
  static int s_sigt = -1;
  if (s_sigt < 0) s_sigt = getenv("ISAC_PDSCHQ_SIGNAL_TIME") && atoi(getenv("ISAC_PDSCHQ_SIGNAL_TIME")) ? 1 : 0;
  uint64_t sigt_t0 = 0;
  long sigt_p0 = 0;
  const double slot_ns = 10e6 / (double)slots_per_frame;
  while (1) {
    if (s_sigt && sigt_t0) {
      const long need = sigt_p0 + (long)((double)(now_ns() - sigt_t0) / slot_ns + 0.5);
      for (int w = 0; w < 200000 && atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed) < need
                      && !atomic_load_explicit(&g_stop, memory_order_relaxed); w++) {
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
      }
      sigt_t0 = 0;
    }
    nr_pdsch_passive_job_t job;
    nr_pdsch_passive_set_owned_iq(NULL, -1);
    const bool owned = nr_csirs_disc_enabled();

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
    uint64_t enqueue_ns = g_ring_ns[g_tail];
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    /* SLOT GROUP: also take every queued job of the SAME slot (the producer enqueues a slot's
     * grants contiguously), so this thread does that slot's FEP and channel estimate once. */
    nr_pdsch_passive_job_t more[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX];
    uint64_t more_ns[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX];
    int n_more = 0;
    while (g_count > 0 && n_more < (atomic_load(&g_gpu) ? NR_PDSCH_PASSIVE_SLOT_GROUP_MAX : 8)
           && g_ring[g_tail].absolute_slot == job.absolute_slot) {
      more_ns[n_more] = g_ring_ns[g_tail];
      more[n_more++] = g_ring[g_tail];
      g_tail = (g_tail + 1) % g_depth;
      g_count--;
    }
    pthread_mutex_unlock(&g_lock);
    const uint64_t slot_t0 = slot_timing ? now_ns() : 0;
    if (s_sigt) {
      sigt_t0 = now_ns();
      sigt_p0 = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    }
    if (owned) {
      const uint64_t taken = now_ns();
      latency_note(NR_PDSCH_LAT_QUEUE, enqueue_ns, taken);
      for (int k = 0; k < n_more; ++k) latency_note(NR_PDSCH_LAT_QUEUE, more_ns[k], taken);
    }
    /* A slot group can straddle a bump while the producer admits its members. Drop each old
     * member before union estimation, GPU batching, probes, or any decode feedback. */
    if (nr_cfg_reconf_enabled()) {
      int n_live = 0;
      if (nr_passive_job_epoch_old(true, job.config_epoch, nr_cfg_epoch_current))
        atomic_fetch_add_explicit(&g_dropped_epoch, 1, memory_order_relaxed);
      else
        n_live = 1;
      for (int k = 0; k < n_more; k++) {
        if (nr_passive_job_epoch_old(true, more[k].config_epoch, nr_cfg_epoch_current))
          atomic_fetch_add_explicit(&g_dropped_epoch, 1, memory_order_relaxed);
        else if (n_live++ == 0) {
          job = more[k]; enqueue_ns = more_ns[k];
        } else {
          more[n_live - 2] = more[k]; more_ns[n_live - 2] = more_ns[k];
        }
      }
      if (n_live == 0) continue;
      n_more = n_live - 1;
    }
    /* Union of RBs over the group members whose DM-RS configuration matches the head job's. */
    /* PRB-list / PRG grants never read the shared estimate (the decoder bypasses the cache for them),
     * and their first_rb + num_rbs is not their span, so they must not widen the union the OTHER grants
     * of the slot are estimated over. */
#define CONTIG_JOB(j) ((j).freq_alloc.n_prb_list == 0 && (j).freq_alloc.prg == 0)
    int rb_lo = CONTIG_JOB(job) ? job.freq_alloc.first_rb : INT_MAX;
    int rb_hi = CONTIG_JOB(job) ? job.freq_alloc.first_rb + job.freq_alloc.num_rbs : INT_MIN;
    for (int k = 0; k < n_more; k++) {
      if (!CONTIG_JOB(more[k]))
        continue;
      const fapi_nr_dl_config_dlsch_pdu_rel15_t *a = &job.dlsch_pdu, *b = &more[k].dlsch_pdu;
      if (a->dlDmrsSymbPos != b->dlDmrsSymbPos || a->dmrsConfigType != b->dmrsConfigType
          || a->nscid != b->nscid || a->dmrs_ports != b->dmrs_ports || a->n_dmrs_cdm_groups != b->n_dmrs_cdm_groups
          || a->dlDmrsScramblingId != b->dlDmrsScramblingId)
        continue;
      const int lo = more[k].freq_alloc.first_rb, hi = lo + more[k].freq_alloc.num_rbs;
      if (lo < rb_lo) rb_lo = lo;
      if (hi > rb_hi) rb_hi = hi;
    }
    if (rb_hi < rb_lo) /* no contiguous member: rb_n = 0 disables the widening */
      rb_lo = rb_hi = 0;
#undef CONTIG_JOB
    /* Wide first: the data-aided CFR rows come from what decodes, and a wide grant's row is
     * worth more to the sensing grid than a narrow one's if the samples go stale mid-group. */
    if (n_more > 0) {
      nr_pdsch_passive_job_t all[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
      uint64_t all_ns[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
      all_ns[0] = enqueue_ns;
      for (int k = 0; k < n_more; ++k) all_ns[k + 1] = more_ns[k];
      all[0] = job;
      for (int k = 0; k < n_more; k++) all[k + 1] = more[k];
      for (int i = 1; i <= n_more; i++) {
        nr_pdsch_passive_job_t t = all[i];
        const uint64_t t_ns = all_ns[i];
        int j = i - 1;
        while (j >= 0 && all[j].freq_alloc.num_rbs < t.freq_alloc.num_rbs) { all[j + 1] = all[j]; all_ns[j + 1] = all_ns[j]; j--; }
        all[j + 1] = t; all_ns[j + 1] = t_ns;
      }
      job = all[0]; enqueue_ns = all_ns[0];
      for (int k = 0; k < n_more; ++k) more_ns[k] = all_ns[k + 1];
      for (int k = 0; k < n_more; k++) more[k] = all[k + 1];
    }
    nr_pdsch_passive_set_slot_share(n_more > 0, rb_lo, rb_hi - rb_lo);
    if (n_more > 0) atomic_fetch_add_explicit(&g_slot_groups, 1, memory_order_relaxed);

    /* ---- STALENESS CHECK. A job's raw IQ lives in rxdata only until the producer reaches the SAME
     * slot index one frame later, so decoding after that reads the NEXT frame's samples: the CRC
     * fails and it is indistinguishable from a weak channel. Count it as a drop, never "decode
     * anyway and hope". With drop-oldest admission this should now be rare -- if dropped_stale stays
     * high, the consumer pool is still too small for the offered rate. ---- */
    long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    if (lag > (long)atomic_load_explicit(&g_max_lag, memory_order_relaxed)) {
      uint64_t prev = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
      if (owned) {
        while ((uint64_t)lag > prev && !atomic_compare_exchange_weak(&g_max_lag, &prev, (uint64_t)lag)) {}
      } else {
        atomic_store_explicit(&g_max_lag, (uint64_t)(lag > 0 ? lag : 0), memory_order_relaxed);
      }
    }
    /* A k0 > 0 hypothesis targets a slot the producer may not have read yet: wait for it (bounded
     * by 3 ms of wall time) instead of counting it stale. An observed k0 >= 2 (k0 oracle) targets a
     * slot further out, so its bound grows by one slot duration per slot beyond k0 = 1; k0 <= 1 is
     * unchanged. */
    const int wait_max = 30 + (job.sweep_ticket.k0 > 1 ? (int)(job.sweep_ticket.k0 - 1) * (int)(100 / slots_per_frame) : 0);
    for (int w = 0; w < wait_max && prod < job.absolute_slot; w++) {
      struct timespec ts = {0, 100000};
      nanosleep(&ts, NULL);
      prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    }
    if (!nr_passive_samples_valid(prod, job.absolute_slot, slots_per_frame)) {
      const int count = owned ? n_more + 1 : 1;
      atomic_fetch_add_explicit(&g_dropped_stale, count, memory_order_relaxed);
      if (owned) atomic_fetch_add(&g_drop_reason[prod < job.absolute_slot ? NR_PDSCH_DROP_FUTURE : NR_PDSCH_DROP_QUEUE], count);
      release_solver(&job);
      for (int k = 0; owned && k < n_more; k++) release_solver(&more[k]);
      continue;
    }
    if (owned) {
      const size_t ring = 2u * fp->samples_per_frame;
      const size_t early = fp->nb_prefix_samples / fp->ofdm_offset_divisor;
      const size_t off = get_samples_slot_timestamp(fp, job.nr_slot_rx);
      /* Match the GPU's conservative max-CP window, also covering all CPU FFTs. */
      const size_t window = fp->nb_prefix_samples0 + fp->ofdm_symbol_size
          + (fp->symbols_per_slot - 1) * (fp->ofdm_symbol_size + fp->nb_prefix_samples)
          + fp->ofdm_symbol_size;
      void *dst[NB_ANTENNAS_RX];
      const void *src[NB_ANTENNAS_RX];
      for (int a = 0; a < fp->nb_antennas_rx; ++a) {
        dst[a] = g_owned_iq[idx][a];
        src[a] = ue->common_vars.rxdata[a];
      }
      if (!nr_pdsch_copy_iq(dst, src, fp->nb_antennas_rx, ring * sizeof(c16_t),
                            ((off + ring - early) % ring) * sizeof(c16_t), window * sizeof(c16_t),
                            job.absolute_slot, slots_per_frame, gpu_req_producer, NULL)) {
        atomic_fetch_add(&g_dropped_stale, n_more + 1);
        atomic_fetch_add(&g_drop_reason[NR_PDSCH_DROP_COPY], n_more + 1);
        release_solver(&job);
        for (int k = 0; k < n_more; k++) release_solver(&more[k]);
        continue;
      }
      nr_pdsch_passive_set_owned_iq(g_owned_iq[idx], job.nr_slot_rx);
    }

    /* ---- GPU front end: one FEP for the slot, one launch set for every eligible job of the group.
     * A job the GPU cannot take (PT-RS, CSI-RS RM, Nl > antennas, wants the data-aided CFR, BWP probe)
     * keeps n_llr = 0 and goes through the CPU chain below unchanged. ---- */
    int16_t *t_gpu_llr = g_consumer_gpu_llr[idx];
    int16_t *t_gpu_rxF = g_consumer_gpu_rxF[idx];
    int16_t *group_rxF = NULL;
    uint64_t group_fep_ns = 0;
    int group_gpu_jobs = 0;
    nr_gpu_pdsch_job_t gj[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
    int gj_ok[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1] = {0};
    static _Atomic int s_probe_all_g = -1; /* _Atomic: every passivePdsch consumer resolves it (TSAN) */
    if (s_probe_all_g < 0) s_probe_all_g = (getenv("ISAC_PROBE_ALL") != NULL) ? 1 : 0;
    if (g_gpu) {

      int n_gj = 0;
      for (int gi = -1; gi < n_more; gi++) {
        const nr_pdsch_passive_job_t *jb = gi < 0 ? &job : &more[gi];
        nr_pdsch_passive_grant_t g = jb->grant;
        g.source_absolute_slot = jb->absolute_slot;
        gj_ok[gi + 1] = !nr_passive_job_epoch_old(nr_cfg_reconf_enabled(), jb->config_epoch, nr_cfg_epoch_current)
                        && jb->bwp_probe_entry <= 0 && !jb->want_data
                        && nr_pdsch_passive_gpu_job(ue, &jb->dlsch_pdu, &jb->freq_alloc, &g, jb->nr_slot_rx,
                                                    jb->layout_probe || s_probe_all_g, &gj[gi + 1]);
        if (gj_ok[gi + 1]) n_gj++;
      }
      if (n_gj > 0 && t_gpu_llr) {

        /* mirror nr_slot_fep_ant(): the slot's ring position, FO only with cont_fo_comp, OAI rotation */
        const uint32_t ring_len = 2 * fp->samples_per_frame;
        const uint32_t off = get_samples_slot_timestamp(fp, job.nr_slot_rx);
        const double fo = ue->cont_fo_comp ? job.fo_hz : 0.0;
        const int16_t *rx[4];
        for (int a = 0; a < fp->nb_antennas_rx && a < 4; a++) rx[a] = (const int16_t *)nr_pdsch_passive_iq_for_slot(job.nr_slot_rx, ue->common_vars.rxdata)[a];
        const c16_t *rot = fp->symbol_rotation[link_type_dl] + (job.nr_slot_rx % fp->slots_per_subframe) * fp->symbols_per_slot;
        nr_gpu_pdsch_job_t packed[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
        int map[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1], np = 0;
        for (int k = 0; k <= n_more; k++) if (gj_ok[k]) { map[np] = k; packed[np++] = gj[k]; }
        /* Hand off to the ONE dedicated GPU worker thread (see its header comment) instead of
         * locking and executing the GPU call inline on this consumer thread: this consumer just
         * blocks on its own completion condvar, so a bad core assignment for THIS thread no longer
         * stalls the other five while it happens to be holding the GPU's only lock. */
        gpu_fep_req_t req = {.jobs = packed, .n_jobs = np, .config_epoch = job.config_epoch,
                             .source_abs_slot = job.absolute_slot, .slots_per_frame = slots_per_frame, .owned_iq = owned,
                             .ring_len = ring_len, .ring_off = off,
                             .abs_sample = off, .fo_hz = fo, .rot = (const int16_t *)rot,
                             .out = t_gpu_llr, .out_cap = GPU_LLR_CAP,
                             .rxdataF = g_gpu_chk_limit > 0 && atomic_load(&g_gpu_chk_n) < (uint64_t)g_gpu_chk_limit ? t_gpu_rxF : NULL,
                             .read_rc = -1, .ofdm_symbol_size = fp->ofdm_symbol_size};
        for (int a = 0; a < fp->nb_antennas_rx && a < 4; a++) req.rx[a] = rx[a];
        const int64_t rc = gpu_fep_submit_and_wait(&req);
        if (rc < 0 || !atomic_load(&g_gpu)) {
          if (owned) atomic_fetch_add(&g_drop_reason[NR_PDSCH_GPU_FALLBACK], np);
          for (int k = 0; k <= n_more; k++) gj_ok[k] = 0;
        } else {
          group_fep_ns = req.fep_end_ns;
          group_gpu_jobs = np;
          if (req.read_rc == 0) group_rxF = t_gpu_rxF;
          for (int p = 0; p < np; p++) { gj[map[p]] = packed[p]; if (packed[p].n_llr == 0) gj_ok[map[p]] = 0; }
        }
      }
    }

    for (int gi = -1; gi < n_more; gi++) {
    if (gi >= 0) { job = more[gi]; enqueue_ns = more_ns[gi]; }
    NR_CFG_EPOCH_WORK(job.config_epoch, &g_dropped_epoch);
    if (!nr_cfg_epoch_work_current()) {
      continue;
    }
    const nr_gpu_pdsch_job_t *gpu_job = gj_ok[gi + 1] && atomic_load(&g_gpu) ? &gj[gi + 1] : NULL;
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
    job.grant.check_sample_lifetime = !owned;
    job.grant.source_absolute_slot = job.absolute_slot;

    if (job.bwp_probe_entry > 0) {
      /* Passive BWP discovery: per-PRB DM-RS coherence on the DM-RS symbol. dmrs-TypeA-Position is
       * pos2 or pos3 -- score both and keep the one carrying DM-RS (the larger total coherence).
       * ponytail: mapping type A only; a type-B-only BWP needs the TDA's own DM-RS symbol. */
      float coh[2][275];
      double tot[2] = {0, 0};
      const int nrb = fp->N_RB_DL < 275 ? fp->N_RB_DL : 275;
      for (int k = 0; k < 2; k++) {
        const int sym = 2 + k;
        nr_slot_fep(ue, fp, job.nr_slot_rx, sym, rxdataF, link_type_dl, 0, nr_pdsch_passive_iq_for_slot(job.nr_slot_rx, ue->common_vars.rxdata));
        nr_dmrs_prb_coherence(&rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                              fp->first_carrier_offset, nrb, fp->symbols_per_slot, job.nr_slot_rx, sym, 0,
                              fp->Nid_cell, fp->Ncp == NR_NORMAL, coh[k]);
        for (int p = 0; p < nrb; p++) tot[k] += coh[k][p];
      }
      nr_pdcch_bwp_probe_result(job.bwp_probe_entry, job.bwp_probe_payload, tot[1] > tot[0] ? coh[1] : coh[0]);
      nr_slot_fep_fo_override_hz = saved_fo;
      continue;
    }
    /* DM-RS SYMBOL ORACLE (2026-09-15): which symbols of this slot carry DM-RS over the grant's PRBs is
     * measurable before any decode (per-symbol coherence with that symbol's own DM-RS sequence). The
     * observed mask prunes the Technique-D catalog to the (S,L,add_pos,max_len) entries that produce
     * it, so the layout search no longer multiplies with it. Antenna 0, ~14 symbol FFTs; only while
     * the context is unsettled. */
    /* Only in the DCI's OWN slot (k0 = 0 hypothesis): a k0 > 0 job measures slot + k0, where a busy
     * cell has some other PDSCH -- the rank-4 bed recorded that slot's mask (0x804) with k0 = 1 and
     * pruned the true entries away (0/10k probes). K39: a mask seen in the DCI's slot is k0-plausible, not proof of k0 = 0. */
    {
      static _Atomic uint32_t s_gate_n;
      if ((atomic_fetch_add(&s_gate_n, 1) % 2000) == 0)
        LOG_A(PHY, "SENSING: ORACLE_GATE settled=%d gen=%lu nrb=%u k0=%u probe=%u\n", job.sweep_ticket.settled,
              (unsigned long)job.sweep_ticket.generation, job.freq_alloc.num_rbs, job.sweep_ticket.k0, job.layout_probe);
    }
    int oracle_nrb;
    const int oracle_rb0 = probe_span(&job, &oracle_nrb);
    if (!job.sweep_ticket.settled && job.sweep_ticket.generation && oracle_nrb >= 4
        && job.sweep_ticket.k0 == 0) {
      /* dmrs_oracle_measure() indexes nr_dmrs_prb_coherence()'s coh[] and the physical subcarrier by
       * ABSOLUTE carrier CRB (see that function's doc comment), but probe_span() returns a BWP-relative
       * rb0 -- convert once here so both call sites below (the mask oracle and the k0 probe, which reuse
       * this rb0) read the right PRBs whenever BWPStart != 0. No-op at BWPStart = 0 (the lab cell). */
      const int rb0 = nr_dmrs_oracle_crb(job.dlsch_pdu.BWPStart, oracle_rb0), nrb = oracle_nrb;
      double prof[14] = {0}, med = 1.0;
      int last_sym = -1;
      const uint16_t mask = dmrs_oracle_measure(ue, fp, rxdataF_sz, rxdataF, job.nr_slot_rx, rb0, nrb,
                                                job.dlsch_pdu.nscid, job.dlsch_pdu.dlDmrsScramblingId, &last_sym, prof, &med);
      static _Atomic int s_oracle_log = 12;
      if (mask && atomic_load(&s_oracle_log) > 0) {
        atomic_fetch_sub(&s_oracle_log, 1);
        LOG_A(PHY, "SENSING: DMRS_ORACLE slot=%d rb=%d+%d mask=0x%x last_sym=%d k0=%u med=%.2f prof=[%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f]\n",
              job.nr_slot_rx, rb0, nrb, mask, last_sym, job.sweep_ticket.k0, med, prof[0], prof[1], prof[2], prof[3], prof[4], prof[5], prof[6], prof[7],
              prof[8], prof[9], prof[10], prof[11], prof[12], prof[13]);
      }
      if (mask) {
        /* K39: the hypothesised k0 is recorded as plausible only; DM-RS in this slot never prunes other k0. */
        nr_pdsch_config_sweep_observe(&job.sweep_ticket, mask, last_sym, job.sweep_ticket.k0);
      } else {
        /* k0 ORACLE (Task 14). No DM-RS on this grant's PRBs (rb0/nrb = probe_span(): the largest segment of a
         * PRB-list/PRG grant) in the DCI's own slot, so its PDSCH is k0 >= 1
         * slots later. The catalog only enumerates k0 {0,1} (TS 38.214 allows 0..32); rather than all 33,
         * probe the following slots and append ONLY the k0 values the air shows. Reach K = min(32, spf - 2):
         * the ring keeps a slot for spf - 2 slots after it was written (nr_passive_samples_valid), so a
         * farther slot cannot be read. The consumer usually runs only 1-2 slots behind the producer, so the
         * probe WAITS (bounded, same pattern as the stale check) for each target slot to be written, and
         * re-checks retention before and after its FEP. EVERY hit up to K is collected, not only the first:
         * on a busy cell slot+1 usually carries some PDSCH on these PRBs and would hide a true k0 >= 2. A hit
         * is not proof (another UE's PDSCH may sit there), which is why it only ADDS hypotheses -- the TB CRC
         * still decides, and a converged context drops the layers it did not win on. FEP goes to a scratch
         * buffer: rxdataF carries this job's own slot, which the decode's per-thread FEP cache may reuse for
         * the rest of the slot group. ISAC_PDSCH_K0_PROBE=0 disables it (read once).
         * ponytail: 1 in 8 eligible jobs probes (up to K x 14 FFTs + up to K slots of waiting each). */
        static _Atomic int s_k0_probe_on = -1; /* _Atomic: N consumers */
        if (s_k0_probe_on < 0) {
          const char *e = getenv("ISAC_PDSCH_K0_PROBE");
          s_k0_probe_on = (e != NULL && atoi(e) == 0) ? 0 : 1;
        }
        static _Atomic uint32_t s_probe_tick;
        static __thread c16_t *t_probe;
        if (s_k0_probe_on && !t_probe)
          t_probe = (c16_t *)malloc16_clear((size_t)rxdataF_sz * sizeof(c16_t));
        if (s_k0_probe_on && t_probe && (atomic_fetch_add(&s_probe_tick, 1) % 8) == 0) {
          const long K = slots_per_frame - 2 < 32 ? slots_per_frame - 2 : 32;
          const int slot_iters = (int)(100 / slots_per_frame) > 0 ? (int)(100 / slots_per_frame) : 1; /* 100 us steps per slot */
          uint64_t hits = 0; /* bit k: DM-RS seen on these PRBs at slot+k */
          uint16_t hit_mask[33] = {0};
          long reached = 0;
          for (long k = 1; k <= K; k++) {
            const long target = (long)job.absolute_slot + k;
            long p = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
            for (int w = 0; w < 30 + (int)k * slot_iters && p < target; w++) {
              struct timespec ts = {0, 100000};
              nanosleep(&ts, NULL);
              p = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
            }
            if (!nr_passive_samples_valid(p, target, slots_per_frame))
              break;
            double pf[14] = {0}, md = 1.0;
            int ls = -1;
            const uint16_t m = dmrs_oracle_measure(ue, fp, rxdataF_sz, (c16_t(*)[rxdataF_sz])t_probe,
                                                   (int)((job.nr_slot_rx + k) % slots_per_frame), rb0, nrb,
                                                   job.dlsch_pdu.nscid, job.dlsch_pdu.dlDmrsScramblingId, &ls, pf, &md);
            if (!nr_passive_samples_valid(atomic_load(&nr_ue_diag_producer_absolute_slot), target, slots_per_frame))
              break; /* overwritten during the FEP: this measurement is not of slot+k */
            reached = k;
            if (m) {
              hits |= UINT64_C(1) << k;
              hit_mask[k] = m;
            }
          }
          int added = 0, first_k = 0;
          for (int k = 2; k <= K; k++)
            if (hits & (UINT64_C(1) << k)) {
              added += nr_pdsch_config_sweep_add_k0(&job.sweep_ticket, (uint8_t)k);
              if (!first_k)
                first_k = k;
            }
          const uint64_t probes = atomic_fetch_add(&g_k0_probes, 1) + 1;
          atomic_fetch_add(&g_k0_probe_retained, (uint64_t)reached);
          uint64_t mx = atomic_load(&g_k0_probe_retained_max);
          while ((uint64_t)reached > mx && !atomic_compare_exchange_weak(&g_k0_probe_retained_max, &mx, (uint64_t)reached)) {}
          if (hits >> 2)
            atomic_fetch_add(&g_k0_probe_hits, 1);
          static _Atomic int s_k0_log = 20;
          if (((hits >> 2) || (probes % 1000) == 1) && atomic_fetch_sub(&s_k0_log, 1) > 0)
            LOG_A(PHY, "SENSING: K0_PROBE slot=%d rb=%d+%d K=%ld reached=%ld hits=0x%llx first_k0>=2=%d mask=0x%x added=%d "
                  "(probes=%lu with k0>=2 hits=%lu reached mean=%.1f max=%lu)\n",
                  job.nr_slot_rx, rb0, nrb, K, reached, (unsigned long long)hits, first_k, first_k ? hit_mask[first_k] : 0,
                  added, (unsigned long)probes, (unsigned long)atomic_load(&g_k0_probe_hits),
                  (double)atomic_load(&g_k0_probe_retained) / (double)probes,
                  (unsigned long)atomic_load(&g_k0_probe_retained_max));
        }
      }
    }
    nr_pdsch_passive_decode_result_t dec = {0};
    { /* ISAC_PROBE_ALL=1: every job is a first-code-block probe, pinned confs included -- isolates
       * the probe mechanics from the layout search. */
      static _Atomic int s_probe_all = -1; /* _Atomic: every passivePdsch consumer resolves it (TSAN) */
      if (s_probe_all < 0)
        s_probe_all = (getenv("ISAC_PROBE_ALL") != NULL) ? 1 : 0;
      if (s_probe_all)
        job.layout_probe = 1;
    }
    /* GRANTWORK-LITE (ISAC_TD_GRANTWORK, default ON, 0 disables; levers plan R1): this grant's shared per-geometry work
     * (FEP / chest / LLRs per signature, immutable, unified memory) -- the main decode computes and publishes
     * its own signature; CB0 of other hypotheses can then be extracted from it (nr_pdsch_passive_gw_cb0).
     * ISAC_TD_GW_PROBE=1 also serves layout probes from it (whole slot instead of the probe horizon). */
    static _Atomic int s_gw_on = -1, s_gw_probe = -1; /* _Atomic: N consumers */
    if (s_gw_on < 0) {
      const char *e = getenv("ISAC_TD_GW_PROBE");
      s_gw_probe = (e != NULL && atoi(e) != 0) ? 1 : 0;
      e = getenv("ISAC_TD_GRANTWORK"); /* default ON (operator 2026-10-04: fastest combination); 0 disables */
      s_gw_on = (e != NULL && *e && atoi(e) == 0) ? 0 : 1;
    }
    nr_td_grantwork_t *gw = NULL;
    if (s_gw_on && !gpu_job) {
      /* I4: the borrowed FEP buffer belongs to THIS job: any older gw can no longer compute on it */
      __atomic_fetch_add(&g_fep_gen[idx], 1, __ATOMIC_RELEASE);
      gw = nr_pdsch_passive_grantwork_begin(ue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, job.fo_hz,
                                            job.sweep_ticket.k0, &rxdataF[0][0], &g_fep_gen[idx]);
      nr_pdsch_passive_set_grantwork(gw, s_gw_probe != 0);
    }
    /* CB0 ELIMINATION CHANNEL (ISAC_TD_CB0_ELIM, default ON, 0 disables; nr_td_cb0_wire.h): the CB0 hypothesis set of this grant is
     * fixed HERE, before any decode of the grant; the TB decodes on the CPU only when this grant's CB0 batch will run on the
     * CPU backend (dominance rule; a CUDA CB0 batch dominates either TB decoder). */
    bool cb0_tb_cpu = false;
    const nr_td_cb0_job_t cb0_job = {.abs_slot = job.absolute_slot, .job_k0 = job.sweep_ticket.k0, .layout_probe = job.layout_probe != 0,
                                     .gpu_job = gpu_job != NULL, .gw_on = s_gw_on != 0};
    const bool cb0_on = nr_td_cb0_wire_pre(&job.sweep_ticket, gw, &cb0_job, &cb0_tb_cpu);
    if (cb0_tb_cpu)
      nr_pdsch_passive_force_cpu_tb(true);
    nr_pdsch_passive_probe_mode(job.layout_probe != 0);
    /* PT-RS density sweep only once the layout and the Technique-D context are settled (a pinned
     * conf has no ticket: generation 0). */
    nr_pdsch_passive_ptrs_sweep_allow(!job.layout_probe && (job.sweep_ticket.generation == 0 || job.sweep_ticket.settled));
    if (gpu_job)
      nr_pdsch_passive_set_llr_override(t_gpu_llr + gpu_job->llr_offset, gpu_job->n_llr);
    extern __thread int nr_dlsch_mrc_mode_override, nr_dlsch_irc_override;
    /* T26d ISAC_RX_MRC_PER_RNTI receivers: 1 best branch, 3 MRC (+ALIGN env), 4 noise-weighted MRC (IRC diag), 5 IRC */
    nr_dlsch_mrc_mode_override = job.mrc_mode >= 4 ? 1 : (job.mrc_mode > 0 ? job.mrc_mode : -1);
    nr_dlsch_irc_override = job.mrc_mode == 4 ? 3 : job.mrc_mode == 5 ? 2 : job.mrc_mode == 6 ? 4 : (job.mrc_mode > 0 ? 0 : -1);
    extern __thread int nr_dlsch_eq_override;
    { /* ISAC_RX_EQ_AB=1 (T26d): multi-layer post-solve grants (needs ISAC_RX_MRC_PER_RNTI) rotate the equaliser {fixed MMSE, float MMSE, ML} (paired A/B); the search keeps the default */
      static int s_eqab = -1;
      static _Atomic unsigned s_eqn = 0;
      if (s_eqab < 0) s_eqab = getenv("ISAC_RX_EQ_AB") ? atoi(getenv("ISAC_RX_EQ_AB")) : 0;
      nr_dlsch_eq_override = (s_eqab && job.mrc_mode > 0 && !job.layout_probe && __builtin_popcount(job.dlsch_pdu.dmrs_ports) >= 2) ? (int)(atomic_fetch_add(&s_eqn, 1) % 3u) : -1;
    }
    const int eq_used = nr_dlsch_eq_override;
    extern __thread int nr_pdsch_chest_occ_override;
    { /* ISAC_CHEST_OCC_AB=1 (T26d): multi-layer post-solve grants alternate stock / OCC-after-delay chest (paired A/B) */
      static int s_ocab = -1;
      static _Atomic unsigned s_ocn = 0;
      if (s_ocab < 0) s_ocab = getenv("ISAC_CHEST_OCC_AB") ? atoi(getenv("ISAC_CHEST_OCC_AB")) : 0;
      nr_pdsch_chest_occ_override = (s_ocab && job.mrc_mode > 0 && !job.layout_probe && __builtin_popcount(job.dlsch_pdu.dmrs_ports) >= 2) ? (int)(atomic_fetch_add(&s_ocn, 1) % 2u) : -1;
    }
    const int occ_used = nr_pdsch_chest_occ_override;
    extern __thread int nr_pdsch_ldpc_iters_override;
    { /* ISAC_LDPC_ITERS_AB=<a>,<b> (T26d): post-solve full decodes alternate the LDPC iteration limit (paired A/B) */
      static int s_ia = -1, s_ib = -1;
      static _Atomic unsigned s_in = 0;
      if (s_ia < 0) {
        const char *e = getenv("ISAC_LDPC_ITERS_AB");
        if (e == NULL || sscanf(e, "%d,%d", &s_ia, &s_ib) != 2) { s_ia = 0; s_ib = 0; }
      }
      nr_pdsch_ldpc_iters_override = (s_ia > 0 && s_ib > 0 && job.mrc_mode > 0 && !job.layout_probe)
                                         ? ((atomic_fetch_add(&s_in, 1) & 1u) ? s_ib : s_ia) : -1;
    }
    const int it_used = nr_pdsch_ldpc_iters_override;
    const uint64_t dec_t0 = now_ns();
    const nr_pdsch_passive_decode_status_t st_raw =
        nr_pdsch_passive_decode(ue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, rxdataF, &dec);
    if (!gpu_job) nr_pdsch_gpu_record_cpu(ue, &job, &dec, &rxdataF[0][0], rxdataF_sz);
    nr_dlsch_mrc_mode_override = -1;
    nr_dlsch_irc_override = -1;
    nr_dlsch_eq_override = -1;
    nr_pdsch_chest_occ_override = -1;
    nr_pdsch_ldpc_iters_override = -1;
    const double dec_us = (double)(now_ns() - dec_t0) / 1000.0;
    if (owned) {
      const uint64_t done = now_ns();
      const uint64_t cpu_fep = nr_pdsch_passive_last_fep_ns();
      const uint64_t fep = cpu_fep ? cpu_fep : (gpu_job ? group_fep_ns : 0);
      latency_note(NR_PDSCH_LAT_FEP, enqueue_ns, fep);
      if (st_raw == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st_raw == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL) {
        latency_note(NR_PDSCH_LAT_LDPC, fep, done);
        latency_note(NR_PDSCH_LAT_TOTAL, enqueue_ns, done);
      }
    }
    nr_pdsch_passive_set_llr_override(NULL, 0);
    if (cb0_tb_cpu)
      nr_pdsch_passive_force_cpu_tb(false);
    const uint8_t cb0_tb_decoder = cb0_on ? nr_pdsch_passive_last_decoder_used() : 0; /* the main decode's, before any rerun */
    const bool probe_outcome = nr_pdsch_passive_probe_outcome(); /* before the self-check re-runs the decode */
    bool gpu_health_failed = false;
    if (cb0_on) /* the CB0 batch: after the main decode published its signature, on this (job) thread, before job_end */
      nr_td_cb0_wire_run(gw);
    if (job.layout_probe && !gpu_job && st_raw != NR_PDSCH_PASSIVE_DECODE_ERROR && st_raw != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED)
      nr_pdsch_passive_probe_equiv_check(ue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, rxdataF); /* debug, env-gated */
    nr_pdsch_passive_set_grantwork(NULL, false);
    /* ISAC_GPU_SELFCHECK=N: first 32 comparable GPU-fed decodes by default; enforce is opt-in. */
    {
      if (gpu_job && g_gpu_chk_limit > 0 && atomic_load(&g_gpu_chk_n) < (uint64_t)g_gpu_chk_limit
          && st_raw != NR_PDSCH_PASSIVE_DECODE_ERROR && st_raw != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
        const bool gpu_ok = job.layout_probe ? probe_outcome : st_raw == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
        nr_pdsch_passive_decode_result_t dec2 = {0};
        fapi_nr_dl_config_dlsch_pdu_rel15_t pdu2 = job.dlsch_pdu;
        nr_pdsch_passive_probe_mode(job.layout_probe != 0);
        const nr_pdsch_passive_decode_status_t st2 =
            nr_pdsch_passive_decode(ue, &proc, &pdu2, &job.freq_alloc, &job.grant, rxdataF, &dec2);
        if (st2 != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
        const bool cpu_ok = job.layout_probe ? nr_pdsch_passive_probe_outcome() : st2 == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
        const int16_t *lc = NULL;
        uint32_t G = st2 == NR_PDSCH_PASSIVE_DECODE_ERROR ? 0 : nr_pdsch_passive_last_llr(&lc);
        if (G > (1u << 22) || lc == NULL) G = 0; /* a bogus blind grant can leave an out-of-range G (OTA SIGSEGV 2026-10-08) */
        const uint32_t n = gpu_job->n_llr < G ? gpu_job->n_llr : G;
        /* nr_codeword_unscrambling() processes ceil(G/32) full SIMD blocks. */
        const size_t G_padded = ((size_t)G + 31u) & ~(size_t)31u;
        int16_t *lg = G ? calloc(G_padded, sizeof(int16_t)) : NULL;
        double sa = NAN, mc = 0, mg = 0, dmax = 0, sab[8] = {0}; /* sign agreement per bit position of the RE */
        if (lc && lg && n && gpu_job->Qm && gpu_job->Nl) {
          memcpy(lg, t_gpu_llr + gpu_job->llr_offset, (size_t)n * sizeof(int16_t));
          if (n < G) memset(lg + n, 0, (size_t)(G - n) * sizeof(int16_t));
          nr_dlsch_unscrambling(lg, G, 0, pdu2.dlDataScramblingId, job.grant.rnti);
          double sas[14] = {0}, sal[4] = {0}; uint32_t nss[14] = {0}, nsl[4] = {0}; /* b0 agreement per symbol / per layer */
          const uint32_t per_re = gpu_job->Nl * gpu_job->Qm, per_sym = 12u * gpu_job->nb_rb * per_re; /* no data on DM-RS symbols assumed */
          for (uint32_t i = 0; i < n; i++) {
            mc += abs(lc[i]); mg += abs(lg[i]);
            const int e = (lc[i] < 0) == (lg[i] < 0); sab[i % gpu_job->Qm] += e;
            if (i % gpu_job->Qm == 0) { const uint32_t sy = i / per_sym, ly = (i / gpu_job->Qm) % gpu_job->Nl; if (sy < 14) { sas[sy] += e; nss[sy]++; } sal[ly] += e; nsl[ly]++; }
          }
          for (int b = 0; b < gpu_job->Qm; b++) sab[b] /= (double)n / gpu_job->Qm;
          char extra[400]; int u = snprintf(extra, sizeof extra, " b0/sym[");
          for (int q = 0; q < 14 && nss[q]; q++) u += snprintf(extra + u, sizeof extra - u, " %.2f", sas[q] / nss[q]);
          u += snprintf(extra + u, sizeof extra - u, " ] b0/layer[");
          for (int q = 0; q < gpu_job->Nl; q++) u += snprintf(extra + u, sizeof extra - u, " %.2f", nsl[q] ? sal[q] / nsl[q] : 0);
          snprintf(extra + u, sizeof extra - u, " ]");
          LOG_A(PHY, "SENSING: GPU_SELFCHECK_DETAIL%s\n", extra);
          const double f = mg > 0 ? mc / mg : 1.0; /* match the GPU scale to the CPU's mean |LLR| */
          for (uint32_t i = 0; i < n; i++) { const double d = fabs((double)lc[i] - f * lg[i]); if (d > dmax) dmax = d; }
          sa = nr_gpu_fep_sign_agreement(lc, lg, n); mc /= n; mg /= n;
          gpu_dump_record(ue, &job, gpu_job, lc, lg, G);
        }
        free(lg);
        /* FEP stage on its own: GPU rxdataF vs the CPU's for antenna 0 on the first DM-RS symbol over
         * the grant's subcarriers -- normalised correlation (1 = same up to a scale) and scale ratio. */
        double corr = -1, ratio = 0;
        {
          const int sym = __builtin_ctz((unsigned)job.dlsch_pdu.dlDmrsSymbPos | (1u << 15));
          if (group_rxF) {
            double cr = 0, ci = 0, pa = 0, pb = 0;
            const int k0 = fp->first_carrier_offset + 12 * (job.dlsch_pdu.BWPStart + job.freq_alloc.first_rb);
            for (int k = 0; k < 12 * job.freq_alloc.num_rbs; k++) {
              const int kk = (k0 + k) % fp->ofdm_symbol_size;
              const double ar = rxdataF[0][sym * fp->ofdm_symbol_size + kk].r, ai = rxdataF[0][sym * fp->ofdm_symbol_size + kk].i;
              const double br = group_rxF[2 * (sym * fp->ofdm_symbol_size + kk)], bi = group_rxF[2 * (sym * fp->ofdm_symbol_size + kk) + 1];
              cr += ar * br + ai * bi; ci += ai * br - ar * bi; pa += ar * ar + ai * ai; pb += br * br + bi * bi;
            }
            if (pa > 0 && pb > 0) { corr = sqrt(cr * cr + ci * ci) / sqrt(pa * pb); ratio = sqrt(pb / pa); }
          }
        }
        const bool crc_match = st2 != NR_PDSCH_PASSIVE_DECODE_ERROR && gpu_ok == cpu_ok;
        const nr_gpu_fep_check_t check = nr_gpu_fep_reference_result(
            owned ? job.absolute_slot : atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
            job.absolute_slot, slots_per_frame, st2 != NR_PDSCH_PASSIVE_DECODE_ERROR && lc && lg && n
                && group_rxF && gpu_job->Qm && gpu_job->Nl, corr, ratio, sa, crc_match);
        if (check == NR_GPU_FEP_SKIPPED) {
          atomic_fetch_add(&g_gpu_chk_skipped, 1);
        } else {
          uint64_t checked = atomic_load(&g_gpu_chk_n);
          const int limit = atomic_load(&g_gpu_chk_limit);
          while (checked < (uint64_t)limit && !atomic_compare_exchange_weak(&g_gpu_chk_n, &checked, checked + 1)) {}
          if (checked < (uint64_t)limit) {
            atomic_fetch_add(&g_gpu_chk_crc_agree, crc_match);
            atomic_fetch_add(&g_gpu_chk_crc_gpu_ok, gpu_ok);
            atomic_fetch_add(&g_gpu_chk_crc_cpu_ok, cpu_ok);
            if (check == NR_GPU_FEP_FAIL) {
              const uint64_t prior = atomic_fetch_add(&g_gpu_chk_fail, 1);
              if (atomic_load(&g_gpu_chk_enforce)) gpu_health_failed = true;
              if (nr_gpu_fep_should_disable(atomic_load(&g_gpu_chk_enforce), prior)) {
                const int was_disabled = atomic_exchange(&g_gpu_disabled, 1);
                const nr_gpu_fep_api_t *was_gpu = atomic_exchange(&g_gpu, NULL);
                if (!was_disabled && was_gpu)
                  LOG_E(PHY, "SENSING: GPU_FEP disabled: 2 self-check tolerance failures; CPU path for remainder of run\n");
              }
            }
            LOG_A(PHY, "SENSING: GPU_SELFCHECK slot=%ld rnti=0x%x probe=%u nrb=%u Nl=%u Qm=%u crc_gpu=%d crc_cpu=%d evm_gpu=%.3f evm_cpu=%.3f sign_agree=%.4f mean|llr| cpu=%.1f gpu=%.1f max|dllr|(scaled)=%.0f over %u | per-bit [%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] | rxdataF corr=%.4f gpu/cpu scale=%.3f\n",
                  job.absolute_slot, job.rnti, job.layout_probe, job.freq_alloc.num_rbs, gpu_job->Nl, gpu_job->Qm, gpu_ok, cpu_ok,
                  dec.evm_valid ? dec.evm_pct : -1.0, dec2.evm_valid ? dec2.evm_pct : -1.0, sa, mc, mg, dmax, n,
                  sab[0], sab[1], sab[2], sab[3], sab[4], sab[5], sab[6], sab[7], corr, ratio);
          }
        }
        } else {
          atomic_fetch_add(&g_gpu_chk_skipped, 1);
        }
      } else if (gpu_job && atomic_load(&g_gpu_chk_limit) > 0
                 && atomic_load(&g_gpu_chk_n) < (uint64_t)atomic_load(&g_gpu_chk_limit)) {
        atomic_fetch_add(&g_gpu_chk_skipped, 1);
      }
    }
    /* A layout probe's outcome is code block 0's CRC, mapped onto the TB status the feedback below
     * reads; the TB itself was not decoded and must not be submitted or counted. */
    if (gpu_job && !atomic_load(&g_gpu)) gpu_health_failed = true;
    const nr_pdsch_passive_decode_status_t st =
        gpu_health_failed ? NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED
        : !job.layout_probe ? st_raw
        : (st_raw == NR_PDSCH_PASSIVE_DECODE_ERROR || st_raw == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) ? st_raw
        : (probe_outcome ? NR_PDSCH_PASSIVE_DECODE_CRC_OK : NR_PDSCH_PASSIVE_DECODE_CRC_FAIL);
    nr_pdsch_passive_probe_mode(false);
    if (dec.evm_valid && (!job.layout_probe || getenv("ISAC_PDSCH_EVM_PROBES"))
        && (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL)) {
      /* ISAC_PDSCH_EVM_GRANT (T26c): one line per full decode, decoded AND failed. snr = -20 log10(evm). */
      LOG_A(PHY, "SENSING: EVMGRANT rnti=0x%04x %d.%d crc=%d mcs=%u tbl=%u Qm=%u Nl=%u rb=%u+%u nsym=%u evm=%.1f snr=%.1f "
                 "br_snr=%.1f,%.1f,%.1f,%.1f nbrx=%u tok=%u snr_cpe=%.1f probe=%d kappa=%.2f alpha=%.2f,%.2f,%.2f,%.2f "
                 "nvar_db=%.1f,%.1f,%.1f,%.1f rho=%.2f comb=%d ports=0x%x cdm=%u ndmrs=%u eq=%d occ=%d it=%d dec_us=%.0f\n",
            job.rnti, job.frame_rx, job.nr_slot_rx, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, job.grant.mcs, job.grant.mcs_table,
            dec.cw.qamModOrder, dec.cw.Nl, job.freq_alloc.first_rb, job.freq_alloc.num_rbs, dec.evm_nsym, dec.evm_pct,
            dec.evm_pct > 0.f ? -20.0 * log10(dec.evm_pct / 100.0) : 99.0, dec.br_snr_db[0], dec.br_snr_db[1], dec.br_snr_db[2],
            dec.br_snr_db[3], dec.evm_nbrx, job.solver_token,
            dec.evm_cpe_pct > 0.f ? -20.0 * log10(dec.evm_cpe_pct / 100.0) : 99.0, (int)job.layout_probe, dec.kappa,
            dec.br_alpha[0], dec.br_alpha[1], dec.br_alpha[2], dec.br_alpha[3], dec.br_nvar_db[0], dec.br_nvar_db[1],
            dec.br_nvar_db[2], dec.br_nvar_db[3], dec.rho, (int)job.mrc_mode, dec.dmrs_ports, dec.n_cdm, dec.dmrs_add, eq_used, occ_used, it_used, dec_us);
      if (dec.evm_pct > 0.f && !job.layout_probe) nr_pdsch_solver_note_snr(job.rnti, (float)(-20.0 * log10(dec.evm_pct / 100.0)));
    }
    if (job.solver_token) {
      const bool sv_credit = owned || nr_passive_credit_allowed(
          atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed), job.absolute_slot, slots_per_frame);
      /* A CRC pass is evidence even on IQ that expired meanwhile (overwritten samples cannot pass a CRC but by the
       * same chance any wrong hypothesis can); a FAILURE on expired IQ says nothing and is not credited. */
      const bool sv_ok = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
      if (getenv("ISAC_PDSCH_SOLVER_PROBELOG") && atoi(getenv("ISAC_PDSCH_SOLVER_PROBELOG")))
        LOG_A(PHY, "SENSING: PDSCH_SOLVER identity tok=%u rnti=0x%04x key=%d abs=%ld dmrs=%u data=%u nscid=%u crc=%d probe=%d\n",
              job.solver_token, job.rnti, job.coreset_key, (long)job.absolute_slot,
              job.dlsch_pdu.dlDmrsScramblingId, job.dlsch_pdu.dlDataScramblingId,
              job.dlsch_pdu.nscid, sv_ok, (int)job.layout_probe);
      static int s_sdiag = -1;
      static _Atomic int s_sdiag_left = 400;
      if (s_sdiag < 0) s_sdiag = getenv("ISAC_PDSCH_SOLVER_DIAG") ? 1 : 0;
      if (s_sdiag && !job.layout_probe && atomic_fetch_sub(&s_sdiag_left, 1) > 0)
        LOG_A(PHY, "SENSING: PDSCH_SOLVER_FULL rnti=0x%04x %d.%d crc=%d mcs=%u tbl=%u rb=%u+%u ports=0x%x cdm=%u S=%u L=%u dmrs=0x%x csirs_rm=%u slot160=%ld st=%d\n",
              job.rnti, job.frame_rx, job.nr_slot_rx, sv_ok, job.grant.mcs, job.grant.mcs_table, job.freq_alloc.first_rb,
              job.freq_alloc.num_rbs, job.dlsch_pdu.dmrs_ports, job.dlsch_pdu.n_dmrs_cdm_groups, job.dlsch_pdu.start_symbol,
              job.dlsch_pdu.number_symbols, job.dlsch_pdu.dlDmrsSymbPos, job.dlsch_pdu.numCsiRsForRateMatching,
              (long)(job.dci_abs_slot % 160), (int)st);
      nr_pdsch_solver_feedback(job.solver_token, sv_ok,
                               (sv_ok || sv_credit) && nr_cfg_epoch_work_current() && st != NR_PDSCH_PASSIVE_DECODE_ERROR
                                   && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED);
    }
    if (!nr_cfg_epoch_work_current()) {
      nr_slot_fep_fo_override_hz = saved_fo;
      nr_td_grantwork_job_end(gw);
      nr_td_grantwork_release(gw);
      continue;
    }
    if (job.layout_probe) {
      static _Atomic uint64_t s_probe_n, s_probe_ok;
      atomic_fetch_add_explicit(&s_probe_n, 1, memory_order_relaxed);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) atomic_fetch_add_explicit(&s_probe_ok, 1, memory_order_relaxed);
      const uint64_t pn = atomic_load_explicit(&s_probe_n, memory_order_relaxed);
      if (pn == 100 || pn == 400 || (pn % 2000) == 0)
        LOG_A(PHY, "SENSING: LAYOUT_PROBE n=%lu cb0_ok=%lu\n", (unsigned long)atomic_load(&s_probe_n), (unsigned long)atomic_load(&s_probe_ok));
    } else if (!gpu_health_failed)
      nr_passive_replay_dl(&job, &dec);
    nr_slot_fep_fo_override_hz = saved_fo;
    /* Owned IQ passed both copy guards and cannot expire during LDPC. Epoch
     * validity was checked above independently. The legacy borrowed path retains
     * K33: the producer keeps overwriting the ring while we decode (and the GPU path decodes
     * asynchronously). Re-check lifetime NOW: a CRC computed from overwritten IQ is not evidence for or
     * against any hypothesis, layout or scrambling id -> INCONCLUSIVE, no learning-state update of
     * any kind (TD, layout, Qm, data-id, BWP CRC, crc_note/scrambling walk, DM-RS identity). The
     * decoded TB itself is still delivered downstream (CRC-OK is a property of the bits, not credit). */
    const bool credit_ok = owned || nr_passive_credit_allowed(
        atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed), job.absolute_slot, slots_per_frame);
    if (!credit_ok && (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL))
      atomic_fetch_add_explicit(&g_stale_after_decode, 1, memory_order_relaxed);
    if (credit_ok && job.bwp_entry > 0 && st != NR_PDSCH_PASSIVE_DECODE_ERROR && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED)
      nr_pdcch_bwp_crc_result(job.bwp_entry, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
    if (owned && st == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED)
      atomic_fetch_add(&g_drop_reason[NR_PDSCH_DROP_UNSUPPORTED], 1);
    if (owned && st == NR_PDSCH_PASSIVE_DECODE_ERROR)
      atomic_fetch_add(&g_drop_reason[NR_PDSCH_DROP_ERROR], 1);
    if (!owned && st == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED && !nr_passive_samples_valid(
            atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed),
            job.absolute_slot, slots_per_frame))
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);

    if (st != NR_PDSCH_PASSIVE_DECODE_ERROR && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
      atomic_fetch_add_explicit(&g_decoded, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_rnti_dec[job.rnti], 1, memory_order_relaxed);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) {
        atomic_fetch_add_explicit(&g_rnti_ok[job.rnti], 1, memory_order_relaxed);
      }
      /* Scrambling-identity walk eligibility + link health (final review I1): every decode path reports
       * here; the in-line decode in nr_pdcch_blind_monitor_rt.c makes the same call. */
      if (!job.layout_probe && credit_ok)
        nr_pdsch_passive_crc_note(job.rnti, job.grant.scr_dedicated, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
      /* TIMING ADVANCE FROM AN OVERHEARD PDU (nr_passive_mac_ta.h). The payload of a CRC-verified
       * transport block was being discarded; a RAR carries the gNB's absolute advance for the UE it
       * answers, and a TA Command CE carries an update -- i.e. that UE's range to the illuminator,
       * measured without transmitting. Parse-only, on the consumer thread, and silent unless the PDU
       * actually contains one. */
      /* RRCSetup harvest: the ONLY unciphered appearance of the dedicated config, which is the
       * only thing that can retire the DCI 0_1/1_1 sweeps (their field widths are never broadcast).
       * Run on EVERY CRC-OK block rather than gating on a verified TC-RNTI: a successful uper_decode
       * of a full RRCSetup is self-validating, so the decoder is stronger evidence than an RNTI we
       * might never catch. Silent on the ~100 % of blocks that are not CCCH. */
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && dec.tb != NULL && dec.cw.TBS > 0)
        nr_passive_rrc_harvest(dec.tb, dec.cw.TBS / 8);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && dec.tb != NULL && dec.cw.TBS > 0)
        nr_passive_mac_report_ta(job.rnti, job.rnti_class == NR_BLIND_RNTI_CLASS_RA, job.frame_rx,
                                 job.nr_slot_rx, (int)ue->frame_parms.numerology_index,
                                 (uint32_t)job.absolute_slot, dec.tb,
                                 dec.cw.TBS / 8);   /* TBS is in BITS; the parser walks octets */
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
        int pr_nrb;
        const int pr_rb0 = probe_span(&job, &pr_nrb);
        if (pr_nrb > 0 && pdu->dmrsConfigType == 0 && pdu->dlDmrsSymbPos && pthread_mutex_trylock(&g_dl_rank_lock) == 0) {
          const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
          const int sym = __builtin_ctz((unsigned)pdu->dlDmrsSymbPos);
          const int rb_offset = pr_rb0 + (pdu->refPoint ? 0 : pdu->BWPStart);
          const int start_sc  = fp->first_carrier_offset + (pdu->BWPStart + pr_rb0) * 12;
          const double coh = nr_dmrs_port_pair_coherence(&rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                                                         start_sc, rb_offset, pr_nrb, fp->N_RB_DL,
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
            for (int m = 0; m < 12 * pr_nrb; ++m) {
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
        /* DM-RS identity: accumulated on EVERY decode attempt, CRC-OK or not -- the DM-RS sequence
         * is fixed by (nid, slot, symbol) regardless of whether the payload later decodes, so
         * gating this on `crc` would be circular: under a wrong id the CRC never passes, so the
         * identity that would explain the failures could never be measured.
         * Only DEDICATED-class grants (final review I2): SIB1/RAR/paging/TC/CSS-fallback DM-RS is
         * scrambled with N_ID^cell, and mixing it in would average two different identities.
         * dmrsConfigType is no longer restricted to type 1 here either -- forced by the shared
         * estimator's signature change (nr_pdcch_gap_dmrs2 fix), and left inconsistent with type 1
         * would just move this same gap one file over. See nr_pusch_passive_decode.c's UL twin of
         * this comment for the full reasoning. */
        if (credit_ok && job.solver_token && job.rnti_class == NR_BLIND_RNTI_CLASS_C && pdu->dlDmrsSymbPos && dmrs_scan_rate() > 0) {
          const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
          const int sym = __builtin_ctz((unsigned)pdu->dlDmrsSymbPos);
          const int16_t *rx[8];
          for (int a = 0; a < fp->nb_antennas_rx && a < 8; a++) rx[a] = (const int16_t *)&rxdataF[a][sym * fp->ofdm_symbol_size];
          if (fp->nb_antennas_rx <= 8) dmrs_scan_job(fp, job.rnti, job.absolute_slot, job.nr_slot_rx, sym, rx);
        }
        if (credit_ok && job.solver_token && job.coreset_key >= 0 && pr_nrb > 0 && pdu->dlDmrsSymbPos && per_coreset_dmrs_on()) {
          /* T26c: the same blind DM-RS identity estimator, per discovered CORESET population */
          coreset_dmrs_t *cd = coreset_dmrs_slot(job.coreset_key, ue->frame_parms.Nid_cell);
          const int refresh = coreset_dmrs_refresh();
          if (cd && (refresh || __atomic_load_n(&cd->decided, __ATOMIC_ACQUIRE) < 0)
              && pthread_mutex_trylock(&cd->mu) == 0) {
            const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
            const int sym = __builtin_ctz((unsigned)pdu->dlDmrsSymbPos);
            const int rb_offset = pr_rb0 + (pdu->refPoint ? 0 : pdu->BWPStart);
            const int start_sc  = fp->first_carrier_offset + (pdu->BWPStart + pr_rb0) * 12;
            /* A batch of layout variants observes the SAME pilots, not independent
             * grants. Count at most one observation per signal slot. A frozen
             * coherence winner without CRC support must eventually be reconsidered. */
            const bool fresh = !refresh || (job.absolute_slot >= 0 && (uint64_t)job.absolute_slot > cd->last_slot);
            if (refresh && crc) cd->failed_slots = 0;
            if (refresh && fresh) {
              cd->last_slot = (uint64_t)job.absolute_slot;
              if (!crc && __atomic_load_n(&cd->decided, __ATOMIC_ACQUIRE) >= 0
                  && ++cd->failed_slots >= (uint32_t)refresh) {
                const int old = __atomic_load_n(&cd->decided, __ATOMIC_ACQUIRE);
                nr_dmrs_id_set_range(&cd->est.s1, 0, NR_DMRS_ID_CANDIDATES);
                __atomic_store_n(&cd->est.decided_p1, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&cd->decided, -1, __ATOMIC_RELEASE);
                cd->failed_slots = 0;
                LOG_A(PHY, "SENSING: DMRS_ID REFRESH nid=%d old=%d abs=%ld after %d independent failed slots\n",
                      job.coreset_key, old, (long)job.absolute_slot, refresh);
              }
            }
            if (fresh && nr_dmrs_id_2stage_accumulate(&cd->est, &rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size,
                                             start_sc, rb_offset, pr_nrb, fp->N_RB_DL, fp->symbols_per_slot,
                                             job.nr_slot_rx, sym, pdu->nscid, fp->Ncp == NR_NORMAL, pdu->dmrsConfigType)) {
              const int id = nr_dmrs_id_2stage_decided(&cd->est);
              /* T26d: the population can mix DM-RS identities (replays: 249 or 442 on CORESET 10163): keep the three
               * best-scoring ids; the solver tries them per RNTI and the CRC decides */
              double sc[3] = {-1, -1, -1};
              int tp[3] = {id, -1, -1};
              for (int c = 0; c < 1024; c++) {
                const double v = nr_dmrs_id_score(&cd->est.s1, c);
                for (int q = 0; q < 3; q++)
                  if (v > sc[q]) { for (int z = 2; z > q; z--) { sc[z] = sc[z - 1]; tp[z] = tp[z - 1]; } sc[q] = v; tp[q] = c; break; }
              }
              for (int q = 0; q < 3; q++) cd->top[q] = tp[q];
              if (refresh) cd->failed_slots = 0;
              __atomic_store_n(&cd->decided, id, __ATOMIC_RELEASE);
              LOG_A(PHY, "SENSING: DMRS_ID per-CORESET nid=%d top3 %d %d %d\n", job.coreset_key, tp[0], tp[1], tp[2]);
              LOG_A(PHY, "SENSING: DMRS_ID per-CORESET nid=%d PDSCH DM-RS id DECIDED %d (cell %d)\n", job.coreset_key, id,
                    fp->Nid_cell);
            }
            pthread_mutex_unlock(&cd->mu);
          }
        }
        const int dl_ns = pdu->nscid & 1;
        if (credit_ok && job.grant.scr_dedicated && pr_nrb > 0 && pdu->dlDmrsSymbPos
            && nr_dmrs_id_2stage_decided(&g_dl_dmrs_id[dl_ns]) < 0 && pthread_mutex_trylock(&g_dl_dmrs_id_lock[dl_ns]) == 0) {
          nr_dmrs_id_2stage_t *dst = &g_dl_dmrs_id[dl_ns];
          if (!g_dl_dmrs_id_init[dl_ns]) { nr_dmrs_id_2stage_init(dst, "PDSCH", ue->frame_parms.Nid_cell); g_dl_dmrs_id_init[dl_ns] = true; }
          const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
          const int sym = __builtin_ctz((unsigned)pdu->dlDmrsSymbPos);
          /* Same two quantities the estimator itself derives (nr_dl_channel_estimation.c). */
          const int rb_offset = pr_rb0 + (pdu->refPoint ? 0 : pdu->BWPStart);
          const int start_sc  = fp->first_carrier_offset + (pdu->BWPStart + pr_rb0) * 12;
          nr_dmrs_id_2stage_accumulate(dst, &rxdataF[0][sym * fp->ofdm_symbol_size], fp->ofdm_symbol_size, start_sc,
                                       rb_offset, pr_nrb, fp->N_RB_DL, fp->symbols_per_slot, job.nr_slot_rx, sym,
                                       pdu->nscid, fp->Ncp == NR_NORMAL, pdu->dmrsConfigType);
          pthread_mutex_unlock(&g_dl_dmrs_id_lock[dl_ns]);
        }
        /* Data (PDSCH) scrambling identity: feed the outcome back to the per-RNTI sweep the wiring
         * in nr_pdcch_blind_monitor_rt.c consulted when it chose this grant's dlDataScramblingId --
         * only for grants where that sweep's own candidate was actually used (data_id_advance),
         * so an attempt that used the PCI fallback never perturbs a sweep it did not use. */
        if (job.data_id_advance && credit_ok)
          nr_pdsch_passive_data_id_feed(job.rnti, crc);
      }
      /* Technique D scoring: the TB CRC is the only oracle that can tell a right payload
       * interpretation from a wrong one, and this is the one place it is known. */
      nr_pdsch_cfg_hypothesis_t winner;
      if (credit_ok)
        nr_pdcch_dci11_layout_feedback(job.sweep_ticket.layout_index, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
      /* a layout-probe job never enters the CB0 wiring (cb0_on false): its outcome is exempt from note_tb_decoder (it feeds only
       * the full-TB election counters; nr_pdsch_config_sweep.h, DECODER DOMINANCE) */
      if (credit_ok && nr_pdsch_config_sweep_feedback(&job.sweep_ticket, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, &winner)) {
        char bc12[192];
        nr_pdsch_passive_bc12_census(&job.sweep_ticket, &winner, bc12, sizeof(bc12));
        LOG_A(PHY, "SENSING: Technique D CONVERGED rnti=0x%x tda=%u S=%u L=%u mask=0x%x table=%u%s%s\n",
              job.sweep_ticket.rnti, job.sweep_ticket.tda_index, winner.tda_start, winner.tda_length,
              winner.dmrs_mask, winner.mcs_table, bc12, nr_td_cb0_wire_converged_suffix(&job.sweep_ticket));
        nr_pdsch_passive_bc9_converged(&job.sweep_ticket, winner.k0);
      }
      /* CB0 elimination feed: AFTER this grant's TB feedback, BEFORE the BC9 / Qm steps that may re-index the context. */
      if (cb0_on) {
        const nr_td_cb0_tb_t cb0_tb = {.tb_fed = credit_ok, .tb_pass = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, .tb_decoder = cb0_tb_decoder,
                                       .iq_ok_after = credit_ok, .rv = job.grant.rv, .nl = dec.cw.Nl};
        nr_td_cb0_wire_feed(&job.sweep_ticket, &cb0_tb);
      }
      /* BC9 census AFTER the KL feedback: it only reads the sweep and must not delay or reorder the KL path (a trial whose
       * own feedback settled the context is not counted: ticket_siblings refuses a settled context). */
      if (credit_ok)
        nr_pdsch_passive_bc9_note(&job.sweep_ticket, job.rnti, job.dci_abs_slot, &job.dlsch_pdu, job.grant.nb_rb_oh,
                                  st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
      /* BC9d: a TB CRC pass (not a first-code-block layout probe) proves the DCI real: only now may it feed the hard TDD /
       * DCI-adjacency exclusions. After the KL feedback and the census, before the Qm oracle (both may re-index). */
      if (credit_ok && !job.layout_probe)
        nr_pdsch_passive_bc9_confirm(&job.sweep_ticket, job.rnti, job.dci_abs_slot, ue->frame_parms.numerology_index,
                                     st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
      /* Qm-oracle prune runs AFTER this job's CRC feedback above: prune_tables() compacts and
       * re-indexes st->hyp[] without bumping the context generation, so pruning before the CRC
       * feedback for the SAME job would credit that outcome to a hypothesis index that has already
       * moved (nr_pdsch_config_sweep_feedback resolves job.sweep_ticket.hypothesis against the
       * pre-prune array). Ordering this after leaves the DM-RS observe at ~682 untouched -- that one
       * runs on a separate, earlier tap and is out of scope here. */
      if (credit_ok && !job.sweep_ticket.settled && job.sweep_ticket.generation && dec.qm_measured) {
        const int kept = nr_pdsch_config_sweep_observe_qm(&job.sweep_ticket, job.grant.mcs, dec.qm_measured);
        if (kept > 0)
          LOG_A(PHY, "SENSING: Technique D Qm oracle rnti=0x%x mcs=%u qm=%u -> %d hypotheses\n",
                job.sweep_ticket.rnti, job.grant.mcs, dec.qm_measured, kept);
      }
      if ((nr_passive_obs_enabled() || nr_ue_ctx_enabled() || nr_sense_enabled()) && !job.layout_probe &&
          (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL)) {
        /* Per-grant observation record (Task A3; schema in nr_passive_obs.h). Non-blocking. */
        struct timespec ts_;
        clock_gettime(CLOCK_MONOTONIC, &ts_);
        const NR_DL_FRAME_PARMS *ofp_ = &ue->frame_parms;
        /* DCI 1_0 with SI/RA/P-RNTI: HARQ process / NDI are reserved or absent (TS 38.212 7.3.1.2.1) */
        const bool no_harq_ = job.rnti_class >= NR_BLIND_RNTI_CLASS_SI;
        const nr_passive_obs_t o_ = {
            .abs_slot = job.absolute_slot,
            .t_mono_ns = (uint64_t)ts_.tv_sec * 1000000000ull + (uint64_t)ts_.tv_nsec,
            .frame = (int16_t)job.frame_rx, .slot = (int16_t)job.nr_slot_rx, .pci = (int16_t)ofp_->Nid_cell,
            .dir = NR_OBS_DIR_DL, .rnti = job.rnti, .rnti_class = (int8_t)job.rnti_class,
            .start_rb = (int16_t)(job.dlsch_pdu.BWPStart + job.freq_alloc.first_rb),
            .nb_rb = (int16_t)job.freq_alloc.num_rbs,
            .start_sym = (int8_t)job.dlsch_pdu.start_symbol, .nb_sym = (int8_t)job.dlsch_pdu.number_symbols,
            .mcs = (int8_t)job.grant.mcs, .mcs_table = (int8_t)job.grant.mcs_table,
            .qm = (int8_t)dec.cw.qamModOrder, .nl = (int8_t)dec.cw.Nl,
            .dmrs_symb_pos = job.dlsch_pdu.dlDmrsSymbPos, .dmrs_scrambling_id = job.dlsch_pdu.dlDmrsScramblingId,
            .tbs = (int32_t)dec.cw.TBS,                                  /* bits */
            .harq_pid = no_harq_ ? -1 : (int8_t)job.grant.harq_pid, .rv = (int8_t)job.grant.rv,
            .ndi = no_harq_ ? -1 : (int8_t)job.grant.ndi,               /* NOT cw.new_data_indicator (forced 1) */
            .crc = st == NR_PDSCH_PASSIVE_DECODE_CRC_OK ? NR_OBS_CRC_OK : NR_OBS_CRC_FAIL,
            .nvar = (float)dec.nvar, .snr_db = NAN, .fo_comp_hz = (float)job.fo_hz, .delay_samples = NAN,
            .carrier_hz = ofp_->dl_CarrierFreq ? (int64_t)ofp_->dl_CarrierFreq : -1,
            .scs_khz = (int16_t)(ofp_->subcarrier_spacing / 1000),
            .fs_hz = (int64_t)ofp_->samples_per_subframe * 1000};
        if (nr_passive_obs_enabled()) nr_passive_obs_push(&o_);
        if (nr_sense_enabled()) {
          nr_sense_grant_meta_t meta = {
            .stamp_valid=true, .epoch=nr_cfg_reconf_enabled()?(int)job.sense_epoch:-1,
            .identity_gen=nr_cfg_reconf_enabled()?(int)job.sense_identity_gen:-1,
            .dedicated_epoch=nr_cfg_reconf_enabled()?(int)job.sense_dedicated_epoch:-1,
            .rx_ts=-1, .k2=-1, .dci_slot=-1, .transform_precoding=-1,
            .freq_hopping=-1, .dmrs_type=-1, .dmrs_add_pos=-1,
            .dmrs_n_scid=-1, .ta_est_samples=-1, .cfo_est_hz=NAN};
          nr_sense_emit_grant_meta(&o_, &meta);
        }
        if (nr_ue_ctx_enabled()) {
          nr_ue_ctx_on_obs(&o_);
          if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && !no_harq_)
            nr_ue_ctx_on_param(job.rnti, NR_UEP_PDSCH_SCR_ID, job.dlsch_pdu.dlDataScramblingId,
                               NR_UEV_TRUSTED, NR_UEC_CONVERGED, job.absolute_slot);
        }
      }
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK && !job.layout_probe) {
        atomic_fetch_add_explicit(&g_crc_ok, 1, memory_order_relaxed);
        if (job.want_data) {
          /* Publish THIS job's monotonic slot so the CPI grid indexes it correctly. Without this the
           * submit derives the index from proc->frame_rx, which wraps at 1024 -- harmless in order,
           * fatal once several consumers submit concurrently across a wrap. */
          nr_isac_abs_slot_override = (uint64_t)job.absolute_slot;
          nr_isac_pdsch_data_aided_submit(ue, &proc, &dec.cw, &job.dlsch_pdu, &job.freq_alloc, job.rnti,
                                          dec.tb, job.harq_pid_tag, rxdataF, (double)dec.nvar, dec.G);
          nr_isac_abs_slot_override = 0;
        }
      }
    }
    if (cb0_on) { /* decode error / unsupported: the TB path did not feed this grant (no-op when fed above) */
      const nr_td_cb0_tb_t cb0_tb = {.tb_fed = false, .tb_pass = false, .tb_decoder = cb0_tb_decoder, .iq_ok_after = credit_ok,
                                     .rv = job.grant.rv, .nl = dec.cw.Nl};
      nr_td_cb0_wire_feed(&job.sweep_ticket, &cb0_tb);
    }
    if (gw != NULL) {
      /* Debug / profiling (env-gated), after every use of this job's decoded TB: the reruns reuse the
       * private HARQ buffers. Then this job's reference goes; the gw is freed with its last one. */
      const bool gw_used = nr_pdsch_passive_last_used_grantwork(NULL);
      nr_slot_fep_fo_override_hz = job.fo_hz;
      if (!job.layout_probe && !cb0_on) { /* the CB0 batch's lazy entries overwrite the thread's last-decode buffers */
        nr_pdsch_passive_gw_check(gw, st_raw);
        nr_pdsch_passive_gw_profile(gw, gw_used);
      }
      nr_slot_fep_fo_override_hz = saved_fo;
      nr_td_grantwork_job_end(gw); /* I3: no lazy compute after this point (READY entries stay readable) */
      nr_td_grantwork_release(gw);
    }
    } /* slot group */
    if (slot_timing) {
      const uint64_t slot_t1 = now_ns();
      LOG_A(PHY, "SENSING: PDSCH_SLOT_TIME slot=%lu jobs=%d gpu_jobs=%d start_ns=%lu end_ns=%lu us=%.1f\n",
            (unsigned long)job.absolute_slot, n_more + 1, group_gpu_jobs,
            (unsigned long)slot_t0, (unsigned long)slot_t1, (double)(slot_t1 - slot_t0) / 1000.0);
    }
  }

  LOG_I(PHY, "SENSING: passive PDSCH decode consumer %d exiting\n", idx);
  return NULL;
}

static void report_sweep(const nr_pdsch_sweep_report_t *r)
{
  if (nr_ue_ctx_enabled() && r->rnti) {
    if (r->invalidated)
      nr_ue_ctx_on_param(r->rnti, NR_UEP_TD_STATE, 2, NR_UEV_SUSPECT, NR_UEC_REOPENED_NEW_WINNER, -1);
    else if (!r->operational && r->winner >= 0) {
      const nr_pdsch_cfg_hypothesis_t *h = &r->hypothesis;
      const int64_t packed = (int64_t)h->tda_start | ((int64_t)h->tda_length << 4) |
                             ((int64_t)h->k0 << 8) | ((int64_t)h->mapping_type << 14) |
                             ((int64_t)h->dmrs_add_pos << 15) | ((int64_t)(h->dmrs_max_len == 2) << 17) |
                             ((int64_t)h->dmrs_mask << 18);
      nr_ue_ctx_on_param(r->rnti, NR_UEP_TD_WINNER, packed, NR_UEV_TRUSTED, NR_UEC_CONVERGED, -1);
      nr_ue_ctx_on_param(r->rnti, NR_UEP_MCS_TABLE, h->mcs_table, NR_UEV_TRUSTED, NR_UEC_CONVERGED, -1);
      const int64_t dmrs = h->dmrs_add_pos | ((int64_t)(h->dmrs_max_len == 2) << 2) |
                           ((int64_t)h->dmrs_mask << 3);
      nr_ue_ctx_on_param(r->rnti, NR_UEP_DMRS_CFG, dmrs, NR_UEV_TRUSTED, NR_UEC_CONVERGED, -1);
      nr_ue_ctx_on_param(r->rnti, NR_UEP_TD_STATE, 1, NR_UEV_TRUSTED, NR_UEC_CONVERGED, -1);
    }
  }
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

/* nr_pdsch_passive_gpu_warmup: prepares the optional GPU front end (NR_GPU_FEP=1): loads libpdsch_gpu.so, initialises it for the carrier and starts the GPU worker thread.
 *   Reads ISAC_GPU_SELFCHECK (default 32 checks) and ISAC_GPU_SELFCHECK_ENFORCE. Falls back to the CPU path (g_gpu = NULL) when loading or init fails or there are more than 4 RX antennas.
 *   Caller: nr_pdcch_blind_monitor_rt.c (once, before the consumers start). Must run before nr_pdsch_passive_queue_start().
 */
void nr_pdsch_passive_gpu_warmup(PHY_VARS_NR_UE *ue)
{
  /* GPU6: the bit-exact FEP path replaces the (non CPU-equivalent) GPU LLR path */
  if (!nr_pdsch_gpu_exact_warmup(ue) && !atomic_load(&g_gpu_disabled)) g_gpu = nr_gpu_fep_prepare();
  { const char *e = getenv("ISAC_GPU_SELFCHECK"); g_gpu_chk_limit = e ? atoi(e) : 32; }
  { const char *e = getenv("ISAC_GPU_SELFCHECK_ENFORCE"); g_gpu_chk_enforce = e && !strcmp(e, "1"); }
  if (g_gpu) {
    void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
    g_gpu_admit = h ? (int (*)(const nr_gpu_pdsch_job_t *, int, int))dlsym(h, "nr_gpu_fep_admit") : NULL;
    if (!g_gpu_admit) { g_gpu = NULL; return; }
    const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
    const nr_gpu_fep_cfg_t cfg = {.nant = fp->nb_antennas_rx <= 4 ? fp->nb_antennas_rx : 4,
                                  .ofdm_symbol_size = fp->ofdm_symbol_size,
                                  .nb_prefix_samples = fp->nb_prefix_samples,
                                  .nb_prefix_samples0 = fp->nb_prefix_samples0,
                                  .symbols_per_slot = fp->symbols_per_slot,
                                  .first_carrier_offset = fp->first_carrier_offset,
                                  .n_rb_dl = fp->N_RB_DL,
                                  .samples_per_ms = fp->samples_per_subframe,
                                  .ofdm_offset_divisor = (int)fp->ofdm_offset_divisor};
    if (fp->nb_antennas_rx > 4 || g_gpu->init(&cfg) != 0) {
      LOG_W(PHY, "SENSING: PDSCH_GPU requested but libpdsch_gpu.so init failed -- CPU path\n");
      g_gpu = NULL;
    } else {
      LOG_A(PHY, "SENSING: GPU_FEP enabled: %d RX, N=%d, per-slot FEP + chest + MMSE + LLR on the GPU\n", cfg.nant, cfg.ofdm_symbol_size);
      if (atomic_fetch_or(&g_gpu_worker_running, 1) == 0) {
        /* Deliberately its OWN thread/core, not one of the passive-PDSCH consumers: see the worker's
         * header comment. Default core 3 -- isolcpus=2,3 keeps the scheduler off it, ISAC_UE_RT_CORE
         * only pins the receive thread to core 2, so 3 is otherwise unused by any pinned thread here. */
        static int s_gpu_worker_core = -2;
        if (s_gpu_worker_core == -2) {
          const char *e = getenv("ISAC_GPU_WORKER_CORE");
          s_gpu_worker_core = e ? atoi(e) : 3;
        }
        threadCreate(&g_gpu_worker_thread, gpu_fep_worker_thread, NULL, "gpuFepWorker", s_gpu_worker_core, 40);
      }
    }
  }
}

/* nr_pdsch_passive_queue_start: starts the passive PDSCH decode pool: allocates per-consumer rxdataF (and owned-IQ / GPU buffers) and launches n_consumers threads.
 *   Clamps depth to [2, MAX_DEPTH] and consumers to [1, MAX]; cores come from nr_core_plan_cpu() or ISAC_PDSCH_CORES. Idempotent: returns true when already running.
 *   Returns false when an allocation fails (partial allocations are freed). Caller: nr_pdcch_blind_monitor_rt.c (receive setup).
 */
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
    /* FEP is CPU-owned; never register/map the full IQ grid with CUDA. */
    const size_t rxF_bytes = (size_t)ue->frame_parms.nb_antennas_rx * rxdataF_sz * sizeof(c16_t);
    g_rxdataF_gw = false;
    g_rxdataF[i] = (c16_t *)malloc16_clear(rxF_bytes);
    if (g_gpu) {
      g_consumer_gpu_llr[i] = calloc(GPU_LLR_CAP, sizeof(int16_t));
      g_consumer_gpu_rxF[i] = calloc((size_t)ue->frame_parms.symbols_per_slot * ue->frame_parms.ofdm_symbol_size * 2, sizeof(int16_t));
      if (!g_consumer_gpu_llr[i] || !g_consumer_gpu_rxF[i]) g_gpu = NULL;
    }
    if (g_rxdataF[i] == NULL) {
      LOG_E(PHY, "SENSING: passive PDSCH queue: rxdataF allocation failed for consumer %d\n", i);
      for (int j = 0; j < i; j++) { if (g_rxdataF_gw) nr_td_gw_free(g_rxdataF[j]); else free(g_rxdataF[j]); g_rxdataF[j] = NULL; }
      return false;
    }
  }

  if (nr_csirs_disc_enabled()) {
    for (int i = 0; i < n_consumers; ++i)
      for (int a = 0; a < ue->frame_parms.nb_antennas_rx; ++a) {
        g_owned_iq[i][a] = malloc16_clear(2u * (size_t)ue->frame_parms.samples_per_frame * sizeof(c16_t));
        if (!g_owned_iq[i][a]) {
          for (int j = 0; j < n_consumers; ++j) {
            for (int b = 0; b < ue->frame_parms.nb_antennas_rx; ++b) {
              free(g_owned_iq[j][b]); g_owned_iq[j][b] = NULL;
            }
            free(g_rxdataF[j]); g_rxdataF[j] = NULL;
            free(g_consumer_gpu_llr[j]); g_consumer_gpu_llr[j] = NULL;
            free(g_consumer_gpu_rxF[j]); g_consumer_gpu_rxF[j] = NULL;
          }
          return false;
        }
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
     * sync, which is the entire problem this module exists to fix. Also below PREEMPT_RT's threaded
     * IRQs (FIFO 50): at an equal 50, consumers pinned to the NIC IRQ cores (6..11 vs IRQs on 8-13 on
     * the X410 host) held NAPI off until they blocked. One core each from base_core; <0 unpinned. */
    int core = nr_core_plan_cpu(NR_CP_PDSCH, i, (affinity >= 0) ? (affinity + i) : -1);
    { /* ISAC_PDSCH_CORES="6,7,8,9,15,16" (T26c): explicit, non-contiguous consumer cores (GB10 big cores are 5-9,15-19) */
      const char *cl = getenv("ISAC_PDSCH_CORES");
      for (int k = 0; cl && *cl; k++) {
        char *end;
        const long c = strtol(cl, &end, 10);
        if (end == cl) break;
        if (k == i) { core = (int)c; break; }
        cl = (*end == ',') ? end + 1 : end;
      }
    }
    threadCreate(&g_threads[i], nr_pdsch_passive_queue_thread, &g_args[i], name, core, 40);
  }
  LOG_I(PHY, "SENSING: passive PDSCH decode pool: %d consumer(s), depth %d, cores %d..%d, drop-oldest\n",
        n_consumers, depth, affinity, (affinity >= 0) ? affinity + n_consumers - 1 : -1);
  return true;
}

bool nr_pdsch_passive_queue_running(void)
{
  return atomic_load_explicit(&g_running, memory_order_acquire) != 0;
}

static bool enqueue_one(const nr_pdsch_passive_job_t *job, uint64_t enqueue_ns);

/* SLOT BATCHING (2026-09-15). The producer hands over one grant at a time and the consumers drain
 * the ring faster than a slot's grants arrive, so the consumer-side slot group never formed
 * (rfsim, 3 UEs: SLOTSHARE 0/15689). Jobs are held back on the producer thread until the slot
 * changes (or the occasion ends), then pushed contiguously under ONE lock and ONE wake-up, so the
 * consumer that takes the first grant of a slot finds its siblings right behind it. Costs one
 * slot of latency against a one-frame sample lifetime. */
static nr_pdsch_passive_job_t g_pending[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX];
static uint64_t g_pending_ns[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX];
static int g_n_pending = 0;
/* The producer side is several blind-PDCCH scan consumers since Task A7 (one flushes at its occasion
 * start while another enqueues from its Phase 2), so the pending batch has its own lock, taken before
 * g_lock and never the other way round. */
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_batches = 0, g_batches_multi = 0; ///< producer flushes, and those with >1 grant

/* nr_pdsch_passive_queue_flush_locked: pushes the producer's pending slot batch into the ring under one lock, so that the consumers see the whole slot at once.
 *   Updates batch counters (logged every 5000 batches). Requires g_pending_mu held; takes g_lock inside. Called by flush(), enqueue() and enqueue-time flushes.
 */
static void nr_pdsch_passive_queue_flush_locked(void) /* g_pending_mu held */
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
    enqueue_one(&g_pending[i], g_pending_ns[i]);
  g_n_pending = 0;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_lock);
}

void nr_pdsch_passive_queue_flush(void)
{
  pthread_mutex_lock(&g_pending_mu);
  nr_pdsch_passive_queue_flush_locked();
  pthread_mutex_unlock(&g_pending_mu);
}

/* nr_pdsch_passive_queue_fill_pct: returns the ring fill level in percent (queued plus pending grants over depth), or 0 when the pool is not running.
 *   Racy read, used only as an admission hint by the blind monitor's rate limiter. Caller: nr_pdcch_blind_monitor_rt.c.
 */
int nr_pdsch_passive_queue_fill_pct(void)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire) || g_depth <= 0) return 0;
  return (int)(100 * (g_count + g_n_pending) / g_depth); /* racy read: admission hint only */
}

/* nr_pdsch_passive_queue_enqueue: producer entry point: adds one passive PDSCH grant to the pending slot batch; the batch is flushed when the slot changes or the batch is full.
 *   Stamps the config epoch and sensing epoch, normalises the PRB allocation (refuses invalid PRB lists), and flushes at once when CSI-RS discovery is on.
 *   Returns false when the pool is not running or the grant is refused. Caller: nr_pdcch_blind_monitor_rt.c (per PDCCH grant).
 */
bool nr_pdsch_passive_queue_enqueue(const nr_pdsch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  pthread_mutex_lock(&g_pending_mu);
  if (g_n_pending > 0 && (g_pending[0].absolute_slot != job->absolute_slot
                          || g_n_pending == (atomic_load(&g_gpu) ? NR_PDSCH_PASSIVE_SLOT_GROUP_MAX : 8)))
    nr_pdsch_passive_queue_flush_locked();
  g_pending[g_n_pending] = *job;
  g_pending_ns[g_n_pending] = nr_csirs_disc_enabled() ? now_ns() : 0;
  const bool reconf=nr_cfg_reconf_enabled();
  const bool sense=nr_sense_enabled();
  g_pending[g_n_pending].config_epoch = nr_passive_job_epoch_stamp(reconf, nr_cfg_epoch_work_stamp);
  if (sense && reconf) {
    const uint64_t pair=nr_cfg_epoch_cfr_stamp();
    g_pending[g_n_pending].sense_epoch=(uint32_t)(pair>>32);
    g_pending[g_n_pending].sense_identity_gen=(uint32_t)pair;
    g_pending[g_n_pending].sense_dedicated_epoch=nr_cfg_epoch_dedicated_current();
  }
  /* ONE normalisation for everyone downstream: decoder, data-aided tap (recomputes nb_rb/G from
   * num_rbs), queue probes, narrow-grant budget. A no-op for a contiguous grant. */
  if (!nr_pdsch_passive_alloc_normalise(&g_pending[g_n_pending].freq_alloc, job->dlsch_pdu.BWPSize)) {
    static _Atomic unsigned long c_ = 0;
    const unsigned long n_ = atomic_fetch_add_explicit(&c_, 1, memory_order_relaxed) + 1;
    if (n_ == 1 || (n_ % 200) == 0)
      LOG_W(PHY, "SENSING: PDSCHQ refused an invalid PRB-list grant n=%lu (n_prb_list=%u bwp_size=%u)\n", n_,
            (unsigned)job->freq_alloc.n_prb_list, (unsigned)job->dlsch_pdu.BWPSize);
    pthread_mutex_unlock(&g_pending_mu);
    return false;
  }
  g_n_pending++;
  /* Never wait for the next PDCCH occasion (which can be many slots away).
   * Consumers still coalesce siblings already resident when they take the lock. */
  if (nr_csirs_disc_enabled()) nr_pdsch_passive_queue_flush_locked();
  pthread_mutex_unlock(&g_pending_mu);
  return true;
}

static _Atomic uint64_t g_dropped_probe; /* ISAC_PDSCHQ_PROBE_LAST: probes refused/evicted (also in dropped_full) */
/* probe_last_enabled: returns ISAC_PDSCHQ_PROBE_LAST (cached): when on, under overload layout probes are evicted before full decodes.
 *   Caller: enqueue_one().
 */
static bool probe_last_enabled(void)
{
  static int en = -1;
  if (en < 0) en = getenv("ISAC_PDSCHQ_PROBE_LAST") && atoi(getenv("ISAC_PDSCHQ_PROBE_LAST")) ? 1 : 0;
  return en == 1;
}

/* enqueue_one: inserts one job into the consumer ring (g_lock held by the caller), applying the overload rules when the ring is full.
 *   Narrow grants (< 1/8 of the band) are refused at >= 90 % fill; with ISAC_PDSCHQ_PROBE_LAST a probe may be evicted or refused first.
 *   Otherwise the oldest job of the RNTI with most entries is dropped (per-RNTI fairness). Counters: dropped_narrow, dropped_full. Caller: nr_pdsch_passive_queue_flush_locked().
 */
static bool enqueue_one(const nr_pdsch_passive_job_t *job, uint64_t enqueue_ns) /* g_lock held by the caller */
{
  /* DECODE BUDGET BY SENSING VALUE (2026-09-15). Under overload the sensing pipeline wants the
   * slot's CFR coverage, so a grant narrower than 1/8 of the band is refused while the ring is
   * >= 90 % full rather than evicting an older, wider one. Narrow grants of a shared slot still
   * get in whenever there is room, so multi-UE slots are not hollowed out below the threshold.
   * ponytail: fixed 1/8 and 90 %; make them coverage-driven if drops stay high. */
  if (g_count * 10 >= g_depth * 9 && job->freq_alloc.num_rbs * 8 < g_ue->frame_parms.N_RB_DL) {
    atomic_fetch_add_explicit(&g_dropped_narrow, 1, memory_order_relaxed);
    release_solver(job);
    return false;
  }
  if (g_count == g_depth && probe_last_enabled()) {
    /* ISAC_PDSCHQ_PROBE_LAST=1 (T26c): under overload a layout PROBE (blind solver search, CB0 only) is the cheapest
     * loss -- a later grant of the same RNTI probes again -- while a full decode of a solved RNTI is that grant's only
     * chance. Evict the oldest probe of the RNTI holding the most probes; with no probe resident, an incoming probe is
     * refused instead of evicting a full decode. Full decodes then fall through to the per-RNTI fair rule below. */
    int top_n = 0, victim = -1;
    uint16_t top_rnti = 0;
    for (int i = 0; i < g_count; i++) {
      const nr_pdsch_passive_job_t *e = &g_ring[(g_tail + i) % g_depth];
      if (!e->layout_probe) continue;
      int n = 0;
      for (int j = 0; j < g_count; j++) {
        const nr_pdsch_passive_job_t *f = &g_ring[(g_tail + j) % g_depth];
        n += f->layout_probe && f->rnti == e->rnti;
      }
      if (n > top_n) { top_n = n; top_rnti = e->rnti; }
    }
    if (top_n > 0)
      for (int i = 0; i < g_count && victim < 0; i++) {
        const nr_pdsch_passive_job_t *e = &g_ring[(g_tail + i) % g_depth];
        if (e->layout_probe && e->rnti == top_rnti) victim = i;
      }
    if (victim < 0 && job->layout_probe) {
      atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
      if ((atomic_fetch_add_explicit(&g_dropped_probe, 1, memory_order_relaxed) % 500) == 0)
        LOG_A(PHY, "SENSING: PDSCHQ_PROBE_LAST probes dropped=%llu (no full decode evicted for a probe)\n",
              (unsigned long long)atomic_load(&g_dropped_probe));
      return false;
    }
    if (victim >= 0) {
      release_solver(&g_ring[(g_tail + victim) % g_depth]);
      for (int i = victim; i > 0; i--) {
        g_ring[(g_tail + i) % g_depth] = g_ring[(g_tail + i - 1) % g_depth];
        g_ring_ns[(g_tail + i) % g_depth] = g_ring_ns[(g_tail + i - 1) % g_depth];
      }
      g_tail = (g_tail + 1) % g_depth;
      g_count--;
      atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
      atomic_fetch_add_explicit(&g_dropped_probe, 1, memory_order_relaxed);
    }
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
    release_solver(&g_ring[(g_tail + victim) % g_depth]);
    for (int i = victim; i > 0; i--) { /* shift the entries older than the victim up by one */
      g_ring[(g_tail + i) % g_depth] = g_ring[(g_tail + i - 1) % g_depth];
      g_ring_ns[(g_tail + i) % g_depth] = g_ring_ns[(g_tail + i - 1) % g_depth];
    }
    g_tail = (g_tail + 1) % g_depth;
    g_count--;
    atomic_fetch_add_explicit(&g_dropped_full, 1, memory_order_relaxed);
  }
  g_ring[g_head] = *job;
  g_ring_ns[g_head] = enqueue_ns;
  g_head         = (g_head + 1) % g_depth;
  g_count++;
  atomic_fetch_add_explicit(&g_queued, 1, memory_order_relaxed);
  return true;
}

/* nr_pdsch_passive_queue_get_stats: copies the queue statistics (latency histograms, drop reasons, queued/decoded/CRC counts, batches, GPU counters) into *out.
 *   Reads atomics only; out == NULL is a no-op. Callers: nr_passive_metrics.c and nr_pdcch_blind_monitor_rt.c (periodic summary).
 */
void nr_pdsch_passive_queue_get_stats(nr_pdsch_passive_queue_stats_t *out)
{
  if (out == NULL) {
    return;
  }
  for (int s = 0; s < NR_PDSCH_LAT_STAGES; ++s)
    for (int b = 0; b < NR_PDSCH_LAT_BINS; ++b)
      out->schedule.hist[s][b] = atomic_load_explicit(&g_latency[s][b], memory_order_relaxed);
  for (int r = 0; r < NR_PDSCH_DROP_REASONS; ++r)
    out->schedule.reason[r] = atomic_load_explicit(&g_drop_reason[r], memory_order_relaxed);
  out->queued        = atomic_load_explicit(&g_queued, memory_order_relaxed);
  out->decoded       = atomic_load_explicit(&g_decoded, memory_order_relaxed);
  out->crc_ok        = atomic_load_explicit(&g_crc_ok, memory_order_relaxed);
  out->dropped_full  = atomic_load_explicit(&g_dropped_full, memory_order_relaxed);
  out->dropped_narrow = atomic_load_explicit(&g_dropped_narrow, memory_order_relaxed);
  out->dropped_stale = atomic_load_explicit(&g_dropped_stale, memory_order_relaxed);
  out->stale_after_decode = atomic_load_explicit(&g_stale_after_decode, memory_order_relaxed);
  out->dropped_epoch = atomic_load_explicit(&g_dropped_epoch, memory_order_relaxed);
  out->max_lag_slots = atomic_load_explicit(&g_max_lag, memory_order_relaxed);
  out->slot_groups   = atomic_load_explicit(&g_slot_groups, memory_order_relaxed);
  out->batches       = atomic_load_explicit(&g_batches, memory_order_relaxed);
  out->batches_multi = atomic_load_explicit(&g_batches_multi, memory_order_relaxed);
  out->gpu_stale_pre = atomic_load_explicit(&g_gpu_stale_pre, memory_order_relaxed);
  out->gpu_stale_post = atomic_load_explicit(&g_gpu_stale_post, memory_order_relaxed);
  out->gpu_selfcheck_fail = atomic_load_explicit(&g_gpu_chk_fail, memory_order_relaxed);
  out->gpu_selfcheck_skipped = atomic_load_explicit(&g_gpu_chk_skipped, memory_order_relaxed);
  out->gpu_disabled = atomic_load_explicit(&g_gpu_disabled, memory_order_relaxed);
}

/* nr_pdsch_passive_queue_stop: stops the passive decode pool: sets g_stop, joins the consumer threads, frees their buffers and stops the GPU worker.
 *   Idempotent when not running. Frees the owned-IQ arenas per consumer. Caller: executables/nr-uesoftmodem.c (teardown). Clears g_running last.
 */
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
    if (g_rxdataF_gw)
      nr_td_gw_free(g_rxdataF[i]);
    else
      free(g_rxdataF[i]);
    g_rxdataF[i] = NULL;
    for (int a = 0; a < g_ue->frame_parms.nb_antennas_rx; ++a) {
      free(g_owned_iq[i][a]); g_owned_iq[i][a] = NULL;
    }
    free(g_consumer_gpu_llr[i]); g_consumer_gpu_llr[i] = NULL;
    free(g_consumer_gpu_rxF[i]); g_consumer_gpu_rxF[i] = NULL;
  }
  if (atomic_exchange(&g_gpu_worker_running, 0)) {
    pthread_mutex_lock(&g_gpu_wq_lock);
    pthread_cond_broadcast(&g_gpu_wq_work);
    pthread_mutex_unlock(&g_gpu_wq_lock);
    pthread_join(g_gpu_worker_thread, NULL);
  }
  atomic_store_explicit(&g_running, 0, memory_order_release);
}
