#include "nr_passive_cfg_epoch.h"
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
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"

/* ---- UL DM-RS scrambling identity, one two-window state per nSCID (final review I5) -------- */
static nr_dmrs_id_2stage_t g_ul_dmrs_id[NR_UL_DMRS_NSCID];
static bool                g_ul_dmrs_id_init[NR_UL_DMRS_NSCID];
static pthread_mutex_t     g_ul_dmrs_id_lock[NR_UL_DMRS_NSCID] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

/* U4: fixed admission table. Tickets make stale CRC feedback from an earlier
 * incarnation or configuration epoch harmless. ID priors are ranking hints;
 * only a nonzero TB with a valid CRC confirms the IDs used for that attempt. */
#define UL_IDS_UE_CAP 64
typedef struct {
  nr_ue_ul_ticket_t ticket;
  uint16_t priors[3], dmrs_id[2], data_id;
  uint8_t n_priors, confirmed_mask, trial;
  bool used;
} ul_ids_ue_t;
static ul_ids_ue_t g_ue_ids[UL_IDS_UE_CAP];
static pthread_mutex_t g_ue_ids_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_ue_ids_dropped;
uint64_t nr_pusch_passive_ul_ids_dropped(void)
{ return atomic_load_explicit(&g_ue_ids_dropped,memory_order_relaxed); }

bool nr_pusch_passive_ul_ue_state_enabled(void)
{
  const char *v=getenv("ISAC_UL_UE_STATE");
  return v && strcmp(v,"1")==0;
}
static bool same_ticket(const nr_ue_ul_ticket_t *a,const nr_ue_ul_ticket_t *b)
{
  return a->rnti==b->rnti && a->identity_gen==b->identity_gen &&
         a->incarnation==b->incarnation && a->epoch==b->epoch;
}
static void prior_add(ul_ids_ue_t *s,int64_t value)
{
  if(value<0 || value>65535) return;
  for(int i=0;i<s->n_priors;i++) if(s->priors[i]==value) return;
  if(s->n_priors<3) s->priors[s->n_priors++]=(uint16_t)value;
}
void nr_pusch_passive_ul_ids_seed(const nr_ue_ul_ticket_t *t,const nr_ue_ctx_t *dl,uint16_t pci)
{
  if(!nr_pusch_passive_ul_ue_state_enabled() || !t || !dl ||
     dl->identity_gen!=t->identity_gen || dl->incarnation!=t->incarnation ||
     dl->cfg[NR_UEP_DCI_LEN_DL].value<0 || dl->cfg[NR_UEP_DCI_LEN_DL].verif!=NR_UEV_TRUSTED) return;
  if(pthread_mutex_trylock(&g_ue_ids_lock)) {
    atomic_fetch_add_explicit(&g_ue_ids_dropped,1,memory_order_relaxed); return;
  }
  ul_ids_ue_t *s=NULL;
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && g_ue_ids[i].ticket.rnti==t->rnti) {s=&g_ue_ids[i];break;}
  if(!s) for(int i=0;i<UL_IDS_UE_CAP;i++) if(!g_ue_ids[i].used) {s=&g_ue_ids[i];break;}
  if(s) {
    if(!s->used || !same_ticket(&s->ticket,t)) {memset(s,0,sizeof(*s));s->ticket=*t;s->used=true;}
    prior_add(s,pci);
    prior_add(s,dl->cfg[NR_UEP_PDSCH_SCR_ID].value);
    prior_add(s,dl->cfg[NR_UEP_PDCCH_SCR_ID].value);
  } else atomic_fetch_add_explicit(&g_ue_ids_dropped,1,memory_order_relaxed);
  pthread_mutex_unlock(&g_ue_ids_lock);
}
void nr_pusch_passive_ul_ids_confirm(const nr_ue_ul_ticket_t *t,uint8_t nscid,uint16_t dmrs_id,uint16_t data_id)
{
  if(!nr_pusch_passive_ul_ue_state_enabled() || !t) return;
  pthread_mutex_lock(&g_ue_ids_lock);
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && same_ticket(&g_ue_ids[i].ticket,t)) {
    g_ue_ids[i].dmrs_id[nscid&1]=dmrs_id;
    g_ue_ids[i].data_id=data_id;
    g_ue_ids[i].confirmed_mask|=1u<<(nscid&1);
    break;
  }
  pthread_mutex_unlock(&g_ue_ids_lock);
}
bool nr_pusch_passive_ul_ids_get(const nr_ue_ul_ticket_t *t,uint8_t nscid,uint16_t *dmrs_id,uint16_t *data_id)
{
  if(!nr_pusch_passive_ul_ue_state_enabled() || !t) return false;
  if(pthread_mutex_trylock(&g_ue_ids_lock)) return false;
  bool found=false;
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && same_ticket(&g_ue_ids[i].ticket,t) &&
                                     (g_ue_ids[i].confirmed_mask & (1u<<(nscid&1)))) {
    if(dmrs_id) *dmrs_id=g_ue_ids[i].dmrs_id[nscid&1];
    if(data_id) *data_id=g_ue_ids[i].data_id;
    found=true;break;
  }
  pthread_mutex_unlock(&g_ue_ids_lock);
  return found;
}
void nr_pusch_passive_ul_ids_current(uint16_t rnti,uint8_t nscid,uint16_t pci,
                                     uint16_t *dmrs_id,uint16_t *data_id)
{
  if(dmrs_id) *dmrs_id=pci;
  if(data_id) *data_id=pci;
  if(!nr_pusch_passive_ul_ue_state_enabled() || pthread_mutex_trylock(&g_ue_ids_lock)) return;
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && g_ue_ids[i].ticket.rnti==rnti) {
    ul_ids_ue_t *s=&g_ue_ids[i];
    if(s->n_priors) {
      const int ns=nscid&1;
      if(dmrs_id) *dmrs_id=(s->confirmed_mask&(1u<<ns))?s->dmrs_id[ns]:s->priors[(s->trial/s->n_priors)%s->n_priors];
      if(data_id) *data_id=s->confirmed_mask?s->data_id:s->priors[s->trial%s->n_priors];
    }
    break;
  }
  pthread_mutex_unlock(&g_ue_ids_lock);
}
void nr_pusch_passive_ul_ids_failed(const nr_ue_ul_ticket_t *t)
{
  if(!nr_pusch_passive_ul_ue_state_enabled() || !t) return;
  pthread_mutex_lock(&g_ue_ids_lock);
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && same_ticket(&g_ue_ids[i].ticket,t)) {
    if(!g_ue_ids[i].confirmed_mask && g_ue_ids[i].n_priors>1)
      g_ue_ids[i].trial=(g_ue_ids[i].trial+1)%(g_ue_ids[i].n_priors*g_ue_ids[i].n_priors);
    break;
  }
  pthread_mutex_unlock(&g_ue_ids_lock);
}
void nr_pusch_passive_ul_ids_reset(uint16_t rnti)
{
  pthread_mutex_lock(&g_ue_ids_lock);
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && g_ue_ids[i].ticket.rnti==rnti)
    memset(&g_ue_ids[i],0,sizeof(g_ue_ids[i]));
  pthread_mutex_unlock(&g_ue_ids_lock);
}
void nr_pusch_passive_ul_ids_reset_all(void)
{
  pthread_mutex_lock(&g_ue_ids_lock);
  memset(g_ue_ids,0,sizeof(g_ue_ids));
  pthread_mutex_unlock(&g_ue_ids_lock);
}

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
  if (!nr_cfg_epoch_work_current()) return;
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
  if (!nr_cfg_epoch_work_current()) return;
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
