/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_report.h"
#include "coherent_types.h"
#include <atomic>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
namespace nr_isac::coherent {
std::string unique_stamp()
{
  static std::atomic<unsigned> counter{0};
  std::time_t t = std::time(nullptr); std::tm tm{}; gmtime_r(&t, &tm);
  char buf[32]; std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%S", &tm);
  return std::string(buf) + "_" + std::to_string(getpid()) + "_" + std::to_string(counter++);
}
JsonlSink::JsonlSink(const std::string& dir, const std::string& stem)
    : path_((dir.empty() ? std::string(".") : dir) + "/" + stem + "." + unique_stamp() + ".jsonl")
{
  f_ = std::fopen(path_.c_str(), "a");   // append: never truncates
  if (!f_) throw std::runtime_error("cannot open " + path_);
}
JsonlSink::~JsonlSink() { if (f_) std::fclose(f_); }
void JsonlSink::write_line(const std::string& json)
{
  std::lock_guard<std::mutex> lock(mu_);
  std::fputs(json.c_str(), f_); std::fputc('\n', f_); std::fflush(f_);
}
bool parse_volume(const std::string& text, Volume* out)
{
  double v[6]; char c; std::istringstream is(text);
  for (int i = 0; i < 6; ++i) { if (!(is >> v[i])) return false; if (i < 5 && !(is >> c && c == ':')) return false; }
  if (!(v[0] < v[1] && v[2] < v[3] && v[4] < v[5])) return false;
  *out = Volume{v[0], v[1], v[2], v[3], v[4], v[5]};
  return true;
}
} // namespace nr_isac::coherent
