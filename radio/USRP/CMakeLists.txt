# SPDX-License-Identifier: LicenseRef-CSSL-1.0

add_compile_options(-Wunused-parameter)
#find_package(Boost REQUIRED)
find_package(UHD REQUIRED UHD)
# usrp_lib.cpp calls boost::this_thread::sleep; a UHD build that does not itself link
# libboost_thread (UHD 4.11 + DPDK on the DGX) leaves that symbol unresolved at dlopen time.
find_package(Boost REQUIRED COMPONENTS thread)

add_library(oai_usrpdevif MODULE usrp_lib.cpp)
#target_include_directories(oai_usrpdevif PRIVATE Boost::boost)
target_link_libraries(oai_usrpdevif PRIVATE ${UHD_LIBRARIES} Boost::thread)
target_include_directories(oai_usrpdevif PRIVATE ${UHD_INCLUDE_DIRS})
target_link_libraries(oai_usrpdevif PRIVATE log_headers)
set_target_properties(oai_usrpdevif PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_custom_command(TARGET oai_usrpdevif POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink liboai_usrpdevif.so liboai_device.so
                   WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# MPCI wideband: standalone UHD -> shm IQ ring publisher (no OAI softmodem/config/log dependency)
add_executable(shm_iq_uhd_pub shm_iq_uhd_pub.cpp shm_iq_uhd_core.c ../COMMON/shm_iq_ring.c)
target_include_directories(shm_iq_uhd_pub PRIVATE ${UHD_INCLUDE_DIRS} ../COMMON)
target_link_libraries(shm_iq_uhd_pub PRIVATE ${UHD_LIBRARIES} Boost::thread rt pthread)
set_target_properties(shm_iq_uhd_pub PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
