/* SPDX-License-Identifier: OAI-Public-License-1.1 */
/* Offline replay of a recorded cfr_rows.bin through the SAME engine and conf as the live receiver.
 * isac_replay (NR_ISAC_FIXED_WORK_REPLAY: blocking admission) runs in lockstep: after each record
 * it waits until the engine is idle, so CPI windows close exactly as in a live engine that keeps up,
 * independently of host speed (the tracker's own time budget still depends on it). isac_replay_rt (production engine) with --realtime
 * instead paces records at their recorded timestamps and never waits: a slow engine shows up as
 * lost rows, deadline-shed detector work and missing CPIs. */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
extern "C" {
#include "common/config/config_load_configmodule.h"
#include "common/utils/LOG/log.h"
#include "nr_isac.h"
configmodule_interface_t *uniqCfg = NULL;
void exit_function(const char *file, const char *function, const int line, const char *s, const int a)
{
  std::fprintf(stderr, "exit_function %s:%d %s: %s\n", file, line, function, s ? s : "");
  std::exit(a ? 1 : 0);
}
}

int main(int argc, char **argv)
{
  std::string rows;
  bool realtime = false;
  double rate = 1.0; // --rate X: pace at X times the recorded row rate (stress)
  int loops = 1; // --loops N: replay the file N times back to back, slot/time shifted (sustained run)
  std::vector<char *> args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--rows") && i + 1 < argc)
      rows = argv[++i];
    else if (!std::strcmp(argv[i], "--realtime"))
      realtime = true;
    else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc)
      rate = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc)
      loops = std::atoi(argv[++i]);
    else
      args.push_back(argv[i]);
  }
  if (rows.empty() || !(rate > 0.0) || loops < 1) {
    std::fprintf(stderr, "usage: isac_replay -O <ue.conf> --rows <cfr_rows.bin> [--realtime [--rate X]] [--loops N]\n");
    return 2;
  }
  if (!(uniqCfg = load_configmodule((int)args.size(), args.data(), 0))) {
    std::fprintf(stderr, "config load failed\n");
    return 2;
  }
  logInit();
  nr_isac_init();
  nr_isac_start();
  if (!nr_isac_enabled()) {
    std::fprintf(stderr, "sensing not enabled by the conf\n");
    return 3;
  }
  FILE *f = std::fopen(rows.c_str(), "rb");
  if (!f) {
    std::perror(rows.c_str());
    return 2;
  }
  const auto started = std::chrono::steady_clock::now();
  std::vector<float> h;
  std::vector<uint32_t> k, l;
  uint64_t n_rows = 0, n_close = 0, first_t = 0, last_t = 0;
  int rc = 0;
  // Loop i shifts every record by i*(span + 100 slots) in both the slot clock and mono time, so the
  // engine sees one continuous stream (the SFN wraps as it would live).
  int loop = 0;
  uint64_t span_ns = 0, slot_ns = 0;
  int64_t span_slots = 0, prev_slot = -1;
  for (;;) {
    char magic[4];
    uint32_t kind, slot, prb, scs, ant, re;
    float frac, noise;
    int32_t source;
    uint64_t fc, session, t;
    uint16_t pci, spf;
    if (std::fread(magic, 1, 4, f) != 4) {
      if (++loop >= loops || !slot_ns) break;
      if (loop == 1) span_ns = last_t - first_t;
      std::rewind(f);
      continue;
    }
    if (std::memcmp(magic, "CFR1", 4)) {
      std::fprintf(stderr, "bad record magic after %llu rows\n", (unsigned long long)n_rows);
      rc = 4;
      break;
    }
    bool ok = std::fread(&kind, 4, 1, f) && std::fread(&slot, 4, 1, f) && std::fread(&frac, 4, 1, f)
              && std::fread(&source, 4, 1, f) && std::fread(&prb, 4, 1, f) && std::fread(&scs, 4, 1, f)
              && std::fread(&fc, 8, 1, f) && std::fread(&pci, 2, 1, f) && std::fread(&spf, 2, 1, f)
              && std::fread(&ant, 4, 1, f) && std::fread(&re, 4, 1, f) && std::fread(&noise, 4, 1, f)
              && std::fread(&session, 8, 1, f) && std::fread(&t, 8, 1, f);
    if (ok && kind == 0) {
      h.resize((size_t)2 * ant * re);
      k.resize(re);
      l.resize(re);
      ok = std::fread(h.data(), sizeof(float), h.size(), f) == h.size() && std::fread(k.data(), 4, re, f) == re
           && std::fread(l.data(), 4, re, f) == re;
    }
    if (!ok) {
      std::fprintf(stderr, "truncated record after %llu rows\n", (unsigned long long)n_rows);
      rc = 4;
      break;
    }
    if (kind == 0 && spf) {
      const int64_t cycle = (int64_t)spf * 1024;
      if (!slot_ns) slot_ns = 10000000ULL / spf;
      if (loop == 0) {
        if (prev_slot >= 0) span_slots += (((int64_t)slot - prev_slot) % cycle + cycle) % cycle;
        prev_slot = slot;
      }
      slot = (uint32_t)(((int64_t)slot + loop * (span_slots + 100)) % cycle);
    }
    t += loop * (span_ns + 100 * slot_ns);
    if (!n_rows && !n_close) first_t = t;
    last_t = t;
    if (realtime)
      std::this_thread::sleep_until(started + std::chrono::nanoseconds((uint64_t)((t - first_t) / rate)));
    if (kind == 1) {
      nr_isac_request_discard();
      ++n_close;
      continue;
    }
    // ISAC_REPLAY_DMRS_DESPREAD=1: emulate the frequency-OCC despreading that nr_pdsch_passive_queue.c's
    // dmrs_ls_cfr_submit now applies at capture time, for recordings made before it (every blind DM-RS row
    // of such a rank>=2 recording carries the port pair interleaved; average each consecutive RE pair).
    static const bool despread = std::getenv("ISAC_REPLAY_DMRS_DESPREAD") != nullptr;
    if (despread && source == NR_ISAC_SRC_PDSCH_DMRS_BLIND && re >= 2) {
      const uint32_t m = re / 2;
      std::vector<float> h2((size_t)2 * ant * m); std::vector<uint32_t> k2(m), l2(m);
      for (uint32_t a = 0; a < ant; ++a) for (uint32_t q = 0; q < m; ++q) for (int c2 = 0; c2 < 2; ++c2)
        h2[2 * ((size_t)a * m + q) + c2] = 0.5f * (h[2 * ((size_t)a * re + 2 * q) + c2] + h[2 * ((size_t)a * re + 2 * q + 1) + c2]);
      for (uint32_t q = 0; q < m; ++q) { k2[q] = (k[2 * q] + k[2 * q + 1]) / 2; l2[q] = l[2 * q]; }
      nr_isac_carrier_t c{prb, scs, fc, pci, spf};
      nr_isac_submit_cfr_multi_session(slot, frac, source, &c, h2.data(), ant, m, k2.data(), l2.data(), m, noise, session);
      ++n_rows;
      while (!realtime && !nr_isac_drained()) std::this_thread::sleep_for(std::chrono::microseconds(50));
      continue;
    }
    nr_isac_carrier_t c{prb, scs, fc, pci, spf};
    nr_isac_submit_cfr_multi_session(slot, frac, source, &c, h.data(), ant, re, k.data(), l.data(), re, noise, session);
    ++n_rows;
    while (!realtime && !nr_isac_drained())
      std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  std::fclose(f);
  while (!nr_isac_drained())
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  nr_isac_stop();
  const double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("isac_replay: rows=%llu gate_closes=%llu gate_discarded_rows=%llu recorded_span_s=%.3f replay_wall_s=%.3f\n",
              (unsigned long long)n_rows, (unsigned long long)n_close,
              (unsigned long long)nr_isac_gate_discarded_rows(), (last_t - first_t) * 1e-9, wall_s);
  return rc;
}
