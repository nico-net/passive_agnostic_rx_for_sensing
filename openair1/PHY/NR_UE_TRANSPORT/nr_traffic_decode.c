/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_traffic_decode.h"
#include "nr_pdcch_blind_monitor.h"
#include "common/utils/nr_cell.h"
#include <pthread.h>
#include <string.h>

#define TD_RNTIS 128
typedef struct { bool used; uint16_t rnti; uint32_t grants, fails; uint64_t last_check_ns, last_t_ns, stamp; } td_ent_t;
typedef struct { uint64_t clock; td_ent_t e[2][TD_RNTIS]; } td_tab_t;
NR_CELL_STATIC_INIT(pthread_mutex_t, g_td_mu, PTHREAD_MUTEX_INITIALIZER)
#define g_td_mu NR_CELL_REF(g_td_mu)
NR_CELL_STATIC(td_tab_t, g_td)
#define g_td NR_CELL_REF(g_td)

/* LRU entry of one direction (t->mu held). */
static td_ent_t *ent(td_tab_t *t, int dir, uint16_t rnti)
{
  td_ent_t *e = t->e[dir & 1], *v = e;
  for (int i = 0; i < TD_RNTIS; i++) {
    if (e[i].used && e[i].rnti == rnti) { e[i].stamp = ++t->clock; return &e[i]; }
    if (!e[i].used) v = &e[i];
    else if (v->used && e[i].stamp < v->stamp) v = &e[i];
  }
  memset(v, 0, sizeof *v);
  v->used = true; v->rnti = rnti; v->stamp = ++t->clock;
  return v;
}

bool nr_traffic_decode_wanted(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned,
                              uint64_t t_ns, bool hrv_alert)
{
  if (rnti_class != NR_BLIND_RNTI_CLASS_C) return true;
  if (dir == 0 && ls != NR_LAYOUT_SOLVED) return false; /* FIXED: layout known; CANDIDATE/UNSOLVED: the solver's probes work */
  if (dir != 0 && !ul_table_learned) return true;
  td_tab_t *t = &g_td;
  pthread_mutex_lock(&g_td_mu);
  td_ent_t *e = ent(t, dir, rnti);
  if (!e->last_check_ns) e->last_check_ns = t_ns ? t_ns : 1;
  e->last_t_ns = t_ns;
  const bool want = hrv_alert || ++e->grants >= NR_TRAFFIC_CHECK_EVERY_GRANTS || t_ns - e->last_check_ns >= NR_TRAFFIC_T_CHECK_NS;
  pthread_mutex_unlock(&g_td_mu);
  return want;
}

bool nr_traffic_decode_result(int dir, uint16_t rnti, int rv, float snr_db, bool crc_ok)
{
  td_tab_t *t = &g_td;
  pthread_mutex_lock(&g_td_mu);
  td_ent_t *e = ent(t, dir, rnti);
  e->grants = 0;
  e->last_check_ns = e->last_t_ns ? e->last_t_ns : 1;
  bool reopen = false;
  if (crc_ok) e->fails = 0;
  else if (rv == 0 && snr_db >= NR_TRAFFIC_FAIL_MIN_SNR_DB && ++e->fails >= NR_TRAFFIC_K_REOPEN) {
    e->fails = 0;
    reopen = true;
  }
  pthread_mutex_unlock(&g_td_mu);
  return reopen;
}
