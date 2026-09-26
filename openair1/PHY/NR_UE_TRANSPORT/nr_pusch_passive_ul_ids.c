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

/* See nr_pusch_passive_ul_ids.h for why this state lives here instead of in
 * nr_pusch_passive_decode.c. */

#include "nr_pusch_passive_ul_ids.h"
#include "PHY/NR_UE_TRANSPORT/nr_scrambling_id_sweep.h"

#include <pthread.h>
#include <stdatomic.h>
#include "common/utils/LOG/log.h"

/* ---- UL DM-RS scrambling identity, one two-window state per nSCID (final review I5) -------- */
static nr_dmrs_id_2stage_t g_ul_dmrs_id[NR_UL_DMRS_NSCID];
static bool                g_ul_dmrs_id_init[NR_UL_DMRS_NSCID];
static pthread_mutex_t     g_ul_dmrs_id_lock[NR_UL_DMRS_NSCID] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

const nr_dmrs_id_2stage_t *nr_pusch_passive_ul_dmrs_id(int nscid)
{
  return &g_ul_dmrs_id[nscid & 1];
}

nr_dmrs_id_2stage_t *nr_pusch_passive_ul_dmrs_trylock(int nscid, bool *was_initialized)
{
  const int ns = nscid & 1;
  if (pthread_mutex_trylock(&g_ul_dmrs_id_lock[ns]) != 0)
    return NULL;
  if (was_initialized)
    *was_initialized = g_ul_dmrs_id_init[ns];
  g_ul_dmrs_id_init[ns] = true;
  return &g_ul_dmrs_id[ns];
}

void nr_pusch_passive_ul_dmrs_unlock(int nscid)
{
  pthread_mutex_unlock(&g_ul_dmrs_id_lock[nscid & 1]);
}

/* ---- dataScramblingIdentityPUSCH walk + its eligibility window (Task 13, final review I1) --------- */
static nr_scrambling_id_sweep_t g_ul_data_id;
static bool                     g_ul_data_id_init;
static pthread_mutex_t          g_ul_data_id_lock = PTHREAD_MUTEX_INITIALIZER;
static nr_scr_link_t            g_ul_link;
/* Windowed/resettable (review fix round 1, finding 1): 0_1 CRC fails since the last 0_1 pass. */
static _Atomic uint32_t g_ul_fails_since_ok;

bool nr_pusch_passive_ul_walk_eligible(uint16_t rnti, int dmrs_decided_id)
{
  return nr_scrambling_walk_eligible(dmrs_decided_id, nr_scr_link_healthy(&g_ul_link, rnti),
                                     atomic_load_explicit(&g_ul_fails_since_ok, memory_order_relaxed));
}

void nr_pusch_passive_ul_crc_note(uint16_t rnti, bool dedicated, bool tb_crc_ok)
{
  nr_scr_link_note(&g_ul_link, rnti, dedicated, tb_crc_ok);
  if (!dedicated)
    return;
  if (tb_crc_ok)
    atomic_store_explicit(&g_ul_fails_since_ok, 0, memory_order_relaxed);
  else
    atomic_fetch_add_explicit(&g_ul_fails_since_ok, 1, memory_order_relaxed);
}

/* Review fix round 1, finding 1 (CRITICAL): `latched` must be checked BEFORE `advance_ok` -- an
 * already-decided id must survive regardless of whether the (now recovered) link still looks
 * "stalled". See task-13-report.md for the full bug history. */
uint16_t nr_pusch_passive_data_id_current(uint16_t pci, int dmrs_id, bool advance_ok)
{
  pthread_mutex_lock(&g_ul_data_id_lock);
  int id = -1;
  if (g_ul_data_id_init && g_ul_data_id.latched >= 0) {
    id = g_ul_data_id.latched;
  } else if (advance_ok) {
    if (!g_ul_data_id_init) {
      nr_scrambling_id_sweep_init(&g_ul_data_id, pci, dmrs_id);
      g_ul_data_id_init = true;
      LOG_A(PHY, "SENSING: UL DATA_ID_WALK START first candidate=%d (reason: UL DM-RS id %d decided, UL link healthy, "
                 ">= %d consecutive DCI 0_1 TB CRC fails)\n",
            nr_scrambling_id_sweep_current(&g_ul_data_id), dmrs_id, NR_SCR_WALK_MIN_FAILS);
    }
    id = nr_scrambling_id_sweep_current(&g_ul_data_id);
  }
  pthread_mutex_unlock(&g_ul_data_id_lock);
  return (uint16_t)(id >= 0 ? id : pci);
}

void nr_pusch_passive_data_id_feed(bool tb_crc_ok)
{
  pthread_mutex_lock(&g_ul_data_id_lock);
  if (g_ul_data_id_init && g_ul_data_id.latched < 0) {
    const int tried = nr_scrambling_id_sweep_current(&g_ul_data_id);
    nr_scrambling_id_sweep_feed(&g_ul_data_id, tb_crc_ok ? 1 : 0);
    if (g_ul_data_id.latched >= 0)
      LOG_A(PHY, "SENSING: UL DATA_ID_WALK LATCHED n_id=%d after %u tries (reason: TB CRC pass)\n", g_ul_data_id.latched,
            g_ul_data_id.tries);
    else
      LOG_A(PHY, "SENSING: UL DATA_ID_WALK STEP candidate=%d failed CRC -> next=%d (%d/%d)%s\n", tried,
            nr_scrambling_id_sweep_current(&g_ul_data_id), g_ul_data_id.pos, g_ul_data_id.n,
            g_ul_data_id.pos == 0 ? " WRAPPED: every candidate failed once, starting over from the PCI" : "");
  }
  pthread_mutex_unlock(&g_ul_data_id_lock);
}
