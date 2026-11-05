/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_traffic_decode.h"
#include "PHY/NR_UE_ISAC/nr_isac_mode.h"
#include "nr_pdcch_blind_monitor.h"
#include "nr_sense_unit.h"
#include "nr_sense_sched.h"
#include "common/utils/nr_cell.h"
#include "nr_snr_gate.h"
#include <pthread.h>
#include <string.h>

#define TD_RNTIS 128
typedef struct { bool used; uint16_t rnti; uint32_t grants, sgrants, fails; uint8_t k, bo, pend; uint64_t last_check_ns, last_t_ns, stamp; } td_ent_t;
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

static bool policy_wanted(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned, uint64_t t_ns, bool hrv_alert)
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


/* SENSING mode (plan S7, rulings I1 + round 3, S9): every grant of >= T_cell PRBs with a proven layout is decoded; the grants of an ENABLED unit (DL: PDSCH_DATA, UL: PUSCH_DMRS of the RNTI) are all decoded to the end. A disabled unit follows the traffic
 * policy above plus a sampled check decode (1 in NR_TRAFFIC_CHECK_EVERY_GRANTS grants per RNTI and direction, whatever the layout state), so that a TB still reaches the unit gate
 * now and then, the skip is counted and the scheduler keeps a (weighted, nr_sense_unit.c) rate for it. Non-C RNTIs are always decoded. */
bool nr_traffic_decode_wanted(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned,
                              uint64_t t_ns, bool hrv_alert)
{
  return nr_traffic_decode_wanted_prbs(dir, rnti, rnti_class, ls, ul_table_learned, t_ns, hrv_alert, 0, false);
}

bool nr_traffic_decode_wanted_prbs(int dir, uint16_t rnti, int rnti_class, nr_layout_state_t ls, bool ul_table_learned,
                                   uint64_t t_ns, bool hrv_alert, uint16_t est_prbs, bool layout_proven)
{
  if (nr_isac_mode() != NR_ISAC_MODE_SENSING)
    return policy_wanted(dir, rnti, rnti_class, ls, ul_table_learned, t_ns, hrv_alert);
  if (rnti_class != NR_BLIND_RNTI_CLASS_C) return true;
  /* S9: a grant of >= T_cell PRBs (the cell's adaptive continuity floor, sense_min_prb at the start) is decoded when its RNTI's layout is proven (CRC-solved, fixed 1_0, or trusted); an unproven layout is
   * never force-decoded wide (wide probes almost never pass [MEASURED OTA 2026-11-05]): it follows the unit state / traffic policy / sample below, and the solver learns it from its own probes */
  if (layout_proven && est_prbs >= (unsigned)nr_sense_cell_floor(nr_cell_idx())) return true;
  if (nr_sense_unit_enabled(dir == 0 ? NR_UNIT_PDSCH_DATA : NR_UNIT_PUSCH_DMRS, rnti)) return true;
  const bool pol = policy_wanted(dir, rnti, rnti_class, ls, ul_table_learned, t_ns, hrv_alert);
  td_tab_t *t = &g_td;
  pthread_mutex_lock(&g_td_mu);
  td_ent_t *e = ent(t, dir, rnti);
  e->sgrants++; /* grants seen since the last one let through */
  const bool sample = e->sgrants >= NR_TRAFFIC_CHECK_EVERY_GRANTS;
  const unsigned stands_for = e->sgrants;
  if (pol || sample) e->sgrants = 0;
  pthread_mutex_unlock(&g_td_mu);
  if (pol || sample) nr_sense_unit_offer(dir == 0 ? NR_UNIT_PDSCH_DATA : NR_UNIT_PUSCH_DMRS, rnti, stands_for); /* the unit's next skip stands for that many grants */
  return pol || sample;
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

/* sensing mode: the decode policy's verdict feedback only (no record, no counters, no reopen): resets the 1-in-32 grant counter and drives the UL backoff */
void nr_traffic_decode_feedback(uint16_t rnti, int dir, bool probe, int rv, int crc, float snr_db)
{
  if (crc < 0) return;
  if (dir == 1 && (!probe || crc == 1)) nr_traffic_probe_result(dir, rnti, crc == 1);
  if (!probe) (void)nr_traffic_decode_result(dir, rnti, rv, snr_db, crc == 1);
}

/* S9: largest-first order of the n grants competing in one occasion: ord[] = indices by descending prbs[], ties by original index (stable). Bounded: n <= 64 (insertion sort). */
int nr_sense_order_largest_first(const uint16_t *prbs, int n, int *ord)
{
  if (n > 64) n = 64;
  for (int i = 0; i < n; i++) {
    int j = i;
    while (j > 0 && prbs[ord[j - 1]] < prbs[i]) { ord[j] = ord[j - 1]; j--; }
    ord[j] = i;
  }
  return n;
}
