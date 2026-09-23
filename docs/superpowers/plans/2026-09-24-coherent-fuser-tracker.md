# Coherent Fuser + Tracker Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Treat the 4 phase-coherent X410 channels as one array: common-mode sync, per-channel self-calibration, range-Doppler, two-pass 3D focusing, CFAR detection and a 3D tracker, GPU-resident within the 75 ms CPI, wired to the monitor — alongside (not replacing) the existing pipeline.

**Architecture:** A new `CoherentPipeline` is selected in `SensingEngine::process_window` when `coherent_enable = 1`; it consumes the raw 4-antenna `CfrWindow`s the engine already forms. A CPU reference (`coherent_core`) implements every DSP stage and is the test oracle and CUDA fallback; `coherent_cuda` implements the heavy stages on one stream. A C++ `CoherentTracker` runs per CPI. Outputs are append-only JSONL read by a new monitor page. Nothing existing is removed; stages 7–10 are only no longer launched.

**Tech Stack:** C++17, CUDA 12.4 (sm_89, g++-13 host, cuFFT, CUB), OAI CMake (`ENABLE_ISAC_SENSING`, `ENABLE_CHANNEL_SIM_CUDA`), Python 3 (numpy) for the generator/monitor, Plotly 2.35.2 (vendored).

**Spec:** `docs/superpowers/specs/2026-09-23-coherent-fuser-tracker-design.md` (read it before any task).

## Global Constraints

- Repo/worktree: sens6 `/home/sens/NICOLA/multirx-clean-adaptive`, branch `feature/multirx-clean-adaptive`; edit through the sshfs mount `/home/sens/mnt/sens6-multirx`, run git/cmake/ninja/tests over `ssh sens6`.
- Build dirs: `cmake_targets/ran_build/build_sense` (sensing ON + CUDA) and `cmake_targets/ran_build/build_rx` (sensing OFF). Runnable builds also need `ninja params_libconfig params_yaml coding dfts ldpc ldpc_cuda oai_usrpdevif rfsimulator`.
- Never build or benchmark while `/home/sens/NICOLA/.capture_active` exists or `pgrep -x nr-uesoftmodem` is non-empty. Never touch the radio.
- **No file is deleted.** Existing stages 1–10 stay compilable and selectable; `coherent_enable = 0` must reproduce current behaviour byte-for-byte on the existing offline chain test.
- **Golden rule — no tuned or calibrated constants.** Only: the tape survey (`spatial_rx_positions`, `tx_pos_*`), `coherent_survey_sigma_m` (declared tape accuracy, default 0.1), `coherent_volume_m`, `maximum_target_speed_mps`, `false_object_intensity_per_s`, and conventional statistical quantiles (χ² 99 % gate, SPRT α = 0.01, β = 0.10, Laplace prior 1/1 for P_D). Everything else is derived per CPI from the data or the allocation.
- **Allocation-aware:** no 273-PRB / 75 ms / 3.05 m / 1.14 m/s constants anywhere in new code.
- Carrier convention (verified): `CfrWindow::fc_hz` is the DL carrier centre (`nr_isac_carrier_t::dl_center_hz`); subcarrier `k` of the window is at baseband `f_k = (k − subcarriers/2)·scs_hz`.
- Namespace `nr_isac::coherent`; reuse `nr_isac::Vec3` ops (`small_matrix.h`), `nr_isac::fft_inplace` (`fft.h`, inverse divides by N), `nr_isac::median/quantile` (`robust_stats.h`), `nr_isac::slot_duration_s` (`pipeline_types.h`).
- Style: match surrounding OAI style; `LOG_I/LOG_W/LOG_E(PHY, …)` only in `nr_isac.cc`/`coherent_pipeline.cc` (core, tracker, autofocus, UL, report must link without the OAI logger).
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Tests are plain executables with a `require()` helper (same pattern as `tests/flow_gate_test.cc`), registered next to `test_nr_isac_flow_gate` in the root `CMakeLists.txt` (`add_executable`, `add_dependencies(tests …)`, `add_test`).
- Logs are append-only; no new code opens an output file with truncation.

## Review Focus

1. **Degenerate CPIs** (0/1 row, a row with 1 PRB, all rows identical time) → `derive_axes` returns `valid=false` with a reason; the pipeline counts `skipped_cpis[reason]` and never divides by zero. Pinned in Task 3.
2. **LOS missing on a channel** (blocked antenna) → calibration predicts, `c_i` falls, no NaN in weights or reports. Pinned in Task 4.
3. **Voxels whose excess delay exceeds the cropped range axis / volume edges** → never read out of bounds (CPU and CUDA). Pinned in Tasks 5 and 10.
4. **Detection bursts** (hundreds of detections in one CPI) → tracker association stays bounded and fast. Pinned in Task 6.
5. **Two engine processes started in the same second / a retry** → distinct files, appended, never truncated. Pinned in Task 1.

---

## File map

| File | Responsibility |
|---|---|
| `openair1/PHY/NR_UE_ISAC/coherent_types.h` | config, axes, geometry, calibration, detection, track structs; tiny geometry helpers |
| `openair1/PHY/NR_UE_ISAC/coherent_report.{h,cc}` | append-only JSONL writers with unique per-process filenames; JSON encoding |
| `openair1/PHY/NR_UE_ISAC/coherent_core.{h,cc}` | CPU reference: axes, LOS, row sync, range-Doppler, calibrator, grid, envelope, CFAR, NMS, harmonic merge, refinement |
| `openair1/PHY/NR_UE_ISAC/coherent_tracker.{h,cc}` | 3D CV Kalman, Hungarian GNN, adaptive Q, SPRT existence |
| `openair1/PHY/NR_UE_ISAC/coherent_autofocus.{h,cc}` | online antenna-position refinement from confirmed-track phase residuals |
| `openair1/PHY/NR_UE_ISAC/coherent_ul.{h,cc}` | UE direct-path TDOA localisation + UE-illuminated focusing (built, off) |
| `openair1/PHY/NR_UE_ISAC/coherent_cuda.{h,cu}`, `coherent_cuda_stub.cc` | GPU C3/C5/C6 on one stream; stub reports unavailable |
| `openair1/PHY/NR_UE_ISAC/coherent_pipeline.{h,cc}` | worker thread, depth-2 queue, orchestration, timers, reports |
| `openair1/PHY/NR_UE_ISAC/tests/coherent_*_test.cc` | unit/integration tests |
| `openair1/PHY/NR_UE_ISAC/tools/make_coherent_scene.py` (+ `test_make_coherent_scene.py`) | synthetic 4-channel scene generator (rows.bin + truth + perturbed survey) |
| `openair1/PHY/NR_UE_ISAC/tools/test_coherent_chain.sh` | end-to-end offline + real-time test |
| `tests/passive_rx/ota/sensing_coherent_synth.conf` | synthetic coherent conf |
| `tests/passive_rx/monitor/coherent_view.py`, `coherent.html` | monitor coherent page |
| Modified: `CMakeLists.txt`, `pipeline_types.h`, `nr_isac.cc`, `sensing_engine.{h,cc}`, `tests/passive_rx/run_sensing.sh`, `tests/passive_rx/monitor/monitor.py`, `tests/passive_rx/ota/sensing_ota_manual.conf.template` | additive only |

---

### Task 1: Types, config keys, append-only report writer, engine switch

**Files:**
- Create: `openair1/PHY/NR_UE_ISAC/coherent_types.h`, `openair1/PHY/NR_UE_ISAC/coherent_report.h`, `openair1/PHY/NR_UE_ISAC/coherent_report.cc`, `openair1/PHY/NR_UE_ISAC/tests/coherent_report_test.cc`
- Modify: `openair1/PHY/NR_UE_ISAC/pipeline_types.h` (add `coherent` member to `PipelineConfig`), `openair1/PHY/NR_UE_ISAC/nr_isac.cc` (4 config keys), `CMakeLists.txt` (sources + test)

**Interfaces:**
- Produces: all structs below; `bool parse_volume(const std::string&, Volume*)`; `double excess_delay_s(const Vec3& x, const Vec3& tx, const Vec3& rx)`; `class JsonlSink { JsonlSink(const std::string& dir, const std::string& stem); void write_line(const std::string&); const std::string& path() const; }`; `std::string unique_stamp()`; `PipelineConfig::coherent` of type `coherent::CoherentConfig`.

- [ ] **Step 1: Write `coherent_types.h`**

```cpp
/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>
#include "small_matrix.h"

namespace nr_isac::coherent {

using cd = std::complex<double>;
using cf = std::complex<float>;
constexpr double kC = 299792458.0;
constexpr uint32_t kCh = 4;

struct Volume { double x0 = -15, x1 = 15, y0 = -15, y1 = 15, z0 = 0, z1 = 30; };
struct Geometry { std::array<Vec3, kCh> rx{}; Vec3 tx{}; };

struct CoherentConfig {
  bool enable = false;
  bool ul_enable = false;
  Volume volume;
  double survey_sigma_m = 0.1;               // declared tape accuracy (input, not tuned)
  double max_speed_mps = 0.0;                // = PipelineConfig::maximum_target_speed_mps
  double false_object_intensity_per_s = 0.0; // = PipelineConfig::false_object_intensity_per_s
  Geometry geometry;                         // surveyed: spatial_rx_positions + tx_pos_*
  std::string out_dir;                       // directory of report_path
  double monitor_period_s = 0.5;             // = rvm_period_s
};

/** Per-CPI axes, all derived from the CPI's own allocation and row times. */
struct Axes {
  bool valid = false;
  std::string invalid_reason;
  double fc_hz = 0, lambda_m = 0, scs_hz = 0;
  uint32_t subcarriers = 0;  // window grid width
  uint32_t n_fft = 0;        // zero-padded range FFT length
  double delay_step_s = 0;   // 1/(n_fft*scs)
  uint32_t n_range = 0;      // cropped range bins (bin 0 = own LOS)
  double b_eff_hz = 0;       // median observed bandwidth per row
  uint32_t n_dopp = 0;       // Doppler bins
  double dopp_step_hz = 0, dopp0_hz = 0;  // bin d frequency = dopp0_hz + d*dopp_step_hz
  double t_cpi_s = 0, median_dt_s = 0;
  uint32_t notch_half_bins = 2;           // Hann mainlobe half-width (window property)
  std::vector<uint32_t> tested_dopp;      // bins outside the notch and within 2*v_max/lambda
  std::vector<double> row_t_s;            // row times relative to the first row
};

struct Calibration {
  std::array<double, kCh> phase_rad{};      // posterior per-channel phase (ch0 = 0)
  std::array<double, kCh> phase_var{};      // posterior phase variance (rad^2)
  std::array<double, kCh> coh_factor{};     // exp(-phase_var/2)
  std::array<double, kCh> los_snr{};        // linear
  std::array<double, kCh> jitter_rad{};     // sqrt(innovation variance)
  std::array<double, kCh> jitter_bound_rad{}; // 1/sqrt(2*SNR)
  std::array<bool, kCh> los_found{};
  double coherent_gain = 1.0;               // predicted-phase LOS coherent gain in [1,kCh]
  double rho = 0.0;                         // (G-1)/(kCh-1) clipped to [0,1]
};

struct Detection {
  Vec3 pos;                 // final position (rho-blended)
  Vec3 pos_env;             // envelope-pass position
  Vec3 pos_sigma;           // 1-sigma per axis
  double doppler_hz = 0;
  double range_rate_mps = 0;        // bistatic path-length rate = -lambda*doppler
  double range_rate_sigma = 0;
  double snr = 0;                   // linear, per channel equivalent
  uint32_t dopp_bin = 0;
  bool refined = false;
  std::array<cd, kCh> terms{};      // per-channel coherent terms at pos (autofocus input)
  uint64_t illuminator = 0;         // 0 = gNB, else UL session id
  Vec3 tx;                          // illuminator position used
};

struct Track {
  uint64_t id = 0;
  std::array<double, 6> x{};        // x y z vx vy vz
  std::array<double, 36> P{};       // row-major 6x6
  double llr = 0;                   // SPRT log-likelihood ratio
  double q = 0;                     // white-acceleration PSD (adaptive)
  double nis_sum = 0; uint32_t nis_n = 0;
  uint32_t hits = 0, misses = 0;
  double age_s = 0;
  bool confirmed = false;
};

inline double dist(const Vec3& a, const Vec3& b) { return norm(a - b); }
/** Excess bistatic delay of x on the channel at rx, relative to that channel's direct path (s). */
inline double excess_delay_s(const Vec3& x, const Vec3& tx, const Vec3& rx)
{
  return (dist(x, tx) + dist(x, rx) - dist(tx, rx)) / kC;
}
bool parse_volume(const std::string& text, Volume* out);

} // namespace nr_isac::coherent
```

- [ ] **Step 2: Write the failing test `tests/coherent_report_test.cc`**

```cpp
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
```

- [ ] **Step 3: Register sources and the test in `CMakeLists.txt`**

Add to `NR_UE_ISAC_SRC` (after `nr_isac_env.c`, line ~1115):
```cmake
  ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/coherent_report.cc
```
Next to `test_nr_isac_flow_gate` (line ~2405):
```cmake
    add_executable(test_nr_isac_coherent_report ${OPENAIR1_DIR}/PHY/NR_UE_ISAC/tests/coherent_report_test.cc)
    target_include_directories(test_nr_isac_coherent_report PRIVATE ${OPENAIR1_DIR}/PHY/NR_UE_ISAC)
    target_link_libraries(test_nr_isac_coherent_report PRIVATE NR_UE_ISAC)
    add_dependencies(tests test_nr_isac_coherent_report)
    add_test(NAME test_nr_isac_coherent_report COMMAND ./test_nr_isac_coherent_report)
```

- [ ] **Step 4: Build to verify it fails**

Run: `ssh sens6 'cd /home/sens/NICOLA/multirx-clean-adaptive/cmake_targets/ran_build/build_sense && cmake . >/dev/null && ninja test_nr_isac_coherent_report 2>&1 | tail -3'`
Expected: FAIL — `coherent_report.h: No such file or directory`.

- [ ] **Step 5: Write `coherent_report.h` / `.cc`**

```cpp
/* coherent_report.h */
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
```
```cpp
/* coherent_report.cc */
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
```

- [ ] **Step 6: Add the config member and keys**

In `pipeline_types.h` add `#include "coherent_types.h"` and, inside `struct PipelineConfig` (after `rvm_max_range_m`):
```cpp
  coherent::CoherentConfig coherent;   // coherent fuser (coherent_enable); default off
```
In `nr_isac.cc` `nr_isac_init()`: declare `int p_coh=0,p_coh_ul=0; double p_coh_sigma=0.1; char* p_coh_vol=nullptr;` and add to `params[]` (next to `gate_require_ul`):
```cpp
    integer("coherent_enable","1 = coherent array fuser/tracker replaces per-receiver stages 1-6",PARAMFLAG_BOOL,&p_coh,0),
    integer("coherent_ul_enable","1 = also UL-illuminated focusing (built, default off)",PARAMFLAG_BOOL,&p_coh_ul,0),
    real("coherent_survey_sigma_m","declared antenna/gNB survey accuracy (m)",&p_coh_sigma,0.1),
    text("coherent_volume_m","surveillance volume xmin:xmax:ymin:ymax:zmin:zmax (m, ENU)",&p_coh_vol,"-15:15:-15:15:0:30"),
```
After the existing geometry/report-path assignments to `pipeline` (where `pipeline.tx_position`, `pipeline.spatial_receivers`, `pipeline.report_path`, `pipeline.rvm_period_s` are set), add:
```cpp
  pipeline.coherent.enable = p_coh != 0;
  pipeline.coherent.ul_enable = p_coh_ul != 0;
  pipeline.coherent.survey_sigma_m = p_coh_sigma;
  if (!nr_isac::coherent::parse_volume(p_coh_vol ? p_coh_vol : "", &pipeline.coherent.volume)) {
    LOG_E(PHY, "SENSING: malformed coherent_volume_m '%s'; coherent path disabled\n", p_coh_vol ? p_coh_vol : "");
    pipeline.coherent.enable = false;
  }
  pipeline.coherent.max_speed_mps = pipeline.maximum_target_speed_mps;
  pipeline.coherent.false_object_intensity_per_s = pipeline.false_object_intensity_per_s;
  pipeline.coherent.geometry.tx = pipeline.tx_position;
  for (uint32_t i = 0; i < 4; ++i) pipeline.coherent.geometry.rx[i] = pipeline.spatial_receivers.positions[i];
  pipeline.coherent.monitor_period_s = pipeline.rvm_period_s;
  { const std::string rp = pipeline.report_path; const size_t s = rp.rfind('/');
    pipeline.coherent.out_dir = (s == std::string::npos) ? "." : rp.substr(0, s); }
  if (pipeline.coherent.enable && !pipeline.spatial_receivers.configured) {
    LOG_E(PHY, "SENSING: coherent_enable needs spatial_rx_positions (4 antennas); coherent path disabled\n");
    pipeline.coherent.enable = false;
  }
  if (pipeline.coherent.enable) LOG_I(PHY, "SENSING: coherent fuser enabled (ul=%d)\n", (int)pipeline.coherent.ul_enable);
```
(Use the actual local variable name the file uses for the `PipelineConfig` it fills — it is `pipeline` at line ~311; verify.)

- [ ] **Step 7: Build and run the test**

Run: `ssh sens6 'cd .../build_sense && ninja test_nr_isac_coherent_report nr-uesoftmodem 2>&1 | grep -E "error|FAILED"; ./test_nr_isac_coherent_report'`
Expected: `coherent_report_test: PASS`, no build errors. Also `ninja -C ../build_rx nr-uesoftmodem` succeeds.

- [ ] **Step 8: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_types.h openair1/PHY/NR_UE_ISAC/coherent_report.h openair1/PHY/NR_UE_ISAC/coherent_report.cc openair1/PHY/NR_UE_ISAC/tests/coherent_report_test.cc openair1/PHY/NR_UE_ISAC/pipeline_types.h openair1/PHY/NR_UE_ISAC/nr_isac.cc CMakeLists.txt
git commit -m "sensing: coherent fuser types, config keys and append-only JSONL sinks"
```

---

### Task 2: Synthetic coherent scene generator

**Files:**
- Create: `openair1/PHY/NR_UE_ISAC/tools/make_coherent_scene.py`, `openair1/PHY/NR_UE_ISAC/tools/test_make_coherent_scene.py`, `openair1/PHY/NR_UE_ISAC/tools/survey_coherent_square.json`

**Interfaces:**
- Consumes: the `CFR1` record layout of `make_synthetic_rows.py` (import its `header()`; copy its payload layout exactly: interleaved float32 `[ant][2*re]`, then `uint32` subcarrier indices `[re]`, then `uint32` per-RE tag `[re]` = 2).
- Produces: CLI `make_coherent_scene.py --out ROWS --truth TRUTH --survey-out SURVEY [--seconds S] [--square 5|10] [--scramble-phases]`; `rows.bin`; `truth.json` = `{"phases_rad":[4], "targets":[{"name","p0":[3],"v":[3]}], "geometry":{"gnb":[3],"rx":[[3]x4]}}`; `survey-out` = survey JSON (same schema as `survey_synth.json`) with every position perturbed by N(0, 0.05 m) per axis (tape error, fixed seed).

- [ ] **Step 1: Write the failing test `tools/test_make_coherent_scene.py`**

```python
#!/usr/bin/env python3
"""Self-test for make_coherent_scene.py: record structure, DL-only TDD cadence, irregular allocations,
and physics (per-channel LOS-referenced range profile peaks at the target's excess delay)."""
import json, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
HERE = Path(__file__).resolve().parent
C = 299792458.0

def records(path):
    b = open(path, "rb").read(); i = 0; out = []
    while i < len(b):
        assert b[i:i + 4] == b"CFR1"
        kind = struct.unpack_from("<I", b, i + 4)[0]; ant, re_ = struct.unpack_from("<II", b, i + 40)
        n = 68 + (0 if kind else 8 * ant * re_ + 8 * re_)
        hdr = b[i:i + 68]; pay = b[i + 68:i + n]; out.append((kind, hdr, pay, ant, re_)); i += n
    return out

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    subprocess.check_call([sys.executable, str(HERE / "make_coherent_scene.py"), "--out", str(d / "rows.bin"),
                           "--truth", str(d / "truth.json"), "--survey-out", str(d / "survey.json"), "--seconds", "0.5"])
    tr = json.load(open(d / "truth.json")); sv = json.load(open(d / "survey.json"))
    rec = [r for r in records(d / "rows.bin") if r[0] == 0]
    assert len(rec) > 100, len(rec)
    widths = {r[4] for r in rec}; assert len(widths) > 5, "allocations must vary per row"
    assert all(r[3] == 4 for r in rec)
    g = np.array(tr["geometry"]["gnb"]); rx = [np.array(p) for p in tr["geometry"]["rx"]]
    rx_s = [np.array(sv["rx_antennas_m"][f"ch{i}"]) for i in range(4)]
    err = max(np.linalg.norm(a - b) for a, b in zip(rx, rx_s)); assert 0.01 < err < 0.4, err
    # physics on the widest row: LOS-referenced profile of channel 0 peaks near 0 excess delay
    kind, hdr, pay, ant, re_ = max(rec, key=lambda r: r[4])
    iq = np.frombuffer(pay[:8 * ant * re_], np.float32).reshape(ant, 2 * re_)
    h = iq[:, 0::2] + 1j * iq[:, 1::2]
    k = np.frombuffer(pay[8 * ant * re_:8 * ant * re_ + 4 * re_], np.uint32).astype(float)
    f = (k - 273 * 6) * 30000.0
    d_los = np.linalg.norm(rx[0] - g) / C
    taus = np.arange(-50, 400) * 1e-9
    prof = np.abs(np.exp(2j * np.pi * np.outer(taus, f)) @ h[0])
    assert abs(taus[np.argmax(prof)] - d_los) < 20e-9, (taus[np.argmax(prof)], d_los)
print("test_make_coherent_scene: PASS")
```

- [ ] **Step 2: Run it to verify it fails**

Run: `ssh sens6 'cd /home/sens/NICOLA/multirx-clean-adaptive/openair1/PHY/NR_UE_ISAC/tools && python3 test_make_coherent_scene.py'`
Expected: FAIL (`make_coherent_scene.py` missing).

- [ ] **Step 3: Write `survey_coherent_square.json` and `make_coherent_scene.py`**

```json
{"frame": "ENU metres, origin at the X410 (synthetic 10 m checkerboard square)",
 "gnb_m": [35.0, 20.0, 6.0],
 "rx_antennas_m": {"ch0": [0.0, 0.0, 0.5], "ch1": [10.0, 0.0, 3.5], "ch2": [0.0, 10.0, 3.5], "ch3": [10.0, 10.0, 0.5]},
 "max_range_m": 120}
```
```python
#!/usr/bin/env python3
# openair1/PHY/NR_UE_ISAC/tools/make_coherent_scene.py
"""4-channel phase-coherent CFR rows for the coherent fuser tests: one LO (common CFO/phase noise),
per-channel constant phase offsets, identical cables (+ sub-ns residuals), DL-only TDD cadence with
irregular grants and per-row allocations, a static wall reflection of the gNB that is STRONGER than
the LOS on channel 2, a person, a car and a drone (with a ground bounce), and a tape-perturbed survey."""
import argparse, json
import numpy as np
from make_synthetic_rows import header

C = 299792458.0
PRB, SCS, FC, SPF = 273, 30000, 3450000000, 20

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True); ap.add_argument("--truth", required=True)
    ap.add_argument("--survey-out", required=True); ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--square", type=float, default=10.0); ap.add_argument("--snr-db", type=float, default=20.0)
    ap.add_argument("--scramble-phases", action="store_true", help="random phase per channel per ROW (no coherence)")
    a = ap.parse_args()
    rng = np.random.default_rng(11)
    s = a.square
    rx = [np.array(p) for p in ([0, 0, 0.5], [s, 0, 3.5], [0, s, 3.5], [s, s, 0.5])]
    gnb = np.array([35.0, 20.0, 6.0])
    phases = rng.uniform(-np.pi, np.pi, 4); phases[0] = 0.0
    resid_delay = rng.uniform(-0.5e-9, 0.5e-9, 4)                  # identical cables, sub-ns residuals
    wall_gnb = gnb * np.array([1, 1, 1]); wall_gnb[1] = 2 * 30.0 - gnb[1]   # mirror of gNB in plane y = 30 m
    targets = [dict(name="person", p0=[4.0, 18.0, 1.0], v=[0.0, -1.2, 0.0], a=0.25),
               dict(name="car",    p0=[-12.0, 5.0, 0.8], v=[8.0, 0.0, 0.0], a=0.6),
               dict(name="drone",  p0=[8.0, 8.0, 15.0], v=[-3.0, 2.0, 0.5], a=0.12)]
    k_all = np.arange(PRB * 12, dtype=np.uint32)
    f_all = (k_all.astype(float) - PRB * 6) * SCS
    lam = C / FC; sigma = 10 ** (-a.snr_db / 20); cfo_hz = 37.0
    def path(i, p_from, p_to_rx, amp, f):
        tau = (np.linalg.norm(p_from - gnb) if p_from is not None else 0.0)
        return tau
    with open(a.out, "wb") as out:
        for n in range(int(a.seconds / 0.0005)):
            t = n * 0.0005
            if (n % 10) >= 7 or rng.random() > 0.6:                # DL slots of a 7D pattern, 60 % grant probability
                continue
            nprb = int(rng.choice([4, 8, 16, 32, 64, 128, 200, 273])); start = int(rng.integers(0, PRB - nprb + 1))
            k = k_all[start * 12:(start + nprb) * 12]; f = f_all[start * 12:(start + nprb) * 12]
            common = np.exp(2j * np.pi * cfo_hz * t) * np.exp(1j * rng.normal(0, 0.05))   # one LO for all channels
            h = np.empty((4, k.size), np.complex64)
            for i in range(4):
                taus, amps = [], []
                taus.append(np.linalg.norm(rx[i] - gnb) / C); amps.append(1.0 if i != 2 else 0.3)   # LOS weak on ch2
                taus.append((np.linalg.norm(wall_gnb - rx[i])) / C); amps.append(0.6 if i == 2 else 0.2)  # static wall
                for tg in targets:
                    p = np.array(tg["p0"]) + np.array(tg["v"]) * t
                    taus.append((np.linalg.norm(p - gnb) + np.linalg.norm(rx[i] - p)) / C); amps.append(tg["a"])
                    if tg["name"] == "drone":                        # ground bounce: image at -z, reflection -0.5
                        pm = p * np.array([1, 1, -1])
                        taus.append((np.linalg.norm(pm - gnb) + np.linalg.norm(rx[i] - pm)) / C); amps.append(-0.5 * tg["a"])
                acc = np.zeros(k.size, complex)
                for tau, amp in zip(taus, amps):
                    acc += amp * np.exp(-2j * np.pi * (FC + f) * (tau + resid_delay[i]))
                ph = rng.uniform(-np.pi, np.pi) if a.scramble_phases else phases[i]
                noise = sigma * (rng.standard_normal(k.size) + 1j * rng.standard_normal(k.size)) / np.sqrt(2)
                h[i] = (acc * common * np.exp(1j * ph) + noise).astype(np.complex64)
            inter = np.empty((4, 2 * k.size), np.float32); inter[:, 0::2] = h.real; inter[:, 1::2] = h.imag
            out.write(header(0, n % (1024 * SPF), 0.0, 3, PRB, SCS, FC, 2, SPF, 4, k.size, sigma ** 2, 0, int(t * 1e9)))
            out.write(inter.tobytes()); out.write(k.tobytes()); out.write(np.full(k.size, 2, np.uint32).tobytes())
    tape = np.random.default_rng(3)
    pert = lambda p: (np.array(p) + tape.normal(0, 0.05, 3)).round(3).tolist()
    json.dump({"frame": "ENU m, origin at the X410 (tape-perturbed)", "gnb_m": pert(gnb),
               "rx_antennas_m": {f"ch{i}": pert(rx[i]) for i in range(4)}, "max_range_m": 120},
              open(a.survey_out, "w"))
    json.dump({"phases_rad": phases.tolist(), "cfo_hz": cfo_hz,
               "targets": [{"name": t_["name"], "p0": t_["p0"], "v": t_["v"]} for t_ in targets],
               "geometry": {"gnb": gnb.tolist(), "rx": [r.tolist() for r in rx]}}, open(a.truth, "w"))

if __name__ == "__main__":
    main()
```
(Remove the unused `path()` helper if the linter complains; it is not referenced.)

- [ ] **Step 4: Run the test to verify it passes**

Run: `ssh sens6 'cd .../NR_UE_ISAC/tools && python3 test_make_coherent_scene.py'`
Expected: `test_make_coherent_scene: PASS`.

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/tools/make_coherent_scene.py openair1/PHY/NR_UE_ISAC/tools/test_make_coherent_scene.py openair1/PHY/NR_UE_ISAC/tools/survey_coherent_square.json
git commit -m "sensing: synthetic phase-coherent 4-channel scene generator"
```

---

### Task 3: CPU reference — axes, LOS, common row sync, range-Doppler

**Files:**
- Create: `openair1/PHY/NR_UE_ISAC/coherent_core.h`, `openair1/PHY/NR_UE_ISAC/coherent_core.cc`, `openair1/PHY/NR_UE_ISAC/tests/coherent_core_test.cc`
- Modify: `CMakeLists.txt` (add `coherent_core.cc` to `NR_UE_ISAC_SRC`; test `test_nr_isac_coherent_core` exactly like Task 1's test block)

**Interfaces:**
- Consumes: `CfrWindow` (`pipeline_types.h`), Task 1 types.
- Produces:
  ```cpp
  Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps);
  struct LosEstimate { std::array<double,kCh> delay_s{}; std::array<cd,kCh> tap{}; std::array<double,kCh> snr{}; std::array<bool,kCh> found{}; };
  LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa);
  struct RowSync { std::vector<double> phase_rad; std::vector<double> delay_s; };
  RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& los);
  struct RdResult { RangeDoppler rd; std::array<cd,kCh> los_tap{}; std::array<double,kCh> noise{}; };
  RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& los, const RowSync& sync);
  struct RangeDoppler { Axes axes; std::vector<cf> v; size_t idx(uint32_t ch,uint32_t r,uint32_t d) const; };  // [ch][range][dopp]
  double gamma_upper_quantile(uint32_t shape, double p);   // x with Q(shape,x) = p, integer shape
  ```

- [ ] **Step 1: Write the failing test `tests/coherent_core_test.cc`** (single-path synthetic windows built in-test)

```cpp
// openair1/PHY/NR_UE_ISAC/tests/coherent_core_test.cc
#include "coherent_core.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;

// rows x 3276 grid, antennas 4; paths: (per-channel absolute delay, amplitude, doppler); channel phase offsets
struct Path { std::array<double, kCh> tau; double amp; double doppler_hz; };
static CfrWindow make_window(uint32_t rows, const std::vector<Path>& paths, std::array<double, kCh> ph,
                             double cfo_hz, uint32_t nprb_row, uint32_t seed)
{
  CfrWindow w; w.antennas = 4; w.rows = rows; w.subcarriers = 273 * 12; w.scs_hz = 30000; w.fc_hz = 3.45e9; w.pci = 2;
  w.values.assign((size_t)4 * rows * w.subcarriers, {0, 0}); w.observed.assign((size_t)rows * w.subcarriers, 0);
  std::mt19937 rng(seed); std::normal_distribution<double> n01(0, 1);
  const double slot = slot_duration_s(w.scs_hz);
  for (uint32_t r = 0; r < rows; ++r) {
    w.row_time_slots.push_back(r * 2.0);          // every 2nd slot
    w.row_slot_idx.push_back(r * 2); w.row_slot_frac.push_back(0); w.row_source_mask.push_back(1u << 3);
    const double t = r * 2.0 * slot;
    const uint32_t start = (r * 37) % (273 - nprb_row + 1);
    for (uint32_t k = start * 12; k < (start + nprb_row) * 12; ++k) {
      w.observed[w.cell(r, k)] = 1;
      const double f = ((double)k - 273 * 6) * w.scs_hz;
      for (uint32_t a = 0; a < 4; ++a) {
        std::complex<double> acc = 0;
        for (const Path& p : paths)
          acc += p.amp * std::exp(std::complex<double>(0, -2 * M_PI * ((w.fc_hz + f) * p.tau[a] - p.doppler_hz * t)));
        acc *= std::exp(std::complex<double>(0, ph[a] + 2 * M_PI * cfo_hz * t));
        acc += 0.01 * std::complex<double>(n01(rng), n01(rng));
        w.values[w.sample(a, r, k)] = std::complex<float>(acc);
      }
    }
  }
  return w;
}

int main() {
  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol;
  // --- degenerate CPI (Review Focus 1)
  { CfrWindow w = make_window(1, {}, {0, 0, 0, 0}, 0, 4, 1);
    Axes a = derive_axes(w, vol, g, 10.0); require(!a.valid && !a.invalid_reason.empty(), "1-row CPI rejected with reason"); }
  // --- one LOS per channel + a moving target
  const Vec3 tgt{5, 15, 1.5};
  std::array<double, kCh> los{}, tt{};
  for (uint32_t i = 0; i < 4; ++i) { los[i] = (dist(g.tx, g.rx[i]) + 30.0) / kC; tt[i] = (dist(tgt, g.tx) + dist(tgt, g.rx[i]) + 30.0) / kC; }
  // +30 m common STO: the receiver's FFT window offset
  const double fd = 40.0;
  CfrWindow w = make_window(60, {{los, 1.0, 0.0}, {tt, 0.2, fd}}, {0, 1.1, -2.0, 0.4}, 23.0, 64, 2);
  Axes a = derive_axes(w, vol, g, 10.0);
  require(a.valid, "axes valid");
  require(std::abs(a.b_eff_hz - 64 * 12 * 30000.0) < 1, "b_eff = median observed bandwidth");
  require(a.n_dopp > 0 && a.dopp_step_hz > 0 && !a.tested_dopp.empty(), "doppler axis derived");
  LosEstimate L = find_los(w, a, 1e-4);
  for (uint32_t i = 0; i < 4; ++i) {
    require(L.found[i], "LOS found");
    require(std::abs(L.delay_s[i] - los[i]) < 0.25 * a.delay_step_s + 1e-12, "LOS delay sub-bin accurate");
  }
  RowSync s = estimate_row_sync(w, a, L);
  require(s.phase_rad.size() == w.rows && s.delay_s.size() == w.rows, "row sync sized");
  RdResult R = range_doppler(w, a, L, s);
  // target peak at its excess delay and doppler on every channel
  for (uint32_t i = 0; i < 4; ++i) {
    const double ex = excess_delay_s(tgt, g.tx, g.rx[i]);
    const uint32_t rb = (uint32_t)std::lround(ex / a.delay_step_s);
    uint32_t best_r = 0, best_d = 0; float best = 0;
    for (uint32_t r = 0; r < a.n_range; ++r) for (uint32_t d = 0; d < a.n_dopp; ++d) {
      const float m = std::abs(R.rd.v[R.rd.idx(i, r, d)]); if (m > best) { best = m; best_r = r; best_d = d; } }
    require(std::abs((int)best_r - (int)rb) <= 1, "target at its excess delay");
    const double fbest = a.dopp0_hz + best_d * a.dopp_step_hz;
    require(std::abs(fbest - fd) <= a.dopp_step_hz, "target at its doppler (CFO removed by row sync)");
  }
  // quantile helper
  const double x = gamma_upper_quantile(4, 1e-3);
  require(std::abs(std::exp(-x) * (1 + x + x * x / 2 + x * x * x / 6) - 1e-3) < 1e-9, "Q(4,x)=p");
  std::puts("coherent_core_test: PASS");
  return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `ssh sens6 'cd .../build_sense && cmake . >/dev/null && ninja test_nr_isac_coherent_core 2>&1 | tail -2'`
Expected: FAIL (`coherent_core.h` missing).

- [ ] **Step 3: Write `coherent_core.h`** with exactly the interfaces listed above (plus `#include "coherent_types.h"`, `#include "pipeline_types.h"`), and `RangeDoppler::idx` returning `((size_t)ch*axes.n_range + r)*axes.n_dopp + d`.

- [ ] **Step 4: Write `coherent_core.cc` (this task's part)**

```cpp
/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#include "coherent_core.h"
#include "fft.h"
#include "robust_stats.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace nr_isac::coherent {
namespace {
uint32_t next_pow2(uint32_t n) { uint32_t p = 1; while (p < n) p <<= 1; return p; }
double hann(double u) { return 0.5 - 0.5 * std::cos(2 * M_PI * u); }  // u in [0,1]
double baseband_hz(const CfrWindow& w, uint32_t k) { return ((double)k - w.subcarriers / 2.0) * w.scs_hz; }
// Observed [lo,hi] subcarrier span of a row; returns false when the row is empty.
bool row_span(const CfrWindow& w, uint32_t r, uint32_t* lo, uint32_t* hi)
{
  int l = -1, h = -1;
  for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) { if (l < 0) l = (int)k; h = (int)k; }
  if (l < 0) return false; *lo = (uint32_t)l; *hi = (uint32_t)h; return true;
}
// Centred-index inverse FFT of one row (or the row mean) after a delay ramp and a Hann window over
// the row's observed band; amplitude normalised by the window sum so every allocation gives the
// path amplitude at its peak.
std::vector<cd> row_profile(const CfrWindow& w, const Axes& a, uint32_t ant, int row /* -1 = mean */,
                            double ramp_delay_s, double phase_rad)
{
  std::vector<cd> buf(a.n_fft, cd(0, 0));
  double wsum = 0;
  auto add_row = [&](uint32_t r, double scale) {
    uint32_t lo, hi; if (!row_span(w, r, &lo, &hi)) return;
    for (uint32_t k = lo; k <= hi; ++k) {
      if (!w.observed[w.cell(r, k)]) continue;
      const double u = (hi > lo) ? (double)(k - lo) / (hi - lo) : 0.5;
      const double win = hann(u) * scale; wsum += win;
      const double f = baseband_hz(w, k);
      const cd ramp = std::polar(1.0, 2 * M_PI * f * ramp_delay_s - phase_rad);
      const int q = (int)k - (int)(w.subcarriers / 2);
      buf[(size_t)((q % (int)a.n_fft + (int)a.n_fft) % (int)a.n_fft)] += cd(w.values[w.sample(ant, r, k)]) * ramp * win;
    }
  };
  if (row >= 0) add_row((uint32_t)row, 1.0);
  else for (uint32_t r = 0; r < w.rows; ++r) add_row(r, 1.0 / w.rows);
  fft_inplace(buf, true);                         // inverse: sum * e^{+j...} / N
  if (wsum > 0) for (cd& v : buf) v *= (double)a.n_fft / wsum;
  return buf;
}
} // namespace

double gamma_upper_quantile(uint32_t shape, double p)
{
  auto Q = [shape](double x) { double term = 1, s = 1; for (uint32_t k = 1; k < shape; ++k) { term *= x / k; s += term; } return std::exp(-x) * s; };
  double lo = 0, hi = 1; while (Q(hi) > p) hi *= 2;
  for (int i = 0; i < 200; ++i) { const double m = 0.5 * (lo + hi); (Q(m) > p ? lo : hi) = m; }
  return 0.5 * (lo + hi);
}

Axes derive_axes(const CfrWindow& w, const Volume& vol, const Geometry& g, double max_speed_mps)
{
  Axes a;
  auto fail = [&](const char* why) { a.valid = false; a.invalid_reason = why; return a; };
  if (!w.valid() || w.antennas != kCh) return fail("window invalid or not 4 antennas");
  if (w.rows < 3) return fail("fewer than 3 rows");
  a.fc_hz = w.fc_hz; a.lambda_m = kC / w.fc_hz; a.scs_hz = w.scs_hz; a.subcarriers = w.subcarriers;
  std::vector<double> bw;
  for (uint32_t r = 0; r < w.rows; ++r) { uint32_t lo, hi; if (row_span(w, r, &lo, &hi)) bw.push_back((hi - lo + 1) * w.scs_hz); }
  if (bw.size() < 3) return fail("fewer than 3 non-empty rows");
  a.b_eff_hz = median(bw);
  if (a.b_eff_hz < 2 * w.scs_hz) return fail("median allocation narrower than 2 subcarriers");
  a.n_fft = next_pow2(w.subcarriers);
  a.delay_step_s = 1.0 / (a.n_fft * w.scs_hz);
  double max_ex = 0;
  for (double x : {vol.x0, vol.x1}) for (double y : {vol.y0, vol.y1}) for (double z : {vol.z0, vol.z1})
    for (const Vec3& rx : g.rx) max_ex = std::max(max_ex, excess_delay_s(Vec3{x, y, z}, g.tx, rx));
  a.n_range = std::min<uint32_t>(a.n_fft / 2, (uint32_t)std::ceil(max_ex / a.delay_step_s) + 4);
  const double slot = slot_duration_s(w.scs_hz);
  a.row_t_s.resize(w.rows);
  for (uint32_t r = 0; r < w.rows; ++r) a.row_t_s[r] = (w.row_time_slots[r] - w.row_time_slots[0]) * slot;
  std::vector<double> dt;
  for (uint32_t r = 1; r < w.rows; ++r) if (a.row_t_s[r] > a.row_t_s[r - 1]) dt.push_back(a.row_t_s[r] - a.row_t_s[r - 1]);
  if (dt.empty()) return fail("all rows at the same time");
  a.median_dt_s = median(dt);
  a.t_cpi_s = a.row_t_s.back() + a.median_dt_s;
  a.dopp_step_hz = 1.0 / a.t_cpi_s;
  const double span = 1.0 / (2 * a.median_dt_s);
  a.n_dopp = 2 * (uint32_t)std::ceil(span / a.dopp_step_hz);
  a.dopp0_hz = -(double)(a.n_dopp / 2) * a.dopp_step_hz;
  const double f_max = (max_speed_mps > 0) ? 2 * max_speed_mps / a.lambda_m : span;
  for (uint32_t d = 0; d < a.n_dopp; ++d) {
    const double f = a.dopp0_hz + d * a.dopp_step_hz;
    if (std::abs(f) <= a.notch_half_bins * a.dopp_step_hz) continue;
    if (std::abs(f) > f_max) continue;
    a.tested_dopp.push_back(d);
  }
  a.valid = true;
  return a;
}

LosEstimate find_los(const CfrWindow& w, const Axes& a, double pfa)
{
  LosEstimate L;
  for (uint32_t i = 0; i < kCh; ++i) {
    std::vector<cd> p = row_profile(w, a, i, -1, 0.0, 0.0);
    std::vector<double> pw(p.size()); for (size_t n = 0; n < p.size(); ++n) pw[n] = std::norm(p[n]);
    const double noise = median(pw) / std::log(2.0);                 // exponential noise mean
    const double thr = noise * gamma_upper_quantile(1, pfa);        // single complex bin: Gamma(1)
    const size_t strongest = (size_t)(std::max_element(pw.begin(), pw.end()) - pw.begin());
    // earliest significant local maximum within n_range bins before the strongest (circular)
    size_t best = strongest;
    for (uint32_t back = a.n_range; back > 0; --back) {
      const size_t n = (strongest + p.size() - back) % p.size();
      const size_t nm = (n + p.size() - 1) % p.size(), np = (n + 1) % p.size();
      if (pw[n] > thr && pw[n] >= pw[nm] && pw[n] >= pw[np]) { best = n; break; }
    }
    if (pw[best] <= thr) { L.found[i] = false; continue; }
    const size_t bm = (best + p.size() - 1) % p.size(), bp = (best + 1) % p.size();
    const double y0 = std::abs(p[bm]), y1 = std::abs(p[best]), y2 = std::abs(p[bp]);
    const double den = y0 - 2 * y1 + y2;
    const double frac = (std::abs(den) > 0) ? 0.5 * (y0 - y2) / den : 0.0;
    double bin = (double)best + std::clamp(frac, -0.5, 0.5);
    if (bin > a.n_fft / 2.0) bin -= a.n_fft;                        // negative delays wrap
    L.delay_s[i] = bin * a.delay_step_s;
    L.tap[i] = p[best]; L.snr[i] = pw[best] / noise; L.found[i] = true;
  }
  return L;
}

RowSync estimate_row_sync(const CfrWindow& w, const Axes& a, const LosEstimate& L)
{
  RowSync s; s.phase_rad.assign(w.rows, 0.0); s.delay_s.assign(w.rows, 0.0);
  std::vector<std::array<cd, kCh>> tap(w.rows);
  std::array<cd, kCh> mean{};
  for (uint32_t r = 0; r < w.rows; ++r)
    for (uint32_t i = 0; i < kCh; ++i) {
      if (!L.found[i]) continue;
      cd acc = 0; double n = 0;
      for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
        acc += cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * L.delay_s[i]); n += 1; }
      tap[r][i] = n > 0 ? acc / n : cd(0); mean[i] += tap[r][i] / (double)w.rows;
    }
  for (uint32_t r = 0; r < w.rows; ++r) {
    cd c = 0;
    for (uint32_t i = 0; i < kCh; ++i) if (L.found[i] && std::abs(mean[i]) > 0)
      c += tap[r][i] * std::conj(mean[i]) / std::abs(mean[i]);     // weight = channel LOS amplitude
    s.phase_rad[r] = std::arg(c);
    // common delay drift: phase slope over the row's dominant subcarrier spacing
    cd slope = 0; uint32_t prev = UINT32_MAX;
    for (uint32_t k = 0; k < w.subcarriers; ++k) if (w.observed[w.cell(r, k)]) {
      if (prev != UINT32_MAX && k - prev == 1)
        for (uint32_t i = 0; i < kCh; ++i) if (L.found[i]) {
          const cd z1 = cd(w.values[w.sample(i, r, k)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, k) * L.delay_s[i]);
          const cd z0 = cd(w.values[w.sample(i, r, prev)]) * std::polar(1.0, 2 * M_PI * baseband_hz(w, prev) * L.delay_s[i]);
          slope += z1 * std::conj(z0);
        }
      prev = k;
    }
    s.delay_s[r] = (std::abs(slope) > 0) ? -std::arg(slope) / (2 * M_PI * w.scs_hz) : 0.0;
  }
  return s;
}

RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s)
{
  RdResult out; out.rd.axes = a; out.rd.v.assign((size_t)kCh * a.n_range * a.n_dopp, cf(0, 0));
  std::vector<double> win(w.rows); double wsum = 0;
  for (uint32_t r = 0; r < w.rows; ++r) { win[r] = hann(a.t_cpi_s > 0 ? a.row_t_s[r] / a.row_t_s.back() : 0.5); wsum += win[r]; }
  for (uint32_t i = 0; i < kCh; ++i) {
    std::vector<std::vector<cd>> prof(w.rows);
    std::vector<cd> mean(a.n_range, cd(0));
    for (uint32_t r = 0; r < w.rows; ++r) {
      std::vector<cd> p = row_profile(w, a, i, (int)r, L.delay_s[i] + s.delay_s[r], s.phase_rad[r]);
      prof[r].assign(p.begin(), p.begin() + a.n_range);
      for (uint32_t m = 0; m < a.n_range; ++m) mean[m] += prof[r][m] / (double)w.rows;
    }
    out.los_tap[i] = mean[0];                                        // before static removal
    for (uint32_t r = 0; r < w.rows; ++r) for (uint32_t m = 0; m < a.n_range; ++m) prof[r][m] -= mean[m];
    for (uint32_t m = 0; m < a.n_range; ++m)
      for (uint32_t d = 0; d < a.n_dopp; ++d) {
        const double f = a.dopp0_hz + d * a.dopp_step_hz; cd acc = 0;
        for (uint32_t r = 0; r < w.rows; ++r) acc += prof[r][m] * win[r] * std::polar(1.0, -2 * M_PI * f * a.row_t_s[r]);
        out.rd.v[out.rd.idx(i, m, d)] = cf(acc / wsum);
      }
    std::vector<double> pw;
    for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d : a.tested_dopp) pw.push_back(std::norm(out.rd.v[out.rd.idx(i, m, d)]));
    out.noise[i] = pw.empty() ? 1.0 : std::max(median(pw) / std::log(2.0), std::numeric_limits<double>::min());
  }
  return out;
}

} // namespace nr_isac::coherent
```
Note on the Doppler sign: `x(t) ∝ e^{+j2π f_d t}` peaks at `+f_d` with the kernel `e^{-j2π f t}` — consistent with the test's `+doppler_hz` phase term. Bistatic range-rate = `−λ·f_d` (used in Task 5).

- [ ] **Step 5: Run the test to verify it passes**

Run: `ssh sens6 'cd .../build_sense && ninja test_nr_isac_coherent_core && ./test_nr_isac_coherent_core'`
Expected: `coherent_core_test: PASS`. If the target-doppler check is off by one bin, print `best_d` and `a.dopp0_hz` before changing anything; do not loosen the tolerance.

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_core.h openair1/PHY/NR_UE_ISAC/coherent_core.cc openair1/PHY/NR_UE_ISAC/tests/coherent_core_test.cc CMakeLists.txt
git commit -m "sensing: coherent CPU reference - allocation-aware axes, LOS, common row sync, range-Doppler"
```

---

### Task 4: Per-channel calibrator and coherence diagnostic

**Files:**
- Modify: `coherent_core.h`, `coherent_core.cc`; Create: `tests/coherent_calibration_test.cc`; `CMakeLists.txt` (test `test_nr_isac_coherent_calibration`)

**Interfaces:**
- Consumes: `LosEstimate`, `RdResult::los_tap`.
- Produces:
  ```cpp
  class Calibrator {
  public:
    /** One CPI: los taps (LOS-referenced), found flags and LOS SNR -> posterior calibration. */
    Calibration update(const std::array<cd,kCh>& los_tap, const std::array<bool,kCh>& found,
                       const std::array<double,kCh>& los_snr);
    const Calibration& last() const { return last_; }
  private:
    std::array<cd,kCh> s_{};          // unit phasor state per channel (ch0 fixed at 1)
    std::array<double,kCh> p_{};      // phase variance
    std::array<double,kCh> q_{};      // process noise (covariance matching, cumulative mean)
    std::array<uint32_t,kCh> nq_{};
    bool init_ = false;
    Calibration last_;
  };
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// openair1/PHY/NR_UE_ISAC/tests/coherent_calibration_test.cc
#include "coherent_core.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac::coherent;
static double wrap(double x) { return std::atan2(std::sin(x), std::cos(x)); }
int main() {
  std::mt19937 rng(5); std::normal_distribution<double> n(0, 1);
  const std::array<double, kCh> ph{0.0, 1.1, -2.0, 0.4};
  // stable phases: recovered, coherent gain -> ~4, rho -> ~1
  Calibrator cal; Calibration c;
  for (int k = 0; k < 40; ++k) {
    std::array<cd, kCh> tap; std::array<bool, kCh> f{true, true, true, true}; std::array<double, kCh> snr{};
    const double common = n(rng);                                   // common phase noise per CPI
    for (uint32_t i = 0; i < kCh; ++i) { tap[i] = std::polar(1.0, ph[i] + common + 0.05 * n(rng)); snr[i] = 200; }
    c = cal.update(tap, f, snr);
  }
  for (uint32_t i = 1; i < kCh; ++i) require(std::abs(wrap(c.phase_rad[i] - ph[i])) < 0.05, "phase recovered");
  require(c.coherent_gain > 3.8 && c.rho > 0.9, "coherent: G~4, rho~1");
  // scrambled phases each CPI: G -> ~1, rho -> ~0, coherence factors fall
  Calibrator bad; Calibration b;
  std::uniform_real_distribution<double> u(-M_PI, M_PI);
  for (int k = 0; k < 40; ++k) {
    std::array<cd, kCh> tap; std::array<bool, kCh> f{true, true, true, true}; std::array<double, kCh> snr{200, 200, 200, 200};
    for (uint32_t i = 0; i < kCh; ++i) tap[i] = std::polar(1.0, u(rng));
    b = bad.update(tap, f, snr);
  }
  require(b.rho < 0.35, "scrambled: rho low");
  for (uint32_t i = 1; i < kCh; ++i) require(b.coh_factor[i] < 0.5, "scrambled: channel faded");
  // LOS missing on channel 2 (Review Focus 2): predicts, no NaN, variance grows
  const double v_before = c.phase_var[2];
  std::array<cd, kCh> tap{cd(1), std::polar(1.0, ph[1]), cd(0), std::polar(1.0, ph[3])};
  Calibration m = cal.update(tap, {true, true, false, true}, {200, 200, 0, 200});
  require(std::isfinite(m.phase_rad[2]) && std::isfinite(m.coh_factor[2]) && m.phase_var[2] > v_before, "missing LOS: predict");
  std::puts("coherent_calibration_test: PASS");
  return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `ninja test_nr_isac_coherent_calibration` → FAIL (`Calibrator` undeclared).

- [ ] **Step 3: Implement `Calibrator::update` in `coherent_core.cc`**

```cpp
Calibration Calibrator::update(const std::array<cd, kCh>& tap, const std::array<bool, kCh>& found,
                               const std::array<double, kCh>& snr)
{
  Calibration c;
  const bool ref_ok = found[0] && std::abs(tap[0]) > 0;
  const cd ref = ref_ok ? std::conj(tap[0]) / std::abs(tap[0]) : cd(1);
  // Coherence of THIS CPI's LOS taps under the PREVIOUS calibration (predicted -> not tautological).
  if (init_ && ref_ok) {
    cd sum = 0; double pow_sum = 0;
    for (uint32_t i = 0; i < kCh; ++i) if (found[i] && std::abs(tap[i]) > 0) {
      const cd unit = tap[i] * ref / std::abs(tap[i]);
      sum += unit * std::conj(s_[i]); pow_sum += 1.0;
    }
    if (pow_sum > 1) { c.coherent_gain = std::norm(sum) / pow_sum; c.rho = std::clamp((c.coherent_gain - 1) / (pow_sum - 1), 0.0, 1.0); }
  }
  for (uint32_t i = 0; i < kCh; ++i) {
    c.los_found[i] = found[i]; c.los_snr[i] = snr[i];
    if (i == 0) { s_[0] = cd(1); p_[0] = 0; continue; }
    const double r = (found[i] && ref_ok && snr[i] > 0 && snr[0] > 0) ? 1 / (2 * snr[i]) + 1 / (2 * snr[0]) : 0;
    c.jitter_bound_rad[i] = r > 0 ? std::sqrt(r) : 0;
    if (!init_) { s_[i] = cd(1); p_[i] = M_PI * M_PI / 3; q_[i] = 0; nq_[i] = 0; }   // uniform-phase prior variance
    const double p_pred = p_[i] + q_[i];
    if (!(found[i] && ref_ok && r > 0)) { p_[i] = p_pred; continue; }            // predict only (no LOS)
    const cd z = tap[i] * ref / std::abs(tap[i]);
    const double nu = std::arg(z * std::conj(s_[i]));                           // wrapped innovation
    const double k = p_pred / (p_pred + r);
    s_[i] *= std::polar(1.0, k * nu);
    p_[i] = (1 - k) * p_pred;
    // covariance matching: innovation power beyond what the model predicts -> process noise
    const double q_obs = std::max(0.0, nu * nu - (p_pred + r));
    q_[i] = (q_[i] * nq_[i] + q_obs) / (nq_[i] + 1); ++nq_[i];
    c.jitter_rad[i] = std::sqrt(nu * nu);
  }
  init_ = true;
  for (uint32_t i = 0; i < kCh; ++i) {
    c.phase_rad[i] = std::arg(s_[i]); c.phase_var[i] = p_[i];
    c.coh_factor[i] = std::exp(-0.5 * std::min(p_[i], 50.0));
  }
  if (!init_ || c.coherent_gain <= 1.0) c.rho = std::max(0.0, c.rho);
  last_ = c;
  return c;
}
```
(Clamp `50.0` is only an exp-underflow guard: e^{-25} is already 0 for any purpose — it does not shape results.)

- [ ] **Step 4: Run to verify it passes** — `./test_nr_isac_coherent_calibration` → `PASS`.

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_core.h openair1/PHY/NR_UE_ISAC/coherent_core.cc openair1/PHY/NR_UE_ISAC/tests/coherent_calibration_test.cc CMakeLists.txt
git commit -m "sensing: coherent per-channel phase calibrator with predicted-phase coherence gain"
```

---

### Task 5: Focusing (envelope + coherent refinement) and detection

**Files:**
- Modify: `coherent_core.h`, `coherent_core.cc`; Create: `tests/coherent_focus_test.cc`; `CMakeLists.txt` (test `test_nr_isac_coherent_focus`)

**Interfaces:**
- Consumes: `Axes`, `RdResult`, `Calibration`, `Geometry`, `Volume`.
- Produces:
  ```cpp
  struct Grid { Vec3 origin; double step = 0; uint32_t nx = 0, ny = 0, nz = 0;
                size_t size() const { return (size_t)nx * ny * nz; }
                Vec3 at(size_t v) const; };                    // v = (iz*ny + iy)*nx + ix
  Grid envelope_grid(const Volume& vol, const Axes& a);        // step = c/(4*b_eff)
  /** E[t][v] for t over a.tested_dopp: sum_i |RD_i(dtau_i(x), d)|^2 / noise_i (Gamma(kCh,1) under noise) */
  std::vector<float> envelope(const RdResult& R, const Grid& g, const Geometry& geo);
  struct DetectParams { double pfa = 0; };                     // derived, see detect_params()
  DetectParams detect_params(const Axes& a, const Grid& g, double false_object_intensity_per_s);
  std::vector<Detection> detect(const std::vector<float>& E, const RdResult& R, const Grid& g,
                                const Geometry& geo, const DetectParams& p);
  void refine(Detection& det, const RdResult& R, const Grid& g, const Geometry& geo, const Calibration& cal);
  ```

- [ ] **Step 1: Write the failing test** (uses Task 3's `make_window` — copy the helper into this file verbatim; do not share test code across files)

```cpp
// openair1/PHY/NR_UE_ISAC/tests/coherent_focus_test.cc  (make_window copied from coherent_core_test.cc)
// ... #includes, require(), Path, make_window exactly as in coherent_core_test.cc ...
int main() {
  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol;
  const std::array<double, kCh> ph{0, 1.1, -2.0, 0.4};
  const Vec3 t1{5, 12, 1.2}, t2{-6, -4, 1.0}, drone{8, 8, 15}, mirror{8, 8, -15};
  auto taus = [&](const Vec3& p) { std::array<double, kCh> t{}; for (uint32_t i = 0; i < 4; ++i) t[i] = (dist(p, g.tx) + dist(p, g.rx[i]) + 30) / kC; return t; };
  std::array<double, kCh> los{}; for (uint32_t i = 0; i < 4; ++i) los[i] = (dist(g.tx, g.rx[i]) + 30) / kC;
  CfrWindow w = make_window(64, {{los, 1.0, 0}, {taus(t1), 0.2, 45}, {taus(t2), 0.25, -110}, {taus(drone), 0.15, 70},
                                  {taus(mirror), -0.07, 70}, {taus(t1), 0.05, 90}},   // 2nd harmonic of t1
                            ph, 0, 128, 3);
  Axes a = derive_axes(w, vol, g, 20.0); require(a.valid, "axes");
  LosEstimate L = find_los(w, a, 1e-4); RowSync s = estimate_row_sync(w, a, L); RdResult R = range_doppler(w, a, L, s);
  Calibrator cal; Calibration c{}; for (int k = 0; k < 5; ++k) c = cal.update(R.los_tap, L.found, L.snr);
  Grid G = envelope_grid(vol, a);
  require(std::abs(G.step - kC / (4 * a.b_eff_hz)) < 1e-9, "grid step derived from bandwidth");
  std::vector<float> E = envelope(R, G, g);
  require(E.size() == a.tested_dopp.size() * G.size(), "envelope sized");
  DetectParams p = detect_params(a, G, 1.0); require(p.pfa > 0 && p.pfa < 1e-3, "pfa derived from intensity");
  std::vector<Detection> D = detect(E, R, G, g, p);
  for (Detection& d : D) refine(d, R, G, g, c);
  auto near = [&](const Vec3& q, double tol) { for (const Detection& d : D) if (dist(d.pos, q) < tol) return true; return false; };
  const double tol = kC / (2 * a.b_eff_hz);                        // one range resolution cell
  require(near(t1, tol) && near(t2, tol) && near(drone, tol), "three targets detected within a resolution cell");
  for (const Detection& d : D) require(d.pos.z >= vol.z0 - 1e-9, "no below-ground detection (mirror rejected)");
  int t1_hits = 0; for (const Detection& d : D) if (dist(d.pos_env, t1) < 2 * G.step) ++t1_hits;
  require(t1_hits == 1, "harmonic merged into its fundamental");
  for (const Detection& d : D) require(d.refined && std::isfinite(d.pos_sigma.x) && d.pos_sigma.x > 0, "refined, finite sigma");
  // out-of-volume / out-of-axis voxels never read out of bounds (Review Focus 3): tiny n_range axis
  Axes tiny = a; tiny.n_range = 3; RdResult R3 = R; R3.rd.axes = tiny;
  R3.rd.v.assign((size_t)kCh * tiny.n_range * tiny.n_dopp, cf(0));
  std::vector<float> E3 = envelope(R3, G, g); require(E3.size() == tiny.tested_dopp.size() * G.size(), "no OOB");
  std::puts("coherent_focus_test: PASS");
  return 0;
}
```

- [ ] **Step 2: Run to verify it fails** → FAIL (`envelope_grid` undeclared).

- [ ] **Step 3: Implement in `coherent_core.cc`**

```cpp
Vec3 Grid::at(size_t v) const
{
  const size_t ix = v % nx, iy = (v / nx) % ny, iz = v / ((size_t)nx * ny);
  return Vec3{origin.x + ix * step, origin.y + iy * step, origin.z + iz * step};
}
Grid envelope_grid(const Volume& vol, const Axes& a)
{
  Grid g; g.step = kC / (4 * a.b_eff_hz); g.origin = Vec3{vol.x0, vol.y0, vol.z0};
  g.nx = (uint32_t)std::floor((vol.x1 - vol.x0) / g.step) + 1;
  g.ny = (uint32_t)std::floor((vol.y1 - vol.y0) / g.step) + 1;
  g.nz = (uint32_t)std::floor((vol.z1 - vol.z0) / g.step) + 1;
  return g;
}
namespace {
// Linear complex interpolation of channel i's range axis at a fractional bin; false when outside.
bool sample_rd(const RdResult& R, uint32_t ch, double bin, uint32_t d, cd* out)
{
  const uint32_t n = R.rd.axes.n_range;
  if (!(bin >= 0) || bin > (double)(n - 1)) return false;
  const uint32_t b0 = (uint32_t)bin; const uint32_t b1 = std::min(b0 + 1, n - 1); const double t = bin - b0;
  *out = cd(R.rd.v[R.rd.idx(ch, b0, d)]) * (1 - t) + cd(R.rd.v[R.rd.idx(ch, b1, d)]) * t;
  return true;
}
} // namespace
std::vector<float> envelope(const RdResult& R, const Grid& g, const Geometry& geo)
{
  const Axes& a = R.rd.axes;
  std::vector<float> E(a.tested_dopp.size() * g.size(), 0.f);
  for (size_t v = 0; v < g.size(); ++v) {
    const Vec3 x = g.at(v);
    double bin[kCh];
    for (uint32_t i = 0; i < kCh; ++i) bin[i] = excess_delay_s(x, geo.tx, geo.rx[i]) / a.delay_step_s;
    for (size_t t = 0; t < a.tested_dopp.size(); ++t) {
      double e = 0; cd s;
      for (uint32_t i = 0; i < kCh; ++i) if (sample_rd(R, i, bin[i], a.tested_dopp[t], &s)) e += std::norm(s) / R.noise[i];
      E[t * g.size() + v] = (float)e;
    }
  }
  return E;
}
DetectParams detect_params(const Axes& a, const Grid& g, double intensity)
{
  DetectParams p;
  const double tested = (double)g.size() * std::max<size_t>(1, a.tested_dopp.size());
  p.pfa = std::min(0.5, intensity * a.t_cpi_s / tested);
  return p;
}
std::vector<Detection> detect(const std::vector<float>& E, const RdResult& R, const Grid& g,
                              const Geometry& geo, const DetectParams& p)
{
  const Axes& a = R.rd.axes;
  const double thr_unit = gamma_upper_quantile(kCh, p.pfa);
  const double med_unit = gamma_upper_quantile(kCh, 0.5);
  struct Cand { size_t t, v; double e, med; };
  std::vector<Cand> c;
  for (size_t t = 0; t < a.tested_dopp.size(); ++t) {
    std::vector<double> col(E.begin() + t * g.size(), E.begin() + (t + 1) * g.size());
    const double med = median(col);                                  // robust per-Doppler noise level
    if (!(med > 0)) continue;
    const double thr = thr_unit * med / med_unit;
    for (size_t v = 0; v < g.size(); ++v) if (E[t * g.size() + v] > thr) c.push_back({t, v, E[t * g.size() + v], med});
  }
  std::sort(c.begin(), c.end(), [](const Cand& l, const Cand& r) { return l.e > r.e; });
  std::vector<Detection> out;
  const double nms_r = 2 * g.step;
  for (const Cand& k : c) {
    const Vec3 x = g.at(k.v);
    if (x.z < 0) continue;                                           // ground-bounce mirrors
    const double fd = a.dopp0_hz + a.tested_dopp[k.t] * a.dopp_step_hz;
    bool drop = false;
    for (const Detection& d : out) {
      if (dist(d.pos_env, x) > nms_r) continue;
      if (std::abs(d.doppler_hz - fd) <= a.dopp_step_hz) { drop = true; break; }            // NMS
      const double ratio = fd / d.doppler_hz; const double kk = std::round(ratio);          // harmonic of a stronger one
      if (kk >= 2 && std::abs(fd - kk * d.doppler_hz) <= a.dopp_step_hz) { drop = true; break; }
    }
    if (drop) continue;
    Detection d; d.pos = d.pos_env = x; d.doppler_hz = fd; d.dopp_bin = a.tested_dopp[k.t];
    d.range_rate_mps = -a.lambda_m * fd;
    d.snr = std::max(1e-6, (k.e * med_unit / k.med - kCh) / kCh);
    d.tx = geo.tx;
    const double res = kC / (2 * a.b_eff_hz);
    const double sd = std::max(res / std::sqrt(2 * d.snr), g.step / std::sqrt(12.0));
    d.pos_sigma = Vec3{sd, sd, sd};
    d.range_rate_sigma = std::max(a.lambda_m * a.dopp_step_hz / std::sqrt(2 * d.snr), a.lambda_m * a.dopp_step_hz / std::sqrt(12.0));
    out.push_back(d);
  }
  return out;
}
void refine(Detection& det, const RdResult& R, const Grid& g, const Geometry& geo, const Calibration& cal)
{
  const Axes& a = R.rd.axes;
  Vec3 centroid{}; for (const Vec3& r : geo.rx) centroid = centroid + r * 0.25;
  double D = 0; for (uint32_t i = 0; i < kCh; ++i) for (uint32_t j = i + 1; j < kCh; ++j) D = std::max(D, dist(geo.rx[i], geo.rx[j]));
  const double Rng = std::max(dist(det.pos_env, centroid), g.step);
  const double fringe = a.lambda_m * Rng / (2 * D);
  const double fs = fringe / 2;
  const int n = (int)std::ceil(g.step / fs);
  auto coh_terms = [&](const Vec3& x, std::array<cd, kCh>* terms) {
    cd sum = 0;
    for (uint32_t i = 0; i < kCh; ++i) {
      const double ex = excess_delay_s(x, geo.tx, geo.rx[i]); cd s;
      if (!sample_rd(R, i, ex / a.delay_step_s, det.dopp_bin, &s)) { (*terms)[i] = 0; continue; }
      (*terms)[i] = cal.coh_factor[i] * std::polar(1.0, -cal.phase_rad[i]) * s / std::sqrt(R.noise[i])
                    * std::polar(1.0, 2 * M_PI * a.fc_hz * ex);
      sum += (*terms)[i];
    }
    return std::norm(sum);
  };
  double best = -1; Vec3 bx = det.pos_env; std::array<cd, kCh> bt{};
  for (int iz = -n; iz <= n; ++iz) for (int iy = -n; iy <= n; ++iy) for (int ix = -n; ix <= n; ++ix) {
    const Vec3 x = det.pos_env + Vec3{ix * fs, iy * fs, iz * fs};
    if (x.z < 0) continue;
    std::array<cd, kCh> t; const double c = coh_terms(x, &t);
    if (c > best) { best = c; bx = x; bt = t; }
  }
  det.pos = bx * cal.rho + det.pos_env * (1 - cal.rho);
  det.terms = bt;
  const double res = kC / (2 * a.b_eff_hz);
  const double sd = std::max((cal.rho * fringe + (1 - cal.rho) * res) / std::sqrt(2 * det.snr), fs / std::sqrt(12.0));
  det.pos_sigma = Vec3{sd, sd, sd};
  det.refined = true;
}
```

- [ ] **Step 4: Run to verify it passes** → `coherent_focus_test: PASS`. If a target is missed, print the per-Doppler median, threshold and the target voxel's E before changing any logic; the thresholds are derived and must not be edited to pass.

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_core.h openair1/PHY/NR_UE_ISAC/coherent_core.cc openair1/PHY/NR_UE_ISAC/tests/coherent_focus_test.cc CMakeLists.txt
git commit -m "sensing: coherent envelope focusing, derived CFAR, NMS/harmonic merge, coherent refinement"
```

---

### Task 6: Coherent tracker

**Files:**
- Create: `coherent_tracker.h`, `coherent_tracker.cc`, `tests/coherent_tracker_test.cc`; Modify: `CMakeLists.txt` (source + test `test_nr_isac_coherent_tracker`)

**Interfaces:**
- Consumes: `Detection`, `Track`, `Volume`.
- Produces:
  ```cpp
  struct TrackerParams { double max_speed_mps = 0; double false_object_intensity_per_s = 0; Volume volume; Vec3 array_centroid; };
  class CoherentTracker {
  public:
    explicit CoherentTracker(const TrackerParams& p);
    /** One CPI at time t_s (seconds, monotonic). Returns the tracks after the update. */
    const std::vector<Track>& step(double t_s, double t_cpi_s, const std::vector<Detection>& dets,
                                   std::vector<int>* assoc = nullptr);   // assoc[i] = track index or -1
    double pd() const;
  private: /* members in Step 3 */
  };
  std::vector<int> hungarian(const std::vector<std::vector<double>>& cost);   // rows->col or -1; INF = forbidden
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// openair1/PHY/NR_UE_ISAC/tests/coherent_tracker_test.cc
#include "coherent_tracker.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;
static Detection det(Vec3 p, double sd) { Detection d; d.pos = d.pos_env = p; d.pos_sigma = {sd, sd, sd}; d.snr = 50; d.range_rate_sigma = 1e9; d.refined = true; return d; }
int main() {
  // hungarian basics
  { const double I = 1e18; auto a = hungarian({{1, 5}, {5, 1}}); require(a[0] == 0 && a[1] == 1, "diag");
    auto b = hungarian({{I, I}, {3, I}}); require(b[0] == -1 && b[1] == 0, "forbidden"); }
  TrackerParams tp; tp.max_speed_mps = 20; tp.false_object_intensity_per_s = 1.0; tp.array_centroid = {5, 5, 2};
  CoherentTracker trk(tp);
  std::mt19937 rng(9); std::normal_distribution<double> n(0, 0.3); std::uniform_real_distribution<double> u(-15, 15), uz(0, 30);
  const double T = 0.075;
  for (int k = 0; k < 120; ++k) {
    const double t = k * T;
    std::vector<Detection> D;
    D.push_back(det(Vec3{-10 + 1.2 * t + n(rng), 3 + n(rng), 1 + n(rng)}, 0.4));      // person
    D.push_back(det(Vec3{-14 + 8.0 * t + n(rng), -6 + n(rng), 0.8 + n(rng)}, 0.4));   // car
    if (k % 5) D.push_back(det(Vec3{8 - 3 * t + n(rng), 8 + 2 * t + n(rng), 15 + n(rng)}, 0.4)); // drone, P_D 0.8
    if (k % 3 == 0) D.push_back(det(Vec3{u(rng), u(rng), uz(rng)}, 0.4));              // clutter
    trk.step(t, T, D);
  }
  const auto& tr = trk.step(120 * T, T, {});
  int confirmed = 0; for (const Track& x : tr) { if (x.confirmed) ++confirmed;
    for (double v : x.x) require(std::isfinite(v), "state finite");
    for (int i = 0; i < 6; ++i) require(x.P[i * 6 + i] > 0, "covariance diagonal positive"); }
  require(confirmed == 3, "exactly the 3 real targets confirmed (clutter not)");
  require(trk.pd() > 0.6 && trk.pd() < 1.0, "P_D estimated online");
  // burst (Review Focus 4): 400 detections in one CPI stays fast
  std::vector<Detection> burst; for (int i = 0; i < 400; ++i) burst.push_back(det(Vec3{u(rng), u(rng), uz(rng)}, 0.4));
  const auto t0 = std::chrono::steady_clock::now(); trk.step(121 * T, T, burst);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  require(ms < 20.0, "400-detection burst under 20 ms");
  std::puts("coherent_tracker_test: PASS");
  return 0;
}
```

- [ ] **Step 2: Run to verify it fails** → FAIL (header missing).

- [ ] **Step 3: Implement `coherent_tracker.h/.cc`**

```cpp
/* coherent_tracker.h */
#pragma once
#include "coherent_types.h"
#include <vector>
namespace nr_isac::coherent {
struct TrackerParams { double max_speed_mps = 0; double false_object_intensity_per_s = 0; Volume volume; Vec3 array_centroid; };
std::vector<int> hungarian(const std::vector<std::vector<double>>& cost);
class CoherentTracker {
public:
  explicit CoherentTracker(const TrackerParams& p);
  const std::vector<Track>& step(double t_s, double t_cpi_s, const std::vector<Detection>& dets, std::vector<int>* assoc = nullptr);
  double pd() const { return (pd_hits_ + 1.0) / (pd_hits_ + pd_misses_ + 2.0); }   // Laplace prior 1/1
private:
  void predict(Track& t, double dt) const;
  double position_nis(const Track& t, const Detection& d, double* logdet_s) const;
  void update_position(Track& t, const Detection& d, double* nis) const;
  void update_rate(Track& t, const Detection& d) const;
  TrackerParams p_;
  std::vector<Track> tracks_;
  uint64_t next_id_ = 1;
  double last_t_ = -1;
  double pd_hits_ = 0, pd_misses_ = 0;
};
} // namespace nr_isac::coherent
```
```cpp
/* coherent_tracker.cc */
#include "coherent_tracker.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace nr_isac::coherent {
namespace {
constexpr double kInf = 1e18;
constexpr double kGate3 = 11.3448667301444;           // chi2_3 99 % (conventional)
constexpr double kAlpha = 0.01, kBeta = 0.10;          // SPRT (conventional)
const double kConfirm = std::log((1 - kBeta) / kAlpha), kDelete = std::log(kBeta / (1 - kAlpha));
double& P(Track& t, int r, int c) { return t.P[r * 6 + c]; }
double Pc(const Track& t, int r, int c) { return t.P[r * 6 + c]; }
bool inv3(const double a[9], double o[9], double* det)
{
  const double d = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (!(std::abs(d) > 0)) return false; *det = d;
  o[0] = (a[4] * a[8] - a[5] * a[7]) / d; o[1] = (a[2] * a[7] - a[1] * a[8]) / d; o[2] = (a[1] * a[5] - a[2] * a[4]) / d;
  o[3] = (a[5] * a[6] - a[3] * a[8]) / d; o[4] = (a[0] * a[8] - a[2] * a[6]) / d; o[5] = (a[2] * a[3] - a[0] * a[5]) / d;
  o[6] = (a[3] * a[7] - a[4] * a[6]) / d; o[7] = (a[1] * a[6] - a[0] * a[7]) / d; o[8] = (a[0] * a[4] - a[1] * a[3]) / d;
  return true;
}
void symmetrize(Track& t) { for (int r = 0; r < 6; ++r) for (int c = r + 1; c < 6; ++c) { const double m = 0.5 * (P(t, r, c) + P(t, c, r)); P(t, r, c) = P(t, c, r) = m; } }
} // namespace

std::vector<int> hungarian(const std::vector<std::vector<double>>& cost)
{
  const int n = (int)cost.size(); if (!n) return {};
  const int m = (int)cost[0].size(); const int N = std::max(n, m);
  std::vector<std::vector<double>> a(N + 1, std::vector<double>(N + 1, kInf));
  for (int i = 0; i < n; ++i) for (int j = 0; j < m; ++j) a[i + 1][j + 1] = cost[i][j];
  std::vector<double> u(N + 1), v(N + 1); std::vector<int> p(N + 1), way(N + 1);
  for (int i = 1; i <= N; ++i) {
    p[0] = i; int j0 = 0; std::vector<double> minv(N + 1, std::numeric_limits<double>::infinity()); std::vector<char> used(N + 1, 0);
    do {
      used[j0] = 1; const int i0 = p[j0]; double delta = std::numeric_limits<double>::infinity(); int j1 = 0;
      for (int j = 1; j <= N; ++j) if (!used[j]) {
        const double cur = a[i0][j] - u[i0] - v[j];
        if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
        if (minv[j] < delta) { delta = minv[j]; j1 = j; }
      }
      for (int j = 0; j <= N; ++j) { if (used[j]) { u[p[j]] += delta; v[j] -= delta; } else minv[j] -= delta; }
      j0 = j1;
    } while (p[j0] != 0);
    do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
  }
  std::vector<int> row(n, -1);
  for (int j = 1; j <= N; ++j) if (p[j] >= 1 && p[j] <= n && j <= m && a[p[j]][j] < kInf / 2) row[p[j] - 1] = j - 1;
  return row;
}

CoherentTracker::CoherentTracker(const TrackerParams& p) : p_(p) {}

void CoherentTracker::predict(Track& t, double dt) const
{
  for (int i = 0; i < 3; ++i) t.x[i] += dt * t.x[i + 3];
  // P = F P F^T + q * [dt^3/3 dt^2/2; dt^2/2 dt] per axis
  std::array<double, 36> Pn = t.P;
  auto F = [dt](int r, int c) { return (r == c) ? 1.0 : ((c == r + 3 && r < 3) ? dt : 0.0); };
  for (int r = 0; r < 6; ++r) for (int c = 0; c < 6; ++c) {
    double s = 0; for (int i = 0; i < 6; ++i) for (int j = 0; j < 6; ++j) s += F(r, i) * t.P[i * 6 + j] * F(c, j);
    Pn[r * 6 + c] = s;
  }
  t.P = Pn;
  for (int i = 0; i < 3; ++i) { P(t, i, i) += t.q * dt * dt * dt / 3; P(t, i, i + 3) += t.q * dt * dt / 2; P(t, i + 3, i) += t.q * dt * dt / 2; P(t, i + 3, i + 3) += t.q * dt; }
  symmetrize(t); t.age_s += dt;
}
double CoherentTracker::position_nis(const Track& t, const Detection& d, double* logdet) const
{
  const double r[3] = {d.pos_sigma.x * d.pos_sigma.x, d.pos_sigma.y * d.pos_sigma.y, d.pos_sigma.z * d.pos_sigma.z};
  double S[9], Si[9], det; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) S[i * 3 + j] = Pc(t, i, j) + (i == j ? r[i] : 0);
  if (!inv3(S, Si, &det)) return kInf;
  const double nu[3] = {d.pos.x - t.x[0], d.pos.y - t.x[1], d.pos.z - t.x[2]};
  double q = 0; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q += nu[i] * Si[i * 3 + j] * nu[j];
  if (logdet) *logdet = std::log(det);
  return q;
}
void CoherentTracker::update_position(Track& t, const Detection& d, double* nis) const
{
  const double r[3] = {d.pos_sigma.x * d.pos_sigma.x, d.pos_sigma.y * d.pos_sigma.y, d.pos_sigma.z * d.pos_sigma.z};
  double S[9], Si[9], det; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) S[i * 3 + j] = Pc(t, i, j) + (i == j ? r[i] : 0);
  if (!inv3(S, Si, &det)) return;
  double K[18]; for (int a = 0; a < 6; ++a) for (int j = 0; j < 3; ++j) { double s = 0; for (int i = 0; i < 3; ++i) s += Pc(t, a, i) * Si[i * 3 + j]; K[a * 3 + j] = s; }
  const double nu[3] = {d.pos.x - t.x[0], d.pos.y - t.x[1], d.pos.z - t.x[2]};
  if (nis) { double q = 0; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) q += nu[i] * Si[i * 3 + j] * nu[j]; *nis = q; }
  for (int a = 0; a < 6; ++a) for (int j = 0; j < 3; ++j) t.x[a] += K[a * 3 + j] * nu[j];
  // Joseph form: P = (I-KH)P(I-KH)^T + K R K^T
  std::array<double, 36> IKH{}; for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) IKH[a * 6 + b] = (a == b) - (b < 3 ? K[a * 3 + b] : 0.0);
  std::array<double, 36> T1{}, Pn{};
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) { double s = 0; for (int c = 0; c < 6; ++c) s += IKH[a * 6 + c] * t.P[c * 6 + b]; T1[a * 6 + b] = s; }
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) { double s = 0; for (int c = 0; c < 6; ++c) s += T1[a * 6 + c] * IKH[b * 6 + c]; for (int j = 0; j < 3; ++j) s += K[a * 3 + j] * r[j] * K[b * 3 + j]; Pn[a * 6 + b] = s; }
  t.P = Pn; symmetrize(t);
}
void CoherentTracker::update_rate(Track& t, const Detection& d) const
{
  if (!(d.range_rate_sigma > 0 && d.range_rate_sigma < 1e8)) return;
  const Vec3 x{t.x[0], t.x[1], t.x[2]};
  const Vec3 h = normalized(x - d.tx) + normalized(x - p_.array_centroid);    // gradient w.r.t. velocity
  const double hv[6] = {0, 0, 0, h.x, h.y, h.z};
  const double pred = h.x * t.x[3] + h.y * t.x[4] + h.z * t.x[5];
  double PH[6], S = d.range_rate_sigma * d.range_rate_sigma;
  for (int a = 0; a < 6; ++a) { double s = 0; for (int b = 0; b < 6; ++b) s += t.P[a * 6 + b] * hv[b]; PH[a] = s; }
  for (int a = 0; a < 6; ++a) S += hv[a] * PH[a];
  const double nu = d.range_rate_mps - pred;
  if (nu * nu / S > 6.634896601021214) return;                               // chi2_1 99 %: outlier rate ignored
  double K[6]; for (int a = 0; a < 6; ++a) K[a] = PH[a] / S;
  for (int a = 0; a < 6; ++a) t.x[a] += K[a] * nu;
  for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) t.P[a * 6 + b] -= K[a] * S * K[b];
  symmetrize(t);
  for (int a = 0; a < 6; ++a) if (!(t.P[a * 6 + a] > 0)) t.P[a * 6 + a] = std::abs(t.P[a * 6 + a]) + 1e-12;
}

const std::vector<Track>& CoherentTracker::step(double t_s, double t_cpi_s, const std::vector<Detection>& dets, std::vector<int>* assoc)
{
  const double dt = (last_t_ < 0) ? t_cpi_s : std::max(0.0, t_s - last_t_); last_t_ = t_s;
  for (Track& t : tracks_) predict(t, dt);
  const Volume& V = p_.volume;
  const double vol = std::max(1e-9, (V.x1 - V.x0) * (V.y1 - V.y0) * (V.z1 - V.z0));
  const double clutter_density = std::max(1e-12, p_.false_object_intensity_per_s * t_cpi_s / vol);
  std::vector<std::vector<double>> cost(tracks_.size(), std::vector<double>(dets.size(), kInf));
  for (size_t i = 0; i < tracks_.size(); ++i) for (size_t j = 0; j < dets.size(); ++j) {
    double ld; const double q = position_nis(tracks_[i], dets[j], &ld); if (q <= kGate3) cost[i][j] = q + ld; }
  std::vector<int> a = hungarian(cost);
  std::vector<char> used(dets.size(), 0);
  const double pd = this->pd();
  for (size_t i = 0; i < tracks_.size(); ++i) {
    Track& t = tracks_[i];
    if (i < a.size() && a[i] >= 0) {
      const Detection& d = dets[(size_t)a[i]]; used[(size_t)a[i]] = 1;
      double ld; const double q = position_nis(t, d, &ld);
      const double like = std::exp(-0.5 * q) / std::sqrt(std::pow(2 * M_PI, 3) * std::exp(ld));
      t.llr += std::log(std::max(1e-300, pd * like / clutter_density));
      double nis = 0; update_position(t, d, &nis); update_rate(t, d);
      t.nis_sum += nis; ++t.nis_n; ++t.hits;
      t.q = t.q * std::max(1e-3, (t.nis_sum / t.nis_n) / 3.0) / std::max(1e-3, ((t.nis_sum - nis) / std::max(1u, t.nis_n - 1)) / 3.0);
      if (t.confirmed) pd_hits_ += 1;
    } else {
      t.llr += std::log(std::max(1e-300, 1 - pd)); ++t.misses;
      if (t.confirmed) pd_misses_ += 1;
    }
    if (!t.confirmed && t.llr >= kConfirm) t.confirmed = true;
  }
  tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.llr <= kDelete; }), tracks_.end());
  const double q0 = std::pow(p_.max_speed_mps / 1.0, 2);          // declared bound, stage-10 convention
  for (size_t j = 0; j < dets.size(); ++j) if (!used[j]) {
    Track t; t.id = next_id_++; t.q = q0;
    t.x = {dets[j].pos.x, dets[j].pos.y, dets[j].pos.z, 0, 0, 0};
    const double vs = p_.max_speed_mps * p_.max_speed_mps;
    t.P.fill(0); P(t, 0, 0) = dets[j].pos_sigma.x * dets[j].pos_sigma.x; P(t, 1, 1) = dets[j].pos_sigma.y * dets[j].pos_sigma.y;
    P(t, 2, 2) = dets[j].pos_sigma.z * dets[j].pos_sigma.z; P(t, 3, 3) = P(t, 4, 4) = P(t, 5, 5) = vs;
    t.hits = 1; tracks_.push_back(t);
  }
  if (assoc) { assoc->assign(dets.size(), -1); for (size_t i = 0; i < a.size(); ++i) if (a[i] >= 0) (*assoc)[(size_t)a[i]] = (int)i; }
  return tracks_;
}
} // namespace nr_isac::coherent
```
Note on the adaptive `q` line: it rescales `q` by the ratio of the running mean NIS after/before this update, so `q` tracks `mean_NIS/3` multiplicatively from `q0` — no constants beyond the declared start value; the `1e-3` floors only prevent a zero division on the first update.

- [ ] **Step 4: Run to verify it passes** → `coherent_tracker_test: PASS`. If exactly-3-confirmed fails, print each track's llr/hits/misses first.

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_tracker.h openair1/PHY/NR_UE_ISAC/coherent_tracker.cc openair1/PHY/NR_UE_ISAC/tests/coherent_tracker_test.cc CMakeLists.txt
git commit -m "sensing: coherent 3D tracker (CV Kalman, Joseph form, Hungarian GNN, SPRT existence, online P_D)"
```

---

### Task 7: Autofocus (online antenna-position refinement)

**Files:**
- Create: `coherent_autofocus.h`, `coherent_autofocus.cc`, `tests/coherent_autofocus_test.cc`; Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: refined `Detection::terms`, `Detection::pos`, `Detection::snr`, surveyed `Geometry`, `coherent_survey_sigma_m`.
- Produces:
  ```cpp
  class Autofocus {
  public:
    Autofocus(const Geometry& surveyed, double survey_sigma_m, double fc_hz);
    /** Feed one detection that belongs to a CONFIRMED track. */
    void add(const Detection& d);
    /** Current refined geometry (surveyed + estimated per-antenna corrections). */
    Geometry geometry() const;
    std::array<double, kCh> correction_norm_m() const;
  private: /* Step 3 */
  };
  ```

- [ ] **Step 1: Write the failing test**

```cpp
// openair1/PHY/NR_UE_ISAC/tests/coherent_autofocus_test.cc
#include "coherent_autofocus.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
static void require(bool c, const char* m) { if (!c) throw std::runtime_error(m); }
using namespace nr_isac; using namespace nr_isac::coherent;
int main() {
  Geometry truth; truth.tx = {35, 20, 6};
  truth.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Geometry survey = truth;
  const Vec3 err[4] = {{0.02, -0.01, 0.0}, {-0.015, 0.02, 0.01}, {0.01, 0.015, -0.02}, {-0.02, -0.01, 0.015}};
  for (int i = 0; i < 4; ++i) survey.rx[i] = truth.rx[i] + err[i];
  const double fc = 3.45e9, lam = kC / fc;
  Autofocus af(survey, 0.1, fc);
  double before = 0; for (int i = 0; i < 4; ++i) before += norm(err[i]);
  std::mt19937 rng(4); std::uniform_real_distribution<double> u(-12, 12), uz(0.5, 20); std::normal_distribution<double> n(0, 0.05);
  for (int k = 0; k < 400; ++k) {
    Detection d; d.pos = {u(rng), u(rng), uz(rng)}; d.tx = truth.tx; d.snr = 100;
    // terms as the focuser would produce at the (true) voxel using the SURVEYED geometry
    for (int i = 0; i < 4; ++i) {
      const double ph_true = 2 * M_PI * (dist(d.pos, truth.tx) + dist(d.pos, truth.rx[i]) - dist(truth.tx, truth.rx[i])) / lam;
      const double ph_comp = 2 * M_PI * (dist(d.pos, survey.tx) + dist(d.pos, survey.rx[i]) - dist(survey.tx, survey.rx[i])) / lam;
      d.terms[i] = std::polar(1.0, -ph_true + ph_comp + n(rng));
    }
    af.add(d);
  }
  const Geometry g = af.geometry();
  double after = 0; for (int i = 0; i < 4; ++i) after += dist(g.rx[i], truth.rx[i]);
  require(after < 0.5 * before, "autofocus halves the survey error");
  std::puts("coherent_autofocus_test: PASS");
  return 0;
}
```

- [ ] **Step 2: Run to verify it fails** → FAIL (header missing).

- [ ] **Step 3: Implement**

```cpp
/* coherent_autofocus.h */
#pragma once
#include "coherent_types.h"
namespace nr_isac::coherent {
class Autofocus {
public:
  Autofocus(const Geometry& surveyed, double survey_sigma_m, double fc_hz);
  void add(const Detection& d);
  Geometry geometry() const;
  std::array<double, kCh> correction_norm_m() const;
private:
  Geometry surveyed_; double k_;                 // 2*pi*fc/c
  std::array<std::array<double, 9>, kCh> J_{};   // information matrices
  std::array<std::array<double, 3>, kCh> b_{};   // information vectors
};
} // namespace nr_isac::coherent
```
```cpp
/* coherent_autofocus.cc */
#include "coherent_autofocus.h"
#include <cmath>
namespace nr_isac::coherent {
namespace {
bool solve3(const std::array<double, 9>& A, const std::array<double, 3>& b, double x[3])
{
  const double d = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
  if (!(std::abs(d) > 0)) return false;
  auto det3 = [](double a, double b_, double c, double d_, double e, double f, double g, double h, double i) { return a * (e * i - f * h) - b_ * (d_ * i - f * g) + c * (d_ * h - e * g); };
  x[0] = det3(b[0], A[1], A[2], b[1], A[4], A[5], b[2], A[7], A[8]) / d;
  x[1] = det3(A[0], b[0], A[2], A[3], b[1], A[5], A[6], b[2], A[8]) / d;
  x[2] = det3(A[0], A[1], b[0], A[3], A[4], b[1], A[6], A[7], b[2]) / d;
  return true;
}
} // namespace
Autofocus::Autofocus(const Geometry& s, double sigma, double fc) : surveyed_(s), k_(2 * M_PI * fc / kC)
{
  const double w = 1.0 / (sigma * sigma);                          // prior: surveyed position, declared sigma
  for (uint32_t i = 0; i < kCh; ++i) { J_[i] = {w, 0, 0, 0, w, 0, 0, 0, w}; b_[i] = {0, 0, 0}; }
}
void Autofocus::add(const Detection& d)
{
  std::complex<double> sum = 0; for (const auto& t : d.terms) sum += t;
  if (std::abs(sum) == 0) return;
  const double var = 1.0 / (2 * std::max(d.snr, 1e-6));          // phase variance from SNR
  const Geometry g = geometry();
  for (uint32_t i = 0; i < kCh; ++i) {
    if (std::abs(d.terms[i]) == 0) continue;
    const double e = std::arg(d.terms[i] * std::conj(sum));        // residual vs the coherent sum
    // residual phase = -k * (u_tx,i - u_x,i) . delta_i   (u = unit vectors from rx_i)
    const Vec3 ux = normalized(d.pos - g.rx[i]), ut = normalized(d.tx - g.rx[i]);
    const Vec3 a = (ut - ux) * (-k_);
    const double av[3] = {a.x, a.y, a.z};
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) J_[i][r * 3 + c] += av[r] * av[c] / var; b_[i][r] += av[r] * e / var; }
  }
}
Geometry Autofocus::geometry() const
{
  Geometry g = surveyed_;
  for (uint32_t i = 0; i < kCh; ++i) { double x[3]; if (solve3(J_[i], b_[i], x)) g.rx[i] = surveyed_.rx[i] + Vec3{x[0], x[1], x[2]}; }
  return g;
}
std::array<double, kCh> Autofocus::correction_norm_m() const
{
  std::array<double, kCh> n{}; const Geometry g = geometry();
  for (uint32_t i = 0; i < kCh; ++i) n[i] = dist(g.rx[i], surveyed_.rx[i]);
  return n;
}
} // namespace nr_isac::coherent
```
Known limit (write it as a comment at the top of `coherent_autofocus.cc`): the linear model holds while the per-channel residual is within ±π; residuals from a wrong fringe are down-weighted only through SNR. The pipeline feeds only detections associated with confirmed tracks.

- [ ] **Step 4: Run to verify it passes** → PASS. (Note the residual sign convention in the test: if it fails with the error *growing*, the sign of `a` is inverted — verify by deriving `∂(−ph_true + ph_comp)/∂δ` before changing it.)

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_autofocus.h openair1/PHY/NR_UE_ISAC/coherent_autofocus.cc openair1/PHY/NR_UE_ISAC/tests/coherent_autofocus_test.cc CMakeLists.txt
git commit -m "sensing: coherent autofocus - online antenna position refinement from confirmed tracks"
```

---

### Task 8: UL path (built, disabled by default)

**Files:**
- Create: `coherent_ul.h`, `coherent_ul.cc`, `tests/coherent_ul_test.cc`; Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CfrWindow` (a UL session window), `Axes`, `find_los`-style profiles.
- Produces:
  ```cpp
  struct UeFix { bool valid = false; Vec3 pos; double sigma_m = 0; std::array<double, kCh> direct_delay_s{}; };
  /** UE position from its direct-path arrival per channel (TDOA, grid search over the volume). */
  UeFix localise_ue(const CfrWindow& ul, const Axes& a, const Geometry& geo, const Volume& vol, double pfa);
  /** Geometry for focusing with the UE as illuminator. */
  Geometry ue_geometry(const Geometry& geo, const UeFix& ue);
  ```

- [ ] **Step 1: Write the failing test** — build a window with `make_window` (copied verbatim from Task 3) whose "LOS" paths are the UE→antenna one-way delays plus a common unknown offset (+17 m), and assert `localise_ue` returns `valid` with `dist(pos, ue_true) < 2 * kC/(2*a.b_eff_hz)` and a finite positive `sigma_m`; and that `ue_geometry(geo, fix).tx == fix.pos` with the antennas unchanged.

```cpp
// in main():
Geometry g; g.tx = {35, 20, 6}; g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
Volume vol; const Vec3 ue{-6, 9, 1.4};
std::array<double, kCh> d{}; for (uint32_t i = 0; i < 4; ++i) d[i] = (dist(ue, g.rx[i]) + 17.0) / kC;
CfrWindow w = make_window(40, {{d, 1.0, 0}}, {0, 1.1, -2.0, 0.4}, 0, 128, 7);
Axes a = derive_axes(w, vol, g, 20.0);
UeFix f = localise_ue(w, a, g, vol, 1e-4);
require(f.valid && dist(f.pos, ue) < 2 * kC / (2 * a.b_eff_hz) && f.sigma_m > 0, "UE localised");
Geometry ug = ue_geometry(g, f); require(dist(ug.tx, f.pos) == 0 && dist(ug.rx[2], g.rx[2]) == 0, "UE geometry");
```

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement**

```cpp
/* coherent_ul.cc */
#include "coherent_ul.h"
#include <cmath>
#include <limits>
namespace nr_isac::coherent {
UeFix localise_ue(const CfrWindow& ul, const Axes& a, const Geometry& geo, const Volume& vol, double pfa)
{
  UeFix f;
  const LosEstimate L = find_los(ul, a, pfa);                       // earliest significant arrival = UE direct path
  for (uint32_t i = 0; i < kCh; ++i) if (!L.found[i]) return f;
  f.direct_delay_s = L.delay_s;
  const double sd = a.delay_step_s / std::sqrt(12.0);              // arrival quantisation (sub-bin estimate)
  const Grid g = envelope_grid(vol, a);
  double best = std::numeric_limits<double>::infinity(); size_t bv = 0;
  std::vector<double> cost(g.size());
  for (size_t v = 0; v < g.size(); ++v) {
    const Vec3 x = g.at(v); double c = 0;
    for (uint32_t i = 1; i < kCh; ++i) {
      const double meas = L.delay_s[i] - L.delay_s[0];
      const double model = (dist(x, geo.rx[i]) - dist(x, geo.rx[0])) / kC;
      c += (meas - model) * (meas - model) / (2 * sd * sd);
    }
    cost[v] = c; if (c < best) { best = c; bv = v; }
  }
  // sigma = RMS distance of voxels inside the chi2 +1 contour
  double s2 = 0; size_t n = 0; const Vec3 b = g.at(bv);
  for (size_t v = 0; v < g.size(); ++v) if (cost[v] <= best + 1.0) { s2 += norm2(g.at(v) - b); ++n; }
  f.pos = b; f.sigma_m = std::max(std::sqrt(s2 / std::max<size_t>(1, n)), g.step / std::sqrt(12.0)); f.valid = true;
  return f;
}
Geometry ue_geometry(const Geometry& geo, const UeFix& ue) { Geometry g = geo; g.tx = ue.pos; return g; }
} // namespace nr_isac::coherent
```
Note: the UE-illuminated echo delays in the UL window are referenced by `find_los`/`range_doppler` to each channel's own UE direct path, so `excess_delay_s(x, ue.pos, rx_i)` is exactly the UL focusing model — the Task 5 functions are reused unchanged with `ue_geometry(...)`.

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_ul.h openair1/PHY/NR_UE_ISAC/coherent_ul.cc openair1/PHY/NR_UE_ISAC/tests/coherent_ul_test.cc CMakeLists.txt
git commit -m "sensing: coherent UL path - UE direct-path TDOA localisation (built, off by default)"
```

---

### Task 9: CoherentPipeline (CPU path), reports, engine switch, offline end-to-end

**Files:**
- Create: `coherent_pipeline.h`, `coherent_pipeline.cc`, `tools/test_coherent_chain.sh`, `tests/passive_rx/ota/sensing_coherent_synth.conf`
- Modify: `sensing_engine.h` (member), `sensing_engine.cc` (construct + dispatch), `CMakeLists.txt` (sources)

**Interfaces:**
- Consumes: everything from Tasks 1–8.
- Produces:
  ```cpp
  struct CoherentStats { uint64_t processed = 0, skipped = 0, overruns = 0, queue_waits = 0; double last_ms = 0; };
  class CoherentPipeline {
  public:
    explicit CoherentPipeline(const CoherentConfig& cfg);
    ~CoherentPipeline();                      // joins the worker
    void submit(CfrWindow dl, std::vector<CfrWindow> ul, uint64_t sequence, double air_time_s);
    CoherentStats stats() const;
  };
  ```
  JSONL schemas (the monitor in Task 11 relies on them exactly):
  - `coherent_reports.*.jsonl` per CPI: `{"cpi":N,"t":air_s,"t_cpi_s":..,"b_eff_hz":..,"range_res_m":c/B,"grid_step_m":..,"n_voxels":..,"n_dopp_tested":..,"rows":..,"gpu":bool,"timing_ms":{"sync":..,"rd":..,"env":..,"detect":..,"refine":..,"track":..,"total":..},"overrun":bool,"stats":{processed,skipped,overruns,queue_waits},"skipped_reason":str|null,"detections":[{"p":[x,y,z],"s":[sx,sy,sz],"rr":..,"rr_s":..,"snr":..,"fd":..,"ill":0}],"topview":{"nx":..,"ny":..,"x0":..,"y0":..,"step":..,"db":[...]}|null,"rd":[{"ch":i,"nr":..,"nd":..,"range_m_per_bin":..,"dopp0_hz":..,"dopp_step_hz":..,"db":[...]}]|null}`
  - `coherent_tracks.*.jsonl` per CPI: `{"cpi":N,"t":air_s,"tracks":[{"id":..,"p":[3],"v":[3],"s":[3],"pe":existence_prob,"hits":..,"age":..,"confirmed":bool}]}` with `pe = 1/(1+exp(-llr))`.
  - `coherence.*.jsonl` per CPI: `{"cpi":N,"t":air_s,"phase":[4],"phase_var":[4],"jitter":[4],"bound":[4],"snr":[4],"los_found":[4],"G":..,"rho":..,"af_corr_m":[4]}`.
  Images (`topview`, `rd`) only when `monitor_period_s` has elapsed since the last emitted image (steady clock); values are dB relative to each image's median, rounded to 0.1 dB.

- [ ] **Step 1: Write the end-to-end test script `tools/test_coherent_chain.sh`**

```bash
#!/bin/bash
# Offline end-to-end: make_coherent_scene -> isac_replay (lockstep) -> coherent JSONL checks.
set -euo pipefail
BUILD=${BUILD:?set BUILD}; T=$(cd "$(dirname "$0")" && pwd); O=${O:-/tmp/isac_coherent}
REPO=$(cd "$T/../../../.." && pwd); CONF_T=$REPO/tests/passive_rx/ota/sensing_coherent_synth.conf
rm -rf "$O"; mkdir -p "$O"
python3 "$T/make_coherent_scene.py" --out "$O/rows.bin" --truth "$O/truth.json" --survey-out "$O/survey.json" --seconds ${SECONDS_RUN:-6}
python3 "$REPO/tests/passive_rx/survey.py" "$O/survey.json" --geometry "$O/g.json" --apply "$CONF_T" "$O/coherent.conf" --report-path "$O/reports.jsonl"
(cd "$BUILD" && ./isac_replay -O "$O/coherent.conf" --rows "$O/rows.bin") > "$O/replay.txt"
python3 - "$O" <<'EOF'
import glob, json, sys, numpy as np
O = sys.argv[1]; tr = json.load(open(f"{O}/truth.json"))
reps = [json.loads(l) for f in sorted(glob.glob(f"{O}/coherent_reports.*.jsonl")) for l in open(f)]
trk = [json.loads(l) for f in sorted(glob.glob(f"{O}/coherent_tracks.*.jsonl")) for l in open(f)]
coh = [json.loads(l) for f in sorted(glob.glob(f"{O}/coherence.*.jsonl")) for l in open(f)]
assert len(reps) > 20 and len(trk) == len(reps) and len(coh) == len(reps), (len(reps), len(trk), len(coh))
ph = np.array(tr["phases_rad"]); est = np.array(coh[-1]["phase"])
err = np.angle(np.exp(1j * (est - ph)))
assert np.all(np.abs(err[1:]) < 0.3), ("phases", err)
assert coh[-1]["G"] > 3.0 and coh[-1]["rho"] > 0.6, ("coherence", coh[-1]["G"], coh[-1]["rho"])
res = reps[-1]["range_res_m"]; last = trk[-1]; t = last["t"]
conf = [x for x in last["tracks"] if x["confirmed"]]
for tg in tr["targets"]:
    p = np.array(tg["p0"]) + np.array(tg["v"]) * t
    d = min((np.linalg.norm(np.array(x["p"]) - p) for x in conf), default=1e9)
    assert d < res, (tg["name"], d, res)
assert all(x["p"][2] >= -1e-6 for r in trk for x in r["tracks"]), "below-ground track"
assert len(conf) <= len(tr["targets"]) + 1, ("ghost tracks", len(conf))
print(f"coherent chain: {len(reps)} CPIs, G={coh[-1]['G']:.2f}, rho={coh[-1]['rho']:.2f}, confirmed={len(conf)}")
EOF
echo "test_coherent_chain: PASS"
```
And `tests/passive_rx/ota/sensing_coherent_synth.conf` (geometry keys are rewritten by `survey.py --apply`):
```
// Synthetic coherent-fuser test conf (tools/test_coherent_chain.sh). Geometry keys come from survey.py.
sensing = {
  enable = 1;
  sources = "pdsch_dmrs_blind,pdsch_data";
  gate_require_ul = 0;
  capture = 0;
  report_path = "/tmp/isac_coherent/reports.jsonl";
  num_ues = 1;
  maximum_target_speed_mps     = 20.0;
  max_range_m                  = 120.0;
  false_object_intensity_per_s = 1.0;
  coherent_enable = 1;
  coherent_ul_enable = 0;
  coherent_volume_m = "-15:15:-15:25:0:30";
  coherent_survey_sigma_m = 0.1;
  spatial_rx_positions = "0,0,0.5;10,0,3.5;0,10,3.5;10,10,0.5";
  tx_pos_x = 35.0;
  tx_pos_y = 20.0;
  tx_pos_z = 6.0;
  rvm_max_range_m = 120.0;
  rvm_period_s = 0.5;
};
```

- [ ] **Step 2: Run to verify it fails**

Run: `ssh sens6 'cd /home/sens/NICOLA/multirx-clean-adaptive && BUILD=$PWD/cmake_targets/ran_build/build_sense bash openair1/PHY/NR_UE_ISAC/tools/test_coherent_chain.sh'`
Expected: FAIL — no `coherent_reports.*.jsonl` (the engine does not dispatch yet). The DL flow gate: the generator writes source 3 rows with session 0; if `isac_replay` rejects session-less DL for the gate, set `gate_require_ul = 0` (already set) and confirm in `replay.txt` that CPIs were formed; if not, print the gate counters before changing code.

- [ ] **Step 3: Implement `coherent_pipeline.h/.cc`**

```cpp
/* coherent_pipeline.h */
#pragma once
#include "coherent_autofocus.h"
#include "coherent_core.h"
#include "coherent_report.h"
#include "coherent_tracker.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
namespace nr_isac::coherent {
struct CoherentStats { uint64_t processed = 0, skipped = 0, overruns = 0, queue_waits = 0; double last_ms = 0; };
class CudaCoherent;   // Task 10
class CoherentPipeline {
public:
  explicit CoherentPipeline(const CoherentConfig& cfg);
  ~CoherentPipeline();
  void submit(CfrWindow dl, std::vector<CfrWindow> ul, uint64_t sequence, double air_time_s);
  CoherentStats stats() const;
private:
  struct Job { CfrWindow dl; std::vector<CfrWindow> ul; uint64_t seq; double t; };
  void run();
  void process(Job& job);
  CoherentConfig cfg_;
  Calibrator cal_;
  std::unique_ptr<CoherentTracker> tracker_;
  std::unique_ptr<Autofocus> af_;
  std::unique_ptr<CudaCoherent> cuda_;
  JsonlSink reports_, tracks_, coherence_;
  mutable std::mutex mu_; std::condition_variable cv_; std::deque<Job> q_; bool stop_ = false;
  std::thread worker_;
  CoherentStats st_;
  std::chrono::steady_clock::time_point last_image_{};
};
} // namespace nr_isac::coherent
```
```cpp
/* coherent_pipeline.cc  -- CPU path; Task 10 adds the CUDA branch */
#include "coherent_pipeline.h"
#include "coherent_ul.h"
#include "common/utils/LOG/log.h"
#include <cmath>
#include <sstream>
namespace nr_isac::coherent {
namespace {
using clk = std::chrono::steady_clock;
double ms_since(clk::time_point t0) { return std::chrono::duration<double, std::milli>(clk::now() - t0).count(); }
std::string jnum(double v) { if (!std::isfinite(v)) return "null"; std::ostringstream o; o.precision(6); o << v; return o.str(); }
std::string jvec(const Vec3& v) { return "[" + jnum(v.x) + "," + jnum(v.y) + "," + jnum(v.z) + "]"; }
template <class A> std::string jarr(const A& a) { std::string s = "["; for (size_t i = 0; i < a.size(); ++i) s += (i ? "," : "") + jnum((double)a[i]); return s + "]"; }
std::string jbools(const std::array<bool, kCh>& a) { std::string s = "["; for (size_t i = 0; i < a.size(); ++i) s += std::string(i ? "," : "") + (a[i] ? "true" : "false"); return s + "]"; }
std::vector<double> to_db(const std::vector<double>& p)
{
  std::vector<double> q; for (double v : p) if (v > 0) q.push_back(v);
  const double ref = q.empty() ? 1.0 : median(q);
  std::vector<double> o(p.size()); for (size_t i = 0; i < p.size(); ++i) o[i] = std::round(10 * 10 * std::log10(std::max(p[i], 1e-30) / ref)) / 10;
  return o;
}
} // namespace

CoherentPipeline::CoherentPipeline(const CoherentConfig& cfg)
    : cfg_(cfg), reports_(cfg.out_dir, "coherent_reports"), tracks_(cfg.out_dir, "coherent_tracks"),
      coherence_(cfg.out_dir, "coherence")
{
  TrackerParams tp; tp.max_speed_mps = cfg.max_speed_mps; tp.false_object_intensity_per_s = cfg.false_object_intensity_per_s;
  tp.volume = cfg.volume; for (const Vec3& r : cfg.geometry.rx) tp.array_centroid = tp.array_centroid + r * 0.25;
  tracker_ = std::make_unique<CoherentTracker>(tp);
  worker_ = std::thread([this] { run(); });
  LOG_I(PHY, "SENSING: coherent pipeline writing %s\n", reports_.path().c_str());
}
CoherentPipeline::~CoherentPipeline()
{
  { std::lock_guard<std::mutex> l(mu_); stop_ = true; } cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}
void CoherentPipeline::submit(CfrWindow dl, std::vector<CfrWindow> ul, uint64_t seq, double t)
{
  std::unique_lock<std::mutex> l(mu_);
  if (q_.size() >= 2) { ++st_.queue_waits; cv_.wait(l, [this] { return q_.size() < 2 || stop_; }); }   // never drop
  q_.push_back(Job{std::move(dl), std::move(ul), seq, t});
  cv_.notify_all();
}
CoherentStats CoherentPipeline::stats() const { std::lock_guard<std::mutex> l(mu_); return st_; }
void CoherentPipeline::run()
{
  for (;;) {
    Job j;
    { std::unique_lock<std::mutex> l(mu_); cv_.wait(l, [this] { return !q_.empty() || stop_; });
      if (q_.empty() && stop_) return; j = std::move(q_.front()); q_.pop_front(); }
    cv_.notify_all();
    try { process(j); } catch (const std::exception& e) { LOG_E(PHY, "SENSING: coherent CPI %llu failed: %s\n", (unsigned long long)j.seq, e.what()); }
  }
}
void CoherentPipeline::process(Job& j)
{
  const auto t0 = clk::now(); double tm[6] = {0};
  if (!af_) af_ = std::make_unique<Autofocus>(cfg_.geometry, cfg_.survey_sigma_m, j.dl.fc_hz);
  const Geometry geo = af_->geometry();
  Axes a = derive_axes(j.dl, cfg_.volume, geo, cfg_.max_speed_mps);
  // LOS search tests n_fft bins per channel: at most one expected false LOS pick per CPI over all channels.
  const double pfa_los = a.valid ? std::min(0.5, 1.0 / ((double)a.n_fft * kCh)) : 0.5;
  std::ostringstream rep;
  if (!a.valid) {
    { std::lock_guard<std::mutex> l(mu_); ++st_.skipped; }
    rep << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"skipped_reason\":\"" << a.invalid_reason << "\"}";
    reports_.write_line(rep.str()); tracks_.write_line("{\"cpi\":" + std::to_string(j.seq) + ",\"t\":" + jnum(j.t) + ",\"tracks\":[]}");
    coherence_.write_line("{\"cpi\":" + std::to_string(j.seq) + ",\"t\":" + jnum(j.t) + ",\"skipped\":true}");
    return;
  }
  auto s0 = clk::now();
  const LosEstimate L = find_los(j.dl, a, pfa_los);
  const RowSync rs = estimate_row_sync(j.dl, a, L); tm[0] = ms_since(s0); s0 = clk::now();
  const RdResult R = range_doppler(j.dl, a, L, rs); tm[1] = ms_since(s0); s0 = clk::now();
  const Calibration cal = cal_.update(R.los_tap, L.found, L.snr);
  const Grid G = envelope_grid(cfg_.volume, a);
  const std::vector<float> E = envelope(R, G, geo); tm[2] = ms_since(s0); s0 = clk::now();
  std::vector<Detection> D = detect(E, R, G, geo, detect_params(a, G, cfg_.false_object_intensity_per_s)); tm[3] = ms_since(s0); s0 = clk::now();
  for (Detection& d : D) refine(d, R, G, geo, cal); tm[4] = ms_since(s0); s0 = clk::now();
  // UL illuminators (built, off by default)
  if (cfg_.ul_enable)
    for (const CfrWindow& u : j.ul) {
      Axes au = derive_axes(u, cfg_.volume, geo, cfg_.max_speed_mps); if (!au.valid) continue;
      const UeFix f = localise_ue(u, au, geo, cfg_.volume, pfa_los); if (!f.valid) continue;
      const Geometry ug = ue_geometry(geo, f);
      const LosEstimate Lu = find_los(u, au, pfa_los); const RdResult Ru = range_doppler(u, au, Lu, estimate_row_sync(u, au, Lu));
      const Grid Gu = envelope_grid(cfg_.volume, au);
      std::vector<Detection> Du = detect(envelope(Ru, Gu, ug), Ru, Gu, ug, detect_params(au, Gu, cfg_.false_object_intensity_per_s));
      for (Detection& d : Du) { refine(d, Ru, Gu, ug, cal); d.illuminator = u.session_id; d.pos_sigma = d.pos_sigma + Vec3{f.sigma_m, f.sigma_m, f.sigma_m}; D.push_back(d); }
    }
  std::vector<int> assoc;
  const std::vector<Track>& T = tracker_->step(j.t, a.t_cpi_s, D, &assoc);
  for (size_t k = 0; k < D.size(); ++k) if (assoc[k] >= 0 && T[(size_t)assoc[k]].confirmed && D[k].illuminator == 0) af_->add(D[k]);
  tm[5] = ms_since(s0);
  const double total = ms_since(t0); const bool overrun = total > a.t_cpi_s * 1e3;
  { std::lock_guard<std::mutex> l(mu_); ++st_.processed; st_.last_ms = total; if (overrun) ++st_.overruns; }
  const CoherentStats st = stats();
  rep << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"t_cpi_s\":" << jnum(a.t_cpi_s) << ",\"b_eff_hz\":" << jnum(a.b_eff_hz)
      << ",\"range_res_m\":" << jnum(kC / a.b_eff_hz) << ",\"grid_step_m\":" << jnum(G.step) << ",\"n_voxels\":" << G.size()
      << ",\"n_dopp_tested\":" << a.tested_dopp.size() << ",\"rows\":" << j.dl.rows << ",\"gpu\":false"
      << ",\"timing_ms\":{\"sync\":" << jnum(tm[0]) << ",\"rd\":" << jnum(tm[1]) << ",\"env\":" << jnum(tm[2]) << ",\"detect\":" << jnum(tm[3])
      << ",\"refine\":" << jnum(tm[4]) << ",\"track\":" << jnum(tm[5]) << ",\"total\":" << jnum(total) << "}"
      << ",\"overrun\":" << (overrun ? "true" : "false")
      << ",\"stats\":{\"processed\":" << st.processed << ",\"skipped\":" << st.skipped << ",\"overruns\":" << st.overruns << ",\"queue_waits\":" << st.queue_waits << "}"
      << ",\"skipped_reason\":null,\"detections\":[";
  for (size_t k = 0; k < D.size(); ++k)
    rep << (k ? "," : "") << "{\"p\":" << jvec(D[k].pos) << ",\"s\":" << jvec(D[k].pos_sigma) << ",\"rr\":" << jnum(D[k].range_rate_mps)
        << ",\"rr_s\":" << jnum(D[k].range_rate_sigma) << ",\"snr\":" << jnum(D[k].snr) << ",\"fd\":" << jnum(D[k].doppler_hz)
        << ",\"ill\":" << D[k].illuminator << "}";
  rep << "]";
  const bool image = std::chrono::duration<double>(clk::now() - last_image_).count() >= cfg_.monitor_period_s;
  if (image) {
    last_image_ = clk::now();
    std::vector<double> top((size_t)G.nx * G.ny, 0.0);
    for (size_t t = 0; t < a.tested_dopp.size(); ++t) for (size_t v = 0; v < G.size(); ++v) {
      const size_t xy = v % ((size_t)G.nx * G.ny); top[xy] = std::max(top[xy], (double)E[t * G.size() + v]); }
    rep << ",\"topview\":{\"nx\":" << G.nx << ",\"ny\":" << G.ny << ",\"x0\":" << jnum(G.origin.x) << ",\"y0\":" << jnum(G.origin.y)
        << ",\"step\":" << jnum(G.step) << ",\"db\":" << jarr(to_db(top)) << "},\"rd\":[";
    for (uint32_t i = 0; i < kCh; ++i) {
      std::vector<double> p((size_t)a.n_range * a.n_dopp);
      for (uint32_t m = 0; m < a.n_range; ++m) for (uint32_t d = 0; d < a.n_dopp; ++d) p[(size_t)m * a.n_dopp + d] = std::norm(R.rd.v[R.rd.idx(i, m, d)]);
      rep << (i ? "," : "") << "{\"ch\":" << i << ",\"nr\":" << a.n_range << ",\"nd\":" << a.n_dopp << ",\"range_m_per_bin\":" << jnum(kC * a.delay_step_s)
          << ",\"dopp0_hz\":" << jnum(a.dopp0_hz) << ",\"dopp_step_hz\":" << jnum(a.dopp_step_hz) << ",\"db\":" << jarr(to_db(p)) << "}";
    }
    rep << "]";
  } else rep << ",\"topview\":null,\"rd\":null";
  rep << "}";
  reports_.write_line(rep.str());
  std::ostringstream tr; tr << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"tracks\":[";
  for (size_t k = 0; k < T.size(); ++k) {
    const Track& x = T[k];
    tr << (k ? "," : "") << "{\"id\":" << x.id << ",\"p\":[" << jnum(x.x[0]) << "," << jnum(x.x[1]) << "," << jnum(x.x[2]) << "],\"v\":[" << jnum(x.x[3]) << ","
       << jnum(x.x[4]) << "," << jnum(x.x[5]) << "],\"s\":[" << jnum(std::sqrt(x.P[0])) << "," << jnum(std::sqrt(x.P[7])) << "," << jnum(std::sqrt(x.P[14]))
       << "],\"pe\":" << jnum(1 / (1 + std::exp(-x.llr))) << ",\"hits\":" << x.hits << ",\"age\":" << jnum(x.age_s) << ",\"confirmed\":" << (x.confirmed ? "true" : "false") << "}";
  }
  tr << "]}"; tracks_.write_line(tr.str());
  std::ostringstream co;
  co << "{\"cpi\":" << j.seq << ",\"t\":" << jnum(j.t) << ",\"phase\":" << jarr(cal.phase_rad) << ",\"phase_var\":" << jarr(cal.phase_var)
     << ",\"jitter\":" << jarr(cal.jitter_rad) << ",\"bound\":" << jarr(cal.jitter_bound_rad) << ",\"snr\":" << jarr(cal.los_snr)
     << ",\"los_found\":" << jbools(cal.los_found) << ",\"G\":" << jnum(cal.coherent_gain) << ",\"rho\":" << jnum(cal.rho)
     << ",\"af_corr_m\":" << jarr(af_->correction_norm_m()) << "}";
  coherence_.write_line(co.str());
}
} // namespace nr_isac::coherent
```

- [ ] **Step 4: Wire the engine**

In `sensing_engine.h` add `#include "coherent_pipeline.h"` and a member `std::unique_ptr<coherent::CoherentPipeline> coherent_;`. In the `SensingEngine` constructor body (after `config_` is stored), add:
```cpp
  if (config_.coherent.enable) coherent_ = std::make_unique<coherent::CoherentPipeline>(config_.coherent);
```
In `SensingEngine::process_window`, immediately after `report.midpoint_air_time_s` is computed (line ~1245), before `if (config_.spatial_receivers.configured)`:
```cpp
  if (coherent_) {   // coherent fuser replaces the per-receiver stages 1-6 at runtime (nothing removed)
    coherent_->submit(std::move(dl_window), std::move(ul_windows), sequence, report.midpoint_air_time_s);
    return;
  }
```
In `SensingEngine::stop()` (before existing joins) add `coherent_.reset();` so the worker drains and joins.
Add `coherent_pipeline.cc coherent_tracker.cc coherent_autofocus.cc coherent_ul.cc coherent_core.cc` to `NR_UE_ISAC_SRC` if not yet listed.

- [ ] **Step 5: Run the end-to-end test to verify it passes**

Run the Step 2 command. Expected: `coherent chain: ... confirmed=3` and `test_coherent_chain: PASS`. Then prove non-regression: `BUILD=... bash openair1/PHY/NR_UE_ISAC/tools/test_offline_chain.sh` — its lockstep functional part must be identical to before (coherent_enable defaults 0).

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_pipeline.h openair1/PHY/NR_UE_ISAC/coherent_pipeline.cc openair1/PHY/NR_UE_ISAC/sensing_engine.h openair1/PHY/NR_UE_ISAC/sensing_engine.cc openair1/PHY/NR_UE_ISAC/tools/test_coherent_chain.sh tests/passive_rx/ota/sensing_coherent_synth.conf CMakeLists.txt
git commit -m "sensing: coherent pipeline (CPU), JSONL reports, engine switch, offline end-to-end test"
```

---

### Task 10: CUDA path (C3, C5, C6 on one stream) + real-time test

**Files:**
- Create: `coherent_cuda.h`, `coherent_cuda.cu`, `coherent_cuda_stub.cc`, `tests/coherent_cuda_parity_test.cc`
- Modify: `coherent_pipeline.{h,cc}` (use CUDA when available), `CMakeLists.txt` (`.cu` in the `ENABLE_CHANNEL_SIM_CUDA` list with `--default-stream=per-thread`; stub in the else branch; parity test), `tools/test_coherent_chain.sh` (real-time stage)

**Interfaces:**
- Consumes: `CfrWindow`, `Axes`, `LosEstimate`, `RowSync`, `Grid`, `Geometry`, `Calibration` (CPU computes LOS/row-sync/calibration — they are O(rows·subcarriers) and cheap).
- Produces:
  ```cpp
  class CudaCoherent {
  public:
    static bool available();                         // false in the stub or with no device
    CudaCoherent();                                  // creates stream, pinned staging, cuFFT plan cache
    ~CudaCoherent();
    /** Upload once, then range profiles + static removal + NUDFT on the device. RD stays resident;
     *  los_tap and per-channel noise come back (tiny). */
    RdResult range_doppler(const CfrWindow& w, const Axes& a, const LosEstimate& L, const RowSync& s, bool download_rd);
    /** Envelope over the resident RD + per-Doppler median (CUB radix sort per segment) + threshold +
     *  local-max compaction; returns candidates only (host does NMS/harmonic merge in the same
     *  code as detect()). */
    std::vector<Detection> detect(const Grid& g, const Geometry& geo, const DetectParams& p, std::vector<float>* topview_max);
    /** Coherent refinement grid per detection on the device (same statistic as refine()). */
    void refine(std::vector<Detection>& dets, const Grid& g, const Geometry& geo, const Calibration& cal);
  };
  ```

- [ ] **Step 1: Write the failing parity test `tests/coherent_cuda_parity_test.cc`** — build the Task 5 scene (copy `make_window` + scene verbatim), run the CPU path and the CUDA path on the same window and require:
  - `max |RD_cpu − RD_gpu| / max|RD_cpu| < 1e-3` (download the RD for the test),
  - same detection count; every CUDA detection within one `G.step` of a CPU detection and |Δsnr|/snr < 1 %,
  - refined positions within `fringe/2` of the CPU ones,
  - and when `!CudaCoherent::available()`, print `SKIP (no CUDA)` and exit 0.

```cpp
// #includes (coherent_core.h, coherent_cuda.h, <cmath>, <cstdio>, <random>, <stdexcept>), require(), Path and
// make_window copied verbatim from coherent_core_test.cc.
int main() {
  if (!CudaCoherent::available()) { std::puts("coherent_cuda_parity_test: SKIP (no CUDA)"); return 0; }
  Geometry g; g.tx = {35, 20, 6};
  g.rx = {Vec3{0, 0, .5}, Vec3{10, 0, 3.5}, Vec3{0, 10, 3.5}, Vec3{10, 10, .5}};
  Volume vol; const std::array<double, kCh> ph{0, 1.1, -2.0, 0.4};
  const Vec3 t1{5, 12, 1.2}, t2{-6, -4, 1.0}, drone{8, 8, 15};
  auto taus = [&](const Vec3& p) { std::array<double, kCh> t{}; for (uint32_t i = 0; i < 4; ++i) t[i] = (dist(p, g.tx) + dist(p, g.rx[i]) + 30) / kC; return t; };
  std::array<double, kCh> los{}; for (uint32_t i = 0; i < 4; ++i) los[i] = (dist(g.tx, g.rx[i]) + 30) / kC;
  CfrWindow w = make_window(64, {{los, 1.0, 0}, {taus(t1), 0.2, 45}, {taus(t2), 0.25, -110}, {taus(drone), 0.15, 70}}, ph, 0, 128, 3);
  Axes a = derive_axes(w, vol, g, 20.0); require(a.valid, "axes");
  LosEstimate L = find_los(w, a, 1e-4); RowSync s = estimate_row_sync(w, a, L); RdResult R = range_doppler(w, a, L, s);
  Calibrator cal; Calibration c{}; for (int k = 0; k < 5; ++k) c = cal.update(R.los_tap, L.found, L.snr);
  Grid G = envelope_grid(vol, a);
  CudaCoherent gpu;
  RdResult Rg = gpu.range_doppler(w, a, L, s, true);
  double m = 0, dm = 0;
  for (size_t k = 0; k < R.rd.v.size(); ++k) { m = std::max(m, (double)std::abs(R.rd.v[k])); dm = std::max(dm, (double)std::abs(R.rd.v[k] - Rg.rd.v[k])); }
  require(dm / m < 1e-3, "RD parity");
  std::vector<Detection> Dc = detect(envelope(R, G, g), R, G, g, detect_params(a, G, 1.0));
  std::vector<Detection> Dg = gpu.detect(G, g, detect_params(a, G, 1.0), nullptr);
  require(Dc.size() == Dg.size(), "same detection count");
  for (const Detection& x : Dg) { bool ok = false; for (const Detection& y : Dc) ok |= dist(x.pos_env, y.pos_env) <= G.step && std::abs(x.snr - y.snr) / y.snr < 0.01; require(ok, "detection parity"); }
  for (Detection& d : Dc) refine(d, R, G, g, c);
  gpu.refine(Dg, G, g, c);
  Vec3 centroid{}; for (const Vec3& r : g.rx) centroid = centroid + r * 0.25;
  double D = 0; for (uint32_t i = 0; i < 4; ++i) for (uint32_t j = i + 1; j < 4; ++j) D = std::max(D, dist(g.rx[i], g.rx[j]));
  for (const Detection& xg : Dg) {
    const Detection* xc = nullptr;
    for (const Detection& y : Dc) if (dist(y.pos_env, xg.pos_env) <= G.step) xc = &y;
    require(xc != nullptr, "refine pairing");
    const double fringe = a.lambda_m * std::max(dist(xg.pos_env, centroid), G.step) / (2 * D);
    require(dist(xg.pos, xc->pos) <= fringe / 2 + 1e-9, "refine parity");
  }
  std::puts("coherent_cuda_parity_test: PASS");
}
```

- [ ] **Step 2: Run to verify it fails** — `ninja test_nr_isac_coherent_cuda_parity` → FAIL (`coherent_cuda.h` missing).

- [ ] **Step 3: Implement `coherent_cuda.cu`** — kernels (one stream `cudaStreamPerThread`; persistent device buffers grown on demand, never freed per CPI; pinned host staging):

```cuda
// 1. Per (channel,row): build the zero-padded centred row spectrum with ramp e^{+j2pi f (d_i+delta_r) - j phi_r},
//    Hann over the row's observed span, into a [ch*rows][n_fft] batch; row window sum -> wsum[ch*rows].
__global__ void build_rows(const float2* __restrict__ values, const uint8_t* __restrict__ observed,
                           const int2* __restrict__ span /*lo,hi per row*/, const double* __restrict__ los_delay /*[ch]*/,
                           const double* __restrict__ row_delay, const double* __restrict__ row_phase,
                           uint32_t rows, uint32_t nsc, uint32_t nfft, double scs, cufftComplex* __restrict__ out,
                           float* __restrict__ wsum);
// 2. cufftExecC2C(plan, out, out, CUFFT_INVERSE) over batch = ch*rows (plan cached per (n_fft,batch)).
// 3. Normalise row m by n_fft/wsum (cuFFT inverse is unnormalised -> factor 1/wsum only), crop n_range,
//    compute per (ch,range) slow-time mean (los tap = mean at range 0), subtract.
__global__ void crop_norm_mean(const cufftComplex* in, const float* wsum, uint32_t rows, uint32_t nfft, uint32_t nrange,
                               cufftComplex* prof /*[ch][range][rows]*/, cufftComplex* mean /*[ch][range]*/);
// 4. NUDFT: RD[ch][m][d] = sum_r win_r * prof[ch][m][r] * e^{-j2pi f_d t_r} / sum win.
__global__ void nudft(const cufftComplex* prof, const double* t, const float* win, float winsum, uint32_t rows,
                      uint32_t nrange, uint32_t ndopp, double f0, double df, cufftComplex* rd);
// 5. Envelope: one thread per (tested doppler index, voxel); 4 channel linear interpolations; bounds-checked
//    exactly like sample_rd(); E layout [t][v]; also atomicMax into topview[xy] (float bits, E>=0).
__global__ void envelope_k(const cufftComplex* rd, const float* inv_noise, const uint32_t* tested, uint32_t ntested,
                           uint32_t nrange, uint32_t ndopp, double delay_step, double3 origin, double step,
                           uint32_t nx, uint32_t ny, uint32_t nz, double3 tx, const double3* rx, float* E, unsigned int* topview);
// 6. Per-Doppler median: cub::DeviceSegmentedRadixSort::SortKeys on a copy of E with ntested segments of n_vox;
//    median = sorted[t*n_vox + n_vox/2]. (Same exact order statistic as the CPU median() for odd counts; for
//    even counts use the mean of the two middle elements, matching robust_stats.h.)
// 7. Threshold + compaction: E > thr_unit*med_t/med_unit -> atomicAdd append (t, v, e, med) to a candidate buffer.
//    Host: sort candidates by e, then run the SAME NMS / harmonic / z<0 loop as detect() (factor it into
//    `std::vector<Detection> finalize_candidates(...)` in coherent_core.cc and call it from both paths).
// 8. Refinement: one block per detection, threads over the (2n+1)^3 fine grid, block argmax reduction of |sum|^2;
//    host rebuilds terms/sigma with the same formulas as refine() (factor refine()'s post-argmax part into
//    `void finish_refine(Detection&, const Vec3& best, const std::array<cd,kCh>& terms, double fringe,
//    double fs, const Axes&, const Calibration&)` in coherent_core.cc and call it from both paths).
```
Implement each kernel fully; every index computed in `size_t`; every read of `rd` bounds-checked as in `sample_rd`. Timings per stage with `cudaEventRecord` on the stream. `coherent_cuda_stub.cc` defines `available()` returning false and throwing constructors/methods (never called when unavailable).

In `coherent_pipeline.cc`: construct `cuda_` in the constructor when `CudaCoherent::available()`; in `process()` use `cuda_->range_doppler(..., image)`, `cuda_->detect(...)`, `cuda_->refine(...)` when `cuda_` is set (CPU functions otherwise), set `"gpu":true`, and when `cuda_required()` (`cuda_support.h`) is true and CUDA is unavailable, `LOG_E` once and skip CPIs with reason `"cuda required but unavailable"`.

- [ ] **Step 4: Run parity + the chain test on the GPU path** — `./test_nr_isac_coherent_cuda_parity` → PASS; `test_coherent_chain.sh` → PASS with `"gpu":true` in the reports.

- [ ] **Step 5: Add the real-time stage to `test_coherent_chain.sh`** (append before the final echo):

```bash
(cd "$BUILD" && NR_ISAC_CPUS=${NR_ISAC_CPUS:-11,12,13} ./isac_replay_rt -O "$O/coherent.conf" --rows "$O/rows.bin" --realtime) > "$O/replay_rt.txt"
python3 - "$O" <<'EOF'
import glob, json, sys, numpy as np
O = sys.argv[1]
fs = sorted(glob.glob(f"{O}/coherent_reports.*.jsonl"), key=lambda p: p)
rt = [json.loads(l) for l in open(fs[-1])]                       # newest file = the real-time run
ok = [r for r in rt if r.get("skipped_reason") is None]
tot = np.array([r["timing_ms"]["total"] for r in ok]); tcpi = np.array([r["t_cpi_s"] for r in ok]) * 1e3
p95 = np.percentile(tot, 95)
assert all(r["gpu"] for r in ok), "real-time run must be on the GPU"
assert p95 < np.median(tcpi), (p95, np.median(tcpi))
assert ok[-1]["stats"]["overruns"] == 0, ok[-1]["stats"]
lock = [json.loads(l) for l in open(fs[-2])]
assert len(rt) == len(lock), (len(rt), len(lock))
print(f"real time: p95 {p95:.1f} ms vs CPI {np.median(tcpi):.1f} ms, {len(rt)} CPIs, queue_waits {ok[-1]['stats']['queue_waits']}")
EOF
```
Expected: PASS with p95 well under the CPI duration.

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_ISAC/coherent_cuda.h openair1/PHY/NR_UE_ISAC/coherent_cuda.cu openair1/PHY/NR_UE_ISAC/coherent_cuda_stub.cc openair1/PHY/NR_UE_ISAC/tests/coherent_cuda_parity_test.cc openair1/PHY/NR_UE_ISAC/coherent_core.h openair1/PHY/NR_UE_ISAC/coherent_core.cc openair1/PHY/NR_UE_ISAC/coherent_pipeline.h openair1/PHY/NR_UE_ISAC/coherent_pipeline.cc openair1/PHY/NR_UE_ISAC/tools/test_coherent_chain.sh CMakeLists.txt
git commit -m "sensing: coherent CUDA path (resident CPI, NUDFT, envelope, median CFAR, refinement) + real-time test"
```

---

### Task 11: Launcher, manual template, monitor coherent page

**Files:**
- Create: `tests/passive_rx/monitor/coherent_view.py`, `tests/passive_rx/monitor/coherent.html`, `tests/passive_rx/monitor/test_coherent_view.py`
- Modify: `tests/passive_rx/monitor/monitor.py` (register `/coherent` + `/api/coherent` when `--coherent-dir` is given), `tests/passive_rx/run_sensing.sh`, `tests/passive_rx/ota/sensing_ota_manual.conf.template`

**Interfaces:**
- Consumes: the three JSONL schemas from Task 9 (files `<dir>/coherent_{reports,tracks}.*.jsonl`, `<dir>/coherence.*.jsonl`).
- Produces: `class CoherentView(dir, maxlines=4000)` with `.snapshot() -> dict` = `{"geometry":{...}, "reports":[last 1 full + last 200 compact], "tracks": last, "track_trails": {id: [[t,x,y,z],...]} (last 20 s), "coherence":[last 400], "health": {...}}`; always follows the NEWEST file of each stem (`max(glob, key=mtime)`).

- [ ] **Step 1: Write the failing test `test_coherent_view.py`**

```python
#!/usr/bin/env python3
import json, os, tempfile, time
from coherent_view import CoherentView
with tempfile.TemporaryDirectory() as d:
    def w(stem, stamp, rows):
        with open(f"{d}/{stem}.{stamp}.jsonl", "a") as f:
            for r in rows: f.write(json.dumps(r) + "\n")
    w("coherent_reports", "A", [{"cpi": 1, "t": 0.1, "detections": [], "timing_ms": {"total": 5}, "t_cpi_s": 0.075, "stats": {}, "gpu": True, "topview": None, "rd": None}])
    time.sleep(0.02)
    w("coherent_reports", "B", [{"cpi": 7, "t": 0.5, "detections": [{"p": [1, 2, 3]}], "timing_ms": {"total": 6}, "t_cpi_s": 0.075, "stats": {}, "gpu": True, "topview": None, "rd": None}])
    w("coherent_tracks", "B", [{"cpi": 7, "t": 0.5, "tracks": [{"id": 3, "p": [1, 2, 3], "v": [0, 0, 0], "confirmed": True, "pe": 0.99}]}])
    w("coherence", "B", [{"cpi": 7, "t": 0.5, "phase": [0, 1, 2, 3], "G": 3.9, "rho": 0.97}])
    v = CoherentView(d, geometry={"gnb": [35, 20, 6], "rx": [[0, 0, .5], [10, 0, 3.5], [0, 10, 3.5], [10, 10, .5]], "volume": [-15, 15, -15, 15, 0, 30]})
    time.sleep(0.5); s = v.snapshot()
    assert s["reports"][-1]["cpi"] == 7, "follows the newest file"
    assert s["tracks"]["tracks"][0]["id"] == 3 and "3" in {str(k) for k in s["track_trails"]}
    assert s["coherence"][-1]["G"] == 3.9 and s["health"]["gpu"] is True
print("test_coherent_view: PASS")
```

- [ ] **Step 2: Run to verify it fails** — `cd tests/passive_rx/monitor && python3 test_coherent_view.py` → FAIL (module missing).

- [ ] **Step 3: Implement `coherent_view.py`** (reuse `monitor.py`'s `tail_file`/`json_lines` helpers by importing them; a background thread per stem re-globs every 1 s and switches to a newer file when one appears; bounded deques; trails pruned to the last 20 s of air time).

```python
#!/usr/bin/env python3
"""Coherent fuser view: tails the newest coherent_{reports,tracks}/coherence JSONL in a run dir."""
import collections, glob, json, os, threading, time

class _Follower(threading.Thread):
    def __init__(self, pattern, on_line):
        super().__init__(daemon=True); self.pattern, self.on_line, self.path, self.pos = pattern, on_line, None, 0
    def run(self):
        while True:
            files = glob.glob(self.pattern)
            if files:
                newest = max(files, key=os.path.getmtime)
                if newest != self.path: self.path, self.pos = newest, 0
                with open(self.path) as f:
                    f.seek(self.pos)
                    for line in f:
                        if not line.endswith("\n"): break
                        self.pos += len(line)
                        try: self.on_line(json.loads(line))
                        except json.JSONDecodeError: pass
            time.sleep(0.2)

class CoherentView:
    def __init__(self, run_dir, geometry=None, maxlines=4000):
        self.geometry = geometry or {}
        self.lock = threading.Lock()
        self.compact = collections.deque(maxlen=200); self.full = None
        self.tracks = None; self.trails = collections.defaultdict(lambda: collections.deque(maxlen=400))
        self.coh = collections.deque(maxlen=400)
        for stem, cb in (("coherent_reports", self._rep), ("coherent_tracks", self._trk), ("coherence", self._coh)):
            _Follower(os.path.join(run_dir, f"{stem}.*.jsonl"), cb).start()
    def _rep(self, r):
        with self.lock:
            if r.get("topview") is not None or self.full is None: self.full = r
            self.compact.append({k: r.get(k) for k in ("cpi", "t", "detections", "timing_ms", "t_cpi_s", "stats", "gpu", "range_res_m", "skipped_reason")})
    def _trk(self, r):
        with self.lock:
            self.tracks = r; t = r.get("t", 0)
            for x in r.get("tracks", []): self.trails[x["id"]].append([t] + x["p"])
            for k in list(self.trails):
                while self.trails[k] and t - self.trails[k][0][0] > 20.0: self.trails[k].popleft()
                if not self.trails[k]: del self.trails[k]
    def _coh(self, r):
        with self.lock: self.coh.append(r)
    def snapshot(self):
        with self.lock:
            last = self.compact[-1] if self.compact else {}
            health = {"gpu": last.get("gpu"), "total_ms": (last.get("timing_ms") or {}).get("total"),
                      "cpi_ms": (last.get("t_cpi_s") or 0) * 1e3, "stats": last.get("stats")}
            reps = list(self.compact)
            if self.full is not None: reps = reps[:-1] + [dict(reps[-1], topview=self.full.get("topview"), rd=self.full.get("rd"))] if reps else [self.full]
            return {"geometry": self.geometry, "reports": reps, "tracks": self.tracks,
                    "track_trails": {str(k): list(v) for k, v in self.trails.items()},
                    "coherence": list(self.coh), "health": health}
```
`coherent.html`: one page, vendored Plotly (`/vendor/plotly-2.35.2.min.js` as `sensing.html` uses), polling `/api/coherent` every 500 ms:
- **3D scene** (`scatter3d`, `uirevision:"keep-camera"`): antennas (squares, labelled ch0–3), gNB (diamond), volume box (12 line segments), current detections (points, size ∝ log10 snr, hover p/rr/snr/s), track trails (lines per id) + head markers coloured by `pe` (confirmed solid, tentative 30 % opacity) + velocity cones/arrows (`p` → `p + v`).
- **Top view** (`heatmap` of `topview.db`, x/y from `x0,y0,step`) with detections overlaid.
- **RD maps** (4 `heatmap`s, axes from `range_m_per_bin`, `dopp0_hz`, `dopp_step_hz`).
- **Coherence** (4 phase traces vs time, jitter vs bound per channel, `G` and `rho` traces, autofocus correction).
- **Health strip** (text): total ms vs CPI ms, GPU/CPU, processed/skipped/overruns/queue_waits, last skipped reason.

In `monitor.py`: add `--coherent-dir DIR` and `--geometry` (already exists); when set, create `CoherentView(DIR, geometry=json.load(open(geometry)))` and serve `GET /coherent` → `coherent.html`, `GET /api/coherent` → `json.dumps(view.snapshot())` (mirror the existing `/sensing` handler code path; do not change existing routes).

- [ ] **Step 4: Launcher and template**

`run_sensing.sh`:
1. Detect coherent mode: `COH=$(grep -Eq '^\s*coherent_enable\s*=\s*1' "$RUN/ue.conf" && echo 1 || echo 0)` right after the survey `--apply`.
2. In coherent mode do **not** start `realtime_chain.py` (keep the code, guard it with `if [ "$COH" = 0 ]`), and start the monitor with `--coherent-dir "$RUN" --geometry "$RUN/geometry.json"` in addition to the existing args.
3. Retry handling: replace any `: > file` / `> file` truncation of chain outputs (`status.jsonl`, `tracks.jsonl`) on retry with a rename to `<name>.tryN.jsonl` (append-only history) — grep the file for `>` redirections to those names and fix each.
4. Verdict in coherent mode: `cpis` = number of lines over all `coherent_reports.*.jsonl` in `$RUN` whose `skipped_reason` is null; `valid` additionally requires the last report's `stats.overruns == 0`.

`sensing_ota_manual.conf.template` (inside `sensing = {`), add:
```
  // ===== COHERENT FUSER (DL only) =====
  gate_require_ul = 0;
  sources = "pdsch_dmrs_blind,pdsch_data";
  coherent_enable = 1;
  coherent_ul_enable = 0;
  coherent_volume_m = "-15:15:-15:15:0:30";
  coherent_survey_sigma_m = 0.1;
```
(Replace the existing `sources = ...` line rather than duplicating the key — libconfig rejects duplicates.)

- [ ] **Step 5: Run the tests**

`python3 tests/passive_rx/monitor/test_coherent_view.py` → PASS; `python3 tests/passive_rx/monitor/test_monitor.py` → still PASS; `bash -n tests/passive_rx/run_sensing.sh`; then a dry monitor check against the Task 9 output: `python3 tests/passive_rx/monitor/monitor.py --port 8091 --coherent-dir /tmp/isac_coherent --geometry /tmp/isac_coherent/g.json & sleep 3; curl -s localhost:8091/api/coherent | python3 -c "import json,sys; s=json.load(sys.stdin); assert s['reports'] and s['coherence']; print('monitor api ok')"; kill %1`.

- [ ] **Step 6: Commit**

```bash
git add tests/passive_rx/monitor/coherent_view.py tests/passive_rx/monitor/coherent.html tests/passive_rx/monitor/test_coherent_view.py tests/passive_rx/monitor/monitor.py tests/passive_rx/run_sensing.sh tests/passive_rx/ota/sensing_ota_manual.conf.template
git commit -m "passive rx: coherent monitor page, coherent launcher mode (chain unwired), no log truncation on retry"
```

---

## Final verification (after all tasks)

1. `build_sense` and `build_rx` full builds, all `test_nr_isac_coherent_*` tests, `test_nr_isac_flow_gate`, `test_nr_llr_confidence` pass.
2. `test_offline_chain.sh` (existing pipeline, `coherent_enable = 0`) — functional part unchanged.
3. `test_coherent_chain.sh` — offline + real-time PASS on the GPU.
4. `test_make_coherent_scene.py`, `test_coherent_view.py`, `test_monitor.py` PASS.
5. `git status` clean; no file deleted (`git diff --diff-filter=D --name-only <base>..HEAD` is empty).
