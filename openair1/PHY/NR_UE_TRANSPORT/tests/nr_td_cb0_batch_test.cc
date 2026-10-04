/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_td_cb0_batch: batched CB0 decode of many Technique D hypotheses per grant (levers spec section 5.4 / 9). */
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include <dlfcn.h>
#include "nr_td_cb0_fixture.h"

namespace {

struct Fx {
  cb0_fx_t fx{};
  explicit Fx(uint32_t A, uint16_t R, uint8_t Qm, uint8_t Nl, uint8_t rv, uint32_t G, uint32_t lbrm = 0, uint32_t seed = 1)
  {
    fx.A = A; fx.R = R; fx.Qm = Qm; fx.Nl = Nl; fx.rv = rv; fx.G = G; fx.tbslbrm = lbrm;
    EXPECT_EQ(cb0_fx_encode(&fx, seed, 64), 0);
  }
  ~Fx() { cb0_fx_free(&fx); }
  nr_td_cb0_item_t item() const { return cb0_fx_item(&fx); }
};

/* 106 PRB, 12 data symbols, 1 DM-RS symbol (6 RE / PRB), 64QAM R 0.505, rank 1: C > 1, BG1. */
Fx *big(uint8_t rv = 0)
{
  const uint16_t R = 5170;
  const uint32_t A = cb0_fx_tbs(6, R, 106, 12, 6, 1), G = cb0_fx_G(106, 12, 6, 1, 6, 1);
  return new Fx(A, R, 6, 1, rv, G);
}
/* 6 PRB 16QAM R 0.12: C == 1, BG2. */
Fx *small(uint8_t rv = 0)
{
  const uint16_t R = 1200;
  const uint32_t A = cb0_fx_tbs(4, R, 6, 12, 6, 1), G = cb0_fx_G(6, 12, 6, 1, 4, 1);
  return new Fx(A, R, 4, 1, rv, G);
}

bool have_gpu()
{
#ifdef NR_TD_CB0_HAVE_CUDA
  return true;
#else
  return false;
#endif
}

class Cb0 : public ::testing::Test {
 protected:
  void SetUp() override
  {
    cb0_fx_init();
    nr_td_cb0_use_gpu(0);
    nr_td_cb0_use_cuda_ldpc(0);
    nr_td_cb0_set_threads(1);
  }
};

std::vector<nr_td_cb0_result_t> run(const std::vector<nr_td_cb0_item_t> &it)
{
  std::vector<nr_td_cb0_result_t> out(it.size());
  nr_td_cb0_batch(it.data(), (int)it.size(), out.data());
  return out;
}

/* Correct / wrong-TBS / wrong-rv / wrong-Qm hypotheses on a TB encoded with the OAI chain: the batch equals the
 * receiver's own decoder (nrLDPC_coding_decoder with nb_segments_to_decode = 1, as passive_ldpc_decode_core's
 * probe), the correct one passes and is also a full-TB pass, the wrong ones fail. */
TEST_F(Cb0, CpuReferenceMatchesFullDecodeCb0)
{
  for (int which = 0; which < 2; which++) {
    Fx *f = which ? small() : big();
    std::vector<nr_td_cb0_item_t> items;
    nr_td_cb0_item_t ok = f->item();
    items.push_back(ok);
    nr_td_cb0_item_t wtbs = ok; /* the next MCS's TBS on the same geometry */
    wtbs.tbs = cb0_fx_tbs(f->fx.Qm, f->fx.R + 600, which ? 6 : 106, 12, 6, 1);
    ASSERT_NE(wtbs.tbs, ok.tbs);
    items.push_back(wtbs);
    nr_td_cb0_item_t wrv = ok;
    wrv.rv = 2;
    items.push_back(wrv);
    nr_td_cb0_item_t wqm = ok; /* a lower order read of the same LLRs: G scales with Qm */
    wqm.Qm = 2;
    wqm.G = ok.G / ok.Qm * 2;
    items.push_back(wqm);
    const auto out = run(items);
    for (size_t i = 0; i < items.size(); i++) {
      int seg0 = -1;
      const int rx = cb0_fx_rx_decode(&items[i], 1, &seg0);
      EXPECT_EQ(out[i].pass, rx) << "fixture " << which << " item " << i;
      EXPECT_EQ(out[i].decoder_used, NR_TD_CB0_DEC_CPU_LAYERED);
    }
    EXPECT_EQ(out[0].pass, 1) << "fixture " << which;
    EXPECT_NE(cb0_fx_tb_crc(f->fx.payload, f->fx.A), 0u) << "CRC tables not initialised (crcTableInit)";
    int bits = 0;
    EXPECT_EQ(cb0_fx_rx_decode_bits(&items[0], f->fx.payload, &bits), 1) << "full TB of the correct hypothesis";
    EXPECT_EQ(bits, 1) << "decoded TB payload + CRC differ from the source bits";
    for (size_t i = 1; i < items.size(); i++)
      EXPECT_EQ(out[i].pass, 0) << "fixture " << which << " wrong hypothesis " << i;
    EXPECT_LT(out[0].iters, 8);
    EXPECT_GE(out[1].iters, 8);
    delete f;
  }
}

/* GPU rate de-matching + de-interleaving + filler/puncture/saturation: bit-identical to the CPU reference
 * (nr_deinterleaving_ldpc + nr_rate_matching_ldpc_rx), int16 circular buffer and int8 decoder input, over
 * BG1/BG2 x C = 1 / C > 1 x Qm 2..8 x rv 0..3 x LBRM off/on x Nl 1..4, in one batch. */
TEST_F(Cb0, GpuDematchBitIdenticalSweep)
{
  if (!have_gpu())
    GTEST_SKIP() << "built without CUDA";
  ASSERT_EQ(nr_td_cb0_use_gpu(1), 1) << "libtd_cb0_gpu.so not loadable / no device";
  struct Geo { uint16_t nrb, R; } geos[4] = {{8, 7700}, {106, 5000}, {4, 2000}, {106, 1500}};
  std::mt19937 rng(7);
  std::vector<std::vector<int16_t>> llrs;
  std::vector<nr_td_cb0_item_t> items;
  int seen[3][2] = {{0}}; /* [BG][C > 1] */
  int lbrm_binds = 0, repetition = 0;
  for (auto &g : geos)
    for (uint8_t Qm = 2; Qm <= 8; Qm += 2)
      for (uint8_t Nl = 1; Nl <= 4; Nl++)
        for (uint8_t rv = 0; rv < 4; rv++)
          for (int lb = 0; lb < 2; lb++) {
            nr_td_cb0_item_t it{};
            it.G = cb0_fx_G(g.nrb, 12, 6, 1, Qm, Nl);
            it.Qm = Qm; it.Nl = Nl; it.rv = rv; it.R = g.R; it.max_iter = 8;
            it.llr_shift = (uint8_t)((Qm + Nl + rv + lb) % 9); /* K38 k_h 0..8 */
            it.tbs = cb0_fx_tbs(Qm, g.R, g.nrb, 12, 6, Nl);
            nr_td_cb0_meta_t m;
            static const int16_t placeholder = 0;
            it.llr = &placeholder; /* replaced by the item's own LLRs below */
            if (nr_td_cb0_meta(&it, &m) != 0)
              continue;
            if (lb) {
              it.tbslbrm = m.C * m.N / 2; /* N_ref = 3/4 N < N */
              if (nr_td_cb0_meta(&it, &m) != 0)
                continue;
              lbrm_binds += m.Ncb < m.N;
            }
            repetition += m.E > m.Ncb;
            seen[m.BG][m.C > 1]++;
            std::uniform_int_distribution<int> u(-4000, 4000); /* wide: the shift and the int8 rail both bite */
            llrs.emplace_back(it.G);
            for (auto &v : llrs.back())
              v = (int16_t)u(rng);
            items.push_back(it);
          }
  for (size_t i = 0; i < items.size(); i++)
    items[i].llr = llrs[i].data();
  ASSERT_GT(seen[1][0], 0); ASSERT_GT(seen[1][1], 0); ASSERT_GT(seen[2][0], 0); ASSERT_GT(seen[2][1], 0);
  ASSERT_GT(lbrm_binds, 0);
  ASSERT_GT(repetition, 0);
  const int n = (int)items.size();
  std::vector<int8_t> lc((size_t)n * NR_TD_CB0_L_STRIDE), lg((size_t)n * NR_TD_CB0_L_STRIDE);
  std::vector<int16_t> dc((size_t)n * NR_TD_CB0_D_STRIDE), dg((size_t)n * NR_TD_CB0_D_STRIDE);
  std::vector<int8_t> sc(n), sg(n);
  ASSERT_EQ(nr_td_cb0_dematch(items.data(), n, 0, lc.data(), dc.data(), sc.data()), 0);
  ASSERT_EQ(nr_td_cb0_dematch(items.data(), n, 1, lg.data(), dg.data(), sg.data()), 0);
  int diff_items = 0;
  for (int i = 0; i < n; i++) {
    nr_td_cb0_meta_t m;
    ASSERT_EQ(nr_td_cb0_meta(&items[i], &m), 0);
    ASSERT_EQ(sc[i], 0);
    ASSERT_EQ(sg[i], 1) << "item " << i << " not dematched on the GPU";
    const bool dl = memcmp(&dc[(size_t)i * NR_TD_CB0_D_STRIDE], &dg[(size_t)i * NR_TD_CB0_D_STRIDE], m.N * 2) != 0;
    const bool ll = memcmp(&lc[(size_t)i * NR_TD_CB0_L_STRIDE], &lg[(size_t)i * NR_TD_CB0_L_STRIDE], m.Kc * m.Z) != 0;
    if (dl || ll) {
      if (diff_items++ < 5)
        ADD_FAILURE() << "item " << i << " BG " << (int)m.BG << " C " << m.C << " Qm " << (int)m.Qm << " rv " << (int)m.rv
                      << " E " << m.E << " Ncb " << m.Ncb << " N " << m.N << " d differs " << dl << " l differs " << ll;
    }
  }
  EXPECT_EQ(diff_items, 0) << "of " << n << " items";
  printf("[sweep] %d items bit-identical (BG1 C1 %d, BG1 C>1 %d, BG2 C1 %d, BG2 C>1 %d, LBRM binding %d, E > Ncb %d)\n",
         n - diff_items, seen[1][0], seen[1][1], seen[2][0], seen[2][1], lbrm_binds, repetition);
}

/* The GPU-dematch batch gives the same pass / iterations as the all-CPU batch, item by item (1 and 4 threads). */
TEST_F(Cb0, BatchResultsEqualCpu)
{
  if (!have_gpu())
    GTEST_SKIP() << "built without CUDA";
  Fx *b = big(), *s = small(), *b1 = big(1);
  std::vector<nr_td_cb0_item_t> items;
  for (Fx *f : {b, s, b1}) {
    nr_td_cb0_item_t ok = f->item();
    items.push_back(ok);
    for (uint8_t rv = 0; rv < 4; rv++) {
      nr_td_cb0_item_t w = ok;
      w.rv = rv;
      items.push_back(w);
    }
    nr_td_cb0_item_t w = ok;
    w.tbs = ok.tbs - 8 * 3;
    items.push_back(w);
    w = ok;
    w.Qm = 2;
    w.G = ok.G / ok.Qm * 2;
    items.push_back(w);
  }
  const auto cpu = run(items);
  ASSERT_EQ(nr_td_cb0_use_gpu(1), 1);
  for (int th : {1, 4}) {
    nr_td_cb0_set_threads(th);
    const auto gpu = run(items);
    int passes = 0;
    for (size_t i = 0; i < items.size(); i++) {
      EXPECT_EQ(gpu[i].pass, cpu[i].pass) << "item " << i << " threads " << th;
      EXPECT_EQ(gpu[i].iters, cpu[i].iters) << "item " << i;
      EXPECT_EQ(gpu[i].tb_result, cpu[i].tb_result);
      EXPECT_EQ(gpu[i].C, cpu[i].C);
      EXPECT_EQ(gpu[i].dematch_gpu, 1);
      EXPECT_EQ(cpu[i].dematch_gpu, 0);
      passes += gpu[i].pass == 1;
    }
    EXPECT_GE(passes, 3) << "each fixture's correct hypothesis passes";
  }
  delete b; delete s; delete b1;
}

/* C == 1: the CB0 CRC is the TB CRC (CRC16 / CRC24A), the result says so; C > 1 it does not. */
TEST_F(Cb0, C1MeansTbResultFlag)
{
  Fx *s = small(), *b = big();
  ASSERT_EQ(s->fx.C, 1u);
  ASSERT_GT(b->fx.C, 1u);
  const auto out = run({s->item(), b->item()});
  EXPECT_EQ(out[0].pass, 1);
  EXPECT_EQ(out[0].tb_result, 1);
  EXPECT_EQ(out[0].C, 1);
  EXPECT_EQ(out[1].pass, 1);
  EXPECT_EQ(out[1].tb_result, 0);
  EXPECT_EQ(out[1].C, b->fx.C);
  /* and for C == 1 the CB0 verdict equals the receiver's full-TB verdict, also on a failing hypothesis */
  nr_td_cb0_item_t w = s->item();
  w.rv = 3;
  const auto o2 = run({w});
  EXPECT_EQ(o2[0].tb_result, 1);
  EXPECT_EQ(o2[0].pass, cb0_fx_rx_decode(&w, 0, nullptr));
  delete s; delete b;
}

/* An all-zero codeword (every LLR "bit 0") is a valid LDPC codeword with an all-zero CRC: the decoder's CRC passes it,
 * the receiver's all-zero guards reject it (probe guard on CB0 when C > 1, TB guard when C == 1). Same here. */
TEST_F(Cb0, AllZeroCodewordIsRejected)
{
  for (int which = 0; which < 2; which++) {
    Fx *f = which ? small() : big();
    std::vector<int16_t> zero(f->fx.G, 64);
    nr_td_cb0_item_t it = f->item();
    it.llr = zero.data();
    const auto out = run({it});
    EXPECT_EQ(out[0].pass, 0) << "fixture " << which;
    EXPECT_LT(out[0].iters, 8) << "the decoder itself converged: only the guard rejects it";
    EXPECT_EQ(out[0].pass, cb0_fx_rx_decode(&it, 1, nullptr));
    delete f;
  }
}

/* K38: the batch applies exactly the k_h it is given (llr >> k_h, int16 arithmetic) and nothing else: an item with
 * shift k on raw LLRs equals an item with shift 0 on LLRs pre-shifted by the full decode's own loop, CPU and GPU. */
TEST_F(Cb0, ShiftAppliedExactly)
{
  std::mt19937 rng(3);
  std::uniform_int_distribution<int> u(-3000, 3000);
  const uint32_t G = cb0_fx_G(106, 12, 6, 1, 6, 2);
  std::vector<int16_t> raw(G);
  for (auto &v : raw) v = (int16_t)u(rng);
  std::vector<std::vector<int16_t>> pre;
  std::vector<nr_td_cb0_item_t> a, b;
  for (int k = 0; k <= 8; k++) {
    nr_td_cb0_item_t it{};
    it.G = G; it.Qm = 6; it.Nl = 2; it.rv = (uint8_t)(k % 4); it.R = 5170; it.max_iter = 8;
    it.tbs = cb0_fx_tbs(6, 5170, 106, 12, 6, 2);
    it.llr = raw.data(); it.llr_shift = (uint8_t)k;
    a.push_back(it);
    pre.emplace_back(G);
    for (uint32_t i = 0; i < G; i++) pre.back()[i] = (int16_t)(raw[i] >> k); /* nr_pdsch_passive_decode.c's loop */
  }
  for (int k = 0; k <= 8; k++) { nr_td_cb0_item_t it = a[k]; it.llr = pre[k].data(); it.llr_shift = 0; b.push_back(it); }
  nr_td_cb0_item_t bad = a[0]; bad.llr_shift = 9;
  const auto ob = run({bad});
  EXPECT_EQ(ob[0].pass, -1);
  const int n = (int)a.size();
  for (int g = 0; g < (have_gpu() ? 2 : 1); g++) {
    std::vector<int8_t> la((size_t)n * NR_TD_CB0_L_STRIDE), lb((size_t)n * NR_TD_CB0_L_STRIDE);
    std::vector<int16_t> da((size_t)n * NR_TD_CB0_D_STRIDE), db((size_t)n * NR_TD_CB0_D_STRIDE);
    std::vector<int8_t> st(n);
    ASSERT_EQ(nr_td_cb0_dematch(a.data(), n, g, la.data(), da.data(), st.data()), 0);
    ASSERT_EQ(nr_td_cb0_dematch(b.data(), n, g, lb.data(), db.data(), st.data()), 0);
    EXPECT_EQ(la, lb) << "gpu " << g;
    EXPECT_EQ(da, db) << "gpu " << g;
  }
  /* and k_h = 0 vs k_h = 3 on the same raw LLRs differ (the shift is not ignored) */
  std::vector<int8_t> l0((size_t)NR_TD_CB0_L_STRIDE), l3((size_t)NR_TD_CB0_L_STRIDE);
  std::vector<int8_t> st(1);
  nr_td_cb0_item_t x = a[0]; x.rv = 0; x.llr_shift = 0;
  nr_td_cb0_item_t y = x; y.llr_shift = 3;
  nr_td_cb0_dematch(&x, 1, 0, l0.data(), nullptr, st.data());
  nr_td_cb0_dematch(&y, 1, 0, l3.data(), nullptr, st.data());
  EXPECT_NE(l0, l3);
}

/* Exact dedup: items with an identical computation key are decoded once, the verdict copied; anything that changes
 * the computation (LLR buffer, shift, rv, TBS -> K'/CRC) keeps its own decode. */
TEST_F(Cb0, DedupExact)
{
  Fx *f = big();
  const nr_td_cb0_item_t ok = f->item();
  std::vector<int16_t> copy(f->fx.llr, f->fx.llr + f->fx.G); /* same content, other buffer: NOT merged */
  std::vector<nr_td_cb0_item_t> items;
  items.push_back(ok);
  items.push_back(ok); /* exact duplicate */
  nr_td_cb0_item_t w = ok; w.rv = 2; items.push_back(w);
  items.push_back(w); /* duplicate of the wrong one */
  nr_td_cb0_item_t c = ok; c.llr = copy.data(); items.push_back(c);
  nr_td_cb0_item_t s1 = ok; s1.llr_shift = 1; items.push_back(s1);
  nr_td_cb0_item_t t1 = ok; t1.tbs = ok.tbs - 8 * 3; items.push_back(t1);
  nr_td_cb0_item_t dt = ok; dt.mcs_table = 2; items.push_back(dt); /* mcs_table alone is informational: merged */
  nr_td_cb0_stats_t s0, s9;
  nr_td_cb0_get_stats(&s0);
  const auto out = run(items);
  nr_td_cb0_get_stats(&s9);
  const int want_dedup[] = {0, 1, 0, 1, 0, 0, 0, 1};
  for (size_t i = 0; i < items.size(); i++)
    EXPECT_EQ(out[i].dedup, want_dedup[i]) << "item " << i;
  EXPECT_EQ(s9.items - s0.items, items.size());
  EXPECT_EQ(s9.decoded - s0.decoded, 5u);
  EXPECT_EQ(out[1].pass, out[0].pass);
  EXPECT_EQ(out[3].pass, out[2].pass);
  EXPECT_EQ(out[7].pass, out[0].pass);
  EXPECT_EQ(out[0].pass, 1);
  EXPECT_EQ(out[4].pass, 1);
  EXPECT_EQ(out[2].pass, 0);
  /* dedup on vs a one-item-per-call reference: identical verdicts */
  for (size_t i = 0; i < items.size(); i++) {
    const auto o = run({items[i]});
    EXPECT_EQ(o[0].pass, out[i].pass) << "item " << i;
    EXPECT_EQ(o[0].iters, out[i].iters) << "item " << i;
  }
  delete f;
}

/* CUDA LDPC through G1's pool (public TB entry, worker queue): correct hypotheses pass, wrong ones fail, every item
 * says decoder_used = CUDA; a pool that cannot run (test hook: skip the launch) sends the items to G1's CPU fallback
 * and they say decoder_used = CPU -- never a CUDA label on a non-GPU verdict. */
TEST_F(Cb0, CudaLdpcPoolPath)
{
  if (!have_gpu())
    GTEST_SKIP() << "built without CUDA";
  ASSERT_EQ(nr_td_cb0_use_cuda_ldpc(1), 1) << "libldpc_cuda.so not loadable";
  Fx *b = big(), *s = small(), *b2 = big(2);
  std::vector<nr_td_cb0_item_t> items;
  std::vector<int> truth;
  for (Fx *f : {b, s, b2}) {
    nr_td_cb0_item_t ok = f->item();
    items.push_back(ok); truth.push_back(1);
    for (uint8_t rv = 0; rv < 4; rv++)
      if (rv != ok.rv) { nr_td_cb0_item_t w = ok; w.rv = rv; items.push_back(w); truth.push_back(0); }
    nr_td_cb0_item_t w = ok; w.tbs = ok.tbs + 8 * 6; items.push_back(w); truth.push_back(0);
  }
  /* > 64 distinct decodes (dedup must not collapse them): replicas on private copies of the LLR buffers */
  std::vector<std::vector<int16_t>> copies;
  const size_t n0 = items.size();
  for (int k = 0; k < 8; k++)
    for (size_t i = 0; i < n0; i++) {
      const Fx *src = items[i].llr == b->fx.llr ? b : (items[i].llr == s->fx.llr ? s : b2);
      copies.emplace_back(src->fx.llr, src->fx.llr + src->fx.G);
    }
  for (int k = 0, c = 0; k < 8; k++)
    for (size_t i = 0; i < n0; i++, c++) {
      nr_td_cb0_item_t it = items[i];
      it.llr = copies[c].data();
      items.push_back(it);
      truth.push_back(truth[i]);
    }
  nr_td_cb0_set_threads(4);
  nr_td_cb0_use_cuda_ldpc(0);
  const auto cpu = run(items); /* oracle: the CPU layered decoder on the same items */
  ASSERT_EQ(nr_td_cb0_use_cuda_ldpc(1), 1);
  const auto out = run(items);
  int tbs_twin_pass = 0;
  for (size_t i = 0; i < items.size(); i++) {
    EXPECT_EQ(out[i].dedup, 0) << "item " << i;
    EXPECT_EQ(out[i].decoder_used, NR_TD_CB0_DEC_CUDA_FLOODING) << "item " << i;
    EXPECT_EQ(out[i].pass, cpu[i].pass) << "item " << i << " CUDA vs CPU";
    if (truth[i])
      EXPECT_EQ(out[i].pass, 1) << "item " << i;
    else if (i % 5 != 4) /* per fixture: [true, 3 wrong rv, wrong TBS] */
      EXPECT_EQ(out[i].pass, 0) << "item " << i << " (wrong rv)";
    else
      tbs_twin_pass += out[i].pass == 1;
  }
  /* tbs + 48 bits at C = 11: same K, Z and E, K' larger by 4 -> the decoded CB0 is the true codeword and its CRC24B
   * over 4 more (zero filler) bits still checks (a CRC codeword followed by zeros stays a codeword). A CB0 cannot
   * separate such TBS twins; recorded, not a bug. */
  printf("[cuda] TBS-twin CB0 passes: %d\n", tbs_twin_pass);
  /* G1 hook: skip every launch -> G1 falls back to its CPU decoder per TB; the label follows */
  void *h = dlopen("./libldpc_cuda.so", RTLD_NOW | RTLD_NOLOAD | RTLD_DEEPBIND);
  ASSERT_NE(h, nullptr);
  auto hooks = (void (*)(int, int, int, int, int, int, int))dlsym(h, "ldpc_cuda_test_hooks");
  auto reset = (void (*)(void))dlsym(h, "ldpc_cuda_test_reset");
  ASSERT_TRUE(hooks && reset);
  hooks(1, 0, 0, -1, 50, -1, -1);
  const std::vector<nr_td_cb0_item_t> few(items.begin(), items.begin() + 5);
  const auto o2 = run(few);
  for (size_t i = 0; i < few.size(); i++) {
    EXPECT_NE(o2[i].decoder_used, NR_TD_CB0_DEC_CUDA_FLOODING) << "item " << i << ": CUDA label on a skipped launch";
    if (o2[i].decoder_used == NR_TD_CB0_DEC_CPU_LAYERED)
      EXPECT_EQ(o2[i].pass, truth[i]) << "item " << i;
    else
      EXPECT_EQ(o2[i].pass, -1);
  }
  hooks(0, 0, 0, -1, 200, -1, -1);
  reset();
  nr_td_cb0_use_cuda_ldpc(0);
  dlclose(h);
  delete b; delete s; delete b2;
}

/* Malformed items are inconclusive (-1, with a reason) and do not disturb their neighbours; no decoder = -1. */
TEST_F(Cb0, InconclusiveOnError)
{
  Fx *s = small();
  const nr_td_cb0_item_t ok = s->item();
  std::vector<nr_td_cb0_item_t> items;
  std::vector<int> want;
  auto add = [&](nr_td_cb0_item_t it, int err) { items.push_back(it); want.push_back(err); };
  add(ok, NR_TD_CB0_OK);
  nr_td_cb0_item_t w;
  w = ok; w.llr = nullptr; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.G = 0; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.Qm = 3; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.rv = 4; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.Nl = 0; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.tbs = 0; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.max_iter = 0; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.R = 0; add(w, NR_TD_CB0_ERR_ARG); /* no code rate and no forced BG: BG unknowable */
  w = ok; w.bg = 3; add(w, NR_TD_CB0_ERR_ARG);
  w = ok; w.tbs = 3000000; add(w, NR_TD_CB0_ERR_SEG); /* C = 357 > 255 */
  w = ok; w.G = 2; add(w, NR_TD_CB0_ERR_E);          /* G / (Nl Qm C) = 0 */
  w = ok; w.tbslbrm = 8; add(w, NR_TD_CB0_ERR_RM);   /* N_ref < K - 2Z: Foffset > Ncb */
  add(ok, NR_TD_CB0_OK);
  for (int gpu = 0; gpu < (have_gpu() ? 2 : 1); gpu++) {
    nr_td_cb0_use_gpu(gpu);
    const auto out = run(items);
    for (size_t i = 0; i < items.size(); i++) {
      if (want[i] == NR_TD_CB0_OK) {
        EXPECT_EQ(out[i].pass, 1) << "item " << i << " gpu " << gpu;
      } else {
        EXPECT_EQ(out[i].pass, -1) << "item " << i << " gpu " << gpu;
        EXPECT_EQ(out[i].err, want[i]) << "item " << i;
        EXPECT_EQ(out[i].decoder_used, 0);
      }
    }
  }
  nr_td_cb0_use_gpu(0);
  nr_td_cb0_set_ldpc_decoder(nullptr);
  const auto out = run({ok});
  EXPECT_EQ(out[0].pass, -1);
  EXPECT_EQ(out[0].err, NR_TD_CB0_ERR_DECODER);
  nr_td_cb0_result_t r;
  EXPECT_EQ(nr_td_cb0_batch(nullptr, 1, &r), -1);
  EXPECT_EQ(nr_td_cb0_batch(nullptr, 0, nullptr), 0);
  cb0_fx_reinit_decoder();
  delete s;
}

} // namespace
