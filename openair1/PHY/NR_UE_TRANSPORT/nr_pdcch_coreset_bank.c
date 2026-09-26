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

#include "common/utils/LOG/log.h"

/* Verified dedicated geometries remain operational while discovery continues for another UE's
 * CORESET. Entries are immutable after the release-store publishes them, so the receive producer
 * may test the count while the single scan consumer appends without a lock. */
#define NR_PDCCH_DISCOVERED_CORESETS 8
static nr_pdcch_discovered_coreset_t g_coreset_bank[NR_PDCCH_DISCOVERED_CORESETS];
static _Atomic int g_coreset_bank_n;

int nr_pdcch_coreset_bank_count(void)
{
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

/* Is this geometry already a verified bank entry? (stage 1-2 hand-off: a discovered CORESET the walk found
 * first must not be re-dwelled -- run s3live5 re-tested it, the alias rule retired it as "not verified",
 * and the walk resumed instead of pausing.) */
bool nr_pdcch_blind_monitor_bank_has_geometry(int rb_offset, int groups, int duration, int bundle, int interleaver,
                                               int shift, int nid)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    if ((int)(b->bwp_start + b->coreset_rb_offset) == rb_offset && (int)b->coreset_freq_domain == groups
        && (int)b->coreset_duration == duration && (int)b->coreset_reg_bundle_size == bundle
        && (bundle == 0 || ((int)b->coreset_interleaver_size == interleaver && (int)b->coreset_shift_index == shift))
        && (int)b->coreset_pdcch_dmrs_scrambling_id == nid)
      return true;
  }
  return false;
}

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
      && a->dci_length_override == b->dci_length_override;
}

/* A banked operational scan may deliberately cover a wider RB interval than the observed
 * footprint. It covers any later hypothesis inside that interval when symbol, duration, DM-RS ID
 * and CCE-to-REG mapping agree. Skipping such hypotheses prevents discovery from repeatedly
 * rediscovering its first archived CORESET while still allowing a different mapping in the same
 * RBs to become a separate bank entry. */
bool nr_pdcch_coreset_bank_covers(int rb_offset, int span_rb, int duration, int symbol,
                                  int bundle, int interleaver, int shift, int dmrs_id)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    const nr_pdcch_blind_monitor_cfg_t *b = &g_coreset_bank[i].cfg;
    const int bank_span = b->coreset_freq_domain * 6;
    if (b->coreset_rb_offset <= rb_offset
        && b->coreset_rb_offset + bank_span >= rb_offset + span_rb
        && b->coreset_duration == duration && b->ss_first_symbol == symbol
        && b->coreset_reg_bundle_size == bundle
        && b->coreset_interleaver_size == interleaver
        && b->coreset_shift_index == shift
        && b->coreset_pdcch_dmrs_scrambling_id == dmrs_id)
      return true;
  }
  return false;
}

bool nr_pdcch_coreset_bank_has_owner(uint16_t rnti)
{
  if (!rnti)
    return false;
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  for (int i = 0; i < n; ++i)
    for (int j = 0; j < g_coreset_bank[i].nowners; ++j)
      if (g_coreset_bank[i].owners[j] == rnti)
        return true;
  return false;
}

/* A verified dedicated DCI length is a high-value cell prior for another CORESET, but not ground
 * truth: different UEs may have different BWPs/configurations. The fast catalog tests the modal
 * bank length for a bounded eight rounds; the exhaustive lap still tries every legal length. */
int nr_pdcch_coreset_bank_length_hint(void)
{
  const int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  int best = 0, best_count = 0;
  for (int i = 0; i < n; ++i) {
    const int len = g_coreset_bank[i].cfg.dci_length_override;
    if (len <= 0)
      continue;
    int count = 0;
    for (int j = 0; j < n; ++j)
      if (g_coreset_bank[j].cfg.dci_length_override == len)
        ++count;
    if (count > best_count) {
      best = len;
      best_count = count;
    }
  }
  return best;
}

int nr_pdcch_coreset_bank_add(const nr_pdcch_blind_monitor_cfg_t *cfg, uint16_t owner)
{
  if (cfg == NULL || cfg->dci_length_override <= 0)
    return -1;
  int n = atomic_load_explicit(&g_coreset_bank_n, memory_order_acquire);
  int at = -1;
  for (int i = 0; i < n; ++i)
    if (coreset_same_geometry(&g_coreset_bank[i].cfg, cfg)) { at = i; break; }
  if (at < 0) {
    /* A narrow CCE-compatible subset can decode the same UE as its already banked CORESET. It is
     * useful evidence but not a second independent configuration; retaining every such alias can
     * fill the bounded bank before another UE is reached. */
    if (nr_pdcch_coreset_bank_has_owner(owner))
      return -1;
    if (n >= NR_PDCCH_DISCOVERED_CORESETS) {
      LOG_W(PHY, "SENSING: multi-CORESET bank full (%d); verified geometry left unarchived\n", n);
      return -1;
    }
    at = n;
    memset(&g_coreset_bank[at], 0, sizeof(g_coreset_bank[at]));
    g_coreset_bank[at].cfg = *cfg;
    g_coreset_bank[at].cfg.autodiscover = 0;
    g_coreset_bank[at].cfg.ss_monitoring_slot_periodicity = 1;
    g_coreset_bank[at].cfg.ss_monitoring_slot_offset = 0;
    g_coreset_bank[at].cfg.ss_duration = 1;
    atomic_store_explicit(&g_coreset_bank_n, n + 1, memory_order_release);
    LOG_A(PHY, "SENSING: multi-CORESET bank add index=%d offset=%d span=%d symbol=%d mapping=%d/%d/%d len=%d\n",
          at, cfg->coreset_rb_offset, cfg->coreset_freq_domain * 6, cfg->ss_first_symbol,
          cfg->coreset_reg_bundle_size, cfg->coreset_interleaver_size, cfg->coreset_shift_index,
          cfg->dci_length_override);
  }
  nr_pdcch_discovered_coreset_t *e = &g_coreset_bank[at];
  for (int i = 0; i < e->nowners; ++i)
    if (e->owners[i] == owner) return at;
  if (owner && e->nowners < NR_PDCCH_BLIND_MAX_UE)
    e->owners[e->nowners++] = owner;
  return at;
}
