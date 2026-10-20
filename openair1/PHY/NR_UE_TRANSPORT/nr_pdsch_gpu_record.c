#include "nr_pdsch_gpu_record.h"
#include "nr_pdsch_passive_decode.h"

/* nr_pdsch_gpu_csi_disjoint: a PDSCH with CSI-RS rate matching never takes the GPU path (the disjoint-geometry admission is off). */
bool nr_pdsch_gpu_csi_disjoint(const fapi_nr_dl_config_dlsch_pdu_rel15_t *pdu, const freq_alloc_bitmap_t *fa)
{
  (void)fa;
  return !pdu->numCsiRsForRateMatching;
}

/* nr_pdsch_gpu_readback: reads the GPU rxdataF back for the record.
 *   How it works: Reads one symbol when all jobs share the same first DM-RS symbol, otherwise the whole grid (api->read_rxdataF).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_record.h:24, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:206.
 */
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
