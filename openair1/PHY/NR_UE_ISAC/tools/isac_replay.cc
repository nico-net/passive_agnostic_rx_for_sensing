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
  std::vector<char *> args{argv[0]};
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--rows") && i + 1 < argc)
      rows = argv[++i];
    else if (!std::strcmp(argv[i], "--realtime"))
      realtime = true;
    else
      args.push_back(argv[i]);
  }
  if (rows.empty()) {
    std::fprintf(stderr, "usage: isac_replay -O <ue.conf> --rows <cfr_rows.bin> [--realtime]\n");
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
  for (;;) {
    char magic[4];
    uint32_t kind, slot, prb, scs, ant, re;
    float frac, noise;
    int32_t source;
    uint64_t fc, session, t;
    uint16_t pci, spf;
    if (std::fread(magic, 1, 4, f) != 4) break;
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
    if (!n_rows && !n_close) first_t = t;
    last_t = t;
    if (realtime)
      std::this_thread::sleep_until(started + std::chrono::nanoseconds(t - first_t));
    if (kind == 1) {
      nr_isac_request_discard();
      ++n_close;
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
