/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_td.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/MODULATION/nr_modulation.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>

typedef struct {
  c16_t *samples;
  nr_csirs_disc_slot_meta_t meta;
  unsigned offset[14], span;
  c16_t rot[14];
  double fo;
  bool compensate;
} td_slot;
static struct {
  NR_DL_FRAME_PARMS fp;
  pthread_mutex_t lock, commit;
  void *sample_pool;
  td_slot **slots, *storage, *batch[NR_CSIRS_DISC_DEFAULT_BATCH];
  int capacity, count, head, tail, n_ant;
  uint64_t epoch;
  size_t stride;
  int16_t *input, *output;
  bool unified, ready;
} td;
extern uint64_t csirs_disc_confirm_drops __attribute__((weak));
extern uint64_t csirs_disc_confirm_wait_max_us __attribute__((weak));
uint64_t csirs_disc_fep_gpu_chunks, csirs_disc_fep_cpu_chunks;
uint64_t csirs_disc_fep_us, csirs_disc_fep_max_us;
uint64_t csirs_disc_fft_us, csirs_disc_fft_batches, csirs_disc_td_overflows;


/* ISAC_CSIRS_DISC_RATELIMIT shared state (written by nr_csirs_disc_process, read by td_push). rl_div == 0: baseline duty. */
static int rl_div, rl_G, rl_rearm;
uint64_t csirs_disc_rl_skipped;
static int feed_div = -1, feed_win = 640;
static long burst_end = -1, burst_len = -1;

/* td_feed_cfg: reads ISAC_CSIRS_DISC_FEED_DIV (default 7) and _FEED_WIN (default 640, min 160) once. */
static void td_feed_cfg(void)
{
  if (feed_div >= 0) return;
  const char *e = getenv("ISAC_CSIRS_DISC_FEED_DIV"), *w = getenv("ISAC_CSIRS_DISC_FEED_WIN");
  feed_div = e && atoi(e) > 0 ? atoi(e) : 7;
  if (w && atoi(w) >= 160) feed_win = atoi(w);
}

/* nr_csirs_disc_td_feed_params: the SEARCH ingest duty (window slots, windows per period). */
void nr_csirs_disc_td_feed_params(uint32_t *win, uint32_t *div)
{
  td_feed_cfg();
  *win = (uint32_t)feed_win; *div = (uint32_t)feed_div;
}

/* nr_csirs_disc_td_set_duty: steady != 0 feeds G windows per div_s; rearm restarts the acquisition burst (one-shot). */
void nr_csirs_disc_td_set_duty(bool steady, uint32_t G, uint32_t div_s, bool rearm)
{
  __atomic_store_n(&rl_G, (int)G, __ATOMIC_RELAXED);
  __atomic_store_n(&rl_div, steady ? (int)div_s : 0, __ATOMIC_RELAXED);
  if (rearm) __atomic_store_n(&rl_rearm, 1, __ATOMIC_RELAXED);
}

/* usec: returns CLOCK_MONOTONIC time in microseconds (timing of the TD front end). */
static uint64_t usec(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* nr_csirs_disc_fft_cpu: CPU FFT of `transforms` consecutive symbols of size fft from `in` to `out` (the SIMDe DFT of the carrier size); the fallback of the TD front end.
 *   Caller: nr_csirs_disc_td_drain() (CPU FEP path) and tests/nr_csirs_disc_td_test.cc.
 */
void nr_csirs_disc_fft_cpu(int fft, int transforms, const int16_t *in, int16_t *out)
{
  const dft_size_idx_t size = get_dft(fft);
  for (int i = 0; i < transforms; ++i)
    dft(size, (int16_t *)in + (size_t)i * 2 * fft, out + (size_t)i * 2 * fft, 1);
}

/* nr_csirs_disc_td_init: allocates the time-domain front end for a carrier: the slot ring (CP-bearing sample spans per slot, huge-page pool, unified memory when the GPU allocator is loaded) and the FFT input and output buffers for one batch.
 *   Returns 0, or -1 for unsupported parameters or allocation failure (everything freed). Callers: nr_csirs_disc_start() in nr_csirs_disc_feed.c and tests/nr_csirs_disc_td_fixture.c.
 */
int nr_csirs_disc_td_init(const NR_DL_FRAME_PARMS *fp, int n_ant, int capacity)
{
  if (!fp || n_ant < 1 || n_ant > 4 || !fp->ofdm_symbol_size || !fp->ofdm_offset_divisor || capacity < 1)
    return -1;
  memset(&td, 0, sizeof(td));
  td.fp = *fp;
  td.n_ant = n_ant;
  td.capacity = capacity;
  td.stride = 14 * (fp->ofdm_symbol_size + fp->nb_prefix_samples0 + fp->nb_prefix_samples);
  pthread_mutex_init(&td.lock, NULL);
  pthread_mutex_init(&td.commit, NULL);
  td.ready = true;
  td.slots = calloc(capacity, sizeof(*td.slots));
  td.storage = calloc(capacity+NR_CSIRS_DISC_DEFAULT_BATCH,sizeof(*td.storage));
  if (!td.slots || !td.storage) goto fail;
  td.unified = nr_csirs_disc_alloc_unified && nr_csirs_disc_free_unified;
  /* The worker reads these owned CP-bearing spans directly on the GPU. */
  const size_t slot_bytes = ((td.stride*n_ant*sizeof(c16_t)+63)/64)*64;
  const size_t huge = 2*1024*1024;
  const size_t pool_bytes = ((slot_bytes*(capacity+NR_CSIRS_DISC_DEFAULT_BATCH)+huge-1)/huge)*huge;
  td.sample_pool = td.unified ? nr_csirs_disc_alloc_unified(pool_bytes) : aligned_alloc(huge, pool_bytes);
  if (!td.sample_pool) goto fail;
  /* A GB-scale queue otherwise incurs hundreds of thousands of 4K mappings.
   * Best-effort THP; correctness and allocation do not depend on availability. */
  if (!td.unified) madvise(td.sample_pool, pool_bytes, MADV_HUGEPAGE);
  memset(td.sample_pool, 0, pool_bytes);
  for (int i = 0; i < capacity + NR_CSIRS_DISC_DEFAULT_BATCH; ++i) {
    td_slot *s = &td.storage[i];
    if(i<capacity) td.slots[i]=s;else td.batch[i-capacity]=s;
    s->samples = (c16_t *)((char *)td.sample_pool+i*slot_bytes);
  }
  size_t bytes = (size_t)NR_CSIRS_DISC_DEFAULT_BATCH*n_ant*14*fp->ofdm_symbol_size*sizeof(c16_t);
  td.unified = nr_csirs_disc_alloc_unified && nr_csirs_disc_free_unified;
  td.input = td.unified ? nr_csirs_disc_alloc_unified(bytes) : aligned_alloc(64, bytes);
  td.output = td.unified ? nr_csirs_disc_alloc_unified(bytes) : aligned_alloc(64, bytes);
  if (!td.input || !td.output) goto fail;
  memset(td.input, 0, bytes);
  memset(td.output, 0, bytes);
  return 0;
fail:
  nr_csirs_disc_td_free();
  return -1;
}

/* nr_csirs_disc_td_free: releases the time-domain front end (pool, slots, batch buffers, locks). Safe to call when not initialised; resets the state.
 *   Callers: nr_csirs_disc_stop() and the start failure path in nr_csirs_disc_feed.c, nr_csirs_disc_td_init() on failure, and tests.
 */
void nr_csirs_disc_td_free(void)
{
  if (!td.ready) return;
  if (td.unified && td.sample_pool) nr_csirs_disc_free_unified(td.sample_pool);
  else free(td.sample_pool);
  free(td.slots);
  free(td.storage);
  if (td.unified) {
    if (td.input) nr_csirs_disc_free_unified(td.input);
    if (td.output) nr_csirs_disc_free_unified(td.output);
  } else { free(td.input); free(td.output); }
  pthread_mutex_destroy(&td.lock);
  pthread_mutex_destroy(&td.commit);
  memset(&td, 0, sizeof(td));
}

/* nr_csirs_disc_td_reset: discards the queued slots of the front end for a new sample epoch (count, head and tail cleared, epoch bumped) under the commit and queue locks.
 *   Called by nr_csirs_disc_ring_reset() for the worker ring (nr_csirs_disc_feed.c).
 */
void nr_csirs_disc_td_reset(void)
{
  if (!td.ready) return;
  pthread_mutex_lock(&td.commit);
  pthread_mutex_lock(&td.lock);
  td.count = td.head = td.tail = 0;
  ++td.epoch;
  pthread_mutex_unlock(&td.lock);
  pthread_mutex_unlock(&td.commit);
}

/* nr_csirs_disc_td_pending: returns the number of time-domain slots queued and not yet drained (0 when the front end is not initialised).
 *   Callers: disc_worker_main() (wake-up decision) and nr_csirs_disc_td_drain().
 */
int nr_csirs_disc_td_pending(void)
{
  if (!td.ready) return 0;
  pthread_mutex_lock(&td.lock);
  int n = td.count;
  pthread_mutex_unlock(&td.lock);
  return n;
}

/* nr_csirs_disc_td_push: producer side of the front end, called from the PHY receive path for each DL slot: copies the slot's CP-bearing sample window of all antennas into the queue.
 *   Duty-cycled ingest for real-time safety: ISAC_CSIRS_DISC_FEED_WIN (default 640 slots) and ISAC_CSIRS_DISC_FEED_DIV (default 7) skip windows; an acquisition burst (ISAC_CSIRS_DISC_FEED_BURST, default 5120 slots) runs at ISAC_CSIRS_DISC_FEED_ACQ_DIV.
 *   Returns 0 when queued, 1 when skipped by the duty cycle, -1 when dropped (format change, full queue, lock busy; counted as feed drops). Caller: phy_procedures_nr_ue.c (PHY receive path).
 */
int nr_csirs_disc_td_push(PHY_VARS_NR_UE *ue, uint32_t abs_slot, uint16_t frame, uint8_t slot)
{
  if (!td.ready || !ue) return -1;
  /* Bounded, contiguous ingest duty cycle. OTA 2026-10-07 (DGX + X410, 4 x 122.88 Msps DPDK): any
   * ~2 GB/s of CPU-coherent memory traffic (CPU memcpy or GPU access to host memory) kills the radio
   * stream, and full-rate ingest is exactly that (re-reading 1 MB/slot). Feed ISAC_CSIRS_DISC_FEED_WIN
   * (default 640) contiguous slots, then skip FEED_DIV-1 windows (default 7: ~14 %, odd so successive
   * windows rotate through P=640 residues). Contiguous windows keep TRS burst pairs and off-evidence. */
  td_feed_cfg();
  /* Acquisition phase: for the first ISAC_CSIRS_DISC_FEED_BURST slots (default 5120 = 2.56 s) after the
   * first push, feed at ISAC_CSIRS_DISC_FEED_ACQ_DIV duty (default 2: alternate 320-slot windows, ~50 %),
   * then the steady duty cycle. A full-rate 2560-slot burst killed the radio stream in ~1 of 7 starts
   * (OTA 2026-10-07 adapt2_5, t9wb); 50 % keeps the same amount of data at half the memory pressure. */
  static int acq_div = -1;
  if (burst_len < 0) {
    const char *b = getenv("ISAC_CSIRS_DISC_FEED_BURST"), *ad = getenv("ISAC_CSIRS_DISC_FEED_ACQ_DIV");
    burst_len = b && atol(b) >= 0 ? atol(b) : 5120;
    acq_div = ad && atoi(ad) > 0 ? atoi(ad) : 2;
  }
  if (burst_end < 0 || __atomic_exchange_n(&rl_rearm, 0, __ATOMIC_RELAXED)) burst_end = (long)abs_slot + burst_len;
  const bool in_burst = (long)abs_slot < burst_end;
  if (in_burst && acq_div > 1 && ((long)abs_slot - (burst_end - burst_len)) / 320 % acq_div != 0) return 1;
  /* Steady windows are aligned to the first fed slot. */
  static long feed_origin = -1;
  if (feed_origin < 0) feed_origin = (long)abs_slot;
  const long rel = (long)abs_slot - feed_origin;
  /* ISAC_CSIRS_DISC_RATELIMIT steady state: G windows every div_s (nr_csirs_disc_rate_internal.h). */
  const int rl_div_s = __atomic_load_n(&rl_div, __ATOMIC_RELAXED);
  if (!in_burst && rl_div_s > 0 && rel >= 0) {
    if ((rel / feed_win) % rl_div_s >= __atomic_load_n(&rl_G, __ATOMIC_RELAXED)) {
      __atomic_fetch_add(&csirs_disc_rl_skipped, 1, __ATOMIC_RELAXED);
      return 1;
    }
  } else
  if (!in_burst && feed_div > 1 && rel >= 0 && (rel / feed_win) % feed_div != 0) return 1;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  if (slot >= fp->slots_per_frame || fp->ofdm_symbol_size != td.fp.ofdm_symbol_size
      || fp->N_RB_DL != td.fp.N_RB_DL || fp->nb_antennas_rx != td.n_ant
      || fp->first_carrier_offset != td.fp.first_carrier_offset
      || fp->ofdm_offset_divisor != td.fp.ofdm_offset_divisor
      || fp->nb_prefix_samples != td.fp.nb_prefix_samples
      || fp->nb_prefix_samples0 != td.fp.nb_prefix_samples0
      || fp->numerology_index != td.fp.numerology_index)
    goto drop;
  if (pthread_mutex_trylock(&td.lock)) goto drop;
  if (td.count == td.capacity) {
    pthread_mutex_unlock(&td.lock);
    __atomic_fetch_add(&csirs_disc_td_overflows, 1, __ATOMIC_RELAXED);
    goto drop;
  }
  td_slot *s = td.slots[td.tail];
  s->compensate = ue->cont_fo_comp;
  s->fo = isnan(nr_slot_fep_fo_override_hz) ? ue->dl_Doppler_shift+ue->freq_offset : nr_slot_fep_fo_override_hz;
  unsigned offset = get_samples_slot_timestamp(fp, slot);
  const unsigned cp0 = ue->is_synchronized ? fp->nb_prefix_samples0 : fp->nb_prefix_samples;
  for (int l=0;l<14;++l) {
    offset += ((slot*14+l) % (7 << fp->numerology_index)) ? fp->nb_prefix_samples : cp0;
    s->offset[l] = offset - fp->nb_prefix_samples/fp->ofdm_offset_divisor;
    offset += fp->ofdm_symbol_size;
    s->rot[l] = fp->symbol_rotation[link_type_dl][(slot % fp->slots_per_subframe)*14+l];
  }
  s->span = s->offset[13] - s->offset[0] + fp->ofdm_symbol_size;
  if (s->span > td.stride) { pthread_mutex_unlock(&td.lock); goto drop; }
  const unsigned total = 2*fp->samples_per_frame;
  const unsigned start = s->offset[0] % total;
  const unsigned first = s->span < total-start ? s->span : total-start;
  for (int a=0;a<td.n_ant;++a) {
    memcpy(s->samples+a*td.stride, ue->common_vars.rxdata[a]+start, first*sizeof(c16_t));
    if (first < s->span)
      memcpy(s->samples+a*td.stride+first, ue->common_vars.rxdata[a], (s->span-first)*sizeof(c16_t));
  }
  s->meta = (nr_csirs_disc_slot_meta_t){abs_slot, frame, slot, 1};
  td.tail = (td.tail+1)%td.capacity;
  ++td.count;
  pthread_mutex_unlock(&td.lock);
  return 0;
drop:
  __atomic_fetch_add(&csirs_disc_feed_drops, 1, __ATOMIC_RELAXED);
  return -1;
}

/* nr_csirs_disc_td_drain: converts the queued time-domain slots into frequency-domain symbols in the ring, in batches: GPU FEP when the GPU chain is loaded, otherwise CPU FFT (with fo compensation and the symbol rotation).
 *   Each batch is written under the commit lock, dropped when the epoch changed, and pushed (ring write begin/end or nr_csirs_disc_ring_push). A device-resident ring never mixes failed GPU batches with CPU FEP.
 *   Returns the number of slots written. Logs an FEP summary when slots were written. Callers: disc_worker_main() (nr_csirs_disc_feed.c) and tests/nr_csirs_disc_td_fixture.c.
 */
int nr_csirs_disc_td_drain(nr_csirs_disc_ring_t *ring)
{
  if (!td.ready || !ring) return 0;
  int remaining = nr_csirs_disc_td_pending(), done = 0;
  /* Never overwrite entries before the detector can fold them. */
  if (remaining > ring->capacity) remaining = ring->capacity;
  const int fft = td.fp.ofdm_symbol_size;
  const uint64_t start = usec(), fft_start = csirs_disc_fft_us;
  int gpu_chunks = 0, cpu_chunks = 0;
  while (remaining > 0) {
    int n = remaining < NR_CSIRS_DISC_DEFAULT_BATCH ? remaining : NR_CSIRS_DISC_DEFAULT_BATCH;
    pthread_mutex_lock(&td.lock);
    if (n > td.count) n = td.count;
    const uint64_t epoch = td.epoch;
    for (int i=0;i<n;++i) {
      td_slot *swap = td.batch[i];
      td.batch[i] = td.slots[td.head];
      td.slots[td.head] = swap;
      td.head = (td.head+1)%td.capacity;
    }
    td.count -= n;
    pthread_mutex_unlock(&td.lock);
    if (!n) break;
    const uint64_t begin = usec();
    const nr_csirs_disc_gpu_ops_t *ops = nr_csirs_disc_gpu_load();
    bool gpu = false;
    if (td.unified && ops && ops->fep) {
      nr_csirs_disc_fep_slot_t slots[NR_CSIRS_DISC_DEFAULT_BATCH];
      nr_csirs_disc_slot_meta_t meta[NR_CSIRS_DISC_DEFAULT_BATCH];
      for(int i=0;i<n;++i) {
        slots[i]=(nr_csirs_disc_fep_slot_t){.samples=(int16_t *)td.batch[i]->samples,
                                          .fo=td.batch[i]->fo,.compensate=td.batch[i]->compensate};
        memcpy(slots[i].offset,td.batch[i]->offset,sizeof(slots[i].offset));
        memcpy(slots[i].rotation,td.batch[i]->rot,sizeof(slots[i].rotation));
        meta[i]=td.batch[i]->meta;
      }
      nr_csirs_disc_fep_cfg_t cfg={.fft=fft,.n_ant=td.n_ant,.n_rb=td.fp.N_RB_DL,
          .first_carrier=td.fp.first_carrier_offset,.samples_per_ms=td.fp.samples_per_subframe,
          .stride=td.stride,.timeshift=(int16_t *)td.fp.timeshift_symbol_rotation};
#if defined(__aarch64__) && defined(__ARM_FEATURE_QRDMX)
      cfg.rotation_mode=2;
#elif defined(__aarch64__)
      cfg.rotation_mode=1;
#endif
      pthread_mutex_lock(&td.commit);
      if(epoch==td.epoch) {
        int first=nr_csirs_disc_ring_write_begin(ring,n);
        if(first>=0) {
          gpu=ops->fep(&cfg,slots,n,ring,first)==0;
          nr_csirs_disc_ring_write_end(ring,n,gpu?meta:NULL);
          if(gpu) done+=n;
        }
      }
      pthread_mutex_unlock(&td.commit);
    }
    if(gpu) {
      ++gpu_chunks;
      __atomic_fetch_add(&csirs_disc_fft_us,usec()-begin,__ATOMIC_RELAXED);
      __atomic_fetch_add(&csirs_disc_fft_batches,1,__ATOMIC_RELAXED);
      remaining-=n;
      continue;
    }
    if(ring->device_grid) {
      /* Host IQ is deliberately stale on a resident ring. Never mix a failed
       * device batch with CPU FEP; retry discovery on later bounded windows. */
      __atomic_fetch_add(&csirs_disc_feed_drops,n,__ATOMIC_RELAXED);remaining-=n;continue;
    }
    static bool warned;
    if(!warned) {
      LOG_W(PHY,"SENSING: CSIRS_DISC CPU FEP fallback (not real-time); GPU chain unavailable or failed\n");
      warned=true;
    }
    for (int i=0;i<n;++i) for (int a=0;a<td.n_ant;++a) for (int l=0;l<14;++l) {
      const td_slot *s = td.batch[i];
      const c16_t *src = s->samples+a*td.stride+s->offset[l]-s->offset[0];
      c16_t *dst = (c16_t *)td.input+((i*td.n_ant+a)*14+l)*fft;
      if (s->compensate)
        nr_fo_compensation(s->fo, td.fp.samples_per_subframe, s->offset[l], src, dst, fft);
      else memcpy(dst, src, fft*sizeof(c16_t));
    }
    nr_csirs_disc_fft_cpu(fft,n*td.n_ant*14,td.input,td.output);
    ++cpu_chunks;
    __atomic_fetch_add(&csirs_disc_fft_us, usec()-begin, __ATOMIC_RELAXED);
    __atomic_fetch_add(&csirs_disc_fft_batches, 1, __ATOMIC_RELAXED);
    for (int i=0;i<n;++i) {
      const int16_t *ants[4];
      for (int a=0;a<td.n_ant;++a) {
        ants[a] = td.output+(size_t)(i*td.n_ant+a)*14*fft*2;
        for (int l=0;l<14;++l)
          apply_nr_rotation_symbol_RX(14, 1, td.fp.timeshift_symbol_rotation,
              td.fp.first_carrier_offset, (c16_t *)ants[a]+l*fft, td.batch[i]->rot,
              td.fp.N_RB_DL, 0, l);
      }
      /* Reset and publication serialize briefly, never during FFT/detection. */
      pthread_mutex_lock(&td.commit);
      if (epoch == td.epoch) {
        const nr_csirs_disc_slot_meta_t m = td.batch[i]->meta;
        if (nr_csirs_disc_ring_push(ring,ants,fft,td.fp.first_carrier_offset,m.abs_slot,m.frame,m.slot) >= 0)
          ++done;
      }
      pthread_mutex_unlock(&td.commit);
    }
    remaining -= n;
  }
  __atomic_fetch_add(&csirs_disc_fep_gpu_chunks,gpu_chunks,__ATOMIC_RELAXED);
  __atomic_fetch_add(&csirs_disc_fep_cpu_chunks,cpu_chunks,__ATOMIC_RELAXED);
  uint64_t elapsed=usec()-start;
  __atomic_fetch_add(&csirs_disc_fep_us,elapsed,__ATOMIC_RELAXED);
  uint64_t peak=__atomic_load_n(&csirs_disc_fep_max_us,__ATOMIC_RELAXED);
  if(elapsed>peak) __atomic_store_n(&csirs_disc_fep_max_us,elapsed,__ATOMIC_RELAXED);
  struct timespec fts; clock_gettime(CLOCK_MONOTONIC, &fts);
  if (done) LOG_I(PHY, "SENSING: CSIRS_DISC FEP slots=%d total_us=%llu fft_pack_us=%llu gpu_chunks=%d cpu_chunks=%d pending=%d feed_drops=%llu td_overflows=%llu confirm_drops=%llu confirm_wait_max_us=%llu t=%ld.%03ld\n",
      done, (unsigned long long)(usec()-start), (unsigned long long)(csirs_disc_fft_us-fft_start),
      gpu_chunks, cpu_chunks, nr_csirs_disc_td_pending(),
      (unsigned long long)__atomic_load_n(&csirs_disc_feed_drops,__ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&csirs_disc_td_overflows,__ATOMIC_RELAXED),
      &csirs_disc_confirm_drops ? (unsigned long long)__atomic_load_n(&csirs_disc_confirm_drops,__ATOMIC_RELAXED) : 0ull,
      &csirs_disc_confirm_wait_max_us ? (unsigned long long)__atomic_load_n(&csirs_disc_confirm_wait_max_us,__ATOMIC_RELAXED) : 0ull, (long)fts.tv_sec, fts.tv_nsec / 1000000);
  return done;
}
