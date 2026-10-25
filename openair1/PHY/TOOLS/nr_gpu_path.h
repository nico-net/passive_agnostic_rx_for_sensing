/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Per-path GPU/CPU choice (sens6 per-path, 2026-10-24).
 *
 * WHY: on a DISCRETE tier (PCIe GPU, explicit copies) a GPU-offloaded path is not automatically faster than the CPU path: every call pays
 * H2D/D2H copies and a host wait. Each path therefore runs where it is faster on this host, decided once per process:
 *   - by a short timed self-benchmark at init (nr_gpu_path_finish() with the two medians), or
 *   - by an online trial on the first live calls (nr_gpu_path_pick() / nr_gpu_path_sample(): the first calls alternate or are paired, the
 *     medians of the measured times decide).
 * Fail closed: a benchmark that cannot be measured, a GPU error during the trial, or a trial that never completes -> CPU.
 * SHARED tier (DGX GB10, integrated): every path stays on the GPU without any measurement (unchanged behaviour), unless NR_GPU_PATH_SELECT=bench.
 * CPU tier: CPU. One log line per path when its choice is final ("SENSING: GPU_PATH path=<name> tier=<tier> choice=gpu|cpu reason=<r> ...");
 * a path that enters an online trial logs "choice=trial" once at init and its final line when the trial ends.
 *
 * Environment (read once per path, at nr_gpu_path_begin()):
 *   NR_GPU_PATH_SELECT=auto|bench|off  auto (default): measure on DISCRETE only; bench: also on SHARED; off: no selection (GPU wherever the
 *                                       module is ready: the behaviour before this layer).
 *   NR_GPU_PATH_<NAME>=gpu|cpu|auto    per path (NAME = FEP, CHEST, DEMOD, LDPC_DL, LDPC_UL, PASS_DL, PASS_UL, UL_FEP): forces the choice
 *                                       (gpu still needs a ready GPU module; logged reason=env).
 *   NR_GPU_PATH_TRIAL=<n>               samples per side for a decision (default 8, 2..64).
 *   NR_GPU_PATH_MARGIN=<x>              the GPU wins iff median_gpu < x * median_cpu (default 1.0, 0.1..10).
 * No CUDA dependency: part of libnr_gpu_platform (one instance shared by the core and the dlopen GPU modules). */
#ifndef NR_GPU_PATH_H
#define NR_GPU_PATH_H
#include "nr_gpu_platform.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  NR_GPU_PATH_FEP,     /* DL bit-exact FEP (standalone exact FEP entry) */
  NR_GPU_PATH_CHEST,   /* DL standalone exact chest / delay */
  NR_GPU_PATH_DEMOD,   /* DL standalone demod */
  NR_GPU_PATH_LDPC_DL, /* DL standalone GPU LDPC (GPU_EXACT_LDPC) */
  NR_GPU_PATH_LDPC_UL, /* UL GPU LDPC (UL_GPU_LDPC) */
  NR_GPU_PATH_PASS_DL, /* DL graph pass (GPUPASS) */
  NR_GPU_PATH_PASS_UL, /* UL graph pass */
  NR_GPU_PATH_UL_FEP,  /* UL standalone FEP */
  NR_GPU_PATH_N
} nr_gpu_path_t;

enum { NR_GPU_PATH_CPU = 0, NR_GPU_PATH_GPU = 1, NR_GPU_PATH_TRIAL = 2, NR_GPU_PATH_UNSET = -1 };
enum { NR_GPU_PATH_SEL_AUTO = 0, NR_GPU_PATH_SEL_BENCH = 1, NR_GPU_PATH_SEL_OFF = 2 };
/* how the path can be measured (argument of nr_gpu_path_begin) */
enum { NR_GPU_PATH_MEASURE_NONE = 0, NR_GPU_PATH_MEASURE_BENCH = 1, NR_GPU_PATH_MEASURE_TRIAL = 2 };

const char *nr_gpu_path_name(nr_gpu_path_t p);
/* ---- pure policy (unit-tested without a device) ---- */
/* "gpu" -> GPU, "cpu" -> CPU, NULL/""/"auto" -> UNSET (automatic), anything else -> -2 (ignored, warned) */
int nr_gpu_path_parse_override(const char *v);
/* "auto"/NULL/"" -> AUTO, "bench" -> BENCH, "off" -> OFF, else -1 (ignored, warned) */
int nr_gpu_path_parse_select(const char *v);
/* First decision of a path: CPU, GPU, or TRIAL (= measure: a benchmark or an online trial follows). *reason names the rule:
 *   gpu_unavailable (module not ready; overrides everything), env (override), select_off, cpu_tier, shared (SHARED tier without bench),
 *   direct_only (no DISCRETE implementation of this entry: measure == NONE on a non-SHARED tier: fail closed), measure. */
int nr_gpu_path_policy_begin(nr_gpu_platform_model_t tier, int select, int override, int gpu_ok, int measure, const char **reason);
/* Verdict from medians: GPU iff both are finite and > 0, n_gpu, n_cpu >= n_min and gpu_us < margin * cpu_us ("faster"); "slower" else;
 * "bench_fail" for invalid measurements (fail closed: CPU). */
int nr_gpu_path_policy_verdict(double gpu_us, double cpu_us, int n_gpu, int n_cpu, int n_min, double margin, const char **reason);
/* median of n values (copies, n <= 64); NAN for n < 1 */
double nr_gpu_path_median(const double *v, int n);

/* ---- process-wide registry (thread-safe) ---- */
/* Decides the path's first state with the platform tier and the environment; logs a final choice (or "choice=trial") once. Idempotent: a
 * second call returns the current state. gpu_ok = the GPU entry of the path is ready (module loaded and initialised). */
int nr_gpu_path_begin(nr_gpu_path_t p, int gpu_ok, int measure);
/* The same with an explicit tier (tests; the receiver uses nr_gpu_path_begin) */
int nr_gpu_path_begin_tier(nr_gpu_path_t p, nr_gpu_platform_model_t tier, int gpu_ok, int measure);
/* Ends a TRIAL with an init benchmark: medians gpu_us / cpu_us over n runs each (n < 1 or invalid values = bench_fail -> CPU). Logs. No-op
 * (returns the state) when the path is not in TRIAL. */
int nr_gpu_path_finish(nr_gpu_path_t p, double gpu_us, double cpu_us, int n, const char *detail);
/* Current state: CPU, GPU, TRIAL, or UNSET (never begun). Lock-free. */
int nr_gpu_path_state(nr_gpu_path_t p);
/* 1 unless the path's state is CPU (UNSET, GPU and TRIAL return 1): the cheap test for callers that only need "may the GPU run". */
int nr_gpu_path_use_gpu(nr_gpu_path_t p);
/* Online trial, alternating calls: during TRIAL returns 1 (run the GPU side) or 0 (run the CPU side) alternately; outside TRIAL returns
 * nr_gpu_path_use_gpu(). A trial that has handed out 32 x n_min picks without reaching a decision ends as CPU (trial_incomplete). */
int nr_gpu_path_pick(nr_gpu_path_t p);
/* Online trial sample: the time of one call on the GPU (on_gpu = 1) or CPU side, in the caller's unit (us, or us per work unit). Ignored
 * outside TRIAL and for non-finite / negative values. When both sides hold n_min samples, the medians decide (logged). */
void nr_gpu_path_sample(nr_gpu_path_t p, int on_gpu, double t);
/* A GPU error on the path while it is not final (TRIAL): -> CPU (reason=gpu_error), logged. A GPU path already chosen is left to the module's
 * own breaker. */
void nr_gpu_path_fail(nr_gpu_path_t p, const char *why);
/* Trial parameters in force (after the environment): samples per side, margin */
int nr_gpu_path_trial_n(void);
double nr_gpu_path_margin(void);
/* Tests: forget every decision and re-read the environment at the next begin. */
void nr_gpu_path_test_reset(void);
/* Tests: the last log line written by the registry */
const char *nr_gpu_path_test_last_line(void);
#ifdef __cplusplus
}
#endif
#endif
