/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* Raw dump of one coherent CPI (CfrWindow + the config fields that derive its Axes), for offline
 * GPU/CPU parity tests on real recordings. Written by coherent_pipeline.cc when NR_ISAC_COH_DUMP=<dir>
 * (first NR_ISAC_COH_DUMP_N CPIs, default 4); read by tests/coherent_cuda_parity_test.cc. */
#pragma once
#include <cstdio>
#include <string>
#include "coherent_types.h"
#include "pipeline_types.h"

namespace nr_isac::coherent {

struct CpiDump {
  CfrWindow w;
  Volume vol;
  Geometry geo;
  double max_speed_mps = 0;
};

namespace dump_detail {
template <class T> bool put(FILE* f, const std::vector<T>& v)
{ const uint64_t n = v.size(); return fwrite(&n, 8, 1, f) == 1 && (n == 0 || fwrite(v.data(), sizeof(T), n, f) == n); }
template <class T> bool get(FILE* f, std::vector<T>& v)
{ uint64_t n = 0; if (fread(&n, 8, 1, f) != 1 || n > (1ull << 32)) return false; v.resize(n); return n == 0 || fread(v.data(), sizeof(T), n, f) == n; }
template <class T> bool pod(FILE* f, T& x, bool wr) { return wr ? fwrite(&x, sizeof(T), 1, f) == 1 : fread(&x, sizeof(T), 1, f) == 1; }
inline bool io(FILE* f, CpiDump& d, bool wr)
{
  CfrWindow& w = d.w;
  bool ok = pod(f, w.antennas, wr) && pod(f, w.rows, wr) && pod(f, w.subcarriers, wr) && pod(f, w.scs_hz, wr) && pod(f, w.fc_hz, wr)
            && pod(f, w.pci, wr) && pod(f, d.vol, wr) && pod(f, d.max_speed_mps, wr);
  for (Vec3& v : d.geo.rx) ok = ok && pod(f, v.x, wr) && pod(f, v.y, wr) && pod(f, v.z, wr);
  ok = ok && pod(f, d.geo.tx.x, wr) && pod(f, d.geo.tx.y, wr) && pod(f, d.geo.tx.z, wr);
  if (wr) return ok && put(f, w.values) && put(f, w.observed) && put(f, w.row_time_slots) && put(f, w.row_slot_idx) && put(f, w.row_slot_frac) && put(f, w.row_source_mask);
  return ok && get(f, w.values) && get(f, w.observed) && get(f, w.row_time_slots) && get(f, w.row_slot_idx) && get(f, w.row_slot_frac) && get(f, w.row_source_mask);
}
} // namespace dump_detail

inline bool write_cpi_dump(const std::string& path, CpiDump d)
{
  FILE* f = fopen(path.c_str(), "wb"); if (!f) return false;
  const bool ok = dump_detail::io(f, d, true); return fclose(f) == 0 && ok;
}
inline bool read_cpi_dump(const std::string& path, CpiDump& d)
{
  FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
  const bool ok = dump_detail::io(f, d, false); fclose(f); return ok && d.w.valid();
}

} // namespace nr_isac::coherent
