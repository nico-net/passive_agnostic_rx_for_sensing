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
