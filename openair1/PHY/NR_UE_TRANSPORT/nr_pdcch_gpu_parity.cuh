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

/* GPU6: the same arithmetic, one 256-thread block per (job, CORESET symbol). Every reduction is an exact
 * integer sum (the CPU's double accumulations of integer terms are exact below 2^53, its uint32 lane sums
 * wrap identically in any order), so the result does not depend on thread order. */
static const int PDCCH_PAR_THREADS = 256, PDCCH_PAR_MAX_RB = 276;
__global__ static void __launch_bounds__(PDCCH_PAR_THREADS)
k_pdcch_cpu_arithmetic_par(const nr_gpu_pdcch_job_t *jobs, const uint32_t *gold, const float2 *rx, int N, int nant, int fco,
                           int16_t *out, int force_mask)
{
  const int j = blockIdx.x, s = blockIdx.y, t = threadIdx.x;
  const auto job = jobs[j];
  if (s >= job.duration || job.n_rb > PDCCH_PAR_MAX_RB) return;
  __shared__ short2 h[4][PDCCH_PAR_MAX_RB];
  __shared__ unsigned long long dp[4], pw[4];
  __shared__ unsigned lanes[4][4], hist[256];
  __shared__ int peak, mask, count, comp_shift, sh;
  const int len = 9 * job.n_rb, shift = __ffs(len) - 1;
  const uint32_t *g = gold + ((size_t)j * MAX_DUR + s) * GOLD_WORDS;
  for (int i = t; i < 4 * 4; i += PDCCH_PAR_THREADS) lanes[i / 4][i % 4] = 0;
  for (int i = t; i < 256; i += PDCCH_PAR_THREADS) hist[i] = 0;
  if (t < 4) { dp[t] = 0; pw[t] = 0; }
  if (t == 0) peak = 0;
  for (int x = t; x < nant * job.n_rb; x += PDCCH_PAR_THREADS) {
    const int a = x / job.n_rb, rb = x % job.n_rb;
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
    h[a][rb] = make_short2((short)((r / 3 >> 2) * 4), (short)((q / 3 >> 2) * 4));
  }
  __syncthreads();
  for (int x = t; x < nant * len; x += PDCCH_PAR_THREADS) {
    const int a = x / len, i = x % len;
    const short2 v = h[a][i / 9];
    const unsigned power = (unsigned)((int)v.x * v.x + (int)v.y * v.y);
    if (i < (len & ~3)) atomicAdd(&lanes[a][i % 4], (unsigned)((int)power >> shift));
    if (i) {
      const short2 prev = h[a][(i - 1) / 9];
      const long long dr = (int)v.x - prev.x, di = (int)v.y - prev.y;
      atomicAdd(&dp[a], (unsigned long long)(dr * dr + di * di));
      atomicAdd(&pw[a], (unsigned long long)power);
    }
  }
  __syncthreads();
  if (t == 0) {
    double rough[4], best = 1e30;
    unsigned avg[4];
    for (int a = 0; a < nant; ++a) {
      avg[a] = ((uint64_t)lanes[a][0] + lanes[a][1] + lanes[a][2] + lanes[a][3]) / (len >> shift);
      rough[a] = pw[a] > 0 ? (double)dp[a] / (double)pw[a] : 1e30;
      best = fmin(best, rough[a]);
    }
    int mk = 0, c = 0;
    unsigned level = 0;
    for (int a = 0; a < nant; ++a)
      if (force_mask ? (force_mask & (1 << a)) : rough[a] <= 4 * best) {
        mk |= 1 << a; ++c; level = max(level, avg[a]);
      }
    int l2 = level ? __clz(level) : 0;
    l2 = level ? (level > (3037000499U >> l2) ? 32 - l2 : 31 - l2) : 0;
    mask = mk; count = c; comp_shift = l2 / 2 + 5;
  }
  __syncthreads();
  int16_t *o = out + job.llr_offset + 2 * s * len;
  const int data[9] = {0, 2, 3, 4, 6, 7, 8, 10, 11};
  for (int i = t; i < len; i += PDCCH_PAR_THREADS) {
    int r = 0, q = 0;
    const int k = (fco + 12 * (job.bwp_start_rb + job.rb_offset + i / 9) + data[i % 9]) % N;
    for (int a = 0; a < nant; ++a)
      if (mask & (1 << a)) {
        const float2 y = rx[((size_t)a * MAX_DUR + s) * N + k];
        const short2 v = h[a][i / 9];
        r += gpu_sat16(((int)y.x * v.x + (int)y.y * v.y) >> comp_shift);
        q += gpu_sat16(((int)y.y * v.x - (int)y.x * v.y) >> comp_shift);
      }
    const int16_t orr = count ? r / count : 0, oq = count ? q / count : 0;
    o[2 * i] = orr; o[2 * i + 1] = oq;
    const int m = max(abs((int)orr), abs((int)oq));
    atomicAdd(&hist[min(255, m)], 1u);
    atomicMax(&peak, m);
  }
  __syncthreads();
  if (t == 0) {
    int p90 = 0, acc = 0, sv = 0, pk = peak;
    for (; p90 < 256; ++p90) { acc += hist[p90]; if (acc * 10 >= len * 9) break; }
    if (p90 >= 255) while (pk > 31 && sv < 15) { pk >>= 1; ++sv; }
    else if (p90 > 24) while ((p90 >> sv) > 24 && sv < 8) ++sv;
    else if (p90 > 0) while ((p90 << (-sv + 1)) <= 24 && sv > -6) --sv;
    sh = sv;
  }
  __syncthreads();
  for (int i = t; i < 2 * len; i += PDCCH_PAR_THREADS) {
    const int v = sh > 0 ? o[i] >> sh : o[i] * (1 << -sh);
    o[i] = max(-32, min(31, v));
  }
}
