[OFFLINE VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, A7-follow-up commit (parent = merge of cloud/dgx-next-steps b6e5fb27ac)] -- gtest
[SIM VERIFIED, cloud x86 Xeon-2.8GHz-4c, 2026-10-01, same commit] -- rfsim partial-TSAN oracle, regression gate

A7 follow-up: the two race groups A7 left (../a7_concurrency/README.md section 2, "REMAINING in round 2"), both
present with ONE scan consumer too, so they predate N>1.

## What was racing and the fix
1. UEthread_0 (PHY receive thread) <-> passivePdcchN, nr_pdcch_blind_monitor.c (13 reports in A7 round 2).
   Technique A (observe_symbol1()/autodiscover_step(), receive thread, every DL slot before discovery) fills
   s_hit_count/s_hit_count1/s_obs_calls and folds s_lt_*; consumers touch the same arrays in
   note_rnti_for_windows() (every C/TC accept) and autodiscover_next()/reset(). Worse (not yet reported because it
   fires once per epoch): the footprint COMMIT ran on the receive thread and rewrote g_cfg geometry and the
   extent/map/lane cursors that a consumer reads and advances inside its discovery occasion under g_phase2_mu.
   - s_techA_mu: a LEAF mutex held only around the array touches (no I/O, no other lock inside). The receive
     thread accumulates + snapshots under it and evaluates the dwell on the snapshot; never takes g_phase2_mu.
   - Deferred commit: once the scan pool runs (rt.c sets nr_pdcch_blind_monitor_autodiscover_defer_commit(true)
     after nr_pdcch_passive_queue_start()), autodiscover_step() only POSTS the decision (catalogue, duration
     evidence, pci/symbol/slot); run_occasion() applies it on a consumer under g_phase2_mu
     (nr_pdcch_blind_monitor_autodiscover_apply_pending()) before any pass reads the root geometry. The receive
     thread skips Technique A while a decision is pending. No pool (in-line scan, unit tests) = applied at once,
     exactly as before.
   - SS occasion gate: the receive thread now reads ss_monitoring_slot_periodicity/offset/ss_duration through
     nr_pdcch_blind_monitor_occasion_gate(), one atomic word published by every writer (parse, CSS0 autoconf on
     the MAC thread, the commit on a consumer) -- otherwise moving the commit to a consumer would race the gate.
   - s_disc_paused (read by the receive thread's discovery gate) is _Atomic.
2. passivePdsch pool.
   - nr_pdsch_passive_queue_thread(): lazily resolved getenv statics (s_probe_all_g ~687, s_probe_all ~882,
     s_k0_probe_on, s_chk) are _Atomic.
   - passivePdsch1 <-> Tpool worker (memcpy in passive_ldpc_decode_core() reassembly vs
     nr_process_decode_segment()): a real ordering gap in common/utils/threadPool/task_ans.c (UPSTREAM OAI file):
     completed_many_task_ans() decremented the counter with memory_order_relaxed and only the LAST completer posts
     the semaphore, so the joiner synchronised with that worker alone; the other workers' segment writes were
     unordered (harmless on x86 TSO, a real hazard on the DGX's aarch64). Now acq_rel.
   - Same pattern surfaced in nr_pdsch_passive_decode.c once the queue statics were fixed (rounds 1-2 below):
     every lazily resolved env static (20 + g_pdtim_on + s_ptrs_k/l, L stored before K), three shared log budgets,
     and g_sfo_ppm_ema (plain double read by every consumer while the DMRSFO tracker stores it) -> _Atomic.

## Unit test
DiscoveryGates.DeferredCommitIsAppliedByTheConsumerNotTheReceiveThread (nr_pdcch_blind_monitor_test.cc): with
defer_commit(true) and a concurrent note_rnti_for_windows() thread, the step converges but only posts (pending,
not done, a second decision is refused); apply_pending() under the Phase-2 lock commits it (coreset_rb_offset, the
receive thread's gate view = 1/0/1).
test_nr_pdcch_blind_monitor: 198 pass + 2 skips (= A7's 197 + this test), shuffle seeds 1/3/5 too
-> gtest_shuffle_1_3_5.txt

## TSAN oracle (same method as A7: rfsim 106 PRB, scan_thread "2:16:-1", 300 s, partial instrumentation)
Instrumented: NR_UE_TRANSPORT/{nr_pdcch_blind_*,nr_pdcch_passive_queue,nr_pdsch_passive_*,nr_pdcch_coreset_*,
dci_nr}.c + threadPool/task_ans.c (needed so TSAN sees the completion counter's ordering) via tsan_cc_launcher.sh.
  cmake -S . -B cmake_targets/ran_build/build_tsanp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOAI_SIMU=ON \
    -DENABLE_ISAC_SENSING=ON -DCCACHE_FOUND=$PWD/<this dir>/tsan_cc_launcher.sh \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread
  (CCACHE_FOUND, not CMAKE_C_COMPILER_LAUNCHER: the root CMakeLists overrides the launcher when ccache exists.)
  ninja -C cmake_targets/ran_build/build_tsanp nr-uesoftmodem rfsimulator ldpc dfts params_libconfig coding
  Run dir BUILD=<w> with <w>/nr-softmodem -> the plain build's gNB and <w>/nr-uesoftmodem a 2-line wrapper that
  sets LD_LIBRARY_PATH=build_tsanp before exec'ing the TSAN receiver (libtsan intercepts dlopen, so the binary's
  RUNPATH is not searched for the config/LDPC/rfsim modules; the gNB must keep its own non-TSAN modules):
  BUILD=<w> SCANTHREAD=2:16:-1 TSAN_OPTIONS="log_path=<out>/tsan halt_on_error=0" tests/passive_rx/dgx/rfsim_arm.sh <out>/arm 300
| run | code | reports | blind_monitor.c | pdsch_passive_queue.c / decode.c / task_ans / Tpool | stage reached |
|---|---|---|---|---|---|
| A7 round 2 (../a7_concurrency) | A7 final | 24 | 13 | 4 | TRACKING |
| tsan_rfsim_partial_round1.txt | fixes 1 + 2 (queue statics, task_ans) | 8 | 0 | 2 (decode.c:356, :3869) | TRACKING, conv 154 s, CONVERGED 2 |
| tsan_rfsim_partial_round2.txt | + 20 decode.c env statics | 6 | 0 | 1 (decode.c:101 pdtim) | TRACKING, conv 146 s |
| tsan_rfsim_partial_round3.txt | FINAL | 5 | 0 | 0 | TRACKING, conv 160 s |
Round 3 leftovers, none in the scan code: rfsimulator teardown (clear_old_packets, deque delete), SIGINT handler
(trigger_deregistration errno, signal-unsafe calloc/new) -- the "rfsim/exit" class A7 also left.
In every TSAN run the second footprint decision (~130 s, after autodiscover_next) was taken with the 2-consumer
pool running, i.e. posted by the receive thread and committed on a consumer (COREMAPLT / "recurrent oracle
committed" lines in the receiver log). (The first decision lands before the pool starts -- the pool starts lazily
on the first on-occasion slot -- and is applied in-line, single-threaded.)

## Regression gate (default config, SCANTHREAD auto "1:8:-1", final code)
GATE_CRC_MIN=93.0 GATE_DROP_MAX=2.5 tests/passive_rx/dgx/rfsim_regress.sh 1 -> PASS, crc 96.85 %, drop_full 1.26 %,
CONVERGED 2, ttc 2.82 s (A1 HEAD baseline crc 94.9-96.9 %, drop 0.77-1.55 %) -> regress_base_r1.score.json/.time.txt
(An earlier gate on the round-1 code: PASS, crc 95.21 %, drop_full 1.42 %, CONVERGED 2; its second decision at 39.4 s
went through the deferred path with 1 consumer.)
sens6 frozen check: git diff sens6-frozen-2026-09-30 -- tests/passive_rx/captures tests/passive_rx/*.conf
tests/passive_rx/sens6_host_snapshot_2026-09-30 -> empty.
