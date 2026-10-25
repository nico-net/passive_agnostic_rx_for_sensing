# Multi-PCI seam owned by the cloud task group C (plan 2026-10-23-mpci-main-plan.md); targets for cell state, pool fair share, transport.

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
  add_test(NAME test_iq_chan_soak5 COMMAND nice -n 19 test_iq_chan --soak 5) # the 60 s run is the STOP evidence, run by hand
endif()
# ---- end C1C2 ----
