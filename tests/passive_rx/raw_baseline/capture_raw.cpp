// Receive-only bounded raw IQ capture. No NR interpretation, TX, or external sync.
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using Sample = std::complex<int16_t>;
static_assert(sizeof(Sample) == 4, "SC16 complex storage required");
constexpr size_t CHANNELS = 4;
constexpr size_t CHUNK = 65536;
constexpr uint64_t MAX_BYTES = UINT64_C(8) * 1024 * 1024 * 1024;
struct Block { uint64_t offset, count; int64_t tick; };
struct Continuity {
  uint64_t samples = 0;
  int64_t first = 0;
  bool accept(size_t n, int64_t tick, bool valid) {
    if (!valid || !n || tick < 0 || samples > uint64_t(INT64_MAX)) return false;
    if (samples && (first > INT64_MAX - int64_t(samples) || tick != first + int64_t(samples))) return false;
    if (!samples) first = tick;
    samples += n;
    return true;
  }
};
static void require(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
static void save(const std::filesystem::path &path, const void *data, size_t bytes) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  require(fd >= 0, "cannot create " + path.string());
  const auto *p = static_cast<const char *>(data);
  while (bytes) {
    ssize_t n = ::write(fd, p, std::min(bytes, size_t(16 * 1024 * 1024)));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { ::close(fd); throw std::runtime_error("write failed: " + path.string()); }
    p += n; bytes -= size_t(n);
  }
  int sync_rc = ::fsync(fd);
  int close_rc = ::close(fd);
  require(sync_rc == 0 && close_rc == 0, "flush failed: " + path.string());
}
static void save_text(const std::filesystem::path &p, const std::string &s) { save(p, s.data(), s.size()); }
struct StopStream {
  uhd::rx_streamer::sptr stream;
  bool active = false;
  void stop() {
    if (active) {
      stream->issue_stream_cmd(uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS));
      active = false;
    }
  }
  ~StopStream() { try { stop(); } catch (...) {} }
};
static int self_test() {
  Continuity c;
  require(c.accept(10, 100, true), "first block");
  require(c.accept(7, 110, true), "contiguous block");
  require(!c.accept(1, 118, true), "gap accepted");
  require(!c.accept(1, 116, true), "overlap accepted");
  require(!c.accept(1, 117, false), "bad metadata accepted");
  require(!c.accept(0, 117, true), "empty receive accepted");
  require(c.samples == 17 && c.accept(1, 117, true), "rejection modified state");
  std::cout << "PASS: continuity, gap, overlap, metadata, empty-block, state invariants; no device opened\n";
  return 0;
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") return self_test();
    require(argc == 8, "usage: capture_raw OUTDIR DEVICE_ARGS SAMPLES_PER_CHANNEL RATE_HZ FREQ_HZ GAIN_DB ANTENNA");
    const std::filesystem::path out(argv[1]);
    const uint64_t wanted = std::stoull(argv[3]);
    const double rate = std::stod(argv[4]), freq = std::stod(argv[5]), gain = std::stod(argv[6]);
    require(wanted && wanted <= MAX_BYTES / CHANNELS / sizeof(Sample), "capture exceeds 8 GiB memory bound");
    require(std::isfinite(rate) && rate > 0 && std::isfinite(freq) && freq > 0 && std::isfinite(gain), "invalid RF measurement settings");
    const uint16_t endian = 1;
    require(*reinterpret_cast<const uint8_t *>(&endian) == 1, "little-endian host required");
    require(std::filesystem::is_directory(out), "output directory must exist");
    for (size_t c = 0; c < CHANNELS; ++c)
      require(!std::filesystem::exists(out / ("rx" + std::to_string(c) + ".sc16")), "refusing to overwrite IQ");
    require(!std::filesystem::exists(out / "capture_info.json"), "refusing to overwrite metadata");
    require(!std::filesystem::exists(out / "timestamps.tsv"), "refusing to overwrite timestamps");

    // Prefault all sample storage BEFORE claiming/streaming the device. No file
    // I/O or growing sample buffers on the receive path.
    std::array<std::vector<Sample>, CHANNELS> samples;
    for (auto &v : samples) v.resize(wanted);
    std::vector<Block> blocks;
    blocks.reserve(size_t(wanted / 1024 + 2));
    std::cout << "ALLOCATED bytes=" << wanted * CHANNELS * sizeof(Sample) << std::endl;
    auto usrp = uhd::usrp::multi_usrp::make(argv[2]);
    require(usrp->get_rx_num_channels() >= CHANNELS, "four receive channels required");
    usrp->set_clock_source("internal");
    usrp->set_time_source("internal");
    std::ostringstream info;
    info << std::setprecision(17) << "{\n  \"format\": \"sc16_le\",\n  \"channels\": 4,\n"
         << "  \"samples_per_channel\": " << wanted << ",\n  \"sample_rate_hz\": " << rate
         << ",\n  \"duration_s\": " << double(wanted) / rate << ",\n"
         << "  \"clock_source\": \"internal\",\n  \"time_source\": \"internal\",\n"
         << "  \"external_cell_synchronization\": false,\n  \"receiver_corrections_applied\": false,\n"
         << "  \"rf_channels\": [\n";
    for (size_t c = 0; c < CHANNELS; ++c) {
      usrp->set_rx_rate(rate, c);
      usrp->set_rx_freq(uhd::tune_request_t(freq), c);
      usrp->set_rx_gain(gain, c);
      usrp->set_rx_antenna(argv[7], c);
      require(std::abs(usrp->get_rx_rate(c) - rate) < 0.1, "unsupported exact sample rate");
      require(std::abs(usrp->get_rx_freq(c) - freq) < 1.0, "RF center mismatch");
      info << "    {\"channel\": " << c << ", \"frequency_hz\": " << usrp->get_rx_freq(c)
           << ", \"rate_hz\": " << usrp->get_rx_rate(c) << ", \"gain_db\": " << usrp->get_rx_gain(c)
           << ", \"bandwidth_hz\": " << usrp->get_rx_bandwidth(c)
           << ", \"antenna\": " << std::quoted(usrp->get_rx_antenna(c)) << "}" << (c + 1 < CHANNELS ? ",\n" : "\n");
    }
    info << "  ],\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    for (size_t c = 0; c < CHANNELS; ++c) {
      const auto sensors = usrp->get_rx_sensor_names(c);
      if (std::find(sensors.begin(), sensors.end(), "lo_locked") != sensors.end())
        require(usrp->get_rx_sensor("lo_locked", c).to_bool(), "RX LO not locked");
    }
    uhd::stream_args_t args("sc16", "sc16");
    args.channels = {0, 1, 2, 3};
    auto rx = usrp->get_rx_stream(args);
    StopStream guard{rx, false};
    uhd::stream_cmd_t start(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
    start.stream_now = false;
    start.time_spec = usrp->get_time_now() + uhd::time_spec_t(0.5);
    guard.active = true;
    rx->issue_stream_cmd(start);
    Continuity continuity;
    while (continuity.samples < wanted) {
      const size_t count = std::min<uint64_t>(CHUNK, wanted - continuity.samples);
      std::vector<void *> buffers;
      for (auto &v : samples) buffers.push_back(v.data() + continuity.samples);
      uhd::rx_metadata_t md;
      const size_t n = rx->recv(buffers, count, md, continuity.samples ? 1.0 : 2.0, false);
      require(n <= count, "receive exceeded requested extent");
      const int64_t tick = md.has_time_spec ? md.time_spec.to_ticks(rate) : -1;
      const uint64_t offset = continuity.samples;
      require(continuity.accept(n, tick, md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE &&
                                        md.has_time_spec && !md.out_of_sequence),
              "invalid/discontinuous RX at sample " + std::to_string(offset) + ": " + md.strerror());
      blocks.push_back({offset, n, tick});
    }
    guard.stop();
    // Drain only the tail after the captured interval, bounded independently of
    // firmware behavior. No samples from this tail enter the saved interval.
    std::array<std::vector<Sample>, CHANNELS> tail;
    std::vector<void *> buffers;
    for (auto &v : tail) { v.resize(CHUNK); buffers.push_back(v.data()); }
    for (int i = 0; i < 16; ++i) {
      uhd::rx_metadata_t md;
      if (!rx->recv(buffers, CHUNK, md, 0.02, false)) break;
    }
    guard.stream.reset(); rx.reset(); usrp.reset();
    std::cout << "RF_RELEASED samples_per_channel=" << continuity.samples << " blocks=" << blocks.size() << std::endl;
    std::ostringstream timestamps;
    timestamps << "offset_samples\tcount_samples\tfirst_sample_tick\n";
    for (const auto &b : blocks) timestamps << b.offset << '\t' << b.count << '\t' << b.tick << '\n';
    save_text(out / "timestamps.tsv", timestamps.str());
    for (size_t c = 0; c < CHANNELS; ++c)
      save(out / ("rx" + std::to_string(c) + ".sc16"), samples[c].data(), samples[c].size() * sizeof(Sample));
    info << "  \"first_sample_tick\": " << continuity.first << ",\n"
         << "  \"timestamp_tick_rate_hz\": " << rate << ",\n"
         << "  \"blocks\": " << blocks.size() << ",\n"
         << "  \"rx_metadata_errors\": 0,\n  \"timestamp_discontinuities\": 0,\n"
         << "  \"status\": \"COMPLETE_CONTIGUOUS_RAW_IQ\"\n}\n";
    save_text(out / "capture_info.json", info.str());
    std::cout << "PASS: four unfiltered RX channels, exact sample counts and continuous hardware timestamps\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "VOID: " << e.what() << std::endl;
    return 2;
  }
}
