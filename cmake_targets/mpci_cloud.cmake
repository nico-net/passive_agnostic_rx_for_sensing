# Multi-PCI seam owned by the cloud task group C (plan 2026-10-23-mpci-main-plan.md); targets for cell state, pool fair share, transport.

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
