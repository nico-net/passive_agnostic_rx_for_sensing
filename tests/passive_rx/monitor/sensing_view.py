"""Pure extraction from the frozen chain's outputs into what the page draws. No I/O here."""
import math, re, threading, time

TRAIL = 120
RE_GATE_OPEN = re.compile(r"SENSING_GATE open rnti=(0x[0-9a-fA-F]+)")
RE_GATE_CLOSE = re.compile(r"SENSING_GATE close")
RE_GATE_STATS = re.compile(r"SENSING_GATE stats open=(\d) admitted=(\d+) rejected=(\d+) gate_discarded_rows=(\d+)")
RE_BRFO = re.compile(r"BRANCHFO d_vs_br0=\[([^\]]*)\]")
RE_LLRCONF = re.compile(r"LLRCONF qm=(\d) calibrated=(\d) tau_rel=([-\d.]+) crc_ok_bit_agreement=([\d.]+)")

def _db(v):
    return round(10.0 * math.log10(v), 2) if v and v > 0 else None

def _det_pair(d):
    # DL (spatial_receivers[].detections) uses bistatic_range_m/bistatic_velocity_mps
    # (report_writer.cc:298-299). UL (uplink_sessions[].receivers[].detections) uses
    # delta_path_range_m/delta_path_range_rate_mps (report_writer.cc:478-480). One helper
    # serves both call sites, so accept either pair.
    r = d.get("bistatic_range_m", d.get("delta_path_range_m"))
    v = d.get("bistatic_velocity_mps", d.get("delta_path_range_rate_mps"))
    return [r, v]

def _receiver_view(s, keep):
    out = dict(keep or {})
    out.update({"rx": s.get("receiver_id"), "pos": s.get("receiver_position_enu_m") or (keep or {}).get("pos")})
    if "rvm_blob" in s and s.get("rvm_range_bins"):
        out.update({"nb": s["rvm_range_bins"], "nr": s["rvm_rate_bins"], "range_res_m": s.get("rvm_range_res_m"),
                    "rate_res_mps": s.get("rvm_rate_res_mps"), "map_db": [_db(v) for v in s["rvm_blob"]]})
    out["dets"] = [_det_pair(d) for d in (s.get("detections") or [])]
    return out

class SensingState:
    def __init__(self):
        self._lock = threading.Lock()
        self.dl, self.ul, self.tracks, self.ue, self.geometry = [], {}, {}, {}, {}
        self.pipeline = {"gate_open": None, "last_report_wall": None, "cpis": 0}

    def set_geometry(self, g):
        with self._lock:
            self.geometry = {"gnb": g.get("transmitter_position_m"),
                             "rx": [g["receiver_positions_m"][f"rx{i}"] for i in range(4)] if g.get("receiver_positions_m") else []}

    def add_report(self, rep):
        with self._lock:
            srs = rep.get("spatial_receivers") or []
            self.dl = [_receiver_view(s, self.dl[i] if i < len(self.dl) else None) for i, s in enumerate(srs)]
            for sess in rep.get("uplink_sessions") or []:
                k = str(sess.get("pusch_session_id")); old = self.ul.get(k, [])
                self.ul[k] = [_receiver_view(s, old[i] if i < len(old) else None) for i, s in enumerate(sess.get("receivers") or [])]
            p = self.pipeline
            p.update({"cpis": p["cpis"] + 1, "last_report_wall": time.time(),
                      "dropped_cpis": rep.get("dropped_cpis"), "discarded_pending_rows": rep.get("discarded_pending_rows"),
                      "dropped_submissions": rep.get("dropped_submissions"), "cpi_plan": rep.get("cpi_plan")})

    def add_tracks(self, rec):
        with self._lock:
            live = set()
            for t in rec.get("tracks") or []:
                k = str(t["track_id"]); live.add(k)
                e = self.tracks.setdefault(k, {"trail": [], "confirmed": False})
                e["trail"] = (e["trail"] + [list(t["position"])])[-TRAIL:]; e["confirmed"] = bool(t.get("confirmed"))
            for k in [k for k in self.tracks if k not in live]:
                del self.tracks[k]

    def add_status(self, rec):
        with self._lock:
            self.ue = rec.get("ue") or {}
            self.pipeline.update({"chain_ms": rec.get("ms"), "chain_lag_s": time.time() - rec["wall"] if rec.get("wall") else None,
                                  "ul_sessions": rec.get("sessions")})

    def add_log_line(self, line):
        with self._lock:
            p = self.pipeline
            if RE_GATE_OPEN.search(line): p["gate_open"] = True; p["gate_rnti"] = RE_GATE_OPEN.search(line).group(1)
            elif RE_GATE_CLOSE.search(line): p["gate_open"] = False
            m = RE_GATE_STATS.search(line)
            if m: p.update({"gate_open": m.group(1) == "1", "admitted": int(m.group(2)), "rejected": int(m.group(3)),
                            "gate_discarded_rows": int(m.group(4))})
            m = RE_BRFO.search(line)
            if m: p["branchfo_hz"] = [float(x) for x in m.group(1).split()]
            m = RE_LLRCONF.search(line)
            if m: p.setdefault("llrconf", {})[m.group(1)] = {"calibrated": m.group(2) == "1", "tau_rel": float(m.group(3)),
                                                             "agreement": float(m.group(4))}
            if "CUDA" in line and "SENSING" in line: p["backend_line"] = line.strip()[-160:]

    def snapshot(self):
        with self._lock:
            return {"dl": self.dl, "ul": self.ul, "tracks": self.tracks, "ue": self.ue, "geometry": self.geometry,
                    "pipeline": dict(self.pipeline), "now": time.time()}
