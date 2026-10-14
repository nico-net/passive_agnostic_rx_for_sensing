/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
// ULFEP: the UL GPU front end (nr_pusch_gpu_fep.cuh in libpdsch_gpu.so) against the CPU receiver, bit for bit:
// dft(get_dft(4096)) of the window + apply_nr_rotation_symbol_RX (rotate=1) or the bare DFT (rotate=0), random int16 IQ
// with a spread of amplitudes (uniform +-40 .. +-3000, i.e. rms up to ~1700; real OTA UL captures have rms 290-750), odd and
// even carrier widths, several windows per batch. KNOWN LIMIT (measured): from uniform +-4000 (rms ~2300, white, all samples)
// the CPU dft4096 saturates in intermediate butterflies in a way the kernel (nr_exact::fft4096, shared with the PDSCH exact
// FEP) does not reproduce: ~128-512 of 4096 bins differ. The runtime check of ISAC_UL_GPU_FEP_CHECK guards that case.
// Skipped (exit 77 / GTEST_SKIP) without a CUDA device, a coherent-memory platform or the module.
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
extern "C" {
#include "openair1/PHY/TOOLS/tools_defs.h"
// nr_modulation.h has C-only VLA prototypes: declare the function used
void init_timeshift_rotation(const int ofdm_symbol_size, const int nb_prefix_samples, const uint ofdm_offset_divisor,
                             c16_t *timeshift_symbol_rotation);
// slot_fep_nr.c lives in the (huge) UE library: its apply_nr_rotation_symbol_RX is copied here, call for call
// (rotate_cpx_vector / mult_cpx_vector are the real OAI SIMD routines).
static void apply_nr_rotation_symbol_RX(const int symbols_per_slot, const int slots_per_subframe, const c16_t *shift_rot,
                                        const int first_carrier_offset, c16_t *this_symbol, const c16_t *rot, int nb_rb,
                                        int slot, int symbol)
{
  const int symb_offset = (slot % slots_per_subframe) * symbols_per_slot;
  c16_t rot2 = rot[symbol + symb_offset];
  rot2.i = -rot2.i;
  if (nb_rb & 1) {
    rotate_cpx_vector(this_symbol, rot2, this_symbol, (nb_rb + 1) * 6, 15);
    rotate_cpx_vector(this_symbol + first_carrier_offset - 6, rot2, this_symbol + first_carrier_offset - 6, (nb_rb + 1) * 6, 15);
    mult_cpx_vector(this_symbol, shift_rot, this_symbol, (nb_rb + 1) * 6, 15);
    mult_cpx_vector(this_symbol + first_carrier_offset - 6, shift_rot + first_carrier_offset - 6,
                    this_symbol + first_carrier_offset - 6, (nb_rb + 1) * 6, 15);
  } else {
    rotate_cpx_vector(this_symbol, rot2, this_symbol, nb_rb * 6, 15);
    rotate_cpx_vector(this_symbol + first_carrier_offset, rot2, this_symbol + first_carrier_offset, nb_rb * 6, 15);
    mult_cpx_vector(this_symbol, shift_rot, this_symbol, nb_rb * 6, 15);
    mult_cpx_vector(this_symbol + first_carrier_offset, shift_rot + first_carrier_offset, this_symbol + first_carrier_offset,
                    nb_rb * 6, 15);
  }
}
#include "openair1/PHY/NR_UE_TRANSPORT/nr_pusch_gpu_fep.h"
}

TEST(UlGpuFep, BitExactVsCpu)
{
  ASSERT_EQ(0, load_dftslib());
  setenv("ISAC_UL_GPU_FEP", "1", 1);
  const nr_pusch_gpu_fep_api_t *api = nr_pusch_gpu_fep_load();
  if (!api) GTEST_SKIP() << "libpdsch_gpu.so not loadable";
  const int N = 4096, ncp = 288, div = 8, nsym = 14, spsf = 2;
  if (api->init(4, N, ncp, div, 20) != 0) GTEST_SKIP() << "no CUDA device / coherent host memory";
  std::vector<c16_t> ts(N);
  init_timeshift_rotation(N, ncp, div, ts.data());
  std::mt19937 rng(7);
  std::vector<c16_t> rot(nsym * spsf);
  for (auto &r : rot) { r.r = (int16_t)(rng() % 60000 - 30000); r.i = (int16_t)(rng() % 60000 - 30000); }
  const size_t len = 69632;  // one UL snapshot: 1 ms + margin
  std::vector<std::vector<c16_t>> iq(4, std::vector<c16_t>(len));
  std::vector<std::vector<c16_t>> out(4 * 3 * 8, std::vector<c16_t>(N));
  int bad = 0, rows = 0;
  for (int nb_rb : {217, 106, 273, 51}) {
    const int fco = N - nb_rb * 6;
    for (int amp : {40, 300, 1000, 3000}) {
      for (auto &a : iq) for (auto &x : a) { x.r = (int16_t)((int)(rng() % (2 * amp + 1)) - amp); x.i = (int16_t)((int)(rng() % (2 * amp + 1)) - amp); }
      for (int rotate : {1, 0}) {
        const int n = 6;
        std::vector<nr_pusch_gpu_fep_item_t> it(n);
        std::vector<int> start(n), sym(n), slot(n);
        for (int i = 0; i < n; i++) {
          memset(&it[i], 0, sizeof(it[i]));
          start[i] = (int)(rng() % (len - N)); sym[i] = (int)(rng() % nsym); slot[i] = (int)(rng() % spsf);
          const c16_t r = rot[sym[i] + slot[i] * nsym];
          for (int a = 0; a < 4; a++) { it[i].src[a] = (const int16_t *)&iq[a][start[i]]; it[i].dst[a] = (int16_t *)out[i * 4 + a].data(); }
          it[i].rot[0] = r.r; it[i].rot[1] = r.i; it[i].rotate = rotate;
        }
        ASSERT_EQ(0, api->run(it.data(), n, nb_rb, fco));
        for (int i = 0; i < n; i++)
          for (int a = 0; a < 4; a++) {
            c16_t ref[N] __attribute__((aligned(32)));
            dft(get_dft(N), (int16_t *)&iq[a][start[i]], (int16_t *)ref, 1);
            if (rotate) apply_nr_rotation_symbol_RX(nsym, spsf, ts.data(), fco, ref, rot.data(), nb_rb, slot[i], sym[i]);
            rows++;
            { const bool d = memcmp(ref, out[i * 4 + a].data(), sizeof(ref)) != 0; bad += d; if (d) { int nd = 0, mx = 0; const c16_t *g = out[i * 4 + a].data();
            for (int k = 0; k < N; k++) { const int dr = abs(ref[k].r - g[k].r), di = abs(ref[k].i - g[k].i); if (dr || di) nd++; mx = std::max(mx, std::max(dr, di)); }
            printf("DIFF amp=%d nb_rb=%d rotate=%d n_diff=%d max_abs=%d\n", amp, nb_rb, rotate, nd, mx); } }
          }
      }
    }
  }
  EXPECT_EQ(0, bad) << bad << " of " << rows << " rows differ";
  uint64_t st[10];
  api->stats(st);
  EXPECT_GT(st[0], 0u);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
