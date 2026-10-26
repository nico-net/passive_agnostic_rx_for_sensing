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
#include "nr_ul_first_pusch.h"
#include "PHY/NR_UE_TRANSPORT/nr_scrambling_id_sweep.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"

/* ---- UL DM-RS scrambling identity, one two-window state per nSCID (final review I5) -------- */
typedef nr_dmrs_id_2stage_t g_ul_dmrs_id_cell_t[NR_UL_DMRS_NSCID];
NR_CELL_STATIC(g_ul_dmrs_id_cell_t, g_ul_dmrs_id);
#define g_ul_dmrs_id NR_CELL_REF(g_ul_dmrs_id)
typedef bool g_ul_dmrs_id_init_cell_t[NR_UL_DMRS_NSCID];
NR_CELL_STATIC(g_ul_dmrs_id_init_cell_t, g_ul_dmrs_id_init);
#define g_ul_dmrs_id_init NR_CELL_REF(g_ul_dmrs_id_init)
typedef pthread_mutex_t g_ul_dmrs_id_lock_cell_t[NR_UL_DMRS_NSCID];
NR_CELL_STATIC_INIT(g_ul_dmrs_id_lock_cell_t, g_ul_dmrs_id_lock, {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER});
#define g_ul_dmrs_id_lock NR_CELL_REF(g_ul_dmrs_id_lock)

/* U4: fixed admission table. Tickets make stale CRC feedback from an earlier
 * incarnation or configuration epoch harmless. ID priors are ranking hints;
 * only a nonzero TB with a valid CRC confirms the IDs used for that attempt. */
#define UL_IDS_UE_CAP 64
typedef struct {
  nr_ue_ul_ticket_t ticket;
  uint16_t priors[3], dmrs_id[2], data_id;
  uint16_t data_priors[3]; /* dataScramblingIdentityPUSCH hypotheses, best first (see nr_pusch_passive_ul_ids_seed) */
  uint8_t n_priors, n_data_priors, confirmed_mask, trial;
  uint32_t failed_grants; /* U7: one bounded data-ID candidate per failed grant */
  bool used;
} ul_ids_ue_t;
typedef ul_ids_ue_t g_ue_ids_cell_t[UL_IDS_UE_CAP];
NR_CELL_STATIC(g_ue_ids_cell_t, g_ue_ids);
#define g_ue_ids NR_CELL_REF(g_ue_ids)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_ue_ids_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_ue_ids_lock NR_CELL_REF(g_ue_ids_lock)
NR_CELL_STATIC(_Atomic uint64_t, g_ue_ids_dropped);
#define g_ue_ids_dropped NR_CELL_REF(g_ue_ids_dropped)
uint64_t nr_pusch_passive_ul_ids_dropped(void)
{ return atomic_load_explicit(&g_ue_ids_dropped,memory_order_relaxed); }

/* dataScramblingIdentityPUSCH LOCK per RNTI. The id is RRC configuration (TS 38.331 PUSCH-Config; TS 38.211 6.3.1.1 c_init):
 * it does not change with the receiver's own configuration epochs, DCI lengths or tickets. A TB CRC pass with a nonzero TB
 * proves the id used for that grant; from then on every grant of the RNTI uses it, and neither the ticket-keyed priors nor
 * the per-failure walk below may replace it. Before this lock a new ticket (epoch change) wiped the confirmation and, after
 * 9 failures -- which wide grants produced for other reasons (SFO, UCI) -- 15 of 16 grants were sent with walked, wrong
 * ids ([MEASURED, sens6 OTA lab cell, dd14_lab_20261025_154832]: 2.2 k grants on ~400 wrong ids, none decoding).
 * Released only on CRC evidence: a TB pass with a different id for this RNTI (the background id search sweeps the failing
 * grants and hands its CB-CRC-confirmed id to the decoder) moves the lock; the RNTI's release clears it. No failure count.
 *
 * Only a DECISIVE pass sets or moves it (the rule of the UL layout lock, lock_decisive): a TB with a 24-bit CRC
 * (TBS > 3824 bits, TS 38.212 7.2.1 -- with C > 1 every code-block CRC24B passed as well), or a 16-bit-CRC TB confirmed by
 * a second pass of the same id on another slot. A single CRC16 pass of a walked or swept id is a 1-in-65536 chance event,
 * and the walk / background search try thousands of ids: on air one such pass locked n_id=744 instead of the PCI-default 2
 * and every later grant of the RNTI failed ([MEASURED, sens6 OTA lab, ulint1_lab_20261025_211640]: 82 of 175537 decoded
 * vs 40671 of 87815 without the lock). Until a decisive pass the cell default (n_ID = PCI, TS 38.211 6.3.1.1, when
 * dataScramblingIdentityPUSCH is absent) stays the first hypothesis, as the walk and the priors already order it. */
#define UL_DATA_LOCK_CAP 64
/* entry: (1 << 32) | rnti << 16 | id, read lock-free (the DCI path must not block); written under g_ue_ids_lock */
typedef _Atomic uint64_t g_data_lock_cell_t[UL_DATA_LOCK_CAP];
NR_CELL_STATIC(g_data_lock_cell_t, g_data_lock);
#define g_data_lock NR_CELL_REF(g_data_lock)
/* a non-decisive (CRC16) pass waiting for its confirmation on another slot, per RNTI: (rnti, id, slot) */
typedef struct {
  uint16_t rnti, id;
  int64_t slot;
  bool used;
} ul_data_pending_t;
typedef ul_data_pending_t g_data_pending_cell_t[UL_DATA_LOCK_CAP];
NR_CELL_STATIC(g_data_pending_cell_t, g_data_pending);
#define g_data_pending NR_CELL_REF(g_data_pending)
NR_CELL_STATIC(uint32_t, g_data_pending_next);
#define g_data_pending_next NR_CELL_REF(g_data_pending_next)
NR_CELL_STATIC(uint32_t, g_data_lock_next);
#define g_data_lock_next NR_CELL_REF(g_data_lock_next)

static int data_lock_slot(uint16_t rnti)
{
  for (int i = 0; i < UL_DATA_LOCK_CAP; i++) {
    const uint64_t v = atomic_load_explicit(&g_data_lock[i], memory_order_acquire);
    if ((v >> 32) && (uint16_t)(v >> 16) == rnti)
      return i;
  }
  return -1;
}

bool nr_pusch_passive_ul_data_id_locked(uint16_t rnti, uint16_t *data_id)
{
  const int i = data_lock_slot(rnti);
  if (i < 0)
    return false;
  if (data_id)
    *data_id = (uint16_t)atomic_load_explicit(&g_data_lock[i], memory_order_acquire);
  return true;
}

static void data_lock_ok_locked(uint16_t rnti, uint16_t id)
{
  int i = data_lock_slot(rnti);
  const uint64_t v = (1ull << 32) | ((uint64_t)rnti << 16) | id;
  if (i < 0) {
    for (int k = 0; k < UL_DATA_LOCK_CAP && i < 0; k++)
      if (!(atomic_load_explicit(&g_data_lock[k], memory_order_relaxed) >> 32))
        i = k;
    if (i < 0)
      i = (int)(g_data_lock_next++ % UL_DATA_LOCK_CAP);
    LOG_A(PHY, "SENSING: UL DATA_ID LOCK rnti=0x%04x n_id=%u (TB CRC pass)\n", rnti, id);
  } else if ((uint16_t)atomic_load_explicit(&g_data_lock[i], memory_order_relaxed) != id) {
    LOG_A(PHY, "SENSING: UL DATA_ID LOCK rnti=0x%04x n_id %u -> %u (TB CRC pass)\n", rnti,
          (unsigned)(uint16_t)atomic_load_explicit(&g_data_lock[i], memory_order_relaxed), id);
  }
  atomic_store_explicit(&g_data_lock[i], v, memory_order_release);
}

static void data_lock_clear_locked(uint16_t rnti)
{
  const int i = data_lock_slot(rnti);
  if (i >= 0)
    atomic_store_explicit(&g_data_lock[i], 0, memory_order_release);
}

void nr_pusch_passive_ul_ids_flags_reset_for_test(void) {} /* the UL chain is always on: nothing cached */
bool nr_pusch_passive_ul_ue_state_enabled(void) { return true; }
static bool same_ticket(const nr_ue_ul_ticket_t *a,const nr_ue_ul_ticket_t *b)
{
  return a->rnti==b->rnti && a->identity_gen==b->identity_gen &&
         a->incarnation==b->incarnation && a->epoch==b->epoch;
}
static void data_prior_add(ul_ids_ue_t *s,int64_t value)
{
  if(value<0 || value>1023) return; /* a data scrambling id is 0..1023 (TS 38.211 6.3.1.1) */
  for(int i=0;i<s->n_data_priors;i++) if(s->data_priors[i]==value) return;
  if(s->n_data_priors<3) s->data_priors[s->n_data_priors++]=(uint16_t)value;
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
    /* UL data scrambling id hypotheses (TS 38.331: dataScramblingIdentityPUSCH and dataScramblingIdentityPDSCH are separate
     * fields and may differ, so none of these is assumed -- only a decisive UL CRC pass locks an id). Order: the id a DL
     * PDSCH CRC pass of this RNTI confirmed (a gNB that configures one id per UE very often uses it for both directions),
     * then the cell default (n_ID = PCI, used when the field is absent), then the PDCCH id; the walk follows. */
    if(dl->cfg[NR_UEP_PDSCH_SCR_ID].source!=NR_UES_UNKNOWN && dl->cfg[NR_UEP_PDSCH_SCR_ID].verif==NR_UEV_TRUSTED)
      data_prior_add(s,dl->cfg[NR_UEP_PDSCH_SCR_ID].value);
    data_prior_add(s,pci);
    data_prior_add(s,dl->cfg[NR_UEP_PDCCH_SCR_ID].value);
  } else atomic_fetch_add_explicit(&g_ue_ids_dropped,1,memory_order_relaxed);
  pthread_mutex_unlock(&g_ue_ids_lock);
}
/* caller holds g_ue_ids_lock: is this pass decisive (see the lock's comment)? A CRC16 pass is recorded and becomes decisive
 * when the same id passes again for the same RNTI on another slot. */
static bool data_pass_decisive_locked(uint16_t rnti, uint16_t id, uint32_t tbs_bits, int64_t abs_slot)
{
  if (tbs_bits > 3824)
    return true;
  ul_data_pending_t *p = NULL;
  for (int i = 0; i < UL_DATA_LOCK_CAP && !p; i++)
    if (g_data_pending[i].used && g_data_pending[i].rnti == rnti)
      p = &g_data_pending[i];
  if (p && p->id == id && p->slot != abs_slot) {
    memset(p, 0, sizeof(*p));
    return true;
  }
  if (!p) {
    for (int i = 0; i < UL_DATA_LOCK_CAP && !p; i++)
      if (!g_data_pending[i].used)
        p = &g_data_pending[i];
    if (!p)
      p = &g_data_pending[g_data_pending_next++ % UL_DATA_LOCK_CAP];
  }
  *p = (ul_data_pending_t){.rnti = rnti, .id = id, .slot = abs_slot, .used = true};
  return false;
}

void nr_pusch_passive_ul_ids_confirm(const nr_ue_ul_ticket_t *t,uint8_t nscid,uint16_t dmrs_id,uint16_t data_id,
                                     uint32_t tbs_bits,int64_t abs_slot)
{
  if(!nr_pusch_passive_ul_ue_state_enabled() || !t) return;
  pthread_mutex_lock(&g_ue_ids_lock);
  if(!data_pass_decisive_locked(t->rnti,data_id,tbs_bits,abs_slot)) { pthread_mutex_unlock(&g_ue_ids_lock); return; }
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && same_ticket(&g_ue_ids[i].ticket,t)) {
    g_ue_ids[i].dmrs_id[nscid&1]=dmrs_id;
    g_ue_ids[i].data_id=data_id;
    g_ue_ids[i].confirmed_mask|=1u<<(nscid&1);
    break;
  }
  data_lock_ok_locked(t->rnti, data_id);
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
  if(!nr_pusch_passive_ul_ue_state_enabled()) return;
  uint16_t locked_id=0;
  const bool locked=nr_pusch_passive_ul_data_id_locked(rnti,&locked_id); /* lock-free: holds even when the trylock below loses */
  if(data_id && locked) *data_id=locked_id;
  if(pthread_mutex_trylock(&g_ue_ids_lock)) return;
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && g_ue_ids[i].ticket.rnti==rnti) {
    ul_ids_ue_t *s=&g_ue_ids[i];
    if(s->n_priors) {
      const int ns=nscid&1;
      if(dmrs_id) *dmrs_id=(s->confirmed_mask&(1u<<ns))?s->dmrs_id[ns]:s->priors[(s->trial/s->n_priors)%s->n_priors];
      if(data_id) {
        if (locked) *data_id=locked_id;
    else if (s->confirmed_mask) *data_id=s->data_id;
        else if (s->failed_grants>=9 && (s->failed_grants%16)!=0)
          *data_id=(uint16_t)((s->failed_grants-9-(s->failed_grants-1)/16)%1024);
        else { /* a data id is 0..1023; DL scrambling priors may be larger (DM-RS ids go to 65535): skip those, fall back to the PCI */
          uint16_t d=pci;
          if(s->n_data_priors) d=s->data_priors[s->trial%s->n_data_priors];
          *data_id=d;
        }
      }
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
    if(!g_ue_ids[i].confirmed_mask && g_ue_ids[i].failed_grants<UINT32_MAX)
      ++g_ue_ids[i].failed_grants;
    break;
  }
  pthread_mutex_unlock(&g_ue_ids_lock);
}
void nr_pusch_passive_ul_ids_reset(uint16_t rnti)
{
  pthread_mutex_lock(&g_ue_ids_lock);
  for(int i=0;i<UL_IDS_UE_CAP;i++) if(g_ue_ids[i].used && g_ue_ids[i].ticket.rnti==rnti)
    memset(&g_ue_ids[i],0,sizeof(g_ue_ids[i]));
  data_lock_clear_locked(rnti);
  for(int i=0;i<UL_DATA_LOCK_CAP;i++) if(g_data_pending[i].used && g_data_pending[i].rnti==rnti) memset(&g_data_pending[i],0,sizeof(g_data_pending[i]));
  pthread_mutex_unlock(&g_ue_ids_lock);
}
void nr_pusch_passive_ul_ids_reset_all(void)
{
  pthread_mutex_lock(&g_ue_ids_lock);
  memset(g_ue_ids,0,sizeof(g_ue_ids));
  for(int i=0;i<UL_DATA_LOCK_CAP;i++) atomic_store_explicit(&g_data_lock[i],0,memory_order_release);
  memset(g_data_pending,0,sizeof(g_data_pending));
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
NR_CELL_STATIC(nr_scrambling_id_sweep_t, g_ul_data_id);
#define g_ul_data_id NR_CELL_REF(g_ul_data_id)
NR_CELL_STATIC(bool, g_ul_data_id_init);
#define g_ul_data_id_init NR_CELL_REF(g_ul_data_id_init)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_ul_data_id_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_ul_data_id_lock NR_CELL_REF(g_ul_data_id_lock)
NR_CELL_STATIC(nr_scr_link_t, g_ul_link);
#define g_ul_link NR_CELL_REF(g_ul_link)
/* Windowed/resettable (review fix round 1, finding 1): 0_1 CRC fails since the last 0_1 pass. */
NR_CELL_STATIC(_Atomic uint32_t, g_ul_fails_since_ok);
#define g_ul_fails_since_ok NR_CELL_REF(g_ul_fails_since_ok)

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
