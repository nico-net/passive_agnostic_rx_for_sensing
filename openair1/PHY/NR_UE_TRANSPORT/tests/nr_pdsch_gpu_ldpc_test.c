/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* DECGPU exact LDPC: the GPU decoder (libpdsch_gpu.so nr_pdsch_gpu_ldpc_run) against OAI's CPU decoder
 * (./libldpc.so LDPCdecoder) on the same int8 inputs, every BG / rate row set, several Z, from hopeless to clean.
 *  1. every snapshot: the GPU hard decision after iteration s + 3 == LDPCdecoder(numMaxIter = s + 2) with a CRC
 *     callback that never passes (it then runs all iterations and leaves the last hard decision in p_out);
 *  2. the stop rule: noisy all-zero codewords with the real check_crc -- the host rule (first snapshot whose CRC
 *     passes, else fail) gives LDPCdecoder's verdict and numIter.
 * Needs a CUDA device. Prints LDPC_GPU_TEST mismatches=0 on success. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.h"
#include "common/utils/LOG/log.h"

typedef int32_t (*ldpc_dec_fn)(t_nrLDPC_dec_params *, int8_t *, uint8_t *, t_nrLDPC_time_stats *, decode_abort_t *);
static int never(uint8_t *b, uint32_t n, uint8_t t) { (void)b; (void)n; (void)t; return 0; }

static double gauss(void)
{
  const double u = (drand48() + 1e-12), v = drand48();
  return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int ncol_of(int BG, int R)
{
  return BG == 1 ? (R == 13 ? 68 : R == 23 ? 35 : 27) : (R == 15 ? 52 : R == 13 ? 32 : 17);
}



int main(int argc, char **argv)
{
  logInit();
  crcTableInit();
  void *h = dlopen("./libldpc.so", RTLD_NOW | RTLD_LOCAL);
  void *g = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  ldpc_dec_fn cpu = h ? (ldpc_dec_fn)dlsym(h, "LDPCdecoder") : NULL;
  nr_gpu_ldpc_init_fn ginit = g ? (nr_gpu_ldpc_init_fn)dlsym(g, "nr_pdsch_gpu_ldpc_init") : NULL;
  nr_gpu_ldpc_run_fn grun = g ? (nr_gpu_ldpc_run_fn)dlsym(g, "nr_pdsch_gpu_ldpc_run") : NULL;
  if (!cpu || !ginit || !grun || ginit() != 0) {
    printf("LDPC_GPU_TEST setup failed (%s)\n", dlerror());
    return 2;
  }
  const int reps = argc > 1 ? atoi(argv[1]) : 3;
  srand48(12345);
  static const int zs1[] = {384, 352, 208, 112, 64, 36, 15, 2}, zs2[] = {384, 256, 96, 52, 20, 14, 6}; /* Z % 4 != 0 (14, 6: BG2 with Kb = 8) takes the scalar kernel */
  static const int rs1[] = {13, 23, 89}, rs2[] = {15, 13, 23};
  t_nrLDPC_time_stats *ts = calloc(1, 1 << 16);
  decode_abort_t ab;
  init_abort(&ab);
  static nr_gpu_ldpc_job_t jobs[NR_GPU_LDPC_MAX_JOBS];
  static int8_t in[NR_GPU_LDPC_MAX_JOBS][68 * 384 + 64];
  static int8_t inc[68 * 384 + 64];
  static uint8_t out[27000 + 64];
  long n_cmp = 0, n_bad = 0, n_stop = 0, n_stop_bad = 0, n_ok = 0, n_fb = 0;
  double t_gpu = 0, t_cpu = 0;
  for (int BG = 1; BG <= 2; BG++)
    for (int zi = 0; zi < (BG == 1 ? 8 : 7); zi++)
      for (int ri = 0; ri < 3; ri++)
        for (int rep = 0; rep < reps; rep++) {
          const int Z = BG == 1 ? zs1[zi] : zs2[zi], R = BG == 1 ? rs1[ri] : rs2[ri], Kc = BG == 1 ? 68 : 52;
          const int K = (BG == 1 ? 22 : (Z & 3) ? 8 : 10) * Z, ncol = ncol_of(BG, R), it_max = 8;
          if (K & 7)
            continue;
          const int nj = 1 + (int)(lrand48() % 6);
          for (int k = 0; k < nj; k++) { /* all-zero codeword (valid, CRC 0) through noise of a random level, or junk */
            const int mode = (int)(lrand48() % 5);
            const double sigma = 0.3 + 0.4 * k + 0.2 * (lrand48() % 4);
            const double amp = mode == 4 ? 300.0 : 6.0 + 4.0 * (lrand48() % 4);
            memset(in[k], 0, sizeof(in[k]));
            for (int i = 2 * Z; i < ncol * Z; i++) {
              double v = mode == 3 ? 160.0 * (drand48() - 0.5) * 2 : amp * (1.0 + sigma * gauss());
              in[k][i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : lround(v));
            }
            jobs[k] = (nr_gpu_ldpc_job_t){.BG = BG, .R = R, .Z = Z, .K = K, .it_max = it_max, .llr = in[k]};
          }
          struct timespec a, b;
          clock_gettime(CLOCK_MONOTONIC, &a);
          if (grun(jobs, nj) != 0) {
            printf("LDPC_GPU_TEST run failed BG %d Z %d R %d\n", BG, Z, R);
            return 3;
          }
          clock_gettime(CLOCK_MONOTONIC, &b);
          t_gpu += (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
          for (int k = 0; k < nj; k++) {
            /* 1. snapshots */
            for (int s = 0; s < jobs[k].nsnap; s++) {
              t_nrLDPC_dec_params p = {.BG = BG, .Z = Z, .R = R, .numMaxIter = s + 2, .Kprime = K, .outMode = nrLDPC_outMode_BIT,
                                       .crc_type = CRC24_B, .check_crc = never};
              memcpy(inc, in[k], Kc * Z);
              set_abort(&ab, false);
              memset(out, 0, sizeof(out));
              cpu(&p, inc, out, ts, &ab);
              n_cmp++;
              if (memcmp(out, jobs[k].snap[s], K >> 3) != 0) {
                if (n_bad++ < 10) {
                  int q = 0;
                  while (out[q] == jobs[k].snap[s][q])
                    q++;
                  printf("MISMATCH BG %d Z %d R %d job %d snapshot %d (iteration %d): byte %d cpu %02x gpu %02x\n", BG, Z, R, k, s, s + 3,
                         q, out[q], jobs[k].snap[s][q]);
                }
              }
            }
            if (jobs[k].nsnap != it_max - 1 && !jobs[k].syn0)
              printf("BAD nsnap %d syn0 %d\n", jobs[k].nsnap, jobs[k].syn0), n_bad++;
            /* 2. stop rule with the real CRC (all-zero payload: CRC24B of zeros is 0) */
            t_nrLDPC_dec_params p = {.BG = BG, .Z = Z, .R = R, .numMaxIter = it_max, .Kprime = K, .outMode = nrLDPC_outMode_BIT,
                                     .crc_type = CRC24_B, .check_crc = check_crc};
            memcpy(inc, in[k], Kc * Z);
            set_abort(&ab, false);
            memset(out, 0, sizeof(out));
            clock_gettime(CLOCK_MONOTONIC, &a);
            const int it_cpu = cpu(&p, inc, out, ts, &ab);
            clock_gettime(CLOCK_MONOTONIC, &b);
            t_cpu += (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
            int hit = -1;
            for (int s = 0; s < jobs[k].nsnap && hit < 0; s++)
              if (check_crc(jobs[k].snap[s], K, CRC24_B))
                hit = s;
            n_stop++;
            if (hit < 0 && jobs[k].syn0) {
              n_fb++; /* the host would redo this TB on the CPU */
              continue;
            }
            const int it_gpu = hit >= 0 ? hit + 1 : it_max;
            n_ok += hit >= 0;
            if (it_gpu != it_cpu || (hit >= 0 && memcmp(out, jobs[k].snap[hit], K >> 3) != 0)) {
              if (n_stop_bad++ < 10)
                printf("STOP MISMATCH BG %d Z %d R %d job %d: cpu numIter %d gpu %d\n", BG, Z, R, k, it_cpu, it_gpu);
            }
          }
        }
  printf("LDPC_GPU_TEST snapshots=%ld mismatches=%ld stop_cases=%ld (decoded %ld, cpu-fallback %ld) stop_mismatches=%ld gpu %.0f us total, cpu(stop run) %.0f us total\n",
         n_cmp, n_bad, n_stop, n_ok, n_fb, n_stop_bad, t_gpu, t_cpu);
  /* warm timing: BG1 Z=384 R=1/3, n code blocks per launch, decodable (stops at a zero syndrome) and hopeless (9 iterations) */
  for (int hop = 0; hop < 2; hop++)
    for (int nj = 1; nj <= 4; nj *= 2) {
      for (int k = 0; k < nj; k++) {
        memset(in[k], 0, sizeof(in[k]));
        for (int i = 2 * 384; i < 68 * 384; i++) {
          const double v = hop ? 40.0 * gauss() : 12.0 * (1.0 + 0.9 * gauss());
          in[k][i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : lround(v));
        }
        jobs[k] = (nr_gpu_ldpc_job_t){.BG = 1, .R = 13, .Z = 384, .K = 8448, .it_max = 8, .llr = in[k]};
      }
      double best = 1e9, sum = 0;
      for (int rep = 0; rep < 50; rep++) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        grun(jobs, nj);
        clock_gettime(CLOCK_MONOTONIC, &b);
        const double us = (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
        sum += us;
        best = us < best ? us : best;
      }
      t_nrLDPC_dec_params p = {.BG = 1, .Z = 384, .R = 13, .numMaxIter = 8, .Kprime = 8448, .outMode = nrLDPC_outMode_BIT,
                               .crc_type = CRC24_B, .check_crc = check_crc};
      struct timespec a, b;
      clock_gettime(CLOCK_MONOTONIC, &a);
      for (int k = 0; k < nj; k++) {
        memcpy(inc, in[k], 68 * 384);
        set_abort(&ab, false);
        cpu(&p, inc, out, ts, &ab);
      }
      clock_gettime(CLOCK_MONOTONIC, &b);
      printf("LDPC_GPU_BENCH BG1 Z384 R13 %s cbs=%d nsnap=%d gpu mean %.0f us min %.0f us, cpu %.0f us\n", hop ? "hopeless" : "decodable", nj,
             jobs[0].nsnap, sum / 50, best, (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3);
    }
  printf("LDPC_GPU_TEST mismatches=%ld\n", n_bad + n_stop_bad);
  return n_bad + n_stop_bad ? 1 : 0;
}
