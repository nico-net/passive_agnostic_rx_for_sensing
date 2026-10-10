/* Recorded-slot GPU benchmark. The CPU-only mode validates a replay dump without
 * a CUDA device; GPU mode reports one FEP + one batched LLR call per slot. */
#include "nr_pdsch_gpu_record.h"
#include "nr_pdsch_gpu_stage.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#include <string>

struct Stage { nr_pdsch_gpu_stage_row_t h; std::vector<int16_t> ls, fir; };
struct Sample {
  nr_pdsch_gpu_record_t h;
  std::vector<int16_t> rot, iq, cpu, rxF;
  std::vector<Stage> stages;
  nr_pdsch_gpu_stage_meta_t meta = {};
};
static double us(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{ return std::chrono::duration<double, std::micro>(b - a).count(); }
static double quantile(std::vector<double> v, double q)
{
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[(size_t)std::ceil(q * v.size()) - 1];
}
/* read_one: reads one recorded PDSCH grant (header, IQ, CPU LLRs, rxF and v3 stage rows) from a dump, with field and size validation.
 *   How it works: Returns false on bad magic, version, geometry, sizes or a short read. Version 3 adds the stage rows, each validated before its arrays are read.
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_slot_bench.cc:101, openair1/PHY/NR_UE_TRANSPORT/tests/nr_gpu_slot_bench.cc:133.
 */
static bool read_one(FILE *f, Sample &s)
{
  if (fread(&s.h, sizeof(s.h), 1, f) != 1) return false;
  const auto &h = s.h;
  if (h.magic != NR_PDSCH_GPU_RECORD_MAGIC || (h.version != 2 && h.version != 3) || h.cfg.nant < 1 || h.cfg.nant > 4
      || h.cfg.symbols_per_slot != 14 || h.cfg.ofdm_symbol_size < 128 || h.cfg.ofdm_symbol_size > 8192
      || h.iq_samples_per_ant < 14u * h.cfg.ofdm_symbol_size || h.iq_samples_per_ant > 200000
      || h.llr_count == 0 || h.llr_count > (1u << 22)) return false;
  s.rot.resize(2 * h.cfg.symbols_per_slot);
  s.iq.resize((size_t)2 * h.cfg.nant * h.iq_samples_per_ant);
  s.cpu.resize(h.llr_count);
  s.rxF.resize((size_t)2 * h.cfg.nant * h.cfg.ofdm_symbol_size * (h.version == 3 ? 14 : 1));
  const size_t expected = sizeof(h) + (s.rot.size() + s.iq.size() + s.cpu.size() + s.rxF.size()) * sizeof(int16_t);
  if ((h.version == 2 && h.bytes != expected) || h.bytes < expected) return false;
  bool ok = fread(s.rot.data(), sizeof(int16_t), s.rot.size(), f) == s.rot.size()
      && fread(s.iq.data(), sizeof(int16_t), s.iq.size(), f) == s.iq.size()
      && fread(s.cpu.data(), sizeof(int16_t), s.cpu.size(), f) == s.cpu.size()
      && fread(s.rxF.data(), sizeof(int16_t), s.rxF.size(), f) == s.rxF.size();
  size_t bytes = expected;
  if (ok && h.version == 3) {
    uint32_t rows;
    if (fread(&rows, sizeof(rows), 1, f) != 1 || rows > 224) return false;
    if (fread(&s.meta, sizeof(s.meta), 1, f) != 1) return false;
    bytes += sizeof(rows) + sizeof(s.meta);
    for (uint32_t i = 0; i < rows; ++i) {
      Stage r;
      if (fread(&r.h, sizeof(r.h), 1, f) != 1 || r.h.count > 3300 || !r.h.count
          || r.h.symbol >= 14 || r.h.layer >= h.job.Nl || r.h.antenna >= (unsigned)h.cfg.nant) return false;
      r.ls.resize(r.h.count*2); r.fir.resize(r.h.count*2);
      if (fread(r.ls.data(), 4, r.h.count, f) != r.h.count
          || fread(r.fir.data(), 4, r.h.count, f) != r.h.count) return false;
      bytes += sizeof(r.h) + r.h.count*8;
      s.stages.push_back(std::move(r));
    }
  }
  return ok && bytes == h.bytes;
}

/* 38.211 Gold scrambling bits. CPU LLRs in the dump have already been descrambled. */
static void descramble(int16_t *v, size_t n, uint16_t rnti, uint16_t nid)
{
  uint32_t x1 = 1, x2 = ((uint32_t)rnti << 15) + nid;
  for (int i = 0; i < 1600; ++i) {
    x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1u) << 30);
    x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1u) << 30);
  }
  for (size_t i = 0; i < n; ++i) {
    if ((x1 ^ x2) & 1u) v[i] = -v[i];
    x1 = (x1 >> 1) | (((x1 ^ (x1 >> 3)) & 1u) << 30);
    x2 = (x2 >> 1) | (((x2 ^ (x2 >> 1) ^ (x2 >> 2) ^ (x2 >> 3)) & 1u) << 30);
  }
}

/* main: PDSCH GPU slot benchmark: replays a dump grouped by slot and reports the recorded CPU reference counts.
 *   How it works: Reads all records, sorts by absolute slot and reports slot groups, max jobs per slot and jobs over the GPU batch limit. Then runs the mode: --cpu-only, --gpu, --exact, --delay or --pair (compares two dumps, GPU6).
 *   ISAC_* flags read: ISAC_GPU_BENCH_CPU_FEP.
 *   Callers: none (program entry point).
 */
int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: nr_gpu_slot_bench dump.bin [--cpu-only|--gpu] [repetitions]\n"); return 2; }
  if (argc > 2 && strcmp(argv[2], "--gpu") && strcmp(argv[2], "--cpu-only") && strcmp(argv[2], "--exact")
      && strcmp(argv[2], "--delay") && strcmp(argv[2], "--pair")) return 2;
  const bool exact = argc > 2 && !strcmp(argv[2], "--exact");
  const bool delay = argc > 2 && !strcmp(argv[2], "--delay");
  const bool gpu = argc > 2 && (!strcmp(argv[2], "--gpu") || exact || delay);
  const int reps = argc > 3 ? std::max(1, atoi(argv[3])) : 1;
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 2; }
  std::vector<Sample> samples;
  bool malformed = false;
  for (;;) {
    const int lead = fgetc(f);
    if (lead == EOF) break;
    ungetc(lead, f);
    Sample s;
    if (!read_one(f, s)) { malformed = true; break; }
    samples.push_back(std::move(s));
  }
  fclose(f);
  if (malformed || samples.empty()) { fprintf(stderr, "empty or malformed dump\n"); return 2; }
  std::stable_sort(samples.begin(), samples.end(), [](const Sample &a, const Sample &b) {
    return a.h.absolute_slot < b.h.absolute_slot;
  });
  size_t groups = 0, max_jobs = 0, over_limit = 0;
  for (size_t i = 0; i < samples.size();) {
    const size_t begin = i;
    const uint32_t slot = samples[i].h.absolute_slot;
    ++groups;
    do { ++i; } while (i < samples.size() && samples[i].h.absolute_slot == slot);
    max_jobs = std::max(max_jobs, i - begin);
    over_limit += i - begin > 65;
  }
  printf("recorded CPU reference: jobs=%zu slot_groups=%zu max_jobs_per_slot=%zu over_gpu_batch_limit=%zu crc_ok=%zu evm_valid=%zu\n",
         samples.size(), groups, max_jobs, over_limit,
         std::count_if(samples.begin(), samples.end(), [](const Sample &s) { return s.h.crc_ok; }),
         std::count_if(samples.begin(), samples.end(), [](const Sample &s) { return s.h.evm_valid; }));
  if (argc > 3 && !strcmp(argv[2], "--pair")) {
    /* GPU6: the same grant recorded by two replays (e.g. CPU FEP vs exact GPU FEP): is the input of the
     * CPU chain identical, and where does its output start to differ? */
    FILE *g = fopen(argv[3], "rb");
    if (!g) { perror(argv[3]); return 2; }
    std::vector<Sample> other;
    for (;;) {
      const int lead = fgetc(g);
      if (lead == EOF) break;
      ungetc(lead, g);
      Sample o;
      if (!read_one(g, o)) break;
      other.push_back(std::move(o));
    }
    fclose(g);
    size_t iq_same = 0, iq_diff_shown = 0;
    size_t matched = 0, rxf_same = 0, rows_same_count = 0, ls_same = 0, fir_same = 0, delay_same = 0, llr_same = 0, crc_same = 0;
    for (const Sample &a : samples) {
      const Sample *b = nullptr;
      for (const Sample &o : other) {
        nr_gpu_pdsch_job_t ja = a.h.job, jb = o.h.job;
        ja.llr_offset = jb.llr_offset = 0;
        ja.n_llr = jb.n_llr = 0;
        if (o.h.absolute_slot == a.h.absolute_slot && o.h.rnti == a.h.rnti && !memcmp(&ja, &jb, sizeof(ja))
            && o.h.version == 3 && a.h.version == 3) { b = &o; break; }
      }
      if (!b) continue;
      ++matched;
      const int N = a.h.cfg.ofdm_symbol_size;
      uint16_t mask = a.h.job.dmrs_mask;
      for (int l = a.h.job.start_symbol; l < a.h.job.start_symbol + a.h.job.nb_symbols; ++l) mask |= 1u << l;
      bool same = true;
      for (int ant = 0; ant < a.h.cfg.nant && same; ++ant)
        for (int l = 0; l < 14 && same; ++l)
          if (mask >> l & 1)
            same = !memcmp(&a.rxF[2 * ((size_t)ant * 14 * N + (size_t)l * N)], &b->rxF[2 * ((size_t)ant * 14 * N + (size_t)l * N)],
                           (size_t)N * 4);
      rxf_same += same;
      iq_same += a.iq == b->iq && a.h.abs_sample == b->h.abs_sample && a.h.fo_hz == b->h.fo_hz;
      if (a.iq != b->iq && iq_diff_shown++ < 3) {
        size_t first = 0;
        while (first < a.iq.size() && first < b->iq.size() && a.iq[first] == b->iq[first]) ++first;
        printf("IQ_DIFF slot=%u abs_sample=%u/%u fo=%.1f/%.1f first_diff_int16=%zu of %zu a=%d b=%d\n", a.h.absolute_slot,
               a.h.abs_sample, b->h.abs_sample, a.h.fo_hz, b->h.fo_hz, first, a.iq.size(),
               first < a.iq.size() ? a.iq[first] : 0, first < b->iq.size() ? b->iq[first] : 0);
      }
      bool cnt = a.stages.size() == b->stages.size(), ls = cnt, fir = cnt, dly = cnt;
      for (size_t r = 0; cnt && r < a.stages.size(); ++r) {
        const auto &x = a.stages[r], &y = b->stages[r];
        if (x.h.count != y.h.count || x.h.symbol != y.h.symbol || x.h.antenna != y.h.antenna) { cnt = ls = fir = dly = false; break; }
        ls = ls && x.ls == y.ls;
        fir = fir && x.fir == y.fir;
        dly = dly && x.h.delay == y.h.delay && x.h.noise == y.h.noise;
      }
      rows_same_count += cnt; ls_same += ls; fir_same += fir; delay_same += dly;
      const bool llr = a.cpu == b->cpu;
      llr_same += llr;
      crc_same += a.h.crc_ok == b->h.crc_ok;
      if (!(same && cnt && ls && fir && dly && llr))
        printf("PAIR_DIFF slot=%u rnti=0x%04x rb=%u+%u rxF=%d rows=%zu/%zu count_eq=%d ls=%d fir=%d delay_noise=%d llr=%d crc=%u/%u evm=%.1f/%.1f%s\n",
               a.h.absolute_slot, a.h.rnti, a.h.job.start_rb, a.h.job.nb_rb, same, a.stages.size(), b->stages.size(), cnt, ls, fir, dly,
               llr, a.h.crc_ok, b->h.crc_ok, a.h.cpu_evm_pct, b->h.cpu_evm_pct,
               cnt || a.stages.empty() || b->stages.empty() ? "" : (" ls_count " + std::to_string(a.stages[0].h.count) + "/" + std::to_string(b->stages[0].h.count)).c_str());
    }
    printf("PAIR: iq_identical=%zu\n", iq_same);
    printf("PAIR: A=%zu B=%zu matched=%zu rxF_identical=%zu stage_rows_same_shape=%zu ls_identical=%zu fir_identical=%zu "
           "delay_noise_identical=%zu cpu_llr_identical=%zu crc_same=%zu\n",
           samples.size(), other.size(), matched, rxf_same, rows_same_count, ls_same, fir_same, delay_same, llr_same, crc_same);
    return 0;
  }
  if (!gpu) return 0;
  void *lib = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib) { fprintf(stderr, "dlopen libpdsch_gpu.so: %s\n", dlerror()); return 2; }
  auto get = (const nr_gpu_fep_api_t *(*)())dlsym(lib, "nr_gpu_fep_api");
  auto debug_ls = (int (*)(int,int,int,int,float *,float *))dlsym(lib,"nr_pdsch_gpu_debug_ls");
  auto cpu_fep = (int (*)(const int16_t *))dlsym(lib,"nr_pdsch_gpu_debug_cpu_fep");
  const bool inject_cpu_fep = getenv("ISAC_GPU_BENCH_CPU_FEP") && atoi(getenv("ISAC_GPU_BENCH_CPU_FEP"));
  auto last_chunks = (int (*)())dlsym(lib, "nr_gpu_fep_last_chunks");
  const nr_gpu_fep_api_t *api = get ? get() : nullptr;
  if (!api || api->init(&samples[0].h.cfg)) { fprintf(stderr, "GPU unavailable or init failed\n"); return 2; }
  if (delay) {
    /* GPU6 gate: GPU nr_passive_est_delay() equals the CPU delay recorded in every v3 stage row. */
    auto xdel = (int (*)(const int16_t *const *, int, int *))dlsym(lib, "nr_pdsch_gpu_exact_delay");
    if (!xdel) { fprintf(stderr, "no exact delay symbol\n"); return 2; }
    const int N = samples[0].h.cfg.ofdm_symbol_size;
    size_t rows = 0, same = 0;
    std::vector<double> t_us;
    for (int rep = 0; rep < reps; ++rep) for (const Sample &s : samples) {
      for (size_t r0 = 0; r0 < s.stages.size(); r0 += 16) {
        const int n = (int)std::min<size_t>(16, s.stages.size() - r0);
        std::vector<std::vector<int16_t>> buf(n, std::vector<int16_t>((size_t)2 * N, 0));
        const int16_t *ptr[16];
        for (int r = 0; r < n; ++r) {
          std::copy(s.stages[r0 + r].ls.begin(), s.stages[r0 + r].ls.end(), buf[r].begin());
          ptr[r] = buf[r].data();
        }
        int out[48];
        const auto t0 = std::chrono::steady_clock::now();
        if (xdel(ptr, n, out)) { fprintf(stderr, "exact delay refused\n"); return 1; }
        t_us.push_back(us(t0, std::chrono::steady_clock::now()));
        if (rep) continue;
        for (int r = 0; r < n; ++r) {
          ++rows;
          const auto &st = s.stages[r0 + r].h;
          if (out[3 * r] == st.delay) ++same;
          else if (rows - same <= 12)
            printf("DELAY_DIFF slot=%u sym=%u ant=%u cpu=%d gpu=%d pos=%d val=%d\n", s.h.absolute_slot, st.symbol, st.antenna,
                   st.delay, out[3 * r], out[3 * r + 1], out[3 * r + 2]);
        }
      }
    }
    printf("EXACT DELAY: rows=%zu equal=%zu; batch us p50=%.1f p99=%.1f\n", rows, same, quantile(t_us, .5), quantile(t_us, .99));
    return rows == same && rows ? 0 : 1;
  }
  if (exact) {
    /* GPU6 gate: the GPU FEP must reproduce the CPU rxdataF bit for bit on every recorded symbol. */
    auto xfep = (int (*)(const int16_t *const *, uint32_t, uint32_t, double, const int16_t *, uint16_t, int16_t *, size_t))
        dlsym(lib, "nr_pdsch_gpu_exact_fep");
    auto xus = (double (*)(void))dlsym(lib, "nr_pdsch_gpu_exact_last_us");
    if (!xfep || !xus) { fprintf(stderr, "no exact FEP symbol\n"); return 2; }
    size_t points = 0, diff = 0, max_abs = 0, shown = 0, symbols = 0;
    std::vector<double> t_us;
    for (int rep = 0; rep < reps; ++rep) for (const Sample &s : samples) {
      const int N = s.h.cfg.ofdm_symbol_size, na = s.h.cfg.nant;
      const int16_t *rx[4] = {};
      for (int a = 0; a < na; ++a) rx[a] = s.iq.data() + (size_t)2 * a * s.h.iq_samples_per_ant;
      uint16_t mask = 0;
      if (s.h.version == 3) {
        for (int l = s.h.job.start_symbol; l < s.h.job.start_symbol + s.h.job.nb_symbols; ++l) mask |= 1u << l;
        mask |= s.h.job.dmrs_mask;
      } else {
        mask = 1u << s.h.rxF_symbol;
      }
      std::vector<int16_t> out((size_t)2 * na * 14 * N);
      if (xfep(rx, s.h.iq_samples_per_ant, s.h.early, s.h.fo_hz, s.rot.data(), mask, out.data(), (size_t)14 * N)) {
        fprintf(stderr, "exact FEP refused slot %u (fo=%.1f)\n", s.h.absolute_slot, s.h.fo_hz); return 1;
      }
      t_us.push_back(xus());
      if (rep) continue;
      for (int a = 0; a < na; ++a) for (int l = 0; l < 14; ++l) {
        if (!(mask >> l & 1)) continue;
        ++symbols;
        const int16_t *g = out.data() + 2 * ((size_t)a * 14 * N + (size_t)l * N);
        const int16_t *c = s.h.version == 3 ? s.rxF.data() + 2 * ((size_t)a * 14 * N + (size_t)l * N)
                                            : s.rxF.data() + 2 * (size_t)a * N;
        for (int k = 0; k < N; ++k) {
          ++points;
          const int dr = std::abs(g[2 * k] - c[2 * k]), di = std::abs(g[2 * k + 1] - c[2 * k + 1]);
          if (dr || di) {
            ++diff;
            max_abs = std::max<size_t>(max_abs, std::max(dr, di));
            if (shown++ < 12)
              printf("EXACT_DIFF slot=%u ant=%d sym=%d k=%d gpu=(%d,%d) cpu=(%d,%d)\n", s.h.absolute_slot, a, l, k, g[2 * k],
                     g[2 * k + 1], c[2 * k], c[2 * k + 1]);
          }
        }
      }
    }
    printf("EXACT FEP: records=%zu symbols=%zu points=%zu mismatching=%zu max_abs_diff=%zu; exact_fep_us p50=%.1f p99=%.1f "
           "(nant x masked symbols incl. upload/readback)\n",
           samples.size(), symbols, points, diff, max_abs, quantile(t_us, .5), quantile(t_us, .99));
    return diff || !points ? 1 : 0;
  }
  std::vector<double> slot_us, fep_us, llr_us;
  size_t compared = 0, sign_ok = 0, short_llr = 0, split_slots = 0;
  uint64_t raw_gpu_hash = 1469598103934665603ull;
  size_t both_nonzero = 0, both_nonzero_ok = 0, cpu_zero = 0, gpu_zero = 0;
  size_t cpu_zero_gpu_negative = 0, gpu_zero_cpu_negative = 0;
  const int deadzone[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
  size_t deadzone_ok[sizeof(deadzone) / sizeof(deadzone[0])] = {};
  double xy = 0, x2 = 0, y2 = 0;
  double fep_ref_pow = 0, fep_gpu_pow = 0, fep_cross_re = 0, fep_cross_im = 0;
  size_t fep_points = 0;
  for (int rep = 0; rep < reps; ++rep) for (size_t i = 0; i < samples.size();) {
    const size_t begin = i;
    const uint32_t slot = samples[i].h.absolute_slot;
    do { ++i; } while (i < samples.size() && samples[i].h.absolute_slot == slot);
    const Sample &s = samples[begin];
    for (size_t k = begin + 1; k < i; ++k) {
      const auto &other = samples[k];
      if (memcmp(&other.h.cfg, &s.h.cfg, sizeof(s.h.cfg)) || other.h.fo_hz != s.h.fo_hz
          || other.h.abs_sample != s.h.abs_sample || other.h.iq_samples_per_ant != s.h.iq_samples_per_ant
          || other.rot != s.rot || other.iq != s.iq) {
        fprintf(stderr, "slot %u has unequal IQ/FEP inputs across grants; cannot batch safely\n", slot);
        return 2;
      }
    }
    const int16_t *rx[4] = {};
    for (int a = 0; a < s.h.cfg.nant; ++a)
      rx[a] = s.iq.data() + (size_t)2 * a * s.h.iq_samples_per_ant;
    std::vector<nr_gpu_pdsch_job_t> jobs;
    size_t cap = 0;
    for (size_t k = begin; k < i; ++k) { jobs.push_back(samples[k].h.job); cap += samples[k].h.llr_count; }
    std::vector<int16_t> llr(cap + 1024);
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = api->fep_slot(rx, s.h.iq_samples_per_ant, s.h.early, s.h.abs_sample,
                                 s.h.fo_hz, s.rot.data());
    if (inject_cpu_fep && (s.h.version != 3 || !cpu_fep || cpu_fep(s.rxF.data()))) {
      fprintf(stderr,"CPU FEP isolation needs v3 full-symbol records\n"); return 2;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const int64_t n = rc ? -1 : api->pdsch_llr(jobs.data(), jobs.size(), llr.data(), llr.size());
    const auto t2 = std::chrono::steady_clock::now();
    if (n < 0) { fprintf(stderr, "GPU failure at slot %u\n", slot); return 1; }
    if (rep == 0 && last_chunks && last_chunks() != 1) ++split_slots;
    if (rep == 0) for (size_t b = 0; b < (size_t)n; ++b) {
      raw_gpu_hash ^= (uint16_t)llr[b];
      raw_gpu_hash *= 1099511628211ull;
    }
    slot_us.push_back(us(t0, t2)); fep_us.push_back(us(t0, t1)); llr_us.push_back(us(t1, t2));
    if (rep == 0) {
      const int N = s.h.cfg.ofdm_symbol_size;
      std::vector<int16_t> gpu_rxF((size_t)2 * 14 * N);
      for (int a = 0; a < s.h.cfg.nant; ++a) {
        if (api->read_rxdataF(a, gpu_rxF.data())) { fprintf(stderr, "GPU FEP read failed\n"); return 1; }
        for (size_t k = begin; k < i; ++k) {
          const auto &ref = samples[k];
          const int sym = ref.h.rxF_symbol;
          for (int cr = 0; cr < 12 * ref.h.job.nb_rb; ++cr) {
            const int sc = (s.h.cfg.first_carrier_offset + 12 * ref.h.job.start_rb + cr) % N;
            const size_t ci = ((size_t)a * N * (ref.h.version == 3 ? 14 : 1) + (ref.h.version == 3 ? sym*N : 0) + sc) * 2, gi = ((size_t)sym * N + sc) * 2;
            const double ar = ref.rxF[ci], ai = ref.rxF[ci + 1], br = gpu_rxF[gi], bi = gpu_rxF[gi + 1];
            fep_ref_pow += ar * ar + ai * ai;
            fep_gpu_pow += br * br + bi * bi;
            fep_cross_re += ar * br + ai * bi;
            fep_cross_im += ai * br - ar * bi;
            ++fep_points;
          }
        }
      }
    }
    if (rep == 0) for (size_t k = begin; k < i; ++k) {
      const auto &j = jobs[k - begin];
      const auto &ref = samples[k];
      if (!j.n_llr || j.n_llr != ref.h.llr_count) { ++short_llr; continue; }
      int16_t *v = llr.data() + j.llr_offset;
      descramble(v, j.n_llr, ref.h.rnti, ref.h.data_scrambling_id);
      if (debug_ls) for (const auto &st : ref.stages) {
        int di = __builtin_popcount((unsigned)j.dmrs_mask & ((1u<<st.h.symbol)-1));
        float ls[1650], noise;
        if (debug_ls(k-begin, di, st.h.layer, st.h.antenna, ls, &noise)) return 2;
        double e_ls=0, e_fir=0, p_ls=0, p_fir=0;
        const int type=j.dmrs_type, per=type==1?4:6, delta=type==1?(j.ports[st.h.layer]>>1)&1:2*((j.ports[st.h.layer]>>1)%3);
        const int npil=j.nb_rb*(type==1?3:2);
        for (unsigned x=0;x<st.h.count;++x) {
          float t=std::max(0.f,std::min((float)(npil-1),(x-delta-(type==1?1.f:.5f))/per));
          int n=(int)t; float w=t-n;
          for (int ax=0;ax<2;++ax) {
            double a=st.ls[2*x+ax], b=ls[2*(x/per)+ax];
            e_ls+=(a-b)*(a-b); p_ls+=a*a;
            double c=st.fir[2*x+ax], d=ls[2*n+ax];
            if(n+1<npil) d+=w*(ls[2*(n+1)+ax]-d);
            e_fir+=(c-d)*(c-d); p_fir+=c*c;
          }
        }
        printf("STAGE slot=%u rnti=%04x sym=%u layer=%u ant=%u delay_cpu=%d noise_cpu=%u noise_gpu=%.3f ls_db=%.2f fir_db=%.2f cpu_fep=%d\n",
               ref.h.absolute_slot, ref.h.rnti, st.h.symbol, st.h.layer, st.h.antenna, st.h.delay, st.h.noise, noise,
               10*log10(e_ls/std::max(1.,p_ls)),10*log10(e_fir/std::max(1.,p_fir)), inject_cpu_fep);
      }
      size_t job_ok = 0;
      for (uint32_t x = 0; x < j.n_llr; ++x) {
        const bool same = (v[x] < 0) == (ref.cpu[x] < 0);
        for (size_t d = 0; d < sizeof(deadzone) / sizeof(deadzone[0]); ++d)
          deadzone_ok[d] += (v[x] < -deadzone[d]) == (ref.cpu[x] < 0);
        sign_ok += same;
        job_ok += same;
        if (ref.cpu[x] == 0) {
          ++cpu_zero;
          cpu_zero_gpu_negative += v[x] < 0;
        }
        if (v[x] == 0) {
          ++gpu_zero;
          gpu_zero_cpu_negative += ref.cpu[x] < 0;
        }
        if (ref.cpu[x] != 0 && v[x] != 0) {
          ++both_nonzero;
          both_nonzero_ok += same;
        }
        xy += (double)v[x] * ref.cpu[x];
        x2 += (double)v[x] * v[x];
        y2 += (double)ref.cpu[x] * ref.cpu[x];
        ++compared;
      }
      if (ref.h.version == 3)
        printf("PROVENANCE slot=%u rnti=%04x recovered=%u mrc=%u mmse=%u decoder=%u csi=%u ptrs=%u sign=%zu/%u\n",
               ref.h.absolute_slot, ref.h.rnti, ref.meta.recovered, ref.meta.mrc_mode, ref.meta.mmse_mode,
               ref.meta.decoder, ref.meta.csi_count, ref.meta.ptrs, job_ok, j.n_llr);
      if (job_ok * 1000 < (size_t)j.n_llr * 999)
        printf("PDSCH grant slot=%u rnti=0x%04x rb=%u+%u sym=%u+%u dmrs=0x%04x cdm=%u Nl=%u Qm=%u sign=%zu/%u (%.4f) cpu_crc=%u evm=%.1f%%\n",
               ref.h.absolute_slot, ref.h.rnti, j.start_rb, j.nb_rb, j.start_symbol, j.nb_symbols,
               j.dmrs_mask, j.n_cdm_groups_no_data, j.Nl, j.Qm, job_ok, j.n_llr,
               (double)job_ok / j.n_llr, ref.h.crc_ok, ref.h.cpu_evm_pct);
    }
  }
  printf("GPU slot wall us: p50=%.1f p99=%.1f; FEP p50=%.1f p99=%.1f; chest+LLR p50=%.1f p99=%.1f\n",
         quantile(slot_us, .5), quantile(slot_us, .99), quantile(fep_us, .5), quantile(fep_us, .99),
         quantile(llr_us, .5), quantile(llr_us, .99));
  const double corr = x2 > 0 && y2 > 0 ? xy / std::sqrt(x2 * y2) : 0.;
  printf("CPU/GPU LLR sign agreement=%zu/%zu (%.6f), corr=%.6f, length_mismatch=%zu, split_slots=%zu; GPU TB CRC and post-EQ SNR: receiver replay required\n",
         sign_ok, compared, compared ? (double)sign_ok / compared : 0., corr, short_llr, split_slots);
  printf("Raw GPU LLR FNV64=%016llx\n", (unsigned long long)raw_gpu_hash);
  printf("LLR zero audit: both_nonzero_sign=%zu/%zu (%.6f) cpu_zero=%zu cpu_zero_gpu_negative=%zu gpu_zero=%zu gpu_zero_cpu_negative=%zu\n",
         both_nonzero_ok, both_nonzero, both_nonzero ? (double)both_nonzero_ok / both_nonzero : 0.,
         cpu_zero, cpu_zero_gpu_negative, gpu_zero, gpu_zero_cpu_negative);
  for (size_t d = 0; d < sizeof(deadzone) / sizeof(deadzone[0]); ++d)
    printf("LLR negative deadzone %d: sign=%zu/%zu (%.6f)\n", deadzone[d], deadzone_ok[d], compared,
           compared ? (double)deadzone_ok[d] / compared : 0.);
  const double fep_corr = fep_ref_pow > 0 && fep_gpu_pow > 0
      ? std::hypot(fep_cross_re, fep_cross_im) / std::sqrt(fep_ref_pow * fep_gpu_pow) : 0.;
  printf("CPU/GPU FEP first-DMRS: points=%zu corr=%.8f gpu/cpu rms=%.6f fit_error_db=%.2f\n",
         fep_points, fep_corr, fep_ref_pow > 0 ? std::sqrt(fep_gpu_pow / fep_ref_pow) : NAN,
         fep_corr > 0 && fep_corr < 1 ? 10 * std::log10(1 - fep_corr * fep_corr) : NAN);
  return short_llr || !compared || (double)sign_ok / compared < .999 || corr < .995 ? 1 : 0;
}
