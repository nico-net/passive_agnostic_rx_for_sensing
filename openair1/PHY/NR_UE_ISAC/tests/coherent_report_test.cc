// openair1/PHY/NR_UE_ISAC/tests/coherent_report_test.cc
#include "coherent_report.h"
#include "coherent_types.h"
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
static size_t lines(const std::string& p) { std::ifstream f(p); std::string s; size_t n = 0; while (std::getline(f, s)) ++n; return n; }
int main() {
  using namespace nr_isac::coherent;
  Volume v;
  require(parse_volume("-15:15:-15:15:0:30", &v) && v.x0 == -15 && v.z1 == 30, "parse ok");
  require(!parse_volume("1:2:3", &v), "short rejected");
  require(!parse_volume("5:1:0:1:0:1", &v), "inverted rejected");
  const std::string dir = "/tmp/coherent_report_test_" + std::to_string(getpid());
  std::string cmd = "mkdir -p " + dir; require(std::system(cmd.c_str()) == 0, "mkdir");
  std::string p1, p2;
  { JsonlSink a(dir, "coherent_reports"); a.write_line("{\"n\":1}"); p1 = a.path(); }
  { JsonlSink b(dir, "coherent_reports"); b.write_line("{\"n\":2}"); p2 = b.path(); }
  require(p1 != p2, "two sinks in the same second get distinct files");
  require(lines(p1) == 1 && lines(p2) == 1, "each file keeps its own line");
  { std::FILE* f = std::fopen(p1.c_str(), "a"); std::fputs("{\"n\":3}\n", f); std::fclose(f); }
  require(lines(p1) == 2, "file is appendable, never truncated");
  require(Vec3{} .x == 0, "vec3");
  const Vec3 tx{10, 0, 0}, rx{0, 0, 0};
  require(std::abs(excess_delay_s(rx, tx, rx)) < 1e-15, "voxel at rx: zero excess");
  require(excess_delay_s(Vec3{0, 5, 0}, tx, rx) > 0, "off-baseline voxel: positive excess");
  std::puts("coherent_report_test: PASS");
  return 0;
}
