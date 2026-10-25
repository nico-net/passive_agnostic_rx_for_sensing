/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* test_nr_gpu_isa_twin: the GPU twins of the host CPU's ISA-dependent primitives (nr_gpu_isa_twin.cuh), bit for bit,
 * against THIS host's CPU: dft()/idft() of the libdfts.so built here and rotate_cpx_vector/mult_cpx_vector compiled with the
 * project flags (nr_gpu_isa_ref.c).
 *   1. runtime selection (nr_exact::isa_profile) finds a profile;
 *   2. the selected DFT twin == CPU dft/idft for every size it covers (x86: 64..4096, NEON: 4096), 14 vectors each
 *      (random at 5 amplitudes x 2 seeds, full int16 range, full-scale tone, impulse pair, constant -32768);
 *      the other DFT twin differs (the self-test can tell them apart);
 *   3. the selected rotation twin == CPU on 16 rotations x 6 range lengths; the other twins differ;
 *   4. k_fep_exact<profile> == CPU dft + apply_nr_rotation_symbol_RX for 14 symbols x 2 antennas (273 PRB);
 *   5. k_delay_exact<profile> == nr_passive_est_delay (CPU idft4096, the same scaling / peak rules) on 12 LS rows.
 * Exit 0 pass, 1 fail, 77 no CUDA device. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cuda_runtime.h>
#include "nr_pdsch_gpu_exact_fep.cuh"

/* libdfts.so resolves its LOG / T hooks from the executable; this test has no LOG module (none is ever called on the
 * aligned path). g_log points at zeroed storage (all dump flags and levels off): the aarch64 library (oai_dfts_neon.c)
 * reads LOG_DUMPFLAG(DEBUG_DFT) = g_log->dump_mask in dft128 and references write_file_matlab (LOG_M). */
alignas(64) static unsigned char g_log_zero[1 << 16];
extern "C" {
void *g_log = g_log_zero;
int T_stdout = 1;
volatile int *T_freelist_head = nullptr;
void *T_cache = nullptr;
int *T_active = nullptr;
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert) { (void)file; (void)function; (void)line; (void)s; (void)assert; abort(); }
void logRecord_mt(const char *file, const char *func, int line, int comp, int level, const char *format, ...) { (void)file; (void)func; (void)line; (void)comp; (void)level; (void)format; }
int32_t write_file_matlab(const char *fname, const char *vname, void *data, int length, int dec, unsigned int format, int multiVec) { (void)fname; (void)vname; (void)data; (void)length; (void)dec; (void)format; (void)multiVec; return 0; }
}

using namespace nr_exact;
static int g_fail = 0;
#define CHECK(c, ...)                         \
  do {                                        \
    if (!(c)) {                               \
      g_fail++;                               \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                    \
      printf("\n");                           \
    }                                         \
  } while (0)
#define CU(x)                                                                    \
  do {                                                                           \
    cudaError_t e_ = (x);                                                        \
    if (e_ != cudaSuccess) {                                                     \
      printf("CUDA %s at %s:%d\n", cudaGetErrorString(e_), __FILE__, __LINE__); \
      exit(1);                                                                   \
    }                                                                            \
  } while (0)

static uint32_t rng_st = 12345;
static uint32_t rnd() { rng_st = rng_st * 1664525u + 1013904223u; return rng_st >> 8; }
static short rs(int amp) { return (short)((int)(rnd() % (2u * amp)) - amp); }

static void make_vectors(int n, std::vector<short2> &v, int &nv)
{
  const int amps[5] = {100, 1000, 4000, 16000, 32768};
  nv = 14;
  v.assign((size_t)nv * n, make_short2(0, 0));
  for (int k = 0; k < nv; k++)
    for (int i = 0; i < n; i++) {
      short2 &e = v[(size_t)k * n + i];
      if (k < 10) e = make_short2(rs(amps[k % 5]), rs(amps[k % 5]));
      else if (k == 10) e = make_short2((short)(rnd() & 0xffff), (short)(rnd() & 0xffff));
      else if (k == 11) {
        const double a = 2 * M_PI * 5.0 * i / n;
        e = make_short2((short)lrint(32767 * cos(a)), (short)lrint(32767 * sin(a)));
      } else if (k == 12) e = make_short2(i == 0 ? 32767 : i == 1 ? -32768 : 0, i == 0 ? -32768 : i == 2 ? 32767 : 0);
      else e = make_short2(-32768, -32768);
    }
}

template <bool INV>
__global__ void k_x86_n(const short2 *in, short2 *out, const uint16_t *perm, const short2 *blob, int n)
{
  extern __shared__ short2 sm[];
  const short2 *x = in + (size_t)blockIdx.x * n;
  for (int i = threadIdx.x; i < n; i += THREADS) sm[i] = x[perm[i]];
  __syncthreads();
  nr_isa::fft_x86<INV, THREADS>(sm, sm + n, threadIdx.x, n, blob);
  for (int i = threadIdx.x; i < n; i += THREADS) out[(size_t)blockIdx.x * n + i] = sm[i];
}

/* GPU result of DFT twin `dft` on nv vectors of size n; false when the twin does not cover n */
static bool gpu_dft(int dft, int n, bool inv, const std::vector<short2> &in, int nv, std::vector<short2> &out)
{
  std::vector<uint16_t> perm;
  std::vector<short> ta, tb;
  if (dft == nr_isa::DFT_X86) nr_isa::x86_tables(n, perm, ta);
  else if (n == N) twin_tables(nr_isa::P_NEON_QRDMX, perm, ta, tb);
  else return false;
  uint16_t *dp;
  short *da, *db = nullptr;
  short2 *di, *dout;
  CU(cudaMalloc(&dp, perm.size() * 2));
  CU(cudaMalloc(&da, ta.size() * 2));
  CU(cudaMalloc(&di, in.size() * 4));
  CU(cudaMalloc(&dout, in.size() * 4));
  CU(cudaMemcpy(dp, perm.data(), perm.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(da, ta.data(), ta.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(di, in.data(), in.size() * 4, cudaMemcpyHostToDevice));
  if (dft == nr_isa::DFT_X86) {
    if (inv) k_x86_n<true><<<nv, THREADS, 2 * n * sizeof(short2)>>>(di, dout, dp, (const short2 *)da, n);
    else k_x86_n<false><<<nv, THREADS, 2 * n * sizeof(short2)>>>(di, dout, dp, (const short2 *)da, n);
  } else {
    CU(cudaMalloc(&db, tb.size() * 2));
    CU(cudaMemcpy(db, tb.data(), tb.size() * 2, cudaMemcpyHostToDevice));
    if (inv) k_twin_dft<nr_isa::P_NEON_QRDMX, true><<<nv, THREADS>>>(di, dout, dp, da, db);
    else k_twin_dft<nr_isa::P_NEON_QRDMX, false><<<nv, THREADS>>>(di, dout, dp, da, db);
  }
  CU(cudaGetLastError());
  out.resize(in.size());
  CU(cudaMemcpy(out.data(), dout, in.size() * 4, cudaMemcpyDeviceToHost));
  cudaFree(dp); cudaFree(da); cudaFree(db); cudaFree(di); cudaFree(dout);
  return true;
}

static void test_dft(int prof)
{
  const int sel = nr_isa::dft_of(prof), other = sel == nr_isa::DFT_X86 ? nr_isa::DFT_NEON : nr_isa::DFT_X86;
  const int sizes[7] = {64, 128, 256, 512, 1024, 2048, 4096};
  for (int si = 0; si < 7; si++)
    for (int inv = 0; inv < 2; inv++) {
      const int n = sizes[si];
      std::vector<short2> in, ref, g;
      int nv;
      make_vectors(n, in, nv);
      ref.resize(in.size());
      bool cpu = true;
      for (int k = 0; k < nv && cpu; k++) cpu = nr_gpu_isa_cpu_dft(n, inv, (const int16_t *)&in[(size_t)k * n], (int16_t *)&ref[(size_t)k * n]) == 0;
      if (!cpu) { printf("  %s%d: not a size of this CPU DFT library, skipped\n", inv ? "idft" : "dft", n); continue; }
      if (!gpu_dft(sel, n, inv, in, nv, g)) { printf("  %s%d: %s twin does not cover it (GPU paths use 4096)\n", inv ? "idft" : "dft", n, nr_isa::dft_name(sel)); continue; }
      int bad = 0, maxd = 0;
      for (size_t i = 0; i < in.size(); i++)
        if (g[i].x != ref[i].x || g[i].y != ref[i].y) { bad++; maxd = std::max(maxd, std::max(abs(g[i].x - ref[i].x), abs(g[i].y - ref[i].y))); }
      printf("  %s%-5d %s twin vs CPU: %d vectors, %d/%zu outputs differ (max %d LSB)\n", inv ? "idft" : "dft", n, nr_isa::dft_name(sel), nv, bad, in.size(), maxd);
      CHECK(bad == 0, "%s%d: selected twin not bit-exact", inv ? "idft" : "dft", n);
      if (n == N && gpu_dft(other, n, inv, in, nv, g)) {
        int ob = 0;
        for (size_t i = 0; i < in.size(); i++) ob += g[i].x != ref[i].x || g[i].y != ref[i].y;
        printf("  %s%-5d %s twin (not selected) vs CPU: %d/%zu outputs differ\n", inv ? "idft" : "dft", n, nr_isa::dft_name(other), ob, in.size());
        CHECK(ob > 0, "the other DFT twin is indistinguishable on this host");
      }
    }
}

template <int R>
__global__ void k_rot_n(const short2 *x, short2 *y, const short2 *ts, int n, short2 rot)
{
  const int k = blockIdx.x * blockDim.x + threadIdx.x;
  if (k < n) y[k] = rot_ts<R>(x[k], rot.x, nr_isa::xneg(rot.y), ts[k]);
}
static void test_rot(int prof)
{
  const int lens[6] = {12, 636, 1272, 1644, 3276, 4096};
  short2 rots[16] = {make_short2(23170, -23170), make_short2(-32768, -32768), make_short2(32767, 0), make_short2(0, 32767),
                     make_short2(-32768, 0), make_short2(1, -32768), make_short2(32767, 32767), make_short2(-1, -1)};
  for (int r = 8; r < 16; r++) rots[r] = make_short2((short)(rnd() & 0xffff), (short)(rnd() & 0xffff));
  std::vector<short2> x(N), ts(N), y(N), ref(N);
  for (int i = 0; i < N; i++) {
    x[i] = make_short2((short)(rnd() & 0xffff), (short)(rnd() & 0xffff));
    ts[i] = make_short2((short)(rnd() & 0xffff), (short)(rnd() & 0xffff));
  }
  x[1] = make_short2(-32768, -32768);
  ts[1] = make_short2(-32768, -32768);
  short2 *dx, *dy, *dt;
  CU(cudaMalloc(&dx, N * 4)); CU(cudaMalloc(&dy, N * 4)); CU(cudaMalloc(&dt, N * 4));
  CU(cudaMemcpy(dx, x.data(), N * 4, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(dt, ts.data(), N * 4, cudaMemcpyHostToDevice));
  const int twins[3] = {nr_isa::P_NEON_QRDMX, nr_isa::P_NEON_V80, nr_isa::P_X86};
  for (int tw : twins) {
    int bad = 0, total = 0;
    for (int r = 0; r < 16; r++)
      for (int l = 0; l < 6; l++) {
        nr_gpu_isa_cpu_rot((const int16_t *)x.data(), rots[r].x, rots[r].y, (const int16_t *)ts.data(), (int16_t *)ref.data(), lens[l]);
        if (tw == nr_isa::P_NEON_QRDMX) k_rot_n<nr_isa::P_NEON_QRDMX><<<16, 256>>>(dx, dy, dt, lens[l], rots[r]);
        else if (tw == nr_isa::P_NEON_V80) k_rot_n<nr_isa::P_NEON_V80><<<16, 256>>>(dx, dy, dt, lens[l], rots[r]);
        else k_rot_n<nr_isa::P_X86><<<16, 256>>>(dx, dy, dt, lens[l], rots[r]);
        CU(cudaMemcpy(y.data(), dy, lens[l] * 4, cudaMemcpyDeviceToHost));
        for (int i = 0; i < lens[l]; i++) {
          const bool d = y[i].x != ref[i].x || y[i].y != ref[i].y;
          if (d && bad < 3 && tw == prof) printf("    rot %d %d len %d i %d: x %d %d ts %d %d gpu %d %d cpu %d %d\n", rots[r].x, rots[r].y, lens[l], i, x[i].x, x[i].y, ts[i].x, ts[i].y, y[i].x, y[i].y, ref[i].x, ref[i].y);
          bad += d;
        }
        total += lens[l];
      }
    printf("  rotation twin %-5s vs CPU rotate_cpx_vector+mult_cpx_vector: %d/%d REs differ%s\n", nr_isa::rot_name(tw), bad, total, tw == prof ? " (selected)" : "");
    if (tw == prof) CHECK(bad == 0, "selected rotation twin not bit-exact");
    else CHECK(bad > 0, "rotation twin %s indistinguishable from the selected one", nr_isa::rot_name(tw));
  }
  cudaFree(dx); cudaFree(dy); cudaFree(dt);
}

static void test_fep(int prof)
{
  const int nant = 2, ncp = 288, ncp0 = 352, nb_rb = 273, fco = N - nb_rb * 6, so = ncp / 2;
  const size_t ring = (size_t)ncp0 + 14 * (size_t)(N + ncp) + 64;
  std::vector<uint16_t> perm;
  std::vector<short> ta, tb, ts(2 * N);
  twin_tables(prof, perm, ta, tb);
  for (int i = 0; i < N; i++) { /* init_timeshift_rotation */
    const double poff = -i * 2.0 * M_PI * so / N;
    ts[2 * i] = (int16_t)round(cos(poff) * 32767);
    ts[2 * i + 1] = (int16_t)round(sin(-poff) * 32767);
  }
  std::vector<short2> rx(nant * ring);
  for (size_t i = 0; i < rx.size(); i++) rx[i] = (i % 4096 < 64) ? make_short2((short)(rnd() & 0xffff), (short)(rnd() & 0xffff)) : make_short2(rs(3000), rs(3000));
  ExactIO io = {};
  for (int l = 0; l < 14; l++) { io.rot[l] = make_short2((short)(rnd() & 0xffff), (short)(rnd() & 0xffff)); io.sym_list[l] = (uint16_t)l; }
  uint16_t *dp; short *da, *db; short2 *dts, *drx, *dout;
  CU(cudaMalloc(&dp, perm.size() * 2)); CU(cudaMalloc(&da, ta.size() * 2)); CU(cudaMalloc(&db, tb.size() * 2));
  CU(cudaMalloc(&dts, N * 4)); CU(cudaMalloc(&drx, rx.size() * 4)); CU(cudaMalloc(&dout, (size_t)nant * 14 * N * 4));
  CU(cudaMemcpy(dp, perm.data(), perm.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(da, ta.data(), ta.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(db, tb.data(), tb.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(dts, ts.data(), N * 4, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(drx, rx.data(), rx.size() * 4, cudaMemcpyHostToDevice));
  for (int a = 0; a < nant; a++) io.rx[a] = drx + (size_t)a * ring;
  io.ring_len = 0xffffffffu;
  io.origin = 0;
  io.out = dout;
  io.ant_stride = (size_t)14 * N;
  NR_EXACT_LAUNCH(prof, k_fep_exact, dim3(14, nant), THREADS, 0, io, ncp, ncp0, dp, da, db, dts, nb_rb, fco);
  CU(cudaGetLastError());
  std::vector<short2> g((size_t)nant * 14 * N), ref(N);
  CU(cudaMemcpy(g.data(), dout, g.size() * 4, cudaMemcpyDeviceToHost));
  int bad = 0;
  const int len = (nb_rb + 1) * 6, start2 = fco - 6;
  for (int a = 0; a < nant; a++)
    for (int l = 0; l < 14; l++) {
      const short2 *w = &rx[(size_t)a * ring + ncp0 + (size_t)l * (N + ncp)];
      CHECK(nr_gpu_isa_cpu_dft(N, 0, (const int16_t *)w, (int16_t *)ref.data()) == 0, "CPU dft4096 unavailable");
      nr_gpu_isa_cpu_rot((const int16_t *)ref.data(), io.rot[l].x, io.rot[l].y, ts.data(), (int16_t *)ref.data(), len);
      nr_gpu_isa_cpu_rot((const int16_t *)&ref[start2], io.rot[l].x, io.rot[l].y, ts.data() + 2 * start2, (int16_t *)&ref[start2], len);
      const short2 *o = &g[((size_t)a * 14 + l) * N];
      for (int k = 0; k < N; k++) bad += o[k].x != ref[k].x || o[k].y != ref[k].y;
    }
  printf("  k_fep_exact<%s> vs CPU dft4096 + apply_nr_rotation_symbol_RX: %d/%d REs differ (2 RX x 14 symbols, 273 PRB)\n", g_isa_desc, bad, nant * 14 * N);
  CHECK(bad == 0, "k_fep_exact not bit-exact");
  cudaFree(dp); cudaFree(da); cudaFree(db); cudaFree(dts); cudaFree(drx); cudaFree(dout);
}

/* nr_passive_est_delay + nr_est_delay of nr_passive_delay.h / nr_phy_common.c, with this host's CPU idft4096 */
static void cpu_delay(const short2 *ls, int out[3])
{
  int peak = 0;
  for (int k = 0; k < N; k++) peak = std::max(peak, abs((int)ls[k].x) + abs((int)ls[k].y));
  int shift = 0;
  while ((peak >> shift) > 16383 / 64) ++shift;
  std::vector<short2> sc(N), t(N);
  for (int k = 0; k < N; k++) sc[k] = make_short2((short)(ls[k].x / (1 << shift)), (short)(ls[k].y / (1 << shift)));
  nr_gpu_isa_cpu_dft(N, 1, (const int16_t *)sc.data(), (int16_t *)t.data());
  int max_pos = 0, max_val = 0;
  uint64_t mean = 0;
  for (int i = 0; i < N; i++) {
    const int temp = (int)((unsigned)((int)t[i].x * t[i].x + (int)t[i].y * t[i].y) >> 1);
    mean += temp;
    if (temp > max_val) { max_pos = i; max_val = temp; }
  }
  mean /= N;
  if (max_pos > N / 2) max_pos -= N;
  const uint64_t restored = ((uint64_t)max_val << (2 * shift)) / (unsigned)N;
  out[0] = mean > 0 && (uint64_t)max_val / mean > 15 ? max_pos : 0;
  out[1] = max_pos;
  out[2] = restored > INT_MAX ? INT_MAX : (int)restored;
}
static void test_delay(int prof)
{
  const int nr = 12;
  std::vector<uint16_t> perm;
  std::vector<short> ta, tb;
  twin_tables(prof, perm, ta, tb);
  std::vector<short2> ls((size_t)nr * N, make_short2(0, 0));
  for (int r = 0; r < nr; r++) {
    const int m = 12 * (6 + 22 * r), d = (r * 37) % 200 - 100; /* support [0, m), a delay of d samples, rows 0..3 also noise */
    for (int k = 0; k < m && k < N; k++) {
      const double a = -2 * M_PI * (double)d * k / N, amp = r < 6 ? 3000.0 * (r + 1) : 32000.0;
      ls[(size_t)r * N + k] = make_short2((short)std::max(-32768.0, std::min(32767.0, amp * cos(a) + (r < 4 ? rs(2000) : 0))),
                                          (short)std::max(-32768.0, std::min(32767.0, amp * sin(a) + (r < 4 ? rs(2000) : 0))));
    }
  }
  uint16_t *dp; short *da, *db; short2 *dls; int *dout;
  CU(cudaMalloc(&dp, perm.size() * 2)); CU(cudaMalloc(&da, ta.size() * 2)); CU(cudaMalloc(&db, tb.size() * 2));
  CU(cudaMalloc(&dls, ls.size() * 4)); CU(cudaMalloc(&dout, 3 * nr * sizeof(int)));
  CU(cudaMemcpy(dp, perm.data(), perm.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(da, ta.data(), ta.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(db, tb.data(), tb.size() * 2, cudaMemcpyHostToDevice));
  CU(cudaMemcpy(dls, ls.data(), ls.size() * 4, cudaMemcpyHostToDevice));
  DelayIO io = {};
  io.base = dls;
  NR_EXACT_LAUNCH(prof, k_delay_exact, nr, THREADS, 0, io, dp, da, db, dout);
  CU(cudaGetLastError());
  int g[3 * nr];
  CU(cudaMemcpy(g, dout, sizeof(g), cudaMemcpyDeviceToHost));
  int bad = 0, nz = 0;
  for (int r = 0; r < nr; r++) {
    int ref[3];
    cpu_delay(&ls[(size_t)r * N], ref);
    nz += ref[0] != 0;
    if (memcmp(ref, &g[3 * r], sizeof(ref))) {
      bad++;
      printf("    row %d: GPU {%d %d %d} CPU {%d %d %d}\n", r, g[3 * r], g[3 * r + 1], g[3 * r + 2], ref[0], ref[1], ref[2]);
    }
  }
  printf("  k_delay_exact<%s> vs CPU nr_passive_est_delay: %d/%d rows differ (%d with a detected delay)\n", g_isa_desc, bad, nr, nz);
  CHECK(bad == 0, "k_delay_exact not bit-exact");
  cudaFree(dp); cudaFree(da); cudaFree(db); cudaFree(dls); cudaFree(dout);
}

int main()
{
  int ndev = 0;
  if (cudaGetDeviceCount(&ndev) != cudaSuccess || ndev < 1) {
    printf("no CUDA device: skipped\n");
    return 77;
  }
  const int prof = isa_profile();
  printf("selected: %s (cpu_dft=%s)\n", g_isa_desc, nr_gpu_isa_cpu_dft_origin());
  CHECK(prof != nr_isa::P_NONE, "no GPU twin is bit-identical to this host's CPU");
  if (prof == nr_isa::P_NONE) return 1;
  test_dft(prof);
  test_rot(prof);
  test_fep(prof);
  test_delay(prof);
  printf("%s: %d failure(s)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
