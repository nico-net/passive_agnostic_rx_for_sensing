/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Q4: the staged DISCRETE variants of the standalone (non-pass) GPU entries of libpdsch_gpu.so. Part of test_nr_gpu_pass.
 *   - exact FEP (nr_pdsch_gpu_exact_fep): bit-exact vs the CPU (dft + OAI RX symbol rotation), census bytes = exactly the window span of the masked symbols
 *   - delay estimate (nr_pdsch_gpu_exact_delay): equal to the real nr_passive_est_delay
 *   - chest + demod + LDPC (old entries) on host pointers: equal, bit for bit, to the _dev entries of the same process (which the graph tests tie to the CPU stage path);
 *     the demod in both copy-back modes (stage_full: every output; default: LLRs and comp layer 0 of the Qm-oracle symbol)
 * They need a non-DIRECT process (native discrete GPU, or NR_GPU_FORCE_MODEL=discrete); on DIRECT they skip (GB10's old entries are covered by DevEqualsOldEntryPoints).
 * The CPU tier (NR_GPU_FORCE_MODEL=cpu) must refuse every staged entry without a crash. */
#include <gtest/gtest.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
#include "PHY/TOOLS/tools_defs.h"
void init_timeshift_rotation(const int ofdm_symbol_size, const int nb_prefix_samples, const uint ofdm_offset_divisor, c16_t *timeshift_symbol_rotation);
int load_dftslib(void);
}
#include "nr_gpu_pass_fixture.h"
#include "nr_gpu_stage_census.h"
#include "nr_gpu_pass_ul_ref.h"

namespace {
/* copy of slot_fep_nr.c's apply_nr_rotation_symbol_RX (that file lives in the UE library); the vector routines are the real OAI ones */
void rot_rx(int symbols_per_slot, int slots_per_subframe, const c16_t *shift_rot, int fco, c16_t *sym, const c16_t *rot, int nb_rb, int slot, int symbol)
{
  c16_t rot2 = rot[symbol + (slot % slots_per_subframe) * symbols_per_slot];
  rot2.i = -rot2.i;
  const int len = (nb_rb & 1) ? (nb_rb + 1) * 6 : nb_rb * 6, o = (nb_rb & 1) ? fco - 6 : fco;
  rotate_cpx_vector(sym, rot2, sym, len, 15);
  rotate_cpx_vector(sym + o, rot2, sym + o, len, 15);
  mult_cpx_vector(sym, shift_rot, sym, len, 15);
  mult_cpx_vector(sym + o, shift_rot + o, sym + o, len, 15);
}

bool non_direct(GpuPassModule **mp)
{
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  *mp = &m;
  return lib && ok && nr_gpu_model() != NR_GPU_MODEL_DIRECT && m.staged_ready();
}
uint64_t stat(GpuPassModule &m, int e, int k)
{
  uint64_t o[6] = {0};
  if (m.stage_stats) m.stage_stats(e, o);
  return o[k];
}
} // namespace

TEST(GpuStage, ExactFepEqualsCpuAndCountsTheSpan)
{
  GpuPassModule *mp;
  if (!non_direct(&mp)) GTEST_SKIP() << "needs a DISCRETE process (native discrete GPU or NR_GPU_FORCE_MODEL=discrete)";
  ASSERT_EQ(0, load_dftslib());
  const int N = 4096, NCP = 288, NCP0 = 352, NS = 14, NA = 4, NRB = 273, FCO = N - NRB * 6, SLOT = NCP0 + N + (NS - 1) * (N + NCP);
  std::mt19937 rng(11);
  const uint32_t ring_len = 3 * SLOT;
  std::vector<std::vector<c16_t>> ring(NA, std::vector<c16_t>(ring_len));
  for (auto &a : ring) for (auto &x : a) { x.r = (int16_t)((int)(rng() % 2001) - 1000); x.i = (int16_t)((int)(rng() % 2001) - 1000); }
  std::vector<c16_t> rot(2 * NS), ts(N);
  for (auto &r : rot) { r.r = (int16_t)((int)(rng() % 60000) - 30000); r.i = (int16_t)((int)(rng() % 60000) - 30000); }
  init_timeshift_rotation(N, NCP, 8, ts.data());
  const int16_t *rp[4];
  for (int a = 0; a < NA; a++) rp[a] = (const int16_t *)ring[a].data();
  const uint16_t masks[] = {0x3fff, 0x03f8, 0x0001, 0x2000, 0x2124};
  for (int slot : {0, 1})
    for (uint16_t mask : masks) {
      const uint32_t ring_off = ring_len - 1000 + slot; /* the slot wraps around the ring end */
      std::vector<c16_t> out((size_t)NA * NS * N, c16_t{0x5a5a, 0x5a5a});
      const uint64_t h0 = stat(*mp, NR_GPU_STG_FEP, 1), r0 = stat(*mp, NR_GPU_STG_FEP, 0);
      ASSERT_EQ(0, mp->fep_old(rp, ring_len, ring_off, 0.0, (const int16_t *)(rot.data() + slot * NS), mask, (int16_t *)out.data(), (size_t)NS * N));
      int lo = NS, hi = -1;
      for (int l = 0; l < NS; l++) if (mask >> l & 1) { lo = std::min(lo, l); hi = l; }
      const size_t span = (size_t)(hi - lo) * (N + NCP) + N;
      EXPECT_EQ(stat(*mp, NR_GPU_STG_FEP, 0), r0 + 1);
      EXPECT_EQ(stat(*mp, NR_GPU_STG_FEP, 1) - h0, (uint64_t)NA * span * 4) << "H2D must be exactly the span of the masked symbols, mask 0x" << std::hex << mask;
      const uint32_t early = NCP / 8, origin = (ring_off + ring_len - early) % ring_len;
      int bad = 0;
      for (int a = 0; a < NA; a++)
        for (int l = 0; l < NS; l++) {
          const c16_t *got = &out[((size_t)a * NS + l) * N];
          if (!(mask >> l & 1)) { EXPECT_EQ(got[0].r, 0x5a5a) << "unmasked symbol touched"; continue; }
          c16_t in[N] __attribute__((aligned(32))), ref[N] __attribute__((aligned(32)));
          const uint32_t first = origin + NCP0 + l * (N + NCP);
          for (int n = 0; n < N; n++) in[n] = ring[a][(first + n) % ring_len];
          dft(get_dft(N), (int16_t *)in, (int16_t *)ref, 1);
          rot_rx(NS, 2, ts.data(), FCO, ref, rot.data(), NRB, slot, l);
          bad += memcmp(ref, got, sizeof(ref)) != 0;
        }
      EXPECT_EQ(0, bad) << "slot " << slot << " mask 0x" << std::hex << mask;
    }
}

TEST(GpuStage, DelayEqualsCpu)
{
  GpuPassModule *mp;
  if (!non_direct(&mp)) GTEST_SKIP() << "needs a DISCRETE process";
  ASSERT_TRUE(mp->delay_old);
  ASSERT_EQ(0, load_dftslib());
  std::mt19937 rng(5);
  const int N = 4096, R = 16;
  for (int rep = 0; rep < 3; rep++) {
    std::vector<std::vector<c16_t>> ls(R, std::vector<c16_t>(N, c16_t{0, 0}));
    for (int r = 0; r < R; r++) {
      const int d = (int)(rng() % 9) - 4, amp = 200 + 700 * rep;
      for (int k = 0; k < 12 * 100; k++) {
        const double ph = -2 * M_PI * k * d / N;
        ls[r][k].r = (int16_t)(amp * std::cos(ph) + (int)(rng() % 41) - 20);
        ls[r][k].i = (int16_t)(amp * std::sin(ph) + (int)(rng() % 41) - 20);
      }
    }
    const int16_t *rows[R];
    for (int r = 0; r < R; r++) rows[r] = (const int16_t *)ls[r].data();
    for (int n : {1, 5, R}) {
      int out[3 * R];
      const uint64_t b0 = stat(*mp, NR_GPU_STG_DELAY, 1);
      ASSERT_EQ(0, mp->delay_old(rows, n, out));
      EXPECT_EQ(stat(*mp, NR_GPU_STG_DELAY, 1) - b0, (uint64_t)n * N * 4);
      for (int r = 0; r < n; r++) {
        int cpu[3];
        nr_gpu_pass_ul_ref_delay(ls[r].data(), cpu);
        EXPECT_EQ(cpu[0], out[3 * r]);
        EXPECT_EQ(cpu[1], out[3 * r + 1]);
        EXPECT_EQ(cpu[2], out[3 * r + 2]);
      }
    }
  }
}

/* chest + demod + LDPC through the old entry points (host pointers, staged) vs the _dev entries */
static void staged_vs_dev(int nrb, int nl, int qm, int seed, int full)
{
  GpuPassModule *mp;
  ASSERT_TRUE(non_direct(&mp));
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_old, llr_dev;
  GpuPassFixture::stage_full = full;
  const uint64_t cr0 = stat(*mp, NR_GPU_STG_CHEST, 0), dr0 = stat(*mp, NR_GPU_STG_DEMOD, 0), lr0 = stat(*mp, NR_GPU_STG_LDPC_DL, 0);
  const uint64_t dh0 = stat(*mp, NR_GPU_STG_DEMOD, 1), dd0 = stat(*mp, NR_GPU_STG_DEMOD, 2);
  ASSERT_EQ(0, fx.run_stages(&llr_old, true));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const auto comp0 = fx.comp_out; const int rc0 = fx.ldpc_rc, ns0 = fx.ldpc_nsnap;
  ASSERT_EQ(0, fx.run_stages(&llr_dev, false));
  GpuPassFixture::stage_full = 1;
  EXPECT_EQ(cr0 + 1, stat(*mp, NR_GPU_STG_CHEST, 0)) << "the chest must have run staged";
  EXPECT_EQ(dr0 + 1, stat(*mp, NR_GPU_STG_DEMOD, 0)) << "the demod must have run staged";
  EXPECT_EQ(lr0 + 1, stat(*mp, NR_GPU_STG_LDPC_DL, 0)) << "the LDPC must have run staged";
  EXPECT_GT(stat(*mp, NR_GPU_STG_DEMOD, 1), dh0);
  EXPECT_GT(stat(*mp, NR_GPU_STG_DEMOD, 2), dd0);
  EXPECT_EQ(rxF0, fx.rxF_head) << "rxdataF";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delay/nvar";
  ASSERT_EQ(llr_old.size(), llr_dev.size());
  EXPECT_EQ(0, memcmp(llr_old.data(), llr_dev.data(), llr_old.size() * 2)) << "LLRs";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  EXPECT_EQ(ns0, fx.ldpc_nsnap);
  EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots / syn0";
  const auto &dev = fx.comp_out;
  ASSERT_EQ(dev.size(), comp0.size());
  if (full) {
    EXPECT_EQ(dev, comp0) << "comp / mag arrays (stage_full)";
  } else { /* only comp layer 0 of the first symbol with the most REs; everything else untouched (zero on the staged side) */
    const int ext = (12 * nrb + 15) & ~15, per_arr = GpuPassFixture::NSYM * 4 * ext * 2;
    int qm_m = -1;
    for (int sy : fx.data_syms) if (qm_m < 0) qm_m = sy; /* all data symbols carry the same RE count in the fixture: the first */
    size_t diff = 0, untouched = 0;
    for (size_t i = 0; i < dev.size(); i++) {
      const int arr = (int)(i / per_arr), row = (int)((i % per_arr) / (2 * ext)), col = (int)((i % per_arr) % (2 * ext));
      const bool want = arr == 0 && row == qm_m * 4 && col < 2 * 12 * nrb;
      if (want) diff += dev[i] != comp0[i]; else untouched += comp0[i] != 0;
    }
    EXPECT_EQ(0u, diff) << "comp layer 0 of the Qm-oracle symbol";
    EXPECT_EQ(0u, untouched) << "default copy-back must not write anything else";
  }
}
TEST(GpuStage, StagedEqualsDevFullCopyBack)
{
  GpuPassModule *mp;
  if (!non_direct(&mp)) GTEST_SKIP() << "needs a DISCRETE process";
  staged_vs_dev(24, 2, 4, 7, 1);
  staged_vs_dev(12, 1, 6, 9, 1);
  staged_vs_dev(24, 4, 2, 5, 1);
  staged_vs_dev(48, 3, 8, 3, 1);
}
TEST(GpuStage, StagedEqualsDevDecoderCopyBack)
{
  GpuPassModule *mp;
  if (!non_direct(&mp)) GTEST_SKIP() << "needs a DISCRETE process";
  staged_vs_dev(24, 2, 4, 7, 0);
  staged_vs_dev(12, 1, 6, 9, 0);
}

/* the CPU tier (NR_GPU_FORCE_MODEL=cpu): the module refuses to initialise, nothing is staged, nothing crashes */
TEST(GpuStage, CpuTierRefuses)
{
  const char *f = getenv("NR_GPU_FORCE_MODEL");
  if (!f || strcmp(f, "cpu")) GTEST_SKIP() << "only under NR_GPU_FORCE_MODEL=cpu";
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  EXPECT_FALSE(ok);
  EXPECT_FALSE(m.staged_ready());
}
