# LOCKHOLD instrumentation (twomodes/collapse)
Files: openair1/PHY/NR_UE_TRANSPORT/nr_lockhold.{h,c} (pure ring/rate-limit/summary in the header), nr_pdcch_blind_phase2.{c,h} (g_phase2_mu; site = caller __func__:__LINE__ via macro), nr_pdcch_ul_layout_lock.c (g_mu, all sites), nr_pdcch_blind_monitor_rt.c (disc_worker splits), tests/test_nr_lockhold.c, CMakeLists.txt, tools/mpci/mpci_statics.tsv (2 rows).
Mechanism: lock = trylock + 1 clock_gettime (a blocked lock adds a second clock for the wait); unlock = 1 clock_gettime. Thread name (prctl PR_GET_NAME) and TID only when hold > 5 ms. Log after unlock; 16-entry ring per lock per cell, <=1 line/s (suppressed=N carried), 10 s summary only when count > 0.
Cost [MEASURED, DGX GB10, -O2 microbench, uncontended]: plain 7-11 ns, instrumented 102-105 ns per lock+unlock pair (about +95 ns).
Lines (LOG_W PHY):
SENSING: LOCKHOLD cell=%d lock=phase2|ullayout holder=<thread> tid=%d site=<func>:<line> hold_ms=%.2f wait_ms=%.2f suppressed=%u [note_pci_ms= step_ex_ms= poll_ms= snapshot_ms=]
SENSING: LOCKHOLD_SUMMARY cell=%d lock=... window_s=10 holds_gt5ms=%u max_hold_ms=%.2f max_wait_ms=%.2f by=<thread>@<func>:<line>=<n>,...
wait_ms = holder's own queueing time before acquiring; summary max_wait covers all acquisitions in the window.
Verification: 0 build failures, 25 .so entries (24 + liboai_device.so link), ctest failures all in baseline list, test_nr_lockhold passes, static audit 0, frozen check 0.
