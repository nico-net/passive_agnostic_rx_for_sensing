# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(coding MODULE
            3gpplte_sse.c
            3gpplte.c
            3gpplte_turbo_decoder_sse_8bit.c
            3gpplte_turbo_decoder_sse_16bit.c
            3gpplte_turbo_decoder_avx2_16bit.c
            3gpplte_turbo_decoder.c
)
set_target_properties(coding PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(coding PRIVATE log_headers)

add_library(ldpc_orig MODULE
            nrLDPC_decoder/nrLDPC_decoder.c
            nrLDPC_encoder/ldpc_encoder.c
)
set_target_properties(ldpc_orig PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(ldpc_orig PRIVATE ldpc_segment)

add_library(ldpc MODULE
            nrLDPC_decoder/nrLDPC_decoder.c
            nrLDPC_encoder/ldpc_encoder_optim8segmulti.c
)
set_target_properties(ldpc PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(ldpc PRIVATE ldpc_segment)

# GPU LDPC decoder (nrLDPC_cuda/README.md). Off by default; load with --loader.ldpc.shlibversion _cuda.
option(ENABLE_LDPC_CUDA "Build libldpc_cuda.so (CUDA LDPC decoder)" OFF)
if(ENABLE_LDPC_CUDA)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  # One architecture or a list, e.g. -DLDPC_CUDA_ARCH="89;121" (sens6 RTX 4070 + DGX GB10 in one fat binary), or per
  # host (89 on sens6, 121 on the DGX). Every CUDA target below (LDPC, CB0 entry, PDSCH/PDCCH GPU, polar) uses it.
  set(LDPC_CUDA_ARCH "89" CACHE STRING "CUDA architecture(s) for the GPU modules: a number or a ;-list (89;121)")
  foreach(_arch IN LISTS LDPC_CUDA_ARCH)
    if(NOT _arch MATCHES "^[0-9]+[af]?(-real|-virtual)?$")
      message(FATAL_ERROR "LDPC_CUDA_ARCH: '${_arch}' is not a CUDA architecture number (e.g. 89, 121, \"89;121\")")
    endif()
  endforeach()
  message(STATUS "GPU modules: CUDA architectures ${LDPC_CUDA_ARCH}")
  # Own TB decoder (GPU batch per TB) instead of ldpc_segment's per-segment one: the two define the
  # same nrLDPC_coding_* symbols, so the segment encoder and rate matching are compiled in directly.
  add_library(ldpc_cuda MODULE
              nrLDPC_encoder/ldpc_encoder_optim8segmulti.c
              nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_encoder.c
              nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.c
              nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c
              nrLDPC_cuda/nr_ldpc_cuda.c
              nrLDPC_cuda/ldpc_decoder.cu
              nrLDPC_cuda/ldpc_cb0.cu
              nrLDPC_cuda/nrLDPC_cpu_fallback.c
  )
  set_target_properties(ldpc_cuda PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                        CUDA_ARCHITECTURES "${LDPC_CUDA_ARCH}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(ldpc_cuda PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  target_compile_definitions(ldpc_cuda PRIVATE LDPC_CUDA_DISCRETE LDPC_DECODER_NAME="CUDA LDPC decoder")
  target_link_libraries(ldpc_cuda PRIVATE log_headers CUDA::cudart)
  add_dependencies(ldpctest ldpc_cuda)
  if(ENABLE_TESTS)
    # K34 pool-safety tests: need a CUDA device; dlopen ./libldpc_cuda.so, resolving its symbols from the test binary.
    add_executable(test_ldpc_cuda_pool nrLDPC_cuda/tests/ldpc_cuda_pool_test.cc nrLDPC_cuda/tests/ldpc_cuda_pool_test_helper.c)
    target_include_directories(test_ldpc_cuda_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nrLDPC_cuda/tests ${OPENAIR_DIR} ${OPENAIR1_DIR})
    target_link_libraries(test_ldpc_cuda_pool PRIVATE GTest::gtest GTest::gtest_main thread-pool LOG minimal_lib crc_byte UTIL ${CMAKE_DL_LIBS} pthread)
    set_target_properties(test_ldpc_cuda_pool PROPERTIES ENABLE_EXPORTS ON RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    # the plugin takes these from the host executable (as from nr-uesoftmodem): force them into the test binary
    foreach(sym check_crc crc24a crc24b crc16 crcTableInit completed_many_task_ans cpu_meas_enabled exit_function g_log logRecord_mt init_task_ans join_task_ans pushTpool T_active T_cache T_freelist_head T_stdout)
      target_link_options(test_ldpc_cuda_pool PRIVATE "LINKER:--undefined=${sym}")
    endforeach()
    add_dependencies(test_ldpc_cuda_pool ldpc_cuda)
    # BLER through the pool path (nrLDPC_coding_decoder), CUDA and forced CPU fallback: a measurement tool, not a ctest
    add_executable(ldpc_cuda_pool_bler nrLDPC_cuda/tests/ldpc_cuda_pool_bler.c nrLDPC_cuda/tests/ldpc_cuda_pool_test_helper.c)
    target_include_directories(ldpc_cuda_pool_bler PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nrLDPC_cuda/tests ${OPENAIR_DIR} ${OPENAIR1_DIR})
    target_link_libraries(ldpc_cuda_pool_bler PRIVATE thread-pool LOG minimal_lib crc_byte UTIL ${CMAKE_DL_LIBS} pthread m)
    set_target_properties(ldpc_cuda_pool_bler PROPERTIES ENABLE_EXPORTS ON RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    foreach(sym check_crc crc24a crc24b crc16 crcTableInit completed_many_task_ans cpu_meas_enabled exit_function g_log logRecord_mt init_task_ans join_task_ans pushTpool T_active T_cache T_freelist_head T_stdout)
      target_link_options(ldpc_cuda_pool_bler PRIVATE "LINKER:--undefined=${sym}")
    endforeach()
    add_dependencies(ldpc_cuda_pool_bler ldpc_cuda ldpc)
    add_dependencies(tests test_ldpc_cuda_pool)
    add_test(NAME test_ldpc_cuda_pool COMMAND ./test_ldpc_cuda_pool WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    # CB0 batch entry (nrLDPC_cb0_cuda.h): bit-exact vs a CPU model and G1's pool, CRC vs check_crc, CPU TB decoder
    # verdicts (./libldpc.so), batch sizes, async API, fault injection, forced explicit-copy path. Needs a CUDA device.
    add_executable(test_ldpc_cb0_cuda nrLDPC_cuda/tests/ldpc_cb0_cuda_test.cc nrLDPC_cuda/tests/ldpc_cuda_pool_test_helper.c)
    target_include_directories(test_ldpc_cb0_cuda PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/nrLDPC_cuda/tests ${OPENAIR_DIR} ${OPENAIR1_DIR}
                               ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(test_ldpc_cb0_cuda PRIVATE GTest::gtest GTest::gtest_main thread-pool LOG minimal_lib crc_byte UTIL
                          CUDA::cudart ${CMAKE_DL_LIBS} pthread m)
    set_target_properties(test_ldpc_cb0_cuda PROPERTIES ENABLE_EXPORTS ON RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
    foreach(sym check_crc crc24a crc24b crc16 crcTableInit completed_many_task_ans cpu_meas_enabled exit_function g_log logRecord_mt init_task_ans join_task_ans pushTpool T_active T_cache T_freelist_head T_stdout)
      target_link_options(test_ldpc_cb0_cuda PRIVATE "LINKER:--undefined=${sym}")
    endforeach()
    add_dependencies(test_ldpc_cb0_cuda ldpc_cuda ldpc)
    add_dependencies(tests test_ldpc_cb0_cuda)
    add_test(NAME test_ldpc_cb0_cuda COMMAND ./test_ldpc_cb0_cuda WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  endif()
  add_dependencies(nr-uesoftmodem ldpc_cuda)
  # GPU FEP + chest + MMSE + LLR for the passive PDSCH decoder (libpdsch_gpu.so, dlopen'd when
  # NR_GPU_FEP=1). Lives in NR_UE_TRANSPORT; built here because this is where CUDA is enabled.
  add_library(pdsch_gpu MODULE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT/nr_pdsch_gpu_fep.cu)
  set_target_properties(pdsch_gpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                        CUDA_ARCHITECTURES "${LDPC_CUDA_ARCH}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(pdsch_gpu PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  target_link_libraries(pdsch_gpu PRIVATE CUDA::cudart CUDA::cufft)
  add_executable(nr_pdsch_gpu_fep_test ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT/tests/nr_pdsch_gpu_fep_test.cc)
  target_include_directories(nr_pdsch_gpu_fep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT)
  target_link_libraries(nr_pdsch_gpu_fep_test PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(nr_pdsch_gpu_fep_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(nr_pdsch_gpu_fep_test pdsch_gpu)
  add_dependencies(nr-uesoftmodem pdsch_gpu)
  # GPU DM-RS chest + QPSK LLR for the blind PDCCH CORESET-extent search (libpdcch_gpu.so, same
  # NR_GPU_FEP=1 loader knob as pdsch_gpu above -- see nr_pdcch_gpu_fep.h). No cuFFT: FFT stays on
  # the CPU (nr_slot_fep), already shared across every extent candidate -- see that file's top
  # comment for why.
  add_library(pdcch_gpu MODULE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT/nr_pdcch_gpu_fep.cu)
  set_target_properties(pdcch_gpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                        CUDA_ARCHITECTURES "${LDPC_CUDA_ARCH}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(pdcch_gpu PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  target_link_libraries(pdcch_gpu PRIVATE CUDA::cudart)
  add_executable(nr_pdcch_gpu_fep_test ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT/tests/nr_pdcch_gpu_fep_test.cc)
  target_include_directories(nr_pdcch_gpu_fep_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT)
  target_link_libraries(nr_pdcch_gpu_fep_test PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(nr_pdcch_gpu_fep_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(nr_pdcch_gpu_fep_test pdcch_gpu)
  add_dependencies(nr-uesoftmodem pdcch_gpu)
  # Batched polar SC decode for the blind PDCCH search.
  add_library(polar_sc_cuda STATIC nrPolar_tools/cuda/nr_polar_sc_cuda.cu nrPolar_tools/cuda/nr_polar_sc_cuda_host.c)
  set_target_properties(polar_sc_cuda PROPERTIES CUDA_ARCHITECTURES "${LDPC_CUDA_ARCH}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(polar_sc_cuda PRIVATE ${CUDAToolkit_INCLUDE_DIRS} nrPolar_tools/cuda)
  target_link_libraries(polar_sc_cuda PRIVATE polar CUDA::cudart)
  add_executable(nr_polar_sc_cuda_test nrPolar_tools/cuda/nr_polar_sc_cuda_test.cc)
  target_include_directories(nr_polar_sc_cuda_test PRIVATE nrPolar_tools/cuda)
  target_link_libraries(nr_polar_sc_cuda_test PRIVATE polar_sc_cuda polar crc_byte UTIL CUDA::cudart m)
  set_target_properties(nr_polar_sc_cuda_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  # ... wired into the live candidate scan through libpolar_gpu.so, dlopen'd when NR_GPU_POLAR=1
  # (nr_polar_gpu.h). A MODULE, like pdsch_gpu/pdcch_gpu above, so nr-uesoftmodem itself keeps no
  # CUDA link dependency and a run without the knob is the CPU path.
  add_library(polar_gpu MODULE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT/nr_polar_gpu_mod.c)
  set_target_properties(polar_gpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                        POSITION_INDEPENDENT_CODE ON)
  target_include_directories(polar_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT
                             ${CMAKE_CURRENT_SOURCE_DIR}/nrPolar_tools/cuda ${CUDAToolkit_INCLUDE_DIRS})
  target_link_libraries(polar_gpu PRIVATE polar_sc_cuda polar crc_byte UTIL CUDA::cudart m)
  add_dependencies(nr-uesoftmodem polar_gpu)
endif()

add_dependencies(ldpctest ldpc ldpc_orig)
add_dependencies(nr-softmodem ldpc ldpc_orig)
add_dependencies(nr-uesoftmodem ldpc ldpc_orig)
add_dependencies(nr_ulsim ldpc ldpc_orig)
add_dependencies(nr_ulschsim ldpc ldpc_orig)
add_dependencies(nr_dlsim ldpc ldpc_orig)
add_dependencies(nr_dlschsim ldpc ldpc_orig)

add_library(crc_byte OBJECT crc_byte.c)

add_subdirectory(nrLDPC_coding)
add_subdirectory(nrPolar_tools)
add_subdirectory(nrSmallBlock)

# Batched CB0 rate de-matching on the GPU for the Technique D CB0 elimination channel (nr_td_cb0_batch.h):
# libtd_cb0_gpu.so, dlopen'd by nr_td_cb0_batch.c (NR_GPU_CB0=1 or nr_td_cb0_use_gpu(1)), so no CUDA link
# dependency anywhere else and the CPU path works without it.
if(ENABLE_LDPC_CUDA)
  add_library(td_cb0_gpu MODULE ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT/nr_td_cb0_batch.cu)
  set_target_properties(td_cb0_gpu PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                        CUDA_ARCHITECTURES "${LDPC_CUDA_ARCH}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(td_cb0_gpu PRIVATE ${CUDAToolkit_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/../NR_UE_TRANSPORT)
  target_link_libraries(td_cb0_gpu PRIVATE CUDA::cudart)
  add_dependencies(nr-uesoftmodem td_cb0_gpu)
  if(ENABLE_TESTS)
    add_dependencies(test_nr_td_cb0_batch td_cb0_gpu ldpc_cuda)
    add_dependencies(nr_td_cb0_bench td_cb0_gpu ldpc_cuda)
  endif()
endif()
