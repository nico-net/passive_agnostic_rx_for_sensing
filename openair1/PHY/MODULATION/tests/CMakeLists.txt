# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_executable(test_nr_modulation test_nr_modulation.cpp)
target_link_libraries(test_nr_modulation PRIVATE PHY_NR GTest::gtest minimal_lib)
add_dependencies(tests test_nr_modulation)
add_test(NAME test_nr_modulation
  COMMAND ./test_nr_modulation)

if(ENABLE_LDPC_CUDA)
  # ULFEP: UL GPU FEP vs the CPU receiver, bit exact (needs libpdsch_gpu.so and a CUDA device; skips without)
  add_executable(test_nr_pusch_gpu_fep test_nr_pusch_gpu_fep.cpp ${OPENAIR1_DIR}/PHY/TOOLS/dfts_load.c)
  target_include_directories(test_nr_pusch_gpu_fep PRIVATE ${OPENAIR_DIR} ${OPENAIR1_DIR})
  target_link_libraries(test_nr_pusch_gpu_fep PRIVATE PHY_NR GTest::gtest minimal_lib shlib_loader ${CMAKE_DL_LIBS} m)
  set_target_properties(test_nr_pusch_gpu_fep PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(test_nr_pusch_gpu_fep pdsch_gpu dfts)
  add_dependencies(tests test_nr_pusch_gpu_fep)
  add_test(NAME test_nr_pusch_gpu_fep COMMAND ./test_nr_pusch_gpu_fep WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_nr_pusch_gpu_fep PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR};CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps")
  # Q4: the DISCRETE tier runs the staged variant (no skip); only the CPU tier skips (gtest skips exit 0, report it as Skipped)
  set_tests_properties(test_nr_pusch_gpu_fep PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] UlGpuFep.BitExactVsCpu")
  add_test(NAME test_nr_pusch_gpu_fep_discrete COMMAND ./test_nr_pusch_gpu_fep WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_nr_pusch_gpu_fep_discrete PROPERTIES ENVIRONMENT "NR_GPU_FORCE_MODEL=discrete;LD_LIBRARY_PATH=${CMAKE_BINARY_DIR};CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps" SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] UlGpuFep.BitExactVsCpu")
  add_test(NAME test_nr_pusch_gpu_fep_cpu COMMAND ./test_nr_pusch_gpu_fep WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(test_nr_pusch_gpu_fep_cpu PROPERTIES ENVIRONMENT "NR_GPU_FORCE_MODEL=cpu;LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}" SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] UlGpuFep.BitExactVsCpu")
  # GPU FEP at every FFT size of the receiver's cells (sens6 per-path): bit-exact vs the CPU FEP where the host's twin exists (x86: powers of two,
  # aarch64: 4096), refused (CPU FEP) at the 3/4-sampling sizes. One size per process (the module initialises once).
  add_executable(test_nr_pdsch_gpu_exact_sizes test_nr_pdsch_gpu_exact_sizes.cpp ${OPENAIR1_DIR}/PHY/TOOLS/dfts_load.c)
  target_include_directories(test_nr_pdsch_gpu_exact_sizes PRIVATE ${OPENAIR_DIR} ${OPENAIR1_DIR})
  target_link_libraries(test_nr_pdsch_gpu_exact_sizes PRIVATE PHY_NR GTest::gtest minimal_lib shlib_loader ${CMAKE_DL_LIBS} m)
  set_target_properties(test_nr_pdsch_gpu_exact_sizes PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  add_dependencies(test_nr_pdsch_gpu_exact_sizes pdsch_gpu dfts)
  add_dependencies(tests test_nr_pdsch_gpu_exact_sizes)
  foreach(fft_n 512 768 1024 1536 2048 3072 4096)
    add_test(NAME test_nr_pdsch_gpu_exact_size_${fft_n} COMMAND ./test_nr_pdsch_gpu_exact_sizes WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(test_nr_pdsch_gpu_exact_size_${fft_n} PROPERTIES
      ENVIRONMENT "NR_TEST_FEP_N=${fft_n};LD_LIBRARY_PATH=${CMAKE_BINARY_DIR};CUDA_MPS_PIPE_DIRECTORY=/tmp/nicola_nomps;CUDA_MPS_LOG_DIRECTORY=/tmp/nicola_nomps"
      SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\] GpuExactFepSizes" TIMEOUT 120)
  endforeach()
endif()

# SFO: integer sin/cos and per-RE phasor accuracy, CPU table loop == per-RE (GPU) form bit for bit, correction undoes a fractional
# delay, eps from the timing-loop frame reads with its sign and hysteresis
add_executable(test_nr_sfo test_nr_sfo.cpp ${OPENAIR1_DIR}/PHY/MODULATION/nr_sfo_state.c)
target_include_directories(test_nr_sfo PRIVATE ${OPENAIR1_DIR}/PHY/MODULATION ${OPENAIR_DIR})
target_link_libraries(test_nr_sfo PRIVATE GTest::gtest minimal_lib m)
add_dependencies(tests test_nr_sfo)
add_test(NAME test_nr_sfo COMMAND ./test_nr_sfo)
