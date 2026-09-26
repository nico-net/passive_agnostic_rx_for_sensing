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

/* ---- UL DM-RS scrambling identity, one state per nSCID ------------------------------------- */
static nr_dmrs_id_state_t g_ul_dmrs_id[NR_UL_DMRS_NSCID];
static bool               g_ul_dmrs_id_init[NR_UL_DMRS_NSCID];
static pthread_mutex_t    g_ul_dmrs_id_lock[NR_UL_DMRS_NSCID] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
/* Same throttle constant/reasoning as the pre-move code (nr_dmrs_id_estimate_test.cc's
 * FindsAnIdAboveTheOldRange: a stage-2 accumulate is ~64x a stage-1 one). */
#define NR_UL_DMRS_STAGE2_THROTTLE 2048
static uint32_t g_ul_dmrs_stage2_skip[NR_UL_DMRS_NSCID];

const nr_dmrs_id_state_t *nr_pusch_passive_ul_dmrs_id(int nscid)
{
  return &g_ul_dmrs_id[nscid & 1];
}

nr_dmrs_id_state_t *nr_pusch_passive_ul_dmrs_trylock(int nscid, bool *was_initialized)
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

bool nr_pusch_passive_ul_dmrs_stage2_tick(int nscid)
{
  const int ns = nscid & 1;
  return (g_ul_dmrs_stage2_skip[ns]++ % NR_UL_DMRS_STAGE2_THROTTLE) == 0;
}

/* ---- dataScramblingIdentityPUSCH sweep + CRC-stall eligibility counter (Task 13) ------------ */
static nr_scrambling_id_sweep_t g_ul_data_id;
static bool                     g_ul_data_id_init;
static pthread_mutex_t          g_ul_data_id_lock = PTHREAD_MUTEX_INITIALIZER;
/* Review fix round 1, finding 1: windowed/resettable (fails SINCE THE LAST PASS), not a lifetime
 * try/ok ratio -- see nr_pusch_passive_ul_crc_note()'s call sites in nr_pusch_passive_decode.c for
 * why a lifetime counter permanently discards a just-latched correct id. */
static _Atomic uint32_t g_ul_fails_since_ok;

bool nr_pusch_passive_ul_crc_stalled(uint32_t min_tries)
{
  return atomic_load_explicit(&g_ul_fails_since_ok, memory_order_relaxed) >= min_tries;
}

void nr_pusch_passive_ul_crc_note(bool tb_crc_ok)
{
  if (tb_crc_ok)
    atomic_store_explicit(&g_ul_fails_since_ok, 0, memory_order_relaxed);
  else
    atomic_fetch_add_explicit(&g_ul_fails_since_ok, 1, memory_order_relaxed);
}

/* Review fix round 1, finding 1 (CRITICAL): `latched` must be checked BEFORE `advance_ok` -- an
 * already-decided id must survive regardless of whether the (now recovered) link still looks
 * "stalled". See task-13-report.md for the full bug history; unchanged by this move. */
uint16_t nr_pusch_passive_data_id_current(uint16_t pci, int dmrs_id, bool advance_ok)
{
  pthread_mutex_lock(&g_ul_data_id_lock);
  int id = -1;
  if (g_ul_data_id_init && g_ul_data_id.latched >= 0) {
    id = g_ul_data_id.latched;
  } else if (advance_ok) {
    if (!g_ul_data_id_init) { nr_scrambling_id_sweep_init(&g_ul_data_id, pci, dmrs_id); g_ul_data_id_init = true; }
    id = nr_scrambling_id_sweep_current(&g_ul_data_id);
  }
  pthread_mutex_unlock(&g_ul_data_id_lock);
  return (uint16_t)(id >= 0 ? id : pci);
}

void nr_pusch_passive_data_id_feed(bool tb_crc_ok)
{
  pthread_mutex_lock(&g_ul_data_id_lock);
  if (g_ul_data_id_init)
    nr_scrambling_id_sweep_feed(&g_ul_data_id, tb_crc_ok ? 1 : 0);
  pthread_mutex_unlock(&g_ul_data_id_lock);
}
