/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_observer.h"
#include <string.h>

static int same(const nr_csirs_resource_t *a, const nr_csirs_resource_t *b)
{
  return a->row == b->row && a->ports == b->ports && a->density == b->density
      && a->period == b->period && a->offset == b->offset && a->offset2 == b->offset2
      && a->n_offsets == b->n_offsets && a->zp == b->zp && a->freq_domain == b->freq_domain
      && a->start_rb == b->start_rb && a->nr_of_rbs == b->nr_of_rbs;
}

static nr_csirs_observer_entry_t *find(nr_csirs_observer_t *o, const nr_csirs_resource_t *r)
{
  for (int i = 0; i < NR_CSIRS_OBSERVER_MAX; ++i)
    if (o->entry[i].seen && same(&o->entry[i].resource, r)) return &o->entry[i];
  return NULL;
}
static int same_geometry(const nr_csirs_resource_t *a, const nr_csirs_resource_t *b)
{
  nr_csirs_resource_t x = *a, y = *b;
  x.period = y.period = x.offset = y.offset = x.offset2 = y.offset2 = 0;
  x.n_offsets = y.n_offsets = 0;
  return same(&x, &y);
}

void nr_csirs_observer_candidate(nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint64_t slot)
{
  __atomic_fetch_add(&o->csirs_candidates, 1, __ATOMIC_RELAXED);
  if (find(o, r)) return;
  for (int i = 0; i < NR_CSIRS_OBSERVER_MAX; ++i)
    if (!o->entry[i].seen) {
      o->entry[i] = (nr_csirs_observer_entry_t){.resource = *r, .first_slot = slot, .seen = 1};
      return;
    }
}

bool nr_csirs_observer_confirm(nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint64_t slot)
{
  nr_csirs_observer_entry_t *e = find(o, r);
  if (e && e->active) return false;
  bool had_map = false;
  for (int i = 0; i < NR_CSIRS_OBSERVER_MAX; ++i)
    if (o->entry[i].active && o->entry[i].resource.zp == r->zp) {
      had_map = true;
      if (same_geometry(&o->entry[i].resource, r)) {
        o->entry[i].active = 0; /* replacement must not later vanish a second time */
        __atomic_fetch_add(&o->csirs_revoked, 1, __ATOMIC_RELAXED);
        if (r->zp) __atomic_fetch_add(&o->zp_revoked, 1, __ATOMIC_RELAXED);
      }
    }
  if (!e) {
    uint64_t first = slot;
    for (int i = 0; i < NR_CSIRS_OBSERVER_MAX; ++i)
      if (o->entry[i].seen && same_geometry(&o->entry[i].resource, r) && o->entry[i].first_slot < first)
        first = o->entry[i].first_slot;
    for (int i = 0; i < NR_CSIRS_OBSERVER_MAX; ++i)
      if (!o->entry[i].seen) {
        e = &o->entry[i];
        *e = (nr_csirs_observer_entry_t){.resource = *r, .first_slot = first, .seen = 1};
        break;
      }
  }
  if (!e) return false;
  e->active = 1; e->misses = 0; e->last_due = (uint32_t)slot;
  __atomic_fetch_add(&o->csirs_confirmed, 1, __ATOMIC_RELAXED);
  const uint64_t elapsed = slot >= e->first_slot ? slot - e->first_slot : 0;
  __atomic_store_n(&o->csirs_time_to_confirm_slots_last, elapsed, __ATOMIC_RELAXED);
  __atomic_fetch_add(&o->csirs_time_to_confirm_slots_sum, elapsed, __ATOMIC_RELAXED);
  const uint32_t count = __atomic_load_n(&o->csirs_confirm_resource_count, __ATOMIC_RELAXED);
  if (count < NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX) {
    const uint64_t key = (uint64_t)r->zp | ((uint64_t)r->row << 1) | ((uint64_t)r->ports << 6)
        | ((uint64_t)r->density << 12) | ((uint64_t)r->period << 16)
        | ((uint64_t)r->offset << 26) | ((uint64_t)r->offset2 << 36)
        | ((uint64_t)r->freq_domain << 46);
    __atomic_store_n(&o->csirs_confirm_resource_key[count], key, __ATOMIC_RELAXED);
    __atomic_store_n(&o->csirs_confirm_resource_slots[count], elapsed, __ATOMIC_RELAXED);
    __atomic_store_n(&o->csirs_confirm_resource_count, count + 1, __ATOMIC_RELEASE);
  }
  return had_map;
}

bool nr_csirs_observer_due(nr_csirs_observer_t *o, const nr_csirs_resource_t *r, uint32_t slot, bool hit)
{
  nr_csirs_observer_entry_t *e = find(o, r);
  if (!e || !e->active || e->last_due == slot) return false;
  e->last_due = slot;
  if (hit) { e->misses = 0; return false; }
  if (++e->misses < NR_CSIRS_MISSING_PERIODS) return false;
  e->active = 0;
  __atomic_fetch_add(&o->csirs_revoked, 1, __ATOMIC_RELAXED);
  if (r->zp) __atomic_fetch_add(&o->zp_revoked, 1, __ATOMIC_RELAXED);
  return true;
}

void nr_csirs_observer_revoke_zp(nr_csirs_observer_t *o, const nr_csirs_resource_t *r)
{
  nr_csirs_observer_entry_t *e = find(o, r);
  if (!e)
    for (int i = 0; i < NR_CSIRS_OBSERVER_MAX; ++i)
      if (o->entry[i].active && o->entry[i].resource.zp && same_geometry(&o->entry[i].resource, r)) {
        e = &o->entry[i];
        break;
      }
  if (!e || !e->active) return;
  e->active = 0;
  __atomic_fetch_add(&o->csirs_revoked, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&o->zp_revoked, 1, __ATOMIC_RELAXED);
}
void nr_csirs_observer_export_zp(nr_csirs_observer_t *o) { __atomic_fetch_add(&o->zp_exported, 1, __ATOMIC_RELAXED); }
void nr_csirs_observer_add_time(nr_csirs_observer_t *o, nr_csirs_time_t kind, uint64_t us)
{
  uint64_t *p = kind == NR_CSIRS_TIME_SEARCH ? &o->csirs_search_us
      : kind == NR_CSIRS_TIME_IDSWEEP ? &o->csirs_idsweep_us
      : kind == NR_CSIRS_TIME_CONFIRM ? &o->csirs_confirm_us : &o->csirs_cfr_us;
  __atomic_fetch_add(p, us, __ATOMIC_RELAXED);
}
void nr_csirs_observer_metrics(const nr_csirs_observer_t *o, nr_passive_metrics_t *m)
{
#define COPY(field) m->field = __atomic_load_n(&o->field, __ATOMIC_RELAXED)
  COPY(csirs_candidates); COPY(csirs_confirmed); COPY(csirs_revoked); COPY(zp_exported); COPY(zp_revoked);
  COPY(csirs_search_us); COPY(csirs_idsweep_us); COPY(csirs_confirm_us); COPY(csirs_cfr_us);
  COPY(csirs_time_to_confirm_slots_last); COPY(csirs_time_to_confirm_slots_sum);
  m->csirs_confirm_resource_count = __atomic_load_n(&o->csirs_confirm_resource_count, __ATOMIC_ACQUIRE);
  for (uint32_t i = 0; i < m->csirs_confirm_resource_count && i < NR_PASSIVE_CSIRS_RESOURCE_METRICS_MAX; ++i) {
    m->csirs_confirm_resource_key[i] = __atomic_load_n(&o->csirs_confirm_resource_key[i], __ATOMIC_RELAXED);
    m->csirs_confirm_resource_slots[i] = __atomic_load_n(&o->csirs_confirm_resource_slots[i], __ATOMIC_RELAXED);
  }
#undef COPY
}
