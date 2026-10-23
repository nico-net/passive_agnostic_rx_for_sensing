#!/usr/bin/env python3
"""Receiver stdout -> privacy-policed telemetry datasets.

  nr-uesoftmodem ... 2>&1 | python3 tools/mobisys/export.py --experiment-mode commercial --out DIR
  python3 tools/mobisys/export.py --experiment-mode lab --out DIR --input run.log      (lab only)

Commercial mode accepts only a pipe on stdin (a regular file means the raw log was already persisted), keeps
60-s windows with k-suppression and never writes RNTIs, per-grant rows or raw text. Error text never echoes input.
Exit: 0 ok, 2 input/usage refused, 3 policy violation(s) (rows refused), 4 internal error.
"""
import argparse
import os
import platform
import queue
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agg  # noqa: E402
import policy  # noqa: E402
import store  # noqa: E402

SCHEMA = "mobisys-telemetry/1"
RX_COMM = "nr-uesoftmodem"


def _scrub_hook(exc_type, exc, tb):  # no traceback: it would print source lines / values
    sys.stderr.write("exporter: internal error (%s)\n" % exc_type.__name__)


sys.excepthook = _scrub_hook
threading.excepthook = lambda a: _scrub_hook(a.exc_type, a.exc_value, a.exc_traceback)


class Pipeline:
    def __init__(self, a):
        self.a = a
        self.sink = store.Sink(a.out, a.experiment_mode, a.format, a.rotate_rows)
        self.q = queue.Queue(maxsize=a.queue_size)
        self.agg = agg.Aggregator(self._emit, a.experiment_mode, a.slots_per_frame)
        self.lock = threading.Lock()
        self.drops = {}
        self.parse_errors = 0
        self.violations = 0
        self.stop = threading.Event()
        self.t0, self.wall0 = time.monotonic(), time.time()

    def _emit(self, dataset, row):
        try:
            self.q.put_nowait((dataset, row))
        except queue.Full:
            self.drops[dataset] = self.drops.get(dataset, 0) + 1

    def reader(self, stream):
        feed, stamped, base = self.agg.feed_line, self.a.line_time == "unix", None
        while not self.stop.is_set():
            raw = stream.readline(65536)
            if not raw:
                break
            now, t_s = time.time(), time.monotonic() - self.t0
            line = raw.decode("utf-8", "replace")
            if stamped:  # harness prefix "<unix seconds> " (stamp.py style): stripped before parsing
                head, _, rest = line.partition(" ")
                try:
                    now, line = float(head), rest
                    base = now if base is None else base
                    t_s = now - base
                except ValueError:
                    pass
            try:
                with self.lock:
                    feed(line, now, t_s)
            except Exception:  # noqa: BLE001 - class name only; the message could hold input text
                self.parse_errors += 1

    def writer(self):
        last = time.monotonic()
        while True:
            try:
                item = self.q.get(timeout=0.5)
            except queue.Empty:
                item = False
            if item is None:
                break
            if item:
                try:
                    self.sink.write(*item)
                except policy.PolicyViolation:
                    self.violations += 1
            if time.monotonic() - last >= self.a.flush_s:
                self.sink.flush()
                last = time.monotonic()

    def sampler(self):
        prev_host = prev_rx = None
        pid, pid_t = None, 0.0
        hz = os.sysconf("SC_CLK_TCK")
        gpu_t, gpu = 0.0, (None, None, None)
        while not self.stop.wait(1.0):
            row = {"t_s": round(time.monotonic() - self.t0, 3), "src": "host"}
            try:
                with open("/proc/stat") as f:
                    v = [int(x) for x in f.readline().split()[1:]]
                tot, idle = sum(v), v[3] + (v[4] if len(v) > 4 else 0)
                if prev_host:
                    row["host_cpu_pct"] = round(100.0 * (1 - (idle - prev_host[1]) / max(1, tot - prev_host[0])), 2)
                prev_host = (tot, idle)
                with open("/proc/meminfo") as f:
                    for ln in f:
                        if ln.startswith("MemAvailable:"):
                            row["mem_avail_kb"] = int(ln.split()[1])
                if pid is None and time.monotonic() - pid_t > 10:
                    pid_t = time.monotonic()
                    pid = next((int(d) for d in os.listdir("/proc") if d.isdigit() and _comm(d) == RX_COMM), None)
                if pid is not None:
                    with open("/proc/%d/stat" % pid) as f:
                        s = f.read().rsplit(")", 1)[1].split()
                    cpu = (int(s[11]) + int(s[12])) / hz
                    if prev_rx is not None:
                        row["rx_cpu_pct"] = round(100.0 * (cpu - prev_rx[0]) / max(1e-6, time.monotonic() - prev_rx[1]), 2)
                    prev_rx = (cpu, time.monotonic())
                    row["rx_threads"], row["rx_rss_kb"] = int(s[17]), int(s[21]) * (os.sysconf("SC_PAGE_SIZE") // 1024)
            except (OSError, ValueError, IndexError):
                pid, prev_rx = None, None
            if shutil.which("nvidia-smi") and time.monotonic() - gpu_t > 5:
                gpu_t = time.monotonic()
                try:
                    out = subprocess.run(["nvidia-smi", "--query-gpu=utilization.gpu,memory.used,temperature.gpu",
                                          "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=2).stdout
                    gpu = tuple(float(x) for x in out.splitlines()[0].split(","))
                except (OSError, ValueError, IndexError, subprocess.SubprocessError):
                    gpu = (None, None, None)
            row["gpu_util_pct"], row["gpu_mem_mb"], row["gpu_temp_c"] = gpu
            self._emit("system_telemetry", row)

    def finish(self, extra_manifest):
        with self.lock:
            self.agg.close()
        try:
            self.q.put(None, timeout=5)
        except queue.Full:  # writer died; its exception was already reported by the thread hook
            pass
        self.wt.join(10)
        self.sink.flush()
        loss = {"queue_drops": dict(self.drops), "queue_drops_total": sum(self.drops.values()),
                "policy_refused_rows": self.sink.refused_rows, "policy_refused_fields": self.sink.refused_fields,
                "parse_errors": self.parse_errors,
                "k_suppressed_rows": self.agg.c["suppressed_rows"], "k_suppressed_events": self.agg.c["suppressed_events"]}
        summ = self.agg.summary()
        summ.update({"schema": SCHEMA, "mode": self.a.experiment_mode, "provenance": self.a.provenance, "telemetry_loss": loss})
        self.sink.write_json("summary.json", summ)
        try:
            import pyarrow
            pav = pyarrow.__version__
        except Exception:  # noqa: BLE001
            pav = None
        man = {"schema": SCHEMA, "mode": self.a.experiment_mode, "provenance": self.a.provenance,
               "host_label": self.a.host_label, "format": self.sink.format, "pyarrow": pav,
               "python": platform.python_version(), "k": self.agg.k, "window_s": policy.WINDOW_S,
               "slots_per_frame": self.a.slots_per_frame, "rotate_rows": self.a.rotate_rows,
               "started_unix": round(self.wall0, 3), "duration_s": round(time.monotonic() - self.t0, 3),
               "datasets": {d: n for d, n in sorted(self.sink.parts.items())}, "rows_written": self.sink.rows_written,
               "telemetry_loss": loss, "clock_offset_real_minus_mono_ns": summ["clock_offset_real_minus_mono_ns"]}
        man.update(extra_manifest)
        self.sink.write_json("manifest.json", man)
        self.sink.close()
        return 3 if self.violations or self.sink.refused_rows else 0


def _comm(d):
    try:
        with open("/proc/%s/comm" % d) as f:
            return f.read().strip()
    except OSError:
        return None


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--experiment-mode", choices=policy.MODES, default="commercial")
    p.add_argument("--out", required=True)
    p.add_argument("--input", help="lab only: read this file instead of stdin")
    p.add_argument("--provenance", choices=("live", "replay", "synthetic"), default="live")
    p.add_argument("--host-label", default="unlabelled", help="evidence label host, e.g. 'DGX aarch64' or 'cloud x86'")
    p.add_argument("--format", choices=("auto", "parquet", "jsonl"), default="auto")
    p.add_argument("--rotate-rows", type=int, default=50000)
    p.add_argument("--flush-s", type=float, default=10.0)
    p.add_argument("--queue-size", type=int, default=100000)
    p.add_argument("--slots-per-frame", type=int, default=20)
    p.add_argument("--line-time", choices=("arrival", "unix"), default="arrival",
                   help="unix: lines carry a '<unix seconds> ' harness prefix to use as time (replay/synthetic)")
    p.add_argument("--no-sampler", action="store_true")
    return p


def main(argv=None, stdin=None):
    a = build_parser().parse_args(argv)
    commercial = a.experiment_mode == "commercial"
    if a.input is not None:
        if commercial:
            sys.stderr.write("exporter: --input is not allowed in commercial mode; pipe the receiver into stdin\n")
            return 2
        stream, owned = open(a.input, "rb"), True
    else:
        stream, owned = stdin or sys.stdin.buffer, False
        mode = os.fstat(stream.fileno()).st_mode
        if not (stat.S_ISFIFO(mode) or (not commercial and stat.S_ISREG(mode))):
            sys.stderr.write("exporter: stdin must be a pipe (receiver stdout piped in); a regular file or terminal is "
                             "refused%s\n" % (" in commercial mode" if commercial else ""))
            return 2
    os.makedirs(a.out, exist_ok=True)
    pl = Pipeline(a)
    signal.signal(signal.SIGTERM, lambda *_: pl.stop.set())
    pl.wt = threading.Thread(target=pl.writer, daemon=True)
    pl.wt.start()
    rt = threading.Thread(target=pl.reader, args=(stream,), daemon=True)
    rt.start()
    if not a.no_sampler:
        threading.Thread(target=pl.sampler, daemon=True).start()
    try:
        while rt.is_alive() and not pl.stop.is_set():
            rt.join(0.5)
    except KeyboardInterrupt:
        pass
    pl.stop.set()
    rt.join(1.0)
    if owned:
        stream.close()
    return pl.finish({"input": "file" if owned else "stdin"})


def cli():
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except BaseException as e:  # noqa: BLE001
        sys.stderr.write("exporter: internal error (%s)\n" % type(e).__name__)
        sys.exit(4)


if __name__ == "__main__":
    cli()
