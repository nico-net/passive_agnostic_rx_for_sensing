/* Recorded-slot GPU benchmark. The CPU-only mode validates a replay dump without
 * a CUDA device; GPU mode reports one FEP + one batched LLR call per slot. */
#include "nr_pdsch_gpu_record.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

struct Sample {
  nr_pdsch_gpu_record_t h;
  std::vector<int16_t> rot, iq, cpu, rxF;
};
static double us(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{ return std::chrono::duration<double, std::micro>(b - a).count(); }
static double quantile(std::vector<double> v, double q)
{
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  return v[(size_t)std::ceil(q * v.size()) - 1];
}
static bool read_one(FILE *f, Sample &s)
{
  if (fread(&s.h, sizeof(s.h), 1, f) != 1) return false;
  const auto &h = s.h;
  if (h.magic != NR_PDSCH_GPU_RECORD_MAGIC || h.version != 2 || h.cfg.nant < 1 || h.cfg.nant > 4
      || h.cfg.symbols_per_slot != 14 || h.cfg.ofdm_symbol_size < 128 || h.cfg.ofdm_symbol_size > 8192
      || h.iq_samples_per_ant < 14u * h.cfg.ofdm_symbol_size || h.iq_samples_per_ant > 200000
      || h.llr_count == 0 || h.llr_count > (1u << 22)) return false;
  s.rot.resize(2 * h.cfg.symbols_per_slot);
  s.iq.resize((size_t)2 * h.cfg.nant * h.iq_samples_per_ant);
  s.cpu.resize(h.llr_count);
  s.rxF.resize((size_t)2 * h.cfg.nant * h.cfg.ofdm_symbol_size);
  const size_t expected = sizeof(h) + (s.rot.size() + s.iq.size() + s.cpu.size() + s.rxF.size()) * sizeof(int16_t);
  if (h.bytes != expected) return false;
  return fread(s.rot.data(), sizeof(int16_t), s.rot.size(), f) == s.rot.size()
      && fread(s.iq.data(), sizeof(int16_t), s.iq.size(), f) == s.iq.size()
      && fread(s.cpu.data(), sizeof(int16_t), s.cpu.size(), f) == s.cpu.size()
      && fread(s.rxF.data(), sizeof(int16_t), s.rxF.size(), f) == s.rxF.size();
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

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: nr_gpu_slot_bench dump.bin [--cpu-only|--gpu] [repetitions]\n"); return 2; }
  if (argc > 2 && strcmp(argv[2], "--gpu") && strcmp(argv[2], "--cpu-only")) return 2;
  const bool gpu = argc > 2 && !strcmp(argv[2], "--gpu");
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
  if (!gpu) return 0;
  void *lib = dlopen("./libpdsch_gpu.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib) { fprintf(stderr, "dlopen libpdsch_gpu.so: %s\n", dlerror()); return 2; }
  auto get = (const nr_gpu_fep_api_t *(*)())dlsym(lib, "nr_gpu_fep_api");
  auto last_chunks = (int (*)())dlsym(lib, "nr_gpu_fep_last_chunks");
  const nr_gpu_fep_api_t *api = get ? get() : nullptr;
  if (!api || api->init(&samples[0].h.cfg)) { fprintf(stderr, "GPU unavailable or init failed\n"); return 2; }
  std::vector<double> slot_us, fep_us, llr_us;
  size_t compared = 0, sign_ok = 0, short_llr = 0, split_slots = 0;
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
    const auto t1 = std::chrono::steady_clock::now();
    const int64_t n = rc ? -1 : api->pdsch_llr(jobs.data(), jobs.size(), llr.data(), llr.size());
    const auto t2 = std::chrono::steady_clock::now();
    if (n < 0) { fprintf(stderr, "GPU failure at slot %u\n", slot); return 1; }
    if (rep == 0 && last_chunks && last_chunks() != 1) ++split_slots;
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
            const size_t ci = ((size_t)a * N + sc) * 2, gi = ((size_t)sym * N + sc) * 2;
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
      size_t job_ok = 0;
      for (uint32_t x = 0; x < j.n_llr; ++x) {
        const bool same = (v[x] < 0) == (ref.cpu[x] < 0);
        sign_ok += same;
        job_ok += same;
        xy += (double)v[x] * ref.cpu[x];
        x2 += (double)v[x] * v[x];
        y2 += (double)ref.cpu[x] * ref.cpu[x];
        ++compared;
      }
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
  const double fep_corr = fep_ref_pow > 0 && fep_gpu_pow > 0
      ? std::hypot(fep_cross_re, fep_cross_im) / std::sqrt(fep_ref_pow * fep_gpu_pow) : 0.;
  printf("CPU/GPU FEP first-DMRS: points=%zu corr=%.8f gpu/cpu rms=%.6f fit_error_db=%.2f\n",
         fep_points, fep_corr, fep_ref_pow > 0 ? std::sqrt(fep_gpu_pow / fep_ref_pow) : NAN,
         fep_corr > 0 && fep_corr < 1 ? 10 * std::log10(1 - fep_corr * fep_corr) : NAN);
  return short_llr || !compared || (double)sign_ok / compared < .995 || corr < .995 ? 1 : 0;
}
