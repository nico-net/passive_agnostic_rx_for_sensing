/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* BLER through nrLDPC_coding_decoder (the pool path): `ldpc_cuda_pool_bler cuda|cpu_fallback|libldpc [trials]`; libldpc = the repo's CPU reference plugin, same harness.
 * cpu_fallback forces every launch to fail (test hook), so decoding runs on the in-plugin CPU layered decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ldpc_cuda_pool_test_helper.h"
int main(int argc, char **argv)
{
  const int fb = argc > 1 && !strcmp(argv[1], "cpu_fallback");
  const int n = argc > 2 ? atoi(argv[2]) : 300;
  const int ref = argc > 1 && !strcmp(argv[1], "libldpc");
  if ((ref ? lcp_load_path("./libldpc.so") : lcp_load()) != 0) { fprintf(stderr, "load failed\n"); return 2; }
  if (fb) lcp_hooks(1, 0, 0, -1, -1, 1000000, -1);
  srand48(12345);
  const double snr[] = {1.0, 1.5, 2.0, 2.5, 3.0};
  for (int s = 0; s < 5; s++) {
    int err = 0, cpu = 0, cuda = 0;
    for (int i = 0; i < n; i++) {
      uint8_t du = 0;
      err += lcp_bler_trial(snr[s], &du);
      cpu += du == LCP_CPU;
      cuda += du == LCP_CUDA;
    }
    printf("%s EbN0=%.1f BLER %f (%d/%d) decoder_used: cpu_layered=%d cuda_flooding=%d\n", argv[1] ? argv[1] : "cuda", snr[s],
           (double)err / n, err, n, cpu, cuda);
    fflush(stdout);
  }
  return 0;
}
