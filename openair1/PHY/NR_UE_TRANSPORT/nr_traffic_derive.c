/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic mode (E7): derived DCI fields. See nr_traffic_derive.h. */
#include "nr_traffic_derive.h"
#include "nr_pdcch_blind_monitor.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

bool nr_traffic_mcs_qr(int dir, int table, int mcs, int *qm, uint32_t *r10)
{
  if (mcs < 0 || mcs > 31 || table < 0 || table > (dir ? 5 : 2)) return false;
  const uint8_t q = dir ? nr_get_Qm_ul((uint8_t)mcs, (uint8_t)table) : nr_get_Qm_dl((uint8_t)mcs, (uint8_t)table);
  const uint32_t r = dir ? nr_get_code_rate_ul((uint8_t)mcs, (uint8_t)table) : nr_get_code_rate_dl((uint8_t)mcs, (uint8_t)table);
  if (q == 0 || r == 0) return false;   /* reserved row: the tables carry R = 0 there */
  *qm = q; *r10 = r;
  return true;
}

uint32_t nr_traffic_tbs(int qm, uint32_t r10, int nb_rb, int nb_sym, int dmrs_type, int n_cdm, int dmrs_syms, int xoh, int nl)
{
  if (qm <= 0 || r10 == 0 || nb_rb < 1 || nb_rb > 275 || nb_sym < 1 || nb_sym > 14 || nl < 1 || nl > 4) return 0;
  if (n_cdm < 1 || n_cdm > 3 || dmrs_syms < 1 || dmrs_syms > nb_sym || (dmrs_type != 0 && dmrs_type != 1)) return 0;
  if (xoh != 0 && xoh != 6 && xoh != 12 && xoh != 18) return 0;
  const int dmrs_re = (dmrs_type ? 4 : 6) * n_cdm * dmrs_syms;
  if (12 * nb_sym - dmrs_re - xoh <= 0) return 0;
  return nr_compute_tbs((uint16_t)qm, (uint16_t)r10, (uint16_t)nb_rb, (uint16_t)nb_sym, (uint16_t)dmrs_re, (uint16_t)xoh, 0, (uint8_t)nl);
}

void nr_traffic_derive_dl(const nr_traffic_dl_in_t *in, nr_traffic_derived_t *out)
{
  out->nl = out->qm = -1; out->tbs = -1;
  if (!in->layout_trusted) return;
  /* nl: the layout's own (width, DM-RS type) names the antenna-ports table, the codepoint the row, and the row's port count is the number of layers */
  uint8_t cdm = 0; uint16_t ports = 0; int nsym = 0;
  bool ap_ok = in->ap_field >= 0 && nr_pdcch_blind_dl_ap_lookup(in->ap_bits, in->dmrs_type, (unsigned)in->ap_field, &cdm, &ports, &nsym)
               && ports == in->rd_ports && cdm == in->rd_cdm;   /* the reading must be the one this layout implies */
  const int nl = ap_ok ? __builtin_popcount(ports) : 0;
  if (nl >= 1) out->nl = (int8_t)nl;
  /* qm: the MCS under the PROVEN table; a reserved index has no value of its own */
  int qm = 0; uint32_t r10 = 0;
  const bool tbl_ok = in->sv_ok && in->sv_table == in->mcs_table && (in->layout_crc ? in->table_evidence >= 1 : in->table_evidence == 2);
  const bool qr_ok = tbl_ok && nr_traffic_mcs_qr(0, in->mcs_table, in->mcs, &qm, &r10);
  if (qr_ok) out->qm = (int8_t)qm;
  /* tbs: needs a CRC-solved RNTI (S, L and the DM-RS mask are CRC-learnt, not read), nl <= 4, a learnt xOverhead and no special-slot override */
  if (!qr_ok || nl < 1 || nl > 4 || !in->layout_crc || !in->sv_crc || in->sv_ov || !in->class_c) return;
  if (in->sv_S < 0 || in->sv_L < 1 || in->S != in->sv_S || in->L != in->sv_L || in->xoh < 0) return;
  if (in->expect_mask == 0 || in->expect_mask != in->dmrs_mask || nsym != 1) return;   /* double-symbol DM-RS: not derived */
  const uint32_t t = nr_traffic_tbs(qm, r10, in->nb_rb, in->L, in->dmrs_type, cdm, __builtin_popcount(in->dmrs_mask), in->xoh, nl);
  if (t > 0) out->tbs = (int32_t)t;
}
