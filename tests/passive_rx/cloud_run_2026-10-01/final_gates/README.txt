Track-A final gates (A14 Step 4, adapted to the cloud host), code at 8c1434df22 (= final-review fixes + base merge).
[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 8c1434df22] ctest -j4: 126/129; failures env-only (test_thread-pool:
  pthread_getaffinity_np EINVAL in container; nr_cuup_functional_test: no SCTP; time_management_tests: intermittent timing).
  test_nr_pdcch_blind_monitor --gtest_shuffle seeds 1/3/5: 197 pass + 2 skips each (195 + 2 new A7 tests).
[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, 8c1434df22] rfsim_regress.sh 3 (106 PRB, cloud gate crc>=93.0, drop<=2.5):
  PASS 3/3 - crc 95.33 / 96.18 / 95.68 %, drop_full 1.50 / 0.87 / 1.46 %, CONVERGED 2/2 each, ttc 3.78 / 4.10 / 4.18 s,
  CPU ~200 %, RSS ~0.89 GB. HEAD baseline (be2e7fa4b6 code): crc 94.93 / 96.89 / 95.19, drop 1.17 / 1.55 / 0.77 -> within spread.
sens6 frozen gate: git diff --quiet sens6-frozen-2026-09-30 -- captures, *.conf, sens6 snapshot -> 0.
ENABLE_ISAC_SENSING=OFF: nr-uesoftmodem links (build_off, after A3 and again by the final reviewer at HEAD).
