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

# ---- G3 ----
# Per-cell instance quota of the GPU pass pools (pure logic, no CUDA). The GPU two-cell cases live in test_nr_gpu_pass (openair1/PHY/CODING/CMakeLists.txt).
if(ENABLE_TESTS)
  add_executable(test_nr_gpu_pass_quota ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_pass_quota_test.cc)
  target_include_directories(test_nr_gpu_pass_quota PRIVATE ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT ${CMAKE_SOURCE_DIR})
  target_link_libraries(test_nr_gpu_pass_quota PRIVATE GTest::gtest GTest::gtest_main)
  add_dependencies(tests test_nr_gpu_pass_quota)
  add_test(NAME test_nr_gpu_pass_quota COMMAND test_nr_gpu_pass_quota)
  # the two-cell GPU cases register cells with the pass module before the allocation fence: their own process (the fence tests of test_nr_gpu_pass seal it)
  add_test(NAME test_nr_gpu_pass_cells COMMAND test_nr_gpu_pass --gtest_filter=GpuPassCells.* WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()
# ---- end G3 ----

# ---- G3b ----
# The graph pass serves cells of another FEP geometry (n_rb_dl, first carrier offset, CP lengths, antennas): FEP vs the OAI CPU FEP per geometry, two-cell decode vs the stage path and the
# transmitted payload, the GPU memory budget gate. Same link set as test_nr_gpu_pass (the fixture header and the OAI CPU references); own process (the module initialises once).
if(ENABLE_TESTS AND ENABLE_LDPC_CUDA)
  find_package(CUDAToolkit REQUIRED) # CUDA::cudart is directory-scoped in openair1/PHY/CODING
  set(G3B_TESTS ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT/tests)
  add_executable(test_nr_gpu_pass_geo ${G3B_TESTS}/nr_gpu_pass_geo_test.cc
    ${CMAKE_SOURCE_DIR}/openair1/PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.c
    ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS/dfts_load.c)
  target_link_libraries(test_nr_gpu_pass_geo PRIVATE GTest::gtest_main nr_gpu_platform CUDA::cudart thread-pool LOG minimal_lib crc_byte UTIL PHY_NR PHY_NR_COMMON shlib_loader ${CMAKE_DL_LIBS} pthread m)
  add_dependencies(test_nr_gpu_pass_geo dfts pdsch_gpu ldpc)
  # ./libldpc.so (the OAI LDPC encoder of the pass fixture) resolves these from the executable, as in test_nr_gpu_pass
  foreach(sym check_crc crc24a crc24b crc16 crcTableInit completed_many_task_ans cpu_meas_enabled exit_function g_log logRecord_mt init_task_ans join_task_ans pushTpool T_active T_cache T_freelist_head T_stdout)
    target_link_options(test_nr_gpu_pass_geo PRIVATE "LINKER:--undefined=${sym}")
  endforeach()
  set_target_properties(test_nr_gpu_pass_geo PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  target_include_directories(test_nr_gpu_pass_geo PRIVATE ${G3B_TESTS} ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT ${OPENAIR1_DIR} ${OPENAIR_DIR} ${CUDAToolkit_INCLUDE_DIRS})
  add_dependencies(tests test_nr_gpu_pass_geo)
  add_test(NAME test_nr_gpu_pass_geo COMMAND test_nr_gpu_pass_geo WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_nr_gpu_pass_geo PROPERTIES ENVIRONMENT "CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps" SKIP_RETURN_CODE 77)
  add_test(NAME test_nr_gpu_pass_geo_discrete COMMAND test_nr_gpu_pass_geo WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_nr_gpu_pass_geo_discrete PROPERTIES ENVIRONMENT "NR_GPU_PASS_DISCRETE=1;CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps" SKIP_RETURN_CODE 77)
endif()
# ---- end G3b ----

# ---- G6 ----
# Radio -> wideband ring -> in-process channelizer -> per-cell rings: the span/rate planner (iq_span) and the feed (iq_feed) join the in-process IQ library;
# the receiver links it (the cell rings are registered in its anonymous-ring registry, which the dlopen'd shm_iq device shares through -rdynamic).
target_sources(mpci_iq PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON/iq_span.c ${CMAKE_SOURCE_DIR}/radio/COMMON/iq_feed.c)
target_link_libraries(nr-uesoftmodem PRIVATE mpci_iq)
if(ENABLE_TESTS)
  add_executable(test_iq_span ${CMAKE_SOURCE_DIR}/radio/COMMON/tests/test_iq_span.c ${CMAKE_SOURCE_DIR}/common/utils/nr_cell_inventory.c)
  target_include_directories(test_iq_span PRIVATE ${CMAKE_SOURCE_DIR}/common/utils ${CMAKE_SOURCE_DIR}/openair1)
  target_link_libraries(test_iq_span PRIVATE mpci_iq m)
  add_dependencies(tests test_iq_span)
  add_test(NAME test_iq_span COMMAND test_iq_span ${CMAKE_SOURCE_DIR}/tests/passive_rx/mpci/inventory/live_tuned_20261023.json)
  add_executable(test_iq_feed ${CMAKE_SOURCE_DIR}/radio/COMMON/tests/test_iq_feed.c)
  target_include_directories(test_iq_feed PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
  set_target_properties(test_iq_feed PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(test_iq_feed PRIVATE mpci_iq log_headers m dl pthread rt)
  add_dependencies(test_iq_feed shm_iq)
  add_dependencies(tests test_iq_feed)
  add_test(NAME test_iq_feed COMMAND test_iq_feed $<TARGET_FILE:shm_iq>)
endif()
# ---- end G6 ----

# ---- G5 ----
# GPU backend of the in-process channelizer (radio/COMMON/shm_iq_chan_gpu.cu, kernels of the T0 import), dlopen'd by the CHAN path selector of mpci_iq
# (iq_chan_gpu_sel.c: no CUDA link dependency of the receiver). The selector is part of mpci_iq in every build; without the module it keeps the CPU backend.
target_sources(mpci_iq PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON/iq_chan_gpu_sel.c)
target_include_directories(mpci_iq PRIVATE ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
target_link_libraries(mpci_iq PUBLIC ${CMAKE_DL_LIBS})
if(ENABLE_LDPC_CUDA)
  find_package(CUDAToolkit REQUIRED)
  include(${CMAKE_SOURCE_DIR}/cmake_targets/nr_gpu_arch.cmake)
  nr_gpu_enable_cuda() # CUDA is enabled in openair1/PHY/CODING's scope only; this file is included at the top level
  nr_gpu_architectures("${LDPC_CUDA_ARCH}" G5_CUDA_ARCHS)
  add_library(shm_iq_chan_gpu MODULE ${CMAKE_SOURCE_DIR}/radio/COMMON/shm_iq_chan_gpu.cu ${CMAKE_SOURCE_DIR}/radio/COMMON/iq_ddc.c)
  set_target_properties(shm_iq_chan_gpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} CUDA_ARCHITECTURES "${G5_CUDA_ARCHS}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(shm_iq_chan_gpu PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS ${CMAKE_SOURCE_DIR}/openair1/PHY/NR_UE_TRANSPORT
                             ${CUDAToolkit_INCLUDE_DIRS})
  target_link_libraries(shm_iq_chan_gpu PRIVATE nr_gpu_platform CUDA::cudart m pthread)
  add_dependencies(nr-uesoftmodem-optional-modules shm_iq_chan_gpu)
  if(ENABLE_TESTS)
    add_executable(test_iq_chan_gpu ${CMAKE_SOURCE_DIR}/radio/COMMON/tests/test_iq_chan_gpu.c)
    target_include_directories(test_iq_chan_gpu PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
    target_link_libraries(test_iq_chan_gpu PRIVATE mpci_iq nr_gpu_platform ${CMAKE_DL_LIBS} m pthread)
    set_target_properties(test_iq_chan_gpu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    add_dependencies(test_iq_chan_gpu shm_iq_chan_gpu)
    add_dependencies(tests test_iq_chan_gpu)
    add_test(NAME test_iq_chan_gpu COMMAND test_iq_chan_gpu WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    add_test(NAME test_iq_chan_gpu_discrete COMMAND test_iq_chan_gpu --discrete WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(test_iq_chan_gpu test_iq_chan_gpu_discrete PROPERTIES ENVIRONMENT "CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps"
                         SKIP_RETURN_CODE 77)
  endif()
endif()
if(ENABLE_TESTS)
  # in-context bench fixture: real-time file publisher (mpci_file_pub arguments) for 245.76 MS/s x 4 RX
  add_executable(g5_fast_pub ${CMAKE_SOURCE_DIR}/tests/passive_rx/mpci/g5_fast_pub.c)
  target_link_libraries(g5_fast_pub PRIVATE mpci_iq m pthread rt)
  add_dependencies(tests g5_fast_pub)
endif()
# ---- end G5 ----

# ---- LIVE ----
# MPCI-LIVE (A12): live cell changes. iq_live (decisions/order), ru_park (slot thread parked outside the radio gate, tuning mailbox, per-cell quiesce registry).
target_sources(mpci_iq PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON/iq_live.c ${CMAKE_SOURCE_DIR}/radio/COMMON/ru_park.c)
if(ENABLE_TESTS)
  # nr-ue-ru.c is compiled stand-alone by test_nr_shutdown: it needs the park module
  target_sources(test_nr_shutdown PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON/ru_park.c)
  add_executable(test_iq_live ${CMAKE_SOURCE_DIR}/radio/COMMON/tests/test_iq_live.c)
  target_include_directories(test_iq_live PRIVATE ${CMAKE_SOURCE_DIR}/radio/COMMON ${CMAKE_SOURCE_DIR}/openair1/PHY/TOOLS)
  set_target_properties(test_iq_live PROPERTIES ENABLE_EXPORTS ON)
  target_link_libraries(test_iq_live PRIVATE mpci_iq log_headers m dl pthread rt)
  add_dependencies(test_iq_live shm_iq)
  add_dependencies(tests test_iq_live)
  add_test(NAME test_iq_live COMMAND test_iq_live $<TARGET_FILE:shm_iq>)
endif()
# ---- end LIVE ----
