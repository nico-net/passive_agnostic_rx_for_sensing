// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Standalone wideband UHD publisher for the multi-cell receiver (MPCI M4 wideband mode): one radio, N channels, one centre frequency
// (each X410 ZBX channel has its own LO, tuned untimed: no inter-channel phase calibration here), sc16 straight
// from the UHD streamer into the shm IQ ring (radio/COMMON/shm_iq_ring.h), consumed by one shm_iq receiver process per cell.
// No OAI softmodem / config / log dependency. No site defaults: every radio parameter is an explicit option.
//
//   shm_iq_uhd_pub --args "type=x4xx,addr=...,use_dpdk=1" --rate 245760000 --freq 3.6e9 --gain 38 --channels 4 
//       [--antenna RX1] [--ring isac_iq] [--capacity 33554432] [--chan-map 0,1,2,3] [--spp N] [--chunk 65536] [--mcr Hz]
//       [--rx-core C] [--duration s] [--radio-id S] [--time-source free|pps] [--bw Hz]
//       [--multi-streamer (default) | --single-streamer] [--priority high|normal] [--rx-cores a,b,c,d] [--untimed-tune]
//
// --multi-streamer (default, the configuration of uhd benchmark_rate --multi_streamer): one rx_streamer + one recv thread per channel,
// all started by the SAME timed stream command, so the first-sample ticks match. Per chunk the main thread reserves the ring slots
// (before any thread writes), every thread recv()s its channel into its slot, a barrier collects the results and the chunk is
// committed only if all channels report the same tick and count (pub_mc_finish); otherwise all streamers are re-armed together
// (SHMIQ_UHD_RESYNC) and the tick jump is published as a ring gap.
//
// Memory-bandwidth budget (per direction): 4 ch x rate x 4 B = 3.9 GB/s at 245.76 MS/s, 7.9 GB/s at 491.52 MS/s. Zero-copy path
// (recv into ring slots): one write stream (NIC DMA -> mbuf, UHD copies mbuf -> slot) + each consumer's read. Overflow-gap copy
// path is rare (once per gap).
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/tune_request.hpp>
#include <uhd/utils/thread.hpp>
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
#include <memory>
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

static void set_prio(bool high)
{
  if (high && !uhd::set_thread_priority_safe(1.0f, true)) fprintf(stderr, "SHMIQ_UHD_WARN realtime priority not permitted (run as root / CAP_SYS_NICE): continuing at normal priority\n");
}

static void pin_self(long core)
{
  if (core < 0) return;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET((int)core, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) fprintf(stderr, "SHMIQ_UHD_WARN cannot pin to core %ld\n", core);
}

// One recv thread per channel; the main thread is the coordinator. bar: N+1 parties, waited twice per chunk (go / done).
struct Chunk {
  pthread_barrier_t bar;
  int16_t *slots[PUB_MAX_CH];
  pub_chres_t res[PUB_MAX_CH];
  int n = 0;
  volatile bool quit = false;
};

static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

int main(int argc, char **argv)
{
  std::string args, antenna = "RX1", ring = "isac_iq", chan_map, radio_id, tsrc = "free", rx_cores, prio = "high";
  bool multi = true, timed_tune = true;
  double rate = 0, freq = 0, gain = -1, mcr = 0, duration = 0, bw = 0;
  long channels = 0, spp = 0, rx_core = -1, chunk = 65536;
  unsigned long long capacity = 1ull << 25;
  static const option opts[] = {{"args", 1, 0, 'a'}, {"rate", 1, 0, 'r'}, {"freq", 1, 0, 'f'}, {"gain", 1, 0, 'g'}, {"channels", 1, 0, 'n'},
                                {"antenna", 1, 0, 'A'}, {"ring", 1, 0, 'R'}, {"capacity", 1, 0, 'C'}, {"chan-map", 1, 0, 'm'}, {"spp", 1, 0, 's'},
                                {"rx-core", 1, 0, 'c'}, {"duration", 1, 0, 'd'}, {"radio-id", 1, 0, 'i'}, {"time-source", 1, 0, 't'},
                                {"mcr", 1, 0, 'M'}, {"bw", 1, 0, 'b'}, {"chunk", 1, 0, 'k'}, {"multi-streamer", 0, 0, 1000}, {"single-streamer", 0, 0, 1001},
                                {"priority", 1, 0, 1002}, {"rx-cores", 1, 0, 1003}, {"untimed-tune", 0, 0, 1004}, {0, 0, 0, 0}};
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
      case 1000: multi = true; break;
      case 1001: multi = false; break;
      case 1002: prio = optarg; break;
      case 1003: rx_cores = optarg; break;
      case 1004: timed_tune = false; break;
      default: die("usage: --args S --rate Hz --freq Hz --gain dB --channels N [--antenna] [--ring] [--capacity] [--chan-map] [--spp] [--chunk] [--mcr] [--bw] [--rx-core] [--duration] [--radio-id] [--time-source free|pps] [--multi-streamer|--single-streamer] [--priority high|normal] [--rx-cores a,b,..] [--untimed-tune]");
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
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set)) die("cannot pin to --rx-core"); // multi-streamer: pins the coordinator
  }
  std::vector<long> cores;
  for (char *s2 = strdup(rx_cores.c_str()), *sp = nullptr, *t = strtok_r(s2, ",", &sp); t; t = strtok_r(nullptr, ",", &sp)) cores.push_back(atol(t));
  if (!cores.empty() && (long)cores.size() != channels) die("--rx-cores length must equal --channels");
  if (!multi && !cores.empty()) die("--rx-cores needs --multi-streamer (use --rx-core)");
  if (prio != "high" && prio != "normal") die("--priority must be high or normal");
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
    // time base first: the timed tune below needs it
    if (tsrc == "pps") { // PPS-aligned time base: comparable across radios
      usrp->set_time_source("external");
      const auto p0 = usrp->get_time_last_pps();
      const double t0 = now_s();
      while (usrp->get_time_last_pps() == p0) { if (now_s() - t0 > 3) throw std::runtime_error("no PPS edge within 3 s"); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
      usrp->set_time_next_pps(uhd::time_spec_t(0.0));
      std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    } else usrp->set_time_now(uhd::time_spec_t(0.0));
    // CG images have no DDC (usrp_lib.cpp, MEASURED 2026-08-02): a rate != MCR is coerced back; the check below aborts on it.
    for (size_t u : map) {
      usrp->set_rx_rate(rate, u);
      usrp->set_rx_gain(gain, u);
      usrp->set_rx_antenna(antenna, u);
      usrp->set_rx_bandwidth(bw > 0 ? bw : usrp->get_rx_bandwidth_range(u).clip(rate), u);
    }
    // One timed tune for all channels: every LO/NCO command executes at the same radio time. Whether the X410 honours it per
    // channel (ZBX LO/NCO) is not verified; --untimed-tune tunes channel by channel.
    {
      double t_cmd = 0;
      if (timed_tune) {
        t_cmd = (usrp->get_time_now() + uhd::time_spec_t(0.5)).get_real_secs();
        usrp->set_command_time(uhd::time_spec_t(t_cmd));
      }
      for (size_t u : map) usrp->set_rx_freq(uhd::tune_request_t(freq), u);
      if (timed_tune) {
        usrp->clear_command_time();
        std::this_thread::sleep_for(std::chrono::milliseconds(700)); // past the command time, before the lo_locked check
      }
      std::string cl;
      for (size_t u : map) cl += (cl.empty() ? "" : ",") + std::to_string(u);
      fprintf(stderr, "SHMIQ_UHD_TUNE timed=%d t=%.6f chans=%s\n", (int)timed_tune, t_cmd, cl.c_str());
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

    // multi: one single-channel streamer per channel (like benchmark_rate --multi_streamer); single: one N-channel streamer
    const size_t nstr = multi ? (size_t)channels : 1;
    const double dr = mcr / rate;
    if (std::fabs(dr - std::llround(dr)) > 1e-9 * dr) throw std::runtime_error("MCR / rate is not an integer");
    std::vector<RecvCtx> ctxs;
    for (size_t i = 0; i < nstr; ++i) {
      uhd::stream_args_t sargs("sc16", "sc16");
      if (multi) sargs.channels = {map[i]};
      else sargs.channels = map;
      if (spp > 0) sargs.args["spp"] = std::to_string(spp);
      ctxs.push_back(RecvCtx{usrp->get_rx_stream(sargs), multi ? (size_t)1 : (size_t)channels, mcr, 5.0, std::llround(dr), 0, {}});
    }
    fprintf(stderr, "SHMIQ_UHD_STREAM streamers=%zu max_num_samps=%zu chunk=%ld priority=%s\n", nstr, ctxs[0].rxs->get_max_num_samps(), chunk, prio.c_str());

    pub = pub_open(ring.c_str(), (unsigned)channels, capacity, rate, freq, radio_id.c_str(), tsrc == "pps" ? 2 : 1);
    if (!pub) throw std::runtime_error("ring create failed");
    const pub_stats_t *st = pub_stats(pub);
    fprintf(stderr, "SHMIQ_UHD_READY ring=/%s channels=%ld rate=%.0f center_hz=%.3f capacity=%llu bytes=%llu radio=%s time_source=%s\n", ring.c_str(), channels,
            rate, freq, (unsigned long long)pub_capacity(pub), (unsigned long long)(4096 + (uint64_t)channels * pub_capacity(pub) * 4), radio_id.c_str(), tsrc.c_str());

    std::vector<std::vector<int16_t>> scratch((size_t)channels, std::vector<int16_t>(65536 * 2));
    std::vector<void *> sb;
    for (auto &v : scratch) sb.push_back(v.data());
    auto stop_drain = [&]() {
      uhd::stream_cmd_t stop(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
      stop.stream_now = true;
      for (auto &c : ctxs) c.rxs->issue_stream_cmd(stop);
      for (auto &c : ctxs) {
        std::vector<void *> b(sb.begin(), sb.begin() + (long)c.nch);
        uhd::rx_metadata_t md;
        for (int i = 0; i < 200; ++i) if (c.rxs->recv(b, 65536, md, 0.1, false) == 0 && md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) break;
      }
    };
    // timed START on all channels (RFNoC rejects stream_now on the 4-channel X410 streamer, usrp_lib.cpp); the SAME command goes
    // to every streamer so their first-sample ticks agree
    auto start = [&](double lead) {
      uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
      cmd.stream_now = false;
      cmd.time_spec = usrp->get_time_now() + uhd::time_spec_t(lead);
      const int64_t t0 = pub_tick(cmd.time_spec.to_ticks(mcr), ctxs[0].decim, 0);
      for (auto &c : ctxs) { c.start_tick = t0; c.rxs->issue_stream_cmd(cmd); }
    };

    Chunk ck;
    std::vector<std::thread> workers;
    struct Guard { // joins the workers on every exit path (a joinable std::thread would terminate())
      Chunk &ck; std::vector<std::thread> &w;
      ~Guard() { if (w.empty()) return; ck.quit = true; pthread_barrier_wait(&ck.bar); for (auto &t : w) t.join(); pthread_barrier_destroy(&ck.bar); }
    } guard{ck, workers};
    if (multi) {
      pthread_barrier_init(&ck.bar, nullptr, (unsigned)nstr + 1);
      for (size_t i = 0; i < nstr; ++i)
        workers.emplace_back([&, i]() {
          pin_self(cores.empty() ? -1 : cores[i]);
          set_prio(prio == "high");
          for (;;) {
            pthread_barrier_wait(&ck.bar); // go: slots reserved
            if (ck.quit) return;
            int64_t tick = 0;
            int got = 0;
            ck.res[i].st = uhd_recv(&ctxs[i], &ck.slots[i], ck.n, &tick, &got);
            ck.res[i].tick = tick;
            ck.res[i].got = got;
            pthread_barrier_wait(&ck.bar); // done
          }
        });
    } else set_prio(prio == "high");

    start(1.5);
    repin_uhd_ctrl();
    const double t_start = now_s();
    double t_first = 0, t_stat = t_start, t_end = duration > 0 ? t_start + duration : 0;
    int bad_run = 0;
    while (!g_stop && !(t_end && now_s() >= t_end)) {
      pub_mc_act_t act;
      int got;
      if (multi) {
        ck.n = pub_mc_reserve(pub, (int)chunk, ck.slots); // before ANY thread writes a slot (seqlock)
        pthread_barrier_wait(&ck.bar);
        pthread_barrier_wait(&ck.bar);
        got = pub_mc_finish(pub, ck.res, ck.slots, &act);
        if (act == PUB_MC_MISMATCH) {
          fprintf(stderr, "SHMIQ_UHD_RESYNC n=%llu", (unsigned long long)st->resyncs);
          for (long i = 0; i < channels; ++i) fprintf(stderr, " ch%ld=%d/%lld/%d", i, (int)ck.res[i].st, (long long)ck.res[i].tick, ck.res[i].got);
          fprintf(stderr, "\n");
        }
      } else {
        const uint64_t ovf = st->overflows, oth = st->others;
        got = pub_step(pub, uhd_recv, &ctxs[0], (int)chunk);
        act = st->overflows != ovf ? PUB_MC_OVERFLOW : st->others != oth ? PUB_MC_OTHER : got > 0 ? PUB_MC_OK : PUB_MC_IDLE;
      }
      if (got > 0 && t_first == 0) t_first = now_s();
      const bool err = act == PUB_MC_OVERFLOW || act == PUB_MC_MISMATCH || act == PUB_MC_OTHER;
      bad_run = got > 0 ? 0 : bad_run + err;
      if (err && bad_run < 10) {
        // MEASURED in usrp_lib.cpp (X410, DPDK): after ERROR_CODE_OVERFLOW (out of sequence) RFNoC halts the stream: re-arm it
        // (all streamers together); the next packet's timestamp jump becomes a ring gap
        if (act == PUB_MC_OVERFLOW) fprintf(stderr, "SHMIQ_UHD_OVERFLOW n=%llu\n", (unsigned long long)st->overflows);
        stop_drain();
        start(0.1);
        fprintf(stderr, "SHMIQ_UHD_RESTART run=%d\n", bad_run);
      }
      if (st->consecutive_timeouts >= 10 || bad_run >= 10) { fprintf(stderr, "SHMIQ_UHD_FAIL stream dead (timeouts=%d errors=%d)\n", st->consecutive_timeouts, bad_run); rc = 3; break; }
      const double t = now_s();
      if (t - t_stat >= 1.0) {
        t_stat = t;
        fprintf(stderr, "SHMIQ_UHD_STATS t=%.1f samples=%llu rate_meas=%.1f overflows=%llu timeouts=%llu resyncs=%llu ring_head=%llu\n", t - t_start,
                (unsigned long long)st->samples, t_first > 0 && t > t_first ? (double)st->samples / (t - t_first) : 0.0, (unsigned long long)st->overflows,
                (unsigned long long)st->timeouts, (unsigned long long)st->resyncs, (unsigned long long)pub_stats(pub)->head);
      }
    }
    // clean stop: the X410 claim must be released (never leave the device streaming)
    stop_drain();
    fprintf(stderr, "SHMIQ_UHD_END samples=%llu overflows=%llu timeouts=%llu resyncs=%llu inplace=%llu copied=%llu\n", (unsigned long long)st->samples,
            (unsigned long long)st->overflows, (unsigned long long)st->timeouts, (unsigned long long)st->resyncs, (unsigned long long)st->inplace, (unsigned long long)st->copied);
  } catch (const std::exception &e) {
    fprintf(stderr, "SHMIQ_UHD_FAIL exception: %s\n", e.what());
    rc = 2;
  }
  pub_close(pub);
  return rc; // usrp sptr released at scope exit above
}
