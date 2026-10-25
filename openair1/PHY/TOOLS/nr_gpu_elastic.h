/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_ELASTIC_H
#define NR_GPU_ELASTIC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* On-demand sizing of the two modules whose size changes WHAT they observe (csirs discovery ring: slots; PDCCH nID window: batches).
 *
 * T6b gave each module a fixed share of the device budget; on a small card that cut the ring to 221 of 640 slots and the window to 6 of 10.
 * Here the module keeps that share-sized core (base: always held, so never worse than T6b) and grows from the budget headroom (nr_gpu_membudget_headroom:
 * budget minus what every module committed, so the graph cap the other modules did not use is real headroom) in steps of `step` units while its
 * demand exceeds the current size; it gives the extra back after `idle_limit` consecutive ticks whose demand fits the base again.
 *   - full <= base (the share covers the full size, GB10 / unlimited budget): disabled, cur == full from the start, the controller never moves.
 *   - A grow that the budget refuses is partial when some steps fit; when none fits the module keeps its current size (never drops work), counts the
 *     denial, reports it once (first == 1) and backs off `backoff_limit` ticks before it asks again.
 * Pure state machine, no CUDA: the module applies the target and reports the result with nr_gpu_elastic_done(). */
typedef struct {
  int base, full, step, cur;
  int idle_limit, idle;
  int backoff_limit, backoff;
  uint64_t grows, shrinks, denied;
  int denied_reported;
} nr_gpu_elastic_t;
static inline void nr_gpu_elastic_init(nr_gpu_elastic_t *e, int base, int full, int step, int idle_limit, int backoff_limit)
{
  *e = (nr_gpu_elastic_t){.base = base, .full = full > base ? full : base, .step = step < 1 ? 1 : step, .cur = base, .idle_limit = idle_limit < 1 ? 1 : idle_limit,
                          .backoff_limit = backoff_limit < 0 ? 0 : backoff_limit};
}
static inline int nr_gpu_elastic_enabled(const nr_gpu_elastic_t *e) { return e->full > e->base; }
/* Units wanted for a demand: base + whole steps, at most full. */
static inline int nr_gpu_elastic_round(const nr_gpu_elastic_t *e, int demand)
{
  if (demand <= e->base) return e->base;
  int t = e->base + ((demand - e->base + e->step - 1) / e->step) * e->step;
  return t > e->full ? e->full : t;
}
/* One tick with the module's current demand (units). Returns the target size; == cur: nothing to do. */
static inline int nr_gpu_elastic_target(nr_gpu_elastic_t *e, int demand)
{
  if (!nr_gpu_elastic_enabled(e)) return e->cur;
  if (demand > e->cur) {
    e->idle = 0;
    if (e->backoff > 0) { e->backoff--; return e->cur; }
    return nr_gpu_elastic_round(e, demand);
  }
  if (demand <= e->base) {
    if (e->cur > e->base && ++e->idle >= e->idle_limit) { e->idle = 0; return e->base; }
    return e->cur;
  }
  e->idle = 0; /* demand between base and cur: keep what is held */
  return e->cur;
}
/* Largest target' in [cur, target] on the step grid whose extra bytes (per_unit per unit) fit `headroom`; cur when not even one step fits. */
static inline int nr_gpu_elastic_fit(const nr_gpu_elastic_t *e, int target, size_t per_unit, size_t headroom)
{
  if (target <= e->cur) return e->cur;
  if (headroom == SIZE_MAX || !per_unit || (size_t)(target - e->cur) <= headroom / per_unit) return target;
  const int k = (int)(headroom / per_unit);
  return e->cur + (k / e->step) * e->step;
}
/* Report the outcome of a resize attempt toward `want` (> cur for a grow, base for a shrink): ok != 0 -> the module now holds `now` units.
 * A grow that got less than it asked (or failed) counts a denial and starts the back-off. Returns 1 exactly once per controller, on the first denial
 * (the caller logs it), else 0. */
static inline int nr_gpu_elastic_done(nr_gpu_elastic_t *e, int want, int now, int ok)
{
  int denied = 0;
  if (ok) {
    if (now > e->cur) e->grows++;
    else if (now < e->cur) e->shrinks++;
    denied = want > e->cur && now < want;
    e->cur = now;
  } else
    denied = 1;
  if (!denied) {
    e->backoff = 0;
    return 0;
  }
  e->denied++;
  e->backoff = e->backoff_limit;
  if (e->denied_reported) return 0;
  e->denied_reported = 1;
  return 1;
}
#ifdef __cplusplus
}
#endif
#endif
