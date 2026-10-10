/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "radio/USRP/usrp_ring_recovery.h"
#include "radio/USRP/usrp_ring_view.h"
#include <assert.h>
#include <random>
#include <vector>

struct fake_streamer {
  std::mt19937 rng{415};
  int calls = 0;
  /* 1 overflow, 2 exception, 3 timeout, 0 valid packet. */
  int recv() { ++calls; return calls % 997 == 0 ? 1 : calls % 1093 == 0 ? 2 : calls % 1319 == 0 ? 3 : (rng() % 3000 == 0 ? 1 : 0); }
};

/* recovery_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: tests/passive_rx/rxr/test_usrp_ring.cpp:140.
 */
static void recovery_test()
{
  usrp_ring_recovery_state state(1000, 20);
  fake_streamer stream;
  uint64_t last_write = 0, expected_epoch = 0;
  bool pending = false;
  for (int i = 0; i < 30000; i++) {
    const int fault = stream.recv();
    if (fault) {
      assert(state.fault());
      assert(state.boundary == last_write);
      pending = true;
    } else {
      if (pending) {
        state.first_packet();
        expected_epoch++;
        pending = false;
      }
      const size_t n = 1 + (stream.rng() % 24);
      state.samples(n);
      assert(state.write_index == last_write + n);
      last_write = state.write_index;
      assert(state.epoch == expected_epoch);
    }
  }
  usrp_ring_recovery_state stuck(1000, 3);
  for (int i = 0; i < 3; i++) assert(stuck.fault());
  assert(!stuck.fault());
  assert(stuck.write_index == 0 && stuck.epoch == 0);
  stuck.samples(19);
  assert(stuck.failures == 4);
  stuck.samples(1); /* 20 ms of valid streaming resets the failed-start budget */
  assert(stuck.failures == 0);
  for (int i = 0; i < 3; i++) assert(stuck.fault());
  assert(!stuck.fault());
  assert(usrp_ring_epoch_first_ticks(368640000, 385000000) == 385000000);
  assert(usrp_ring_epoch_first_ticks(385000004, 385000000) == 385000004);
  const int64_t old_origin = 368640000, logical_boundary = 1228800;
  const int64_t expected_next = old_origin + logical_boundary;
  const int64_t new_first = usrp_ring_epoch_first_ticks(old_origin, 385000000);
  assert(new_first - expected_next > 0); /* no negative gap from frozen old origin */
  assert(!usrp_ring_has_samples(10, 11, 1)); /* no unsigned wrap */
  assert(usrp_ring_has_samples(11, 10, 1));
}

/* fault_window_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: tests/passive_rx/rxr/test_usrp_ring.cpp:140.
 */
static void fault_window_test()
{
  /* 48 kS/s: 20 ms = 960 samples. A fault every 30 ms, each preceded by a clean 20 ms, resets
   * `failures` every time; only the fault-window budget can end it. */
  usrp_ring_recovery_state s(48000, 20, 20, 60.0);
  int n = 0;
  double t = 0;
  bool ok = true;
  while (ok && n < 1000) {
    ok = s.fault(t);
    n++;
    s.first_packet();
    s.samples(960);
    assert(s.failures == 0);
    t += 0.030;
  }
  assert(!ok && n == 21); /* the 21st fault inside 60 s is fatal */
  /* Sparse faults (one per 5 s, 12 per window) keep recovering indefinitely. */
  usrp_ring_recovery_state sparse(48000, 20, 20, 60.0);
  for (int i = 0; i < 500; i++) {
    assert(sparse.fault(i * 5.0));
    sparse.first_packet();
    sparse.samples(960);
  }
  /* Window disabled (flag off): behaviour is the legacy consecutive-failure budget only. */
  usrp_ring_recovery_state off(48000, 20);
  for (int i = 0; i < 500; i++) {
    assert(off.fault(i * 0.030));
    off.samples(960);
  }
}

/* wrap_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: tests/passive_rx/rxr/test_usrp_ring.cpp:140.
 */
static void wrap_test()
{
  const size_t cap = 17;
  std::vector<int32_t> ring(cap), dst(35);
  for (size_t i = 0; i < cap; i++) {
    int16_t p[2] = {(int16_t)(i * 127 - 1200), (int16_t)(900 - i * 233)};
    ring[i] = (uint16_t)p[0] | ((uint32_t)(uint16_t)p[1] << 16);
  }
  for (size_t start = 0; start < cap; start++)
    for (int n = 1; n <= 35; n++) {
      usrp_ring_shift_window(ring.data(), cap, start, dst.data(), n, 2);
      for (int j = 0; j < n; j++) {
        int32_t v = ring[(start + j) % cap];
        int16_t in[2] = {(int16_t)v, (int16_t)((uint32_t)v >> 16)};
        int16_t out[2] = {(int16_t)dst[j], (int16_t)((uint32_t)dst[j] >> 16)};
        assert(out[0] == (in[0] >> 2));
        assert(out[1] == (in[1] >> 2));
      }
    }
}

/* poll_classify_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: tests/passive_rx/rxr/test_usrp_ring.cpp:140.
 */
static void poll_classify_test()
{
  /* Data always wins, even with a timeout code from a later fragment. */
  assert(usrp_ring_poll_classify(1988, false, 0.0, 0.1) == USRP_RING_POLL_DATA);
  assert(usrp_ring_poll_classify(1988, true, 5.0, 0.1) == USRP_RING_POLL_DATA);
  /* An empty result without an error is not a timeout. */
  assert(usrp_ring_poll_classify(0, false, 5.0, 0.1) == USRP_RING_POLL_DATA);
  /* Empty zero-timeout polls retry until the silence limit, then become a timeout fault. */
  assert(usrp_ring_poll_classify(0, true, 0.0, 0.1) == USRP_RING_POLL_RETRY);
  assert(usrp_ring_poll_classify(0, true, 0.0999, 0.1) == USRP_RING_POLL_RETRY);
  assert(usrp_ring_poll_classify(0, true, 0.1, 0.1) == USRP_RING_POLL_TIMEOUT);
  assert(usrp_ring_poll_classify(0, true, 3.0, 0.1) == USRP_RING_POLL_TIMEOUT);
}

/* cpulist_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: tests/passive_rx/rxr/test_usrp_ring.cpp:140.
 */
static void cpulist_test()
{
  assert(usrp_cpulist_mask("15-19\n") == 0xF8000ULL);
  assert(usrp_cpulist_mask("17-19") == 0xE0000ULL);
  assert(usrp_cpulist_mask("2,4-6") == ((1ULL << 2) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6)));
  assert(usrp_cpulist_mask("") == 0 && usrp_cpulist_mask("\n") == 0 && usrp_cpulist_mask(NULL) == 0);
  assert(usrp_cpulist_mask("16") == (1ULL << 16));
  assert(usrp_cpulist_mask("62-70") == ((1ULL << 62) | (1ULL << 63)));
  /* the 2026-10-08 boot (17-19) leaves the ring core 16 and uhd_ctrl 15 exposed */
  const uint64_t old_boot = usrp_cpulist_mask("17-19"), new_boot = usrp_cpulist_mask("15-19");
  assert(!(old_boot >> 16 & 1) && !(old_boot >> 15 & 1) && (new_boot >> 16 & 1) && (new_boot >> 15 & 1));
}

/* main: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: none (program entry point).
 */
int main() { recovery_test(); fault_window_test(); wrap_test(); poll_classify_test(); cpulist_test(); }
