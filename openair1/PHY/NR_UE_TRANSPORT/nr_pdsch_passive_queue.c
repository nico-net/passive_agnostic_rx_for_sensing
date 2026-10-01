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
#include "PHY/NR_UE_TRANSPORT/nr_passive_mac_ta.h"    // MAC timing-advance parsers + this file's reporter
void nr_passive_rrc_harvest(const uint8_t *tb, uint32_t tb_bytes); // openair2/LAYER2/NR_MAC_UE
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_xoverhead.h"   // reject-only xOverhead elimination by TB CRC
#include "PHY/NR_UE_TRANSPORT/nr_dmrs_id_estimate.h"  // blind DM-RS scrambling-identity estimate
#include "PHY/NR_REFSIG/dmrs_nr.h"                     // get_num_dmrs_re_per_rb
#include "common/utils/nr/nr_common.h"                // get_num_dmrs
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.h" // Technique D scoring
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_qm_oracle.h" // Technique D Qm oracle
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h" // nr_pdcch_dci11_layout_feedback
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_prb_set.h" // nr_prb_segments (probe span of a PRB-list grant)
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_passive_decode.h" // nr_pdsch_passive_alloc_normalise
#include "PHY/NR_UE_TRANSPORT/nr_passive_obs.h" // per-grant observation API (Task A3)

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

/* ---- GPU front end (NR_GPU_FEP=1): FEP + chest + MMSE + LLR for a whole slot group in one launch
 * set, LLRs handed to the decode through nr_pdsch_passive_set_llr_override(). ---- */
static const nr_gpu_fep_api_t *g_gpu; /* NULL = CPU path */
static _Atomic uint64_t g_gpu_slots, g_gpu_jobs, g_gpu_fep_ns, g_gpu_llr_ns;
static _Atomic uint64_t g_gpu_chk_n, g_gpu_chk_crc_agree, g_gpu_chk_crc_gpu_ok, g_gpu_chk_crc_cpu_ok;
#define GPU_LLR_CAP (16u << 20) /* int16 per consumer: ~13 full-band rank-4 256QAM TBs, or any probe batch */

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

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
  const int16_t *rx[4]; uint32_t ring_len, ring_off, abs_sample; double fo_hz; const int16_t *rot;
  int16_t *out; size_t out_cap;
  volatile int done; int64_t rc;
} gpu_fep_req_t;
#define GPU_WORKER_QUEUE_MAX 32
static gpu_fep_req_t *g_gpu_wq[GPU_WORKER_QUEUE_MAX];
static int g_gpu_wq_n;
static pthread_mutex_t g_gpu_wq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gpu_wq_work = PTHREAD_COND_INITIALIZER, g_gpu_wq_done = PTHREAD_COND_INITIALIZER;
static _Atomic int g_gpu_worker_running;
static pthread_t g_gpu_worker_thread;

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
      const uint64_t t0 = now_ns();
      int64_t rc = g_gpu->fep_slot(r->rx, r->ring_len, r->ring_off, r->abs_sample, r->fo_hz, r->rot);
      const uint64_t t1 = now_ns();
      if (rc == 0) rc = g_gpu->pdsch_llr(r->jobs, r->n_jobs, r->out, r->out_cap);
      const uint64_t t2 = now_ns();
      r->rc = rc;
      if (rc >= 0) {
        atomic_fetch_add(&g_gpu_slots, 1);
        atomic_fetch_add(&g_gpu_jobs, (uint64_t)r->n_jobs);
        atomic_fetch_add(&g_gpu_fep_ns, t1 - t0);
        atomic_fetch_add(&g_gpu_llr_ns, t2 - t1);
        const uint64_t ns = atomic_load(&g_gpu_slots);
        if (ns == 50 || (ns % 1000) == 0)
          LOG_A(PHY, "SENSING: GPU_FEP slots=%lu jobs=%lu fep=%.0f us/slot chest+llr=%.0f us/slot (%.1f us/job) selfcheck n=%lu crc_agree=%lu gpu_ok=%lu cpu_ok=%lu\n",
                (unsigned long)ns, (unsigned long)atomic_load(&g_gpu_jobs), atomic_load(&g_gpu_fep_ns) / 1e3 / ns,
                atomic_load(&g_gpu_llr_ns) / 1e3 / ns, atomic_load(&g_gpu_llr_ns) / 1e3 / (double)atomic_load(&g_gpu_jobs),
                (unsigned long)atomic_load(&g_gpu_chk_n), (unsigned long)atomic_load(&g_gpu_chk_crc_agree),
                (unsigned long)atomic_load(&g_gpu_chk_crc_gpu_ok), (unsigned long)atomic_load(&g_gpu_chk_crc_cpu_ok));
      }
    }
    pthread_mutex_lock(&g_gpu_wq_lock);
    for (int i = 0; i < nt; i++) take[i]->done = 1;
    pthread_cond_broadcast(&g_gpu_wq_done);
    pthread_mutex_unlock(&g_gpu_wq_lock);
  }
  LOG_I(PHY, "SENSING: GPU_FEP worker thread exiting\n");
  return NULL;
}

/* Hand a request to the dedicated worker and block (condvar, not the queue's own mutex) until it is
 * done. Returns the same rc fep_slot()/pdsch_llr() would have. */
static int64_t gpu_fep_submit_and_wait(gpu_fep_req_t *req)
{
  req->done = 0;
  pthread_mutex_lock(&g_gpu_wq_lock);
  if (g_gpu_wq_n >= GPU_WORKER_QUEUE_MAX) {
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

/* ISAC_GPU_DUMP=<file>: one record per self-checked job -- the slot's time-domain window per antenna,
 * the job, and both LLR sets -- for offline comparison. */
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
        fwrite(&ue->common_vars.rxdata[a][(off + i) % ring_len], sizeof(c16_t), 1, f);
    fwrite(llr_cpu, sizeof(int16_t), G, f);
    fwrite(llr_gpu, sizeof(int16_t), G, f);
    fclose(f);
  }
  pthread_mutex_unlock(&lk);
}

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
void nr_pdcch_bwp_probe_result(int entry, uint64_t payload, const float *prb_coh); /* nr_pdcch_blind_monitor_rt.c */
void nr_pdcch_bwp_crc_result(int entry, bool crc_ok);
#include "nr_dmrs_id_estimate.h"
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
  if (tb == NULL || tb_bytes == 0)
    return;
  if (is_ra_rnti) {
    uint8_t rapid = 0;
    uint16_t ta = 0, tc_rnti = 0;
    if (nr_passive_mac_rar_ta(tb, tb_bytes, &rapid, &ta, &tc_rnti)) {
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
  }
}

/* DM-RS SYMBOL ORACLE measurement of ONE slot on the PRBs [rb0, rb0+nrb): per-symbol coherence with
 * that symbol's own DM-RS sequence (antenna 0, ~14 symbol FFTs into row 0 of rxdataF). Returns the
 * DM-RS symbol mask (0 = none seen) and, in *last_sym, the last symbol carrying energy on those PRBs
 * (-1 = none). Mapping-type agnostic: a type-B grant's first DM-RS on its first symbol is measured
 * like any other.
 * rb0 is an ABSOLUTE carrier CRB (matches nr_dmrs_prb_coherence()'s coh[] indexing), NOT the grant's
 * BWP-relative PRB index -- callers must convert with nr_dmrs_oracle_crb(BWPStart, rb0) first.
 * nid: the DM-RS identity the job itself decodes with (final review I6) -- the decided identity for the
 * job's nSCID on a dedicated-class grant, N_ID^cell otherwise, i.e. job.dlsch_pdu.dlDmrsScramblingId as
 * the scan thread set it. Scoring with the PCI on a scramblingID0 != PCI cell never fires the oracle. */
static uint16_t dmrs_oracle_measure(PHY_VARS_NR_UE *ue, NR_DL_FRAME_PARMS *fp, uint32_t rxdataF_sz,
                                    c16_t rxdataF[][rxdataF_sz], int nr_slot, int rb0, int nrb, int nscid, int nid,
                                    int *last_sym, double prof[14], double *med_out)
{
  const int n_sym = fp->symbols_per_slot;
  float coh[275];
  double energy[14] = {0}; /* per symbol, over the grant's PRBs: the allocation END is where it stops */
  for (int sym = 0; sym < n_sym && sym < 14; sym++) {
    nr_slot_fep_ant(ue, fp, nr_slot, sym, 0, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
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
    nr_pdsch_config_sweep_observe(ticket, mask, last_sym, 0); /* measured on the DCI's own slot: k0 = 0 */
}

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
    long prod = atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed);
    const long lag  = prod - job.absolute_slot;
    if (lag > (long)atomic_load_explicit(&g_max_lag, memory_order_relaxed)) {
      atomic_store_explicit(&g_max_lag, (uint64_t)(lag > 0 ? lag : 0), memory_order_relaxed);
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
      atomic_fetch_add_explicit(&g_dropped_stale, 1, memory_order_relaxed);
      continue;
    }

    /* ---- GPU front end: one FEP for the slot, one launch set for every eligible job of the group.
     * A job the GPU cannot take (PT-RS, CSI-RS RM, Nl > antennas, wants the data-aided CFR, BWP probe)
     * keeps n_llr = 0 and goes through the CPU chain below unchanged. ---- */
    static __thread int16_t *t_gpu_llr; /* per consumer */
    nr_gpu_pdsch_job_t gj[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
    int gj_ok[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1] = {0};
    static _Atomic int s_probe_all_g = -1; /* _Atomic: every passivePdsch consumer resolves it (TSAN) */
    if (s_probe_all_g < 0) s_probe_all_g = (getenv("ISAC_PROBE_ALL") != NULL) ? 1 : 0;
    if (g_gpu) {
      if (!t_gpu_llr) t_gpu_llr = malloc(GPU_LLR_CAP * sizeof(int16_t));
      int n_gj = 0;
      for (int gi = -1; gi < n_more; gi++) {
        const nr_pdsch_passive_job_t *jb = gi < 0 ? &job : &more[gi];
        nr_pdsch_passive_grant_t g = jb->grant;
        g.source_absolute_slot = jb->absolute_slot;
        gj_ok[gi + 1] = jb->bwp_probe_entry <= 0 && !jb->want_data
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
        for (int a = 0; a < fp->nb_antennas_rx && a < 4; a++) rx[a] = (const int16_t *)ue->common_vars.rxdata[a];
        const c16_t *rot = fp->symbol_rotation[link_type_dl] + (job.nr_slot_rx % fp->slots_per_subframe) * fp->symbols_per_slot;
        nr_gpu_pdsch_job_t packed[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1];
        int map[NR_PDSCH_PASSIVE_SLOT_GROUP_MAX + 1], np = 0;
        for (int k = 0; k <= n_more; k++) if (gj_ok[k]) { map[np] = k; packed[np++] = gj[k]; }
        /* Hand off to the ONE dedicated GPU worker thread (see its header comment) instead of
         * locking and executing the GPU call inline on this consumer thread: this consumer just
         * blocks on its own completion condvar, so a bad core assignment for THIS thread no longer
         * stalls the other five while it happens to be holding the GPU's only lock. */
        gpu_fep_req_t req = {.jobs = packed, .n_jobs = np, .ring_len = ring_len, .ring_off = off,
                             .abs_sample = off, .fo_hz = fo, .rot = (const int16_t *)rot,
                             .out = t_gpu_llr, .out_cap = GPU_LLR_CAP};
        for (int a = 0; a < fp->nb_antennas_rx && a < 4; a++) req.rx[a] = rx[a];
        const int64_t rc = gpu_fep_submit_and_wait(&req);
        if (rc < 0) {
          for (int k = 0; k <= n_more; k++) gj_ok[k] = 0;
        } else {
          for (int p = 0; p < np; p++) { gj[map[p]] = packed[p]; if (packed[p].n_llr == 0) gj_ok[map[p]] = 0; }
        }
      }
    }

    for (int gi = -1; gi < n_more; gi++) {
    if (gi >= 0) job = more[gi];
    const nr_gpu_pdsch_job_t *gpu_job = gj_ok[gi + 1] ? &gj[gi + 1] : NULL;
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

    if (job.bwp_probe_entry > 0) {
      /* Passive BWP discovery: per-PRB DM-RS coherence on the DM-RS symbol. dmrs-TypeA-Position is
       * pos2 or pos3 -- score both and keep the one carrying DM-RS (the larger total coherence).
       * ponytail: mapping type A only; a type-B-only BWP needs the TDA's own DM-RS symbol. */
      float coh[2][275];
      double tot[2] = {0, 0};
      const int nrb = fp->N_RB_DL < 275 ? fp->N_RB_DL : 275;
      for (int k = 0; k < 2; k++) {
        const int sym = 2 + k;
        nr_slot_fep(ue, fp, job.nr_slot_rx, sym, rxdataF, link_type_dl, 0, ue->common_vars.rxdata);
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
     * pruned the true entries away (0/10k probes). A mask seen in the DCI's slot proves k0 = 0. */
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
    nr_pdsch_passive_probe_mode(job.layout_probe != 0);
    /* PT-RS density sweep only once the layout and the Technique-D context are settled (a pinned
     * conf has no ticket: generation 0). */
    nr_pdsch_passive_ptrs_sweep_allow(!job.layout_probe && (job.sweep_ticket.generation == 0 || job.sweep_ticket.settled));
    if (gpu_job)
      nr_pdsch_passive_set_llr_override(t_gpu_llr + gpu_job->llr_offset, gpu_job->n_llr);
    const nr_pdsch_passive_decode_status_t st_raw =
        nr_pdsch_passive_decode(ue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, rxdataF, &dec);
    nr_pdsch_passive_set_llr_override(NULL, 0);
    const bool probe_outcome = nr_pdsch_passive_probe_outcome(); /* before the self-check re-runs the decode */
    if (job.layout_probe && !gpu_job && st_raw != NR_PDSCH_PASSIVE_DECODE_ERROR && st_raw != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED)
      nr_pdsch_passive_probe_equiv_check(ue, &proc, &job.dlsch_pdu, &job.freq_alloc, &job.grant, rxdataF); /* debug, env-gated */
    /* ISAC_GPU_SELFCHECK=N: the first N GPU-fed decodes are re-run on the CPU chain and compared --
     * TB/CB0 CRC agreement, LLR sign agreement and max |dLLR| after matching the two scales. */
    {
      static _Atomic int s_chk = -1; /* _Atomic: N consumers */
      if (s_chk < 0) { const char *e = getenv("ISAC_GPU_SELFCHECK"); s_chk = e ? atoi(e) : 0; }
      if (gpu_job && s_chk > 0 && atomic_load(&g_gpu_chk_n) < (uint64_t)s_chk
          && st_raw != NR_PDSCH_PASSIVE_DECODE_ERROR && st_raw != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) {
        const bool gpu_ok = job.layout_probe ? probe_outcome : st_raw == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
        nr_pdsch_passive_decode_result_t dec2 = {0};
        fapi_nr_dl_config_dlsch_pdu_rel15_t pdu2 = job.dlsch_pdu;
        nr_pdsch_passive_probe_mode(job.layout_probe != 0);
        const nr_pdsch_passive_decode_status_t st2 =
            nr_pdsch_passive_decode(ue, &proc, &pdu2, &job.freq_alloc, &job.grant, rxdataF, &dec2);
        const bool cpu_ok = job.layout_probe ? nr_pdsch_passive_probe_outcome() : st2 == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
        const int16_t *lc; const uint32_t G = nr_pdsch_passive_last_llr(&lc);
        const uint32_t n = gpu_job->n_llr < G ? gpu_job->n_llr : G;
        int16_t *lg = malloc((size_t)G * sizeof(int16_t));
        double sa = 0, mc = 0, mg = 0, dmax = 0, sab[8] = {0}; /* sign agreement per bit position of the RE */
        if (lc && lg && n) {
          memcpy(lg, t_gpu_llr + gpu_job->llr_offset, (size_t)n * sizeof(int16_t));
          if (n < G) memset(lg + n, 0, (size_t)(G - n) * sizeof(int16_t));
          nr_dlsch_unscrambling(lg, G, 0, pdu2.dlDataScramblingId, job.grant.rnti);
          double sas[14] = {0}, sal[4] = {0}; uint32_t nss[14] = {0}, nsl[4] = {0}; /* b0 agreement per symbol / per layer */
          const uint32_t per_re = gpu_job->Nl * gpu_job->Qm, per_sym = 12u * gpu_job->nb_rb * per_re; /* no data on DM-RS symbols assumed */
          for (uint32_t i = 0; i < n; i++) {
            mc += abs(lc[i]); mg += abs(lg[i]);
            const int e = (lc[i] < 0) == (lg[i] < 0); sa += e; sab[i % gpu_job->Qm] += e;
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
          sa /= n; mc /= n; mg /= n;
          gpu_dump_record(ue, &job, gpu_job, lc, lg, G);
        }
        free(lg);
        /* FEP stage on its own: GPU rxdataF vs the CPU's for antenna 0 on the first DM-RS symbol over
         * the grant's subcarriers -- normalised correlation (1 = same up to a scale) and scale ratio. */
        double corr = -1, ratio = 0;
        {
          const int sym = __builtin_ctz((unsigned)job.dlsch_pdu.dlDmrsSymbPos | (1u << 15));
          int16_t *gf = malloc((size_t)fp->symbols_per_slot * fp->ofdm_symbol_size * 2 * sizeof(int16_t));
          if (gf && g_gpu->read_rxdataF(0, gf) == 0) {
            double cr = 0, ci = 0, pa = 0, pb = 0;
            const int k0 = fp->first_carrier_offset + 12 * (job.dlsch_pdu.BWPStart + job.freq_alloc.first_rb);
            for (int k = 0; k < 12 * job.freq_alloc.num_rbs; k++) {
              const int kk = (k0 + k) % fp->ofdm_symbol_size;
              const double ar = rxdataF[0][sym * fp->ofdm_symbol_size + kk].r, ai = rxdataF[0][sym * fp->ofdm_symbol_size + kk].i;
              const double br = gf[2 * (sym * fp->ofdm_symbol_size + kk)], bi = gf[2 * (sym * fp->ofdm_symbol_size + kk) + 1];
              cr += ar * br + ai * bi; ci += ai * br - ar * bi; pa += ar * ar + ai * ai; pb += br * br + bi * bi;
            }
            if (pa > 0 && pb > 0) { corr = sqrt(cr * cr + ci * ci) / sqrt(pa * pb); ratio = sqrt(pb / pa); }
          }
          free(gf);
        }
        atomic_fetch_add(&g_gpu_chk_n, 1);
        atomic_fetch_add(&g_gpu_chk_crc_agree, gpu_ok == cpu_ok);
        atomic_fetch_add(&g_gpu_chk_crc_gpu_ok, gpu_ok);
        atomic_fetch_add(&g_gpu_chk_crc_cpu_ok, cpu_ok);
        LOG_A(PHY, "SENSING: GPU_SELFCHECK slot=%ld rnti=0x%x probe=%u nrb=%u Nl=%u Qm=%u crc_gpu=%d crc_cpu=%d sign_agree=%.4f mean|llr| cpu=%.1f gpu=%.1f max|dllr|(scaled)=%.0f over %u | per-bit [%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] | rxdataF corr=%.4f gpu/cpu scale=%.3f\n",
              job.absolute_slot, job.rnti, job.layout_probe, job.freq_alloc.num_rbs, gpu_job->Nl, gpu_job->Qm, gpu_ok, cpu_ok, sa, mc, mg, dmax, n,
              sab[0], sab[1], sab[2], sab[3], sab[4], sab[5], sab[6], sab[7], corr, ratio);
      }
    }
    /* A layout probe's outcome is code block 0's CRC, mapped onto the TB status the feedback below
     * reads; the TB itself was not decoded and must not be submitted or counted. */
    const nr_pdsch_passive_decode_status_t st =
        !job.layout_probe ? st_raw
        : (st_raw == NR_PDSCH_PASSIVE_DECODE_ERROR || st_raw == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) ? st_raw
        : (probe_outcome ? NR_PDSCH_PASSIVE_DECODE_CRC_OK : NR_PDSCH_PASSIVE_DECODE_CRC_FAIL);
    nr_pdsch_passive_probe_mode(false);
    if (job.layout_probe) {
      static _Atomic uint64_t s_probe_n, s_probe_ok;
      atomic_fetch_add_explicit(&s_probe_n, 1, memory_order_relaxed);
      if (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK) atomic_fetch_add_explicit(&s_probe_ok, 1, memory_order_relaxed);
      const uint64_t pn = atomic_load_explicit(&s_probe_n, memory_order_relaxed);
      if (pn == 100 || pn == 400 || (pn % 2000) == 0)
        LOG_A(PHY, "SENSING: LAYOUT_PROBE n=%lu cb0_ok=%lu\n", (unsigned long)atomic_load(&s_probe_n), (unsigned long)atomic_load(&s_probe_ok));
    } else
      nr_passive_replay_dl(&job, &dec);
    nr_slot_fep_fo_override_hz = saved_fo;
    /* K33: the producer keeps overwriting the ring while we decode (and the GPU path decodes
     * asynchronously). Re-check lifetime NOW: a CRC computed from overwritten IQ is not evidence for or
     * against any hypothesis, layout or scrambling id -> INCONCLUSIVE, no learning-state update of any kind (TD, layout, Qm, data-id, BWP CRC, crc_note/scrambling walk). The
     * decoded TB itself is still delivered downstream (CRC-OK is a property of the bits, not credit). */
    const bool credit_ok = nr_passive_credit_allowed(
        atomic_load_explicit(&nr_ue_diag_producer_absolute_slot, memory_order_relaxed), job.absolute_slot, slots_per_frame);
    if (!credit_ok && (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL))
      atomic_fetch_add_explicit(&g_stale_after_decode, 1, memory_order_relaxed);
    if (credit_ok && job.bwp_entry > 0 && st != NR_PDSCH_PASSIVE_DECODE_ERROR && st != NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED)
      nr_pdcch_bwp_crc_result(job.bwp_entry, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK);
    if (st == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED && !nr_passive_samples_valid(
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
        const int dl_ns = pdu->nscid & 1;
        if (job.grant.scr_dedicated && pr_nrb > 0 && pdu->dlDmrsSymbPos
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
      if (credit_ok && nr_pdsch_config_sweep_feedback(&job.sweep_ticket, st == NR_PDSCH_PASSIVE_DECODE_CRC_OK, &winner))
        LOG_A(PHY, "SENSING: Technique D CONVERGED rnti=0x%x tda=%u S=%u L=%u mask=0x%x table=%u\n",
              job.sweep_ticket.rnti, job.sweep_ticket.tda_index, winner.tda_start, winner.tda_length,
              winner.dmrs_mask, winner.mcs_table);
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
      if (nr_passive_obs_enabled() && !job.layout_probe && (st == NR_PDSCH_PASSIVE_DECODE_CRC_OK || st == NR_PDSCH_PASSIVE_DECODE_CRC_FAIL)) {
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
        nr_passive_obs_push(&o_);
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
  g_gpu = nr_gpu_fep_load();
  if (g_gpu) {
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
      LOG_W(PHY, "SENSING: GPU_FEP requested (NR_GPU_FEP=1) but libpdsch_gpu.so init failed -- CPU path\n");
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
     * sync, which is the entire problem this module exists to fix. Also below PREEMPT_RT's threaded
     * IRQs (FIFO 50): at an equal 50, consumers pinned to the NIC IRQ cores (6..11 vs IRQs on 8-13 on
     * the X410 host) held NAPI off until they blocked. One core each from base_core; <0 unpinned. */
    threadCreate(&g_threads[i], nr_pdsch_passive_queue_thread, &g_args[i], name,
                 (affinity >= 0) ? (affinity + i) : -1, 40);
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
/* The producer side is several blind-PDCCH scan consumers since Task A7 (one flushes at its occasion
 * start while another enqueues from its Phase 2), so the pending batch has its own lock, taken before
 * g_lock and never the other way round. */
static pthread_mutex_t g_pending_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_batches = 0, g_batches_multi = 0; ///< producer flushes, and those with >1 grant

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
    enqueue_one(&g_pending[i]);
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

bool nr_pdsch_passive_queue_enqueue(const nr_pdsch_passive_job_t *job)
{
  if (!atomic_load_explicit(&g_running, memory_order_acquire)) {
    return false;
  }
  pthread_mutex_lock(&g_pending_mu);
  if (g_n_pending > 0 && (g_pending[0].absolute_slot != job->absolute_slot
                          || g_n_pending == NR_PDSCH_PASSIVE_SLOT_GROUP_MAX))
    nr_pdsch_passive_queue_flush_locked();
  g_pending[g_n_pending] = *job;
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
  pthread_mutex_unlock(&g_pending_mu);
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
  out->stale_after_decode = atomic_load_explicit(&g_stale_after_decode, memory_order_relaxed);
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
  if (atomic_exchange(&g_gpu_worker_running, 0)) {
    pthread_mutex_lock(&g_gpu_wq_lock);
    pthread_cond_broadcast(&g_gpu_wq_work);
    pthread_mutex_unlock(&g_gpu_wq_lock);
    pthread_join(g_gpu_worker_thread, NULL);
  }
  atomic_store_explicit(&g_running, 0, memory_order_release);
}
