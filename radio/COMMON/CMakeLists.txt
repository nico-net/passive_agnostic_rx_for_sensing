# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(radio_common STATIC common_lib.c record_player.c shm_iq_pub.c shm_iq_ring.c)
target_include_directories(radio_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(radio_common PUBLIC dl log_headers shlib_loader)

# File-only passive raw-IQ playback; no UHD dependency or fallback.
add_library(raw_iq MODULE raw_iq.c raw_iq_source.c)
target_link_libraries(raw_iq PRIVATE radio_common m pthread)
set_target_properties(raw_iq PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# MPCI M1: shm IQ ring consumer device (one receiver process per cell; see shm_iq.c). iq_ddc at -O3 so its loops vectorise.
add_library(shm_iq MODULE shm_iq.c iq_ddc.c)
set_source_files_properties(iq_ddc.c PROPERTIES COMPILE_OPTIONS "-O3")
target_link_libraries(shm_iq PRIVATE radio_common m pthread rt)
set_target_properties(shm_iq PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(ENABLE_TESTS)
  add_executable(test_shm_iq tests/test_shm_iq.c shm_iq_ring.c shm_iq_pub.c iq_ddc.c)
  set_target_properties(test_shm_iq PROPERTIES ENABLE_EXPORTS ON) # exit_function for the dlopen'd device
  target_link_libraries(test_shm_iq PRIVATE log_headers m dl pthread)
  add_dependencies(test_shm_iq shm_iq)
  add_dependencies(tests test_shm_iq)
  add_test(NAME test_shm_iq COMMAND test_shm_iq $<TARGET_FILE:shm_iq>)
endif()

# MPCI wideband publisher: UHD-free recv -> ring step (radio/USRP/shm_iq_uhd_core.c), tested with a fake source
if(ENABLE_TESTS)
  add_executable(test_shm_iq_uhd_core ../USRP/tests/test_shm_iq_uhd_core.c ../USRP/shm_iq_uhd_core.c shm_iq_ring.c)
  target_include_directories(test_shm_iq_uhd_core PRIVATE ../USRP .)
  target_link_libraries(test_shm_iq_uhd_core PRIVATE rt)
  add_dependencies(tests test_shm_iq_uhd_core)
  add_test(NAME test_shm_iq_uhd_core COMMAND test_shm_iq_uhd_core)
endif()

# MPCI shared channelizer: one process mixes + decimates the wideband ring into one narrow ring per cell (<ring>_<tag>), see shm_iq_chan.c.
add_executable(shm_iq_chan shm_iq_chan.c shm_iq_ring.c iq_ddc.c)
set_source_files_properties(iq_ddc.c PROPERTIES COMPILE_OPTIONS "-O3")
target_link_libraries(shm_iq_chan PRIVATE m dl pthread rt)
set_target_properties(shm_iq_chan PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# GPU backend, dlopen'd by shm_iq_chan --gpu from next to the executable. Same CUDA pattern as td_cb0_gpu (ENABLE_LDPC_CUDA, arch list
# from LDPC_CUDA_ARCH / CMAKE_CUDA_ARCHITECTURES, e.g. 121 on the GB10); the default build never sees nvcc.
if(ENABLE_LDPC_CUDA)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  include(${PROJECT_SOURCE_DIR}/cmake_targets/nr_gpu_arch.cmake)
  nr_gpu_architectures("${LDPC_CUDA_ARCH}" SHM_IQ_CHAN_ARCHS) # LDPC_CUDA_ARCH: the cache list every GPU module follows (openair1/PHY/CODING)
  add_library(shm_iq_chan_gpu MODULE shm_iq_chan_gpu.cu)
  set_target_properties(shm_iq_chan_gpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" PREFIX "lib"
                        CUDA_ARCHITECTURES "${SHM_IQ_CHAN_ARCHS}" POSITION_INDEPENDENT_CODE ON CUDA_RUNTIME_LIBRARY Shared)
  target_include_directories(shm_iq_chan_gpu PRIVATE ${CUDAToolkit_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(shm_iq_chan_gpu PRIVATE CUDA::cudart m)
  add_dependencies(shm_iq_chan shm_iq_chan_gpu)
endif()

if(ENABLE_TESTS)
  add_executable(test_shm_iq_chan tests/test_shm_iq_chan.c shm_iq_ring.c)
  target_link_libraries(test_shm_iq_chan PRIVATE m rt)
  add_dependencies(test_shm_iq_chan shm_iq_chan)
  add_dependencies(tests test_shm_iq_chan)
  add_test(NAME test_shm_iq_chan COMMAND test_shm_iq_chan $<TARGET_FILE:shm_iq_chan>)
  add_test(NAME test_shm_iq_chan_selftest COMMAND shm_iq_chan --selftest --threads 2)
endif()

# Offline stand-in for shm_iq_uhd_pub: publishes a recorded raw IQ capture into a ring (DGX validation of the fan-out without a radio).
if(ENABLE_TESTS)
  add_executable(shm_iq_file_pub ${CMAKE_SOURCE_DIR}/tests/passive_rx/mpci/shm_iq_file_pub.c shm_iq_ring.c)
  target_include_directories(shm_iq_file_pub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(shm_iq_file_pub PRIVATE rt)
  set_target_properties(shm_iq_file_pub PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  add_dependencies(tests shm_iq_file_pub)
endif()
