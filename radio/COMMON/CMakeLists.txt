# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_library(radio_common STATIC common_lib.c record_player.c)
target_include_directories(radio_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(radio_common PUBLIC dl log_headers shlib_loader)

# File-only passive raw-IQ playback; no UHD dependency or fallback.
add_library(raw_iq MODULE raw_iq.c raw_iq_source.c)
target_link_libraries(raw_iq PRIVATE radio_common m pthread)
set_target_properties(raw_iq PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
