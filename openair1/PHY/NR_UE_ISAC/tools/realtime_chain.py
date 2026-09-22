#!/usr/bin/env python3
"""Real-time sensing tail (2026-09-21): one single-threaded process that consumes the engine's
per-CPI DetectionReport stream and runs stages 8 (families), 7 (UE localiser), 9 (multistatic solve,
height model) and 10 (tracker B, look-ahead + fixed-lag smoother) causally, emitting one track record per
CPI. Same code paths as the offline tools (Stage8Stream / LocaliserStream / Stage9Stream / TrackerStream),
so the offline replay and the live chain are identical by construction.

Latency budget (declared): stage 8 fixed lag N_LAG = 10 CPIs (0.75 s) + tracker look-ahead 5 CPIs
(0.375 s); the smoother adds 10 CPIs on the 'smoothed' field only ('tracks' is the filtered state).

Source: --follow <reports.jsonl> tails the engine's report file (works with the live OAI passive
receiver, which appends one JSON line per CPI, and with the replay); --zmq tcp://... reads the engine's
ZeroMQ report publisher if configured. Output: --out tracks.jsonl (+ optional --pub ZMQ).
CPU: one process, one thread (OMP/BLAS threads pinned to 1); use `taskset -c <core>` to pin it.
"""
import os
os.environ.setdefault("OMP_NUM_THREADS", "1"); os.environ.setdefault("OPENBLAS_NUM_THREADS", "1"); os.environ.setdefault("MKL_NUM_THREADS", "1")
import argparse, dataclasses, json, sys, time
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
from extended_object_stage2 import Stage8Stream
from ue_localiser_dfs import LocaliserStream
from extended_object_stage3_dlul import Stage9Stream, attach_ue_track
from stage10_state_tracker import TrackerStream

RANGE_RES_M = 3.0503913105413107
RATE_RES_MPS = 1.1422492665493145


def follow(path, poll_s=0.005, stop_when_idle_s=None):
    """Yield JSON records appended to `path` (blocking tail). Partial lines are waited for."""
    # the engine creates the report file itself (and refuses to overwrite one): wait for it to appear
    while not os.path.exists(path):
        time.sleep(poll_s)
    f = open(path, "r"); buf = ""; idle = 0.0
    while True:
        chunk = f.read()
        if chunk:
            idle = 0.0; buf += chunk
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if line.strip():
                    yield json.loads(line)
        else:
            time.sleep(poll_s); idle += poll_s
            if stop_when_idle_s is not None and idle >= stop_when_idle_s:
                return


class RealtimeChain:
    def __init__(self, tx, rx_positions, ul_sessions, ul_advance=None, max_speed_mps=50.0, max_range_m=624.5676208333334, window_cpis=60):
        self.tx = np.asarray(tx, float); self.rx = [np.asarray(r, float) for r in rx_positions]
        self.s8_dl = Stage8Stream(max_speed_mps, leg="dl")
        self.s8_ul = {k: Stage8Stream(max_speed_mps, leg="ul", ul_session=k) for k in ul_sessions}
        self.loc = {k: LocaliserStream(self.rx, RANGE_RES_M, RATE_RES_MPS, window_cpis, ul_session=k, tx_position=self.tx,
                                       max_range_m=max_range_m, ul_advance=(ul_advance or {}).get(k)) for k in ul_sessions}
        self.ue_tracks = {k: [] for k in ul_sessions}
        self.s9 = Stage9Stream(self.tx, None, self.rx, RATE_RES_MPS)
        self.s10 = TrackerStream()
        self.pending = {}          # time -> {leg: {receiver: [blocks]}} awaiting stage 9
        self.times = []            # report times fed so far (stage 8 emits report n - N_LAG at feed n)
        self.stats = {"cpis": 0, "s9": 0, "tracks": 0, "ms": []}

    def _queue(self, objs, leg, track=None):
        for o in objs:
            o = json.loads(json.dumps(dataclasses.asdict(o))); o["leg"] = leg; o["range_sigma_raw"] = o["range_sigma_m"]   # same serialisation as the offline JSONL
            if track is not None and not attach_ue_track(o, track):
                continue
            self.pending.setdefault(round(o["time_s"], 4), {}).setdefault(leg, {}).setdefault(o["receiver"], []).append(o)

    def feed(self, rec):
        """One DetectionReport (one CPI). Returns the tracker output records produced (0 or 1)."""
        t0 = time.perf_counter(); out = []
        self.stats["cpis"] += 1
        # stage 7: UE tracks (causal, per session) -- must precede UL block attachment
        for k, loc in self.loc.items():
            est = loc.feed(rec)
            if est is not None: self.ue_tracks[k].append(est)
        # stage 8: DL and UL families; each emits measurements for the CPI leaving its lag window
        self._queue(self.s8_dl.feed(rec), "dl")
        for k, s8 in self.s8_ul.items():
            self._queue(s8.feed(rec), f"ul{k}", self.ue_tracks[k])
        # stage 9 + 10 on every completed (lagged) CPI time; all legs share the same lag, so a time
        # is complete once a later report has been fed (stage 8 emits t - N_LAG for every receiver).
        self.times.append(rec["midpoint_air_time_s"])
        from extended_object_stage2 import N_LAG
        complete_t = self.times[-1 - N_LAG] if len(self.times) > N_LAG else None
        for t in sorted(self.pending):
            if complete_t is None or t > complete_t + 1e-6:
                break
            legs = self.pending.pop(t)
            legs.setdefault("dl", {})
            for k in self.s8_ul: legs.setdefault(f"ul{k}", {})
            s9 = self.s9.step(t, legs)
            if s9 is None: continue
            self.stats["s9"] += 1
            tr = self.s10.feed(s9)
            if tr is not None: out.append(tr); self.stats["tracks"] += 1
        self.stats["ms"].append(1000.0 * (time.perf_counter() - t0))
        return out

    def flush(self):
        out = []
        for t in sorted(self.pending):
            legs = self.pending.pop(t); legs.setdefault("dl", {})
            for k in self.s8_ul: legs.setdefault(f"ul{k}", {})
            s9 = self.s9.step(t, legs)
            if s9 is not None:
                tr = self.s10.feed(s9)
                if tr is not None: out.append(tr)
        out.extend(self.s10.flush())
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--follow", help="engine report JSONL to tail (live or replay)")
    ap.add_argument("--geometry", required=True, help="infrastructure_geometry.json (gNB + receiver positions; no target data)")
    ap.add_argument("--ul-sessions", type=int, nargs="*", default=[1, 2])
    ap.add_argument("--ul-advance", nargs="*", default=[], help="k=path.npy per UL session (receiver window-advance sidecar)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--idle-stop-s", type=float, default=None, help="stop after this many seconds without new reports (replay use)")
    ap.add_argument("--pub", default=None, help="optional ZeroMQ PUB endpoint for track records")
    a = ap.parse_args()
    g = json.load(open(a.geometry))
    rx = [g["receiver_positions_m"][f"rx{i}"] for i in range(4)]
    adv = {}
    for item in a.ul_advance:
        k, path = item.split("=", 1); adv[int(k)] = np.load(path)
    chain = RealtimeChain(g["transmitter_position_m"], rx, a.ul_sessions, ul_advance=adv)
    pub = None
    if a.pub:
        import zmq
        pub = zmq.Context().socket(zmq.PUB); pub.bind(a.pub)
    started = time.time()
    with open(a.out, "w") as out:
        def emit(recs):
            for r in recs:
                line = json.dumps(r); out.write(line + "\n"); out.flush()
                if pub is not None: pub.send_string(line)
        for rec in follow(a.follow, stop_when_idle_s=a.idle_stop_s):
            emit(chain.feed(rec))
        emit(chain.flush())
    ms = np.array(chain.stats["ms"]) if chain.stats["ms"] else np.zeros(1)
    print(json.dumps({"cpis": chain.stats["cpis"], "stage9_records": chain.stats["s9"], "track_records": chain.stats["tracks"],
                      "per_cpi_ms": {"median": float(np.median(ms)), "p95": float(np.percentile(ms, 95)), "max": float(ms.max())},
                      "wall_s": time.time() - started}))


if __name__ == "__main__":
    main()
