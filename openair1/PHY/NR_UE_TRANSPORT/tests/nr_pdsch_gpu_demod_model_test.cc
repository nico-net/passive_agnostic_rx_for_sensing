/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Tier guard of the standalone GPU demod (libpdsch_gpu.so, nr_pdsch_gpu_demod_run): its kernels read caller memory in place, so it must refuse
 * (-1, before any CUDA call of the module) on the DISCRETE and CPU tiers instead of faulting or migrating pages. The tier is forced through the
 * platform's diagnostic hook. Skipped (GTEST_SKIP, exit 0) without the module or a device. */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <cstring>
#include "nr_pdsch_gpu_demod.h"

namespace {
enum { TIER_NONE = 0, TIER_DISCRETE = 2 }; /* nr_gpu_platform_model_t values */
void (*g_force)(int);
nr_gpu_demod_run_fn g_run;

/* One job for every tier; a job that reached a kernel would dereference its NULL pointers. */
int run_null_job()
{
  nr_gpu_demod_job_t j;
  memset(&j, 0, sizeof(j));
  j.nbRx = 1;
  j.nl = 1;
  j.ext = 16;
  return g_run(&j);
}

class DemodTier : public testing::Test {
 protected:
  static void SetUpTestSuite()
  {
    void *plat = dlopen("./libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL); /* the module's nr_gpu_* imports */
    if (!plat)
      return;
    g_force = (void (*)(int))dlsym(plat, "nr_gpu_platform_test_force_model");
    void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
    g_run = dl ? (nr_gpu_demod_run_fn)dlsym(dl, "nr_pdsch_gpu_demod_run") : nullptr;
  }
  void SetUp() override
  {
    if (!g_force || !g_run)
      GTEST_SKIP() << "no ./libnr_gpu_platform.so / ./libpdsch_gpu.so";
  }
};
} // namespace

TEST_F(DemodTier, ForcedDiscreteRefuses)
{
  g_force(TIER_DISCRETE);
  EXPECT_EQ(run_null_job(), -1);
}

TEST_F(DemodTier, ForcedCpuRefuses)
{
  g_force(TIER_NONE);
  EXPECT_EQ(run_null_job(), -1);
}
