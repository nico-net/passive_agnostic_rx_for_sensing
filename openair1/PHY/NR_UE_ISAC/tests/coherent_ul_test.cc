// openair1/PHY/NR_UE_ISAC/tests/coherent_ul_test.cc
#include "coherent_core.h"
#include "coherent_ul.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

// rows x 3276 grid, antennas 4; paths: (per-channel absolute delay, amplitude, doppler); channel phase offsets
struct Path { std::array<double, kCh> tau; double amp; double doppler_hz; };
static CfrWindow make_window(uint32_t rows, const std::vector<Path>& paths, std::array<double, kCh> ph,
                             double cfo_hz, uint32_t nprb_row, uint32_t seed, uint32_t comb = 1)
{
  CfrWindow w; w.antennas = 4; w.rows = rows; w.subcarriers = 273 * 12; w.scs_hz = 30000; w.fc_hz = 3.45e9; w.pci = 2;
  w.values.assign((size_t)4 * rows * w.subcarriers, {0, 0}); w.observed.assign((size_t)rows * w.subcarriers, 0);
  std::mt19937 rng(seed), hop(seed + 1000); std::normal_distribution<double> n01(0, 1);
  const double slot = slot_duration_s(w.scs_hz);
  for (uint32_t r = 0; r < rows; ++r) {
    w.row_time_slots.push_back(r * 2.0);          // every 2nd slot
    w.row_slot_idx.push_back(r * 2); w.row_slot_frac.push_back(0); w.row_source_mask.push_back(1u << 3);
    const double t = r * 2.0 * slot;
    // Pseudo-random hopping. A regular stride (was r*37 mod 210 PRB) makes the band centre a
    // sawtooth in time, which is a genuine range-Doppler ambiguity of the waveform: a path seen
    // ~1.6 bins off its delay is serrodyne-shifted by +176 Hz at ~0.94 of its peak, beating the
    // 0.4-bin-off-grid true Doppler (0.90 scalloping) on 2 of 4 channels. Physics, not the code.
    const uint32_t start = hop() % (273 - nprb_row + 1);
    for (uint32_t k = start * 12; k < (start + nprb_row) * 12; k += comb) {   // comb 2 = DM-RS-only row
      w.observed[w.cell(r, k)] = 1;
      const double f = ((double)k - 273 * 6) * w.scs_hz;
      for (uint32_t a = 0; a < 4; ++a) {
        std::complex<double> acc = 0;
        for (const Path& p : paths)
          acc += p.amp * std::exp(std::complex<double>(0, -2 * M_PI * ((w.fc_hz + f) * p.tau[a] - p.doppler_hz * t)));
        acc *= std::exp(std::complex<double>(0, ph[a] + 2 * M_PI * cfo_hz * t));
        acc += 0.01 * std::complex<double>(n01(rng), n01(rng));
        w.values[w.sample(a, r, k)] = std::complex<float>(acc);
      }
    }
  }
  return w;
}

int main() {
  Geometry g; g.tx = {35, 20, 6}; g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol; const Vec3 ue{-6, 9, 1.4};
  std::array<double, kCh> d{}; for (uint32_t i = 0; i < 4; ++i) d[i] = (dist(ue, g.rx[i]) + 17.0) / kC;
  CfrWindow w = make_window(40, {{d, 1.0, 0}}, {0, 1.1, -2.0, 0.4}, 0, 128, 7);
  coherent::Axes a = derive_axes(w, vol, g, 20.0);
  UeFix f = localise_ue(w, a, g, vol, 1e-4);
  require(f.valid && dist(f.pos, ue) < 2 * kC / (2 * a.b_eff_hz) && f.sigma_m > 0, "UE localised");
  Geometry ug = ue_geometry(g, f); require(dist(ug.tx, f.pos) == 0 && dist(ug.rx[2], g.rx[2]) == 0, "UE geometry");
  std::puts("coherent_ul_test: PASS");
  return 0;
}
