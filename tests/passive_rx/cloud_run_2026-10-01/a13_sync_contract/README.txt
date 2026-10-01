[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 03fb79aae3]

A13: tests/passive_rx/offline_sync_contract/build_and_run.sh made arch-aware.
Design: the 9 original x86 command lines are kept verbatim in the x86 branch; the aarch64
branch holds the offline_sync_arm.sh lines verbatim (REPO from BASH_SOURCE, CPM gtest include
via find ~/.cache/cpm -path '*googletest/include' | head -1, error if empty). Duplicated on
purpose: byte-identity beats DRY. On x86 only, if /usr/lib/x86_64-linux-gnu/libgtest.a is
absent, lib/libgtest.a is linked and -I<cpm include> is added to the c++ test compile.

Method: /usr/bin/cc, /usr/bin/c++, objcopy and the test binary replaced with echo in temp
copies; whitespace normalized, mktemp path normalized; outputs diffed.
(a) OLD (bfe4b67c60~1) vs NEW, with a temporary fake system libgtest.a symlink: diff EMPTY.
    (symlink removed afterwards)
(b) OLD vs NEW without it: only differences are the added -I<cpm googletest include> on the
    c++ test compile and libgtest token /usr/lib/x86_64-linux-gnu/libgtest.a -> lib/libgtest.a.
(c) NEW with uname -m forced to aarch64 vs echo-transformed offline_sync_arm.sh (REPO mapped
    to the worktree, CPM path mapped to /root/.cache/cpm): diff EMPTY.

Real run on this host (x86 fallback path), bash build_and_run.sh:
[       OK ] OfflineSync.RealFepCfoSign (78 ms)
[       OK ] OfflineSync.RealEqualizerSfoOriginAndSign (62 ms)
[       OK ] OfflineSync.RealFepFeedbackLaw (10995 ms)
[       OK ] OfflineSync.JointCfoSfoSeparation (43 ms)
[       OK ] OfflineSync.RealFepDeferredCfoSnapshotSurvivesWorkerDispatch (7 ms)
[  PASSED  ] 5 tests.

Caveat: the aarch64 branch is verified ONLY by the echo diff (c). A real aarch64 run is a
DGX follow-up. The x86 system-gtest path is verified only by the echo diff (a) (no system
libgtest.a on this host).
