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
#include <unistd.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_decoder/nrLDPC_types.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_ldpc.h"
#include "common/utils/LOG/log.h"

/* argv[2] == "ul": the same comparison through the UL pool (nr_pusch_gpu_ldpc_*, ISAC_UL_GPU_LDPC), which has a duty cap and a
 * context limit: a refused call (-1 busy, -2 over cap) is retried. */
static nr_gpu_ldpc_run_fn g_urun;
static int ul_retry_run(nr_gpu_ldpc_job_t *jobs, int n)
{
  int r = -1;
  for (int i = 0; i < 2000 && (r == -1 || r == -2); i++) {
    r = g_urun(jobs, n);
    if (r == -1 || r == -2)
      usleep(1000);
  }
  return r;
}
typedef int32_t (*ldpc_dec_fn)(t_nrLDPC_dec_params *, int8_t *, uint8_t *, t_nrLDPC_time_stats *, decode_abort_t *);
static int never(uint8_t *b, uint32_t n, uint8_t t) { (void)b; (void)n; (void)t; return 0; }

/* K bits compare (the bits of the last byte past K are not part of the block) */
static int bits_eq(const uint8_t *a, const uint8_t *b, int K)
{
  return !memcmp(a, b, K >> 3) && (!(K & 7) || !(((a[K >> 3] ^ b[K >> 3]) & 0xff) >> (8 - (K & 7)) << (8 - (K & 7)) & 0xff));
}

static double gauss(void)
{
  const double u = (drand48() + 1e-12), v = drand48();
  return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static int ncol_of(int BG, int R)
{
  return BG == 1 ? (R == 13 ? 68 : R == 23 ? 35 : 27) : (R == 15 ? 52 : R == 13 ? 32 : 17);
}




/* ---- LDPCPERCB: argv[2] == "bench" [reps]: where does the DL GPU LDPC time per code block go? (no radio)
 *   (a) CPU pre-step per TB: deinterleave + rate dematch + int8 pack, verbatim prep_segment() of nr_pdsch_gpu_ldpc_host.c
 *   (b) kernel time (cudaEvent on the launch-only entry nr_pdsch_gpu_ldpc_dev, own stream, all SMs)
 *   (c) wall of nr_pdsch_gpu_ldpc_run (module-owned stream: RT_ISOLATE / DISC_SMS apply; polls with ISAC_GPU_POLL_SLEEP_US)
 * BG1 Z=384 (K = 8448), C = 1..32 code blocks, it_max 8 or 16, decodable or noise-only LLRs, rows R = 13 / 23 / 89. */
#include "PHY/sse_intrin.h"
typedef int (*cuvoid)(void);
static double nowus(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}
static int cmpd(const void *a, const void *b) { return *(double *)a < *(double *)b ? -1 : *(double *)a > *(double *)b; }
static void stat3(double *v, int n, double *mean, double *p50, double *p99)
{
  qsort(v, n, sizeof(double), cmpd);
  double s = 0;
  for (int i = 0; i < n; i++)
    s += v[i];
  *mean = s / n;
  *p50 = v[n / 2];
  *p99 = v[(int)(0.99 * (n - 1))];
}
static int (*nr_rate_matching_ldpc_rx)(uint32_t, uint8_t, uint16_t, int16_t *, int16_t *, uint8_t, uint8_t, uint8_t, uint32_t, uint32_t, uint32_t);
static void (*nr_deinterleaving_ldpc)(uint32_t, uint8_t, int16_t *, int16_t *);
typedef int (*dev_fn)(const nr_gpu_ldpc_job_t *, int, const nr_gpu_buf_t *, nr_gpu_buf_t *, void *);
static int bench(void *g, void *h, int reps)
{
  int (*cMalloc)(void **, size_t) = dlsym(g, "cudaMalloc");
  int (*cSCreate)(void **, unsigned) = dlsym(g, "cudaStreamCreateWithFlags");
  int (*cECreate)(void **) = dlsym(g, "cudaEventCreate");
  int (*cERec)(void *, void *) = dlsym(g, "cudaEventRecord");
  int (*cESync)(void *) = dlsym(g, "cudaEventSynchronize");
  int (*cEEl)(float *, void *, void *) = dlsym(g, "cudaEventElapsedTime");
  int (*cSSync)(void *) = dlsym(g, "cudaStreamSynchronize");
  int (*cMemcpy)(void *, const void *, size_t, int) = dlsym(g, "cudaMemcpy");
  dev_fn dev = (dev_fn)dlsym(g, "nr_pdsch_gpu_ldpc_dev");
  nr_gpu_ldpc_run_fn run = (nr_gpu_ldpc_run_fn)dlsym(g, "nr_pdsch_gpu_ldpc_run");
  nr_rate_matching_ldpc_rx = dlsym(h, "nr_rate_matching_ldpc_rx");
  nr_deinterleaving_ldpc = dlsym(h, "nr_deinterleaving_ldpc");
  if (!nr_rate_matching_ldpc_rx || !nr_deinterleaving_ldpc || !cMalloc || !cSCreate || !cECreate || !cERec || !cESync || !cEEl || !cSSync || !cMemcpy || !dev || !run) {
    printf("BENCH setup failed: cudart symbols via libpdsch_gpu.so not found\n");
    return 2;
  }
  void *st, *e0, *e1;
  cSCreate(&st, 1);
  cECreate(&e0);
  cECreate(&e1);
  nr_gpu_buf_t lb = {.bytes = NR_GPU_LDPC_DEV_IN_BYTES, .model = NR_GPU_MODEL_DIRECT}, bb = {.bytes = NR_GPU_LDPC_DEV_BITS_BYTES, .model = NR_GPU_MODEL_DIRECT};
  if (cMalloc(&lb.dev, lb.bytes) || cMalloc(&bb.dev, bb.bytes)) {
    printf("BENCH cudaMalloc failed\n");
    return 2;
  }
  const int Z = 384, K = 8448, nmax = NR_GPU_LDPC_MAX_JOBS;
  static int8_t in[NR_GPU_LDPC_MAX_JOBS][68 * 384 + 64];
  static nr_gpu_ldpc_job_t jobs[NR_GPU_LDPC_MAX_JOBS];
  static int8_t flat[NR_GPU_LDPC_MAX_JOBS * 68 * 384];
  double *v = malloc(sizeof(double) * reps);
  srand48(777);
  const char *pe = getenv("ISAC_GPU_POLL_SLEEP_US"), *ri = getenv("ISAC_GPU_RT_ISOLATE"), *ds = getenv("ISAC_GPU_DISC_SMS"), *nc = getenv("ISAC_GPU_LDPC_NOCLUSTER");
  printf("BENCH_CFG poll_sleep_us=%s rt_isolate=%s disc_sms=%s nocluster=%s reps=%d\n", pe ? pe : "unset", ri ? ri : "unset", ds ? ds : "unset", nc ? nc : "unset", reps);
  { /* device facts: 16 = MultiProcessorCount, 97 = MaxSharedMemoryPerBlockOptin, 81 = MaxSharedMemoryPerMultiprocessor, 82 = MaxRegistersPerMultiprocessor */
    int (*cAttr)(int *, int, int) = dlsym(g, "cudaDeviceGetAttribute");
    if (!cAttr) { void *rt = dlopen("libcudart.so", RTLD_NOW); if (rt) cAttr = dlsym(rt, "cudaDeviceGetAttribute"); }
    if (cAttr) {
      int smc = 0, so = 0, sm = 0, rg = 0;
      cAttr(&smc, 16, 0); cAttr(&so, 97, 0); cAttr(&sm, 81, 0); cAttr(&rg, 82, 0);
      printf("BENCH_DEV sm_count=%d smem_optin=%d smem_per_sm=%d regs_per_sm=%d\n", smc, so, sm, rg);
    }
  }
  /* (a) CPU pre-step, E per rows set: R13 -> 25344, R23 -> 12672, R89 -> 9504 (Qm 6) */
  {
    const int Rs[3] = {13, 23, 89}, Es[3] = {25344, 12672, 9504};
    int16_t *llr = malloc(sizeof(int16_t) * 32 * 25344), *d = calloc(32 * 68 * 384, sizeof(int16_t)), *e = malloc(sizeof(int16_t) * 25344);
    for (int i = 0; i < 32 * 25344; i++)
      llr[i] = (int16_t)(60 * gauss());
    for (int ri2 = 0; ri2 < 3; ri2++)
      for (int C = 1; C <= 32; C *= 2) {
        for (int rep = 0; rep < reps; rep++) {
          const double a = nowus();
          for (int r = 0; r < C; r++) {
            const int E = Es[ri2];
            int16_t *dd = d + (size_t)r * 68 * Z;
            nr_deinterleaving_ldpc(E, 6, e, llr + (size_t)r * E);
            nr_rate_matching_ldpc_rx(1277992, 1, Z, dd, e, C, 0, 1, E, 0, K - 2 * Z);
            int16_t z[68 * 384 + 16] __attribute__((aligned(16)));
            memset(z, 0, 2 * Z * sizeof(*z));
            memcpy(z + 2 * Z, dd, (K - 2 * Z) * sizeof(*z));
            memcpy(z + K, dd + (K - 2 * Z), (68 * Z - K) * sizeof(*z));
            simde__m128i *pv = (simde__m128i *)&z, *pl = (simde__m128i *)(flat + (size_t)r * 68 * Z);
            for (int i = 0, j = 0; j < ((68 * Z) >> 4) + 1; i += 2, j++)
              pl[j] = simde_mm_packs_epi16(pv[i], pv[i + 1]);
          }
          v[rep] = nowus() - a;
        }
        double m, p5, p9;
        stat3(v, reps, &m, &p5, &p9);
        printf("BENCH_PRE R%d C=%d mean %.1f p50 %.1f p99 %.1f us (%.1f us/cb)\n", Rs[ri2], C, m, p5, p9, p5 / C);
      }
    /* CRC24B of one 8448-bit snapshot, the host's per-snapshot check */
    uint8_t snap[1056];
    for (int i = 0; i < 1056; i++)
      snap[i] = (uint8_t)lrand48();
    const double a = nowus();
    int dummy = 0;
    for (int i = 0; i < 2000; i++) {
      snap[0] = (uint8_t)i;
      dummy += check_crc(snap, K, CRC24_B);
    }
    printf("BENCH_CRC check_crc(8448 bit) %.2f us per snapshot (dummy %d)\n", (nowus() - a) / 2000, dummy);
  }
  /* (b) kernel and (c) run() wall */
  const int Rs[3] = {13, 23, 89}, ncolR[3] = {68, 35, 27};
  for (int ri2 = 0; ri2 < 3; ri2++)
    for (int it = 8; it <= 16; it += 8)
      for (int hop = 0; hop < 2; hop++)
        for (int ci = 0; ci < 32; ci++) {
          static const int clist[] = {1, 2, 4, 8, 16, 32}, cfine[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 32};
          const int nl = getenv("ISAC_BENCH_FINE") ? 14 : 6;
          if (ci >= nl)
            break;
          const int C = (getenv("ISAC_BENCH_FINE") ? cfine : clist)[ci];
          if (getenv("ISAC_BENCH_R") && atoi(getenv("ISAC_BENCH_R")) != Rs[ri2])
            continue;
          for (int k = 0; k < C; k++) {
            memset(in[k], 0, sizeof(in[k]));
            for (int i = 2 * Z; i < ncolR[ri2] * Z; i++) {
              const double x = hop ? 40.0 * gauss() : 12.0 * (1.0 + (ri2 == 0 ? 0.9 : 0.5) * gauss());
              in[k][i] = (int8_t)(x > 127 ? 127 : x < -128 ? -128 : lround(x));
            }
            jobs[k] = (nr_gpu_ldpc_job_t){.BG = 1, .R = Rs[ri2], .Z = Z, .K = K, .it_max = it, .llr = in[k]};
            cMemcpy((char *)lb.dev + (size_t)k * NR_GPU_LDPC_DEV_IN_STRIDE, in[k], NR_GPU_LDPC_DEV_IN_STRIDE, 1);
          }
          double kv[1024], lv[1024], wv[1024];
          const int n = reps < 1024 ? reps : 1024;
          for (int rep = -5; rep < n; rep++) {
            float ms = 0;
            const double a = nowus();
            cERec(e0, st);
            if (dev(jobs, C, &lb, &bb, st)) {
              printf("BENCH dev failed\n");
              return 3;
            }
            cERec(e1, st);
            const double b = nowus();
            cESync(e1);
            cEEl(&ms, e0, e1);
            if (rep >= 0) {
              kv[rep] = ms * 1000.0;
              lv[rep] = b - a;
            }
          }
          int nsn = 0, syn = 0;
          for (int rep = -5; rep < n; rep++) {
            const double a = nowus();
            if (run(jobs, C)) {
              printf("BENCH run failed\n");
              return 3;
            }
            if (rep >= 0)
              wv[rep] = nowus() - a;
          }
          for (int k = 0; k < C; k++) {
            nsn += jobs[k].nsnap;
            syn += jobs[k].syn0;
          }
          double mk, pk, qk, ml, pl2, ql, mw, pw, qw;
          stat3(kv, n, &mk, &pk, &qk);
          stat3(lv, n, &ml, &pl2, &ql);
          stat3(wv, n, &mw, &pw, &qw);
          printf("BENCH_GPU R%d it=%d %s C=%2d | kernel mean %.1f p50 %.1f p99 %.1f (%.1f us/cb) | launch-cpu p50 %.1f | run-wall mean %.1f p50 %.1f p99 %.1f (%.1f us/cb) | nsnap/cb %.1f syn0 %d/%d\n",
                 Rs[ri2], it, hop ? "noise  " : "decodbl", C, mk, pk, qk, pk / C, pl2, mw, pw, qw, pw / C, (double)nsn / C, syn, C);
        }
  return 0;
}

int main(int argc, char **argv)
{
  logInit();
  crcTableInit();
  void *h = dlopen("./libldpc.so", RTLD_NOW | RTLD_LOCAL);
  void *g = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  ldpc_dec_fn cpu = h ? (ldpc_dec_fn)dlsym(h, "LDPCdecoder") : NULL;
  const int ul = argc > 2 && !strcmp(argv[2], "ul");
  const int nc = argc > 2 && !strcmp(argv[2], "nc"); /* non-cluster kernels (what sm_89 runs), forced on this device; the CPU reference decides */
  if (nc)
    setenv("ISAC_GPU_LDPC_NOCLUSTER", "1", 1);
  if (argc > 2 && !strcmp(argv[2], "mix")) /* nj = 1..6 per launch: 1-3 cluster, 4-6 non-cluster, same CPU-reference comparison */
    setenv("ISAC_GPU_LDPC_NC_MIN_CB", "4", 1);
  nr_gpu_ldpc_init_fn ginit = g ? (nr_gpu_ldpc_init_fn)dlsym(g, "nr_pdsch_gpu_ldpc_init") : NULL;
  nr_gpu_ldpc_ul_init_fn uinit = g ? (nr_gpu_ldpc_ul_init_fn)dlsym(g, "nr_pusch_gpu_ldpc_init") : NULL;
  nr_gpu_ldpc_run_fn grun = g ? (nr_gpu_ldpc_run_fn)dlsym(g, ul ? "nr_pusch_gpu_ldpc_run" : "nr_pdsch_gpu_ldpc_run") : NULL;
  if (ul) {
    g_urun = grun;
    grun = ul_retry_run;
  }
  if (!cpu || !grun || (ul ? (!uinit || uinit(100, 1000000) != 0) : (!ginit || ginit() != 0))) {
    printf("LDPC_GPU_TEST setup failed (%s)\n", dlerror());
    return 2;
  }
  if (argc > 2 && !strcmp(argv[2], "bench"))
    return bench(g, h, argc > 3 ? atoi(argv[3]) : 100);
  if (nc) {
    nr_gpu_ldpc_stats_fn st = (nr_gpu_ldpc_stats_fn)dlsym(g, "nr_pdsch_gpu_ldpc_stats");
    uint64_t o[3] = {0};
    if (!st || (st(o), o[0] != 2)) {
      printf("LDPC_GPU_TEST nc: the non-cluster kernels are not selected (family %lu)\n", (unsigned long)o[0]);
      return 2;
    }
  }
  if (!ul) { /* F: refusals carry a reason */
    static nr_gpu_ldpc_job_t bad[1];
    static int8_t z[68 * 384 + 64];
    bad[0] = (nr_gpu_ldpc_job_t){.BG = 1, .R = 15 /* no such BG1 rate */, .Z = 64, .K = 22 * 64, .it_max = 8, .llr = z};
    if (grun(bad, 1) != NR_GPU_LDPC_E_GRAPH_MISS || grun(bad, 0) != NR_GPU_LDPC_E_RANGE) {
      printf("LDPC_GPU_TEST reason codes wrong\n");
      return 3;
    }
    bad[0].R = 13;
    bad[0].K = 8449;
    if (grun(bad, 1) != NR_GPU_LDPC_E_PARAM) {
      printf("LDPC_GPU_TEST param reason wrong\n");
      return 3;
    }
  }
  const int reps = argc > 1 ? atoi(argv[1]) : 3;
  srand48(12345);
  static const int zs1[] = {384, 352, 208, 112, 64, 36, 15, 2}, zs2[] = {384, 256, 96, 52, 20, 14, 6, 15, 11, 7}; /* Z % 4 != 0 (14, 6: BG2 with Kb = 8; 15, 11, 7: Kb = 10, K % 8 != 0) takes the scalar kernel */
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
    for (int zi = 0; zi < (BG == 1 ? 8 : 10); zi++)
      for (int ri = 0; ri < 3; ri++)
        for (int rep = 0; rep < reps; rep++) {
          const int Z = BG == 1 ? zs1[zi] : zs2[zi], R = BG == 1 ? rs1[ri] : rs2[ri], Kc = BG == 1 ? 68 : 52;
          const int K = (BG == 1 ? 22 : (Z == 14 || Z == 6) ? 8 : 10) * Z, ncol = ncol_of(BG, R), it_max = 8;
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
              if (!bits_eq(out, jobs[k].snap[s], K)) {
                if (n_bad++ < 10) {
                  int q = 0;
                  while (q < (K + 7) / 8 - 1 && out[q] == jobs[k].snap[s][q])
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
            if (it_gpu != it_cpu || (hit >= 0 && !bits_eq(out, jobs[k].snap[hit], K))) {
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
  printf("LDPC_GPU_TEST%s mismatches=%ld\n", ul ? " (UL pool)" : "", n_bad + n_stop_bad);
  return n_bad + n_stop_bad ? 1 : 0;
}
