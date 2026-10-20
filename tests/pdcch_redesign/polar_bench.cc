// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// PDCCH-REDESIGN-20261019 micro-benchmark 1: DCI polar decode on the DGX Grace (aarch64).
//   ref   = polar_decoder_int16() as shipped (OAI SC with all-frozen-subtree pruning, simde int16, scalar leaves)
//   fssc  = prototype fast-SSC (rate-0 / rate-1 / REP / SPC nodes, NEON int16), one candidate per call
//   batch = prototype fast-SSC with 8 candidates in the 8 int16 lanes (same tree walk for all 8)
// Build: build.sh next to this file (g++, no cmake). Run: taskset -c 0-4 nice -n 10 ./polar_bench (inside heavy.sh)
// Only DCI lengths 33..64 are supported by the prototype finish() (A + 24 CRC <= 128 bits, A <= 64).
#include "polar_fssc.h"
// ---------------------------------------------------------------- test vectors
static void make_vec(const Code &c, uint16_t rnti, double amp, double sigma, int16_t *llr, uint64_t *payload_out)
{
  uint8_t out[1024 / 8 + 16] = {0};
  uint64_t A[NR_DCI_WORDS] = {((uint64_t)rnd() << 32 | rnd()) & ((1ULL << c.len) - 1), 0, 0};
  *payload_out = A[0];
  polar_encoder_fast(A, out, rnti, 1, NR_POLAR_DCI_MESSAGE_TYPE, c.len, c.al);
  for (int i = 0; i < c.E; i++) {
    const int bit = (out[i >> 3] >> (i & 7)) & 1;
    double v = (bit ? -amp : amp) + sigma * gauss();
    v = std::max(-32000.0, std::min(32000.0, v));
    llr[i] = (int16_t)lrint(v);
  }
}

int main(int argc, char **argv)
{
  crcTableInit();
  if (argc > 2 && !strcmp(argv[1], "dump")) { // dump code tables + vectors + expected u for gpu_bench.cu: polar_bench dump <dir>
    const int cfgs[][2] = {{47, 1}, {47, 2}, {47, 4}, {47, 8}, {49, 2}, {49, 4}, {44, 1}};
    for (auto &cf : cfgs) {
      Code *cp = new Code(); Code &c = *cp; code_init(c, cf[0], cf[1]);
      char fn[256]; snprintf(fn, sizeof fn, "%s/code_%d_%d.bin", argv[2], cf[0], cf[1]); FILE *f = fopen(fn, "wb");
      int32_t h[8] = {c.len, c.al, c.N, c.n, c.K, c.E, c.punct_mode, (int)c.nodes.size()}; fwrite(h, 4, 8, f);
      int32_t rt = c.root; fwrite(&rt, 4, 1, f);
      fwrite(c.rmp.data(), 2, c.E, f); fwrite(c.info.data(), 1, c.N, f);
      int32_t ni = (int)c.idx.size(); fwrite(&ni, 4, 1, f); fwrite(c.idx.data(), 2, ni, f); fwrite(c.tgt.data(), 2, ni, f);
      fwrite(c.nodes.data(), sizeof(Node), c.nodes.size(), f);
      const int NV = 256; fwrite(&NV, 4, 1, f);
      for (int v = 0; v < NV; v++) {
        std::vector<int16_t> llr(c.E); uint64_t pl; make_vec(c, 0x591c, 20, 12, llr.data(), &pl);
        fwrite(llr.data(), 2, c.E, f);
        uint64_t fo[NR_DCI_WORDS]; fssc_decode(c, llr.data(), fo); fwrite(c.x, 1, c.N, f); // x after polar transform = u
      }
      fclose(f); delete cp;
    }
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "breakdown")) { // where the prototype single-candidate time goes (us per decode)
    const int cf[][2] = {{47, 1}, {47, 2}, {47, 4}, {47, 8}};
    for (auto &q : cf) {
      Code *cp = new Code(); Code &c = *cp; code_init(c, q[0], q[1]); const int NV = 256; const int R = 200;
      std::vector<std::vector<int16_t>> vecs(NV, std::vector<int16_t>(c.E)); uint64_t pl, out[NR_DCI_WORDS];
      for (int v = 0; v < NV; v++) make_vec(c, 0x591c, 20, 12, vecs[v].data(), &pl);
      double t[4] = {0, 0, 0, 0}; uint32_t sink = 0;
      for (int r = 0; r < R; r++) for (int v = 0; v < NV; v++) {
        double a = now_us(); dematch(c, vecs[v].data(), c.dt); double b = now_us();
        run_node(c, &c.nodes[c.root], c.dt, c.x); double d = now_us(); polar_transform_bytes(c.x, c.N); double e = now_us();
        sink += finish(c, c.x, out); double f = now_us(); t[0] += b - a; t[1] += d - b; t[2] += e - d; t[3] += f - e; }
      printf("AL%d N=%d: dematch %.3f | tree %.3f | transform %.3f | extract+crc %.3f us (timer overhead ~0.03 each) (%u)\n", q[1], c.N, t[0] / (R * NV), t[1] / (R * NV), t[2] / (R * NV), t[3] / (R * NV), sink & 1);
      delete cp;
    }
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "debug")) { // POLAR_BENCH_DEBUG: clean vectors, show where the reference differs
    Code *cp = new Code(); code_init(*cp, 47, argc > 2 ? atoi(argv[2]) : 4); uint64_t pl, out[NR_DCI_WORDS]; std::vector<int16_t> v(cp->E), t(cp->E);
    for (int k = 0; k < 12; k++) { make_vec(*cp, 0x591c, argc > 3 ? atof(argv[3]) : 200, argc > 3 ? atof(argv[3]) / 10 : 20, v.data(), &pl); t = v; uint32_t rr = polar_decoder_int16(t.data(), out, 1, NR_POLAR_DCI_MESSAGE_TYPE, 47, cp->al); uint64_t fo[NR_DCI_WORDS]; uint32_t rf = fssc_decode(*cp, v.data(), fo);
      printf("sent %012llx ref res=%06x pay=%012llx | fssc res=%06x pay=%012llx\n", (unsigned long long)pl, rr, (unsigned long long)out[0], rf, (unsigned long long)fo[0]); }
    return 0; }
  const int lens[] = {44, 47, 49};
  const int als[] = {1, 2, 4, 8};
  const int NV = 256;
  printf("# polar_bench: us per decode, mean of %d vectors x 40 reps (input memcpy of the ref removed)\n", NV);
  printf("%-4s %-2s %-4s %-4s %-5s | %-7s %-7s %-7s %-7s | decodes ok at sigma/amp = 0.6, 0.9, 1.2: ref/fssc/batch8 (of 2000)\n",
         "len", "AL", "N", "E", "nodes", "ref", "params", "fssc", "batch8");
  for (int len : lens)
    for (int al : als) {
      Code *cp = new Code(); Code &c = *cp;
      code_init(c, len, al);
      const uint16_t rnti = 0x591c;
      std::vector<std::vector<int16_t>> vecs(NV, std::vector<int16_t>(c.E));
      std::vector<uint64_t> pay(NV);
      const double amp = getenv("AMP") ? atof(getenv("AMP")) : 20.0; // production autoscale targets p90 ~ 24 (dci_nr.c)
      for (int v = 0; v < NV; v++) make_vec(c, rnti, amp, amp * 0.6, vecs[v].data(), &pay[v]);
      uint64_t out[NR_DCI_WORDS];
      std::vector<int16_t> tmp(c.E);
      for (int v = 0; v < NV; v++) { memcpy(tmp.data(), vecs[v].data(), c.E * 2); polar_decoder_int16(tmp.data(), out, 1, NR_POLAR_DCI_MESSAGE_TYPE, len, al); }
      const int reps = 40;
      uint32_t sink = 0;
      double t0 = now_us();
      for (int r = 0; r < reps; r++)
        for (int v = 0; v < NV; v++) { memcpy(tmp.data(), vecs[v].data(), c.E * 2); sink += polar_decoder_int16(tmp.data(), out, 1, NR_POLAR_DCI_MESSAGE_TYPE, len, al); }
      double t_ref = (now_us() - t0) / (reps * NV);
      t0 = now_us();
      for (int r = 0; r < reps; r++) for (int v = 0; v < NV; v++) { memcpy(tmp.data(), vecs[v].data(), c.E * 2); asm volatile("" ::: "memory"); }
      t_ref -= (now_us() - t0) / (reps * NV);
      t0 = now_us();
      for (int r = 0; r < reps * NV; r++) { t_nrPolar_params *p = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, len, al); polarReturn(p); }
      double t_par = (now_us() - t0) / (reps * NV);
      t0 = now_us();
      for (int r = 0; r < reps; r++) for (int v = 0; v < NV; v++) sink += fssc_decode(c, vecs[v].data(), out);
      double t_f = (now_us() - t0) / (reps * NV);
      const int16_t *ptr[8]; uint32_t res[8]; uint64_t pp[8];
      t0 = now_us();
      for (int r = 0; r < reps; r++)
        for (int v = 0; v + 8 <= NV; v += 8) { for (int j = 0; j < 8; j++) ptr[j] = vecs[v + j].data(); batch_decode8(c, ptr, res, pp); sink += res[0]; }
      double t_b = (now_us() - t0) / (reps * (NV / 8) * 8);
      char agree[256] = "";
      const double sig_rel[] = {0.6, 0.9, 1.2};
      for (double sr : sig_rel) {
        int okr = 0, okf = 0, okb = 0, tot = 2000;
        std::vector<std::vector<int16_t>> tv(8, std::vector<int16_t>(c.E));
        for (int t = 0; t < tot; t += 8) {
          uint64_t pl[8];
          for (int j = 0; j < 8; j++) make_vec(c, rnti, amp, amp * sr, tv[j].data(), &pl[j]);
          for (int j = 0; j < 8; j++) ptr[j] = tv[j].data();
          batch_decode8(c, ptr, res, pp);
          for (int j = 0; j < 8; j++) {
            memcpy(tmp.data(), tv[j].data(), c.E * 2);
            const uint32_t rr = polar_decoder_int16(tmp.data(), out, 1, NR_POLAR_DCI_MESSAGE_TYPE, len, al);
            uint64_t fo[NR_DCI_WORDS];
            const uint32_t rf = fssc_decode(c, tv[j].data(), fo);
            okr += (rr == rnti && out[0] == pl[j]);
            okf += (rf == rnti && fo[0] == pl[j]);
            okb += (res[j] == rnti && pp[j] == pl[j]);
          }
        }
        char b[96]; snprintf(b, sizeof b, " %d/%d/%d", okr, okf, okb); strcat(agree, b);
      }
      printf("%-4d %-2d %-4d %-4d %-5zu | %-7.3f %-7.3f %-7.3f %-7.3f |%s (%u)\n", len, al, c.N, c.E, c.nodes.size(), t_ref, t_par, t_f, t_b, agree, sink & 1);
      delete cp;
    }
  return 0;
}
