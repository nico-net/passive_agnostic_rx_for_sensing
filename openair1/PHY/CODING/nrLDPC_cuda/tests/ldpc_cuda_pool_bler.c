/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BLER through nrLDPC_coding_decoder (the pool path), random payloads, real CRC + LDPC encoding, exact bit compare.
 *   ldpc_cuda_pool_bler <cuda|libldpc|libldpc_orig> <max_ldpc_iterations> [fallback] [trials]
 * cuda = libldpc_cuda.so (GPU runs 2x the iterations); fallback forces every GPU launch to fail (test hook) so the in-plugin CPU
 * decoder runs; libldpc/libldpc_orig = the repo's CPU reference plugins through the same harness.
 * Per Eb/N0: fail = decodeSuccess false; false_pass = CRC ok but bits differ from the source; pass_exact = CRC ok and bit-exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ldpc_cuda_pool_test_helper.h"
int main(int argc, char **argv)
{
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
