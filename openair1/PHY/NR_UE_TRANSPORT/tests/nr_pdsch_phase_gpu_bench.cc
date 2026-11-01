/* SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * GPUPERF microbench: k_tinterp, k_cpe, k_irc (+ k_irc_final) of libpdsch_gpu.so for a synthetic grant, per launch in microseconds (cudaEvent over `reps` back-to-back launches).
 *   bench_nr_pdsch_phase_gpu [reps=200]      table over 51/106/217/273 PRB, Nl 1..4, 4 RX, double and float. Run from the build directory; the GPU must be idle (flock /tmp/td_measure.lock). */
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include "nr_mmse_exact.h"

int main(int argc, char **argv)
{
  const int reps = argc > 1 ? atoi(argv[1]) : 200;
  void *plat = dlopen("./libnr_gpu_platform.so", RTLD_NOW | RTLD_GLOBAL);
  void *dl = plat ? dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL) : nullptr;
  if (!dl) { fprintf(stderr, "no libpdsch_gpu.so\n"); return 77; }
  auto init = (int (*)(int))dlsym(dl, "nr_pdsch_gpu_demod_init");
  auto bd = (int (*)(int, int, int, int, int, const void *, float *))dlsym(dl, "nr_pdsch_gpu_demod_bench");
  auto bi = (int (*)(int, int, int, int, void *, float *))dlsym(dl, "nr_pdsch_gpu_bench_irc");
  if (!init || !bd || !bi || init(4) != 0) { fprintf(stderr, "module not ready\n"); return 77; }
  void *acc = nullptr;
  cudaMalloc(&acc, sizeof(nr_irc_sum_t) * NR_IRC_NG_MAX);
  printf("%-4s %-3s %-5s | %9s %9s %9s %9s | %9s %9s\n", "PRB", "Nl", "prec", "tinterp", "cpe", "irc", "irc_final", "sum(us)", "prod(us)"); /* prod: tinterp+irc_final fused + cpe + irc */
  for (int prb : {51, 106, 217, 273})
    for (int nl = 1; nl <= 4; nl++)
      for (int f32 = 0; f32 < 2; f32++) {
        float u[4] = {0, 0, 0, 0}, ui = 0;
        const int nb = 4;
        if (bi(prb, nl, nb, reps, acc, &ui) || bd(prb, nl, nb, f32, reps, acc, u)) { printf("%d %d: failed\n", prb, nl); continue; }
        printf("%-4d %-3d %-5s | %9.1f %9.1f %9.1f %9.1f | %9.1f %9.1f\n", prb, nl, f32 ? "float" : "dbl", u[0], u[1], ui, u[2], u[0] + u[1] + ui + u[2], u[3] + u[1] + ui);
      }
  return 0;
}
