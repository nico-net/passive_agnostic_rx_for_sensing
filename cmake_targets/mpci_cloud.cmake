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
  target_link_libraries(test_nr_gpu_sm_plan PRIVATE GTest::gtest)
  add_dependencies(tests test_nr_gpu_sm_plan)
  add_test(NAME test_nr_gpu_sm_plan COMMAND test_nr_gpu_sm_plan)
endif()
# ---- end B2B4 ----

# ---- B3 ----
# Cell admission model (pure C; the host/GPU budgets are header-only, nr_host_membudget.c is linked for the N=1 replay test) + the fixture CLI the Python mirror is compared with.
if(ENABLE_TESTS)
  add_library(nr_cell_admit STATIC ${CMAKE_SOURCE_DIR}/common/utils/nr_cell_admit.c ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/nr_gpu_cell.c) # G1: + nr_gpu_cell.c (nr_cell_admit.c calls nr_gpu_cell_mem)
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
  target_link_libraries(test_nr_cell_inventory PRIVATE nr_cell_inventory GTest::gtest)
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

# ---- C1C2 ----
# C1: IQ ring (anonymous / POSIX shm backing), consumer device shm_iq, publisher wrapper. C2: CPU channelizer iq_chan + iq_ddc. Sources live in radio/COMMON
# (task group C file ownership); the targets are declared here so radio/COMMON/CMakeLists.txt stays untouched. iq_ddc at -O3 so its loops vectorise.
set(MPCI_RC ${CMAKE_SOURCE_DIR}/radio/COMMON)
set_source_files_properties(${MPCI_RC}/iq_ddc.c ${MPCI_RC}/tests/iq_ddc_ref.c PROPERTIES COMPILE_OPTIONS "-O3")

# consumer device, loaded by name like raw_iq (device = { name = "shm_iq"; })
add_library(shm_iq MODULE ${MPCI_RC}/shm_iq.c ${MPCI_RC}/shm_iq_ring.c ${MPCI_RC}/iq_ddc.c)
target_include_directories(shm_iq PRIVATE ${MPCI_RC})
target_link_libraries(shm_iq PRIVATE radio_common m pthread rt)
set_target_properties(shm_iq PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# in-process transport + channelizer as one static library for the receiver (D1/G5 link it)
add_library(mpci_iq STATIC ${MPCI_RC}/shm_iq_ring.c ${MPCI_RC}/iq_ddc.c ${MPCI_RC}/iq_chan.c)
target_include_directories(mpci_iq PUBLIC ${MPCI_RC})
target_link_libraries(mpci_iq PUBLIC m pthread rt)
set_target_properties(mpci_iq PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ENABLE_TESTS)
  # ring / device / publisher tests; --wrap=shm_open counts every shm_open of the ring code (the anonymous path must make none); ENABLE_EXPORTS lets the dlopen'd
  # device share this executable's ring registry and exit_function
  add_executable(test_shm_iq ${MPCI_RC}/tests/test_shm_iq.c ${MPCI_RC}/shm_iq_ring.c ${MPCI_RC}/iq_ddc.c)
  target_include_directories(test_shm_iq PRIVATE ${MPCI_RC} ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
  set_target_properties(test_shm_iq PROPERTIES ENABLE_EXPORTS ON)
  target_link_options(test_shm_iq PRIVATE -Wl,--wrap=shm_open)
  target_link_libraries(test_shm_iq PRIVATE log_headers m dl pthread rt)
  add_dependencies(test_shm_iq shm_iq)
  add_dependencies(tests test_shm_iq)
  add_test(NAME test_shm_iq COMMAND test_shm_iq $<TARGET_FILE:shm_iq>)

  # iq_ddc against the frozen reference (0 LSB), portable vs NEON, passband rule
  add_executable(test_iq_ddc_equiv ${MPCI_RC}/tests/test_iq_ddc_equiv.c ${MPCI_RC}/iq_ddc.c ${MPCI_RC}/tests/iq_ddc_ref.c)
  target_link_libraries(test_iq_ddc_equiv PRIVATE m)
  add_dependencies(tests test_iq_ddc_equiv)
  add_test(NAME test_iq_ddc_equiv COMMAND test_iq_ddc_equiv)
  # channelizer: bit-exactness through the loop, live add/remove/retune, overrun recovery, backend registry, capacity bench; the 60 s soak is a separate test
  add_executable(test_iq_chan ${MPCI_RC}/tests/test_iq_chan.c)
  target_include_directories(test_iq_chan PRIVATE ${MPCI_RC}/tests)
  target_link_libraries(test_iq_chan PRIVATE mpci_iq)
  add_dependencies(tests test_iq_chan)
  add_test(NAME test_iq_chan COMMAND test_iq_chan)
  add_test(NAME test_iq_chan_soak5 COMMAND nice -n 19 $<TARGET_FILE:test_iq_chan> --soak 5) # the 60 s run is the STOP evidence, run by hand
endif()
# ---- end C1C2 ----

# ---- A7 ----
# Per-cell CSI-RS discovery host side: interleave test (detector history, TD front end, prealloc/ledger/bound workers). Same sources and libraries as test_nr_csirs_disc_td
# (nr_cell.c comes with LOG); nr_host_membudget.c is compiled in so the per-cell host ledger is exercised against the real budget.
if(ENABLE_TESTS)
  set(A7_TR ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT)
  add_executable(test_nr_csirs_disc_cell
    ${A7_TR}/tests/nr_csirs_disc_cell_test.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/INIT/nr_parms.c
    ${A7_TR}/nr_csirs_disc_td.c
    ${A7_TR}/nr_csirs_disc_feed.c
    ${A7_TR}/nr_csirs_disc_backend.c
    ${A7_TR}/nr_csirs_disc.c
    ${A7_TR}/nr_csirs_disc_fp.c
    ${A7_TR}/nr_csirs_disc_hyp.c
    ${A7_TR}/nr_csirs_disc_sweep.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/MODULATION/slot_fep_nr.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_host_membudget.c)
  target_include_directories(test_nr_csirs_disc_cell PRIVATE ${A7_TR} ${A7_TR}/tests)
  target_link_libraries(test_nr_csirs_disc_cell PRIVATE nr_csirs_disc_synth nr_phy_common PHY_COMMON nr_common nr_fapi_p5
    asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs UTIL ${CMAKE_DL_LIBS} pthread m)
  target_compile_options(test_nr_csirs_disc_cell PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(test_nr_csirs_disc_cell PRIVATE -Wl,--gc-sections -Wl,--no-export-dynamic
    "-Wl,--export-dynamic-symbol=T_*" -Wl,--export-dynamic-symbol=g_log
    -Wl,--export-dynamic-symbol=logRecord_mt -Wl,--export-dynamic-symbol=exit_function
    -Wl,--export-dynamic-symbol=write_file_matlab
    "-Wl,--export-dynamic-symbol=nr_csirs_disc_sweep_*")
  add_dependencies(test_nr_csirs_disc_cell dfts)
  add_dependencies(tests test_nr_csirs_disc_cell)
  add_test(NAME test_nr_csirs_disc_cell COMMAND test_nr_csirs_disc_cell)
endif()
# ---- end A7 ----

# ---- A3 ----
# Per-cell state of acquisition / config epoch / observation writer / UE context / reconf rules / CORESET map (nr_cell.h conversion).
# The existing unit-test targets of these modules (root CMakeLists.txt) compile the converted sources; those that do not already link LOG
# get it here (nr_cell.c lives in LOG, never list it a second time). The interleave test runs cell X and cell Y alternately (one thread
# re-binding, two threads) and compares each cell with its solo run.
if(ENABLE_TESTS)
  foreach(t test_nr_passive_reconf_rules test_nr_passive_acq_state test_nr_passive_cfg_epoch test_nr_passive_obs test_nr_passive_ue_ctx test_nr_pdcch_coreset_map)
    if(TARGET ${t})
      target_link_libraries(${t} PRIVATE LOG)
    endif()
  endforeach()
  set(A3_SRC ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT)
  add_executable(test_mpci_a3_interleave
    ${A3_SRC}/tests/nr_mpci_a3_interleave_test.cc
    ${A3_SRC}/nr_passive_acq_state.c
    ${A3_SRC}/nr_tdd_pattern.c
    ${A3_SRC}/nr_passive_cfg_epoch.c
    ${A3_SRC}/nr_passive_obs.c
    ${A3_SRC}/nr_passive_ue_ctx.c
    ${A3_SRC}/nr_passive_reconf_rules.c
    ${A3_SRC}/nr_pdcch_coreset_map.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_REFSIG/refsig.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_REFSIG/nr_dmrs_rx.c
    ${CMAKE_SOURCE_DIR}/common/utils/ds/hashtable.c)
  target_include_directories(test_mpci_a3_interleave PRIVATE ${A3_SRC} ${CMAKE_SOURCE_DIR}/common/utils/ds)
  target_link_libraries(test_mpci_a3_interleave PRIVATE nr_pdcch_nid_worker nr_common UTIL utils LOG GTest::gtest pthread m)
  add_dependencies(tests test_mpci_a3_interleave)
  add_test(NAME test_mpci_a3_interleave COMMAND test_mpci_a3_interleave)
endif()
# ---- end A3 ----

# ---- G1 ----
# GPU memory ledger (nr_gpu_memstat) on a mocked CUDA runtime, and nr_gpu_cell_bytes (pure arithmetic, no GPU: also built on the cloud host).
if(ENABLE_TESTS)
  add_executable(test_nr_gpu_memstat ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/tests/nr_gpu_memstat_test.cc
    ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_gpu_memstat.c ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_gpu_platform.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_gpu_mem.c ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/nr_host_membudget.c)
  target_compile_definitions(test_nr_gpu_memstat PRIVATE NR_GPU_PLATFORM_TEST)
  target_include_directories(test_nr_gpu_memstat PRIVATE ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
  target_link_libraries(test_nr_gpu_memstat PRIVATE GTest::gtest_main ${CMAKE_DL_LIBS} pthread)
  add_dependencies(tests test_nr_gpu_memstat)
  add_test(NAME test_nr_gpu_memstat COMMAND test_nr_gpu_memstat)
  add_executable(test_nr_gpu_cell ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_cell_test.cc
    ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/nr_gpu_cell.c)
  target_include_directories(test_nr_gpu_cell PRIVATE ${CMAKE_SOURCE_DIR}/openair1 ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_nr_gpu_cell PRIVATE GTest::gtest_main)
  add_dependencies(tests test_nr_gpu_cell)
  add_test(NAME test_nr_gpu_cell COMMAND test_nr_gpu_cell)
  if(ENABLE_LDPC_CUDA)
    # nr_gpu_cell_bytes vs what the GPU modules allocate: one child process per (kind, PRB, RX), ledger compared with the formula. Serial, on the GPU.
    add_executable(test_nr_gpu_cell_bytes_gpu ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_cell_bytes_gpu_test.cc
      ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/nr_gpu_cell.c)
    target_include_directories(test_nr_gpu_cell_bytes_gpu PRIVATE ${CMAKE_SOURCE_DIR}/openair1 ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
    target_link_libraries(test_nr_gpu_cell_bytes_gpu PRIVATE GTest::gtest ${CMAKE_DL_LIBS} pthread)
    set_target_properties(test_nr_gpu_cell_bytes_gpu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} ENABLE_EXPORTS ON)
    add_dependencies(test_nr_gpu_cell_bytes_gpu dfts)
    add_dependencies(test_nr_gpu_cell_bytes_gpu nr_gpu_platform pdsch_gpu pdcch_gpu pdcch_nid_gpu csirs_disc_gpu nr_csirs_disc_gpu_sweep)
    add_dependencies(tests test_nr_gpu_cell_bytes_gpu)
    add_test(NAME test_nr_gpu_cell_bytes_gpu COMMAND ./test_nr_gpu_cell_bytes_gpu WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(test_nr_gpu_cell_bytes_gpu PROPERTIES RUN_SERIAL TRUE)
  endif()
endif()
# ---- end G1 ----

# ---- A6 ----
# Per-cell UL state: UL layout lock, UL data-id lock table, UL background discovery (nr_cell.h conversion). The existing unit-test targets that compile
# converted sources get LOG (nr_cell.c lives there). The interleave test runs two cells with different UL BWP alternately (one thread re-binding, two
# threads) and compares each cell with its solo run on other cell slots.
if(ENABLE_TESTS)
  foreach(t test_nr_pusch_ul_disc test_nr_pusch_solver test_nr_pdcch_blind_monitor)

# ---- A5 ----
# Per-cell state of the PDSCH solver / async-recovery lane / decode evidence + DL HARQ store / PDSCH queue (nr_cell.h conversion).
# The existing solver and recovery tests compile the converted sources and now need LOG (nr_cell.c lives there). The interleave test runs
# workload X on one cell and a different Y on another (one re-binding thread, two bound threads) and compares each with its solo run; it links
# the receiver libraries (like nr_dlsim) because the decode evidence and the HARQ store live in nr_pdsch_passive_decode.c.
if(ENABLE_TESTS)
  foreach(t test_nr_pdsch_solver test_nr_pdsch_recovery)
    if(TARGET ${t})
      target_link_libraries(${t} PRIVATE LOG)
    endif()
  endforeach()
  set(A6_SRC ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT)
  add_executable(test_mpci_a6_ul_cells
    ${A6_SRC}/tests/nr_mpci_a6_ul_cells_test.cc
    ${A6_SRC}/nr_pusch_ul_bgdisc.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_REFSIG/refsig.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_REFSIG/nr_dmrs_rx.c
    ${CMAKE_SOURCE_DIR}/common/utils/ds/hashtable.c)
  target_include_directories(test_mpci_a6_ul_cells PRIVATE ${A6_SRC} ${CMAKE_SOURCE_DIR}/common/utils)
  target_link_libraries(test_mpci_a6_ul_cells PRIVATE nr_pdcch_blind_monitor MAC_NR_COMMON asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs crc_byte UTIL LOG GTest::gtest pthread m)
  add_dependencies(tests test_mpci_a6_ul_cells)
  add_test(NAME test_mpci_a6_ul_cells COMMAND test_mpci_a6_ul_cells)
endif()
# ---- end A6 ----

  add_executable(test_mpci_a5_interleave ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/tests/nr_mpci_a5_interleave_test.cc
                                       ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/tests/nr_mpci_a5_stubs.c)
  target_include_directories(test_mpci_a5_interleave PRIVATE ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT)
  target_link_libraries(test_mpci_a5_interleave PRIVATE
    -Wl,--start-group UTIL SIMU PHY_NR_COMMON PHY_NR PHY_NR_UE PHY_NR_PASSIVE_UL NR_UE_ISAC nr_pdcch_blind_monitor SCHED_NR_LIB SCHED_NR_UE_LIB MAC_UE_NR MAC_NR_COMMON CONFIG_LIB L2_NR -Wl,--end-group
    m pthread ${T_LIB} ITTI dl nr_ue_phy_meas physim_common NR_L2_UE GTest::gtest)
  target_include_directories(test_mpci_a5_interleave PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/openair1 ${CMAKE_SOURCE_DIR}/executables)
  target_link_libraries(test_mpci_a5_interleave PRIVATE asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs)
  add_dependencies(tests test_mpci_a5_interleave)
  add_test(NAME test_mpci_a5_interleave COMMAND test_mpci_a5_interleave)
endif()
# ---- end A5 ----
