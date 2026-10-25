/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_SM_PLAN_H
#define NR_GPU_SM_PLAN_H
/* Derived SM plan of the GPUSHARE partition (mpci-main plan B.6 / R6, Task B4). PURE: header-only, no CUDA include, no device access, so it
 * is unit-tested with fake devices; nr_gpu_green.h (G-owned) is wired to it in G2 and keeps the driver calls and the read-back.
 *
 * INPUT : SM count of the device, co-scheduling granularity (the smallest SM group the driver hands out; read back from the first
 *         cuDevSmResourceSplitByCount, 0/1 = unknown/none), whether green contexts exist (CUDA >= 12.5 and the driver entry points),
 *         the platform tier (nr_gpu_platform.h; NONE = no GPU).
 * OUTPUT: classes of SMs, never a per-cell carve-out (plan B.6 decision 1):
 *   disc  = round_up(max(1, SM / 6), gran)                      (the CURRENT rule of nr_gpu_green.h: 8 of 48 on the GB10)
 *   rt    = SM - disc                                           (must be > 0, else "no green")
 *   groups: all SMs are cut into groups of group_sz = round_up(group_req, gran) SMs (group_req = 4, the current request; raised only
 *           so that at most 15 full groups exist, the driver array bound of nr_gpu_green.h), plus a tail group of the remaining SMs.
 *           The first disc_groups = ceil(disc / group_sz) groups are the discovery groups, the next group is the PROBE group (and the
 *           first UL group), the rest is the DL context; the UL context spans the probe group and the DL groups (overlapping DL).
 *   split2 (DL / UL / probe contexts instead of one real-time context) iff
 *           (a) DL >= dl_instances x w, w = round_up(NR_SMP_SM_PER_DL_INSTANCE, gran) SMs, and
 *           (b) DL >= 60 % of rt.
 *     Justification. The current code splits when rt >= 32 (a literal). On the GB10 (gran 8) that is 4 DL pass instances (the DL pool
 *     is 4, plan B.6) x 8 SMs, the width of the smallest SM group the driver hands out there; the DL context is 32 of the 40
 *     real-time SMs. So the literal is re-expressed as "every DL instance can own at least one granule of 8 SMs (whole granules on
 *     wider-granule devices)" [INFERRED from the GB10 split, DlSmShare bench; the 8-SM width is the one calibrated point, a
 *     HYPOTHESIS elsewhere] and (b) keeps the probe group (one granule) from taking more than 40 % of the real-time SMs, as 8 of 40 =
 *     20 % does today. Below that, one real-time context and stream priority only, as today for rt < 32.
 *   GB10 (48 SMs, gran 8): disc 8, rt 40, split2: DL 32, UL 40 (overlapping), probe 8 -- identical to today.
 *   RTX 4060 Ti (34 SMs [INFERRED, public spec]), gran 2: disc 6, rt 28, groups of 4, DL would be 22 < 32 -> one real-time context.
 * Where the new rule differs from the literal (not on the GB10): a device with rt >= 32 whose DL part is < 4 widths (e.g. 42 SMs,
 * gran 8: DL 26) stays unsplit; a device with >= 16 full groups (132 SMs, gran 8) now splits (the old code rejected g == 16). */
#include "nr_gpu_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NR_SMP_DISC_DIV 6              /* discovery share: SM / 6 */
#define NR_SMP_GROUP_REQ 4             /* minCount of the all-SM split (current request) */
#define NR_SMP_MAX_FULL_GROUPS 15      /* nr_gpu_green.h rgrp[16]: g < 16 groups, plus the tail */
#define NR_SMP_SM_PER_DL_INSTANCE 8    /* width of one DL pass instance: the GB10 granule (see above) */
#define NR_SMP_DL_INSTANCES 4          /* DL pass pool (compile-time in the pass code) */
#define NR_SMP_DL_PCT_MIN 60           /* DL part >= 60 % of the real-time SMs */

typedef struct {
  int sm_count;     /* multiprocessor count of the device */
  int granularity;  /* smallest SM group the driver gives; < 1 = 1 */
  int green;        /* green contexts available (CUDA >= 12.5 and entry points) */
  int tier;         /* nr_gpu_platform_model_t: NR_GPU_NONE / NR_GPU_SHARED / NR_GPU_DISCRETE */
  int dl_instances; /* DL pass instances, 0 = NR_SMP_DL_INSTANCES */
} nr_gpu_sm_in_t;

typedef struct {
  int green;               /* 0 = no green contexts: every stream uses all SMs (one context, stream priority only) */
  const char *reason;      /* "ok", "no_gpu", "no_green", "no_sm", "no_rt_sms" */
  int sm, gran;
  int disc_sms, rt_sms;    /* classes; disc + rt == sm when green */
  int split2;              /* DL / UL / probe contexts */
  int group_req, group_sz; /* minCount asked of the driver for the all-SM split and the group size it yields */
  int full_groups, tail_sms, disc_groups; /* groups of group_sz, SMs of the tail group, discovery groups */
  int dl_sms, ul_sms, probe_sms;          /* 0 unless split2; ul = dl + probe (the UL context overlaps the DL one) */
  int dl_groups, ul_groups;               /* groups of the DL / UL contexts (tail group included) */
  int idle_sms;            /* SMs in no context when split2 (discovery groups wider than disc) */
  int dl_pct;              /* 100 * dl / rt, 0 unless split2 */
} nr_gpu_sm_plan_t;

static inline int nr_smp_round_up(int v, int g) { return g > 1 ? (v + g - 1) / g * g : v; }

static inline void nr_gpu_sm_plan(const nr_gpu_sm_in_t *in, nr_gpu_sm_plan_t *p)
{
  const int gran = in->granularity > 1 ? in->granularity : 1;
  *p = (nr_gpu_sm_plan_t){.reason = "ok", .sm = in->sm_count, .gran = gran};
  if (in->tier == NR_GPU_NONE) { p->reason = "no_gpu"; return; }
  if (!in->green) { p->reason = "no_green"; return; }
  if (in->sm_count < 1) { p->reason = "no_sm"; return; }
  const int sm = in->sm_count, req = sm / NR_SMP_DISC_DIV > 1 ? sm / NR_SMP_DISC_DIV : 1;
  p->disc_sms = nr_smp_round_up(req, gran);
  p->rt_sms = sm - p->disc_sms;
  if (p->rt_sms <= 0) { p->disc_sms = p->rt_sms = 0; p->reason = "no_rt_sms"; return; }
  p->green = 1;
  /* group cut of all SMs */
  p->group_req = NR_SMP_GROUP_REQ;
  p->group_sz = nr_smp_round_up(p->group_req, gran);
  while (sm / p->group_sz > NR_SMP_MAX_FULL_GROUPS) p->group_sz += gran, p->group_req = p->group_sz;
  p->full_groups = sm / p->group_sz;
  p->tail_sms = sm - p->full_groups * p->group_sz;
  p->disc_groups = (p->disc_sms + p->group_sz - 1) / p->group_sz;
  const int tot = p->full_groups + (p->tail_sms > 0), dl_first = p->disc_groups + 1;
  if (p->full_groups < dl_first || tot <= dl_first) return; /* no room for probe + DL groups: one real-time context */
  const int w = nr_smp_round_up(NR_SMP_SM_PER_DL_INSTANCE, gran), inst = in->dl_instances > 0 ? in->dl_instances : NR_SMP_DL_INSTANCES;
  const int dl = sm - dl_first * p->group_sz;
  if (dl < inst * w || dl * 100 < NR_SMP_DL_PCT_MIN * p->rt_sms) return;
  p->split2 = 1;
  p->dl_sms = dl;
  p->probe_sms = p->group_sz;
  p->ul_sms = dl + p->group_sz;
  p->dl_groups = tot - dl_first;
  p->ul_groups = tot - p->disc_groups;
  p->idle_sms = p->disc_groups * p->group_sz - p->disc_sms;
  p->dl_pct = dl * 100 / p->rt_sms;
}

#ifdef __cplusplus
}
#endif
#endif
