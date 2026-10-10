#include "nr_pdcch_gpu_record.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* nr_pdcch_gpu_record_cpu: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_PDCCH_GPU_DUMP, ISAC_PDCCH_GPU_DUMP_MAX.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5477, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_record.h:18.
 */
void nr_pdcch_gpu_record_cpu(const PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc,
                              const fapi_nr_dl_config_dci_dl_pdu_rel15_t *pdcch, int first_symbol,
                              int n_rb, int cset_start, const c16_t *rxF, int rxF_stride,
                              const c16_t *cpu_llr, uint32_t llr_re, uint32_t absolute_slot)
{
  (void)proc;
  static int enabled = -1, left;
  static const char *path;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  if (__atomic_load_n(&enabled, __ATOMIC_RELAXED) == 0) return;
  pthread_mutex_lock(&lock);
  if (enabled < 0) {
    path = getenv("ISAC_PDCCH_GPU_DUMP");
    const char *limit = getenv("ISAC_PDCCH_GPU_DUMP_MAX");
    left = limit ? atoi(limit) : 128;
    __atomic_store_n(&enabled, path && *path, __ATOMIC_RELAXED);
  }
  if (!enabled || left <= 0) goto out;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const int dur = pdcch->coreset.duration, nant = fp->nb_antennas_rx, N = fp->ofdm_symbol_size;
  if (nant < 1 || nant > 4 || dur < 1 || dur > 3 || N < 128 || N > 8192 || rxF_stride < 14 * N
      || first_symbol < 0 || first_symbol + dur > fp->symbols_per_slot || n_rb < 1 || n_rb > 273
      || llr_re != (uint32_t)(9 * n_rb * dur)) goto out;
  nr_pdcch_gpu_record_t h = {0};
  h.magic = NR_PDCCH_GPU_RECORD_MAGIC;
  h.version = 1;
  h.absolute_slot = absolute_slot;
  h.cfg = (nr_gpu_pdcch_fep_cfg_t){nant, N, fp->symbols_per_slot, fp->first_carrier_offset, fp->N_RB_DL};
  h.job = (nr_gpu_pdcch_job_t){.rb_offset = pdcch->coreset.rb_offset + cset_start,
    .bwp_start_rb = pdcch->BWPStart,
    .dmrs_ref_rb = pdcch->coreset.CoreSetType == NFAPI_NR_CSET_CONFIG_PDCCH_CONFIG ? pdcch->BWPStart : 0,
    .n_rb = n_rb, .start_symbol = first_symbol, .duration = dur,
    .dmrs_scrambling_id = pdcch->coreset.pdcch_dmrs_scrambling_id, .slot = proc->nr_slot_rx};
  h.llr_count = 2 * llr_re;
  const size_t iq_bytes = (size_t)nant * dur * N * sizeof(c16_t);
  const size_t llr_bytes = (size_t)h.llr_count * sizeof(int16_t);
  h.bytes = sizeof(h) + iq_bytes + llr_bytes;
  FILE *f = fopen(path, "ab");
  if (!f) goto out;
  bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
  for (int a = 0; a < nant && ok; ++a)
    ok = fwrite(rxF + (size_t)a * rxF_stride + (size_t)first_symbol * N,
                sizeof(c16_t), (size_t)dur * N, f) == (size_t)dur * N;
  if (ok) ok = fwrite(cpu_llr, llr_bytes, 1, f) == 1;
  if (ok) --left;
  fclose(f);
out:
  pthread_mutex_unlock(&lock);
}
