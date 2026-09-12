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
#include "nr_pdsch_xoverhead.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h" // nr_compute_tbs
#include "common/utils/LOG/log.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>

static nr_pdsch_xoverhead_state_t g_st;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void nr_pdsch_xoverhead_reset(uint16_t assumed)
{
  pthread_mutex_lock(&g_lock);
  memset(&g_st, 0, sizeof(g_st));
  g_st.assumed = assumed;
  pthread_mutex_unlock(&g_lock);
}
nr_pdsch_xoverhead_state_t nr_pdsch_xoverhead_snapshot(void)
{
  pthread_mutex_lock(&g_lock);
  nr_pdsch_xoverhead_state_t s = g_st;
  pthread_mutex_unlock(&g_lock);
  return s;
}
bool nr_pdsch_xoverhead_observe(uint16_t Qm, uint16_t R, uint16_t nb_rb, uint16_t nb_symb,
                                uint16_t nb_dmrs_re, uint16_t used_oh, uint8_t tb_scaling,
                                uint8_t Nl, bool crc_ok)
{
  if (!crc_ok) return false;
  const uint32_t tbs_used = nr_compute_tbs(Qm, R, nb_rb, nb_symb, nb_dmrs_re, used_oh, tb_scaling, Nl);
  if (tbs_used == 0) return false;
  bool just_confirmed = false;
  pthread_mutex_lock(&g_lock);
  if (g_st.assumed != used_oh) {
    /* The decoder changed its assumption under us: evidence for the old value is not evidence for
     * the new one. Start over rather than mix. */
    memset(&g_st, 0, sizeof(g_st));
    g_st.assumed = used_oh;
  }
  ++g_st.crc_ok_seen;
  for (int c = 0; c < NR_XOH_CANDIDATES; ++c) {
    if (nr_xoh_values[c] == used_oh) continue;
    const uint32_t tbs_c = nr_compute_tbs(Qm, R, nb_rb, nb_symb, nb_dmrs_re, nr_xoh_values[c], tb_scaling, Nl);
    if (tbs_c != tbs_used) ++g_st.refuted_by[c];
    else                   ++g_st.indistinct[c];
  }
  if (!g_st.confirmed) {
    bool all = true;
    for (int c = 0; c < NR_XOH_CANDIDATES && all; ++c)
      if (nr_xoh_values[c] != used_oh && g_st.refuted_by[c] == 0) all = false;
    if (all) {
      g_st.confirmed = just_confirmed = true;
      char detail[128]; int n = 0;
      for (int c = 0; c < NR_XOH_CANDIDATES; ++c)
        if (nr_xoh_values[c] != used_oh)
          n += snprintf(detail + n, sizeof(detail) - n, " %u:refuted=%u/indistinct=%u",
                        nr_xoh_values[c], g_st.refuted_by[c], g_st.indistinct[c]);
      LOG_A(PHY, "SENSING: XOVERHEAD CONFIRMED = %u REs/PRB by TB-CRC elimination after %u CRC-OK decodes;"
                 " alternatives%s\n", used_oh, g_st.crc_ok_seen, detail);
    }
  }
  pthread_mutex_unlock(&g_lock);
  return just_confirmed;
}
