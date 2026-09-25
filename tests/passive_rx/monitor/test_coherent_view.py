#!/usr/bin/env python3
import json, os, tempfile, time
import sys; sys.path.insert(0, os.path.dirname(__file__))
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
    assert s["traffic"]["open"] is None or s["traffic"]["open"] is True
    # traffic off: the pipeline's event clears detections and tracks, and the state is exposed for the banner
    w("coherent_reports", "B", [{"event": "traffic", "traffic": False, "t": 0.6, "wall": time.time(), "detections": []}])
    w("coherent_tracks", "B", [{"event": "traffic", "traffic": False, "t": 0.6, "tracks": []}])
    time.sleep(0.5); s = v.snapshot()
    assert s["traffic"]["open"] is False and s["traffic"]["age_s"] < 5, s["traffic"]
    assert all(not r["detections"] for r in s["reports"]), "no detections while traffic is off"
    assert s["tracks"]["tracks"] == [] and not s["track_trails"], "tracks cleared"
    # traffic back: normal reports resume
    w("coherent_reports", "B", [{"cpi": 8, "traffic": True, "t": 0.7, "detections": [{"p": [1, 2, 3]}], "timing_ms": {"total": 6}, "t_cpi_s": 0.075, "stats": {}, "gpu": True, "topview": None, "rd": None}])
    time.sleep(0.5); s = v.snapshot()
    assert s["traffic"]["open"] is True and s["reports"][-1]["detections"], "resumes"
print("test_coherent_view: PASS")
