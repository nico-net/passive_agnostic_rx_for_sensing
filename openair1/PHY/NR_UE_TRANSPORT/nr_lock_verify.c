/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
#include "nr_lock_verify.h"

void nr_ssb_reject_init(nr_ssb_reject_t *r)
{
  *r = (nr_ssb_reject_t){.mask = 0, .pci = -1, .misses = 0};
}

void nr_ssb_reject_add(nr_ssb_reject_t *r, int pci, int ssb_index)
{
  if (ssb_index < 0 || ssb_index > 63)
    return;
  if (r->pci != pci) { /* one cell slot serves one PCI at a time: an older PCI's beams are irrelevant */
    r->mask = 0;
    r->pci = pci;
  }
  r->mask |= 1ull << ssb_index;
  r->misses = 0;
}

bool nr_ssb_reject_skip(const nr_ssb_reject_t *r, int pci, int ssb_index)
{
  return r->pci == pci && ssb_index >= 0 && ssb_index <= 63 && (r->mask >> ssb_index) & 1;
}

void nr_ssb_reject_search_done(nr_ssb_reject_t *r, bool locked, int max_misses)
{
  if (!r->mask)
    return;
  if (locked)
    r->misses = 0;
  else if (++r->misses >= max_misses) /* only rejected beams are decodable: let them be tried again */
    nr_ssb_reject_init(r);
}

nr_lock_verdict_t nr_lock_verify_window(nr_lock_verify_t *v, bool synced, unsigned long pbch_ok, unsigned long pbch_fail,
                                        int ssb_index, int pci, int windows)
{
  if (!synced) {
    v->was_sync = false;
    v->verified = false;
    v->unverified = 0;
    return NR_LOCK_KEEP;
  }
  if (!v->was_sync) { /* a new lock */
    v->was_sync = true;
    v->verified = false;
    v->unverified = 0;
    v->lock_ssb = ssb_index;
    v->lock_pci = pci;
  }
  if (v->verified)
    return NR_LOCK_KEEP;
  if (pbch_ok > 0) {
    v->verified = true;
    v->unverified = 0;
    return NR_LOCK_VERIFIED;
  }
  if (pbch_fail == 0) /* no SSB occasion in this window */
    return NR_LOCK_KEEP;
  return ++v->unverified >= windows ? NR_LOCK_REJECT : NR_LOCK_KEEP;
}
