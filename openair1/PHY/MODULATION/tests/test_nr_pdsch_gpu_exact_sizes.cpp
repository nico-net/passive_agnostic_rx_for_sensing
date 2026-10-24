/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
// GPU FEP for every FFT size (sens6 per-path, 2026-10-24): the bit-exact DL FEP of libpdsch_gpu.so (nr_pdsch_gpu_exact_fep2) against the CPU
// receive FEP, bit for bit, at the FFT size given by NR_TEST_FEP_N (one size per process: the module initialises once). The CPU reference is
// nr_slot_fep_ant()'s computation (slot_fep_nr.c lives in the UE library, so its window arithmetic and apply_nr_rotation_symbol_RX are copied
// here call for call; dft() / rotate_cpx_vector / mult_cpx_vector / init_timeshift_rotation are the real OAI routines). Slots 0..3, every
// symbol and antenna, random int16 IQ at amplitudes 40 .. 12000.
// Expected: the sizes the host's GPU twin reproduces (aarch64 NEON: 4096; x86: powers of two 256..4096) are bit-identical; the others (the 3/4
// sampling sizes 768, 1536, 3072) are refused: nr_pdsch_gpu_exact_ready() = 0 and the FEP call returns -1 (the receiver keeps the CPU FEP).
// Skipped without libpdsch_gpu.so or a CUDA device (GTEST_SKIP, reported as Skipped by ctest).
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
extern "C" {
#include "openair1/PHY/TOOLS/tools_defs.h"
void init_timeshift_rotation(const int ofdm_symbol_size, const int nb_prefix_samples, const uint ofdm_offset_divisor,
                             c16_t *timeshift_symbol_rotation);
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
#include "openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_fep.h"
}

typedef int (*exact_fn_t)(const int16_t *const *, uint32_t, uint32_t, double, const int16_t *, uint16_t, int16_t *, size_t, int);

/* the N_RB of each FFT size (30 kHz, the receiver's cells: 24 / 51 / 106 / 273 PRB; 3/4 sampling: 51 / 106 / 217 PRB) */
static int nb_rb_of(int N)
{
  switch (N) {
    case 512: return 25;
    case 768: return 51;
    case 1024: return 51;
    case 1536: return 106;
    case 2048: return 106;
    case 3072: return 217;
    default: return 273;
  }
}

TEST(GpuExactFepSizes, BitExactVsCpuOrRefused)
{
  const char *ne = getenv("NR_TEST_FEP_N");
  const int N = ne ? atoi(ne) : 4096;
  ASSERT_TRUE(N >= 256 && N <= 4096) << "NR_TEST_FEP_N";
  ASSERT_EQ(0, load_dftslib());
  void *h = dlopen("libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) h = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) GTEST_SKIP() << "libpdsch_gpu.so not loadable";
  auto get = (const nr_gpu_fep_api_t *(*)(void))dlsym(h, "nr_gpu_fep_api");
  auto fep = (exact_fn_t)dlsym(h, "nr_pdsch_gpu_exact_fep2");
  auto ready = (int (*)(void))dlsym(h, "nr_pdsch_gpu_exact_ready");
  ASSERT_TRUE(get && fep && ready);
  const nr_gpu_fep_api_t *api = get();
  if (!api) GTEST_SKIP() << "no CUDA device (module not ready)";
  const int mu = 1, nant = 4, nsym = 14, spsf = 1 << mu, div = 8;
  const int nb_rb = nb_rb_of(N), fco = N - nb_rb * 6;
  const int ncp = N / 128 * 9, ncp0 = N / 128 * (9 + (1 << mu));
  const int slot_len = ncp0 + N + (nsym - 1) * (N + ncp);
  const int samples_per_ms = slot_len * spsf;
  const nr_gpu_fep_cfg_t cfg = {.nant = nant, .ofdm_symbol_size = N, .nb_prefix_samples = ncp, .nb_prefix_samples0 = ncp0, .symbols_per_slot = nsym,
                                .first_carrier_offset = fco, .n_rb_dl = nb_rb, .samples_per_ms = samples_per_ms, .ofdm_offset_divisor = div};
  if (api->init(&cfg) != 0) GTEST_SKIP() << "module init failed (no CUDA device?)";
  const bool pow2_x86 =
#if defined(__x86_64__)
      (N & (N - 1)) == 0;
#else
      N == 4096;
#endif
  printf("N=%d nb_rb=%d ready=%d expected=%s\n", N, nb_rb, ready(), pow2_x86 ? "bit-exact" : "refused");
  const uint32_t ring = (uint32_t)(20 * slot_len); /* 10 ms frame x 2, as the receiver's ring */
  std::vector<std::vector<c16_t>> iq(nant, std::vector<c16_t>(ring));
  std::vector<c16_t> ts(N), rot(nsym * spsf);
  init_timeshift_rotation(N, ncp, div, ts.data());
  std::mt19937 rng(11 + N);
  for (auto &r : rot) { r.r = (int16_t)(rng() % 60000 - 30000); r.i = (int16_t)(rng() % 60000 - 30000); }
  const size_t stride = (size_t)nsym * N;
  std::vector<c16_t> gpu(nant * stride), ref(nant * stride);
  const int early = ncp / div;
  int bad_rows = 0, rows = 0, calls = 0;
  for (int amp : {40, 300, 1000, 3000, 12000}) {
    for (auto &a : iq)
      for (size_t k = 0; k < 6u * slot_len; k++) {
        a[k].r = (int16_t)((int)(rng() % (2 * amp + 1)) - amp);
        a[k].i = (int16_t)((int)(rng() % (2 * amp + 1)) - amp);
      }
    const int16_t *rx[4];
    for (int a = 0; a < nant; a++) rx[a] = (const int16_t *)iq[a].data();
    for (int slot = 1; slot <= 4; slot++) {
      const uint32_t ts_slot = (uint32_t)(slot * slot_len);
      const c16_t *rot_slot = rot.data() + (slot % spsf) * nsym;
      memset(gpu.data(), 0, gpu.size() * sizeof(c16_t));
      const int rc = fep(rx, ring, ts_slot, 0.0, (const int16_t *)rot_slot, (uint16_t)0x3fff, (int16_t *)gpu.data(), stride, 0);
      calls++;
      if (!pow2_x86) {
        EXPECT_EQ(0, ready()) << "a size without a GPU twin must be refused";
        EXPECT_NE(0, rc) << "a size without a GPU twin must be refused";
        continue;
      }
      ASSERT_EQ(1, ready());
      ASSERT_EQ(0, rc) << "GPU FEP refused at N=" << N;
      for (int a = 0; a < nant; a++)
        for (int sym = 0; sym < nsym; sym++) {
          /* nr_slot_fep_ant(): synchronized, no FO, sample_offset 0 */
          uint32_t off = ts_slot;
          for (int l = 0; l <= sym; l++) off += l == 0 ? ncp0 : ncp;
          off += (uint32_t)(N * sym) - (uint32_t)early;
          /* the DFT library asserts 64-byte aligned buffers (oai_dfts.c): aligned scratch, then the reference row */
          alignas(64) static c16_t in[4096], o[4096];
          memcpy(in, &iq[a][off], sizeof(c16_t) * N);
          dft(get_dft(N), (int16_t *)in, (int16_t *)o, 1);
          apply_nr_rotation_symbol_RX(nsym, spsf, ts.data(), fco, o, rot.data(), nb_rb, slot, sym);
          memcpy(&ref[a * stride + (size_t)sym * N], o, sizeof(c16_t) * N);
          rows++;
          if (memcmp(o, &gpu[a * stride + (size_t)sym * N], sizeof(c16_t) * N)) {
            int nd = 0;
            for (int k = 0; k < N; k++) nd += o[k].r != gpu[a * stride + (size_t)sym * N + k].r || o[k].i != gpu[a * stride + (size_t)sym * N + k].i;
            if (bad_rows++ < 5) printf("DIFF N=%d amp=%d slot=%d ant=%d sym=%d n_diff=%d\n", N, amp, slot, a, sym, nd);
          }
        }
    }
  }
  EXPECT_EQ(0, bad_rows) << bad_rows << " of " << rows << " (antenna, symbol) rows differ at N=" << N;
  printf("N=%d calls=%d rows=%d bad=%d\n", N, calls, rows, bad_rows);
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
