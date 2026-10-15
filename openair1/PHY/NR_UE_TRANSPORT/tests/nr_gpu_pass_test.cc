#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include <cuda_runtime.h>
#include <vector>
extern "C" {
#include "PHY/TOOLS/nr_gpu_mem.h"
}
#include <map>
#include "nr_gpu_pass_fixture.h"
#include "nr_gpu_platform.h"
#include "nr_gpu_pass_post.h"
#include <random>
#include <dlfcn.h>
#include <cmath>
#include <algorithm>
static bool have_gpu() { return nr_gpu_model() >= 0 && getenv("NR_GPU_TEST_SKIP") == nullptr; }

TEST(GpuMem, RoundTripBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  for (int force : {0, 1}) {
    setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
    nr_gpu_buf_t b;
    ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 4096));
    EXPECT_EQ(force ? NR_GPU_MODEL_DISCRETE : nr_gpu_model(), b.model);
    for (int i = 0; i < 4096; i++) ((unsigned char *)b.host)[i] = (unsigned char)i;
    ASSERT_EQ(0, nr_gpu_buf_h2d_async(&b, 0, 4096, nullptr));
    memset(b.host, 0, 4096);
    ASSERT_EQ(0, nr_gpu_buf_d2h_async(&b, 0, 4096, nullptr));
    cudaDeviceSynchronize();
    if (b.model == NR_GPU_MODEL_DISCRETE) {
      for (int i = 0; i < 4096; i++) ASSERT_EQ((unsigned char)i, ((unsigned char *)b.host)[i]);
    }
    nr_gpu_buf_free(&b);
  }
}
TEST(GpuMem, AllocIsFirstTouched) {
  if (!have_gpu()) GTEST_SKIP();
  nr_gpu_buf_t b;
  ASSERT_EQ(0, nr_gpu_buf_alloc(&b, 1 << 20));
  for (size_t i = 0; i < (1u << 20); i += 4096) ASSERT_EQ(0, ((unsigned char *)b.host)[i]);
  nr_gpu_buf_free(&b);
}

/* Stage parity of the launch-only DL entry points: the same slot through FEP -> chest -> demod -> LDPC on one stream, once with the
 * buffers of the platform's own model and once forced DISCRETE (device ring + explicit copies). On GB10 the first run is DIRECT
 * (the kernels read the host IQ ring in place), on sens6 both are DISCRETE. Not compared against the OAI CPU chain: that needs the
 * PHY_NR_UE link set (see the Task 2 report); the end-to-end hard decisions are scored against the transmitted bits instead. */
static void dl_stages_case(int nrb, int nl, int qm, int seed, double max_ber) {
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_direct, llr_discrete;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_direct));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const int rc0 = fx.ldpc_rc;
  setenv("ISAC_GPU_FORCE_DISCRETE", "1", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_discrete));
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  ASSERT_EQ(llr_direct.size(), llr_discrete.size());
  ASSERT_EQ(fx.tx_bits.size(), llr_direct.size());
  EXPECT_EQ(rxF0, fx.rxF_head) << "FEP rxdataF differs between the models";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delays/nvar differ between the models";
  EXPECT_EQ(0, memcmp(llr_direct.data(), llr_discrete.data(), llr_direct.size() * 2)) << "demod LLRs differ between the models";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  if (rc0 == 0) EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots differ between the models";
  else ADD_FAILURE() << "LDPC refused (rc " << rc0 << "): below CC 9 the non-cluster kernels run, nothing may refuse";
  size_t err = 0;
  for (size_t i = 0; i < llr_direct.size(); i++) err += (llr_direct[i] < 0) != (fx.tx_bits[i] != 0);
  printf("nrb=%d nl=%d qm=%d: %zu LLRs, BER vs tx %.4f, LDPC rc %d nsnap %d, parity checked (model 1: %s)\n", nrb, nl, qm, llr_direct.size(),
         (double)err / llr_direct.size(), rc0, fx.ldpc_nsnap, nr_gpu_model() == NR_GPU_MODEL_DIRECT ? "DIRECT" : "DISCRETE");
  EXPECT_LT((double)err / llr_direct.size(), max_ber) << "chain does not demodulate the synthetic slot: " << err << " bit errors";
}
/* GB10 only: the Stage-1 bit-exact reference is today's GPU exact path. Same fixture inputs through the OLD entry points (own waits,
 * host buffers) and through the four _dev functions on the DIRECT model. */
static void dev_vs_old_case(int nrb, int nl, int qm, int seed) {
  GpuPassFixture fx = GpuPassFixture::small_grant(nrb, nl, qm, seed);
  std::vector<int16_t> llr_old, llr_dev;
  setenv("ISAC_GPU_FORCE_DISCRETE", "0", 1);
  ASSERT_EQ(0, fx.run_stages(&llr_old, true));
  const auto rxF0 = fx.rxF_head; const auto chest0 = fx.chest_out; const auto snap0 = fx.snap; const int rc0 = fx.ldpc_rc, ns0 = fx.ldpc_nsnap;
  ASSERT_EQ(0, fx.run_stages(&llr_dev, false));
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  EXPECT_EQ(rxF0, fx.rxF_head) << "rxdataF: old vs _dev";
  EXPECT_EQ(chest0, fx.chest_out) << "chest delay/nvar: old vs _dev";
  ASSERT_EQ(llr_old.size(), llr_dev.size());
  EXPECT_EQ(0, memcmp(llr_old.data(), llr_dev.data(), llr_old.size() * 2)) << "LLRs: old vs _dev";
  EXPECT_EQ(rc0, fx.ldpc_rc);
  EXPECT_EQ(ns0, fx.ldpc_nsnap);
  EXPECT_EQ(snap0, fx.snap) << "LDPC snapshots/syn0: old vs _dev";
}
TEST(GpuPassStages, DevEqualsOldEntryPoints) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  unsetenv("ISAC_GPU_FORCE_DISCRETE"); /* GpuMem.RoundTrip leaves it at 1 */
  if (nr_gpu_model() != NR_GPU_MODEL_DIRECT) GTEST_SKIP() << "old entry points need the DIRECT model (GB10)";
  dev_vs_old_case(24, 2, 4, 7);
  dev_vs_old_case(12, 1, 6, 9);
  dev_vs_old_case(24, 4, 2, 5);
}
TEST(GpuPassStages, DlStagesSameBitsBothModels) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok) << "libpdsch_gpu.so lacks the _dev entry points or has no device";
  dl_stages_case(/*nrb=*/24, /*nl=*/2, /*qm=*/4, /*seed=*/7, 0.02);
  dl_stages_case(12, 1, 6, 9, 0.02);
  dl_stages_case(24, 4, 2, 5, 0.15); // rank 4 over a 4-RX channel is ill-conditioned (nr_pdsch_gpu_fep_test): sanity only, parity is the check
}

/* The four _dev calls of one (fep slot, chest slot) captured into a CUDA graph (Relaxed: the module calls cudaHostGetDevicePointer inside),
 * instantiated before any fence; then for three different inputs: stage on the host, replay, and compare with a plain (not captured) run of
 * the same inputs on the same buffers. Both models. */
static void graph_case(int force) {
  setenv("ISAC_GPU_FORCE_DISCRETE", force ? "1" : "0", 1);
  bool ok = false;
  GpuPassModule &m = GpuPassModule::get(&ok);
  ASSERT_TRUE(ok);
  GpuPassFixture in[3] = {GpuPassFixture::small_grant(24, 2, 4, 11), GpuPassFixture::small_grant(24, 2, 4, 12), GpuPassFixture::small_grant(24, 2, 4, 13)};
  GpuPassFixture::Pass P;
  ASSERT_TRUE(P.setup(m, in[0], /*nvar_fixed=*/400));
  std::map<std::pair<int, int>, cudaGraphExec_t> ge;
  GpuPassFixture::Out ref[3], rep[3];
  int rc_ref = 0, rc_rep = 0;
  for (int pass = 0; pass < 2; pass++) /* 0: plain reference of all three inputs, 1: graph replays */
    for (int i = 0; i < 3; i++) {
      ASSERT_TRUE(P.load(in[i]));
      int fs, cs, rc = 0;
      ASSERT_EQ(0, P.stage(&fs, &cs));
      if (pass == 0) {
        ASSERT_EQ(0, P.enqueue_dev(fs, cs, P.s, &rc));
        rc_ref = rc;
      } else {
        auto key = std::make_pair(fs, cs);
        if (!ge.count(key)) {
          cudaGraph_t g;
          ASSERT_EQ(cudaSuccess, cudaStreamBeginCapture(P.s, cudaStreamCaptureModeRelaxed));
          const int er = P.enqueue_dev(fs, cs, P.s, &rc);
          ASSERT_EQ(cudaSuccess, cudaStreamEndCapture(P.s, &g));
          ASSERT_EQ(0, er);
          ASSERT_EQ(cudaSuccess, cudaGraphInstantiate(&ge[key], g, 0));
          cudaGraphDestroy(g);
        } else
          rc = rc_ref;
        ASSERT_EQ(cudaSuccess, cudaGraphLaunch(ge[key], P.s));
        rc_rep = rc;
        EXPECT_EQ(rc_ref, rc_rep);
      }
      ASSERT_TRUE(P.fetch(pass == 0 ? &ref[i] : &rep[i], pass == 0 ? rc : rc_ref));
    }
  for (int i = 0; i < 3; i++) {
    EXPECT_TRUE(ref[i] == rep[i]) << "replayed graph differs from the plain run, input " << i << " force=" << force;
    EXPECT_FALSE(ref[i].llr == ref[(i + 1) % 3].llr) << "inputs should differ";
  }
  for (auto &e : ge) cudaGraphExecDestroy(e.second);
}
TEST(GpuPassStages, DevCapturableAndReplayable) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  graph_case(0);
  graph_case(1);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
}

/* Runs last: raises the allocation fence of the platform. A (BG, Z, R) the init did not build is refused and counted, nothing is
 * allocated or built on the launch path. */
extern "C" void nr_dl_gpu_memory_stats(uint64_t *, uint64_t *);
TEST(GpuPassStages, LdpcDevNoAllocOnMiss) {
  if (!have_gpu()) GTEST_SKIP();
  bool ok = false, lib = false;
  GpuPassModule &m = GpuPassModule::get(&ok, &lib);
  if (!lib) GTEST_SKIP() << "no ./libpdsch_gpu.so";
  ASSERT_TRUE(ok);
  void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_NOLOAD);
  auto misses = (unsigned (*)(void))dlsym(dl, "nr_pdsch_gpu_ldpc_dev_misses");
  auto stats = (void (*)(uint64_t *, uint64_t *))dlsym(dl, "nr_dl_gpu_memory_stats");
  ASSERT_TRUE(misses && stats);
  unsetenv("ISAC_GPU_FORCE_DISCRETE");
  nr_gpu_buf_t lin = {}, bits = {};
  ASSERT_EQ(0, nr_gpu_buf_alloc(&lin, NR_GPU_LDPC_DEV_IN_BYTES));
  ASSERT_EQ(0, nr_gpu_buf_alloc(&bits, NR_GPU_LDPC_DEV_BITS_BYTES));
  nr_gpu_rx_started(); /* the fence */
  uint64_t a0, d0, a1, d1;
  stats(&a0, &d0);
  const unsigned m0 = misses();
  nr_gpu_ldpc_job_t bad = {};
  bad.BG = 2; bad.R = 7; bad.Z = 64; bad.K = 640; bad.it_max = 8; /* R = 7 is no code rate of BG2: never built */
  EXPECT_EQ(-1, m.ldpc(&bad, 1, &lin, &bits, nullptr));
  EXPECT_EQ(m0 + 1, misses());
  nr_gpu_ldpc_job_t good = bad;
  good.R = 15;
  EXPECT_EQ(0, m.ldpc(&good, 1, &lin, &bits, nullptr)); /* a prepared key still launches behind the fence */
  cudaDeviceSynchronize();
  stats(&a1, &d1);
  EXPECT_EQ(a0, a1) << "the launch path allocated";
  EXPECT_EQ(d0, d1) << "an allocation was attempted behind the fence";
  EXPECT_EQ(m0 + 1, misses());
}

/* ---- Task 3: the host steps between the stages as kernels, each against the CPU function it replaces (OAI crc_byte.c, nr_rate_matching.c) ---- */
extern "C" {
void crcTableInit(void);
int check_crc(uint8_t *decoded_bytes, uint32_t n, uint8_t crc_type);
uint32_t crc24a(unsigned char *inptr, int bitlen);
uint32_t crc24b(unsigned char *inptr, int bitlen);
uint32_t crc16(unsigned char *inptr, int bitlen);
void nr_deinterleaving_ldpc(uint32_t E, uint8_t Qm, int16_t *e, int16_t *f);
int nr_rate_matching_ldpc_rx(uint32_t Tbslbrm, uint8_t BG, uint16_t Z, int16_t *d, int16_t *soft_input, uint8_t C, uint8_t rvidx, uint8_t clear,
                             uint32_t E, uint32_t F, uint32_t Foffset);
}
struct PostSyms {
  nr_gpu_pass_chest_post_fn chest = nullptr;
  nr_gpu_pass_chest_post64_fn chest64 = nullptr;
  nr_gpu_pass_rdm_fn rdm = nullptr;
  nr_gpu_pass_crc_fn crc = nullptr;
  static PostSyms &get() {
    static PostSyms p;
    static bool tried = false;
    if (!tried) {
      tried = true;
      void *dl = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
      if (dl) {
        p.chest = (nr_gpu_pass_chest_post_fn)dlsym(dl, "nr_gpu_pass_launch_chest_post");
        p.chest64 = (nr_gpu_pass_chest_post64_fn)dlsym(dl, "nr_gpu_pass_launch_chest_post_f64");
        p.rdm = (nr_gpu_pass_rdm_fn)dlsym(dl, "nr_gpu_pass_launch_rdm");
        p.crc = (nr_gpu_pass_crc_fn)dlsym(dl, "nr_gpu_pass_launch_crc");
      }
    }
    return p;
  }
};
/* device scratch: cudaMalloc'd once per test, copied synchronously (the kernels under test are the only thing launched on the stream) */
template <class T> struct Dev {
  T *p = nullptr; size_t n = 0;
  ~Dev() { cudaFree(p); }
  void up(const void *h, size_t bytes) { if (bytes > n) { cudaFree(p); cudaMalloc((void **)&p, bytes); n = bytes; } cudaMemcpy(p, h, bytes, cudaMemcpyHostToDevice); }
  void down(void *h, size_t bytes) { cudaMemcpy(h, p, bytes, cudaMemcpyDeviceToHost); }
};

/* one TB: random payload, CRC24A/16, split in C code blocks of pay bytes + CRC24B (valid, from the CPU crc functions); 10 % of the cases get bit flips */
struct CrcCase {
  uint32_t tbs, C, pay, nbits_cb;
  uint8_t tbtype;
  std::vector<uint8_t> bits;                  /* C blocks of ceil(nbits_cb/8) bytes */
  std::vector<nr_gpu_crc_seg_t> segs;
  static CrcCase random(int seed, bool unaligned) {
    std::mt19937 g(seed * 7919u + (unaligned ? 1 : 0));
    CrcCase c;
    c.tbs = 8 * (1 + g() % (g() % 4 == 0 ? 20000 : 480));          /* up to 160 kbit, mostly small */
    const uint32_t B = c.tbs + (c.tbs > 3824 ? 24 : 16);
    c.tbtype = c.tbs > 3824 ? 0 : 2;
    c.C = B <= 8448 ? 1 : (B + 8423) / 8424;
    std::vector<uint8_t> tb((B + 7) / 8 + 8);
    for (uint32_t i = 0; i < c.tbs / 8; i++) tb[i] = g();
    const uint32_t cr = c.tbtype == 0 ? crc24a(tb.data(), c.tbs) >> 8 : crc16(tb.data(), c.tbs) >> 16;
    const int cl = c.tbtype == 0 ? 3 : 2;
    for (int i = 0; i < cl; i++) tb[c.tbs / 8 + i] = (cr >> (8 * (cl - 1 - i))) & 255;
    const uint32_t Bb = B / 8;
    c.pay = c.C == 1 ? Bb : (Bb + c.C - 1) / c.C;
    c.nbits_cb = c.C == 1 ? B : (c.pay + 3) * 8;
    if (unaligned && c.C > 1) c.nbits_cb += g() % 7 + 1; /* check_crc on a bit length that is no whole number of bytes */
    const uint32_t stride = (c.nbits_cb + 7) / 8 + 4;
    c.bits.assign((size_t)c.C * stride, 0);
    for (uint32_t r = 0; r < c.C; r++) {
      uint8_t *b = &c.bits[(size_t)r * stride];
      for (uint32_t i = 0; i < c.pay; i++) b[i] = (r * c.pay + i) < tb.size() && r * c.pay + i < Bb ? tb[r * c.pay + i] : (uint8_t)g();
      if (c.C > 1) {
        const uint32_t v = crc24b(b, c.pay * 8) >> 8;
        for (int i = 0; i < 3; i++) b[c.pay + i] = (v >> (8 * (2 - i))) & 255;
      }
      c.segs.push_back({(uint32_t)(r * stride), c.nbits_cb, c.pay, c.C == 1 ? c.tbtype : 1u});
    }
    if (g() % 10 == 0) { /* corrupt: a few bit flips anywhere in the code blocks (payload, CRC or both) */
      for (int f = 1 + g() % 3; f > 0; f--) {
        const uint32_t r = g() % c.C;
        c.bits[(size_t)r * stride + g() % ((c.nbits_cb + 7) / 8)] ^= 1u << (g() % 8);
      }
    }
    return c;
  }
  int cpu_seg_ok(int r) {
    return check_crc(&bits[segs[r].off], segs[r].n_bits, (uint8_t)segs[r].type);
  }
  int cpu_tb_ok() {
    if (C == 1) return cpu_seg_ok(0);
    std::vector<uint8_t> b(C * pay + 8, 0);
    for (uint32_t r = 0; r < C; r++) memcpy(&b[(size_t)r * pay], &bits[segs[r].off], pay);
    return check_crc(b.data(), tbs + (tbs > 3824 ? 24 : 16), tbtype);
  }
};
TEST(GpuPassPost, CrcMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.crc) << "libpdsch_gpu.so has no nr_gpu_pass_launch_crc";
  crcTableInit();
  Dev<uint8_t> dbits, dok;
  Dev<nr_gpu_crc_seg_t> dsegs;
  int nbad_cpu = 0;
  for (int seed = 0; seed < 1000; seed++) {
    CrcCase c = CrcCase::random(seed, seed % 5 == 4);
    dbits.up(c.bits.data(), c.bits.size());
    dsegs.up(c.segs.data(), c.segs.size() * sizeof(c.segs[0]));
    dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
    ASSERT_EQ(0, S.crc(dbits.p, dsegs.p, (int)c.C, c.tbs, dok.p, dok.p + c.C, nullptr));
    std::vector<uint8_t> ok(c.C + 1);
    dok.down(ok.data(), c.C + 1);
    for (uint32_t r = 0; r < c.C; r++) {
      EXPECT_EQ(c.cpu_seg_ok(r), (int)ok[r]) << "seed " << seed << " seg " << r;
      nbad_cpu += !c.cpu_seg_ok(r);
    }
    EXPECT_EQ(c.cpu_tb_ok(), (int)ok[c.C]) << "seed " << seed;
    if (::testing::Test::HasFailure()) break;
  }
  EXPECT_GT(nbad_cpu, 20) << "the corrupted cases must exist";
}

/* nr_rate_matching_ldpc_rx path of prep_segment, transcribed (the host function is static): same calls, same pack */
static int cpu_prep(const nr_gpu_rdm_seg_t &s, const int16_t *llr, int8_t *l) {
  const int Kc = s.BG == 2 ? 52 : 68, Z = s.Z;
  std::vector<int16_t> e(s.E + 16), d((size_t)Kc * Z + 16, 0);
  { /* the two refusals of nr_rate_matching_ldpc_rx that call LOG_E (no logger in this test): same condition, not called */
    const uint32_t N = s.BG == 1 ? 66 * Z : 50 * Z, Ncb = s.Tbslbrm == 0 ? N : std::min(N, 3 * s.Tbslbrm / (2 * s.C));
    if (s.K - s.F - 2 * Z > Ncb) return -1;
  }
  nr_deinterleaving_ldpc(s.E, s.Qm, e.data(), const_cast<int16_t *>(llr + s.llr_off));
  if (nr_rate_matching_ldpc_rx(s.Tbslbrm, s.BG, Z, d.data(), e.data(), s.C, s.rv, 1, s.E, s.F, s.K - s.F - 2 * Z) == -1) return -1;
  std::vector<int16_t> z((size_t)Kc * Z + 64, 0);
  memset(z.data() + s.Kprime, 127, s.F * sizeof(int16_t));
  memcpy(z.data() + 2 * Z, d.data(), (s.Kprime - 2 * Z) * sizeof(int16_t));
  memcpy(z.data() + s.K, d.data() + (s.K - 2 * Z), (Kc * Z - s.K) * sizeof(int16_t));
  for (int i = 0; i < Kc * Z; i++) l[i] = (int8_t)(z[i] > 127 ? 127 : z[i] < -128 ? -128 : z[i]);
  return 0;
}
TEST(GpuPassPost, RateDematchMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.rdm) << "libpdsch_gpu.so has no nr_gpu_pass_launch_rdm";
  static const int zs[] = {2, 7, 11, 16, 22, 36, 64, 96, 128, 176, 208, 256, 320, 384};
  Dev<int16_t> dllr;
  Dev<nr_gpu_rdm_seg_t> dsegs;
  Dev<int8_t> dout;
  int n_rej = 0, n_rep = 0, n_lbrm = 0;
  for (int seed = 0; seed < 1000; seed++) {
    std::mt19937 g(seed * 104729u + 5);
    const int nseg = 1 + g() % 3;
    std::vector<nr_gpu_rdm_seg_t> segs(nseg);
    std::vector<int16_t> llr;
    for (auto &s : segs) {
      s.BG = 1 + g() % 2; s.Z = zs[g() % 14];
      const int Kb = s.BG == 1 ? 22 : 10, Kc = s.BG == 1 ? 68 : 52, N = (s.BG == 1 ? 66 : 50) * s.Z;
      s.K = Kb * s.Z;
      s.F = g() % 3 == 0 ? 0 : g() % (s.K / 4 + 1);
      s.Kprime = s.K - s.F;
      s.C = 1 + g() % 40;
      s.rv = g() % 4;
      s.Qm = 2 * (1 + g() % 4);
      const uint32_t Ncb = g() % 3 ? N : (uint32_t)(s.K / 2 + g() % (N - s.K / 2) + 1); /* LBRM binds in a third of the cases */
      s.Tbslbrm = Ncb == (uint32_t)N ? (g() % 2 ? 0 : 1000000) : (uint32_t)((uint64_t)Ncb * 2 * s.C / 3 + 1);
      s.E = s.Qm * (1 + g() % ((g() % 4 == 0 ? 3 * N : N) / s.Qm)); /* up to 3 N: repetition */
      s.llr_off = (uint32_t)llr.size();
      const int amp = g() % 3 == 0 ? 32767 : g() % 2 ? 60 : 3000; /* full range wraps the int16 sum, small stays unsaturated */
      for (uint32_t i = 0; i < s.E; i++) llr.push_back((int16_t)((int)(g() % (2 * amp + 1)) - amp));
      (void)Kc;
    }
    dllr.up(llr.data(), llr.size() * 2);
    dsegs.up(segs.data(), nseg * sizeof(segs[0]));
    const size_t ob = (size_t)nseg * NR_GPU_LDPC_DEV_IN_STRIDE;
    dout.up(std::vector<int8_t>(ob, 55).data(), ob);
    ASSERT_EQ(0, S.rdm(dllr.p, dsegs.p, nseg, dout.p, nullptr));
    std::vector<int8_t> got(ob);
    dout.down(got.data(), ob);
    for (int i = 0; i < nseg; i++) {
      const int Kc = segs[i].BG == 2 ? 52 : 68;
      std::vector<int8_t> ref((size_t)Kc * segs[i].Z + 16, 0);
      const int rc = cpu_prep(segs[i], llr.data(), ref.data());
      if (rc != 0) { std::fill(ref.begin(), ref.end(), 0); n_rej++; }
      n_rep += segs[i].E > (segs[i].BG == 1 ? 66 : 50) * segs[i].Z;
      n_lbrm += segs[i].Tbslbrm != 0 && segs[i].Tbslbrm != 1000000;
      ASSERT_EQ(0, memcmp(ref.data(), &got[(size_t)i * NR_GPU_LDPC_DEV_IN_STRIDE], (size_t)Kc * segs[i].Z))
          << "seed " << seed << " seg " << i << " BG" << segs[i].BG << " Z" << segs[i].Z << " Qm" << segs[i].Qm << " rv" << segs[i].rv << " E" << segs[i].E
          << " F" << segs[i].F << " Tbslbrm " << segs[i].Tbslbrm << " C" << segs[i].C;
    }
  }
  printf("rate dematch: %d CPU-refused segments, %d with repetition, %d with LBRM\n", n_rej, n_rep, n_lbrm);
  EXPECT_GT(n_rep, 50);
  EXPECT_GT(n_lbrm, 50);
}

/* the CPU math, transcribed from passive_decode_body_core (nvar normalisation + per-branch substitution, DM-RS FO/SFO), FP64 */
static double rel(double a, double b) { return b != 0.0 ? fabs(a - b) / fabs(b) : fabs(a); }
struct ChestRef {
  uint32_t nvar, flags;
  double cfo_hz, sfo_ppm, sfo_dk, coh_gate, coherence;
  int32_t kn0, kn1;
  int64_t re[2], im[2], ksum[2];
  double magsum[2];
};
static ChestRef chest_finalize(const nr_gpu_chest_post_par_t &p, const int64_t *re_, const int64_t *im_, const int64_t *ks_, const double *mag_, int kn0, int kn1);
static ChestRef chest_ref(const nr_gpu_chest_post_par_t &p) {
  ChestRef o = {};
  uint32_t nvar = 0, nvar_ant[8] = {0};
  const int nk = p.n_dmrs_sym * p.nl;
  for (int k = 0; k < nk; k++) {
    const uint32_t *q = &p.nvar_rows[k * p.nrx];
    uint64_t acc = 0; int cnt = 0;
    for (int a = 0; a < p.nrx; a++) if (q[a] > 0) { acc += q[a]; cnt++; }
    nvar += cnt > 0 ? (uint32_t)(acc / (uint64_t)cnt) : 0;
    for (int a = 0; a < 8; a++) nvar_ant[a] = a < p.nrx ? q[a] : 0;
  }
  const uint32_t den = p.nvfix ? (uint32_t)(p.n_dmrs_sym * p.nl) : (uint32_t)(p.number_symbols * p.nl * p.nrx);
  const uint32_t nvar_den = den > 0 ? den : 1u;
  nvar /= nvar_den;
  if (p.branch >= 0 && p.branch < 8 && nvar_ant[p.branch] > 0) {
    const uint64_t scaled = (uint64_t)nvar_ant[p.branch] * (uint64_t)(p.n_dmrs_sym * p.nl);
    const uint32_t nvar_branch = (uint32_t)(scaled / nvar_den);
    if (nvar_branch > 0) nvar = nvar_branch;
  }
  o.nvar = nvar;
  if (!p.h0) return o;
  const uint32_t N = p.N;
  const struct { int16_t r, i; } *h0 = (const decltype(h0))p.h0, *h1 = (const decltype(h1))p.h1;
  uint32_t kmin = N, kmax = 0;
  for (uint32_t k = 0; k < N; k++)
    if (h0[k].r != 0 || h0[k].i != 0 || h1[k].r != 0 || h1[k].i != 0) { if (k < kmin) kmin = k; if (k > kmax) kmax = k; }
  const uint32_t kmid = (kmin <= kmax) ? (kmin + kmax) / 2 : 0;
  double re[2] = {0, 0}, im[2] = {0, 0}, ksum[2] = {0, 0}, magsum[2] = {0, 0};
  uint32_t kn[2] = {0, 0};
  for (uint32_t k = 0; k < N; k++) {
    const double rr = (double)h1[k].r * h0[k].r + (double)h1[k].i * h0[k].i;
    const double ii = (double)h1[k].i * h0[k].r - (double)h1[k].r * h0[k].i;
    if (rr == 0.0 && ii == 0.0) continue;
    const int h = (k <= kmid) ? 0 : 1;
    re[h] += rr; im[h] += ii; ksum[h] += (double)k;
    magsum[h] += sqrt(((double)h1[k].r * h1[k].r + (double)h1[k].i * h1[k].i) * ((double)h0[k].r * h0[k].r + (double)h0[k].i * h0[k].i));
    kn[h]++;
  }
  int64_t rei[2] = {(int64_t)re[0], (int64_t)re[1]}, imi[2] = {(int64_t)im[0], (int64_t)im[1]}, ksi[2] = {(int64_t)ksum[0], (int64_t)ksum[1]};
  const double mg[2] = {magsum[0], magsum[1]};
  const ChestRef f = chest_finalize(p, rei, imi, ksi, mg, kn[0], kn[1]);
  o.flags = f.flags; o.cfo_hz = f.cfo_hz; o.sfo_ppm = f.sfo_ppm; o.sfo_dk = f.sfo_dk; o.coh_gate = f.coh_gate; o.coherence = f.coherence;
  o.kn0 = f.kn0; o.kn1 = f.kn1;
  for (int h = 0; h < 2; h++) { o.re[h] = rei[h]; o.im[h] = imi[h]; o.ksum[h] = ksi[h]; o.magsum[h] = mg[h]; }
  return o;
}
/* the scalar lines after the sums, verbatim from the CPU */
static ChestRef chest_finalize(const nr_gpu_chest_post_par_t &p, const int64_t *re_, const int64_t *im_, const int64_t *ks_, const double *mag_, int kn0, int kn1) {
  ChestRef o = {};
  const double dt_d = p.dt_d;
  const double re[2] = {(double)re_[0], (double)re_[1]}, im[2] = {(double)im_[0], (double)im_[1]}, ksum[2] = {(double)ks_[0], (double)ks_[1]};
  const double magsum[2] = {mag_[0], mag_[1]};
  const uint32_t kn[2] = {(uint32_t)kn0, (uint32_t)kn1};
  const double coh_num = sqrt((re[0] + re[1]) * (re[0] + re[1]) + (im[0] + im[1]) * (im[0] + im[1]));
  const double coh_den = magsum[0] + magsum[1];
  const double coherence = (coh_den > 0.0) ? (coh_num / coh_den) : 0.0;
  const double coh_h[2] = {(magsum[0] > 0.0) ? sqrt(re[0] * re[0] + im[0] * im[0]) / magsum[0] : 0.0,
                           (magsum[1] > 0.0) ? sqrt(re[1] * re[1] + im[1] * im[1]) / magsum[1] : 0.0};
  const double coh_min = (coh_h[0] < coh_h[1]) ? coh_h[0] : coh_h[1];
  const bool halves_usable = (kn[0] >= 8 && kn[1] >= 8);
  const double coh_gate = halves_usable ? coh_min : coherence;
  o.kn0 = kn[0]; o.kn1 = kn[1]; o.coherence = coherence; o.coh_gate = coh_gate;
  if (kn[0] + kn[1] > 0 && coh_gate < 0.30) o.flags |= NR_GPU_POST_FO_REJECTED;
  if (kn[0] + kn[1] > 0 && coh_gate >= 0.30) {
    o.flags |= NR_GPU_POST_FO_VALID;
    o.cfo_hz = atan2(im[0] + im[1], re[0] + re[1]) / (2.0 * M_PI * dt_d);
    if (kn[0] > 16 && kn[1] > 16 && coh_h[0] >= 0.30 && coh_h[1] >= 0.30) {
      const double p_lo = atan2(im[0], re[0]), p_hi = atan2(im[1], re[1]);
      const double k_lo = ksum[0] / (double)kn[0], k_hi = ksum[1] / (double)kn[1];
      double dphi = p_hi - p_lo;
      while (dphi > M_PI) dphi -= 2.0 * M_PI;
      while (dphi < -M_PI) dphi += 2.0 * M_PI;
      const double dk = k_hi - k_lo;
      if (dk != 0.0) {
        o.sfo_ppm = -(dphi / dk) / (2.0 * M_PI * p.df_d * dt_d) * 1.0e6;
        o.sfo_dk = (dk < 0.0) ? -dk : dk;
        o.flags |= NR_GPU_POST_SFO_MEASURED;
      }
    }
  }
  return o;
}
TEST(GpuPassPost, ChestPostMatchesCpu) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.chest && S.chest64) << "libpdsch_gpu.so has no nr_gpu_pass_launch_chest_post[_f64]";
  Dev<uint32_t> drows;
  Dev<int16_t> dh;
  Dev<nr_gpu_chest_post_out_t> dof;
  Dev<nr_gpu_chest_post_out64_t> do64;
  double worst = 0;
  int n_sfo = 0, n_rej = 0, n_exact = 0;
  double rel64_max = 0;
  for (int seed = 0; seed < 1000; seed++) {
    std::mt19937 g(seed * 31337u + 3);
    std::normal_distribution<double> G(0, 1);
    nr_gpu_chest_post_par_t p = {};
    p.nrx = 1 << (g() % 3); p.nl = 1 + g() % 4; p.n_dmrs_sym = 2 + g() % 2;
    p.number_symbols = 6 + g() % 8; p.nvfix = g() % 2; p.branch = g() % 3 == 0 ? -1 : (int)(g() % p.nrx);
    std::vector<uint32_t> rows(p.n_dmrs_sym * p.nl * p.nrx);
    for (auto &r : rows) r = g() % 5 == 0 ? 0 : g() % (g() % 2 ? 100000 : 3000000);
    const int N = 4096;
    p.N = N; p.n_rb_dl = 273; p.df_d = g() % 2 ? 30000.0 : 15000.0;
    p.dt_d = 1.0e-3 / (p.df_d == 30000.0 ? 2 : 1) / 14 * (4 + g() % 8);
    p.df_f = (float)p.df_d; p.dt_f = (float)p.dt_d;
    const int nrb = 1 + g() % 273, start = g() % (273 - nrb + 1) * 12 % (N - 12 * nrb);
    const double cfo = (G(g) * 80) * 2 * M_PI * p.dt_d, sfo = G(g) * 8e-6 * 2 * M_PI * p.df_d * p.dt_d;
    const double amp = 20.0 + g() % 9000, noise = g() % 4 == 0 ? 6.0 : g() % 2 ? 0.1 : 0.5;
    std::vector<int16_t> h0c(2 * N, 0), h1c(2 * N, 0);
    for (int k = 0; k < nrb * 12; k++) {
      const std::complex<double> a(G(g) * amp, G(g) * amp);
      const std::complex<double> b = a * std::polar(1.0, cfo - sfo * k) + std::complex<double>(G(g), G(g)) * (noise * amp);
      auto cl = [](double v) { return (int16_t)std::max(-32768.0, std::min(32767.0, std::round(v))); };
      h0c[2 * (start + k)] = cl(a.real()); h0c[2 * (start + k) + 1] = cl(a.imag());
      h1c[2 * (start + k)] = cl(b.real()); h1c[2 * (start + k) + 1] = cl(b.imag());
    }
    if (g() % 8 == 0) { h0c.assign(2 * N, 0); h1c.assign(2 * N, 0); } /* no estimate at all */
    std::vector<int16_t> hh(h0c); hh.insert(hh.end(), h1c.begin(), h1c.end());
    drows.up(rows.data(), rows.size() * 4);
    dh.up(hh.data(), hh.size() * 2);
    p.nvar_rows = drows.p; p.h0 = dh.p; p.h1 = dh.p + 2 * N;
    nr_gpu_chest_post_par_t pc = p; pc.nvar_rows = rows.data(); pc.h0 = h0c.data(); pc.h1 = h1c.data();
    const ChestRef ref = chest_ref(pc);
    nr_gpu_chest_post_out_t of; nr_gpu_chest_post_out64_t o64;
    do64.up(&o64, sizeof o64); dof.up(&of, sizeof of);
    ASSERT_EQ(0, S.chest(&p, dof.p, nullptr));
    ASSERT_EQ(0, S.chest64(&p, do64.p, nullptr));
    dof.down(&of, sizeof of); do64.down(&o64, sizeof o64);
    n_sfo += (ref.flags & NR_GPU_POST_SFO_MEASURED) != 0; n_rej += (ref.flags & NR_GPU_POST_FO_REJECTED) != 0;
    /* integer part: exact in both builds */
    ASSERT_EQ(1u, of.nvar_valid); ASSERT_EQ(1u, o64.nvar_valid);
    ASSERT_EQ(ref.nvar, of.nvar) << seed; ASSERT_EQ(ref.nvar, o64.nvar) << seed;
    ASSERT_EQ(ref.flags, of.flags) << seed; ASSERT_EQ(ref.flags, o64.flags) << seed;
    ASSERT_EQ(ref.kn0, of.kn0); ASSERT_EQ(ref.kn1, of.kn1); ASSERT_EQ(ref.kn0, o64.kn0); ASSERT_EQ(ref.kn1, o64.kn1);
    /* FP64 build: bit-equal, but for the two atan2() results (cfo, sfo): CUDA's atan2 is not glibc's (last bits, see the header) */
    ASSERT_EQ(ref.re[0], o64.re[0]); ASSERT_EQ(ref.re[1], o64.re[1]); ASSERT_EQ(ref.im[0], o64.im[0]); ASSERT_EQ(ref.im[1], o64.im[1]);
    ASSERT_EQ(ref.ksum[0], o64.ksum[0]); ASSERT_EQ(ref.ksum[1], o64.ksum[1]); ASSERT_EQ(ref.magsum[0], o64.magsum[0]) << seed; ASSERT_EQ(ref.magsum[1], o64.magsum[1]) << seed;
    ASSERT_EQ(ref.sfo_dk, o64.sfo_dk) << seed; ASSERT_EQ(ref.coh_gate, o64.coh_gate) << seed; ASSERT_EQ(ref.coherence, o64.coherence) << seed;
    {
      const ChestRef fin = chest_finalize(pc, o64.re, o64.im, o64.ksum, o64.magsum, o64.kn0, o64.kn1);
      ASSERT_EQ(ref.cfo_hz, fin.cfo_hz) << seed; ASSERT_EQ(ref.sfo_ppm, fin.sfo_ppm) << seed; /* the host half on the device sums: the CPU's bits */
    }
    n_exact += ref.cfo_hz == o64.cfo_hz && ref.sfo_ppm == o64.sfo_ppm;
    rel64_max = std::max(rel64_max, std::max(rel(o64.cfo_hz, ref.cfo_hz), rel(o64.sfo_ppm, ref.sfo_ppm)));
    /* float build: 1e-6 relative */
    const double e[5] = {rel(of.cfo_hz, ref.cfo_hz), rel(of.sfo_ppm, ref.sfo_ppm), rel(of.sfo_dk, ref.sfo_dk), rel(of.coh_gate, ref.coh_gate), rel(of.coherence, ref.coherence)};
    for (double x : e) { if (x > worst) worst = x; }
    for (int i = 0; i < 5; i++) EXPECT_LE(e[i], 1e-6) << "seed " << seed << " field " << i;
    if (::testing::Test::HasFailure()) break;
  }
  printf("chest post: worst float relative error %.3g, %d SFO measured, %d rejected; FP64 atan2-derived cfo/sfo bit-equal in %d of 1000 grants, worst relative difference %.3g\n", worst, n_sfo, n_rej, n_exact, rel64_max);
  EXPECT_LE(rel64_max, 1e-12); /* glibc vs CUDA atan2: the last bit of two angles, amplified by the cancellation in their difference */
  EXPECT_GT(n_sfo, 200);
  EXPECT_GT(n_rej, 20);
}

/* the launch wrappers enqueue only: all four captured into one graph (Global mode: any sync/alloc/copy in them would fail the capture), replayed twice */
TEST(GpuPassPost, LaunchersAreGraphCapturable) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.chest && S.chest64 && S.rdm && S.crc);
  crcTableInit();
  CrcCase c = CrcCase::random(3, false);
  Dev<uint8_t> dbits, dok;
  Dev<nr_gpu_crc_seg_t> dsegs;
  dbits.up(c.bits.data(), c.bits.size());
  dsegs.up(c.segs.data(), c.segs.size() * sizeof(c.segs[0]));
  dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
  nr_gpu_rdm_seg_t rs = {0, 96, 2, 2, 8, 22 * 8, 0, 22 * 8, 0, 1, 0};
  std::vector<int16_t> llr(96, 5);
  Dev<int16_t> dllr; Dev<nr_gpu_rdm_seg_t> drs; Dev<int8_t> dout;
  dllr.up(llr.data(), 192); drs.up(&rs, sizeof rs); dout.up(std::vector<int8_t>(NR_GPU_LDPC_DEV_IN_STRIDE, 9).data(), NR_GPU_LDPC_DEV_IN_STRIDE);
  uint32_t rows[8] = {100, 200, 0, 400, 100, 200, 300, 400};
  Dev<uint32_t> drows; drows.up(rows, sizeof rows);
  Dev<nr_gpu_chest_post_out_t> dof; Dev<nr_gpu_chest_post_out64_t> do64;
  nr_gpu_chest_post_out_t zf = {}; nr_gpu_chest_post_out64_t z64 = {};
  dof.up(&zf, sizeof zf); do64.up(&z64, sizeof z64);
  nr_gpu_chest_post_par_t p = {};
  p.nvar_rows = drows.p; p.nrx = 4; p.nl = 1; p.n_dmrs_sym = 2; p.number_symbols = 12; p.branch = -1; p.N = 4096; p.dt_d = 3e-4; p.df_d = 30000; p.dt_f = 3e-4f; p.df_f = 30000.f;
  cudaStream_t st; ASSERT_EQ(cudaSuccess, cudaStreamCreate(&st));
  cudaGraph_t g; cudaGraphExec_t ge;
  ASSERT_EQ(cudaSuccess, cudaStreamBeginCapture(st, cudaStreamCaptureModeGlobal));
  EXPECT_EQ(0, S.crc(dbits.p, dsegs.p, (int)c.C, c.tbs, dok.p, dok.p + c.C, st));
  EXPECT_EQ(0, S.rdm(dllr.p, drs.p, 1, dout.p, st));
  EXPECT_EQ(0, S.chest(&p, dof.p, st));
  EXPECT_EQ(0, S.chest64(&p, do64.p, st));
  ASSERT_EQ(cudaSuccess, cudaStreamEndCapture(st, &g));
  ASSERT_EQ(cudaSuccess, cudaGraphInstantiate(&ge, g, 0));
  for (int rep = 0; rep < 2; rep++) {
    ASSERT_EQ(cudaSuccess, cudaGraphLaunch(ge, st));
    ASSERT_EQ(cudaSuccess, cudaStreamSynchronize(st));
  }
  std::vector<uint8_t> ok(c.C + 1);
  dok.down(ok.data(), c.C + 1);
  for (uint32_t r = 0; r < c.C; r++) EXPECT_EQ(c.cpu_seg_ok(r), (int)ok[r]);
  EXPECT_EQ(c.cpu_tb_ok(), (int)ok[c.C]);
  nr_gpu_chest_post_out_t of; do64.down(&z64, sizeof z64); dof.down(&of, sizeof of);
  EXPECT_EQ(of.nvar, z64.nvar);
  EXPECT_GT(of.nvar, 0u);
  cudaGraphExecDestroy(ge); cudaGraphDestroy(g); cudaStreamDestroy(st);
}

/* I1/I2 of the review: no DM-RS rows leaves nvar untouched and says so; a TB without reassembly information is refused without dividing */
TEST(GpuPassPost, DegenerateInputs) {
  if (!have_gpu()) GTEST_SKIP();
  PostSyms &S = PostSyms::get();
  ASSERT_TRUE(S.chest && S.crc);
  Dev<uint32_t> drows; uint32_t rows[4] = {1, 2, 3, 4}; drows.up(rows, sizeof rows);
  nr_gpu_chest_post_par_t p = {};
  p.nvar_rows = drows.p; p.nrx = 4; p.nl = 1; p.n_dmrs_sym = 0; p.number_symbols = 12; p.branch = -1;
  nr_gpu_chest_post_out_t o; memset(&o, 0x5a, sizeof o); o.nvar = 0xdeadbeef; o.nvar_valid = 7;
  Dev<nr_gpu_chest_post_out_t> dof; dof.up(&o, sizeof o);
  ASSERT_EQ(0, S.chest(&p, dof.p, nullptr));
  dof.down(&o, sizeof o);
  EXPECT_EQ(0u, o.nvar_valid);
  EXPECT_EQ(0xdeadbeefu, o.nvar);
  crcTableInit();
  CrcCase c = CrcCase::random(11, false);
  while (c.C < 2) c = CrcCase::random(c.tbs + 12, false);
  Dev<uint8_t> dbits, dok; Dev<nr_gpu_crc_seg_t> dsegs;
  auto segs = c.segs; segs[0].pay_bytes = 0;
  dbits.up(c.bits.data(), c.bits.size()); dsegs.up(segs.data(), segs.size() * sizeof(segs[0]));
  dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
  ASSERT_EQ(0, S.crc(dbits.p, dsegs.p, (int)c.C, c.tbs, dok.p, dok.p + c.C, nullptr));
  std::vector<uint8_t> ok(c.C + 1); dok.down(ok.data(), c.C + 1);
  EXPECT_EQ(0, (int)ok[c.C]);
  for (uint32_t r = 0; r < c.C; r++) EXPECT_EQ(c.cpu_seg_ok(r), (int)ok[r]);
  dok.up(std::vector<uint8_t>(c.C + 1, 7).data(), c.C + 1);
  ASSERT_EQ(0, S.crc(dbits.p, dsegs.p, 0, c.tbs, dok.p, dok.p + c.C, nullptr)); /* nseg == 0 */
  dok.down(ok.data(), c.C + 1);
  EXPECT_EQ(0, (int)ok[c.C]);
}
