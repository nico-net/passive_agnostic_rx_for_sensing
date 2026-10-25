# Multi-PCI seam owned by the cloud task group C (plan 2026-10-23-mpci-main-plan.md); targets for cell state, pool fair share, transport.

# ---- B2B4 ----
# B4: pure SM plan (header-only, no CUDA); B2's test (test_nr_core_layout) lives in common/utils/tests/CMakeLists.txt.
if(ENABLE_TESTS)
  add_executable(test_nr_gpu_sm_plan ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/tests/test_nr_gpu_sm_plan.cpp)
  target_include_directories(test_nr_gpu_sm_plan PRIVATE ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
  target_link_libraries(test_nr_gpu_sm_plan PRIVATE GTest::gtest GTest::gtest_main)
  add_dependencies(tests test_nr_gpu_sm_plan)
  add_test(NAME test_nr_gpu_sm_plan COMMAND test_nr_gpu_sm_plan)
endif()
# ---- end B2B4 ----
