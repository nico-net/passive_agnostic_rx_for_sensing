/* RE pattern: a generic PDSCH rate-match resource {symbol mask, per-RB subcarrier mask, RB range, RB density}, carried in the existing
 * fapi_nr_dl_config_csirs_pdu_rel15_t entry of csiRsForRateMatching (csi_type NR_CSI_TYPE_REPAT) so that every consumer of that list --
 * nr_dlsch_csi_overlap_bitmap / nr_dlsch_csi_rb_mask / nr_dlsch_csi_unav_res (CPU G and extractor), nr_dlsch_extract_geometry (the RE index maps
 * of the GPU demod and of the GPU graph pass), the CSI-FULL erase list -- honours it without a second code path and without a layout change of
 * the job or of the replay captures. Field use: scramb_id = 14-bit OFDM symbol mask, freq_domain = 12-bit subcarrier-in-RB mask, freq_density
 * 0 / 1 = every 2nd RB with even / odd CRB, 2 / 3 = every RB, start_rb / nr_of_rbs = RB range (CRB), row = 1 (keeps the row-indexed tables safe).
 * A CSI-RS PDU that expresses the same RE set (e.g. row 1 TRS, 3 REs, one symbol) is bit-exact equivalent (tests/nr_re_pattern_check.c). */
#ifndef NR_RE_PATTERN_H
#define NR_RE_PATTERN_H
#include <string.h>
#include "nfapi/open-nFAPI/nfapi/public_inc/fapi_nr_ue_interface.h"

#define NR_CSI_TYPE_REPAT 3

static inline bool nr_re_pattern_is(const fapi_nr_dl_config_csirs_pdu_rel15_t *p) { return p->csi_type == NR_CSI_TYPE_REPAT; }
static inline uint32_t nr_re_pattern_sym(const fapi_nr_dl_config_csirs_pdu_rel15_t *p) { return p->scramb_id & 0x3fffu; }
static inline uint32_t nr_re_pattern_sc(const fapi_nr_dl_config_csirs_pdu_rel15_t *p) { return p->freq_domain & 0xfffu; }
static inline void nr_re_pattern_make(fapi_nr_dl_config_csirs_pdu_rel15_t *p, uint32_t sym_mask, uint32_t sc_mask, int density, int rb_start,
                                      int n_rb)
{
  memset(p, 0, sizeof(*p));
  p->csi_type = NR_CSI_TYPE_REPAT;
  p->row = 1;
  p->scramb_id = (uint16_t)(sym_mask & 0x3fffu);
  p->freq_domain = (uint16_t)(sc_mask & 0xfffu);
  p->freq_density = (uint8_t)density;
  p->start_rb = (uint16_t)rb_start;
  p->nr_of_rbs = (uint16_t)n_rb;
}
#endif
