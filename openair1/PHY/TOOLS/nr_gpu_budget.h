/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_GPU_BUDGET_H
#define NR_GPU_BUDGET_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One instance in the T18 platform DSO, including RTLD_LOCAL CUDA modules.
 * Opt in with ISAC_GPU_BUDGET=1. No CUDA dependency in this admission policy. */
int nr_gpu_budget_enabled(void);
uint64_t nr_gpu_budget_now_us(void);
unsigned nr_gpu_rt_deadline_us(void);
/* Optional compound-operation deadline (e.g. PDSCH FEP + LLR + self-check). */
void nr_gpu_budget_rt_job_start(uint64_t start_us);
uint64_t nr_gpu_budget_rt_start_us(void);
/* RT admission never waits for discovery; blocks new discovery submissions. */
int nr_gpu_budget_rt_begin(void);
void nr_gpu_budget_rt_end(void);
int nr_gpu_budget_rt_active(void); /* RT users in flight, 0 when the budget is off */
void nr_gpu_budget_rt_result(uint64_t waited_us, int fallback);
/* Query uses CUDA scalar ABI: 0 complete, 600 not ready. Clock injection permits
 * contention tests to exercise the production deadline loop without a device. */
int nr_gpu_budget_rt_wait(int (*query)(void *), void *job, uint64_t start,
                          uint64_t (*clock_us)(void *), void *clock_arg);
/* Worker-only admission: 1 admitted, 0 yield, -1 disabled after a CUDA error.
 * Completion is charged using CUDA event elapsed time, rounded UP to us.
 * No catch-up bursts; one discovery submission globally, at most 5 ms. */
int nr_gpu_budget_discovery_begin(uint64_t now_us);
uint64_t nr_gpu_budget_discovery_delay_us(uint64_t now_us);
void nr_gpu_budget_discovery_end(uint64_t now_us, uint64_t gpu_us);
void nr_gpu_budget_fault(void);
/* OTATAIL: a discovery user that must not be starved (the nID sweep in ACQUIRE) raises prefer; other discovery
 * users (CSI-RS) do not start a slice while it is raised. */
void nr_gpu_budget_discovery_prefer(int on);
int nr_gpu_budget_discovery_preferred(void);
typedef struct {
  double gpu_budget_used_pct;
  uint64_t discovery_yields, rt_gpu_wait_max_us, rt_gpu_fallbacks, discovery_overruns;
} nr_gpu_budget_metrics_t;
void nr_gpu_budget_metrics(nr_gpu_budget_metrics_t *out);
/* Pure duty policy; callers serialize state and provide epoch/bank evidence. */
enum { NR_GPU_DISC_ACQUIRE, NR_GPU_DISC_MAINTAIN, NR_GPU_DISC_PAUSED };
typedef struct { uint64_t epoch, acquired_us, next_us; int published, verified, unexplained, mode; } nr_gpu_discovery_duty_t;
int nr_gpu_discovery_due(nr_gpu_discovery_duty_t *, uint64_t now, uint64_t epoch,
                         int published, int all_verified, int unexplained,
                         uint64_t acquire_us, uint64_t maintain_us, uint64_t recheck_us);
#ifdef NR_GPU_BUDGET_TEST
void nr_gpu_budget_test_reset(unsigned pct);
#endif
#ifdef __cplusplus
}
#endif
#endif
