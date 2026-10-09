#include "nr_pdsch_gpu_record.h"
#include "nr_pdsch_passive_queue.h"
#include "nr_pdsch_passive_decode.h"
#include "nr_transport_proto_ue.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Admit a configured CSI-RS list only when the CPU extractor's own geometry
 * says it removes zero REs from this grant. Opt-in; overlapping CSI stays CPU. */
bool nr_pdsch_gpu_csi_disjoint(const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu, const freq_alloc_bitmap_t *fa)
{
  if (!pdu->numCsiRsForRateMatching) return true;
  const char *e = getenv("ISAC_PDSCH_GPU_CSI_DISJOINT");
  if (!e || strcmp(e, "1") || pdu->numCsiRsForRateMatching > NFAPI_MAX_NUM_CSI_RATEMATCH
      || pdu->BWPSize < 1 || pdu->BWPSize > 275 || pdu->start_symbol >= 14
      || pdu->number_symbols < 1 || pdu->number_symbols > 14 - pdu->start_symbol) return false;
  for (int i = 0; i < pdu->numCsiRsForRateMatching; ++i)
    if (pdu->csiRsForRateMatching[i].row < 1 || pdu->csiRsForRateMatching[i].row > 18) return false;
  return nr_dlsch_csi_unav_res((fapi_nr_dl_config_dlsch_pdu_rel15_t *)pdu, fa) == 0;
}

int nr_pdsch_gpu_readback(const nr_gpu_fep_api_t *api, const nr_gpu_pdsch_job_t *jobs,
                          int n, int ofdm_symbol_size, int16_t *out)
{
  if (!api || !jobs || n < 1 || ofdm_symbol_size < 1 || !out) return -1;
  int sym = jobs[0].dmrs_mask ? __builtin_ctz((unsigned)jobs[0].dmrs_mask) : -1;
  for (int i = 1; i < n; i++)
    if (!jobs[i].dmrs_mask || __builtin_ctz((unsigned)jobs[i].dmrs_mask) != sym) sym = -1;
  if (sym >= 14) sym = -1;
  return sym >= 0 && api->read_rxdataF_symbol
      ? api->read_rxdataF_symbol(0, sym, out + 2 * sym * ofdm_symbol_size)
      : api->read_rxdataF(0, out);
}

/* Called after the CPU decode while this consumer still owns its IQ snapshot.
 * Bounded, opt-in I/O belongs to the offline replay only. */
void nr_pdsch_gpu_record_cpu(PHY_VARS_NR_UE *ue, const nr_pdsch_passive_job_t *src,
                             const nr_pdsch_passive_decode_result_t *dec, const c16_t *rxdataF, uint32_t rxF_stride)
{
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static const char *path;
  static int enabled = -1, left;
  if (__atomic_load_n(&enabled, __ATOMIC_RELAXED) == 0) return;
  pthread_mutex_lock(&lock);
  if (enabled < 0) {
    path = getenv("ISAC_PDSCH_GPU_DUMP");
    const char *limit = getenv("ISAC_PDSCH_GPU_DUMP_MAX");
    left = limit ? atoi(limit) : 128;
    __atomic_store_n(&enabled, path && *path, __ATOMIC_RELAXED);
  }
  if (!enabled || left <= 0 || dec->config_recovered || !rxdataF || dec->status == NR_PDSCH_PASSIVE_DECODE_ERROR
      || dec->status == NR_PDSCH_PASSIVE_DECODE_UNSUPPORTED) goto out;
  nr_pdsch_passive_grant_t grant = src->grant;
  grant.source_absolute_slot = src->absolute_slot;
  nr_gpu_pdsch_job_t job;
  if (!nr_pdsch_passive_gpu_job(ue, &src->dlsch_pdu, &src->freq_alloc, &grant,
                                src->nr_slot_rx, src->layout_probe != 0, &job)) goto out;
  const int16_t *cpu_llr = NULL;
  const uint32_t G = nr_pdsch_passive_last_llr(&cpu_llr);
  if (!cpu_llr || !G || G > (1u << 22)) goto out;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint32_t ring_len = 2u * fp->samples_per_frame;
  const uint32_t off = get_samples_slot_timestamp(fp, src->nr_slot_rx);
  const uint32_t early = fp->nb_prefix_samples / 8;
  const uint32_t window = fp->nb_prefix_samples0 + fp->ofdm_symbol_size
      + (fp->symbols_per_slot - 1) * (fp->ofdm_symbol_size + fp->nb_prefix_samples)
      + fp->ofdm_symbol_size;
  if (window >= ring_len || fp->nb_antennas_rx > 4 || fp->symbols_per_slot > 14) goto out;
  nr_pdsch_gpu_record_t h = {0};
  h.magic = NR_PDSCH_GPU_RECORD_MAGIC;
  h.version = 2;
  h.absolute_slot = src->absolute_slot;
  h.cfg = (nr_gpu_fep_cfg_t){.nant = fp->nb_antennas_rx, .ofdm_symbol_size = fp->ofdm_symbol_size,
    .nb_prefix_samples = fp->nb_prefix_samples, .nb_prefix_samples0 = fp->nb_prefix_samples0,
    .symbols_per_slot = fp->symbols_per_slot, .first_carrier_offset = fp->first_carrier_offset,
    .n_rb_dl = fp->N_RB_DL, .samples_per_ms = fp->samples_per_subframe,
    .ofdm_offset_divisor = fp->ofdm_offset_divisor};
  h.job = job;
  h.iq_samples_per_ant = window;
  h.llr_count = G;
  h.rnti = grant.rnti;
  h.data_scrambling_id = src->dlsch_pdu.dlDataScramblingId;
  h.crc_ok = dec->status == NR_PDSCH_PASSIVE_DECODE_CRC_OK;
  h.evm_valid = dec->evm_valid;
  h.cpu_evm_pct = dec->evm_pct;
  h.fo_hz = ue->cont_fo_comp ? src->fo_hz : 0.0;
  h.abs_sample = off;
  h.early = early;
  h.rxF_symbol = __builtin_ctz((unsigned)job.dmrs_mask);
  const size_t rot_bytes = (size_t)fp->symbols_per_slot * 2 * sizeof(int16_t);
  const size_t iq_bytes = (size_t)fp->nb_antennas_rx * window * 2 * sizeof(int16_t);
  const size_t llr_bytes = (size_t)G * sizeof(int16_t);
  const size_t rxF_bytes = (size_t)fp->nb_antennas_rx * fp->ofdm_symbol_size * sizeof(c16_t);
  h.bytes = sizeof(h) + rot_bytes + iq_bytes + llr_bytes + rxF_bytes;
  FILE *f = fopen(path, "ab");
  if (!f) goto out;
  const c16_t *rot = fp->symbol_rotation[link_type_dl]
      + (src->nr_slot_rx % fp->slots_per_subframe) * fp->symbols_per_slot;
  bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(rot, rot_bytes, 1, f) == 1;
  c16_t **rx = nr_pdsch_passive_iq_for_slot(src->nr_slot_rx, ue->common_vars.rxdata);
  const uint32_t origin = (off + ring_len - early) % ring_len;
  for (int a = 0; a < fp->nb_antennas_rx && ok; ++a) {
    const uint32_t first = window < ring_len - origin ? window : ring_len - origin;
    ok = fwrite(rx[a] + origin, sizeof(c16_t), first, f) == first;
    if (ok && first < window)
      ok = fwrite(rx[a], sizeof(c16_t), window - first, f) == window - first;
  }
  if (ok) ok = fwrite(cpu_llr, llr_bytes, 1, f) == 1;
  for (int a = 0; a < fp->nb_antennas_rx && ok; ++a)
    ok = fwrite(rxdataF + (size_t)a * rxF_stride + (size_t)h.rxF_symbol * fp->ofdm_symbol_size,
                sizeof(c16_t), fp->ofdm_symbol_size, f) == (size_t)fp->ofdm_symbol_size;
  if (ok) --left;
  fclose(f);
out:
  pthread_mutex_unlock(&lock);
}
