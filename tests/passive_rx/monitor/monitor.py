#!/usr/bin/env python3
"""Receiver dashboard: a read-only web page over the passive receiver's own outputs.

Standard library only. It tails files the receiver already writes and never writes into a run directory,
never starts/stops the radio and never changes receiver flags.

  live, next to a running receiver (run directory written by the launcher):
      ./monitor.py --run /path/to/run_dir                       # run.log + metrics.jsonl + obs.jsonl + sense files
  live, explicit files:
      ./monitor.py --log run.log --metrics metrics.jsonl --obs obs.jsonl [--records rec.jsonl --cfr cfr.bin]
  follow the newest run directory of a campaign:
      ./monitor.py --follow '/home/nicola/NICOLA/ota_runs/*'
  post-mortem / replay / A-B compare:
      ./monitor.py --run DIR --runs-root '/home/nicola/NICOLA/ota_runs/*' --scan cells.json

Then open http://127.0.0.1:8080/ (ssh -L 8080:localhost:8080 <host> from a laptop). --bind changes the address.
"""

import argparse
import glob
import gzip
import hashlib
import json
import math
import os
import re
import struct
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import rxparse

HERE = Path(__file__).resolve().parent
LIVE_CATCHUP_BYTES = 8 << 20      # live tail further behind than this skips to the present (counted, shown)
HISTORY_CACHE = 6                 # finished runs kept parsed in memory


# --------------------------------------------------------------------------- JSONL tail (kept: /health + tests)

class JsonlTail:
    """Incremental JSONL reader: tolerates a missing file, a partial last line, truncation/rotation.

    Memory bound: a poll never reads more than MAX_CATCHUP_BYTES. If more than that is waiting
    (first poll of a huge existing file, or a stalled poller) it seeks to the tail and drops the
    first, partial line -- the dashboard wants the present, not the whole history."""
    MAX_CATCHUP_BYTES = 4 * 1024 * 1024

    def __init__(self, path, keep=512):
        self.path, self.keep = path, keep
        self.pos, self.pending, self.last, self.history, self.bad_lines = 0, b"", None, [], 0
        self.ino, self.mtime = None, None

    def poll(self):
        if not self.path:
            return []
        try:
            st = os.stat(self.path)
        except OSError:
            self.pos, self.pending, self.ino = 0, b"", None  # gone (rotated): re-read from 0 when it returns
            self.history, self.last = [], None
            return []
        self.mtime = st.st_mtime
        if st.st_size < self.pos or (self.ino is not None and st.st_ino != self.ino):
            self.pos, self.pending = 0, b""  # truncated or rotated: drop samples of the old file
            self.history, self.last = [], None
        self.ino = st.st_ino
        skip_first = False
        if st.st_size - self.pos > self.MAX_CATCHUP_BYTES:
            self.pos, self.pending, skip_first = st.st_size - self.MAX_CATCHUP_BYTES, b"", True
        try:
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                data = self.pending + f.read(self.MAX_CATCHUP_BYTES)
                self.pos = f.tell()
        except OSError:
            return []
        lines = data.split(b"\n")
        self.pending = lines.pop()  # b"" if data ended with a newline
        if skip_first and lines:
            lines.pop(0)  # we landed mid-line
        elif skip_first:
            self.pending = b""
        new = []
        for l in lines:
            if not l.strip():
                continue
            try:
                obj = json.loads(l.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                self.bad_lines += 1
                continue
            if not isinstance(obj, dict):
                self.bad_lines += 1
                continue
            new.append(obj)
        if new:
            self.history = (self.history + new)[-self.keep:]
            self.last = self.history[-1]
        return new


def _num(d, k):
    v = d.get(k)
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) else None


def health_snapshot(metrics, obs):
    """GET /health: compact machine-readable health (kept for the A5 consumers and test_health.py)."""
    h = {"metrics": metrics.last, "metrics_age_s": None, "bad_lines": metrics.bad_lines + obs.bad_lines,
         "rates": {"crc_pct_window": None, "grants_per_s": None, "drop_full_pct": None},
         "obs": {"dl_per_s": 0.0, "ul_per_s": 0.0, "top_rnti": [], "prb_hist": [0] * 10}}
    if metrics.last is not None and metrics.mtime is not None:
        h["metrics_age_s"] = max(0.0, time.time() - metrics.mtime)
    if len(metrics.history) >= 2:
        a, b = metrics.history[-2], metrics.history[-1]
        g = lambda k: (_num(a, k), _num(b, k))
        (t0, t1), (d0, d1), (c0, c1), (q0, q1), (f0, f1) = (
            g("t_mono_ns"), g("pdschq_decoded"), g("pdschq_crc_ok"), g("scanq_queued"), g("scanq_drop_full"))
        # A negative delta means a counter reset (receiver restart): no rate for that window.
        if any(x is not None and y is not None and y < x for x, y in ((d0, d1), (c0, c1), (q0, q1), (f0, f1))):
            d0 = None  # nulls the pdsch rates
            q0 = None  # nulls the drop rate
        if None not in (d0, d1, c0, c1) and d1 - d0 > 0:
            h["rates"]["crc_pct_window"] = 100.0 * (c1 - c0) / (d1 - d0)
        if None not in (t0, t1, d0, d1) and t1 > t0:
            h["rates"]["grants_per_s"] = (d1 - d0) / ((t1 - t0) / 1e9)
        if None not in (q0, q1, f0, f1) and q1 - q0 > 0:
            h["rates"]["drop_full_pct"] = 100.0 * (f1 - f0) / (q1 - q0)
    recent = [o for o in obs.history[-2000:] if _num(o, "t_mono_ns") is not None]
    if len(recent) >= 2:
        span = max((recent[-1]["t_mono_ns"] - recent[0]["t_mono_ns"]) / 1e9, 1e-9)
        h["obs"]["dl_per_s"] = sum(o.get("dir") == "DL" for o in recent) / span
        h["obs"]["ul_per_s"] = sum(o.get("dir") == "UL" for o in recent) / span
        cnt = {}
        for o in recent:
            r = o.get("rnti")
            if isinstance(r, int):
                cnt[r] = cnt.get(r, 0) + 1
            nb = _num(o, "nb_rb")  # unknown values are JSON null
            if nb is not None:
                h["obs"]["prb_hist"][min(9, max(0, int(nb) * 10 // 275))] += 1
        h["obs"]["top_rnti"] = [list(kv) for kv in sorted(cnt.items(), key=lambda kv: -kv[1])[:5]]
    return h


# --------------------------------------------------------------------------- run files

ANCHOR = re.compile(rb'ISAC_METRICS \{.*?"t_mono_ns":\s*(\d+)')


def _open(path):
    return gzip.open(path, "rb") if str(path).endswith(".gz") else open(path, "rb")


class TimeMap:
    """Log byte offset -> run time, from the t_mono_ns of the ISAC_METRICS lines (one every 20 s).

    shortcut: linear interpolation between 20 s anchors (log lines carry no time of their own); lines get the
    right 20 s window but not sub-window precision. Upgrade when the receiver stamps its lines."""

    def __init__(self):
        self.off, self.t = [], []

    def add(self, off, tm_ns):
        if self.off and off <= self.off[-1]:
            return
        if self.t and tm_ns < self.t[-1]:
            return               # counter restart inside one log: keep the first clock
        self.off.append(off)
        self.t.append(tm_ns)

    def at(self, off):
        """t_mono_ns of a byte offset (None without anchors)."""
        n = len(self.off)
        if n == 0:
            return None
        if n == 1:
            return self.t[0]
        import bisect
        i = bisect.bisect_right(self.off, off)
        i = min(max(i, 1), n - 1)
        o0, o1, t0, t1 = self.off[i - 1], self.off[i], self.t[i - 1], self.t[i]
        tm = t0 + (t1 - t0) * (off - o0) / max(1, o1 - o0)
        if off > self.off[-1]:
            tm = min(tm, self.t[-1] + 25e9)   # beyond the last anchor: at most one period ahead
        return tm

    def start(self):
        return self.at(0) if self.off else None


class LogReader:
    """Tails run.log (or .log.gz) in order; lines beyond a replay time limit are held, not re-read."""

    def __init__(self, path, live):
        self.path, self.live = Path(path), live
        self.f = None
        self.pos = 0
        self.held = None
        self.tmap = TimeMap()
        self.skipped = 0
        self.ino = None
        self.wall_open = time.time()

    def prescan(self):
        """Collect the time anchors of the content that already exists (fast: one substring test per line)."""
        try:
            with _open(self.path) as f:
                off = 0
                for line in f:
                    if b"ISAC_METRICS" in line:
                        m = ANCHOR.search(line)
                        if m:
                            self.tmap.add(off, int(m.group(1)))
                    off += len(line)
        except OSError:
            pass

    def read(self, state, t_limit=None):
        """Feed new complete lines into state. t_limit (run seconds) stops at the first later line (replay)."""
        try:
            st = os.stat(self.path)
        except OSError:
            return False
        gz = str(self.path).endswith(".gz")
        if self.f is None or st.st_ino != self.ino or (not gz and st.st_size < self.pos):
            if self.f:
                self.f.close()
            self.f, self.ino, self.pos, self.held = _open(self.path), st.st_ino, 0, None
            self.prescan()
            if self.tmap.t:
                state.t0_ns = min(self.tmap.t[0], state.t0_ns) if state.t0_ns is not None else self.tmap.t[0]
        if self.live and not gz and st.st_size - self.pos > LIVE_CATCHUP_BYTES and self.pos > 0:
            self.skipped += st.st_size - self.pos       # live: the present matters more than the backlog
            self.f.seek(st.st_size - (256 << 10))
            self.f.readline()
            self.pos, self.held = self.f.tell(), None
        if self.f.tell() != self.pos:
            self.f.seek(self.pos)
        n = 0
        while True:
            if self.held:
                line, off = self.held
                self.held = None
            else:
                off = self.pos
                line = self.f.readline()
                if not line:
                    break
                if not line.endswith(b"\n") and self.live:
                    self.f.seek(off)                     # partial line still being written: retry next poll
                    break
                self.pos = off + len(line)
                if b"ISAC_METRICS" in line and (not self.tmap.off or off > self.tmap.off[-1]):
                    m = ANCHOR.search(line)
                    if m:
                        self.tmap.add(off, int(m.group(1)))
            # before the first anchor the log has no clock: unknown time, not an extrapolation
            tm = self.tmap.at(off) if self.tmap.off and off >= self.tmap.off[0] else None
            if state.t0_ns is None and tm is not None:
                state.t0_ns = self.tmap.t[0]
            if tm is not None and state.t0_ns is not None:
                t = max(0.0, (tm - state.t0_ns) / 1e9)
            else:
                t = None
            if t_limit is not None and t is not None and t > t_limit:
                self.held = (line, off)
                break
            state.feed_line(line.decode("utf-8", "replace").replace("\x1b", "").rstrip("\r\n"), t)
            n += 1
        return n > 0


class JsonlReader:
    """JSONL feeder (offline + live). A record beyond the replay time limit is held for the next call."""

    def __init__(self, path, live, kind):
        self.path, self.live, self.kind = path, live, kind
        self.pos, self.held, self.bad, self.skipped, self.ino = 0, None, 0, 0, None

    def read(self, state, t_limit=None):
        try:
            st = os.stat(self.path)
        except OSError:
            return False
        if self.ino is not None and (st.st_ino != self.ino or st.st_size < self.pos):
            self.pos, self.held = 0, None
        self.ino = st.st_ino
        feed = {"metrics": state.feed_metrics, "obs": state.feed_obs, "sense": state.feed_sense}[self.kind]
        n = 0
        if self.held is not None:
            if not self._fits(state, self.held, t_limit):
                return False
            feed(self.held)
            self.held = None
        with open(self.path, "rb") as f:
            if self.live and st.st_size - self.pos > LIVE_CATCHUP_BYTES and self.pos > 0:
                self.skipped += st.st_size - self.pos
                f.seek(st.st_size - (1 << 20))
                f.readline()
                self.pos = f.tell()
            f.seek(self.pos)
            while True:
                off = f.tell()
                line = f.readline()
                if not line:
                    break
                if not line.endswith(b"\n") and self.live:
                    break                                # partial record still being written
                self.pos = off + len(line)
                if not line.strip():
                    continue
                try:
                    obj = json.loads(line)
                except ValueError:
                    self.bad += 1
                    continue
                if not isinstance(obj, dict):
                    self.bad += 1
                    continue
                if not self._fits(state, obj, t_limit):
                    self.held = obj
                    break
                feed(obj)
                n += 1
        return n > 0

    @staticmethod
    def _fits(state, obj, t_limit):
        if t_limit is None:
            return True
        tm = obj.get("t_mono_ns")
        if not isinstance(tm, (int, float)) or state.t0_ns is None:
            return True
        return (tm - state.t0_ns) / 1e9 <= t_limit


CFR_HEADER = struct.Struct("<4sHHQBBHIQffII")
CFR_SOURCES = ("ssb", "csirs", "pdsch_dmrs", "pdsch_data", "pusch_dmrs", "pusch_data", "prach", "srs")
CFR_POINTS = 256


class CfrReader:
    """NRCF v1 stream (Sensing API v2, §21): headers for every frame, strided IQ for the frames shown."""

    def __init__(self, path, live):
        self.path, self.live = path, live
        self.pos = 0
        self.bad = 0
        self.ino = None

    def read(self, state, t_limit=None):
        if t_limit is not None:
            return False   # shortcut: CFR frames carry abs_slot, not run time, so replay skips them; map via records when needed
        try:
            st = os.stat(self.path)
        except OSError:
            return False
        if self.ino is not None and (st.st_ino != self.ino or st.st_size < self.pos):
            self.pos = 0
        self.ino = st.st_ino
        n = 0
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            while True:
                hb = f.read(CFR_HEADER.size)
                if len(hb) < CFR_HEADER.size:
                    break
                (magic, ver, hlen, fid, src, n_ant, res, n_re, abs_slot, frac, nvar, epoch, idg) = CFR_HEADER.unpack(hb)
                if magic != b"NRCF" or ver != 1 or hlen < CFR_HEADER.size or n_ant < 1 or n_ant > 8 or n_re < 1 or n_re > 46200:
                    self.bad += 1
                    self.pos = st.st_size      # unrecoverable framing: stop reading this file
                    return n > 0
                body = (hlen - CFR_HEADER.size) + 3 * n_re + (-(hlen + 3 * n_re) % 4) + 8 * n_ant * n_re
                if self.pos + CFR_HEADER.size + body > st.st_size:
                    break                       # frame not complete yet
                remaining = st.st_size - self.pos
                sample = remaining < (32 << 20) or (fid % 50 == 0)   # catch-up: decode one frame in 50
                if sample:
                    f.read(hlen - CFR_HEADER.size)
                    kb = f.read(2 * n_re)
                    f.read(n_re + (-(hlen + 3 * n_re) % 4))
                    iq = f.read(8 * n_ant * n_re)
                    stride = max(1, -(-n_re // CFR_POINTS))
                    k = list(memoryview(kb).cast("H")[::stride])
                    fl = memoryview(iq).cast("f")
                    mag, ph = [], []
                    for a in range(n_ant):
                        seg = fl[2 * a * n_re: 2 * (a + 1) * n_re]
                        re_, im_ = seg[0::2 * stride], seg[1::2 * stride]
                        mag.append([round(10 * math.log10(x * x + y * y + 1e-30), 2) for x, y in zip(re_, im_)])
                        ph.append([round(math.atan2(y, x), 3) for x, y in zip(re_, im_)])
                    state.feed_cfr({"frame_id": fid, "source": CFR_SOURCES[src] if src < len(CFR_SOURCES) else str(src),
                                    "n_ant": n_ant, "n_re": n_re, "abs_slot": abs_slot, "noise_var": nvar,
                                    "k": k, "mag": mag, "ph": ph})
                else:
                    f.seek(body, 1)
                    state.cfr["frames"] += 1
                    s = CFR_SOURCES[src] if src < len(CFR_SOURCES) else str(src)
                    state.cfr["by_source"][s] = state.cfr["by_source"].get(s, 0) + 1
                    state.cfr["last_id"] = fid
                self.pos += CFR_HEADER.size + body
                n += 1
        return n > 0


def _first(d, patterns):
    for p in patterns:
        hits = sorted(glob.glob(os.path.join(d, p)), key=lambda q: -os.path.getsize(q))
        if hits:
            return hits[0]
    return None


def discover(run_dir):
    """Map a run directory to the files the receiver/launchers write there (names used by campaign.py, ota.sh, replays)."""
    d = str(run_dir)
    return {
        "log": _first(d, ("run.log", "rx.log", "run.log.gz", "*.log", "*.log.gz")),
        "metrics": _first(d, ("metrics.jsonl", "*metrics*.jsonl")),
        "obs": _first(d, ("obs.jsonl", "*obs*.jsonl")),
        "records": _first(d, ("records.jsonl", "*records*.jsonl", "sense*.jsonl")),
        "cfr": _first(d, ("cfr.bin", "*.nrcf", "*cfr*.bin", "*cfr*")),
        "cmd": _first(d, ("cmd.txt", "cmdline.txt")),
        "env": _first(d, ("env.txt", "*.env")),
        "label": _first(d, ("label.txt", "evidence.txt", "EVIDENCE")),
        "run_json": _first(d, ("run.json",)),
        "scan": _first(d, ("cells.json",)),
    }


def parse_env_text(text):
    """env.txt (KEY=VAL per line) or a single-line OTA env file (KEY=VAL KEY=VAL ...)."""
    env = {}
    for tok in re.split(r"\s+", text):
        if "=" in tok and re.match(r"^[A-Z_][A-Z0-9_]*=", tok):
            k, v = tok.split("=", 1)
            env[k] = v
    return env


def derive_label(run):
    """Evidence label of a run: label.txt if present, else a derivation that says it is derived."""
    if run.files.get("label"):
        try:
            return open(run.files["label"], errors="replace").readline().strip() or None
        except OSError:
            pass
    if run.label:
        return run.label
    cmd = " ".join(run.state.cmdline or [])
    env = run.state.env or {}
    kind = "rfsim" if ("rfsim" in cmd or "rfsimulator" in cmd) else "replay" if (
        "raw_iq" in cmd.lower() or any(k.startswith("ISAC_RAW_IQ") for k in env)) else "OTA (X410)" if "usrp-args" in cmd else "unknown bed"
    when = time.strftime("%Y-%m-%d", time.localtime(run.mtime())) if run.mtime() else "?"
    return "[UNLABELLED, derived: %s, %s, host unknown] add label.txt to the run directory" % (kind, when)


# --------------------------------------------------------------------------- host (live only)

class Host:
    """Receiver process and host health, read from /proc and /sys (cached 2 s, cheap)."""

    def __init__(self):
        self._t = 0.0
        self._v = {}
        self._prev = {}
        self._self = (time.time(), sum(os.times()[:2]))

    def get(self):
        now = time.time()
        if now - self._t < 2.0:
            return self._v
        v = {"rx_pids": [], "rx_cpu_pct": None, "threads": [], "temp_c": None, "mem_avail_gb": None, "monitor_cpu_pct": None}
        try:
            for p in os.listdir("/proc"):
                if p.isdigit():
                    try:
                        if open("/proc/%s/comm" % p).read().strip() == "nr-uesoftmodem":
                            v["rx_pids"].append(int(p))
                    except OSError:
                        pass
        except OSError:
            pass
        hz = os.sysconf("SC_CLK_TCK")
        if v["rx_pids"]:
            pid = v["rx_pids"][0]
            tot = self._ticks("/proc/%d/stat" % pid)
            if tot is not None:
                pt = self._prev.get(pid)
                if pt:
                    v["rx_cpu_pct"] = 100.0 * (tot - pt[1]) / hz / max(1e-3, now - pt[0])
                self._prev[pid] = (now, tot)
            thr = []
            try:
                for tid in os.listdir("/proc/%d/task" % pid):
                    tk = self._ticks("/proc/%d/task/%s/stat" % (pid, tid))
                    key = (pid, tid)
                    pt = self._prev.get(key)
                    if tk is not None:
                        if pt:
                            try:
                                name = open("/proc/%d/task/%s/comm" % (pid, tid)).read().strip()
                            except OSError:
                                name = tid
                            thr.append({"tid": int(tid), "name": name, "cpu_pct": 100.0 * (tk - pt[1]) / hz / max(1e-3, now - pt[0])})
                        self._prev[key] = (now, tk)
            except OSError:
                pass
            v["threads"] = sorted(thr, key=lambda x: -x["cpu_pct"])[:24]
        temps = []
        for z in glob.glob("/sys/class/thermal/thermal_zone*/temp"):
            try:
                temps.append(int(open(z).read()) / 1000.0)
            except (OSError, ValueError):
                pass
        v["temp_c"] = max(temps) if temps else None
        try:
            for line in open("/proc/meminfo"):
                if line.startswith("MemAvailable:"):
                    v["mem_avail_gb"] = int(line.split()[1]) / 1048576.0
        except OSError:
            pass
        s1 = (now, sum(os.times()[:2]))
        v["monitor_cpu_pct"] = 100.0 * (s1[1] - self._self[1]) / max(1e-3, s1[0] - self._self[0])
        self._self = s1
        self._t, self._v = now, v
        return v

    @staticmethod
    def _ticks(path):
        try:
            f = open(path).read().rsplit(")", 1)[1].split()
            return int(f[11]) + int(f[12])
        except (OSError, IndexError, ValueError):
            return None

    @staticmethod
    def proc_config(pid):
        """Command line and ISAC_* environment of the running receiver (environ may be unreadable: root-owned)."""
        out = {"cmdline": None, "env": None}
        try:
            out["cmdline"] = [a.decode("utf-8", "replace") for a in open("/proc/%d/cmdline" % pid, "rb").read().split(b"\0") if a]
        except OSError:
            pass
        try:
            env = {}
            for kv in open("/proc/%d/environ" % pid, "rb").read().split(b"\0"):
                k, _, val = kv.decode("utf-8", "replace").partition("=")
                if k.startswith(("ISAC_", "NR_", "LDPC")):
                    env[k] = val
            out["env"] = env
        except OSError:
            pass
        return out


# --------------------------------------------------------------------------- run = files + state

class Run:
    def __init__(self, rid, files, live, label=None, name=None):
        self.id, self.files, self.live, self.label = rid, files, live, label
        self.name = name or (os.path.basename(os.path.dirname(files["log"])) if files.get("log") else rid)
        self.lock = threading.Lock()
        self.state = rxparse.RunState()
        self.readers = []
        self.loaded = False
        self.t_cursor = None
        if files.get("log"):
            self.readers.append(LogReader(files["log"], live))
        for kind, key in (("metrics", "metrics"), ("obs", "obs"), ("sense", "records")):
            if files.get(key):
                self.readers.append(JsonlReader(files[key], live, kind))
        if files.get("cfr"):
            self.readers.append(CfrReader(files["cfr"], live))
        self.state.t0_ns = self._first_t()
        self._static_config()

    def _first_t(self):
        """Earliest t_mono_ns at the head of the metrics/obs/records files (the log's anchors are added by its reader)."""
        ts = []
        for k in ("metrics", "obs", "records"):
            try:
                with open(self.files[k], "rb") as f:
                    for _ in range(5):
                        m = re.search(rb'"t_mono_ns":\s*(\d+)', f.readline())
                        if m:
                            ts.append(int(m.group(1)))
                            break
            except (OSError, KeyError, TypeError):
                pass
        return min(ts) if ts else None

    def _static_config(self):
        st = self.state
        if self.files.get("cmd"):
            try:
                txt = open(self.files["cmd"], errors="replace").read().strip()
                st.set_cmdline(rxparse.CMD_TOKEN.findall(txt) or txt.split())
            except OSError:
                pass
        if self.files.get("env"):
            try:
                st.env = parse_env_text(open(self.files["env"], errors="replace").read())
            except OSError:
                pass

    def mtime(self):
        ts = [os.path.getmtime(p) for k, p in self.files.items() if p and k in ("log", "metrics", "obs") and os.path.exists(p)]
        return max(ts) if ts else None

    def advance(self, t_limit=None):
        with self.lock:
            # log first: it sets the run clock origin from its ISAC_METRICS anchors
            for r in self.readers:
                try:
                    r.read(self.state, t_limit)
                except Exception as e:     # a corrupt file must never kill the page
                    self.state._event(None, "reader", "reader error on %s: %s" % (getattr(r, "path", "?"), e), "warn")
            self.loaded = True
            self.t_cursor = t_limit

    def skipped(self):
        return sum(getattr(r, "skipped", 0) for r in self.readers)

    def bad(self):
        return sum(getattr(r, "bad", 0) for r in self.readers)

    def duration(self):
        s = self.state
        return s.t_last


class Manager:
    def __init__(self, args):
        self.args = args
        self.host = Host()
        self.primary = None
        self.history = {}
        self.replays = {}
        self.lock = threading.Lock()
        self.flags = {}
        try:
            self.flags = json.load(open(HERE / "isac_flags.json"))
        except (OSError, ValueError):
            pass
        self._open_primary()

    # -- primary (live) run
    def _primary_files(self):
        a = self.args
        files = {}
        if a.follow:
            dirs = [d for d in glob.glob(os.path.expanduser(a.follow)) if os.path.isdir(d)]
            if dirs:
                files = discover(max(dirs, key=lambda d: os.path.getmtime(d)))
        elif a.run:
            files = discover(a.run)
        for k in ("log", "metrics", "obs", "records", "cfr"):
            if getattr(a, k, None):
                files[k] = getattr(a, k)
        return files

    def _open_primary(self):
        files = self._primary_files()
        if not any(files.get(k) for k in ("log", "metrics", "obs")):
            self.primary = None
            return
        live = not self.args.offline
        name = os.path.basename(os.path.normpath(self.args.run)) if self.args.run else None
        self.primary = Run("primary", files, live, self.args.label, name)

    def poll_loop(self):
        while True:
            try:
                if self.args.follow:
                    files = self._primary_files()
                    if self.primary is None or files.get("log") != self.primary.files.get("log"):
                        self._open_primary()
                elif self.primary is None:
                    self._open_primary()
                if self.primary:
                    self.primary.advance()
                    host = self.host.get()
                    if host["rx_pids"] and self.primary.state.cmdline is None:
                        cfg = Host.proc_config(host["rx_pids"][0])
                        if cfg["cmdline"]:
                            self.primary.state.set_cmdline(cfg["cmdline"])
                        if cfg["env"] is not None and self.primary.state.env is None:
                            self.primary.state.env = cfg["env"]
            except Exception as e:
                print("[monitor] poll: %s" % e)
            time.sleep(self.args.poll)

    # -- history
    def run_dirs(self):
        out = []
        for pat in self.args.runs_root or []:
            for d in glob.glob(os.path.expanduser(pat)):
                if os.path.isdir(d) and discover(d).get("log") or (os.path.isdir(d) and discover(d).get("metrics")):
                    out.append(os.path.abspath(d))
        return sorted(set(out), key=lambda d: -os.path.getmtime(d))

    @staticmethod
    def rid(path):
        return "h" + hashlib.sha1(path.encode()).hexdigest()[:12]

    def resolve(self, rid):
        """Run id -> Run. Only the primary run and directories under --runs-root are reachable (no arbitrary paths)."""
        if rid in (None, "", "primary"):
            return self.primary
        with self.lock:
            if rid in self.history:
                return self.history[rid]
        for d in self.run_dirs():
            if self.rid(d) == rid:
                run = Run(rid, discover(d), live=False, name=os.path.basename(d))
                run.dir = d
                run.advance()
                with self.lock:
                    self.history[rid] = run
                    while len(self.history) > HISTORY_CACHE:
                        self.history.pop(next(iter(self.history)))
                return run
        return None

    def replay(self, rid, t):
        base = self.resolve(rid)
        if base is None:
            return None
        key = rid or "primary"
        with self.lock:
            r = self.replays.get(key)
            if r is None or (r.t_cursor is not None and t < r.t_cursor):
                r = Run(key + "-replay", base.files, live=False, label=base.label, name=base.name)
                self.replays = {key: r}       # one replay at a time bounds memory
        r.advance(t)
        r.full_duration = base.duration()
        return r


# --------------------------------------------------------------------------- views

def flag_area(name):
    n = name[5:] if name.startswith("ISAC_") else name
    for pre, area in (("USRP", "Radio / X410"), ("RX_", "Receive combining"), ("SCAN", "Cell search / sync"),
                      ("PSS", "Cell search / sync"), ("SSS", "Cell search / sync"), ("KSSB", "Cell search / sync"),
                      ("ACQ", "Acquisition"), ("CARRIER", "Acquisition"), ("POINTA", "Acquisition"), ("RECONF", "Acquisition"),
                      ("PDCCH", "PDCCH / DCI"), ("DCI", "PDCCH / DCI"), ("COREMAP", "PDCCH / DCI"), ("SI_", "PDCCH / DCI"),
                      ("PDSCH", "PDSCH decode"), ("CHEST", "PDSCH decode"), ("L2MAXH", "PDSCH decode"), ("MMSE", "PDSCH decode"),
                      ("LDPC", "PDSCH decode"), ("PDTIM", "Timing / profiling"), ("TD_", "Technique D solver"),
                      ("SWEEP", "Technique D solver"), ("UL_", "Uplink"), ("PUSCH", "Uplink"), ("PRACH", "Uplink"), ("SRS", "Uplink"),
                      ("CSIRS", "CSI-RS"), ("GPU", "GPU"), ("METRICS", "Outputs"), ("OBS", "Outputs"), ("SENSE", "Outputs"),
                      ("UECTX", "Outputs"), ("RAW_IQ", "Replay only"), ("DL_TRAFFIC", "PDSCH decode")):
        if n.startswith(pre):
            return area
    return "Other"


def config_view(run, flags):
    st = run.state
    env = st.env
    groups = {}
    if env:
        for k, v in sorted(env.items()):
            info = flags.get(k)
            groups.setdefault(flag_area(k), []).append({"name": k, "value": v, "desc": info["desc"] if info else None,
                                                        "known": bool(info), "src": info.get("src") if info else None})
    cfg_text = None
    if st.cfg_path:
        p = st.cfg_path if os.path.isabs(st.cfg_path) else os.path.join(os.path.dirname(run.files.get("log") or "."), st.cfg_path)
        try:
            if os.path.getsize(p) < 256 << 10:
                cfg_text = open(p, errors="replace").read()
        except OSError:
            cfg_text = None
    argv = st.cmdline or []
    pick = {}
    for opt in ("-C", "--ssb", "-r", "--ue-rxgain", "--initial-fo", "--numerology", "--band", "--ue-nb-ant-rx", "-O", "--rfsim",
                "--passive-rx", "--thread-pool", "--usrp-args"):
        if opt in argv:
            i = argv.index(opt)
            nxt = argv[i + 1] if i + 1 < len(argv) else None
            pick[opt] = nxt if nxt is not None and (not nxt.startswith("-") or re.match(r"^-\d", nxt)) else "(set)"
    return {"cmdline": argv, "key_args": pick, "cfg_path": st.cfg_path, "cfg_text": cfg_text,
            "env_groups": groups, "env_source": run.files.get("env") or ("/proc/<pid>/environ" if env is not None and run.live else None)}


def scan_cells(paths):
    out = []
    for p in paths:
        for f in glob.glob(os.path.expanduser(p)):
            try:
                cells = json.load(open(f))
            except (OSError, ValueError):
                continue
            for c in cells if isinstance(cells, list) else []:
                if isinstance(c, dict):
                    out.append({k: c.get(k) for k in ("pci", "gscn", "ssb_mhz", "metric_db", "sss_ratio", "dmrs_ratio", "i_ssb",
                                                      "scs_khz", "cfo_hz", "n_cells", "capture")} | {"file": f})
    return out


def state_payload(mgr, run, q):
    thr = {}
    try:
        thr = {k: float(v) for k, v in json.loads(q.get("thr", "{}")).items() if k in rxparse.DEFAULT_THRESHOLDS}
    except (ValueError, AttributeError):
        thr = {}
    with run.lock:
        snap = run.state.snapshot()
    host = {}
    if run.live:
        host = dict(mgr.host.get(), live=True)
        mt = run.mtime()
        host["silent_s"] = (time.time() - mt) if mt else None
        host["proc_alive"] = bool(host["rx_pids"])
    keys = sorted(snap["cells"].keys(), key=lambda k: (k == "?", k))
    cell = q.get("cell") or (str(snap["cur_pci"]) if snap["cur_pci"] is not None else (keys[0] if keys else None))
    for k in keys:
        snap["cells"][k]["label"] = "PCI %s @ %s MHz" % (k, ("%.2f" % snap["tuned_mhz"]) if snap["tuned_mhz"] else "?")
    return {
        "meta": {"run": run.id, "name": run.name, "live": run.live, "files": {k: v for k, v in run.files.items() if v},
                 "label": derive_label(run), "skipped_bytes": run.skipped(), "bad_records": run.bad(),
                 "replay_t": run.t_cursor, "duration": getattr(run, "full_duration", None) or run.duration(),
                 "now": time.time(), "cell": cell, "cells": keys},
        "snap": snap, "tiles": rxparse.tiles(snap, thr, host), "alerts": rxparse.alerts(snap, thr, host),
        "cell_rows": rxparse.cell_rows(snap, cell) if cell else [], "ul_reason": rxparse.ul_reason(snap),
        "latency": rxparse.latency_view(snap), "dl_totals": rxparse.dl_totals(snap, cell), "kpis": rxparse.kpis(snap, cell),
        "host": host, "config": config_view(run, mgr.flags),
    }


def log_lines(path, q, key_only, n=400, tail_bytes=16 << 20):
    """Filtered raw log: the last n matching lines of the last tail_bytes of the file."""
    if not path or not os.path.exists(path):
        return {"lines": [], "note": "no log file"}
    size = os.path.getsize(path)
    try:
        rx = re.compile(q, re.I) if q else None
    except re.error:
        rx = re.compile(re.escape(q), re.I)
    out = []
    with _open(path) as f:
        start = 0
        if not str(path).endswith(".gz") and size > tail_bytes:
            start = size - tail_bytes
            f.seek(start)
            f.readline()
        for raw in f:
            line = raw.decode("utf-8", "replace").replace("\x1b", "").rstrip()
            if key_only and not rxparse.KEY_LINE.search(line):
                continue
            if rx and not rx.search(line):
                continue
            out.append(line[:600])
            if len(out) > 4 * n:
                out = out[-n:]
    return {"lines": out[-n:], "note": "searched the last %.0f MB of %.0f MB" % (min(size, tail_bytes) / 1e6, size / 1e6)
            if size > tail_bytes else "searched the whole file (%.1f MB)" % (size / 1e6)}


def make_handler(mgr):
    html_path = HERE / "monitor.html"

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype, extra=None):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def _json(self, obj, code=200):
            self._send(code, json.dumps(obj, default=str, allow_nan=False), "application/json")

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = {k: v[-1] for k, v in urllib.parse.parse_qs(u.query).items()}
            try:
                self._route(u.path, q)
            except BrokenPipeError:
                pass
            except ValueError as e:      # NaN/inf guard of json.dumps
                self._json({"error": str(e)}, 500)

        def _route(self, path, q):
            if path in ("/", "/index.html"):
                return self._send(200, html_path.read_bytes(), "text/html; charset=utf-8")
            if path == "/health":
                m, o = JsonlTail(mgr.primary.files.get("metrics") if mgr.primary else None), \
                    JsonlTail(mgr.primary.files.get("obs") if mgr.primary else None, keep=5000)
                m.poll(); o.poll()
                return self._json(health_snapshot(m, o))
            if path == "/api/runs":
                runs = []
                if mgr.primary:
                    runs.append({"id": "primary", "name": mgr.primary.name, "live": mgr.primary.live, "label": derive_label(mgr.primary),
                                 "path": os.path.dirname(mgr.primary.files.get("log") or mgr.primary.files.get("metrics") or ""),
                                 "mtime": mgr.primary.mtime()})
                pdir = os.path.realpath(runs[0]["path"]) if runs else None
                for d in mgr.run_dirs():
                    if os.path.realpath(d) == pdir:
                        continue          # the primary run is already listed
                    lab = discover(d).get("label")
                    runs.append({"id": mgr.rid(d), "name": os.path.basename(d), "live": False, "path": d,
                                 "mtime": os.path.getmtime(d), "label": open(lab, errors="replace").readline().strip() if lab else None})
                return self._json({"runs": runs, "key_re": rxparse.KEY_LINE.pattern, "thresholds": rxparse.DEFAULT_THRESHOLDS,
                                   "mcs_buckets": [b[0] for b in rxparse.MCS_BUCKETS], "stages": rxparse.SYNC_STAGES})
            if path == "/api/state":
                rid = q.get("run", "primary")
                run = mgr.replay(rid, float(q["t"])) if q.get("t") not in (None, "") else mgr.resolve(rid)
                if run is None:
                    return self._json({"error": "no run: start the monitor with --run DIR, --log/--metrics/--obs or --follow"}, 404)
                return self._json(state_payload(mgr, run, q))
            if path == "/api/compare":
                out = []
                for rid in [x for x in q.get("runs", "").split(",") if x][:6]:
                    run = mgr.resolve(rid)
                    if run is None:
                        continue
                    with run.lock:
                        snap = run.state.snapshot()
                    out.append({"id": rid, "name": run.name, "label": derive_label(run), "kpis": rxparse.kpis(snap),
                                "duration": run.duration()})
                return self._json({"runs": out})
            if path == "/api/summary.md":
                run = mgr.resolve(q.get("run", "primary"))
                if run is None:
                    return self._send(404, "no run", "text/plain")
                with run.lock:
                    snap = run.state.snapshot()
                thr = {}
                try:
                    thr = {k: float(v) for k, v in json.loads(q.get("thr", "{}")).items() if k in rxparse.DEFAULT_THRESHOLDS}
                except ValueError:
                    pass
                md = rxparse.summary_md(snap, derive_label(run), run.name, thr, dict(mgr.host.get(), live=True) if run.live else {})
                return self._send(200, md, "text/markdown; charset=utf-8",
                                  {"Content-Disposition": 'attachment; filename="summary-%s.md"' % re.sub(r"[^\w.-]", "_", run.name)})
            if path == "/api/log":
                run = mgr.resolve(q.get("run", "primary"))
                return self._json(log_lines(run.files.get("log") if run else None, q.get("q", ""), q.get("key") == "1",
                                            min(int(q.get("n", 400)), 3000)))
            if path == "/api/log/download":
                run = mgr.resolve(q.get("run", "primary"))
                p = run.files.get("log") if run else None
                if not p or not os.path.exists(p):
                    return self._send(404, "no log", "text/plain")
                size = os.path.getsize(p)
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.send_header("Content-Disposition", 'attachment; filename="%s"' % os.path.basename(p))
                self.end_headers()
                with open(p, "rb") as f:
                    while True:
                        b = f.read(1 << 20)
                        if not b:
                            break
                        self.wfile.write(b)
                return
            if path == "/api/scan":
                paths = list(mgr.args.scan or [])
                if mgr.primary and mgr.primary.files.get("scan"):
                    paths.append(mgr.primary.files["scan"])
                return self._json({"cells": scan_cells(paths), "sources": paths})
            if path == "/api/flags":
                return self._json(mgr.flags)
            return self._send(404, "not found", "text/plain")

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", metavar="DIR", help="run directory (run.log/rx.log, metrics.jsonl, obs.jsonl, sense files, cmd.txt, env.txt)")
    ap.add_argument("--follow", metavar="GLOB", help="follow the newest run directory matching GLOB (live campaigns)")
    ap.add_argument("--log", help="receiver log file (overrides the run directory's)")
    ap.add_argument("--metrics", metavar="FILE.jsonl", help="ISAC_METRICS_PATH file")
    ap.add_argument("--obs", metavar="FILE.jsonl", help="ISAC_OBS_PATH file (per-grant observations, schema 1)")
    ap.add_argument("--records", metavar="FILE.jsonl", help="sensing export records_path (schema 2)")
    ap.add_argument("--cfr", metavar="FILE", help="sensing export cfr_path (NRCF v1)")
    ap.add_argument("--runs-root", action="append", metavar="GLOB",
                    help="run directories for history / A-B compare / replay (repeatable), e.g. '~/NICOLA/ota_runs/*'")
    ap.add_argument("--scan", action="append", metavar="cells.json", help="blind scanner output (fastscan/live_chain), repeatable")
    ap.add_argument("--label", help="evidence label of the primary run (else label.txt in the run directory, else derived)")
    ap.add_argument("--offline", action="store_true", help="treat the primary run as finished (no live catch-up skipping)")
    ap.add_argument("--poll", type=float, default=1.0, help="file poll period in seconds (default 1)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1", help="HTTP bind address (default 127.0.0.1; use an SSH tunnel)")
    args = ap.parse_args()
    if args.runs_root is None:
        args.runs_root = []
    mgr = Manager(args)
    threading.Thread(target=mgr.poll_loop, daemon=True).start()
    srv = ThreadingHTTPServer((args.bind, args.port), make_handler(mgr))
    print("[monitor] http://%s:%d/  (from a laptop: ssh -L %d:localhost:%d <host>)" % (args.bind, args.port, args.port, args.port))
    srv.serve_forever()


if __name__ == "__main__":
    main()
