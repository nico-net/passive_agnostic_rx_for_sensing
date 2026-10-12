#include "/home/nicola/NICOLA/wt/rr-csd-gal/openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_demod.cu"
#include <vector>
using namespace nrdm;
struct Rec { int16_t G[18], b[6], out[6], qm, pad; };
#define NL 3
#define NC (2 * NL + 1)

__device__ double cabs_v(dcx x, int mode)
{
  const double lim = 67108864.0;
  if (mode == 0 || (x.r == trunc(x.r) && x.i == trunc(x.i) && fabs(x.r) < lim && fabs(x.i) < lim)) return cabs_c(x);
  if (mode == 4) { /* glibc 2.35+ __ieee754_hypot kernel, FMA variant */
    double ax = fabs(x.r), ay = fabs(x.i);
    if (ay > ax) { const double t = ax; ax = ay; ay = t; }
    const double t1 = __dadd_rn(ay, ay), t2 = __dadd_rn(ax, -ay);
    return t1 >= ax ? __dsqrt_rn(__fma_rn(t1, ax, __dmul_rn(t2, t2))) : __dsqrt_rn(__fma_rn(ax, ax, __dmul_rn(ay, ay)));
  }
  if (mode == 1) return __dsqrt_rn(__fma_rn(x.r, x.r, __dmul_rn(x.i, x.i)));
  if (mode == 2) return __dsqrt_rn(__dadd_rn(__dmul_rn(x.r, x.r), __dmul_rn(x.i, x.i)));
  const double hx = __dmul_rn(x.r, x.r), lx = __fma_rn(x.r, x.r, -hx), hy = __dmul_rn(x.i, x.i), ly = __fma_rn(x.i, x.i, -hy);
  const double sm = __dadd_rn(hx, hy), e1 = __dadd_rn(fabs(hx) >= fabs(hy) ? __dadd_rn(hx, -sm) : __dadd_rn(hy, -sm), fabs(hx) >= fabs(hy) ? hy : hx);
  const double er = __dadd_rn(__dadd_rn(e1, lx), ly), h0 = __dsqrt_rn(sm);
  return __dadd_rn(h0, __ddiv_rn(__dadd_rn(__fma_rn(-h0, h0, sm), er), __dmul_rn(2.0, h0)));
}
__global__ void kt(const Rec *rec, int n, int16_t *o, int variant)
{
  const int t = blockIdx.x * blockDim.x + threadIdx.x;
  if (t >= n) return;
  const Rec R = rec[t];
  dcx M[NL][NC];
  for (int r = 0; r < NL; r++) {
    for (int c = 0; c < NL; c++) {
      M[r][c] = dcx{(double)R.G[(r * 3 + c) * 2], (double)R.G[(r * 3 + c) * 2 + 1]};
      M[r][NL + 1 + c] = dcx{r == c ? 1.0 : 0.0, 0.0};
    }
    M[r][NL] = dcx{(double)R.b[r * 2], (double)R.b[r * 2 + 1]};
  }
  bool singular = false;
  for (int k = 0; k < NL && !singular; k++) {
    int piv = k;
    for (int r = k + 1; r < NL; r++)
      if (cabs_v(M[r][k], variant >> 6) > cabs_v(M[piv][k], variant >> 6)) piv = r;
    if (cabs_v(M[piv][k], variant >> 6) < 1e-9) { singular = true; break; }
    if (piv != k)
      for (int c = 0; c < NC; c++) { const dcx tt = M[k][c]; M[k][c] = M[piv][c]; M[piv][c] = tt; }
    const dcx inv = divdc3(1.0, 0.0, M[k][k].r, M[k][k].i);
    for (int c = 0; c < NC; c++) M[k][c] = (variant & 2) ? cmul(inv, M[k][c]) : cmul(M[k][c], inv);
    for (int r = 0; r < NL; r++) {
      if (r == k) continue;
      const dcx f = M[r][k];
      if (f.r == 0.0 && f.i == 0.0) continue;
      for (int c = 0; c < NC; c++) {
        const double fr = f.r, fi = f.i, mr = M[k][c].r, mi = M[k][c].i, Mr = M[r][c].r, Mi = M[r][c].i;
        const int form = (variant >> 2) & 15;
        if (form == 0) {
          const dcx tt = (variant & 1) ? cmul(f, M[k][c]) : cmul(M[k][c], f);
          M[r][c] = dcx{__dsub_rn(Mr, tt.r), __dsub_rn(Mi, tt.i)};
        } else if (form == 1) {
          M[r][c] = dcx{__fma_rn(fi, mi, __fma_rn(-fr, mr, Mr)), __fma_rn(-fi, mr, __fma_rn(-fr, mi, Mi))};
        } else if (form == 2) {
          M[r][c] = dcx{__fma_rn(-fr, mr, __fma_rn(fi, mi, Mr)), __fma_rn(-fr, mi, __fma_rn(-fi, mr, Mi))};
        } else if (form == 3) {
          const double tr = __fma_rn(-fi, mi, __dmul_rn(fr, mr)), ti = __fma_rn(fr, mi, __dmul_rn(fi, mr));
          M[r][c] = dcx{__dsub_rn(Mr, tr), __dsub_rn(Mi, ti)};
        } else if (form == 4) {
          const double tr = __fma_rn(fr, mr, -__dmul_rn(fi, mi)), ti = __fma_rn(fi, mr, __dmul_rn(fr, mi));
          M[r][c] = dcx{__dsub_rn(Mr, tr), __dsub_rn(Mi, ti)};
        } else if (form == 5) {
          const double tr = __fma_rn(-fi, mi, __dmul_rn(fr, mr)), ti = __fma_rn(fi, mr, __dmul_rn(fr, mi));
          M[r][c] = dcx{__dsub_rn(Mr, tr), __dsub_rn(Mi, ti)};
        }
      }
    }
  }
  for (int r = 0; r < NL; r++) {
    const double dinv = singular ? 0.0 : M[r][NL + 1 + r].r;
    const double w = dinv > 1e-12 ? __ddiv_rn(1.0, dinv) : 0.0;
    const double orr = w > 0.0 ? __dmul_rn(M[r][NL].r, w) : 0.0, oi = w > 0.0 ? __dmul_rn(M[r][NL].i, w) : 0.0;
    o[t * 6 + r * 2] = qf_s16(orr);
    o[t * 6 + r * 2 + 1] = qf_s16(oi);
  }
}
int main(int argc, char **argv)
{
  FILE *f = fopen(argv[1], "rb");
  std::vector<Rec> v;
  Rec r;
  while (fread(&r, sizeof r, 1, f) == 1) v.push_back(r);
  const int n = (int)v.size();
  printf("records %d\n", n);
  Rec *d; int16_t *o;
  cudaMalloc(&d, n * sizeof(Rec)); cudaMalloc(&o, n * 12);
  cudaMemcpy(d, v.data(), n * sizeof(Rec), cudaMemcpyHostToDevice);
  const int variant = argc > 2 ? atoi(argv[2]) : 0;
  kt<<<(n + 255) / 256, 256>>>(d, n, o, variant);
  std::vector<int16_t> ho(n * 6);
  cudaMemcpy(ho.data(), o, n * 12, cudaMemcpyDeviceToHost);
  long bad = 0;
  for (int i = 0; i < n; i++)
    for (int k = 0; k < 6; k++)
      if (ho[i * 6 + k] != v[i].out[k]) {
        if (bad < 8) {
          printf("MISMATCH rec %d comp[%d] gpu %d cpu %d qm %d\n G:", i, k, ho[i * 6 + k], v[i].out[k], v[i].qm);
          for (int g = 0; g < 18; g++) printf(" %d", v[i].G[g]);
          printf("\n b:"); for (int g = 0; g < 6; g++) printf(" %d", v[i].b[g]); printf("\n");
        }
        bad++;
      }
  printf("mismatches %ld of %d records\n", bad, n);
}
