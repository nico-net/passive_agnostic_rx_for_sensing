/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BLER through nrLDPC_coding_decoder (the pool path), random payloads, real CRC + LDPC encoding, exact bit compare.
 *   ldpc_cuda_pool_bler <cuda|libldpc|libldpc_orig> <max_ldpc_iterations> [fallback] [trials]
 * cuda = libldpc_cuda.so (GPU runs 2x the iterations); fallback forces every GPU launch to fail (test hook) so the in-plugin CPU
 * decoder runs; libldpc/libldpc_orig = the repo's CPU reference plugins through the same harness.
 * Per Eb/N0: fail = decodeSuccess false; false_pass = CRC ok but bits differ from the source; pass_exact = CRC ok and bit-exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ldpc_cuda_pool_test_helper.h"
/* ---- paired-codeword dominance harness (CB0 entry vs the CPU TB decoder) ----
 *   ldpc_cuda_pool_bler paired <cpu_max_it> <trials> <bg1> <Z> <snr0> <snr1> <step> <cap> [cap ...]
 * Per Eb/N0 point: `trials` random-payload codewords (real CRC: CRC24A for K - 24 > 3824 else CRC16, the TB path's
 * rule; OAI encoder; QPSK AWGN), each decoded by
 *   - the CPU TB decoder (./libldpc.so nrLDPC_coding_decoder, C = 1, max_ldpc_iterations = cpu_max_it),
 *   - G1's CUDA TB path (./libldpc_cuda.so, same max_it: GPU runs 2 x) -- the cross-check: CB0 at cap 2 x max_it must
 *     give exactly G1's verdict on every codeword (g1_mismatch = 0),
 *   - the CB0 entry (ldpc_cb0_decode, one submission per cap) at every GPU iteration cap given.
 * Reported per (point, cap): cpu_pass, cb0_pass, cpu_pass_cb0_fail (the dominance violations: must be 0 at a usable
 * cap), cb0_pass_cpu_fail, false passes (CRC ok, bits != source) of both. */
#include "../nrLDPC_cb0_cuda.h"
static int paired(int argc, char **argv)
{
  if (argc < 10) {
    fprintf(stderr, "usage: %s paired cpu_max_it trials bg1 Z snr0 snr1 step cap [cap ...]\n", argv[0]);
    return 1;
  }
  const int M = atoi(argv[2]), n = atoi(argv[3]), bg1 = atoi(argv[4]), Z = atoi(argv[5]);
  const double s0 = atof(argv[6]), s1 = atof(argv[7]), st = atof(argv[8]);
  const int K = (bg1 ? 22 : 10) * Z, KB = K / 8, E = (bg1 ? 66 : 50) * Z, crc = K - 24 > 3824 ? 0 : 2;
  const size_t LS = 68 * 384;
  if (lcp_load_path("./libldpc_cuda.so") != 0 || lcp_load_ref("./libldpc.so") != 0) {
    fprintf(stderr, "load failed\n");
    return 2;
  }
  int (*dec)(const ldpc_cb0_item_t *, int, const int8_t *, size_t, ldpc_cb0_result_t *, uint8_t *, size_t) =
      (int (*)(const ldpc_cb0_item_t *, int, const int8_t *, size_t, ldpc_cb0_result_t *, uint8_t *, size_t))lcp_sym("ldpc_cb0_decode");
  if (!dec) {
    fprintf(stderr, "no CB0 entry\n");
    return 2;
  }
  short *llr = malloc((size_t)n * E * sizeof(short));
  int8_t *l = calloc((size_t)n, LS);
  uint8_t *src = calloc((size_t)n, KB + 8), *bits = malloc((size_t)n * LDPC_CB0_BITS_STRIDE), *cb = malloc(KB + 64);
  uint8_t *okc = malloc(n), *exc = malloc(n), *okg = malloc(n);
  ldpc_cb0_item_t *it = calloc(n, sizeof(*it));
  ldpc_cb0_result_t *r = calloc(n, sizeof(*r));
  printf("bg1,Z,K,crc,cpu_max_it,snr,trials,cap,cpu_pass,cb0_pass,cpu_pass_cb0_fail,cb0_pass_cpu_fail,cpu_false_pass,"
         "cb0_false_pass,g1_pass,g1_mismatch_at_2M,cb0_mean_iters,decoder_used_cpu_ok\n");
  for (double snr = s0; snr <= s1 + 1e-9; snr += st) {
    srand48(7000 + (long)lround(snr * 100) + 131L * Z + bg1);
    int du_bad = 0;
    for (int i = 0; i < n; i++) {
      if (lcp_make_cw(bg1, Z, crc, snr, 0, src + (size_t)i * (KB + 8), l + (size_t)i * LS, llr + (size_t)i * E) != K)
        return 3;
      uint8_t du = 0;
      lcp_decode_cw(1, bg1, Z, llr + (size_t)i * E, M, &okc[i], cb, &du);
      du_bad += du != LCP_CPU;
      exc[i] = okc[i] && !memcmp(cb, src + (size_t)i * (KB + 8), KB);
      lcp_decode_cw(0, bg1, Z, llr + (size_t)i * E, M, &okg[i], cb, &du);
      du_bad += du != LCP_CUDA; /* G1 must not have fallen back (LDPC_CUDA_TIMEOUT_MS generous) */
      it[i] = (ldpc_cb0_item_t){.BG = bg1 ? 1 : 2, .crc_type = (uint8_t)crc, .Z = (uint16_t)Z, .K = (uint16_t)K,
                                .Kprime = (uint16_t)K, .guard_bytes = 0, .iters = 0};
    }
    for (int c = 9; c < argc; c++) {
      const int cap = atoi(argv[c]);
      for (int i = 0; i < n; i++)
        it[i].iters = (uint8_t)cap;
      if (dec(it, n, l, LS, r, bits, LDPC_CB0_BITS_STRIDE) != 0) {
        fprintf(stderr, "CB0 decode failed\n");
        return 4;
      }
      int cp = 0, gp = 0, viol = 0, extra = 0, cfp = 0, gfp = 0, g1p = 0, g1m = 0;
      double its = 0;
      for (int i = 0; i < n; i++) {
        const int g = r[i].crc_ok == 1;
        cp += okc[i];
        gp += g;
        viol += okc[i] && !g;
        extra += g && !okc[i];
        cfp += okc[i] && !exc[i];
        gfp += g && memcmp(bits + (size_t)i * LDPC_CB0_BITS_STRIDE, src + (size_t)i * (KB + 8), KB);
        g1p += okg[i];
        g1m += cap == 2 * M && g != okg[i];
        its += r[i].iters;
      }
      printf("%d,%d,%d,%s,%d,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%s\n", bg1, Z, K, crc ? "CRC16" : "CRC24A", M, snr, n,
             cap, cp, gp, viol, extra, cfp, gfp, g1p, cap == 2 * M ? g1m : -1, its / n, du_bad ? "NO" : "yes");
      fflush(stdout);
    }
  }
  return 0;
}

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "paired"))
    return paired(argc, argv);
  if (argc < 3) { fprintf(stderr, "usage: %s cuda|libldpc|libldpc_orig iters [fallback] [trials]\n", argv[0]); return 1; }
  const int ref = strcmp(argv[1], "cuda") != 0;
  const int iters = atoi(argv[2]);
  const int fb = argc > 3 && !strcmp(argv[3], "fallback");
  const int n = argc > 4 ? atoi(argv[4]) : 300;
  char path[64];
  snprintf(path, sizeof(path), "./%s.so", ref ? argv[1] : "libldpc_cuda");
  if (lcp_load_path(path) != 0) { fprintf(stderr, "load %s failed\n", path); return 2; }
  if (fb) lcp_hooks(1, 0, 0, -1, -1, 1000000, -1);
  const double snr[] = {1.0, 1.5, 2.0, 2.5, 3.0};
  for (int s = 0; s < 5; s++) {
    srand48(1000 + (long)(snr[s] * 10)); /* the same codewords and noise for every plugin */
    int fail = 0, fp = 0, exact = 0, cpu = 0, cuda = 0;
    for (int i = 0; i < n; i++) {
      uint8_t ok = 0, match = 0, du = 0;
      if (lcp_random_tb(1, 384, 1, snr[s], iters, &ok, &match, &du) < 0) { fprintf(stderr, "harness error\n"); return 3; }
      if (!ok) fail++;
      else if (!match) fp++;
      else exact++;
      cpu += du == LCP_CPU;
      cuda += du == LCP_CUDA;
    }
    printf("%s%s it=%d EbN0=%.1f n=%d pass_exact=%d fail=%d false_pass=%d bler_exact=%.3f decoder_used cpu=%d cuda=%d\n", argv[1],
           fb ? "+fallback" : "", iters, snr[s], n, exact, fail, fp, (double)(n - exact) / n, cpu, cuda);
    fflush(stdout);
  }
  return 0;
}
