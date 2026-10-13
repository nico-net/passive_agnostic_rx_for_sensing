/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_GPU_BUDGET_CUH
#define NR_CSIRS_DISC_GPU_BUDGET_CUH
/* OTATAIL: ISAC_CSIRS_DISC_BUDGET=1 (default off; off = the unchanged path). Every CSI-RS discovery GPU step (one
 * launch group and its wait) becomes one slice of the shared GPU discovery budget (nr_gpu_budget.c, needs
 * ISAC_GPU_BUDGET=1): a slice never starts while a real-time PDSCH/PDCCH GPU job is in flight and discovery as a
 * whole stays within ISAC_GPU_DISCOVERY_BUDGET_PCT of GPU time, the way the PDCCH nID sweep already does
 * (nr_gpu_budget_cuda.h). The slice is charged by wall time, so host overhead counts against discovery too.
 * While the nID sweep is in ACQUIRE (nr_gpu_budget_discovery_prefer) no CSI-RS slice starts, so time-to-first-bank
 * is unchanged. The budget symbols are weak: a test binary without libnr_gpu_platform runs ungated. */
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
extern "C" int nr_gpu_budget_enabled(void) __attribute__((weak));
extern "C" uint64_t nr_gpu_budget_now_us(void) __attribute__((weak));
extern "C" int nr_gpu_budget_discovery_begin(uint64_t) __attribute__((weak));
extern "C" uint64_t nr_gpu_budget_discovery_delay_us(uint64_t) __attribute__((weak));
extern "C" void nr_gpu_budget_discovery_end(uint64_t, uint64_t) __attribute__((weak));
extern "C" int nr_gpu_budget_discovery_preferred(void) __attribute__((weak));
struct CsirsDiscGate {
  uint64_t t0 = 0;
  bool held = false;
  static thread_local int depth; /* nested gates (footprints -> fold/lattice) ride the outer slice */
  static bool on()
  {
    static int v = -1;
    if (v < 0) {
      const char *e = getenv("ISAC_CSIRS_DISC_BUDGET");
      v = e && atoi(e) == 1 && nr_gpu_budget_enabled && nr_gpu_budget_now_us && nr_gpu_budget_discovery_begin
          && nr_gpu_budget_discovery_delay_us && nr_gpu_budget_discovery_end && nr_gpu_budget_enabled();
    }
    return v == 1;
  }
  CsirsDiscGate()
  {
    if (!on() || depth++) return;
    int rc;
    for (;;) {
      if (nr_gpu_budget_discovery_preferred && nr_gpu_budget_discovery_preferred()) { usleep(1000); continue; }
      if ((rc = nr_gpu_budget_discovery_begin(nr_gpu_budget_now_us())) != 0) break;
      const uint64_t d = nr_gpu_budget_discovery_delay_us(nr_gpu_budget_now_us());
      if (d > 100) usleep(d - 50); else sched_yield();
    }
    held = rc > 0; /* rc < 0: budget fault latched, run ungated */
    t0 = nr_gpu_budget_now_us();
  }
  ~CsirsDiscGate()
  {
    if (!on()) return;
    depth--;
    if (held) nr_gpu_budget_discovery_end(nr_gpu_budget_now_us(), nr_gpu_budget_now_us() - t0);
  }
};
inline thread_local int CsirsDiscGate::depth = 0;
#endif
