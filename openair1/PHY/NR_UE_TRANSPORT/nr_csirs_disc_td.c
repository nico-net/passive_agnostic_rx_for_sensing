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
  td_slot *slots, batch[NR_CSIRS_DISC_DEFAULT_BATCH];
  int capacity, count, head, tail, n_ant;
  uint64_t epoch;
  size_t stride;
  int16_t *input, *output;
  bool unified, ready;
} td;
extern uint64_t csirs_disc_confirm_drops __attribute__((weak));
uint64_t csirs_disc_fft_us, csirs_disc_fft_batches, csirs_disc_td_overflows;

static uint64_t usec(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

void nr_csirs_disc_fft_cpu(int fft, int transforms, const int16_t *in, int16_t *out)
{
  const dft_size_idx_t size = get_dft(fft);
  for (int i = 0; i < transforms; ++i)
    dft(size, (int16_t *)in + (size_t)i * 2 * fft, out + (size_t)i * 2 * fft, 1);
}

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
  if (!td.slots) goto fail;
  /* Plain host staging stays resident on the CPU. Only worker FFT buffers use
   * managed memory: a GPU page migration can never delay the producer. */
  const size_t slot_bytes = ((td.stride*n_ant*sizeof(c16_t)+63)/64)*64;
  const size_t huge = 2*1024*1024;
  const size_t pool_bytes = ((slot_bytes*(capacity+NR_CSIRS_DISC_DEFAULT_BATCH)+huge-1)/huge)*huge;
  td.sample_pool = aligned_alloc(huge, pool_bytes);
  if (!td.sample_pool) goto fail;
  /* A GB-scale queue otherwise incurs hundreds of thousands of 4K mappings.
   * Best-effort THP; correctness and allocation do not depend on availability. */
  madvise(td.sample_pool, pool_bytes, MADV_HUGEPAGE);
  memset(td.sample_pool, 0, pool_bytes);
  for (int i = 0; i < capacity + NR_CSIRS_DISC_DEFAULT_BATCH; ++i) {
    td_slot *s = i < capacity ? &td.slots[i] : &td.batch[i-capacity];
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

void nr_csirs_disc_td_free(void)
{
  if (!td.ready) return;
  free(td.sample_pool);
  free(td.slots);
  if (td.unified) {
    if (td.input) nr_csirs_disc_free_unified(td.input);
    if (td.output) nr_csirs_disc_free_unified(td.output);
  } else { free(td.input); free(td.output); }
  pthread_mutex_destroy(&td.lock);
  pthread_mutex_destroy(&td.commit);
  memset(&td, 0, sizeof(td));
}

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

int nr_csirs_disc_td_pending(void)
{
  if (!td.ready) return 0;
  pthread_mutex_lock(&td.lock);
  int n = td.count;
  pthread_mutex_unlock(&td.lock);
  return n;
}

int nr_csirs_disc_td_push(PHY_VARS_NR_UE *ue, uint32_t abs_slot, uint16_t frame, uint8_t slot)
{
  if (!td.ready || !ue) return -1;
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
  td_slot *s = &td.slots[td.tail];
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

/* Coalesce the backlog observed at entry; one detection pass follows ALL these
 * chunks, not one expensive detector invocation per 100 queued slots. */
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
      td_slot swap = td.batch[i];
      td.batch[i] = td.slots[td.head];
      td.slots[td.head] = swap;
      td.head = (td.head+1)%td.capacity;
    }
    td.count -= n;
    pthread_mutex_unlock(&td.lock);
    if (!n) break;
    const uint64_t begin = usec();
    for (int i=0;i<n;++i) for (int a=0;a<td.n_ant;++a) for (int l=0;l<14;++l) {
      const td_slot *s = &td.batch[i];
      const c16_t *src = s->samples+a*td.stride+s->offset[l]-s->offset[0];
      c16_t *dst = (c16_t *)td.input+((i*td.n_ant+a)*14+l)*fft;
      if (s->compensate)
        nr_fo_compensation(s->fo, td.fp.samples_per_subframe, s->offset[l], src, dst, fft);
      else memcpy(dst, src, fft*sizeof(c16_t));
    }
    const nr_csirs_disc_gpu_ops_t *ops = nr_csirs_disc_gpu_ops();
    if (!ops || !ops->fft || ops->fft(fft,n*td.n_ant*14,td.input,td.output) < 0) {
      nr_csirs_disc_fft_cpu(fft,n*td.n_ant*14,td.input,td.output);
      ++cpu_chunks;
    } else ++gpu_chunks;
    __atomic_fetch_add(&csirs_disc_fft_us, usec()-begin, __ATOMIC_RELAXED);
    __atomic_fetch_add(&csirs_disc_fft_batches, 1, __ATOMIC_RELAXED);
    for (int i=0;i<n;++i) {
      const int16_t *ants[4];
      for (int a=0;a<td.n_ant;++a) {
        ants[a] = td.output+(size_t)(i*td.n_ant+a)*14*fft*2;
        for (int l=0;l<14;++l)
          apply_nr_rotation_symbol_RX(14, 1, td.fp.timeshift_symbol_rotation,
              td.fp.first_carrier_offset, (c16_t *)ants[a]+l*fft, td.batch[i].rot,
              td.fp.N_RB_DL, 0, l);
      }
      /* Reset and publication serialize briefly, never during FFT/detection. */
      pthread_mutex_lock(&td.commit);
      if (epoch == td.epoch) {
        const nr_csirs_disc_slot_meta_t m = td.batch[i].meta;
        if (nr_csirs_disc_ring_push(ring,ants,fft,td.fp.first_carrier_offset,m.abs_slot,m.frame,m.slot) >= 0)
          ++done;
      }
      pthread_mutex_unlock(&td.commit);
    }
    remaining -= n;
  }
  if (done) LOG_I(PHY, "SENSING: CSIRS_DISC FEP slots=%d total_us=%llu fft_pack_us=%llu gpu_chunks=%d cpu_chunks=%d pending=%d feed_drops=%llu td_overflows=%llu confirm_drops=%llu\n",
      done, (unsigned long long)(usec()-start), (unsigned long long)(csirs_disc_fft_us-fft_start),
      gpu_chunks, cpu_chunks, nr_csirs_disc_td_pending(),
      (unsigned long long)__atomic_load_n(&csirs_disc_feed_drops,__ATOMIC_RELAXED),
      (unsigned long long)__atomic_load_n(&csirs_disc_td_overflows,__ATOMIC_RELAXED),
      &csirs_disc_confirm_drops ? (unsigned long long)__atomic_load_n(&csirs_disc_confirm_drops,__ATOMIC_RELAXED) : 0ull);
  return done;
}
