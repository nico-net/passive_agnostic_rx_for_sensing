/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <cstdio>
#include <mutex>
#include <string>
namespace nr_isac::coherent {
/** "<UTC yyyymmddTHHMMSS>_<pid>_<n>" -- unique per process and per sink. */
std::string unique_stamp();
/** Append-only JSONL file "<dir>/<stem>.<unique_stamp()>.jsonl"; one line per write, flushed. */
class JsonlSink {
public:
  JsonlSink(const std::string& dir, const std::string& stem);
  ~JsonlSink();
  JsonlSink(const JsonlSink&) = delete; JsonlSink& operator=(const JsonlSink&) = delete;
  void write_line(const std::string& json);
  const std::string& path() const { return path_; }
private:
  std::string path_; std::FILE* f_ = nullptr; std::mutex mu_;
};
} // namespace nr_isac::coherent
