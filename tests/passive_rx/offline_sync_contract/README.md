# Offline synchronization contracts

Run `bash tests/passive_rx/offline_sync_contract/build_and_run.sh` from a configured
OAI_SIMU checkout after building `test_nr_pdcch_blind_monitor` and `dfts`.
The script uses the existing build's libraries/objects and leaves artifacts in a fresh /tmp directory.
It does not launch a modem or access radio hardware.

Five tests use analytically synthesized OFDM and the actual production FEP/FFT/equalizer:
CFO sign, feedback-law counterexamples, SFO sign/physical frequency origin, joint CFO/SFO
separation, and cross-thread deferred CFO snapshot/restoration. Invalid/clipped analytic input
aborts the test rather than producing a performance score.

The feedback estimator/control-loop examples are explicit reference formulas, not a claim
that the live DM-RS tracker itself is tested end-to-end. Those live correction switches remain off.
The cross-thread test invokes production nr_slot_fep_ant_snapshot directly on a fresh pthread;
it also executes the lost-snapshot counterexample and checks restoration of reused-worker TLS.
Synthetic tests establish software contracts, not live CRC performance.
