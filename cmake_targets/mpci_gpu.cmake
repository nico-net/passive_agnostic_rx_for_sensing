# Multi-PCI seam owned by the DGX task group G (plan 2026-10-23-mpci-main-plan.md); GPU accounting, per-cell GPU instances, channelizer kernels.

# ---- G2 ----
# nr_gpu_green.h wired to the derived SM plan: the plan's numbers equal the old literal code's (fake driver replay). Pure, no CUDA.
if(ENABLE_TESTS)
  add_executable(test_nr_gpu_green_g2 ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/tests/test_nr_gpu_green_g2.cpp)
  target_include_directories(test_nr_gpu_green_g2 PRIVATE ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
  target_link_libraries(test_nr_gpu_green_g2 PRIVATE GTest::gtest GTest::gtest_main)
  add_dependencies(tests test_nr_gpu_green_g2)
  add_test(NAME test_nr_gpu_green_g2 COMMAND test_nr_gpu_green_g2)
endif()
# ---- end G2 ----
