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
  set(LDPC_CUDA_ARCH "89" CACHE STRING "CUDA architecture for libldpc_cuda.so")
  # Own TB decoder (GPU batch per TB) instead of ldpc_segment's per-segment one: the two define the
  # same nrLDPC_coding_* symbols, so the segment encoder and rate matching are compiled in directly.
  add_library(ldpc_cuda MODULE
              nrLDPC_encoder/ldpc_encoder_optim8segmulti.c
              nrLDPC_coding/nrLDPC_coding_segment/nrLDPC_coding_segment_encoder.c
              nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.c
              nrLDPC_cuda/nrLDPC_coding_cuda_decoder.c
              nrLDPC_cuda/nr_ldpc_cuda.c
              nrLDPC_cuda/ldpc_decoder.cu
  )
  set_target_properties(ldpc_cuda PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
                        CUDA_ARCHITECTURES "${LDPC_CUDA_ARCH}" POSITION_INDEPENDENT_CODE ON)
  target_include_directories(ldpc_cuda PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
  target_compile_definitions(ldpc_cuda PRIVATE LDPC_CUDA_DISCRETE LDPC_DECODER_NAME="CUDA LDPC decoder")
  target_link_libraries(ldpc_cuda PRIVATE log_headers CUDA::cudart)
  add_dependencies(ldpctest ldpc_cuda)
  add_dependencies(nr-uesoftmodem ldpc_cuda)
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
