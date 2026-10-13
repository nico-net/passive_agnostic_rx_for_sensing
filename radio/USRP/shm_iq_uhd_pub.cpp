// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Standalone wideband UHD publisher for the multi-cell receiver (MPCI M4 wideband mode): one radio, N channels, one centre frequency
// (each X410 ZBX channel has its own LO, tuned untimed: no inter-channel phase calibration here), sc16 straight
// from the UHD streamer into the shm IQ ring (radio/COMMON/shm_iq_ring.h), consumed by one shm_iq receiver process per cell.
// No OAI softmodem / config / log dependency. No site defaults: every radio parameter is an explicit option.
//
//   shm_iq_uhd_pub --args "type=x4xx,addr=...,use_dpdk=1" --rate 245760000 --freq 3.6e9 --gain 38 --channels 4 
//       [--antenna RX1] [--ring isac_iq] [--capacity 33554432] [--chan-map 0,1,2,3] [--spp N] [--chunk 65536] [--mcr Hz]
//       [--rx-core C] [--duration s] [--radio-id S] [--time-source free|pps] [--bw Hz]
//
// Memory-bandwidth budget (per direction): 4 ch x rate x 4 B = 3.9 GB/s at 245.76 MS/s, 7.9 GB/s at 491.52 MS/s. Zero-copy path
// (recv into ring slots): one write stream (NIC DMA -> mbuf, UHD copies mbuf -> slot) + each consumer's read. Overflow-gap copy
// path is rare (once per gap).
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/tune_request.hpp>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <stdexcept>
#include <pthread.h>
#include <sched.h>
#include <dirent.h>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include "shm_iq_uhd_core.h"

static volatile sig_atomic_t g_stop;
static void on_sig(int) { g_stop = 1; }

[[noreturn]] static void die(const char *msg)
{
  fprintf(stderr, "SHMIQ_UHD_FAIL %s\n", msg);
  exit(2);
}

// ISAC_UHD_CTRL_CORES="a,b": move UHD's busy-polling uhd_ctrl* threads (same policy as usrp_lib.cpp, without the core plan).
static void repin_uhd_ctrl()
{
  const char *e = getenv("ISAC_UHD_CTRL_CORES");
  if (!e || !*e) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const char *q = e; *q;) {
    char *end = nullptr;
    long c = strtol(q, &end, 10);
    if (end == q) break;
    if (c >= 0 && c < CPU_SETSIZE) CPU_SET((int)c, &set);
    q = (*end == ',') ? end + 1 : end;
  }
  int moved = 0;
  if (DIR *d = opendir("/proc/self/task")) {
    while (dirent *de = readdir(d)) {
      char path[320], comm[32] = {0};
      snprintf(path, sizeof(path), "/proc/self/task/%s/comm", de->d_name);
      FILE *f = fopen(path, "r");
      if (!f) continue;
      if (fgets(comm, sizeof(comm), f) && !strncmp(comm, "uhd_ctrl", 8) && !sched_setaffinity((pid_t)atoi(de->d_name), sizeof(set), &set)) moved++;
      fclose(f);
    }
    closedir(d);
  }
  fprintf(stderr, "SHMIQ_UHD_REPIN uhd_ctrl threads moved=%d cores=%s\n", moved, e);
}

struct RecvCtx {
  uhd::rx_streamer::sptr rxs;
  size_t nch;
  double mcr, timeout;
  int64_t decim, start_tick; // sample ticks = MCR ticks / decim; start_tick: scheduled START of the current stream
  uhd::rx_metadata_t md;
};

static pub_rx_status_t uhd_recv(void *vc, int16_t *const *dst, int n, int64_t *tick, int *got)
{
  RecvCtx *c = (RecvCtx *)vc;
  std::vector<void *> bufs(dst, dst + c->nch);
  const size_t k = c->rxs->recv(bufs, (size_t)n, c->md, c->timeout, false);
  c->timeout = 1.0;
  switch (c->md.error_code) {
    case uhd::rx_metadata_t::ERROR_CODE_NONE:
      if (!c->md.has_time_spec) return PUB_RX_OTHER;
      *tick = pub_tick(c->md.time_spec.to_ticks(c->mcr), c->decim, c->start_tick);
      *got = (int)k;
      return PUB_RX_OK;
    case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW: return PUB_RX_OVERFLOW;
    case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT: return PUB_RX_TIMEOUT;
    default: fprintf(stderr, "SHMIQ_UHD_ERR %s\n", c->md.strerror().c_str()); return PUB_RX_OTHER;
  }
}

static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char **argv)
{
  std::string args, antenna = "RX1", ring = "isac_iq", chan_map, radio_id, tsrc = "free";
  double rate = 0, freq = 0, gain = -1, mcr = 0, duration = 0, bw = 0;
  long channels = 0, spp = 0, rx_core = -1, chunk = 65536;
  unsigned long long capacity = 1ull << 25;
  static const option opts[] = {{"args", 1, 0, 'a'}, {"rate", 1, 0, 'r'}, {"freq", 1, 0, 'f'}, {"gain", 1, 0, 'g'}, {"channels", 1, 0, 'n'},
                                {"antenna", 1, 0, 'A'}, {"ring", 1, 0, 'R'}, {"capacity", 1, 0, 'C'}, {"chan-map", 1, 0, 'm'}, {"spp", 1, 0, 's'},
                                {"rx-core", 1, 0, 'c'}, {"duration", 1, 0, 'd'}, {"radio-id", 1, 0, 'i'}, {"time-source", 1, 0, 't'},
                                {"mcr", 1, 0, 'M'}, {"bw", 1, 0, 'b'}, {"chunk", 1, 0, 'k'}, {0, 0, 0, 0}};
  for (int o; (o = getopt_long(argc, argv, "", opts, nullptr)) != -1;) {
    switch (o) {
      case 'a': args = optarg; break;
      case 'r': rate = atof(optarg); break;
      case 'f': freq = atof(optarg); break;
      case 'g': gain = atof(optarg); break;
      case 'n': channels = atol(optarg); break;
      case 'A': antenna = optarg; break;
      case 'R': ring = optarg; break;
      case 'C': capacity = strtoull(optarg, nullptr, 10); break;
      case 'm': chan_map = optarg; break;
      case 's': spp = atol(optarg); break;
      case 'c': rx_core = atol(optarg); break;
      case 'd': duration = atof(optarg); break;
      case 'i': radio_id = optarg; break;
      case 't': tsrc = optarg; break;
      case 'M': mcr = atof(optarg); break;
      case 'b': bw = atof(optarg); break;
      case 'k': chunk = atol(optarg); break;
      default: die("usage: --args S --rate Hz --freq Hz --gain dB --channels N [--antenna] [--ring] [--capacity] [--chan-map] [--spp] [--chunk] [--mcr] [--bw] [--rx-core] [--duration] [--radio-id] [--time-source free|pps]");
    }
  }
  if (args.empty() || !(rate > 0) || !(freq > 0) || gain < 0 || channels < 1 || channels > 8L) die("--args --rate --freq --gain --channels are required");
  if (tsrc != "free" && tsrc != "pps") die("--time-source must be free or pps");
  if (chunk < 1024 || capacity < (unsigned long long)chunk * 4) die("--chunk >= 1024 and --capacity >= 4 x chunk");
  std::vector<size_t> map;
  if (chan_map.empty()) for (long i = 0; i < channels; ++i) map.push_back((size_t)i);
  else for (char *s = strdup(chan_map.c_str()), *sp = nullptr, *t = strtok_r(s, ",", &sp); t; t = strtok_r(nullptr, ",", &sp)) map.push_back((size_t)atol(t));
  if ((long)map.size() != channels) die("--chan-map length must equal --channels");

  if (rx_core >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int)rx_core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) die("cannot pin to --rx-core");
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_sig; // no SA_RESTART: a blocking recv returns, the loop sees g_stop
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  int rc = 0;
  pub_t *pub = nullptr;
  try {
    // x4xx takes the MCR at device init only (usrp_lib.cpp passes master_clock_rate in the args too)
    if (mcr > 0) args += ",master_clock_rate=" + std::to_string(mcr);
    auto usrp = uhd::usrp::multi_usrp::make(args);
    mcr = usrp->get_master_clock_rate();
    fprintf(stderr, "SHMIQ_UHD_MCR %.0f\n", mcr);
    // CG images have no DDC (usrp_lib.cpp, MEASURED 2026-08-02): a rate != MCR is coerced back; the check below aborts on it.
    for (size_t u : map) {
      usrp->set_rx_rate(rate, u);
      usrp->set_rx_freq(uhd::tune_request_t(freq), u);
      usrp->set_rx_gain(gain, u);
      usrp->set_rx_antenna(antenna, u);
      usrp->set_rx_bandwidth(bw > 0 ? bw : usrp->get_rx_bandwidth_range(u).clip(rate), u);
    }
    for (size_t u : map) {
      const double ar = usrp->get_rx_rate(u);
      fprintf(stderr, "SHMIQ_UHD_CH uhd_ch=%zu rate=%.3f freq=%.3f gain=%.1f bw=%.0f ant=%s\n", u, ar, usrp->get_rx_freq(u), usrp->get_rx_gain(u),
              usrp->get_rx_bandwidth(u), usrp->get_rx_antenna(u).c_str());
      if (std::fabs(ar - rate) > 0.5) { fprintf(stderr, "SHMIQ_UHD_FAIL actual rate %.3f != requested %.3f on ch %zu (check the FPGA image / --mcr)\n", ar, rate, u); return 2; }
      const auto sn = usrp->get_rx_sensor_names(u);
      if (std::find(sn.begin(), sn.end(), "lo_locked") != sn.end()) {
        bool locked = false;
        for (int i = 0; i < 40 && !locked; ++i) { locked = usrp->get_rx_sensor("lo_locked", u).to_bool(); if (!locked) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        fprintf(stderr, "SHMIQ_UHD_LO ch=%zu locked=%d\n", u, (int)locked);
        if (!locked) throw std::runtime_error("lo_locked false"); // not die(): exit() would skip the device release
      }
    }
    if (radio_id.empty()) {
      try { radio_id = usrp->get_usrp_rx_info(map[0]).get("mboard_serial"); } catch (...) { radio_id = "uhd" + std::to_string(getpid()); }
    }
    if (tsrc == "pps") { // PPS-aligned time base: comparable across radios
      usrp->set_time_source("external");
      const auto p0 = usrp->get_time_last_pps();
      const double t0 = now_s();
      while (usrp->get_time_last_pps() == p0) { if (now_s() - t0 > 3) throw std::runtime_error("no PPS edge within 3 s"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
      usrp->set_time_next_pps(uhd::time_spec_t(0.0));
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    } else usrp->set_time_now(uhd::time_spec_t(0.0));

    uhd::stream_args_t sargs("sc16", "sc16");
    sargs.channels = map;
    if (spp > 0) sargs.args["spp"] = std::to_string(spp);
    auto rx = usrp->get_rx_stream(sargs);
    fprintf(stderr, "SHMIQ_UHD_STREAM max_num_samps=%zu chunk=%ld\n", rx->get_max_num_samps(), chunk);

    pub = pub_open(ring.c_str(), (unsigned)channels, capacity, rate, freq, radio_id.c_str(), tsrc == "pps" ? 2 : 1);
    if (!pub) throw std::runtime_error("ring create failed");
    const pub_stats_t *st = pub_stats(pub);
    fprintf(stderr, "SHMIQ_UHD_READY ring=/%s channels=%ld rate=%.0f center_hz=%.3f capacity=%llu bytes=%llu radio=%s time_source=%s\n", ring.c_str(), channels,
            rate, freq, (unsigned long long)pub_capacity(pub), (unsigned long long)(4096 + (uint64_t)channels * pub_capacity(pub) * 4), radio_id.c_str(), tsrc.c_str());

    const double dr = mcr / rate;
    RecvCtx ctx{rx, (size_t)channels, mcr, 5.0, std::llround(dr), 0, {}};
    if (std::fabs(dr - (double)ctx.decim) > 1e-9 * dr) throw std::runtime_error("MCR / rate is not an integer");
    std::vector<std::vector<int16_t>> scratch((size_t)channels, std::vector<int16_t>(65536 * 2));
    std::vector<void *> sb;
    for (auto &v : scratch) sb.push_back(v.data());
    auto stop_drain = [&]() {
      uhd::stream_cmd_t stop(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
      stop.stream_now = true;
      rx->issue_stream_cmd(stop);
      uhd::rx_metadata_t md;
      for (int i = 0; i < 200; ++i) if (rx->recv(sb, 65536, md, 0.1, false) == 0 && md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) break;
    };
    // timed START on all channels (RFNoC rejects stream_now on the 4-channel X410 streamer, usrp_lib.cpp)
    auto start = [&](double lead) {
      uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
      cmd.stream_now = false;
      cmd.time_spec = usrp->get_time_now() + uhd::time_spec_t(lead);
      ctx.start_tick = pub_tick(cmd.time_spec.to_ticks(mcr), ctx.decim, 0);
      rx->issue_stream_cmd(cmd);
    };
    start(1.5);
    repin_uhd_ctrl();
    const double t_start = now_s();
    double t_first = 0, t_stat = t_start, t_end = duration > 0 ? t_start + duration : 0;
    int bad_run = 0;
    while (!g_stop && !(t_end && now_s() >= t_end)) {
      const uint64_t ovf = st->overflows, oth = st->others;
      const int got = pub_step(pub, uhd_recv, &ctx, (int)chunk);
      if (got > 0 && t_first == 0) t_first = now_s();
      const bool err = st->overflows != ovf || st->others != oth;
      bad_run = got > 0 ? 0 : bad_run + err;
      if (err && bad_run < 10) {
        // MEASURED in usrp_lib.cpp (X410, DPDK): after ERROR_CODE_OVERFLOW (out of sequence) RFNoC halts the stream: re-arm it;
        // the next packet's timestamp jump becomes a ring gap
        if (st->overflows != ovf) fprintf(stderr, "SHMIQ_UHD_OVERFLOW n=%llu\n", (unsigned long long)st->overflows);
        stop_drain();
        start(0.1);
        fprintf(stderr, "SHMIQ_UHD_RESTART run=%d\n", bad_run);
      }
      if (st->consecutive_timeouts >= 10 || bad_run >= 10) { fprintf(stderr, "SHMIQ_UHD_FAIL stream dead (timeouts=%d errors=%d)\n", st->consecutive_timeouts, bad_run); rc = 3; break; }
      const double t = now_s();
      if (t - t_stat >= 1.0) {
        t_stat = t;
        fprintf(stderr, "SHMIQ_UHD_STATS t=%.1f samples=%llu rate_meas=%.1f overflows=%llu timeouts=%llu ring_head=%llu\n", t - t_start,
                (unsigned long long)st->samples, t_first > 0 && t > t_first ? (double)st->samples / (t - t_first) : 0.0, (unsigned long long)st->overflows,
                (unsigned long long)st->timeouts, (unsigned long long)pub_stats(pub)->head);
      }
    }
    // clean stop: the X410 claim must be released (never leave the device streaming)
    stop_drain();
    fprintf(stderr, "SHMIQ_UHD_END samples=%llu overflows=%llu timeouts=%llu inplace=%llu copied=%llu\n", (unsigned long long)st->samples,
            (unsigned long long)st->overflows, (unsigned long long)st->timeouts, (unsigned long long)st->inplace, (unsigned long long)st->copied);
    rx.reset();
  } catch (const std::exception &e) {
    fprintf(stderr, "SHMIQ_UHD_FAIL exception: %s\n", e.what());
    rc = 2;
  }
  pub_close(pub);
  return rc; // usrp sptr released at scope exit above
}
