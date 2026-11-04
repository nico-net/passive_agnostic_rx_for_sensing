/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_traffic_decode.h"
#include "nr_pdcch_blind_monitor.h"
#include "common/utils/nr_cell.h"
#include "nr_snr_gate.h"
#include <pthread.h>
#include <string.h>

#define TD_RNTIS 128
typedef struct { bool used; uint16_t rnti; uint32_t grants, fails; uint8_t k, bo, pend; uint64_t last_check_ns, last_t_ns, stamp; } td_ent_t;
typedef struct { uint64_t clock; uint32_t ul_n; td_ent_t e[2][TD_RNTIS]; } td_tab_t;
/* DCIs seen per (dir, C-RNTI): its own table, so that the busy cell's other RNTIs cannot evict a count (exact LRU; shortcut: linear scan of 1024, fine at DCI rates) */
#define TD_SEEN 1024
typedef struct { bool used; uint16_t rnti; uint32_t n; uint64_t stamp; } seen_ent_t;
typedef struct { uint64_t clock; seen_ent_t e[2][TD_SEEN]; } seen_tab_t;
NR_CELL_STATIC(seen_tab_t, g_seen)
#define g_seen NR_CELL_REF(g_seen)
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

/* Backoff: after k consecutive failed grants only 1 grant in 2^k is decoded (k <= 4). pend = a wanted grant whose outcome is not in yet, so the
 * many variants of one grant count as one failure. (t->mu held) */
static bool bo_pass(td_ent_t *e)
{
  if (e->k && ++e->bo < (1u << e->k)) return false;
  e->bo = 0;
  e->pend = true;
  return true;
}

bool nr_traffic_decode_wanted(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned,
                              uint64_t t_ns, bool hrv_alert)
{
  if (rnti_class != NR_BLIND_RNTI_CLASS_C) return true;
  if (dir == 0 && ls != NR_LAYOUT_SOLVED) return false; /* FIXED: layout known; CANDIDATE/UNSOLVED: the solver's probes work */
  td_tab_t *t = &g_td;
  if (dir != 0) {
    /* E4: PUSCH is decoded until the UL layout is locked (FIXED: 0_0 needs no lock) and the MCS table is known; lock/table lost: again (backoff on failures).
     * After that, one UL grant in 32 of the CELL is still decoded: its CRC verdicts are what reopens a stale lock (crc_fail_run) or table (reopens_ul). */
    pthread_mutex_lock(&g_td_mu);
    const bool n32 = (t->ul_n++ % NR_TRAFFIC_UL_CHECK_EVERY) == 0;
    const bool w = (ul_table_learned && (ls == NR_LAYOUT_SOLVED || ls == NR_LAYOUT_FIXED)) ? n32 : bo_pass(ent(t, dir, rnti));
    pthread_mutex_unlock(&g_td_mu);
    return w;
  }
  pthread_mutex_lock(&g_td_mu);
  td_ent_t *e = ent(t, dir, rnti);
  if (!e->last_check_ns) e->last_check_ns = t_ns ? t_ns : 1;
  e->last_t_ns = t_ns;
  const bool want = hrv_alert || ++e->grants >= NR_TRAFFIC_CHECK_EVERY_GRANTS || (t_ns > e->last_check_ns && t_ns - e->last_check_ns >= NR_TRAFFIC_T_CHECK_NS);
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

void nr_traffic_decode_reset(void)
{
  pthread_mutex_lock(&g_td_mu);
  memset(&g_td, 0, sizeof g_td);
  memset(&g_seen, 0, sizeof g_seen);
  pthread_mutex_unlock(&g_td_mu);
}

int nr_traffic_note_dci(int dir, uint16_t rnti, bool count)
{
  pthread_mutex_lock(&g_td_mu);
  seen_tab_t *t = &g_seen;
  seen_ent_t *e = t->e[dir & 1], *v = e;
  int i = 0;
  for (; i < TD_SEEN; i++) {
    if (e[i].used && e[i].rnti == rnti) break;
    if (!e[i].used) v = &e[i];
    else if (v->used && e[i].stamp < v->stamp) v = &e[i];
  }
  seen_ent_t *x = i < TD_SEEN ? &e[i] : v;
  if (i == TD_SEEN) { memset(x, 0, sizeof *x); x->used = true; x->rnti = rnti; }
  x->stamp = ++t->clock;
  if (count && x->n < 1000000u) x->n++;
  const int n = (int)x->n;
  pthread_mutex_unlock(&g_td_mu);
  return n;
}

bool nr_traffic_probe_wanted(int cell, uint16_t rnti, int dir, int n_dci_seen, int n_alive, bool family_match, float snr_db, int mcs, int mcs_table)
{
  (void)cell; (void)rnti;   /* the state is per cell through NR_CELL_STATIC; no per-RNTI backoff: the solver's own search back-off / PARK rate-limit the probes */
  return (n_dci_seen >= 2 || family_match) && n_alive >= 1 && !nr_snr_no_chance_dir(snr_db, mcs, mcs_table, dir & 1);
}

void nr_traffic_probe_result(int dir, uint16_t rnti, bool crc_ok)
{
  pthread_mutex_lock(&g_td_mu);
  td_ent_t *e = ent(&g_td, dir, rnti);
  if (crc_ok) { e->k = 0; e->bo = 0; e->pend = false; }
  else if (e->pend) { e->pend = false; e->bo = 0; if (e->k < 4) e->k++; }
  pthread_mutex_unlock(&g_td_mu);
}
