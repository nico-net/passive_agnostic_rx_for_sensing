#!/usr/bin/env python3
"""Live monitor for the passive ISAC receiver.

Subscribes to the receiver's DetectionReport ZeroMQ PUB socket ([sensing] report_endpoint) and
serves a single-page dashboard. The browser polls /state; CPIs arrive every ~1-30 s, so polling is
well inside the data rate and needs no streaming machinery.

  ./monitor.py --connect tcp://127.0.0.1:5556 --port 8080

--connect may be repeated, but this is a SINGLE-RECEIVER tool: track positions come from the
receiver's own AoA fix (range ellipse + bearing ray), not from cross-receiver fusion. Extra
endpoints are shown side by side, never fused.
"""

import argparse
import json
import re
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import zmq

# nr_isac_source_t order (nr_isac.h). PUSCH_DMRS is the only uplink source, and its geometry is
# UE->target->rx, not gNB->target->rx -- which is why the UI reports it separately and never maps it.
SOURCE_NAMES = ["csi_rs", "pdsch_dmrs", "pdsch_data", "blind", "pusch_dmrs"]
UL_SOURCES = {"pusch_dmrs"}

HISTORY = 200  # CPIs kept per receiver for the sparklines / track trails


class ReportStore:
    """Latest state per receiver, plus a bounded history. Written by SUB threads, read by HTTP."""

    def __init__(self):
        self._lock = threading.Lock()
        self._rx = {}

    def add(self, endpoint, rep):
        rx_id = rep.get("rx_id") or endpoint
        with self._lock:
            st = self._rx.setdefault(
                rx_id,
                {"endpoint": endpoint, "history": deque(maxlen=HISTORY), "trails": {}, "cpi_count": 0,
                 "gaps": deque(maxlen=8)},
            )
            st["latest"] = rep
            now = time.time()
            # Observed inter-report gap. The CPI closes on ROW COUNT, so its wall-clock cadence
            # swings with traffic -- ~4 s at cpi_slots=128 but 150-200 s at 1024. A fixed staleness
            # threshold is therefore meaningless; the UI derives one from these gaps instead.
            if st.get("last_seen") is not None:
                st["gaps"].append(now - st["last_seen"])
            st["last_seen"] = now
            st["cpi_count"] += 1
            st["history"].append(
                {
                    "t": rep.get("cpi_start_time_utc_ns", 0),
                    "n_det": len(rep.get("detections") or []),
                    "cfo_hz": (rep.get("sync", {}).get("cfo", {}) or {}).get("cfo_hz"),
                    "sto_frac": (rep.get("sync", {}).get("sto", {}) or {}).get("mean_frac_bin"),
                    "sfo_ppm": (rep.get("sync", {}).get("sfo", {}) or {}).get("sfo_ppm"),
                    "n_flywheel": (rep.get("sync", {}).get("sto", {}) or {}).get("n_flywheel"),
                    "n_valid": (rep.get("sync", {}).get("sto", {}) or {}).get("n_valid"),
                }
            )
            # Track trails: keep each track's fixed positions so the map can draw its path.
            for tr in rep.get("tracks") or []:
                pos = tr.get("position")
                if not pos:
                    continue
                trail = st["trails"].setdefault(str(tr["track_id"]), deque(maxlen=60))
                trail.append({"x": pos[0], "y": pos[1], "v": tr.get("bistatic_velocity_mps", 0.0)})
            # Drop trails whose track is gone, so a long run does not accumulate dead paths.
            live = {str(t["track_id"]) for t in (rep.get("tracks") or [])}
            for tid in [k for k in st["trails"] if k not in live]:
                del st["trails"][tid]

    def snapshot(self):
        with self._lock:
            out = {}
            for rx_id, st in self._rx.items():
                out[rx_id] = {
                    "endpoint": st["endpoint"],
                    "latest": st.get("latest"),
                    "last_seen": st.get("last_seen"),
                    "cpi_count": st["cpi_count"],
                    "cadence_s": (sorted(st["gaps"])[len(st["gaps"]) // 2] if st["gaps"] else None),
                    "history": list(st["history"]),
                    "trails": {k: list(v) for k, v in st["trails"].items()},
                }
            return out


class LogTail:
    """Tails the receiver log for the health facts that never reach the report: PBCH lock, X410
    stream state, blind-PDCCH accepts, PUSCH/PDSCH CRC. The report is a DSP artifact and says
    nothing about whether the radio is actually alive -- which is the failure mode that matters."""

    PATTERNS = [
        ("pdsch_crc", re.compile(r"pdsch_decode\[try=(\d+) crc_ok=(\d+)")),
        # DL TRANSPORT-BLOCK rate. NOT segs_decoded: those counters are summed only over FAILING
        # TBs (nr_pdsch_passive_decode.c:302-303), so that ratio FALLS as decoding improves.
        ("ldpc", re.compile(r"LDPCDIAG ok=(\d+) seg_fail=(\d+) tb_fail=(\d+) zero_tb=(\d+)")),
        ("pusch", re.compile(r"pusch_passive\[try=(\d+) crc_ok=(\d+)")),
        ("dci", re.compile(r"dci01\[accepts=(\d+) rejects=(\d+)")),
        ("blind", re.compile(r"accepts=(\d+)\s+cfr_submits=(\d+)")),
        ("occ", re.compile(r"occ\[([^\]]*)\]")),
        ("overflow", re.compile(r"[Oo]verflow|OOOO|\bO\b")),
        ("sync_lost", re.compile(r"out.of.sync|sync lost|LOST SYNC", re.I)),
        ("pbch_ok", re.compile(r"PBCH.*(?:decoded|CRC OK)|MIB decoded", re.I)),
    ]

    def __init__(self, path, maxlines=4000):
        self.path = Path(path)
        self.lines = deque(maxlen=maxlines)
        self.counters = {"overflow": 0, "sync_lost": 0, "pbch_ok": 0}
        self.stats = {}
        self._lock = threading.Lock()

    def run(self):
        while not self.path.exists():
            time.sleep(1.0)
        with self.path.open("r", errors="replace") as f:
            f.seek(0, 2)
            while True:
                line = f.readline()
                if not line:
                    time.sleep(0.25)
                    continue
                self._ingest(line.rstrip())

    def _ingest(self, line):
        with self._lock:
            for name, pat in self.PATTERNS:
                m = pat.search(line)
                if not m:
                    continue
                if name in self.counters:
                    self.counters[name] += 1
                elif name == "pdsch_crc":
                    try_n, ok_n = int(m.group(1)), int(m.group(2))
                    self.stats["pdsch_try"] = try_n
                    self.stats["pdsch_ok"] = ok_n
                    self.stats["pdsch_rate"] = (100.0 * ok_n / try_n) if try_n else None
                elif name == "ldpc":
                    ok, sf, tf, zt = (int(m.group(i)) for i in (1, 2, 3, 4))
                    tot = ok + sf + tf
                    self.stats["dl_ok"] = ok
                    self.stats["dl_fail"] = sf + tf
                    self.stats["dl_zero_tb"] = zt
                    self.stats["dl_tb_rate"] = (100.0 * ok / tot) if tot else None
                elif name == "pusch":
                    t, o = int(m.group(1)), int(m.group(2))
                    self.stats["ul_try"] = t
                    self.stats["ul_ok"] = o
                    self.stats["ul_rate"] = (100.0 * o / t) if t else None
                elif name == "dci":
                    self.stats["dci_accepts"] = int(m.group(1))
                    self.stats["dci_rejects"] = int(m.group(2))
                elif name == "blind":
                    self.stats["blind_accepts"] = int(m.group(1))
                    self.stats["blind_submits"] = int(m.group(2))
                elif name == "occ":
                    self.stats["occ"] = m.group(1)
            if any(k in line for k in ("SENSING", "ERROR", "WARN", "overflow", "sync")):
                # Wall-clock stamp: the receiver's own lines carry no time, so without this there is
                # no way to tell a line from this second from one ten minutes old.
                self.lines.append(time.strftime("%H:%M:%S ") + line[-400:])

    def snapshot(self):
        with self._lock:
            return {"counters": dict(self.counters), "stats": dict(self.stats), "lines": list(self.lines)[-400:]}


def replay_thread(path, store, rate_hz):
    """Feed a recorded report JSONL through the same store as the live path, for testing the GUI
    with no receiver attached. Same code path as ZMQ from add() onward, so what you see is what a
    live run renders."""
    period = 1.0 / max(0.01, rate_hz)
    while True:
        with open(path, "r", errors="replace") as f:
            n = 0
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    store.add("replay:" + path, json.loads(line))
                    n += 1
                except json.JSONDecodeError:
                    continue
                time.sleep(period)
        print(f"[monitor] replay {path}: {n} reports, looping")


def sub_thread(endpoint, store):
    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.SUB)
    sock.setsockopt_string(zmq.SUBSCRIBE, "")
    sock.connect(endpoint)
    print(f"[monitor] subscribed to {endpoint}")
    while True:
        try:
            msg = sock.recv(copy=True)
        except zmq.ZMQError as e:
            print(f"[monitor] {endpoint}: {e}")
            time.sleep(1.0)
            continue
        for line in msg.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                store.add(endpoint, json.loads(line))
            except json.JSONDecodeError as e:
                print(f"[monitor] bad JSON from {endpoint}: {e}")


def make_handler(store, logtail, html_path):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/state"):
                payload = {
                    "receivers": store.snapshot(),
                    "log": logtail.snapshot() if logtail else None,
                    "source_names": SOURCE_NAMES,
                    "ul_sources": sorted(UL_SOURCES),
                    "now": time.time(),
                }
                self._send(200, json.dumps(payload).encode(), "application/json")
            elif self.path in ("/", "/index.html"):
                self._send(200, html_path.read_bytes(), "text/html; charset=utf-8")
            else:
                self._send(404, b"not found", "text/plain")

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--connect", action="append", default=[], metavar="ENDPOINT",
                    help="receiver report_endpoint, e.g. tcp://127.0.0.1:5556 (repeatable)")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port (default 8080)")
    ap.add_argument("--bind", default="0.0.0.0", help="HTTP bind address (default 0.0.0.0, for SSH access)")
    ap.add_argument("--log", help="receiver log file to tail for radio/decode health")
    ap.add_argument("--seed", metavar="FILE.jsonl",
                    help="backfill the store from a report JSONL at startup, then subscribe live. "
                         "ZeroMQ PUB/SUB does not replay, so without this a monitor restart shows an "
                         "EMPTY page until the next CPI closes -- up to several minutes at a long "
                         "cpi_slots, which reads as a dead receiver.")
    ap.add_argument("--replay", metavar="FILE.jsonl",
                    help="replay a recorded report JSONL instead of subscribing (testing, no receiver needed)")
    ap.add_argument("--replay-rate", type=float, default=2.0,
                    help="reports per second when replaying (default 2)")
    args = ap.parse_args()

    store = ReportStore()
    if args.seed:
        n = 0
        try:
            with open(args.seed) as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        store.add("seed:" + args.seed, json.loads(line))
                        n += 1
                    except (ValueError, TypeError):
                        continue  # a torn last line while the receiver is mid-write
        except OSError as e:
            print(f"[monitor] seed {args.seed}: {e}")
        # Seeded reports all land at once, so their ARRIVAL gaps are ~0 and would peg the derived
        # staleness threshold at its floor. Recover the real cadence from the reports' own
        # cpi_start_time_utc_ns instead -- that is wall-clock truth and survives the restart.
        with store._lock:
            for st in store._rx.values():
                st["gaps"].clear()
                t = sorted(h["t"] for h in st["history"] if h.get("t"))
                for a, b in zip(t, t[1:]):
                    gap = (b - a) / 1e9
                    if 0.0 < gap < 3600.0:
                        st["gaps"].append(gap)
        print(f"[monitor] seeded {n} reports from {args.seed}")
    if args.replay:
        threading.Thread(target=replay_thread, args=(args.replay, store, args.replay_rate),
                         daemon=True).start()
    else:
        for ep in (args.connect or ["tcp://127.0.0.1:5556"]):
            threading.Thread(target=sub_thread, args=(ep, store), daemon=True).start()

    logtail = None
    if args.log:
        logtail = LogTail(args.log)
        threading.Thread(target=logtail.run, daemon=True).start()

    html_path = Path(__file__).with_name("monitor.html")
    if not html_path.exists():
        raise SystemExit(f"missing {html_path}")

    srv = ThreadingHTTPServer((args.bind, args.port), make_handler(store, logtail, html_path))
    print(f"[monitor] http://{args.bind}:{args.port}/  (ssh -L {args.port}:localhost:{args.port} sens6)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
