/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * PDCCH fast lane: the tasks whose DCI is a DL grant for an RNTI we already know (UE-specific search space, exact RNTI), and every DL
 * task of a common-search-space occasion, are decoded and run through Phase 2 FIRST; their PDSCH jobs are pushed to the consumers at once.
 * Everything else of the occasion (open-RNTI discovery, DCI 1_0 in a dedicated space, other BWPs, UL sweeps, lookahead lanes, BWP
 * probes) runs after, on the same thread, in its original relative order.
 *
 * Nothing is dropped and nothing is skipped: the occasion still runs the same tasks with the same inputs. Only the interleaving of two
 * classes changes. Why that is state-neutral is argued in docs/superpowers/reports/csd/PDCCH-REDESIGN-20261019.md ("Implemented") and
 * checked, on a model of the Phase-2 state, by tests/nr_pdcch_fast_lane_test.cc.
 *
 * General: the predicate reads task attributes only. No per-cell parameter, no search-space or CORESET assumption.
 */
#ifndef NR_PDCCH_FAST_LANE_H
#define NR_PDCCH_FAST_LANE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool ul_scan, is_lookahead, bwp_probe, open_rnti, fmt10, dl_auto;
  int8_t bwp_entry;
  uint16_t rnti_min, rnti_max;
} nr_fl_task_t;

static inline bool nr_fl_is_fast(const nr_fl_task_t *t, bool css_occasion)
{
  if (t->ul_scan || t->is_lookahead || t->bwp_probe || t->bwp_entry > 0) return false;
  if (css_occasion) return true;
  return t->dl_auto && !t->open_rnti && !t->fmt10 && t->rnti_min != 0 && t->rnti_min == t->rnti_max;
}

/* Stable partition. ord[0..returned) = fast tasks in task order, then the rest in task order. */
static inline int nr_fl_partition(const nr_fl_task_t *t, int n, bool css_occasion, int *ord)
{
  int nf = 0;
  for (int i = 0; i < n; i++)
    if (nr_fl_is_fast(&t[i], css_occasion)) ord[nf++] = i;
  int k = nf;
  for (int i = 0; i < n; i++)
    if (!nr_fl_is_fast(&t[i], css_occasion)) ord[k++] = i;
  return nf;
}
#endif
