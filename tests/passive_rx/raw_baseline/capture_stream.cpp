// Continuous, RX-only sc16 recorder. No receiver/cell synchronization is injected.
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/stream_cmd.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
using Sample = std::complex<int16_t>;
static_assert(sizeof(Sample) == 4);

static void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
static void write_all(int fd, const void *data, size_t bytes) {
  const auto *p = static_cast<const char *>(data);
  while (bytes) {
    ssize_t n = write(fd, p, bytes);
    if (n < 0 && errno == EINTR) continue;
    require(n > 0, "disk write: " + std::string(strerror(errno)));
    p += n; bytes -= n;
  }
}
static void exclusive_text(const std::string &path, const std::string &text) {
  int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
  require(fd >= 0, "exclusive output: " + path);
  try { write_all(fd, text.data(), text.size()); require(fsync(fd) == 0, "metadata fsync"); }
  catch (...) { close(fd); throw; }
  close(fd);
}
static std::string hex_digest(const unsigned char *bytes, unsigned length) {
  std::ostringstream s;
  for (unsigned i = 0; i < length; ++i) s << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
  return s.str();
}

class DiskWriter {
  int fd = -1;
  size_t slots, chunk;
  std::vector<Sample> data;
  std::vector<size_t> counts;
  uint64_t produced = 0, consumed = 0;
  bool done = false;
  std::string failure;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  EVP_MD_CTX *digest = nullptr;
  void work() noexcept {
    try {
      for (;;) {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return done || produced != consumed; });
        if (produced == consumed && done) return;
        size_t index = consumed % slots, n = counts[index];
        lock.unlock();
        write_all(fd, data.data() + index * chunk, n * sizeof(Sample));
        require(EVP_DigestUpdate(digest, data.data() + index * chunk, n * sizeof(Sample)) == 1, "SHA256 update");
        written += n;
        lock.lock();
        ++consumed;
      }
    } catch (const std::exception &e) {
      std::lock_guard<std::mutex> lock(mutex); failure = e.what();
    }
  }
public:
  std::atomic<uint64_t> written{0};
  uint64_t maximum_backlog = 0;
  std::string sha256;
  DiskWriter(const std::string &path, uint64_t samples, size_t nslots = 4096, size_t nchunk = 65536)
      : slots(nslots), chunk(nchunk), data(slots * chunk), counts(slots) {
    fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    require(fd >= 0, "cannot exclusively create IQ file");
    int rc = posix_fallocate(fd, 0, static_cast<off_t>(samples * sizeof(Sample)));
    if (rc) { close(fd); fd = -1; throw std::runtime_error("preallocation: " + std::string(strerror(rc))); }
    digest = EVP_MD_CTX_new();
    require(digest && EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) == 1, "SHA256 init");
    worker = std::thread(&DiskWriter::work, this);
  }
  ~DiskWriter() {
    if (worker.joinable()) {
      { std::lock_guard<std::mutex> lock(mutex); done = true; }
      wake.notify_one(); worker.join();
    }
    if (fd >= 0) close(fd);
    if (digest) EVP_MD_CTX_free(digest);
  }
  Sample *available() {
    std::lock_guard<std::mutex> lock(mutex);
    require(failure.empty(), failure);
    require(produced - consumed < slots, "disk backlog exceeded bounded ring; capture VOID");
    return data.data() + (produced % slots) * chunk;
  }
  void publish(size_t n) {
    std::lock_guard<std::mutex> lock(mutex);
    require(n && n <= chunk && produced - consumed < slots, "invalid writer publication");
    counts[produced % slots] = n; ++produced;
    maximum_backlog = std::max(maximum_backlog, produced - consumed);
    wake.notify_one();
  }
  void finish() {
    { std::lock_guard<std::mutex> lock(mutex); done = true; }
    wake.notify_one(); worker.join();
    require(ftruncate(fd, static_cast<off_t>(written.load() * sizeof(Sample))) == 0, "truncate saved prefix");
    require(fdatasync(fd) == 0, "IQ fdatasync");
    require(failure.empty(), failure);
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
    require(EVP_DigestFinal_ex(digest, bytes, &length) == 1 && length == 32, "SHA256 finalize");
    sha256 = hex_digest(bytes, length);
  }
};

struct Continuity {
  uint64_t total = 0; int64_t first = -1;
  void observe(int64_t tick, size_t count, bool metadata_ok) {
    require(metadata_ok && count > 0 && tick >= 0, "invalid RX metadata/count/timestamp");
    if (first < 0) first = tick;
    require(tick == first + static_cast<int64_t>(total), "hardware timestamp discontinuity");
    total += count;
  }
};
struct Record { uint64_t offset; int64_t tick; size_t count; };

static int self_test() {
  Continuity c; c.observe(100, 10, true); c.observe(110, 20, true);
  for (int kind = 0; kind < 3; ++kind) {
    bool rejected = false;
    try { auto trial = c; trial.observe(kind == 0 ? 131 : 130, kind == 1 ? 0 : 4, kind != 2); }
    catch (const std::exception &) { rejected = true; }
    require(rejected, "negative continuity test");
  }
  char directory[] = "/tmp/stream-recorder-test.XXXXXX";
  require(mkdtemp(directory) != nullptr, "self-test temporary directory");
  std::string file = std::string(directory) + "/rx0.sc16";
  std::vector<Sample> expected(100);
  for (int i = 0; i < 100; ++i) expected[i] = Sample(i - 50, 50 - i);
  {
    DiskWriter writer(file, 100, 8, 32);
    size_t offset = 0;
    for (size_t count : {10, 20, 30, 32, 8}) {
      std::copy_n(expected.data() + offset, count, writer.available());
      writer.publish(count); offset += count;
    }
    writer.finish();
    std::vector<Sample> actual(100);
    std::ifstream input(file, std::ios::binary);
    input.read(reinterpret_cast<char *>(actual.data()), 400);
    require(input.gcount() == 400 && actual == expected && writer.written == 100, "writer sample/order test");
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length = 0;
    require(EVP_Digest(expected.data(), 400, digest, &length, EVP_sha256(), nullptr) == 1, "reference SHA256");
    require(writer.sha256 == hex_digest(digest, length), "writer SHA256 mismatch");
  }
  unlink(file.c_str()); rmdir(directory);
  std::cout << "PASS: metadata/gap/zero rejection; asynchronous writer order, exact extent, SHA256; no radio opened\n";
  return 0;
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") return self_test();
    require(argc == 9 || argc == 11, "usage: capture_stream OUT DEVICE_ARGS PHYSICAL_CHANNEL SAVED_SECONDS RATE FREQ GAIN ANTENNA [SKIP_SECONDS RING_GIB]");
    const std::string out = argv[1], args = argv[2], antenna = argv[8];
    const unsigned channel = std::stoul(argv[3]);
    const double seconds = std::stod(argv[4]), rate = std::stod(argv[5]);
    const double frequency = std::stod(argv[6]), gain = std::stod(argv[7]);
    require(std::isfinite(seconds) && seconds > 0 && seconds <= 3600 && std::isfinite(rate) && rate > 0
            && rate <= 250000000 && std::isfinite(frequency) && frequency > 0 && std::isfinite(gain), "invalid recording bounds");
    const double skip_seconds = argc == 11 ? std::stod(argv[9]) : 0;
    const unsigned ring_gib = argc == 11 ? std::stoul(argv[10]) : 1;
    require(std::isfinite(skip_seconds) && skip_seconds >= 0 && skip_seconds <= 3600
            && ring_gib >= 1 && ring_gib <= 16, "invalid skip or memory bound");
    const uint64_t skip_samples = std::llround(skip_seconds * rate);
    const size_t ring_slots = (uint64_t(ring_gib) << 30) / (65536 * sizeof(Sample));
    const uint64_t requested = std::llround(seconds * rate);
    std::filesystem::create_directories(out);
    DiskWriter writer(out + "/rx0.sc16", requested, ring_slots);
    std::cout << "PREALLOCATED bytes=" << requested * 4 << " ring_bytes=" << ring_slots * 65536ULL * 4 << std::endl;
    Continuity continuity;
    std::vector<Record> records;
    records.reserve(requested / 65536 + 4096);
    uhd::usrp::multi_usrp::sptr usrp;
    uhd::rx_streamer::sptr rx;
    std::string error;
    double actual_rate = 0, actual_frequency = 0, actual_gain = 0, actual_bw = 0;
    uint64_t metadata_errors = 0;
    try {
      usrp = uhd::usrp::multi_usrp::make(args);
      require(channel < usrp->get_rx_num_channels(), "physical channel unavailable");
      usrp->set_clock_source("internal"); usrp->set_time_source("internal");
      usrp->set_rx_rate(rate, channel); usrp->set_rx_freq(frequency, channel);
      usrp->set_rx_gain(gain, channel); usrp->set_rx_antenna(antenna, channel);
      actual_rate = usrp->get_rx_rate(channel); actual_frequency = usrp->get_rx_freq(channel);
      actual_gain = usrp->get_rx_gain(channel); actual_bw = usrp->get_rx_bandwidth(channel);
      require(std::abs(actual_rate-rate) < .001 && std::abs(actual_frequency-frequency) < 1, "actual RF geometry differs from request");
      uhd::stream_args_t stream("sc16", "sc16"); stream.channels = {channel};
      rx = usrp->get_rx_stream(stream);
      uhd::stream_cmd_t command(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
      command.stream_now = false; command.time_spec = usrp->get_time_now() + uhd::time_spec_t(.5);
      rx->issue_stream_cmd(command);
      Continuity skipped;
      std::vector<Sample> discard(65536);
      uint64_t skip_progress = static_cast<uint64_t>(rate * 10);
      while (skipped.total < skip_samples) {
        uhd::rx_metadata_t md;
        size_t wanted = std::min<uint64_t>(discard.size(), skip_samples - skipped.total);
        size_t n = rx->recv(discard.data(), wanted, md, skipped.total ? .5 : 3.0, false);
        bool ok = md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE && md.has_time_spec && !md.out_of_sequence;
        if (!ok) { ++metadata_errors; throw std::runtime_error("skip RX metadata: " + md.strerror()); }
        skipped.observe(md.time_spec.to_ticks(rate), n, ok);
        if (skipped.total >= skip_progress) {
          std::cout << "SKIP_PROGRESS sample_seconds=" << skipped.total / rate << std::endl;
          skip_progress += static_cast<uint64_t>(rate * 10);
        }
      }
      uint64_t next_progress = static_cast<uint64_t>(rate * 10);
      while (continuity.total < requested) {
        auto *buffer = writer.available();
        const size_t wanted = std::min<uint64_t>(65536, requested - continuity.total);
        uhd::rx_metadata_t md;
        const size_t n = rx->recv(buffer, wanted, md, continuity.total ? .5 : 3.0, false);
        const bool ok = md.error_code == uhd::rx_metadata_t::ERROR_CODE_NONE && md.has_time_spec && !md.out_of_sequence;
        if (!ok) { ++metadata_errors; throw std::runtime_error("RX metadata: " + md.strerror()); }
        const int64_t tick = md.time_spec.to_ticks(rate);
        if (!continuity.total && skip_samples)
          require(tick == skipped.first + static_cast<int64_t>(skip_samples), "discontinuity at skip-to-capture boundary");
        const uint64_t offset = continuity.total;
        continuity.observe(tick, n, ok);
        records.push_back({offset, tick, n}); writer.publish(n);
        if (continuity.total >= next_progress) {
          std::cout << "PROGRESS sample_seconds=" << continuity.total / rate << " written_samples="
                    << writer.written.load() << " max_backlog_blocks=" << writer.maximum_backlog << std::endl;
          next_progress += static_cast<uint64_t>(rate * 10);
        }
      }
    } catch (const std::exception &e) { error = e.what(); }
    if (rx) {
      try {
        uhd::stream_cmd_t stop(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        stop.stream_now = true; rx->issue_stream_cmd(stop);
        std::vector<Sample> tail(65536);
        for (int i = 0; i < 16; ++i) {
          uhd::rx_metadata_t md;
          if (!rx->recv(tail.data(), tail.size(), md, .02, false)) break;
        }
      } catch (const std::exception &e) { if (error.empty()) error = std::string("stop: ") + e.what(); }
    }
    rx.reset(); usrp.reset();
    std::cout << "RF_RELEASED samples_received=" << continuity.total << std::endl;
    try { writer.finish(); } catch (const std::exception &e) { if (error.empty()) error = e.what(); }
    bool complete = error.empty() && continuity.total == requested && writer.written == requested;
    std::ostringstream journal;
    journal << "offset_samples\tfirst_tick\tsamples\n";
    for (auto r : records) journal << r.offset << '\t' << r.tick << '\t' << r.count << '\n';
    exclusive_text(out + "/timestamps.tsv", journal.str());
    std::ostringstream info;
    info << std::setprecision(17) << "{\n\"recorder\":\"bounded_stream_v1\",\"format\":\"sc16_le\",\"channels\":1,"
         << "\n\"samples_per_channel\":" << writer.written.load() << ",\"requested_samples_per_channel\":" << requested
         << ",\"sample_rate_hz\":" << rate << ",\"duration_s\":" << writer.written / rate
         << ",\"first_sample_tick\":" << continuity.first << ",\"timestamp_tick_rate_hz\":" << rate
         << ",\"blocks\":" << records.size() << ",\"rx_metadata_errors\":" << metadata_errors
         << ",\"skipped_samples_before_capture\":" << skip_samples
         << ",\"skipped_duration_s\":" << skip_seconds
         << ",\"stream_duration_s\":" << writer.written / rate + skip_seconds
         << ",\"ring_gib\":" << ring_gib
         << ",\"max_disk_backlog_blocks\":" << writer.maximum_backlog
         << ",\"clock_source\":\"internal\",\"time_source\":\"internal\",\"external_cell_synchronization\":false,\"receiver_corrections_applied\":false,"
         << "\n\"rf_channels\":[{\"channel\":0,\"physical_channel\":" << channel << ",\"frequency_hz\":" << actual_frequency
         << ",\"rate_hz\":" << actual_rate << ",\"gain_db\":" << actual_gain << ",\"bandwidth_hz\":" << actual_bw << "}],"
         << "\n\"status\":\"" << (complete ? "COMPLETE_CONTIGUOUS_RAW_IQ" : "VOID") << "\"\n}\n";
    exclusive_text(out + "/capture_info.json", info.str());
    if (!writer.sha256.empty()) exclusive_text(out + "/IQ_SHA256SUMS", writer.sha256 + "  rx0.sc16\n");
    if (!error.empty()) exclusive_text(out + "/failure.txt", error + "\n");
    std::cout << (complete ? "COMPLETE" : "VOID") << " samples=" << writer.written << " seconds=" << writer.written / rate
              << " sha256=" << writer.sha256 << " reason=" << error << std::endl;
    return complete ? 0 : 2;
  } catch (const std::exception &e) { std::cerr << "VOID: " << e.what() << '\n'; return 2; }
}
