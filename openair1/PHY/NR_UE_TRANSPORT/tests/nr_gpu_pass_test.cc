#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cuda_runtime.h>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
static bool have_gpu() { return nr_gpu_model() >= 0 && getenv("NR_GPU_TEST_SKIP") == nullptr; }

TEST(GpuMem, RoundTripBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  for (int force : {0, 1}) {
    setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
    nr_gpu_buf_t b;
    ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 4096));
    EXPECT_EQ(force ? NR_GPU_MODEL_DISCRETE : nr_gpu_model(), b.model);
    for (int i = 0; i < 4096; i++) ((unsigned char *)b.host)[i] = (unsigned char)i;
    ASSERT_EQ(0, nr_gpu_buf_h2d_async(&b, 0, 4096, nullptr));
    memset(b.host, 0, 4096);
    ASSERT_EQ(0, nr_gpu_buf_d2h_async(&b, 0, 4096, nullptr));
    cudaDeviceSynchronize();
    if (b.model == NR_GPU_MODEL_DISCRETE) {
      for (int i = 0; i < 4096; i++) ASSERT_EQ((unsigned char)i, ((unsigned char *)b.host)[i]);
    }
    nr_gpu_buf_free(&b);
  }
}
TEST(GpuMem, AllocIsFirstTouched) {
  if (!have_gpu()) GTEST_SKIP();
  nr_gpu_buf_t b;
  ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 1 << 20));
  for (size_t i = 0; i < (1u << 20); i += 4096) ASSERT_EQ(0, ((unsigned char *)b.host)[i]);
  nr_gpu_buf_free(&b);
}
