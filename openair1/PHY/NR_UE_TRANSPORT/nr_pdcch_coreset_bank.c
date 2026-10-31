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

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.c
 * \brief See nr_pdcch_coreset_bank.h -- moved verbatim out of nr_pdcch_blind_monitor_rt.c, same
 * statics semantics and log lines, only renamed to external linkage (this is now a separate
 * translation unit) and re-exposed to the RT file through accessors instead of shared globals.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.h"

#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <math.h>

#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include "nr_pdcch_cell.h"

/* Verified dedicated geometries remain operational while discovery continues for another UE's
 * CORESET. Entries are immutable after the release-store publishes them, so the receive producer
 * may test the count while the single scan consumer appends without a lock. */
typedef nr_pdcch_discovered_coreset_t g_coreset_bank_cell_t[NR_PDCCH_DISCOVERED_CORESETS];
NR_CELL_STATIC_HEAP(g_coreset_bank_cell_t, g_coreset_bank);
#define g_coreset_bank NR_CELL_HEAP_REF(g_coreset_bank_cell_t, g_coreset_bank)
NR_CELL_STATIC(_Atomic int, g_coreset_bank_n);
#define g_coreset_bank_n NR_CELL_REF(g_coreset_bank_n)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_coreset_bank_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_coreset_bank_lock NR_CELL_REF(g_coreset_bank_lock)
/* Guards the AL1 UNION fields of the entries (n_al1_fam, al1_fam, n_al1_union, al1_union). Lock order: bank lock, then this one; no holder of this mutex takes the bank lock. */
pthread_mutex_t nr_pdcch_al1_mu = PTHREAD_MUTEX_INITIALIZER;
NR_CELL_STATIC_INIT(pthread_cond_t, g_coreset_bank_quiescent, PTHREAD_COND_INITIALIZER);
#define g_coreset_bank_quiescent NR_CELL_REF(g_coreset_bank_quiescent)
NR_CELL_STATIC(unsigned, g_dispatch_readers);
#define g_dispatch_readers NR_CELL_REF(g_dispatch_readers)
NR_CELL_STATIC(bool, g_compaction_pending);
#define g_compaction_pending NR_CELL_REF(g_compaction_pending)
NR_CELL_STATIC(uint64_t, g_bank_last_slot);
#define g_bank_last_slot NR_CELL_REF(g_bank_last_slot)
NR_CELL_STATIC(_Atomic bool, g_identity_reset_pending);
#define g_identity_reset_pending NR_CELL_REF(g_identity_reset_pending)
NR_CELL_STATIC(uint32_t, g_bank_epoch);
#define g_bank_epoch NR_CELL_REF(g_bank_epoch)
NR_CELL_STATIC(_Atomic uint32_t, g_bank_identity);
#define g_bank_identity NR_CELL_REF(g_bank_identity)
typedef struct {
  uint32_t identity_gen;
  int n;
  nr_pdcch_discovered_coreset_t entry[NR_PDCCH_DISCOVERED_CORESETS];
} g_dormant_bank_cell_t;
NR_CELL_STATIC_HEAP(g_dormant_bank_cell_t, g_dormant_bank);
#define g_dormant_bank NR_CELL_HEAP_REF(g_dormant_bank_cell_t, g_dormant_bank)
static void bank_epoch_listener(const nr_cfg_epoch_snapshot_t *s);
static void bank_epoch_locked(const nr_cfg_epoch_snapshot_t *s);
typedef void (*g_remove_hook_cell_t)(const nr_pdcch_blind_monitor_cfg_t *, void *);
NR_CELL_STATIC(g_remove_hook_cell_t, g_remove_hook);
#define g_remove_hook NR_CELL_REF(g_remove_hook)
NR_CELL_STATIC(void *, g_remove_hook_arg);
#define g_remove_hook_arg NR_CELL_REF(g_remove_hook_arg)

/* Caller holds the bank lock. Physical compaction waits for all RT dispatchers to leave: their
 * cfg and AL1 pointers are borrowed for an entire pass, including its unlocked decode phase. */
static void compact_removed(void)
{
  if (g_dispatch_readers)
    return;
  int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (g_identity_reset_pending) {
    pthread_mutex_lock(&nr_pdcch_al1_mu); /* entries carry the AL1 union fields (bank lock, then al1 mutex) */
    g_dormant_bank.n = n;
    memcpy(g_dormant_bank.entry, g_coreset_bank, sizeof(g_coreset_bank));
    memset(g_coreset_bank, 0, sizeof(g_coreset_bank));
    pthread_mutex_unlock(&nr_pdcch_al1_mu);
    atomic_store_explicit(&g_coreset_bank_n, 0, memory_order_release);
    g_identity_reset_pending = false;
    n = 0;
  }
  for (int i = 0; i < n;) {
    if (g_coreset_bank[i].state != NR_CORESET_REMOVED) {
      ++i;
      continue;
    }
    if (g_remove_hook)
      g_remove_hook(&g_coreset_bank[i].cfg, g_remove_hook_arg);
    LOG_A(PHY, "SENSING: CORESET bank REMOVED index=%d offset=%d\n", i,
          g_coreset_bank[i].cfg.coreset_rb_offset);
    pthread_mutex_lock(&nr_pdcch_al1_mu);
    memmove(&g_coreset_bank[i], &g_coreset_bank[i + 1], (n - i - 1) * sizeof(g_coreset_bank[0]));
    memset(&g_coreset_bank[--n], 0, sizeof(g_coreset_bank[0]));
    pthread_mutex_unlock(&nr_pdcch_al1_mu);
    atomic_store_explicit(&g_coreset_bank_n, n, memory_order_release);
  }
  g_compaction_pending = false;
  pthread_cond_broadcast(&g_coreset_bank_quiescent);
}

void nr_pdcch_coreset_bank_dispatch_enter(void)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (nr_cfg_reconf_enabled() && g_bank_epoch != nr_cfg_epoch_current()) {
    const nr_cfg_epoch_snapshot_t s = nr_cfg_epoch_snapshot();
    bank_epoch_locked(&s);
  }
  while (g_compaction_pending)
    pthread_cond_wait(&g_coreset_bank_quiescent, &g_coreset_bank_lock);
  ++g_dispatch_readers;
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

void nr_pdcch_coreset_bank_dispatch_leave(void)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (--g_dispatch_readers == 0)
    compact_removed();
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

void nr_pdcch_coreset_bank_set_remove_hook(void (*hook)(const nr_pdcch_blind_monitor_cfg_t *, void *), void *arg)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  g_remove_hook = hook;
  g_remove_hook_arg = arg;
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

int nr_pdcch_coreset_bank_remove(int index)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (index < 0 || index >= n || g_coreset_bank[index].state == NR_CORESET_REMOVED) {
    pthread_mutex_unlock(&g_coreset_bank_lock);
    return -1;
  }
  g_coreset_bank[index].state = NR_CORESET_REMOVED;
  g_compaction_pending = true;
  compact_removed();
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return 0;
}

void nr_pdcch_coreset_bank_note_accept(int index, uint64_t slot)
{
  if (!nr_cfg_epoch_work_current()) return;
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_coreset_bank_lock); return; }
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (index >= 0 && index < n && g_coreset_bank[index].state != NR_CORESET_REMOVED) {
    nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[index];
    if (slot > g_bank_last_slot) g_bank_last_slot = slot;
    if (slot > e->last_accept_slot)
      e->last_accept_slot = slot;
    if (e->accepts_window != UINT32_MAX)
      ++e->accepts_window;
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

/* note_confirm: one independent CRC-valid DCI on this entry (not a duplicate grant), counted once per distinct slot. Caller: the non-dup accept paths. */
void nr_pdcch_coreset_bank_note_confirm(int index, uint64_t slot)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (index >= 0 && index < n && g_coreset_bank[index].state != NR_CORESET_REMOVED && slot != g_coreset_bank[index].confirm_slot) {
    g_coreset_bank[index].confirm_slot = slot;
    __atomic_store_n(&g_coreset_bank[index].accepts_total, g_coreset_bank[index].accepts_total + 1, __ATOMIC_RELAXED);
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

/* Ring of recent DCIs decoded by operational (non-dwell) passes, keyed by (slot, RNTI, payload hash). A PDCCH transmission belongs to exactly one CORESET/search-space candidate, so a dwell geometry whose CRC-valid DCI is one of these
 * is decoding a transmission the bank already explains (an overlapping geometry can CRC-pass on the same REs) and gives no evidence of its own. */
#define NR_DECODED_RING 256
typedef struct { uint64_t slot; uint32_t hash; uint16_t rnti; } g_decoded_cell_t[NR_DECODED_RING];
NR_CELL_STATIC(g_decoded_cell_t, g_decoded);
#define g_decoded NR_CELL_REF(g_decoded)
NR_CELL_STATIC(unsigned, g_decoded_next);
#define g_decoded_next NR_CELL_REF(g_decoded_next)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_decoded_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_decoded_lock NR_CELL_REF(g_decoded_lock)

void nr_pdcch_coreset_bank_note_decoded(uint64_t slot, uint16_t rnti, uint32_t payload_hash)
{
  if (!rnti) return;
  pthread_mutex_lock(&g_decoded_lock);
  for (int i = 0; i < NR_DECODED_RING; ++i)
    if (g_decoded[i].rnti == rnti && g_decoded[i].slot == slot && g_decoded[i].hash == payload_hash) {
      pthread_mutex_unlock(&g_decoded_lock);
      return;
    }
  g_decoded[g_decoded_next % NR_DECODED_RING].slot = slot;
  g_decoded[g_decoded_next % NR_DECODED_RING].rnti = rnti;
  g_decoded[g_decoded_next % NR_DECODED_RING].hash = payload_hash;
  ++g_decoded_next;
  pthread_mutex_unlock(&g_decoded_lock);
}

bool nr_pdcch_coreset_bank_decoded_elsewhere(uint64_t slot, uint16_t rnti, uint32_t payload_hash)
{
  bool found = false;
  pthread_mutex_lock(&g_decoded_lock);
  for (int i = 0; i < NR_DECODED_RING && !found; ++i)
    found = g_decoded[i].rnti == rnti && g_decoded[i].slot == slot && g_decoded[i].hash == payload_hash;
  pthread_mutex_unlock(&g_decoded_lock);
  return found;
}

void nr_pdcch_coreset_bank_decoded_clear(void)
{
  pthread_mutex_lock(&g_decoded_lock);
  memset(g_decoded, 0, sizeof(g_decoded));
  g_decoded_next = 0;
  pthread_mutex_unlock(&g_decoded_lock);
}

/* ---- Downstream PDSCH certification (B1/B2, 2026-10-30) ----
 * Measured OTA on PCI 403: the CORESET adopted differed between runs (nID 32896: 8-42 RNTIs solved; nID 52320: 0-5 solved, 342 TB passes in 38534 decodes) and a
 * same-nID, same-RBs entry on the next symbol collected 150-905 grants with 0 passes. A CRC-valid DCI proves a PDCCH, not that this geometry carries this cell's
 * data. Each entry therefore counts its non-duplicate grants and the TB CRC passes of those grants' RNTIs; an entry with grants and no pass is retired when another
 * entry's Wilson lower pass rate makes "no pass in n grants" significant, and the retired footprint is re-admitted only after a backoff that doubles with each
 * retirement (starting at the time the evidence took), reset by a configuration epoch. */
#define NR_CERT_MAP 4096
typedef struct { uint16_t rnti; uint8_t serial; } g_rnti_src_cell_t[NR_CERT_MAP];
NR_CELL_STATIC(g_rnti_src_cell_t, g_rnti_src);
#define g_rnti_src NR_CELL_REF(g_rnti_src)
NR_CELL_STATIC(uint8_t, g_serial_next);
#define g_serial_next NR_CELL_REF(g_serial_next)
#define NR_RETIRED_MAX 16
typedef struct { int nid, rb, groups; uint64_t until, span; uint8_t n; bool used; } retired_t;
typedef retired_t g_retired_cell_t[NR_RETIRED_MAX];
NR_CELL_STATIC(g_retired_cell_t, g_retired);
#define g_retired NR_CELL_REF(g_retired)
NR_CELL_STATIC(unsigned, g_retired_w);
#define g_retired_w NR_CELL_REF(g_retired_w)

void nr_pdcch_cert_wilson(uint32_t k, uint32_t n, double z, double *lo, double *hi)
{
  if (!n) { *lo = 0.0; *hi = 1.0; return; }
  const double p = (double)k / n, z2 = z * z, d = 1.0 + z2 / n, c = p + z2 / (2.0 * n);
  const double h = z * sqrt(p * (1.0 - p) / n + z2 / (4.0 * (double)n * n));
  *lo = (c - h) / d > 0.0 ? (c - h) / d : 0.0;
  *hi = (c + h) / d < 1.0 ? (c + h) / d : 1.0;
}

bool nr_pdcch_cert_zero_pass_significant(uint32_t n, double p_lo, double alpha)
{
  if (!n || p_lo <= 0.0) return false;
  if (p_lo >= 1.0) return true;
  return (double)n * log1p(-p_lo) < log(alpha);
}

static nr_pdcch_discovered_coreset_t *entry_by_serial_locked(uint8_t serial)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  for (int i = 0; serial && i < n; ++i)
    if (g_coreset_bank[i].serial == serial && g_coreset_bank[i].state != NR_CORESET_REMOVED) return &g_coreset_bank[i];
  return NULL;
}

void nr_pdcch_coreset_bank_note_grant(int index, uint16_t rnti)
{
  if (!rnti || !nr_cfg_epoch_work_current()) return;
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (index >= 0 && index < n && g_coreset_bank[index].state != NR_CORESET_REMOVED) {
    nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[index];
    if (e->grants != UINT32_MAX) ++e->grants;
    g_rnti_src[rnti % NR_CERT_MAP].rnti = rnti;
    g_rnti_src[rnti % NR_CERT_MAP].serial = e->serial;
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

void nr_pdcch_coreset_bank_note_pdsch(uint16_t rnti, bool dedicated, bool crc_ok)
{
  if (!rnti || !dedicated || !crc_ok || !nr_cfg_epoch_work_current()) return;
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (g_rnti_src[rnti % NR_CERT_MAP].rnti == rnti) {
    nr_pdcch_discovered_coreset_t *e = entry_by_serial_locked(g_rnti_src[rnti % NR_CERT_MAP].serial);
    if (e) {
      if (e->passes != UINT32_MAX) ++e->passes;
      if (e->probation) {
        e->probation = 0;
        LOG_A(PHY, "SENSING: CORESET bank CERTIFIED (probation entry) nid=%d offset=%d symbol=%d: own TB CRC pass rnti=0x%04x grants=%u\n",
              e->cfg.coreset_pdcch_dmrs_scrambling_id, e->cfg.coreset_rb_offset, e->cfg.ss_first_symbol, rnti, e->grants);
      }
    }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

bool nr_pdcch_coreset_bank_certified(int index)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  const bool r = index >= 0 && index < n && g_coreset_bank[index].state != NR_CORESET_REMOVED
                 && g_coreset_bank[index].passes > 0 && !g_coreset_bank[index].probation;
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return r;
}

bool nr_pdcch_coreset_bank_nid_certified(int nid)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  bool r = false;
  for (int i = 0; i < n && !r; ++i)
    r = g_coreset_bank[i].state != NR_CORESET_REMOVED && g_coreset_bank[i].cfg.coreset_pdcch_dmrs_scrambling_id == nid
        && g_coreset_bank[i].passes > 0 && !g_coreset_bank[i].probation;
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return r;
}

static bool rb_overlap(int a, int an, int b, int bn) { return a < b + bn && b < a + an; }

static const retired_t *retired_match_locked(int nid, int rb, int groups)
{
  for (int i = 0; i < NR_RETIRED_MAX; ++i)
    if (g_retired[i].used && g_retired[i].nid == nid && rb_overlap(rb, 6 * groups, g_retired[i].rb, 6 * g_retired[i].groups)
        && g_bank_last_slot < g_retired[i].until)
      return &g_retired[i];
  return NULL;
}

bool nr_pdcch_coreset_bank_readmit_blocked(int nid, int grid_rb, int groups)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const bool r = retired_match_locked(nid, grid_rb, groups) != NULL;
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return r;
}

/* Caller holds the bank lock and calls compact_removed() afterwards. */
static int assess_locked(uint64_t slot)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  int retired = 0;
  double lo[NR_PDCCH_DISCOVERED_CORESETS] = {0};
  for (int i = 0; i < n; ++i) {
    double hi;
    if (g_coreset_bank[i].state != NR_CORESET_REMOVED)
      nr_pdcch_cert_wilson(g_coreset_bank[i].passes, g_coreset_bank[i].grants, NR_PDCCH_CERT_Z, &lo[i], &hi);
  }
  for (int i = 0; i < n; ++i) {
    nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[i];
    if (e->state == NR_CORESET_REMOVED || e->passes || !e->grants || !e->probation) continue; /* only same-nID/same-RB twins are retirable: a different CORESET of the cell is never rejected for its PDSCH yield */
    double ref = 0.0; /* strongest competitor's lower pass rate */
    for (int j = 0; j < n; ++j)
      if (j != i && g_coreset_bank[j].state != NR_CORESET_REMOVED && lo[j] > ref) ref = lo[j];
    if (!nr_pdcch_cert_zero_pass_significant(e->grants, ref, NR_PDCCH_CERT_ALPHA)) continue;
    LOG_A(PHY, "SENSING: CORESET bank RETIRE index=%d nid=%d offset=%d span=%d symbol=%d: %u grants, 0 TB CRC passes; competitor pass rate >= %.4f (P[no pass] < %.0e)\n",
          i, e->cfg.coreset_pdcch_dmrs_scrambling_id, e->cfg.coreset_rb_offset, e->cfg.coreset_freq_domain * 6, e->cfg.ss_first_symbol,
          e->grants, ref, NR_PDCCH_CERT_ALPHA);
    /* backoff: the time the evidence took, doubled per repeat of the same footprint */
    const int rb = e->cfg.bwp_start + e->cfg.coreset_rb_offset, groups = e->cfg.coreset_freq_domain, nid = e->cfg.coreset_pdcch_dmrs_scrambling_id;
    retired_t *r = NULL;
    for (int k = 0; k < NR_RETIRED_MAX && !r; ++k)
      if (g_retired[k].used && g_retired[k].nid == nid && g_retired[k].rb == rb && g_retired[k].groups == groups) r = &g_retired[k];
    if (!r) { r = &g_retired[g_retired_w++ % NR_RETIRED_MAX]; *r = (retired_t){.used = true, .nid = nid, .rb = rb, .groups = groups}; }
    const uint64_t span = slot > e->born_slot ? slot - e->born_slot : 1;
    r->span = r->span > span ? r->span : span;
    if (r->n < 40) ++r->n;
    r->until = slot + (r->span << (r->n - 1 < 20 ? r->n - 1 : 20));
    e->state = NR_CORESET_REMOVED;
    g_compaction_pending = true;
    ++retired;
  }
  return retired;
}

int nr_pdcch_coreset_bank_assess(uint64_t slot)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (slot > g_bank_last_slot) g_bank_last_slot = slot;
  const int r = assess_locked(slot);
  compact_removed();
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return r;
}

void nr_pdcch_coreset_bank_note_dci(int index, uint64_t slot, uint16_t rnti, uint32_t payload_hash)
{
  if (!nr_cfg_epoch_work_current()) return;
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (!nr_cfg_epoch_work_current()) { pthread_mutex_unlock(&g_coreset_bank_lock); return; }
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (index >= 0 && index < n && rnti && g_coreset_bank[index].state == NR_CORESET_STALE) {
    nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[index];
    if (!e->stale_proof_rnti) {
      e->stale_proof_slot = slot;
      e->stale_proof_rnti = rnti;
      e->stale_proof_hash = payload_hash;
    } else if (slot > e->stale_proof_slot && rnti == e->stale_proof_rnti
               && payload_hash != e->stale_proof_hash) {
      e->state = NR_CORESET_VERIFIED;
      e->verified_epoch = nr_cfg_reconf_enabled() ? nr_cfg_epoch_work_stamp() : 0;
      e->stale_since_slot = 0;
      e->stale_proof_slot = 0;
      LOG_A(PHY, "SENSING: CORESET bank VERIFIED index=%d slot=%llu\n", index, (unsigned long long)slot);
    }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

void nr_pdcch_coreset_bank_tick(uint64_t slot, bool traffic_elsewhere,
                                uint32_t t_stale_slots, uint32_t t_remove_slots)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (slot > g_bank_last_slot) g_bank_last_slot = slot;
  assess_locked(slot);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  bool any_accept = traffic_elsewhere;
  for (int i = 0; i < n; ++i)
    any_accept |= g_coreset_bank[i].accepts_window != 0;
  for (int i = 0; i < n; ++i) {
    nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[i];
    const bool elsewhere = traffic_elsewhere || (any_accept && !e->accepts_window);
    if (nr_cfg_reconf_enabled() && elsewhere && e->state == NR_CORESET_STALE && e->stale_since_slot == UINT64_MAX)
      e->stale_since_slot = slot;
    if (!e->last_accept_slot)
      e->last_accept_slot = slot;
    if (e->state == NR_CORESET_VERIFIED && elsewhere && slot >= e->last_accept_slot
        && slot - e->last_accept_slot >= t_stale_slots) {
      e->state = NR_CORESET_STALE;
      e->stale_since_slot = slot;
      e->stale_proof_slot = 0;
      LOG_A(PHY, "SENSING: CORESET bank STALE index=%d slot=%llu\n", i, (unsigned long long)slot);
    } else if (e->state == NR_CORESET_STALE && slot >= e->stale_since_slot
               && slot - e->stale_since_slot >= t_remove_slots) {
      e->state = NR_CORESET_REMOVED;
      g_compaction_pending = true;
    }
    e->accepts_window = 0;
  }
  compact_removed();
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

int nr_pdcch_coreset_bank_count(void)
{
  if (nr_cfg_reconf_enabled() && (g_identity_reset_pending || g_bank_identity != nr_cfg_epoch_identity_gen())) return 0;
  return atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
}

const nr_pdcch_blind_monitor_cfg_t *nr_pdcch_coreset_bank_cfg(int index)
{
  return &g_coreset_bank[index].cfg;
}

nr_pdcch_discovered_coreset_t *nr_pdcch_coreset_bank_entry(int index)
{
  return &g_coreset_bank[index];
}

nr_coreset_state_t nr_pdcch_coreset_bank_state(int index)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  const nr_coreset_state_t state = index >= 0 && index < n
      ? g_coreset_bank[index].state : NR_CORESET_REMOVED;
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return state;
}

/* Is this geometry already a verified bank entry? (stage 1-2 hand-off: a discovered CORESET the walk found
 * first must not be re-dwelled -- run s3live5 re-tested it, the alias rule retired it as "not verified",
 * and the walk resumed instead of pausing.) */
static bool bank_footprint(int rb_offset, int groups, int duration, int symbol,
                           int bundle, int interleaver, int shift, int nid, bool verified)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    if (g_coreset_bank[i].state == NR_CORESET_REMOVED
        || (verified && g_coreset_bank[i].state != NR_CORESET_VERIFIED))
      continue;
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    if ((int)(b->bwp_start + b->coreset_rb_offset) == rb_offset && (int)b->coreset_freq_domain == groups
        && (symbol < 0 || b->ss_first_symbol == symbol)
        && (int)b->coreset_duration == duration && (int)b->coreset_reg_bundle_size == bundle
        && (bundle == 0 || ((int)b->coreset_interleaver_size == interleaver && (int)b->coreset_shift_index == shift))
        && (int)b->coreset_pdcch_dmrs_scrambling_id == nid)
      { pthread_mutex_unlock(&g_coreset_bank_lock); return true; }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return false;
}

/* nr_pdcch_coreset_bank_has_footprint: true when the bank holds a CORESET with this footprint (RB range, groups, duration, symbol, bundle, interleaver, shift and scrambling ID), verified or not. Caller: nr_pdcch_discovered_snapshot() and nr_pdcch_blind_monitor_bank_has_geometry(). */
bool nr_pdcch_coreset_bank_has_footprint(int rb,int groups,int dur,int sym,int L,int R,int shift,int nid)
{ return bank_footprint(rb,groups,dur,sym,L,R,shift,nid,false); }
/* nr_pdcch_coreset_bank_verified_footprint: true when the bank holds a verified CORESET with this footprint (same arguments as nr_pdcch_coreset_bank_has_footprint()). Caller: the tests and the discovery path. */
bool nr_pdcch_coreset_bank_verified_footprint(int rb,int groups,int dur,int sym,int L,int R,int shift,int nid)
{ return bank_footprint(rb,groups,dur,sym,L,R,shift,nid,true); }

/* nr_pdcch_blind_monitor_bank_has_geometry: true when the bank holds a CORESET with this geometry at any symbol (sym = -1). Caller: discovered_poll() in nr_pdcch_blind_monitor.c (already-verified entries are skipped without a dwell). */
bool nr_pdcch_blind_monitor_bank_has_geometry(int rb, int groups, int duration, int bundle, int interleaver,
                                               int shift, int nid)
{
  return nr_pdcch_coreset_bank_has_footprint(rb, groups, duration, -1, bundle, interleaver, shift, nid);
}

/* coreset_same_geometry: true when two monitor configurations describe the same CORESET: same BWP, footprint, first symbol and scrambling ID, and the same length override (ignored for discovered CORESETs).
 *   Pure function. Caller: nr_pdcch_coreset_bank_add().
 */
static bool coreset_same_geometry(const nr_pdcch_blind_monitor_cfg_t *a,
                                  const nr_pdcch_blind_monitor_cfg_t *b)
{
  return a->bwp_start == b->bwp_start && a->bwp_size == b->bwp_size
      && a->coreset_rb_offset == b->coreset_rb_offset
      && a->coreset_freq_domain == b->coreset_freq_domain
      && a->coreset_duration == b->coreset_duration
      && a->coreset_reg_bundle_size == b->coreset_reg_bundle_size
      && a->coreset_interleaver_size == b->coreset_interleaver_size
      && a->coreset_shift_index == b->coreset_shift_index
      && a->coreset_pdcch_dmrs_scrambling_id == b->coreset_pdcch_dmrs_scrambling_id
      && a->ss_first_symbol == b->ss_first_symbol
      && (b->discovered_coreset || a->dci_length_override == b->dci_length_override);
}

/* A banked operational scan may deliberately cover a wider RB interval than the observed
 * footprint. It covers any later hypothesis inside that interval when symbol, duration, DM-RS ID
 * and CCE-to-REG mapping agree. Skipping such hypotheses prevents discovery from repeatedly
 * rediscovering its first archived CORESET while still allowing a different mapping in the same
 * RBs to become a separate bank entry. */
bool nr_pdcch_coreset_bank_covers(int rb_offset, int span_rb, int duration, int symbol,
                                  int bundle, int interleaver, int shift, int dmrs_id)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    if (g_coreset_bank[i].state == NR_CORESET_REMOVED)
      continue;
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    const int bank_span = b->coreset_freq_domain * 6;
    if (b->coreset_rb_offset <= rb_offset
        && b->coreset_rb_offset + bank_span >= rb_offset + span_rb
        && b->coreset_duration == duration && b->ss_first_symbol == symbol
        && b->coreset_reg_bundle_size == bundle
        && b->coreset_interleaver_size == interleaver
        && b->coreset_shift_index == shift
        && b->coreset_pdcch_dmrs_scrambling_id == dmrs_id)
      { pthread_mutex_unlock(&g_coreset_bank_lock); return true; }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return false;
}

bool nr_pdcch_coreset_bank_occupancy_outside(int rb_offset, int symbol)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  bool outside = n > 0;
  for (int i = 0; i < n; ++i) {
    if (g_coreset_bank[i].state == NR_CORESET_REMOVED)
      continue;
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    const int first = b->bwp_start + b->coreset_rb_offset;
    if (rb_offset >= first && rb_offset + 6 <= first + b->coreset_freq_domain * 6
        && symbol >= b->ss_first_symbol && symbol < b->ss_first_symbol + b->coreset_duration) {
      outside = false;
      break;
    }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return outside;
}

bool nr_pdcch_coreset_bank_has_owner(uint16_t rnti)
{
  if (!rnti)
    return false;
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i)
    if (g_coreset_bank[i].state != NR_CORESET_REMOVED)
    for (int j = 0; j < g_coreset_bank[i].nowners; ++j)
      if (g_coreset_bank[i].owners[j] == rnti) {
        pthread_mutex_unlock(&g_coreset_bank_lock);
        return true;
      }
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return false;
}

/* A verified dedicated DCI length is a high-value cell prior for another CORESET, but not ground
 * truth: different UEs may have different BWPs/configurations. The fast catalog tests the modal
 * bank length for a bounded eight rounds; the exhaustive lap still tries every legal length. */
int nr_pdcch_coreset_bank_length_hint(void)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  int best = 0, best_count = 0;
  for (int i = 0; i < n; ++i) {
    if (g_coreset_bank[i].state == NR_CORESET_REMOVED)
      continue;
    const int len = g_coreset_bank[i].cfg.dci_length_override;
    if (len <= 0)
      continue;
    int count = 0;
    for (int j = 0; j < n; ++j)
      if (g_coreset_bank[j].state != NR_CORESET_REMOVED && g_coreset_bank[j].cfg.dci_length_override == len)
        ++count;
    if (count > best_count) {
      best = len;
      best_count = count;
    }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return best;
}

int nr_pdcch_candidate_res(const nr_pdcch_blind_monitor_cfg_t *c, int cce, int al, uint32_t *out, int max)
{
  const int span = c->coreset_freq_domain * 6, dur = c->coreset_duration;
  const int Lin = c->coreset_reg_bundle_size, R = c->coreset_interleaver_size;
  if (span < 6 || dur < 1 || dur > 3 || al < 1 || cce < 0 || max < 6 * al) return -1;
  const int N_regs = span * dur, Lb = Lin ? Lin : 6;
  if (Lin && (R < 1 || N_regs % (R * Lin))) return -1;
  if (Lb % dur) return -1;
  const int B_rb = Lb / dur, nbpc = 6 / Lb, n_cce = N_regs / 6, nb_tot = n_cce * nbpc;
  if (cce + al > n_cce) return -1;
  const int C = Lin ? N_regs / (R * Lin) : 0;
  int f[nb_tot > 0 ? nb_tot : 1];
  int cc = 0, r = 0;
  for (int nb = 0; nb < nb_tot; nb++) {
    if (!Lin) f[nb] = nb;
    else {
      if (r == R) { r = 0; cc++; }
      f[nb] = ((r * C) + cc + c->coreset_shift_index) % (N_regs / Lb);
      r++;
    }
  }
  const int first = cce * nbpc, n = al * nbpc;
  int ord[96];
  if (n > 96) return -1;
  for (int i = 0; i < n; i++) {
    int v = f[first + i], j = i;
    while (j > 0 && ord[j - 1] > v) { ord[j] = ord[j - 1]; j--; }
    ord[j] = v;
  }
  int k = 0;
  for (int sy = 0; sy < dur; sy++)
    for (int i = 0; i < n; i++)
      for (int b = 0; b < B_rb; b++)
        out[k++] = ((uint32_t)(c->bwp_start + c->coreset_rb_offset + ord[i] * B_rb + b) << 4) | (uint32_t)(c->ss_first_symbol + sy);
  return k;
}

static bool explains_locked(const nr_pdcch_blind_monitor_cfg_t *c, int cce, int al, int below)
{
  uint32_t a[96], b[96];
  if (nr_pdcch_candidate_res(c, cce, al, a, 96) < 0) return false;
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  for (int i = 0; i < n && (below < 0 || i < below); ++i) {
    const nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[i];
    if (e->state == NR_CORESET_REMOVED || e->cfg.coreset_pdcch_dmrs_scrambling_id != c->coreset_pdcch_dmrs_scrambling_id
        || coreset_same_geometry(&e->cfg, c))
      continue;
    const int n_cce = e->cfg.coreset_freq_domain * e->cfg.coreset_duration;
    for (int x = 0; x + al <= n_cce; ++x)
      if (nr_pdcch_candidate_res(&e->cfg, x, al, b, 96) == 6 * al && !memcmp(a, b, sizeof(uint32_t) * 6 * al))
        return true;
    /* AL1 union: the entry also decodes AL1 candidates of alternative mapping families (REG sets, see nr_pdcch_al1_union). Same identity rule: same ordered REs.
     * The union is written under nr_pdcch_al1_mu, so it is read under it (briefly, AL1 candidates only). */
    if (al != 1) continue;
    pthread_mutex_lock(&nr_pdcch_al1_mu);
    const int nu = (int)e->n_al1_union, dur = e->cfg.coreset_duration;
    for (int j = 0; j < nu && j < NR_PDCCH_AL1_UNION_MAX; ++j) {
      uint32_t u[6]; int k = 0, lastrb = -1;
      const int base = e->cfg.bwp_start + e->cfg.coreset_rb_offset;
      for (int sy = 0; sy < dur; ++sy) {
        lastrb = -1;
        for (int q = 0; q < 6; ++q) {
          const int rb = e->al1_union[j][q] / dur;
          if (rb == lastrb) continue;
          lastrb = rb;
          if (k < 6) u[k] = ((uint32_t)(base + rb) << 4) | (uint32_t)(e->cfg.ss_first_symbol + sy);
          ++k;
        }
      }
      if (k == 6 && !memcmp(a, u, sizeof(u))) { pthread_mutex_unlock(&nr_pdcch_al1_mu); return true; }
    }
    pthread_mutex_unlock(&nr_pdcch_al1_mu);
  }
  return false;
}

bool nr_pdcch_coreset_bank_explains(const nr_pdcch_blind_monitor_cfg_t *c, int cce, int al, int below)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  const bool r = explains_locked(c, cce, al, below);
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return r;
}

void nr_pdcch_coreset_bank_note_alias(int index, uint64_t slot, int cce, int al)
{
  if (index < 1 || al < 1) return;
  pthread_mutex_lock(&g_coreset_bank_lock);
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
  if (index < n && g_coreset_bank[index].state != NR_CORESET_REMOVED) {
    nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[index];
    if (explains_locked(&e->cfg, cce, al, index)) {
      if (slot != e->alias_slot) { e->alias_slot = slot; ++e->alias_hits; }
    } else
      ++e->own_hits;
    if (e->own_hits == 0 && e->alias_hits >= 2) {
      LOG_A(PHY, "SENSING: multi-CORESET bank evict index=%d offset=%d span=%d mapping=%d/%d/%d: all %u accepts re-expressible by an older entry\n",
            index, e->cfg.coreset_rb_offset, e->cfg.coreset_freq_domain * 6, e->cfg.coreset_reg_bundle_size,
            e->cfg.coreset_interleaver_size, e->cfg.coreset_shift_index, e->alias_hits);
      e->state = NR_CORESET_REMOVED;
      g_compaction_pending = true;
      compact_removed();
    }
  }
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

/* nr_pdcch_coreset_bank_add: adds a verified CORESET (monitor configuration cfg, with the owner RNTI) to the bank: reuses an existing entry with the same geometry (recording the owner), or appends a new entry while the bank has room.
 *   Refuses requests outside the current configuration epoch, an unset length override, or an alias of an existing owner; a full bank is logged and refused. Returns the entry index, or -1. Callers: nr_pdcch_discovered_observe() and the tests.
 */
int nr_pdcch_coreset_bank_add(const nr_pdcch_blind_monitor_cfg_t *cfg, uint16_t owner)
{
  if (!nr_cfg_epoch_work_current()) return -1;
  if (nr_cfg_reconf_enabled()) nr_cfg_epoch_subscribe(bank_epoch_listener);
  if (cfg == NULL || cfg->dci_length_override <= 0)
    return -1;
  pthread_mutex_lock(&g_coreset_bank_lock);
  if (!nr_cfg_epoch_work_current() || g_identity_reset_pending) { pthread_mutex_unlock(&g_coreset_bank_lock); return -1; }
  int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  if (nr_cfg_reconf_enabled() && n == 0) {
    g_bank_epoch = nr_cfg_epoch_current();
    g_bank_identity = nr_cfg_epoch_identity_gen();
  }
  int at = -1;
  for (int i = 0; i < n; ++i)
    if (g_coreset_bank[i].state != NR_CORESET_REMOVED && coreset_same_geometry(&g_coreset_bank[i].cfg, cfg)) { at = i; break; }
  if (at < 0) {
    /* B1/B2: a footprint retired for lack of downstream CRC evidence waits out its backoff. */
    if (retired_match_locked(cfg->coreset_pdcch_dmrs_scrambling_id, cfg->bwp_start + cfg->coreset_rb_offset, cfg->coreset_freq_domain)) {
      LOG_A(PHY, "SENSING: multi-CORESET bank add deferred: nid=%d offset=%d span=%d symbol=%d was retired (no TB CRC evidence), backing off\n",
            cfg->coreset_pdcch_dmrs_scrambling_id, cfg->coreset_rb_offset, cfg->coreset_freq_domain * 6, cfg->ss_first_symbol);
      pthread_mutex_unlock(&g_coreset_bank_lock);
      return -1;
    }
    /* A narrow CCE-compatible subset can decode the same UE as its already banked CORESET. It is
     * useful evidence but not a second independent configuration; retaining every such alias can
     * fill the bounded bank before another UE is reached. */
    for (int i = 0; owner && !cfg->discovered_coreset && i < n; ++i)
      if (g_coreset_bank[i].state != NR_CORESET_REMOVED)
        for (int j = 0; j < g_coreset_bank[i].nowners; ++j)
          if (g_coreset_bank[i].owners[j] == owner) {
            pthread_mutex_unlock(&g_coreset_bank_lock);
            return -1;
          }
    if (n >= NR_PDCCH_DISCOVERED_CORESETS) {
      LOG_W(PHY, "SENSING: multi-CORESET bank full (%d); verified geometry left unarchived\n", n);
      pthread_mutex_unlock(&g_coreset_bank_lock);
      return -1;
    }
    at = n;
    pthread_mutex_lock(&nr_pdcch_al1_mu);
    memset(&g_coreset_bank[at], 0, sizeof(g_coreset_bank[at]));
    pthread_mutex_unlock(&nr_pdcch_al1_mu);
    g_coreset_bank[at].cfg = *cfg;
    g_coreset_bank[at].verified_epoch = nr_cfg_reconf_enabled() ? nr_cfg_epoch_work_stamp() : 0;
    g_coreset_bank[at].cfg.autodiscover = 0;
    g_coreset_bank[at].cfg.ss_monitoring_slot_periodicity = 1;
    g_coreset_bank[at].cfg.ss_monitoring_slot_offset = 0;
    g_coreset_bank[at].cfg.ss_duration = 1;
    if (++g_serial_next == 0) g_serial_next = 1;
    g_coreset_bank[at].serial = g_serial_next;
    g_coreset_bank[at].born_slot = g_bank_last_slot;
    for (int i = 0; i < n; ++i) { /* B1: same nID and RB footprint as a live entry (other symbol/mapping): its own TB CRC evidence is required */
      const nr_pdcch_blind_monitor_cfg_t *o = &g_coreset_bank[i].cfg;
      if (g_coreset_bank[i].state != NR_CORESET_REMOVED && o->coreset_pdcch_dmrs_scrambling_id == cfg->coreset_pdcch_dmrs_scrambling_id
          && o->bwp_start + o->coreset_rb_offset == cfg->bwp_start + cfg->coreset_rb_offset && o->coreset_freq_domain == cfg->coreset_freq_domain)
        g_coreset_bank[at].probation = 1;
    }
    atomic_store_explicit(&g_coreset_bank_n, n + 1, memory_order_release);
    LOG_A(PHY, "SENSING: multi-CORESET bank add index=%d offset=%d span=%d symbol=%d mapping=%d/%d/%d len=%d\n",
          at, cfg->coreset_rb_offset, cfg->coreset_freq_domain * 6, cfg->ss_first_symbol,
          cfg->coreset_reg_bundle_size, cfg->coreset_interleaver_size, cfg->coreset_shift_index,
          cfg->dci_length_override);
  }
  nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[at];
  for (int i = 0; i < e->nowners; ++i)
    if (e->owners[i] == owner) { pthread_mutex_unlock(&g_coreset_bank_lock); return at; }
  if (owner && e->nowners < NR_PDCCH_BLIND_MAX_UE)
  { e->owners[e->nowners] = owner; __atomic_store_n(&e->nowners, (uint8_t)(e->nowners + 1), __ATOMIC_RELAXED); }
  pthread_mutex_unlock(&g_coreset_bank_lock);
  return at;
}

bool nr_pdcch_coreset_bank_occupancy_sample(uint8_t *history, bool hit)
{
  *history = (uint8_t)((*history << 1) | hit);
  /* A single candidate or several windows in one sample are not traffic evidence. */
  return hit && __builtin_popcount((unsigned)*history) >= 3;
}

static void bank_epoch_locked(const nr_cfg_epoch_snapshot_t *s)
{
  if (s->epoch <= g_bank_epoch) return;
  const bool reverify = nr_cfg_epoch_reverifies(s, g_bank_epoch);
  g_bank_epoch = s->epoch;
  if (s->identity_gen != g_bank_identity) {
    if (!g_identity_reset_pending) g_dormant_bank.identity_gen = g_bank_identity;
    g_bank_identity = s->identity_gen;
    g_identity_reset_pending = g_compaction_pending = true;
    memset(g_retired, 0, sizeof(g_retired)); /* a new cell re-certifies from scratch */
    memset(g_rnti_src, 0, sizeof(g_rnti_src));
    /* The RT remove hook only clears length banks and all TD contexts. Both
     * already isolate by identity_gen; invoking it here would destroy dormant
     * old-cell records (and potentially freshly opened new-cell contexts). */
    compact_removed();
  } else if (reverify) {
    const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
      nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[i];
      if (e->state == NR_CORESET_REMOVED) continue;
      e->state = NR_CORESET_STALE;
      e->stale_since_slot = UINT64_MAX; /* start only once traffic elsewhere supplies absence evidence */
      e->stale_proof_rnti = 0;
      e->stale_proof_slot = 0;
      e->accepts_window = 0;
      e->grants = e->passes = 0; /* re-certify after a reconfiguration */
      e->born_slot = g_bank_last_slot;
    }
    memset(g_retired, 0, sizeof(g_retired)); /* a CORESET legitimately new after the reconfiguration is not locked out */
    memset(g_rnti_src, 0, sizeof(g_rnti_src));
  }
}
static void bank_epoch_listener(const nr_cfg_epoch_snapshot_t *s)
{
  pthread_mutex_lock(&g_coreset_bank_lock);
  bank_epoch_locked(s);
  pthread_mutex_unlock(&g_coreset_bank_lock);
}

/* nr_pdcch_coreset_bank_cell_prealloc: allocates this file's heap objects of `cell` (call before nr_cell_alloc_fence()). */
void nr_pdcch_coreset_bank_cell_prealloc(int cell)
{
  NR_CELL_HEAP_PREALLOC(g_coreset_bank_cell_t, g_coreset_bank, cell);
  NR_CELL_HEAP_PREALLOC(g_dormant_bank_cell_t, g_dormant_bank, cell);
}
