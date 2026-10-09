/* Fixed-point PDCCH path matching dci_nr.c. Parity first: one CUDA thread per
 * CORESET symbol; parallel reductions can follow once the recorded gate passes. */
__device__ static int gpu_sat16(int x) { return max(-32768, min(32767, x)); }
__global__ static void k_pdcch_cpu_arithmetic(const nr_gpu_pdcch_job_t *jobs, const uint32_t *gold,
                                            const float2 *rx, int N, int nant, int fco,
                                            int16_t *out, int force_mask)
{
  const int j = blockIdx.x, s = blockIdx.y;
  const auto job = jobs[j];
  if (s >= job.duration || threadIdx.x) return;
  short2 h[4][275];
  double rough[4], best = 1e30;
  unsigned avg[4];
  const int len = 9 * job.n_rb, shift = __ffs(len) - 1;
  const uint32_t *g = gold + ((size_t)j * MAX_DUR + s) * GOLD_WORDS;
  for (int a = 0; a < nant; ++a) {
    for (int rb = 0; rb < job.n_rb; ++rb) {
      int r = 0, q = 0;
      for (int p = 0; p < 3; ++p) {
        int m = 3 * (job.dmrs_ref_rb + job.rb_offset + rb) + p;
        int pr = ((g[2*m/32] >> (2*m%32)) & 1) ? -23170 : 23170;
        int pi = ((g[(2*m+1)/32] >> ((2*m+1)%32)) & 1) ? 23170 : -23170;
        int k = (fco + 12 * (job.bwp_start_rb + job.rb_offset + rb) + 1 + 4*p) % N;
        float2 y = rx[((size_t)a * MAX_DUR + s) * N + k];
        r += (short)(((int)y.x * pr - (int)y.y * pi) >> 15);
        q += (short)(((int)y.x * pi + (int)y.y * pr) >> 15);
      }
      /* filt16a_1 and mulhi_s1_int16: truncate BEFORE multiplying by four. */
      h[a][rb] = make_short2((short)((r / 3 >> 2) * 4), (short)((q / 3 >> 2) * 4));
    }
    double dp = 0, pw = 0;
    uint32_t lanes[4] = {};
    for (int i = 0; i < len; ++i) {
      short2 v = h[a][i/9];
      unsigned power = (unsigned)((int)v.x*v.x + (int)v.y*v.y);
      if (i < (len & ~3)) lanes[i%4] += (int)power >> shift;
      if (i) {
        short2 prev = h[a][(i-1)/9];
        double dr = (int)v.x-prev.x, di = (int)v.y-prev.y;
        dp += dr*dr + di*di;
        pw += power;
      }
    }
    avg[a] = ((uint64_t)lanes[0]+lanes[1]+lanes[2]+lanes[3]) / (len >> shift);
    rough[a] = pw > 0 ? dp/pw : 1e30;
    best = fmin(best, rough[a]);
  }
  int mask = 0, count = 0;
  unsigned level = 0;
  for (int a = 0; a < nant; ++a)
    if (force_mask ? (force_mask & (1<<a)) : rough[a] <= 4*best) {
      mask |= 1<<a; ++count; level = max(level, avg[a]);
    }
  int l2 = level ? __clz(level) : 0;
  l2 = level ? (level > (3037000499U >> l2) ? 32-l2 : 31-l2) : 0;
  int comp_shift = l2/2 + 5;
  int16_t *o = out + job.llr_offset + 2*s*len;
  unsigned hist[256] = {};
  int peak = 0;
  const int data[9] = {0,2,3,4,6,7,8,10,11};
  for (int i = 0; i < len; ++i) {
    int r = 0, q = 0;
    int k = (fco + 12*(job.bwp_start_rb + job.rb_offset + i/9) + data[i%9]) % N;
    for (int a = 0; a < nant; ++a) if (mask & (1<<a)) {
      float2 y = rx[((size_t)a * MAX_DUR + s)*N+k];
      short2 v = h[a][i/9];
      r += gpu_sat16(((int)y.x*v.x+(int)y.y*v.y) >> comp_shift);
      q += gpu_sat16(((int)y.y*v.x-(int)y.x*v.y) >> comp_shift);
    }
    o[2*i] = count ? r/count : 0; o[2*i+1] = count ? q/count : 0;
    int m = max(abs((int)o[2*i]), abs((int)o[2*i+1]));
    ++hist[min(255,m)]; peak = max(peak,m);
  }
  int p90 = 0, acc = 0, sh = 0;
  for (; p90 < 256; ++p90) { acc += hist[p90]; if (acc*10 >= len*9) break; }
  if (p90 >= 255) while (peak > 31 && sh < 15) { peak >>= 1; ++sh; }
  else if (p90 > 24) while ((p90 >> sh) > 24 && sh < 8) ++sh;
  else if (p90 > 0) while ((p90 << (-sh+1)) <= 24 && sh > -6) --sh;
  for (int i = 0; i < 2*len; ++i) {
    int v = sh > 0 ? o[i] >> sh : o[i] * (1 << -sh);
    o[i] = max(-32,min(31,v));
  }
}
