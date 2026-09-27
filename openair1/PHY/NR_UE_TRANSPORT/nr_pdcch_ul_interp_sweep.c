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

#include "nr_pdcch_ul_interp_sweep.h"
#include "common/utils/LOG/log.h"
#include <stdatomic.h>
#include <string.h>
bool nr_pusch_tda_legal(int mapping_type, int S, int L)
{
  if (S < 0 || L < 1 || S + L > 14)
    return false;
  if (mapping_type == 0)
    return S == 0 && L >= 4;
  return mapping_type == 1;
}
int nr_pdcch_ul_interp_sweep_generate(nr_hyp_t *out, int cap)
{
  /* {S, L, mapping (0 = A, 1 = B), k2}. Mapping type B rows are enumerated like type A: the extraction
   * derives the type-B DM-RS mask (first DM-RS on the first PUSCH symbol) from the row's own mapping.
   * S=2 L=12 used to be listed as type A, which TS 38.214 Table 6.1.2.1-1 forbids (type A has S = 0):
   * it is legal only as type B, so it is listed as type B. The rows stay a curated list: the full legal
   * set (11 type A + 105 type B) x k2 {1..4} x the 96 field combinations below is 44,544 raw hypotheses
   * against NR_HYP_SWEEP_MAX_RAW = 8192, and without a UL DM-RS oracle nothing prunes it. */
  static const uint8_t tda[][4] = {
    {0,14,0,1},{0,14,0,2},{0,14,0,3},{0,14,0,4},
    {0,7,0,1},{0,7,0,2},{2,12,1,1},{2,12,1,2},{0,4,1,1},{0,4,1,2}
  };
  /* TS 38.214 6.1.4.1: which MCS table opts.mcs_table names depends on transform precoding --
   * nr_get_Qm_ul()/nr_get_code_rate_ul() (nr_mac_common.c) index 0=Table 5.1.3.1-1 (qam64), 1=Table
   * 5.1.3.1-2 (qam256), 2=Table 5.1.3.1-3 (qam64LowSE), 3=Table 6.1.4.1-1 (the TP-enabled default),
   * 4=Table 6.1.4.1-2 (TP-enabled qam64LowSE) -- get_pusch_mcs_table()'s own `2 + (is_tp<<1)` /
   * `0 + is_tp*3` arithmetic. qam256 (index 1) is not combined with transform precoding (spec:
   * mcs-Table is not applicable when transformPrecoder is enabled), so TP-enabled has two table
   * choices, not three. opts.mcs_table is the FINAL index used downstream (blind_ul_finish(),
   * fill_pusch_pdu()) -- generating it correctly here means no separate resolution step exists to
   * forget, matching this field's own header comment ("3..5 = TP variants"). */
  static const uint8_t mcs_no_tp[3] = {0, 1, 2};
  static const uint8_t mcs_tp[2]    = {3, 4};
  if (!out || cap<=0) return NR_HYP_SWEEP_INVALID;
  int n=0;
  for (int t=0;t<10;++t)
    for (int type=0;type<2;++type)
      for (int pos=0;pos<4;++pos)
        for (int max=1;max<=2;++max)
          for (int tp=0;tp<2;++tp) {
            if (tp && type != 0) continue; // TP requires DM-RS type 1 (38.211 6.4.1.1.1.2).
            const uint8_t *mcs_list = tp ? mcs_tp : mcs_no_tp;
            const int      n_mcs    = tp ? 2 : 3;
            for (int mi=0; mi<n_mcs; ++mi) {
              if (n==cap || n==NR_HYP_SWEEP_MAX_RAW) {
                LOG_E(PHY,"UL interpretation search refused: raw cap exceeded\n");
                return NR_HYP_SWEEP_RAW_OVERFLOW;
              }
              nr_pdcch_ul_interp_hyp_t h={tda[t][0],tda[t][1],tda[t][2],tda[t][3],
                                         type,pos,max,tp,mcs_list[mi]};
              out[n]=(nr_hyp_t){.len=sizeof(h)};
              memcpy(out[n++].bytes,&h,sizeof(h));
            }
          }
  return n;
}
int nr_pdcch_ul_interp_sweep_generate_pinned(nr_hyp_t *out, int cap, int S, int L, int mapping_type)
{
  if (!out || cap <= 0 || mapping_type < NR_PUSCH_MAPPING_EITHER || mapping_type > 1)
    return NR_HYP_SWEEP_INVALID;
  /* Ambiguous (S,L): generate for every mapping type nr_pusch_tda_legal() actually admits, never
   * just the one this file used to guess. A specific mapping_type (0 or 1) is still checked against
   * nr_pusch_tda_legal() rather than trusted -- an illegal (mapping,S,L) triple must still refuse. */
  int maps[2], nmaps = 0;
  if (mapping_type == NR_PUSCH_MAPPING_EITHER) {
    if (nr_pusch_tda_legal(0, S, L)) maps[nmaps++] = 0;
    if (nr_pusch_tda_legal(1, S, L)) maps[nmaps++] = 1;
  } else if (nr_pusch_tda_legal(mapping_type, S, L)) {
    maps[nmaps++] = mapping_type;
  }
  if (nmaps == 0)
    return NR_HYP_SWEEP_INVALID;
  int n = 0;
  for (int mi = 0; mi < nmaps; ++mi)
    for (int k2 = 1; k2 <= 4; ++k2)
      for (int type = 0; type < 2; ++type)
        for (int pos = 0; pos < 4; ++pos)
          for (int max = 1; max <= 2; ++max)
            for (int tp = 0; tp < 2; ++tp) {
              /* Final CP/DFT-s-OFDM table indices consumed by the UL PHY:
               * TP uses DM-RS type 1 and the final UL MCS table indices 3/4. */
              if (tp && type != 0) continue;
              for (int mcs = 0; mcs < (tp ? 2 : 3); ++mcs) {
                if (n == cap || n == NR_HYP_SWEEP_MAX_RAW) {
                  LOG_E(PHY, "UL pinned interpretation search refused: raw cap exceeded\n");
                  return NR_HYP_SWEEP_RAW_OVERFLOW;
                }
                nr_pdcch_ul_interp_hyp_t h = {(uint8_t)S, (uint8_t)L, (uint8_t)maps[mi], (uint8_t)k2,
                                              type, pos, max, tp, tp ? 3 + mcs : mcs};
                out[n] = (nr_hyp_t){.len = sizeof(h)};
                memcpy(out[n++].bytes, &h, sizeof(h));
              }
            }
  return n;
}

bool nr_pusch_ul_energy_span(const double energy[14], double rel_thresh, int *S, int *L)
{
  if (!energy || !S || !L || rel_thresh <= 0.0 || rel_thresh >= 1.0)
    return false;
  double emax = 0.0;
  for (int i = 0; i < 14; i++)
    if (energy[i] > emax) emax = energy[i];
  if (emax <= 0.0)
    return false;
  int first = -1, last = -1;
  for (int i = 0; i < 14; i++) {
    if (energy[i] > rel_thresh * emax) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (first < 0)
    return false;
  *S = first;
  *L = last - first + 1;
  return true;
}

/* State machine for the pin's validity flag, so "check-then-act" on the plain S/L/mapping fields
 * below cannot tear: EMPTY -> (one winning CAS) -> WRITING -> (release-store) -> VALID. Two UL
 * consumer threads calling nr_pusch_ul_dmrs_pin_set() concurrently is a real, not theoretical, case
 * (`ul_thread` can run more than one consumer) -- review-caught: the previous version's
 * load-then-store was a plain check-then-act with no ordering against a concurrent writer, so a
 * reader could observe VALID with fields from two different callers half-written. */
enum { UL_PIN_EMPTY = 0, UL_PIN_VALID = 1, UL_PIN_WRITING = 2 };
static _Atomic int g_ul_dmrs_pin_valid;
static int g_ul_dmrs_pin_S, g_ul_dmrs_pin_L, g_ul_dmrs_pin_mapping;

void nr_pusch_ul_dmrs_pin_set(int S, int L)
{
  /* First observation wins and is never overwritten -- same rule the DL DM-RS oracle already uses.
   * A losing CAS means either a pin already exists (UL_PIN_VALID) or another thread is mid-write
   * (UL_PIN_WRITING) -- either way this caller's own (S,L) is correctly discarded. */
  int expected = UL_PIN_EMPTY;
  if (!atomic_compare_exchange_strong_explicit(&g_ul_dmrs_pin_valid, &expected, UL_PIN_WRITING,
                                               memory_order_acq_rel, memory_order_relaxed))
    return;
  /* Ambiguous (S,L): both mapping types are legal at S=0,L>=4 (NR_PUSCH_MAPPING_EITHER, see the
   * header) -- energy occupancy alone cannot distinguish them, so the pin must not pretend it can. */
  const bool legal_a = nr_pusch_tda_legal(0, S, L);
  const bool legal_b = nr_pusch_tda_legal(1, S, L);
  if (!legal_a && !legal_b) {
    atomic_store_explicit(&g_ul_dmrs_pin_valid, UL_PIN_EMPTY, memory_order_release); /* release the claim */
    return;
  }
  g_ul_dmrs_pin_S = S;
  g_ul_dmrs_pin_L = L;
  g_ul_dmrs_pin_mapping = (legal_a && legal_b) ? NR_PUSCH_MAPPING_EITHER : (legal_a ? 0 : 1);
  atomic_store_explicit(&g_ul_dmrs_pin_valid, UL_PIN_VALID, memory_order_release);
  LOG_A(PHY, "SENSING: UL_DMRS_PIN S=%d L=%d mapping=%s (from energy occupancy)\n", S, L,
        g_ul_dmrs_pin_mapping == NR_PUSCH_MAPPING_EITHER ? "A|B" : (g_ul_dmrs_pin_mapping ? "B" : "A"));
}

bool nr_pusch_ul_dmrs_pin_get(int *S, int *L, int *mapping_type)
{
  /* Only UL_PIN_VALID is a safe read: UL_PIN_WRITING means a winner is between the CAS above and
   * its own release-store, and the fields are not yet published. The acquire load here pairs with
   * that release-store, so once this sees VALID the plain field writes above are visible whole. */
  if (atomic_load_explicit(&g_ul_dmrs_pin_valid, memory_order_acquire) != UL_PIN_VALID)
    return false;
  if (S) *S = g_ul_dmrs_pin_S;
  if (L) *L = g_ul_dmrs_pin_L;
  if (mapping_type) *mapping_type = g_ul_dmrs_pin_mapping;
  return true;
}

void nr_pusch_ul_dmrs_pin_reset(void)
{
  atomic_store_explicit(&g_ul_dmrs_pin_valid, UL_PIN_EMPTY, memory_order_release);
}

bool nr_pdcch_ul_interp_sweep_apply(const nr_hyp_t *hyp, int idx, nr_pdcch_blind_ul_opts_t *o)
{
  if (!hyp || !o || hyp->len!=sizeof(nr_pdcch_ul_interp_hyp_t) ||
      idx<0 || idx>=16 || o->tda_count<=idx) return false;
  nr_pdcch_ul_interp_hyp_t h;
  memcpy(&h,hyp->bytes,sizeof(h));
  if (h.transform_precoding && h.dmrs_config_type != 0) return false;
  o->tda_start[idx]=h.tda_start; o->tda_length[idx]=h.tda_length;
  o->tda_mapping[idx]=h.tda_mapping; o->tda_k2[idx]=h.tda_k2;
  o->dmrs_config_type=h.dmrs_config_type; o->dmrs_add_pos=h.dmrs_add_pos;
  o->dmrs_max_length=h.dmrs_max_length; o->transform_precoding=h.transform_precoding;
  o->mcs_table=h.mcs_table;
  return true;
}
