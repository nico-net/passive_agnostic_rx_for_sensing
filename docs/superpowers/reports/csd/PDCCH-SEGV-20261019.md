# PDCCH-thread SIGSEGV, 2026-10-19: root cause and fix

Branch csd/int16-merge19, base fff35b2d1d. Evidence labels follow PROJECT_MEMORY §0.1.

## Summary

The process is not killed by a memory bug in the PDCCH code. It is killed by the SLOWPASS_BT diagnostic. That diagnostic is on by default: `ISAC_SCAN2_SLOWPASS_MS` defaults to 8 ms. Its watchdog sends SIGUSR2 to a scan consumer (passivePdcch*) whose pass runs longer than 8 ms, and the handler calls `backtrace()`.

When the signal lands while `nr_polar_dci_decode_fssc()` (the fast-SSC DCI decoder merged in d79b8b7bc5) is on the stack, libgcc unwinds through a frame that has SVE stack slots. It reads stack data as the return address and faults on it. The SIGSEGV handler then calls `backtrace()` again, faults a second time with SIGSEGV blocked, and the kernel kills the process (RX_EXIT=139).

## Backtrace mapping

All four on-air crashes have the same signature, each with zero frames printed [MEASURED, run.log]:

| run | build | log tail |
|---|---|---|
| ab_uuTM2_1_134923 (TIM) | d79b8b7bc5 | `SLOWPASS_BT thread passivePdcch1`, then `ISAC_SEGV_BT signal 11 addr 0x62ff9affa3ffb2 thread passivePdcch1` |
| ab_dgVR1_1_150847 (Vodafone) | 66a913319f | `SLOWPASS_BT thread passivePdcch0`, then `ISAC_SEGV_BT ... addr 0x52ff8cff7bff74 thread passivePdcch0` |
| ab_gsV1_1_173107 (Vodafone) | HEAD binary (16:43) | `SLOWPASS_BT thread passivePdcch0`, then `ISAC_SEGV_BT ... addr 0xffc2017afff1ffad thread passivePdcch0` |
| ab_gsV2_1_173602 (Vodafone) | HEAD binary | `SLOWPASS_BT thread passivePdcch1`, then `ISAC_SEGV_BT ... addr 0xffffffd6fff4000e thread passivePdcch1` |

- **Same thread, no frames.** In each run the SEGV comes on the same thread, straight after its SLOWPASS_BT header and before any frame of either backtrace. The fault is therefore inside the handler's `backtrace()` [MEASURED].
- **Why only passivePdcch threads.** The SLOWPASS watchdog signals only scan consumers [code: `nr_pdcch_blind_monitor_rt.c` slowpass_enter/watchdog].
- **The fault addresses are four int16 values.** For example, 0x0062,0xff9a,0xffa3,0xffb2 = 98,-102,-93,-78. They are soft-bit data read as a return address [MEASURED pattern; meaning INFERRED, then reproduced below].
- **The other gsV1 backtraces** are the earlier, successful SLOWPASS_BT backtraces of passivePdcch1, mapped on the HEAD binary with addr2line at pc-4:
  - `slowpass_handler` rt.c:5740
  - `__kernel_rt_sigreturn`
  - `pthread_mutex_lock`
  - `nr_pdcch_blind_monitor_run_occasion_one` rt.c:7516 (`pthread_mutex_lock(&ul_length_lock)`)
  - `nr_pdcch_blind_monitor_run_occasion_early` rt.c:5410
  - `nr_pdcch_passive_queue_thread` nr_pdcch_passive_queue.c:250

  This thread was blocked more than 8 ms on the UL length lock while the other consumer ran the pass. The crashing thread's own stack was never printed. 28 of the 30 passivePdcch SLOWPASS samples from the HEAD binary are mutex waits [MEASURED].
- **Code identity.** uuTM2 and dgVR1 come from older builds, but the code involved (`slowpass_handler`, `isac_segv_bt`, `nr_polar_fssc.c`) is identical across d79b8b7bc5, 66a913319f and HEAD (git diff empty for these). The same mechanism is plausible for those two runs [INFERRED].

## Cause

- **SVE frames in the binary.** GCC 13.3 with `-mcpu=native` (Grace, SVE2) gives `nr_polar_dci_decode_fssc` scalable stack slots. Its CFA is a DWARF expression over the VG register (`DW_OP_breg31 sp; DW_OP_bregx 46 (vg); lit8; mul; plus_uconst 1072; plus`) [MEASURED, readelf]. Only 4 FDEs in nr-uesoftmodem use this pattern:
  - `nr_polar_dci_decode_fssc` (new in d79b8b7bc5)
  - `nr_pdcch_blind_monitor_autodiscover_step`
  - `score_candidate`
  - `lp_tab_build`
- **The unwind breaks at the fssc frame.** In the reproducer, a per-frame `_Unwind_Backtrace` log shows the walk is sane up to the `nr_polar_dci_decode_fssc` frame. The next return address is then int16 data, and libgcc faults on it, for example at `0xffad003a001fffe0` [MEASURED, 3 of 3 samples].
- **Not fully explained.** Exactly why libgcc_s 14.2 computes this CFA wrong is not established [INFERRED: VG-based CFA expression].
- **Why it appeared with the merge.** d79b8b7bc5 made the fast-SSC decoder the PDCCH polar decoder, so the scan threads now spend much of each pass inside an SVE frame.
- **Not the cause.** I reviewed the suspects listed in the brief and did not find this crash's cause there: the x8 batch buffers, the early-start queue, the spin hand-off, the fast-lane ordering, the DCIRV ring and the monitor's layout parse. The crash signature does not point to them [INFERRED, code review only].

## Fix

- **New header `openair1/PHY/NR_UE_TRANSPORT/nr_safe_backtrace.h`.** `nr_safe_backtrace()` wraps `backtrace()` in `sigsetjmp`. `nr_safe_backtrace_on_fault()`, called first in the SIGSEGV/SIGBUS handler, `siglongjmp`s out of a faulting unwind on that thread.
- **`slowpass_handler`** uses it. When an unwind faults, it prints `SLOWPASS_BT unwind faulted, skipped` instead of dying.
- **`isac_segv_bt`** calls the hook first. Genuine faults behave as before.
- **No other behaviour changes:** no new env flags, decoder and flags untouched.

## Test

`test_nr_polar_fssc`, new case `PolarFssc.SignalBacktraceDuringDecodeSurvives`. It runs as a death test in a child process: one thread decodes DCIs with `nr_polar_dci_decode_fssc` for 4 s while the main thread sends SIGUSR2 every 20 us, and the handler backtraces.

- **Before (plain `backtrace()`):** the child dies with SIGSEGV within 2 ms, in 5 of 5 runs. The fault addresses are int16 patterns, the same signature as on air [MEASURED, BENCH/unit DGX].
- **After:** the test passes in 3 of 3 runs. One run logged ok=340 and faulted=51920 unwinds, all survived [MEASURED, BENCH/unit DGX]. About 99 % of unwinds taken mid-decode fault, so in production almost any SLOWPASS signal that lands in the fssc decoder was fatal.

Build `ninja -j6 -k 0 nr-uesoftmodem oai_usrpdevif tests`: only the known link failures (test_nr_csi_rb_extent, test_nr_ssb_rate_match_prod, nr_pusch_ra0sim). ctest -j4: the only failures are on the known list: 9 test_nr_pusch_ra0_* (Not Run, link), test_nr_ssb_rate_match_prod and test_nr_csi_rb_extent (Not Run, link), nr_td_sim_test, dft_test and test_nr_modulation. test_nr_pdcch_blind_monitor ran 298 cases: 282 passed, 16 skipped, 0 failed. test_nr_polar_fssc passed (4 cases) [MEASURED, BENCH/ctest unit, DGX].

## Not done

- No OTA or replay validation (by brief).
- The bad CFA evaluation is worked around, not fixed in the toolchain.
- SLOWPASS samples that fault are dropped, not printed.
