#!/bin/bash
# CMAKE_C_COMPILER_LAUNCHER for the PARTIAL TSAN build (A7 method): adds -fsanitize=thread only when the
# translation unit is one of the blind-PDCCH / passive-PDSCH scan sources (plus task_ans.c, whose atomic
# completion counter must be instrumented for TSAN to see the LDPC pool -> joiner ordering). Everything
# else is compiled exactly as in a plain build, so the receiver runs near real time and reaches TRACKING.
# Link with -fsanitize=thread (CMAKE_EXE_LINKER_FLAGS / CMAKE_SHARED_LINKER_FLAGS).
# usage: cmake ... -DCCACHE_FOUND=$PWD/tsan_cc_launcher.sh   (the root CMakeLists.txt sets CMAKE_C/CXX_COMPILER_LAUNCHER
#        from CCACHE_FOUND whenever ccache is installed, overriding -DCMAKE_C_COMPILER_LAUNCHER; on a host without
#        ccache -DCMAKE_C_COMPILER_LAUNCHER=$PWD/tsan_cc_launcher.sh works too). Check: nm <obj> | grep -c __tsan_
src=""
for a in "$@"; do case "$a" in *.c) src="$a" ;; esac; done
case "$src" in
  */NR_UE_TRANSPORT/nr_pdcch_blind_*.c | */NR_UE_TRANSPORT/nr_pdcch_passive_queue.c | \
  */NR_UE_TRANSPORT/nr_pdsch_passive_*.c | */NR_UE_TRANSPORT/nr_pdcch_coreset_*.c | \
  */NR_UE_TRANSPORT/dci_nr.c | */threadPool/task_ans.c)
    exec "$@" -fsanitize=thread ;;
esac
exec "$@"
