/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Tests of the CB0 batch entry of libldpc_cuda.so (nrLDPC_cb0_cuda.h). Needs a CUDA device.
 *
 * References:
 *  - a CPU model of the GPU decoder (G1's flooding normalised min-sum, the same int8 saturation, per-thread edge
 *    partition and tie order, syndrome early termination): bits, iterations and early-termination flag must be
 *    bit-exact for every item, including the ones that do not converge;
 *  - OAI check_crc on the returned bits: the GPU CRC verdict must equal it for every item;
 *  - G1's TB pool (ldpc_pool_decode) on the same inputs: identical bits;
 *  - OAI's CPU TB decoder (libldpc.so) on clean codewords: same verdict, bits equal to the random source.
 * Every codeword has a random payload and a real CRC (crcTableInit() is called by the helper's loader). */
#include <gtest/gtest.h>
#include <cuda_runtime.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <climits>
#include <vector>
#include "ldpc_cuda_pool_test_helper.h"
#include "../nrLDPC_cb0_cuda.h"
#include "../ldpc_tables_bg1.h"
#include "../ldpc_tables_bg2.h"

extern "C" int check_crc(uint8_t *decoded_bytes, uint32_t n, uint8_t crc_type);

namespace {

/* ---- the plugin's CB0 API (dlsym) ---- */
struct Api {
  int (*init)(void);
  int (*submit)(const ldpc_cb0_item_t *, int, const int8_t *, size_t, int, ldpc_cb0_ticket_t **);
  int (*collect)(ldpc_cb0_ticket_t *, ldpc_cb0_result_t *, uint8_t *, size_t);
  int (*decode)(const ldpc_cb0_item_t *, int, const int8_t *, size_t, ldpc_cb0_result_t *, uint8_t *, size_t);
  int (*mode)(void);
  void (*counters)(ldpc_cb0_counters_t *);
  void (*hooks)(int, int, int, int, int);
  void (*reset)(void);
  int (*set_mem)(int);
};
Api A;

const size_t STRIDE = 68 * 384; /* input stride used by the tests */

/* ---- CPU model of the GPU decoder ---- */
struct HostBG {
  uint32_t rows, cols, cn_stride, vn_stride;
  const uint32_t *cn_deg, *vn_deg, *cn, *vn;
};
HostBG host_bg(int BG, int Z)
{
  static const uint32_t s_val[8][8] = {{2, 4, 8, 16, 32, 64, 128, 256}, {3, 6, 12, 24, 48, 96, 192, 384},
                                       {5, 10, 20, 40, 80, 160, 320},   {7, 14, 28, 56, 112, 224},
                                       {9, 18, 36, 72, 144, 288},       {11, 22, 44, 88, 176, 352},
                                       {13, 26, 52, 104, 208},          {15, 30, 60, 120, 240}};
  int ils = -1;
  for (int i = 0; i < 8 && ils < 0; i++)
    for (int j = 0; j < 8; j++)
      if (s_val[i][j] == (uint32_t)Z) {
        ils = i;
        break;
      }
  const void *cn[2][8] = {{BG1_CN_TABLE()}, {BG2_CN_TABLE()}};
  const void *vn[2][8] = {{BG1_VN_TABLE()}, {BG2_VN_TABLE()}};
  const uint32_t *cnd[2][8] = {{BG1_CN_DEGREE_TABLE()}, {BG2_CN_DEGREE_TABLE()}};
  const uint32_t *vnd[2][8] = {{BG1_VN_DEGREE_TABLE()}, {BG2_VN_DEGREE_TABLE()}};
  const uint32_t cns[2][8] = {{BG1_CN_TABLE(sizeof)}, {BG2_CN_TABLE(sizeof)}};
  const uint32_t vns[2][8] = {{BG1_VN_TABLE(sizeof)}, {BG2_VN_TABLE(sizeof)}};
  HostBG h;
  h.rows = BG == 1 ? 46 : 42;
  h.cols = BG == 1 ? 68 : 52;
  h.cn = (const uint32_t *)cn[BG - 1][ils];
  h.vn = (const uint32_t *)vn[BG - 1][ils];
  h.cn_deg = cnd[BG - 1][ils];
  h.vn_deg = vnd[BG - 1][ils];
  h.cn_stride = cns[BG - 1][ils] / (4 * h.rows);
  h.vn_stride = vns[BG - 1][ils] / (4 * h.cols);
  return h;
}

struct ModelOut {
  std::vector<uint8_t> bits;
  int iters;
  bool converged;
};
ModelOut model_decode(int BG, int Z, const int8_t *ch, int num_iter, int K)
{
  const HostBG g = host_bg(BG, Z);
  const int U = 4, R = g.rows, Cc = g.cols;
  std::vector<int8_t> msg((size_t)R * Cc * Z, 0), total((size_t)Cc * Z, 0);
  const int8_t *lt = ch;
  ModelOut o;
  o.iters = num_iter;
  o.converged = false;
  for (int it = 0; it < num_iter; it++) {
    for (int row = 0; row < R; row++) {
      const uint32_t deg = g.cn_deg[row];
      const uint32_t *e = &g.cn[row * g.cn_stride];
      for (int i = 0; i < Z; i++) {
        int m1[4], m2[4], im[4];
        uint32_t sg = 0;
        for (int y = 0; y < U; y++) {
          m1[y] = m2[y] = INT_MAX;
          im[y] = -1;
          for (uint32_t ii = y; ii < deg; ii += U) {
            const uint32_t col = e[ii] & 0xffff, s = e[ii] >> 16;
            const int idx = (row + col * R) * Z + i;
            int t = lt[col * Z + (i + s) % Z];
            if (it)
              t -= msg[idx];
            sg |= (uint32_t)(t < 0) << ii;
            const int a = abs(t);
            if (a < m1[y]) {
              m2[y] = m1[y];
              m1[y] = a;
              im[y] = idx;
            } else if (a < m2[y])
              m2[y] = a;
          }
        }
        int a1 = INT_MAX, a2 = INT_MAX, ai = -1;
        for (int y = 0; y < U; y++) {
          if (m1[y] < a1) {
            a2 = a1;
            a1 = m1[y];
            ai = im[y];
          } else if (m1[y] < a2)
            a2 = m1[y];
          a2 = std::min(a2, m2[y]);
        }
        const int ns = (__builtin_popcount(sg) & 1) ? -1 : 1;
        a1 = std::min(std::max(a1 * 3 / 4, -127), 127);
        a2 = std::min(std::max(a2 * 3 / 4, -127), 127);
        for (uint32_t ii = 0; ii < deg; ii++) {
          const uint32_t col = e[ii] & 0xffff;
          const int idx = (row + col * R) * Z + i;
          const int mv = idx == ai ? a2 : a1;
          msg[idx] = (int8_t)(mv * ns * (((sg >> ii) & 1) ? -1 : 1));
        }
      }
    }
    for (int col = 0; col < Cc; col++) {
      const uint32_t deg = g.vn_deg[col];
      const uint32_t *e = &g.vn[col * g.vn_stride];
      for (int i = 0; i < Z; i++) {
        int sum = 0;
        for (int y = 0; y < U; y++) {
          int p = 0;
          for (uint32_t j = y; j < deg; j += U) {
            const uint32_t row = e[j] & 0xffff, s = e[j] >> 16;
            p += msg[(row + col * R) * Z + (uint32_t)(i - s + (Z << 8)) % Z];
          }
          if (y == 0)
            p += ch[col * Z + i];
          sum += std::min(std::max(p, -127), 127);
        }
        total[col * Z + i] = (int8_t)std::min(std::max(sum, -127), 127);
      }
    }
    lt = total.data();
    if (it + 1 < num_iter) { /* syndrome early termination, as batch_syndrome_kernel */
      bool unsat = false;
      for (int row = 0; row < R && !unsat; row++) {
        const uint32_t *e = &g.cn[row * g.cn_stride];
        for (int i = 0; i < Z && !unsat; i++) {
          int par = 0;
          for (uint32_t ii = 0; ii < g.cn_deg[row]; ii++)
            par ^= lt[(e[ii] & 0xffff) * Z + (i + (e[ii] >> 16)) % Z] < 0;
          unsat = par;
        }
      }
      if (!unsat) {
        o.iters = it + 1;
        o.converged = true;
        break;
      }
    }
  }
  o.bits.assign((K + 7) / 8, 0);
  for (int k = 0; k < K; k++)
    if (lt[k] < 0)
      o.bits[k / 8] |= 0x80 >> (k % 8);
  return o;
}

/* ---- test batches ---- */
struct Batch {
  std::vector<ldpc_cb0_item_t> it;
  std::vector<int8_t> l;            /* n * STRIDE */
  std::vector<std::vector<uint8_t>> src;
  std::vector<int> kind;            /* 0 truth, 1 noise, 2 truth with corrupted CRC */
  int n() const { return (int)it.size(); }
};
/* (bg1, Z, crc_type) shapes; K = 22Z / 10Z, Z % 4 == 0 */
struct Shape {
  int bg1, Z, crc; /* bg1: 1 = BG1, 0 = BG2 */
};
const Shape SHAPES[] = {{1, 384, 0}, {1, 208, 0}, {1, 64, 2}, {1, 36, 2}, {0, 384, 0}, {0, 96, 2}, {0, 52, 2}, {0, 16, 2},
                        {1, 384, 1}, {0, 176, 1}};

void add_item(Batch &b, const Shape &s, int kind, double ebn0, int iters, int guard)
{
  const int Kc = s.bg1 ? 68 : 52, K = (s.bg1 ? 22 : 10) * s.Z;
  const size_t at = b.l.size();
  b.l.resize(at + STRIDE, 0);
  std::vector<uint8_t> src(K / 8 + 8);
  if (kind == 1) {
    for (int k = 0; k < Kc * s.Z; k++)
      b.l[at + k] = (int8_t)(k < 2 * s.Z ? 0 : (lrand48() % 81) - 40);
  } else {
    ASSERT_EQ(lcp_make_cw(s.bg1, s.Z, s.crc, ebn0, kind == 2, src.data(), &b.l[at], nullptr), K);
  }
  ldpc_cb0_item_t x = {};
  x.BG = s.bg1 ? 1 : 2;
  x.Z = (uint16_t)s.Z;
  x.K = (uint16_t)K;
  x.Kprime = (uint16_t)K;
  x.crc_type = (uint8_t)s.crc;
  x.iters = (uint8_t)iters;
  x.guard_bytes = (uint16_t)guard;
  b.it.push_back(x);
  b.src.push_back(src);
  b.kind.push_back(kind);
}

/* mixed shapes / iterations / kinds; truths at an SNR where some need many iterations and a few fail */
Batch mixed_batch(int n, long seed, bool small_only = false)
{
  srand48(seed);
  Batch b;
  const int its[] = {16, 8, 20, 4};
  for (int i = 0; i < n; i++) {
    const Shape &s = SHAPES[small_only ? 3 + i % 5 : i % 10];
    const int kind = i % 5 == 4 ? 1 : (i % 7 == 6 ? 2 : 0);
    const double snr = (i % 3 == 0) ? 1.5 : (i % 3 == 1 ? 2.5 : 4.0);
    add_item(b, s, kind, snr, its[(i / 10) % 4], (i % 2) ? (s.bg1 ? 22 : 10) * s.Z / 8 - 3 : 0);
  }
  return b;
}

std::vector<ldpc_cb0_result_t> run(const Batch &b, std::vector<uint8_t> *bits = nullptr, int *rc = nullptr)
{
  std::vector<ldpc_cb0_result_t> r(b.n());
  if (bits)
    bits->assign((size_t)b.n() * LDPC_CB0_BITS_STRIDE, 0);
  const int e = A.decode(b.it.data(), b.n(), b.l.data(), STRIDE, r.data(), bits ? bits->data() : nullptr, LDPC_CB0_BITS_STRIDE);
  if (rc)
    *rc = e;
  else
    EXPECT_EQ(e, 0);
  return r;
}

void expect_crc_matches_oai(const Batch &b, const std::vector<ldpc_cb0_result_t> &r, const std::vector<uint8_t> &bits)
{
  for (int i = 0; i < b.n(); i++) {
    uint8_t buf[LDPC_CB0_BITS_STRIDE + 8];
    memcpy(buf, &bits[(size_t)i * LDPC_CB0_BITS_STRIDE], LDPC_CB0_BITS_STRIDE);
    EXPECT_EQ(r[i].crc_ok, check_crc(buf, b.it[i].Kprime, b.it[i].crc_type)) << "item " << i;
  }
}

struct Ctr {
  uint64_t err, fb, poi, dis, trips;
};
Ctr tb_ctr()
{
  Ctr c;
  lcp_counters(&c.err, &c.fb, &c.poi, &c.dis);
  c.trips = lcp_trips();
  return c;
}

class Cb0Test : public ::testing::Test {
 protected:
  static void SetUpTestSuite()
  {
    ASSERT_EQ(lcp_load(), 0);
    A.init = (int (*)(void))lcp_sym("ldpc_cb0_init");
    A.submit = (decltype(A.submit))lcp_sym("ldpc_cb0_submit");
    A.collect = (decltype(A.collect))lcp_sym("ldpc_cb0_collect");
    A.decode = (decltype(A.decode))lcp_sym("ldpc_cb0_decode");
    A.mode = (int (*)(void))lcp_sym("ldpc_cb0_mem_mode");
    A.counters = (decltype(A.counters))lcp_sym("ldpc_cb0_get_counters");
    A.hooks = (decltype(A.hooks))lcp_sym("ldpc_cb0_test_hooks");
    A.reset = (void (*)(void))lcp_sym("ldpc_cb0_test_reset");
    A.set_mem = (int (*)(int))lcp_sym("ldpc_cb0_test_set_mem");
    ASSERT_TRUE(A.init && A.submit && A.collect && A.decode && A.mode && A.counters && A.hooks && A.reset && A.set_mem);
    ASSERT_EQ(A.init(), 0);
    lcp_load_ref("./libldpc.so"); /* optional: the CPU TB decoder test skips without it */
  }
  void SetUp() override
  {
    A.hooks(0, 0, 2000, 4, 5000);
    A.reset();
    lcp_hooks(0, 0, 0, 256, 200, 4, 5000);
    lcp_reset();
  }
  void TearDown() override
  {
    A.hooks(0, 0, 2000, 4, 5000);
    A.reset();
  }
};

TEST_F(Cb0Test, ReportsMemoryMode)
{
  int integrated = 0;
  cudaDeviceGetAttribute(&integrated, cudaDevAttrIntegrated, 0);
  const char *me = getenv("LDPC_CB0_MEM");
  EXPECT_EQ(A.mode(), (integrated && !(me && !strcmp(me, "explicit"))) ? 1 : 2);
}

/* GPU decoder == CPU model, item by item: bits, iterations, early termination; CRC == OAI check_crc; guard. */
TEST_F(Cb0Test, BitExactAgainstCpuModelMixedZ)
{
  const Batch b = mixed_batch(60, 11);
  std::vector<uint8_t> bits;
  const auto r = run(b, &bits);
  int conv = 0, notconv = 0, pass = 0, fail = 0;
  for (int i = 0; i < b.n(); i++) {
    const ldpc_cb0_item_t &x = b.it[i];
    const ModelOut m = model_decode(x.BG, x.Z, &b.l[(size_t)i * STRIDE], x.iters, x.K);
    ASSERT_EQ(r[i].decoder_used, 2) << i;
    EXPECT_EQ(r[i].iters, m.iters) << "item " << i;
    EXPECT_EQ(r[i].converged, m.converged) << "item " << i;
    EXPECT_EQ(0, memcmp(&bits[(size_t)i * LDPC_CB0_BITS_STRIDE], m.bits.data(), m.bits.size())) << "bits of item " << i;
    uint8_t buf[LDPC_CB0_BITS_STRIDE + 8] = {0};
    memcpy(buf, m.bits.data(), m.bits.size());
    EXPECT_EQ(r[i].crc_ok, check_crc(buf, x.Kprime, x.crc_type)) << "item " << i;
    bool z = x.guard_bytes > 0;
    for (int k = 0; k < x.guard_bytes; k++)
      z &= m.bits[k] == 0;
    EXPECT_EQ(r[i].zero, z) << i;
    conv += m.converged;
    notconv += !m.converged;
    pass += r[i].crc_ok == 1;
    fail += r[i].crc_ok == 0;
    if (b.kind[i] == 0 && r[i].crc_ok == 1)
      EXPECT_EQ(0, memcmp(m.bits.data(), b.src[i].data(), x.K / 8)) << "a pass must be the source " << i;
    if (b.kind[i] != 0)
      EXPECT_EQ(r[i].crc_ok, 0) << "noise / corrupted CRC must fail " << i;
  }
  EXPECT_GT(conv, 5);
  EXPECT_GT(notconv, 5); /* both regimes exercised */
  EXPECT_GT(pass, 5);
  EXPECT_GT(fail, 5);
}

/* Same inputs through G1's TB pool (ldpc_pool_decode): identical bits. */
TEST_F(Cb0Test, BitsEqualG1Pool)
{
  srand48(5);
  const Shape shp[] = {{1, 384, 0}, {0, 96, 2}};
  for (const Shape &s : shp)
    for (int iters : {16, 8}) {
      Batch b;
      for (int i = 0; i < 24; i++)
        add_item(b, s, i % 4 == 3 ? 1 : 0, i % 2 ? 1.5 : 2.5, iters, 0);
      std::vector<uint8_t> bits;
      const auto r = run(b, &bits);
      int8_t *pl = lcp_host_llr();
      const uint32_t in_stride = 68 * 384, bstride = (68 * 384 + 7) / 8;
      const uint32_t base = 1024; /* pool slots well away from anything else */
      for (int i = 0; i < b.n(); i++)
        memcpy(pl + (size_t)(base + i) * in_stride, &b.l[(size_t)i * STRIDE], STRIDE);
      int rq = -1;
      ASSERT_EQ(lcp_pool_decode(b.it[0].BG, s.Z, iters, base, b.n(), b.it[0].K, &rq), 0);
      ASSERT_EQ(rq, 0);
      for (int i = 0; i < b.n(); i++) {
        EXPECT_EQ(0, memcmp(lcp_host_bits() + (size_t)(base + i) * bstride, &bits[(size_t)i * LDPC_CB0_BITS_STRIDE], b.it[i].K / 8))
            << "Z " << s.Z << " iters " << iters << " item " << i;
        EXPECT_EQ(r[i].decoder_used, 2);
      }
    }
}

/* CRC verdict == OAI check_crc on the returned bits, all three CRC types, valid and corrupted CRCs. */
TEST_F(Cb0Test, CrcVerdictEqualsCheckCrc)
{
  srand48(7);
  Batch b;
  for (int i = 0; i < 90; i++)
    add_item(b, SHAPES[i % 10], i % 3 == 2 ? 2 : (i % 9 == 4 ? 1 : 0), 99, 8, 0);
  std::vector<uint8_t> bits;
  const auto r = run(b, &bits);
  expect_crc_matches_oai(b, r, bits);
  for (int i = 0; i < b.n(); i++)
    EXPECT_EQ(r[i].crc_ok, b.kind[i] == 0 ? 1 : 0) << i;
}

/* Verdicts against OAI's CPU TB decoder (libldpc.so) on clean codewords: same verdict, bits = source. */
TEST_F(Cb0Test, VerdictEqualsCpuTbDecoderOnCleanCodewords)
{
  srand48(9);
  const Shape shp[] = {{1, 384, 0}, {1, 64, 2}, {0, 96, 2}, {0, 384, 2}}; /* the TB path picks CRC16 for A <= 3824 */
  Batch b;
  std::vector<std::vector<short>> llr;
  for (int i = 0; i < 40; i++) {
    const Shape &s = shp[i % 4];
    const int E = (s.bg1 ? 66 : 50) * s.Z, K = (s.bg1 ? 22 : 10) * s.Z;
    llr.emplace_back(E);
    std::vector<uint8_t> src(K / 8 + 8);
    b.l.resize(b.l.size() + STRIDE, 0);
    const int kind = i % 5 == 4 ? 1 : 0;
    if (kind == 1) {
      for (int k = 0; k < E; k++)
        llr.back()[k] = (short)((lrand48() % 81) - 40);
      for (int k = 0; k < E; k++) { /* the same noise as CB0 input (deinterleave Qm = 2) */
        const int ii = k / (E / 2), j = k % (E / 2);
        const short v = llr.back()[ii + 2 * j];
        b.l[b.l.size() - STRIDE + 2 * s.Z + k] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
      }
    } else {
      ASSERT_EQ(lcp_make_cw(s.bg1, s.Z, s.crc, 4.0, 0, src.data(), &b.l[b.l.size() - STRIDE], llr.back().data()), K);
    }
    ldpc_cb0_item_t x = {};
    x.BG = s.bg1 ? 1 : 2;
    x.Z = (uint16_t)s.Z;
    x.K = x.Kprime = (uint16_t)K;
    x.crc_type = (uint8_t)s.crc;
    x.iters = 16;
    b.it.push_back(x);
    b.src.push_back(src);
    b.kind.push_back(kind);
  }
  std::vector<uint8_t> bits;
  const auto r = run(b, &bits);
  int compared = 0;
  for (int i = 0; i < b.n(); i++) {
    const Shape &s = shp[i % 4];
    uint8_t ok = 0, du = 0;
    std::vector<uint8_t> cb(b.it[i].K / 8 + 8);
    if (lcp_decode_cw(1, s.bg1, s.Z, llr[i].data(), 8, &ok, cb.data(), &du) < 0)
      GTEST_SKIP() << "./libldpc.so not loadable";
    uint8_t ok_g1 = 0, du_g1 = 0;
    std::vector<uint8_t> cg(b.it[i].K / 8 + 8);
    ASSERT_GE(lcp_decode_cw(0, s.bg1, s.Z, llr[i].data(), 8, &ok_g1, cg.data(), &du_g1), 0);
    EXPECT_EQ(du_g1, LCP_CUDA);
    EXPECT_EQ(r[i].crc_ok, ok_g1) << "G1 TB path, item " << i;
    EXPECT_EQ(du, LCP_CPU);
    EXPECT_EQ(r[i].crc_ok, ok) << "item " << i;
    if (ok) {
      EXPECT_EQ(0, memcmp(cb.data(), b.src[i].data(), b.it[i].K / 8));
      EXPECT_EQ(0, memcmp(&bits[(size_t)i * LDPC_CB0_BITS_STRIDE], b.src[i].data(), b.it[i].K / 8));
    }
    compared++;
  }
  EXPECT_EQ(compared, b.n());
}

/* N = 1, 64, 600, 2000 mixed Z: each item's result equals its single-item decode (batch invariance), truths pass,
 * noise fails; a sample is also checked against the CPU model. */
TEST_F(Cb0Test, BatchSizesAreInvariant)
{
  for (int n : {1, 64, 600, 2000}) {
    const Batch b = mixed_batch(n, 100 + n, n >= 600);
    std::vector<uint8_t> bits;
    const auto r = run(b, &bits);
    for (int i = 0; i < n; i += (n > 64 ? 37 : 1)) {
      Batch one;
      one.it.push_back(b.it[i]);
      one.l.assign(b.l.begin() + (size_t)i * STRIDE, b.l.begin() + (size_t)(i + 1) * STRIDE);
      std::vector<uint8_t> b1;
      const auto r1 = run(one, &b1);
      EXPECT_EQ(r1[0].crc_ok, r[i].crc_ok) << "n " << n << " item " << i;
      EXPECT_EQ(r1[0].iters, r[i].iters);
      EXPECT_EQ(0, memcmp(b1.data(), &bits[(size_t)i * LDPC_CB0_BITS_STRIDE], b.it[i].K / 8));
    }
    for (int i = 0; i < n; i++) {
      ASSERT_EQ(r[i].decoder_used, 2);
      if (b.kind[i] != 0)
        EXPECT_EQ(r[i].crc_ok, 0) << i;
    }
    for (int i = 0; i < n; i += std::max(1, n / 8)) {
      const ldpc_cb0_item_t &x = b.it[i];
      const ModelOut m = model_decode(x.BG, x.Z, &b.l[(size_t)i * STRIDE], x.iters, x.K);
      EXPECT_EQ(0, memcmp(&bits[(size_t)i * LDPC_CB0_BITS_STRIDE], m.bits.data(), m.bits.size())) << n << " " << i;
      EXPECT_EQ(r[i].iters, m.iters);
    }
    expect_crc_matches_oai(b, r, bits);
  }
}

/* Asynchronous API: three submissions in flight, collected later, equal the synchronous results; a fourth is BUSY. */
TEST_F(Cb0Test, AsyncSubmitCollectPipelines)
{
  Batch b[4] = {mixed_batch(64, 1), mixed_batch(64, 2), mixed_batch(64, 3), mixed_batch(8, 4)};
  std::vector<ldpc_cb0_result_t> ref[3];
  for (int k = 0; k < 3; k++)
    ref[k] = run(b[k]);
  ldpc_cb0_ticket_t *t[3];
  for (int k = 0; k < 3; k++)
    ASSERT_EQ(A.submit(b[k].it.data(), b[k].n(), b[k].l.data(), STRIDE, 0, &t[k]), 0);
  ldpc_cb0_ticket_t *t4 = nullptr;
  EXPECT_EQ(A.submit(b[3].it.data(), b[3].n(), b[3].l.data(), STRIDE, 0, &t4), LDPC_CB0_E_BUSY);
  for (int k = 0; k < 3; k++) {
    std::vector<ldpc_cb0_result_t> r(b[k].n());
    ASSERT_EQ(A.collect(t[k], r.data(), nullptr, 0), 0);
    for (int i = 0; i < b[k].n(); i++) {
      EXPECT_EQ(r[i].crc_ok, ref[k][i].crc_ok);
      EXPECT_EQ(r[i].iters, ref[k][i].iters);
    }
  }
}

/* Bad items are reported per item; the rest of the batch decodes. */
TEST_F(Cb0Test, BadItemsAreInconclusiveOthersDecode)
{
  Batch b = mixed_batch(10, 21);
  b.it[1].Z = 385;      /* not a lifting size */
  b.it[3].iters = 0;    /* no iterations */
  b.it[5].Kprime = 100; /* not a multiple of 8 */
  std::vector<uint8_t> bits;
  const auto r = run(b, &bits);
  for (int i = 0; i < b.n(); i++) {
    if (i == 1 || i == 3 || i == 5) {
      EXPECT_EQ(r[i].crc_ok, -1);
      EXPECT_EQ(r[i].err, LDPC_CB0_E_ARG);
      EXPECT_EQ(r[i].decoder_used, 0);
    } else {
      EXPECT_EQ(r[i].decoder_used, 2);
      EXPECT_NE(r[i].crc_ok, -1);
    }
  }
}

/* ---- fault injection: the batch is inadmissible, the TB path's breaker / counters / pool are untouched ---- */
void expect_tb_path_untouched(const Ctr &before)
{
  const Ctr a = tb_ctr();
  EXPECT_EQ(a.trips, before.trips);
  EXPECT_EQ(a.dis, 0u);
  EXPECT_EQ(a.err, before.err);
  EXPECT_EQ(a.fb, before.fb);
  uint8_t ok[1], du = 0;
  const uint8_t noise[1] = {0};
  EXPECT_EQ(lcp_run_tb(1, 1, noise, ok, &du), 0);
  EXPECT_EQ(du, LCP_CUDA) << "the TB decode right after must still run on the GPU";
  EXPECT_TRUE(ok[0]);
}

void expect_all_inconclusive(const std::vector<ldpc_cb0_result_t> &r, int err)
{
  for (size_t i = 0; i < r.size(); i++) {
    EXPECT_EQ(r[i].crc_ok, -1) << i;
    EXPECT_EQ(r[i].err, err) << i;
  }
}

TEST_F(Cb0Test, InjectedErrorMakesBatchInadmissible)
{
  const Ctr before = tb_ctr();
  const Batch b = mixed_batch(32, 31);
  A.hooks(1, 0, 0, 0, 0);
  int rc = 0;
  const auto r = run(b, nullptr, &rc);
  EXPECT_EQ(rc, LDPC_CB0_E_CUDA);
  expect_all_inconclusive(r, LDPC_CB0_E_CUDA);
  A.hooks(0, 0, 0, 0, 0);
  expect_tb_path_untouched(before);
  const auto r2 = run(b); /* recovers */
  for (auto &x : r2)
    EXPECT_EQ(x.decoder_used, 2);
}

TEST_F(Cb0Test, TimeoutMakesBatchInadmissible)
{
  const Ctr before = tb_ctr();
  const Batch b = mixed_batch(32, 41);
  A.hooks(2, 400, 50, 0, 0); /* stream stalled 400 ms, deadline ~50 ms */
  int rc = 0;
  const auto r = run(b, nullptr, &rc);
  EXPECT_EQ(rc, LDPC_CB0_E_TIMEOUT);
  expect_all_inconclusive(r, LDPC_CB0_E_TIMEOUT);
  A.hooks(0, 0, 2000, 0, 0);
  expect_tb_path_untouched(before);
  usleep(500000); /* the abandoned slot frees itself when the stalled work drains */
  ldpc_cb0_counters_t c;
  A.counters(&c);
  EXPECT_GE(c.timeouts, 1u);
  const auto r2 = run(b);
  for (auto &x : r2)
    EXPECT_EQ(x.decoder_used, 2);
}

TEST_F(Cb0Test, StickyErrorDisablesCb0OnlyAndBatchIsInadmissible)
{
  const Ctr before = tb_ctr();
  const Batch b = mixed_batch(16, 51);
  A.hooks(3, 0, 0, 0, 0);
  int rc = 0;
  const auto r = run(b, nullptr, &rc);
  EXPECT_EQ(rc, LDPC_CB0_E_CUDA);
  expect_all_inconclusive(r, LDPC_CB0_E_CUDA);
  A.hooks(0, 0, 0, 0, 0);
  ldpc_cb0_counters_t c;
  A.counters(&c);
  EXPECT_EQ(c.state, 2u);
  const auto r2 = run(b, nullptr, &rc);
  EXPECT_EQ(rc, LDPC_CB0_E_DISABLED);
  expect_all_inconclusive(r2, LDPC_CB0_E_DISABLED);
  expect_tb_path_untouched(before);
  A.reset();
  const auto r3 = run(b);
  for (auto &x : r3)
    EXPECT_EQ(x.decoder_used, 2);
}

TEST_F(Cb0Test, ConsecutiveErrorsTripCb0BreakerOnly)
{
  const Ctr before = tb_ctr();
  ldpc_cb0_counters_t c0, c1;
  A.counters(&c0);
  const Batch b = mixed_batch(8, 61);
  A.hooks(1, 0, 0, 3, 60000);
  int rc = 0;
  for (int k = 0; k < 3; k++) {
    run(b, nullptr, &rc);
    EXPECT_EQ(rc, LDPC_CB0_E_CUDA);
  }
  A.hooks(0, 0, 0, 0, 0);
  run(b, nullptr, &rc);
  EXPECT_EQ(rc, LDPC_CB0_E_BYPASSED);
  A.counters(&c1);
  EXPECT_EQ(c1.breaker_trips, c0.breaker_trips + 1);
  EXPECT_EQ(c1.state, 1u);
  expect_tb_path_untouched(before);
}

/* The discrete-GPU path, forced on this host: pageable, pinned, managed and device inputs give the unified results. */
TEST_F(Cb0Test, ForcedExplicitCopyPathMatches)
{
  const int mode0 = A.mode();
  const Batch b = mixed_batch(150, 71);
  std::vector<uint8_t> bits_u;
  const auto ru = run(b, &bits_u);
  ASSERT_EQ(A.set_mem(2), 2);
  const size_t bytes = b.l.size();
  int8_t *pinned = nullptr, *managed = nullptr, *dev = nullptr;
  ASSERT_EQ(cudaHostAlloc((void **)&pinned, bytes, cudaHostAllocDefault), cudaSuccess);
  ASSERT_EQ(cudaMallocManaged((void **)&managed, bytes), cudaSuccess);
  ASSERT_EQ(cudaMalloc((void **)&dev, bytes), cudaSuccess);
  memcpy(pinned, b.l.data(), bytes);
  memcpy(managed, b.l.data(), bytes);
  ASSERT_EQ(cudaMemcpy(dev, b.l.data(), bytes, cudaMemcpyHostToDevice), cudaSuccess);
  const int8_t *srcs[4] = {b.l.data(), pinned, managed, dev};
  const char *names[4] = {"pageable", "pinned", "managed", "device"};
  for (int k = 0; k < 4; k++) {
    ldpc_cb0_counters_t c0, c1;
    A.counters(&c0);
    std::vector<ldpc_cb0_result_t> r(b.n());
    std::vector<uint8_t> bits((size_t)b.n() * LDPC_CB0_BITS_STRIDE);
    ASSERT_EQ(A.decode(b.it.data(), b.n(), srcs[k], STRIDE, r.data(), bits.data(), LDPC_CB0_BITS_STRIDE), 0) << names[k];
    A.counters(&c1);
    EXPECT_EQ(c1.mode, 2u);
    if (k < 2)
      EXPECT_GT(c1.h2d_bytes, c0.h2d_bytes) << names[k] << ": a host input must be copied";
    if (k == 3)
      EXPECT_EQ(c1.h2d_bytes, c0.h2d_bytes) << "device input is read in place";
    for (int i = 0; i < b.n(); i++) {
      EXPECT_EQ(r[i].crc_ok, ru[i].crc_ok) << names[k] << " " << i;
      EXPECT_EQ(r[i].iters, ru[i].iters) << names[k] << " " << i;
      EXPECT_EQ(0, memcmp(&bits[(size_t)i * LDPC_CB0_BITS_STRIDE], &bits_u[(size_t)i * LDPC_CB0_BITS_STRIDE], b.it[i].K / 8));
    }
  }
  cudaFreeHost(pinned);
  cudaFree(managed);
  cudaFree(dev);
  EXPECT_EQ(A.set_mem(mode0), mode0);
}

} // namespace
