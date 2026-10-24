/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * CPU tier of the GPU graph pass (libpdsch_gpu.so, nr_gpu_pass_init): with the platform forced to the CPU tier the pass must refuse (-1, repeatedly)
 * before it touches CUDA, so the receiver keeps the non-pass GPU / CPU path. The tier is forced through the platform's diagnostic hook.
 * Skipped (GTEST_SKIP, exit 0) without the module. */
#include <gtest/gtest.h>
#include <dlfcn.h>

namespace {
enum { TIER_NONE = 0 }; /* nr_gpu_platform_model_t value */
}

TEST(GpuPassCpu, InitRefusesOnCpuTier)
{
  void *plat = dlopen("./libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL); /* the module's nr_gpu_* imports */
  void *dl = plat ? dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL) : nullptr;
  if (!plat || !dl)
    GTEST_SKIP() << "no ./libnr_gpu_platform.so / ./libpdsch_gpu.so";
  auto force = (void (*)(int))dlsym(plat, "nr_gpu_platform_test_force_model");
  auto init = (int (*)(int))dlsym(dl, "nr_gpu_pass_init");
  ASSERT_TRUE(force && init);
  force(TIER_NONE);
  EXPECT_EQ(init(1), -1);
  EXPECT_EQ(init(8), -1) << "refusal is stable";
}
