# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_executable(test_nr_ue_power_procedures
  test_nr_ue_power_procedures.cpp)
target_link_libraries(test_nr_ue_power_procedures PRIVATE nr_ue_power_procedures GTest::gtest minimal_lib)
add_dependencies(tests test_nr_ue_power_procedures)
add_test(NAME test_nr_ue_power_procedures
  COMMAND ./test_nr_ue_power_procedures)

add_executable(test_nr_ue_ra_procedures test_nr_ue_ra_procedures.cpp)
target_link_libraries(test_nr_ue_ra_procedures PRIVATE nr_ue_ra_procedures nr_ue_power_procedures GTest::gtest minimal_lib MAC_UE_NR)
add_dependencies(tests test_nr_ue_ra_procedures)
add_test(NAME test_nr_ue_ra_procedures
  COMMAND ./test_nr_ue_ra_procedures)

# MIB dmrs-TypeA-Position -> blind PDCCH monitor handoff on a no-CORESET#0 (NSA, FR1 kSSB >= 24) cell.
# Links the REAL blind monitor (not the link-only stubs test_nr_ue_ra_procedures uses): the monitor's
# configured state is what the test asserts. dci_nr.c/refsig.c/nr_dmrs_rx.c/hashtable.c complete the
# library's PHY references, exactly as for test_nr_pdcch_blind_monitor.
add_executable(test_nr_ue_mib_blind_handoff
  test_nr_ue_mib_blind_handoff.cpp
  ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT/dci_nr.c
  ${OPENAIR1_DIR}/PHY/NR_REFSIG/refsig.c
  ${OPENAIR1_DIR}/PHY/NR_REFSIG/nr_dmrs_rx.c
  ${OPENAIR_DIR}/common/utils/ds/hashtable.c)
target_include_directories(test_nr_ue_mib_blind_handoff PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_TRANSPORT ${OPENAIR_DIR}/common/utils/ds)
target_link_libraries(test_nr_ue_mib_blind_handoff PRIVATE
  -Wl,--start-group MAC_UE_NR nr_pdcch_blind_monitor -Wl,--end-group
  nr_ue_ra_procedures nr_ue_power_procedures nr_common MAC_NR_COMMON polar crc_byte UTIL
  asn1_nr_rrc_hdrs asn1_lte_rrc_hdrs GTest::gtest minimal_lib)
add_dependencies(tests test_nr_ue_mib_blind_handoff)
add_test(NAME test_nr_ue_mib_blind_handoff
  COMMAND ./test_nr_ue_mib_blind_handoff)
