__global__ void sweep_ref(nr_csirs_disc_sweep_map_t map, const short2 *rx, const int *slot,
                      int ants, int spf, nr_csirs_disc_sweep_cfg_t cfg, float *scores, int cand0 = 0, int nctot = 0)
{
  __shared__ uint32_t gold[14][words_max];
  __shared__ float sums[4];
  int candidate = cand0 + blockIdx.x; /* CSIRS_DISC_CHUNK: a chunk launch covers candidates [cand0, cand0+gridDim.x) of nctot */
  int id = cfg.id_first + candidate / cfg.delta_max;
  int delta = candidate % cfg.delta_max;
  int l = threadIdx.x;
  if (l < 14 && (map.sym_mask & (1u << l))) {
    uint64_t t = 14 * ((slot[blockIdx.y] + delta) % spf) + l + 1;
    uint32_t init = ((t * (2 * id + 1) << 10) + id) & 0x7fffffff;
    Gold g;
    g.reset(init);
    for (int w = 0; w < map.words; ++w)
      gold[l][w] = g.next();
  }
  __syncthreads();
  int lane = threadIdx.x % 32, warp = threadIdx.x / 32;
  float total = 0;
  for (int task = warp; task < map.ports * ants; task += 4) {
    int p = task / ants, ant = task % ants;
    int begin = map.begin[p], n = map.begin[p+1] - begin;
    // This form also supports block_re greater than n without integer overflow.
    int nb = n ? 1 + (n - 1) / cfg.block_re : 0;
    float rho = 0, active = 0;
    const short2 *samples = rx + ((size_t)blockIdx.y * ants + ant) * map.n_re;
    for (int b = lane; b < nb; b += 32) {
      int first = b * cfg.block_re, count = min(cfg.block_re, n - first);
      float ar = 0, ai = 0, energy = 0;
      for (int k = 0; k < count; ++k) {
        int i = begin + first + k;
        nr_csirs_disc_re_t re = map.re[i];
        unsigned bits = (gold[re.symbol][re.m / 16] >> (2 * (re.m % 16))) & 3;
        // OAI QPSK at amplitude 16384 is exactly (+/-8192,+/-8192).
        // Unit amplitudes cancel from the normalized correlation.
        float xr = (bits & 1 ? -1.f : 1.f) * re.sign;
        float xi = (bits & 2 ? -1.f : 1.f) * re.sign;
        short2 y = samples[i];
        float yr = y.x, yi = y.y;
        ar += yr*xr + yi*xi;
        ai += yi*xr - yr*xi;
        energy += yr*yr + yi*yi;
      }
      if (energy > 0) {
        rho += fminf(1.f, sqrtf((ar*ar + ai*ai) / (energy * (2 * count))));
        active += 1;
      }
    }
    rho = warp_sum(rho);
    active = warp_sum(active);
    if (lane == 0 && active > 0)
      total += rho / active * sqrtf(n / active) / .886f;
  }
  if (lane == 0)
    sums[warp] = total;
  __syncthreads();
  if (threadIdx.x == 0)
    scores[(size_t)blockIdx.y * (nctot ? nctot : gridDim.x) + candidate] = (sums[0]+sums[1]+sums[2]+sums[3]) / (map.ports*ants);
}
