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
}

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

int main() { recovery_test(); wrap_test(); }
