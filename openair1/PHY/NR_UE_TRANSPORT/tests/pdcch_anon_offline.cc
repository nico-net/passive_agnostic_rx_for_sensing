// Offline analysis of ISAC_PDCCH_ANON_DUMP files (equalised PDCCH candidates of a configured-nID CORESET, dumped
// by the anonymous DCI length sweep). For every candidate passing the joint prescreen and every DCI length in
// [lmin, lmax] it runs (a) the plain polar decode after n_RNTI = 0 descrambling (CSS-type DCIs) and (b) the GF(2)
// joint (RNTI, payload) solver (USS DCIs scrambled with an unknown C-RNTI), then reports which (method, length,
// RNTI) recur across distinct occasions with distinct payloads -- chance hits do not recur.
// Usage: pdcch_anon_offline <dump> [lmin=30] [lmax=64] [max_occasions=0(all)]
#include <algorithm>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

extern "C" {
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"
#include "openair1/PHY/gold.h"
#include "nr_pdcch_joint_solve.h"
}
extern "C" configmodule_interface_t *uniqCfg = nullptr;
extern "C" void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  if (s) fprintf(stderr, "%s:%d %s() Exiting: %s\n", file, line, function, s);
  if (assert) abort(); else exit(EXIT_SUCCESS);
}

namespace {
struct Enc { uint16_t A; uint8_t L; };
int enc_cb(void *vctx, nr_dci_bits_t payload, uint16_t crc_mask, uint8_t *coded, int E)
{
  const Enc *c = (const Enc *)vctx;
  uint32_t out[(NR_PDCCH_JOINT_MAX_E + 31) / 32 + 1] = {0};
  polar_encoder_fast(payload.w, out, (int32_t)crc_mask, 1, NR_POLAR_DCI_MESSAGE_TYPE, c->A, c->L);
  nr_bit2byte_uint32_8(out, E, coded);
  return 0;
}
std::map<std::tuple<int, int, int>, std::pair<Enc *, nr_pdcch_joint_model_t *>> g_models;
nr_pdcch_joint_model_t *model(int A, int L, int nid)
{
  auto k = std::make_tuple(A, L, nid);
  auto it = g_models.find(k);
  if (it != g_models.end()) return it->second.second;
  Enc *e = new Enc{(uint16_t)A, (uint8_t)L};
  nr_pdcch_joint_model_t *m = nr_pdcch_joint_model_new_ex(enc_cb, e, A, L * 108, (uint16_t)nid, 1, 0);
  g_models[k] = {e, m};
  return m;
}
void descramble(const int16_t *in, int n, uint32_t rnti, uint16_t nid, int16_t *out)
{
  uint32_t x1 = 0, x2 = (uint32_t)((((uint64_t)rnti << 16) + nid) % (1ULL << 31));
  uint32_t w = gold_generic(&x1, &x2, 1);
  for (int i = 0; i < n; i++) {
    if (i && (i % 32) == 0) w = gold_generic(&x1, &x2, 0);
    out[i] = ((w >> (i % 32)) & 1) ? (int16_t)-in[i] : in[i];
  }
}
struct Ev { std::set<uint32_t> occ; std::set<uint64_t> pay; int hits = 0; int dl = 0; std::set<int> al; std::multiset<int> mm; };
}  // namespace

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s dump [lmin lmax max_occ]\n", argv[0]); return 2; }
  logInit();
  set_glog(OAILOG_DISABLE);
  const int lmin = argc > 2 ? atoi(argv[2]) : 30, lmax = argc > 3 ? atoi(argv[3]) : 64;
  const int maxocc = argc > 4 ? atoi(argv[4]) : 0;
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  std::map<std::tuple<int, int, int, int>, Ev> ev; // method, nid, len, rnti
  long n_occ = 0, n_cand = 0, n_screen = 0;
  std::map<int, long> cand_al, screen_al;
  char mg[4];
  while (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "PAD1", 4)) {
    int32_t h[6];
    if (fread(h, sizeof(h), 1, f) != 1) break;
    const int frame = h[0], slot = h[1], nid = h[2], nc = h[5];
    const uint32_t occ = (uint32_t)(frame * 40 + slot);
    ++n_occ;
    std::set<int> seen_cce_al;
    for (int c = 0; c < nc; ++c) {
      uint8_t l2[2]; uint16_t cce;
      if (fread(l2, 1, 2, f) != 2 || fread(&cce, 2, 1, f) != 1) goto done;
      const int L = l2[0], E = L * 108;
      std::vector<int16_t> e(E), z(E);
      if (fread(e.data(), 2, E, f) != (size_t)E) goto done;
      if (!seen_cce_al.insert(cce * 32 + L).second) continue; // the sweep may list a candidate twice
      ++n_cand; ++cand_al[L];
      if (!nr_pdcch_joint_prescreen(e.data(), E)) continue;
      ++n_screen; ++screen_al[L];
      descramble(e.data(), E, 0, (uint16_t)nid, z.data());
      for (int A = lmin; A <= lmax; ++A) {
        if (A + 24 > E) continue;
        uint64_t bits[4] = {0};
        const uint32_t crc = polar_decoder_int16(z.data(), bits, 1, NR_POLAR_DCI_MESSAGE_TYPE, A, L);
        if ((crc >> 16) == 0 && crc >= 1 && crc <= 65519) {
          Ev &x = ev[std::make_tuple(0, nid, A, (int)crc)];
          x.occ.insert(occ); x.pay.insert(bits[0]); ++x.hits; x.al.insert(L);
          x.dl += (int)((bits[0] >> (A - 1)) & 1);
        }
        if (A > 64) continue;
        nr_pdcch_joint_model_t *m = model(A, L, nid);
        if (!m) continue;
        nr_pdcch_joint_result_t r;
        if (nr_pdcch_joint_solve(m, z.data(), 0, &r) && r.rnti >= 1 && r.rnti <= 65519) {
          Ev &x = ev[std::make_tuple(1, nid, A, (int)r.rnti)];
          x.occ.insert(occ); x.pay.insert(r.payload.w[0]); ++x.hits; x.al.insert(L); x.mm.insert(r.mismatched_bits);
          x.dl += (int)((r.payload.w[0] >> (A - 1)) & 1);
        }
      }
    }
    if (maxocc && n_occ >= maxocc) break;
  }
done:
  printf("occasions=%ld candidates=%ld prescreen_pass=%ld\n", n_occ, n_cand, n_screen);
  for (auto &p : cand_al) printf("  AL%d candidates=%ld prescreen=%ld\n", p.first, p.second, screen_al[p.first]);
  std::vector<std::pair<int, std::tuple<int, int, int, int>>> top;
  long single = 0;
  for (auto &p : ev) {
    if (p.second.occ.size() >= 2) top.push_back({(int)p.second.occ.size(), p.first});
    else ++single;
  }
  std::sort(top.rbegin(), top.rend());
  printf("one-off (method,nid,len,rnti) tuples: %ld; recurrent: %zu\n", single, top.size());
  for (size_t i = 0; i < top.size() && i < 60; ++i) {
    auto &k = top[i].second;
    Ev &x = ev[k];
    std::string als;
    for (int a : x.al) als += std::to_string(a) + ",";
    int mmin = x.mm.empty() ? -1 : *x.mm.begin();
    printf("%s nid=%d len=%d rnti=0x%04x occasions=%zu payloads=%zu hits=%d dl_ind=%d AL={%s} min_mm=%d\n",
           std::get<0>(k) ? "JOINT " : "POLAR0", std::get<1>(k), std::get<2>(k), std::get<3>(k), x.occ.size(),
           x.pay.size(), x.hits, x.dl, als.c_str(), mmin);
  }
  return 0;
}
