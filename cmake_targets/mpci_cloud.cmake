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

# ---- B3 ----
# Cell admission model (pure C; the host/GPU budgets are header-only, nr_host_membudget.c is linked for the N=1 replay test) + the fixture CLI the Python mirror is compared with.
if(ENABLE_TESTS)
  add_library(nr_cell_admit STATIC ${CMAKE_SOURCE_DIR}/common/utils/nr_cell_admit.c)
  target_include_directories(nr_cell_admit PUBLIC ${CMAKE_SOURCE_DIR}/common/utils ${CMAKE_SOURCE_DIR}/openair1)
  add_executable(test_nr_cell_admit ${CMAKE_SOURCE_DIR}/common/utils/tests/test_nr_cell_admit.cpp ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_host_membudget.c)
  target_include_directories(test_nr_cell_admit PRIVATE ${CMAKE_SOURCE_DIR}/common/utils ${CMAKE_SOURCE_DIR}/openair1)
  target_link_libraries(test_nr_cell_admit PRIVATE nr_cell_admit GTest::gtest GTest::gtest_main pthread)
  add_dependencies(tests test_nr_cell_admit)
  add_test(NAME test_nr_cell_admit COMMAND test_nr_cell_admit)
  add_executable(nr_cell_admit_cli ${CMAKE_SOURCE_DIR}/common/utils/tests/nr_cell_admit_cli.c)
  target_link_libraries(nr_cell_admit_cli PRIVATE nr_cell_admit)
  add_dependencies(tests nr_cell_admit_cli)
  add_test(NAME test_admit_model_py COMMAND python3 ${CMAKE_SOURCE_DIR}/tests/passive_rx/mpci/test_admit_model.py --cli $<TARGET_FILE:nr_cell_admit_cli>)
endif()
# ---- end B3 ----

# ---- D2 ----
# Per-cell line attribution: LOG prefix + nr_cprintf shim (C test), monitor/scorer multi-cell tests (python).
if(ENABLE_TESTS)
  add_executable(test_nr_log_cell ${CMAKE_SOURCE_DIR}/common/utils/tests/test_nr_log_cell.c)
  target_include_directories(test_nr_log_cell PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/common/utils)
  target_link_libraries(test_nr_log_cell PRIVATE LOG CONFIG_LIB minimal_lib pthread)
  add_dependencies(tests test_nr_log_cell)
  add_test(NAME test_nr_log_cell COMMAND test_nr_log_cell)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME test_mpci_multicell_monitor
      COMMAND ${Python3_EXECUTABLE} -m unittest -v test_multicell
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests/passive_rx/monitor)
  endif()
endif()
# ---- end D2 ----

# ---- D1 ----
# Cell inventory v1 (strict parser + atomic-file watcher) and the cell manager. nr_cell.c is compiled directly (these tests do not link LOG; as in A1).
# nr_cell_admit (static lib, B3 block) + nr_host_membudget.c as in test_nr_cell_admit. MPCI_INV_DIR is the fixture directory (a path, not a behaviour switch).
if(ENABLE_TESTS)
  add_library(nr_cell_inventory STATIC ${CMAKE_SOURCE_DIR}/common/utils/nr_cell_inventory.c)
  target_include_directories(nr_cell_inventory PUBLIC ${CMAKE_SOURCE_DIR}/common/utils ${CMAKE_SOURCE_DIR}/openair1)
  add_executable(test_nr_cell_inventory ${CMAKE_SOURCE_DIR}/common/utils/tests/test_nr_cell_inventory.cpp)
  target_link_libraries(test_nr_cell_inventory PRIVATE nr_cell_inventory GTest::gtest GTest::gtest_main)
  target_compile_definitions(test_nr_cell_inventory PRIVATE MPCI_INV_DIR="${CMAKE_SOURCE_DIR}/tests/passive_rx/mpci/inventory")
  add_dependencies(tests test_nr_cell_inventory)
  add_test(NAME test_nr_cell_inventory COMMAND test_nr_cell_inventory)
  add_executable(test_nr_cell_mgr
    ${CMAKE_SOURCE_DIR}/common/utils/tests/test_nr_cell_mgr.cpp
    ${CMAKE_SOURCE_DIR}/common/utils/nr_cell_mgr.c
    ${CMAKE_SOURCE_DIR}/common/utils/nr_cell.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_host_membudget.c)
  target_include_directories(test_nr_cell_mgr PRIVATE ${CMAKE_SOURCE_DIR}/common/utils ${CMAKE_SOURCE_DIR}/openair1)
  target_link_libraries(test_nr_cell_mgr PRIVATE nr_cell_inventory nr_cell_admit GTest::gtest GTest::gtest_main pthread)
  target_compile_definitions(test_nr_cell_mgr PRIVATE MPCI_INV_DIR="${CMAKE_SOURCE_DIR}/tests/passive_rx/mpci/inventory")
  add_dependencies(tests test_nr_cell_mgr)
  add_test(NAME test_nr_cell_mgr COMMAND test_nr_cell_mgr)
endif()
# ---- end D1 ----
