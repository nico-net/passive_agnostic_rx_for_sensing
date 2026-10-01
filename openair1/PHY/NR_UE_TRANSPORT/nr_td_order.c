/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_td_order.h"
#include "nr_pdsch_qm_oracle.h"

/* TS 38.214 Table 5.1.2.1.1-2 (normal CP), verified against openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.c
 * table_5_1_2_1_1_2_time_dom_res_alloc_A_dmrs_typeA_pos{2,3}.
 * {mapping (0=A,1=B), S if pos2, L if pos2, S if pos3, L if pos3}; k0 = 0 for all rows. */
static const uint8_t k_def_a[16][5] = {
    {0, 2, 12, 3, 11}, {0, 2, 10, 3, 9}, {0, 2, 9, 3, 8},   {0, 2, 7, 3, 6},   {0, 2, 5, 3, 4},
    {1, 9, 4, 10, 4},  {1, 4, 4, 6, 4},  {1, 5, 7, 5, 7},   {1, 5, 2, 5, 2},   {1, 9, 2, 9, 2},
    {1, 12, 2, 12, 2}, {0, 1, 13, 1, 13}, {0, 1, 6, 1, 6},  {0, 2, 4, 2, 4},   {1, 4, 7, 4, 7},
    {1, 8, 4, 8, 4},
};

bool nr_td_default_table_a(int row, int pos, nr_td_tdra_t *out)
{
  if (row < 1 || row > 16 || (pos != 2 && pos != 3))
    return false;
  const uint8_t *r = k_def_a[row - 1];
  out->mapping = r[0];
  out->k0 = 0;
  out->S = pos == 2 ? r[1] : r[3];
  out->L = pos == 2 ? r[2] : r[4];
  return true;
}

static bool tdra_eq(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_tdra_t *t)
{
  return h->tda_start == t->S && h->tda_length == t->L && h->mapping_type == t->mapping && h->k0 == t->k0;
}

float nr_td_ordering_score(const nr_pdsch_cfg_hypothesis_t *h, const nr_td_side_info_t *si)
{
  float s = 0.0f;
  if (si->w_sib1 > 0)
    for (int i = 0; i < si->n_sib1 && i < NR_TD_MAX_SIB1_TDRA; i++)
      if (tdra_eq(h, &si->sib1[i])) {
        s += si->w_sib1;
        break;
      }
  if (si->w_default > 0 && si->dmrs_typeA_pos) {
    nr_td_tdra_t t;
    for (int r = 1; r <= 16; r++)
      if (nr_td_default_table_a(r, si->dmrs_typeA_pos, &t) && tdra_eq(h, &t)) {
        s += si->w_default;
        break;
      }
  }
  if (si->w_obs > 0) {
    if (si->obs_dmrs_mask >= 0 && h->dmrs_mask == (uint16_t)si->obs_dmrs_mask)
      s += si->w_obs;
    if (si->obs_qm > 0 && nr_pdsch_qm_of_mcs((uint8_t)si->obs_mcs, h->mcs_table) == si->obs_qm)
      s += si->w_obs;
    if (si->obs_last_symbol >= 0 && h->tda_start + h->tda_length - 1 == si->obs_last_symbol)
      s += si->w_obs;
  }
  if (si->w_field > 0) {
    int n = 0;
    n += si->f_S >= 0 && h->tda_start == si->f_S;
    n += si->f_L >= 0 && h->tda_length == si->f_L;
    n += si->f_mapping >= 0 && h->mapping_type == si->f_mapping;
    n += si->f_k0 >= 0 && h->k0 == si->f_k0;
    n += si->f_dmrs_add_pos >= 0 && h->dmrs_add_pos == si->f_dmrs_add_pos;
    n += si->f_dmrs_max_len >= 0 && h->dmrs_max_len == si->f_dmrs_max_len;
    s += si->w_field * si->f_conf * (float)n;
  }
  return s;
}
