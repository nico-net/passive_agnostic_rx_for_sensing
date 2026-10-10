/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * GPU-DEMOD S1 test. Part A (CPU only): the scalar models of nr_pdsch_gpu_demod.cuh against the REAL CPU functions.
 * Final line: DEMOD_MODEL_MISMATCHES total=N (exit 1 when N != 0). Part B (GPU module) is added in stage S1b. */
#include "nr_pdsch_gpu_demod.cuh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

extern "C" {
void nr_channel_compensation(uint32_t buffer_length, uint32_t pdsch_buf_size_max, int nb_rx_ant, int nb_layers, void *rxFext,
                             void *chFext, void *ch_maga, void *ch_magb, void *ch_magc, nrd_c16 **rxComp, void *rho,
                             int mod_order, uint32_t symbol, uint32_t output_shift);
void nr_qpsk_llr(const nrd_c16 *x, int16_t *llr, uint32_t nb_re);
void nr_16qam_llr(const nrd_c16 *x, const nrd_c16 *m1, int16_t *llr, uint32_t nb_re);
void nr_64qam_llr(const nrd_c16 *x, const nrd_c16 *m1, const nrd_c16 *m2, int16_t *llr, uint32_t nb_re);
void nr_256qam_llr(const nrd_c16 *x, const nrd_c16 *m1, const nrd_c16 *m2, const nrd_c16 *m3, int16_t *llr, uint32_t nb_re);
void nr_scale_channel(int size, void *ch_estimates_ext, int symb, uint32_t len, int nrOfLayers, int nb_rx, int shift_ch_ext);
void nr_channel_level(const int symbol, const int size_est, const void *ch_estimates_ext, const int nb_rx, const int Nl,
                      int32_t *avg, const uint32_t len);
}

/* LOG/CONFIG_LIB need these outside a full softmodem executable (same convention as dlsch_fixed_point_test.cc). */
struct configmodule_interface_s;
extern "C" { struct configmodule_interface_s *uniqCfg = nullptr; }
extern "C" void exit_function(const char *, const char *, const int, const char *, const int) { abort(); }

extern "C" int logInit(void); /* nr_channel_level() uses LOG_D */
static std::mt19937 rng(12345);
/* amplitude classes: 0 full range incl. extremes, 1 medium, 2 small, 3 mostly-extreme */
static int16_t rnd16(int cls)
{
  switch (cls) {
    case 0: { int k = rng() % 16; if (k == 0) return -32768; if (k == 1) return 32767; if (k == 2) return -32767; return (int16_t)(rng() & 0xffff); }
    case 1: return (int16_t)((int)(rng() % 8001) - 4000);
    case 2: return (int16_t)((int)(rng() % 129) - 64);
    default: { int k = rng() % 4; return k == 0 ? -32768 : k == 1 ? 32767 : k == 2 ? 0 : (int16_t)(rng() & 0xffff); }
  }
}
static void fill(std::vector<nrd_c16> &v, int cls)
{
  for (auto &e : v) { e.r = rnd16(cls); e.i = rnd16(cls); }
}

int main()
{
  logInit();
  long total = 0;
  const int nbrx_l[] = {1, 2, 3, 4}, nl_l[] = {1, 2, 3, 4}, qm_l[] = {2, 4, 6, 8}, sh_l[] = {0, 3, 7, 11, 15, 20};
  const uint32_t len_l[] = {16, 48, 3280};
  /* ---- channel compensation ---- */
  long comp_bad = 0, comp_cases = 0;
  for (int nb_rx : nbrx_l) for (int nl : nl_l) for (int qm : qm_l) for (int sh : sh_l) for (uint32_t len : len_l) for (int cls = 0; cls < 4; cls++) {
    std::vector<nrd_c16> rxF((size_t)nb_rx * len), chF((size_t)nl * nb_rx * len);
    fill(rxF, cls); fill(chF, (cls + 1) % 4);
    const nrd_c16 poison = {0x5a5a, (int16_t)0xa5a5};
    std::vector<nrd_c16> cr((size_t)nl * len, poison), ca((size_t)nl * len, poison), cb((size_t)nl * len, poison), cc((size_t)nl * len, poison);
    std::vector<nrd_c16> mr(cr), ma(ca), mb(cb), mc(cc);
    std::vector<nrd_c16 *> rp(nl), mp(nl);
    for (int l = 0; l < nl; l++) { rp[l] = &cr[(size_t)l * len]; mp[l] = &mr[(size_t)l * len]; }
    nr_channel_compensation(len, len, nb_rx, nl, rxF.data(), chF.data(), ca.data(), cb.data(), cc.data(), rp.data(), nullptr, qm, 0, sh);
    for (int l = 0; l < nl; l++)
      for (uint32_t re = 0; re < len; re++)
        nrd_comp_re(nb_rx, len, re, qm, sh, rxF.data(), &chF[(size_t)l * nb_rx * len], &mr[(size_t)l * len], &ma[(size_t)l * len],
                    &mb[(size_t)l * len], &mc[(size_t)l * len]);
    long bad = 0;
    bad += memcmp(cr.data(), mr.data(), cr.size() * 4) != 0;
    bad += memcmp(ca.data(), ma.data(), ca.size() * 4) != 0;
    bad += memcmp(cb.data(), mb.data(), cb.size() * 4) != 0;
    bad += memcmp(cc.data(), mc.data(), cc.size() * 4) != 0;
    comp_cases++;
    if (bad && comp_bad < 5) printf("COMP MISMATCH nb_rx=%d nl=%d qm=%d shift=%d len=%u cls=%d (rx,a,b,c bad flags sum %ld)\n", nb_rx, nl, qm, sh, len, cls, bad);
    comp_bad += bad != 0;
  }
  printf("comp: cases=%ld mismatching_cases=%ld\n", comp_cases, comp_bad);
  total += comp_bad;
  /* ---- LLR ---- */
  long llr_bad = 0, llr_cases = 0;
  const uint32_t nre_l[] = {1, 2, 3, 4, 5, 7, 8, 9, 12, 100, 3139, 3276};
  for (int qm : qm_l) for (uint32_t nre : nre_l) for (int cls = 0; cls < 4; cls++) for (int rep = 0; rep < 3; rep++) {
    const uint32_t cap = (nre + 15) & ~15u;
    std::vector<nrd_c16> x(cap), a(cap), b(cap), c(cap);
    fill(x, cls); fill(a, (cls + rep) % 4); fill(b, (cls + 2 * rep) % 4); fill(c, (cls + 3 * rep) % 4);
    std::vector<int16_t> ref((size_t)cap * qm + 64, 0x1234), mod(ref);
    if (qm == 2) nr_qpsk_llr(x.data(), ref.data(), nre);
    else if (qm == 4) nr_16qam_llr(x.data(), a.data(), ref.data(), nre);
    else if (qm == 6) nr_64qam_llr(x.data(), a.data(), b.data(), ref.data(), nre);
    else nr_256qam_llr(x.data(), a.data(), b.data(), c.data(), ref.data(), nre);
    for (uint32_t re = 0; re < nre; re++) nrd_llr_re(qm, re, nre, x.data(), a.data(), b.data(), c.data(), mod.data());
    llr_cases++;
    const bool bad = memcmp(ref.data(), mod.data(), ref.size() * 2) != 0;
    if (bad && llr_bad < 5) printf("LLR MISMATCH qm=%d nb_re=%u cls=%d\n", qm, nre, cls);
    llr_bad += bad;
  }
  printf("llr: cases=%ld mismatching_cases=%ld\n", llr_cases, llr_bad);
  total += llr_bad;
  /* ---- scale + channel level ---- */
  long sc_bad = 0, lv_bad = 0, sc_cases = 0;
  const uint32_t slen_l[] = {1, 3, 4, 5, 12, 48, 100, 3139, 3276};
  for (int sh : {0, 1, 3, 7, 15}) for (uint32_t len : slen_l) for (int cls = 0; cls < 4; cls++) {
    std::vector<nrd_c16> a(len + 16);
    fill(a, cls);
    std::vector<nrd_c16> ref(a);
    nr_scale_channel((int)(len + 16), ref.data(), 0, len, 1, 1, sh);
    std::vector<nrd_c16> mod(a);
    for (uint32_t i = 0; i < len; i++) mod[i] = nrd_scale(a[i], sh);
    sc_bad += memcmp(ref.data(), mod.data(), ref.size() * 4) != 0;
    int32_t avg = 0;
    nr_channel_level(0, (int)(len + 16), a.data(), 1, 1, &avg, len);
    lv_bad += avg != nrd_channel_level_row(a.data(), len);
    sc_cases++;
  }
  printf("scale: cases=%ld mismatching=%ld ; channel_level: mismatching=%ld\n", sc_cases, sc_bad, lv_bad);
  total += sc_bad + lv_bad;
  printf("DEMOD_MODEL_MISMATCHES total=%ld\n", total);
  return total ? 1 : 0;
}
