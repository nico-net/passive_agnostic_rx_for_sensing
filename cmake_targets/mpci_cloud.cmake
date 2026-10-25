# Multi-PCI seam owned by the cloud task group C (plan 2026-10-23-mpci-main-plan.md); targets for cell state, pool fair share, transport.

# ---- A1 ----
# nr_cell core (cell index, cell-indexed statics, cell paths) + static-TLS size check on its test binary.
if(ENABLE_TESTS)
add_executable(test_nr_cell
  ${CMAKE_SOURCE_DIR}/common/utils/tests/test_nr_cell.cpp
  ${CMAKE_SOURCE_DIR}/common/utils/tests/test_nr_cell_statics.c
  ${CMAKE_SOURCE_DIR}/common/utils/nr_cell.c)
target_include_directories(test_nr_cell PRIVATE ${CMAKE_SOURCE_DIR}/common/utils)
target_link_libraries(test_nr_cell PRIVATE GTest::gtest GTest::gtest_main pthread)
add_dependencies(tests test_nr_cell)
add_test(NAME test_nr_cell COMMAND test_nr_cell)
add_test(NAME test_nr_cell_static_tls
  COMMAND ${CMAKE_SOURCE_DIR}/tools/mpci/check_static_tls.sh $<TARGET_FILE:test_nr_cell>)
endif()
# ---- end A1 ----

# ---- A2 ----
# Static audit: scanner unit tests + table check (new object => fail). `--strict` (UNCLASSIFIED => fail) is NOT enabled yet:
# it becomes the gate when A3-A8 have classified every PER_CELL candidate.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME test_static_audit_scanner COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mpci/test_static_audit.py)
  add_test(NAME static_audit_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/mpci/static_audit.py --check)
endif()
# ---- end A2 ----

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
