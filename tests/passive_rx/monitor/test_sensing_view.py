#!/usr/bin/env python3
"""Self-check for sensing_view.py's pure extraction (no I/O, no HTTP).

    python3 tests/passive_rx/monitor/test_sensing_view.py
"""
import math, sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from sensing_view import SensingState

# report_writer.cc emits "bistatic_range_m"/"bistatic_velocity_mps" for DL spatial-receiver
# detections (openair1/PHY/NR_UE_ISAC/report_writer.cc:298-299) -- NOT "range_rate_mps" as an
# earlier draft of this test assumed. UL detections use yet another pair,
# "delta_path_range_m"/"delta_path_range_rate_mps" (report_writer.cc:478-480); sensing_view.py
# accepts both so the one _receiver_view() helper works for spatial_receivers[] and
# uplink_sessions[].receivers[] alike.

def spatial(i):
    return {"receiver_id": f"rx{i}", "receiver_position_enu_m": [i, 0, 2], "rvm_range_bins": 3, "rvm_rate_bins": 2,
            "rvm_range_res_m": 3.05, "rvm_rate_res_mps": 1.1, "rvm_blob": [1, 10, 100, 1000, 0, 1],
            "detections": [{"bistatic_range_m": 6.1, "bistatic_velocity_mps": 1.1}]}

def test_maps_and_ul():
    s = SensingState(); s.set_geometry({"transmitter_position_m": [9, 9, 9], "receiver_positions_m": {f"rx{i}": [i, 0, 2] for i in range(4)}})
    rep = {"midpoint_air_time_s": 1.0, "spatial_receivers": [spatial(i) for i in range(4)],
           "uplink_sessions": [{"pusch_session_id": 17921, "receivers": [spatial(i) for i in range(4)]}]}
    s.add_report(rep); snap = s.snapshot()
    assert len(snap["dl"]) == 4 and snap["dl"][0]["nb"] == 3 and snap["dl"][0]["nr"] == 2
    assert snap["dl"][0]["map_db"][1] == 10.0 and snap["dl"][0]["map_db"][4] is None   # 10*log10; zero -> None
    assert snap["dl"][2]["dets"] == [[6.1, 1.1]]
    assert list(snap["ul"]) == ["17921"] and len(snap["ul"]["17921"]) == 4
    assert snap["geometry"]["gnb"] == [9, 9, 9]

def test_ul_detection_key_names():
    """UL detections carry delta_path_range_m / delta_path_range_rate_mps, not the DL names."""
    s = SensingState()
    rx = {"receiver_id": "rx0", "detections": [{"delta_path_range_m": 12.5, "delta_path_range_rate_mps": -2.0}]}
    s.add_report({"uplink_sessions": [{"pusch_session_id": 1, "receivers": [rx]}]})
    assert s.snapshot()["ul"]["1"][0]["dets"] == [[12.5, -2.0]]

def test_map_persists_when_report_has_none():
    s = SensingState(); s.add_report({"spatial_receivers": [spatial(0)]})
    s.add_report({"spatial_receivers": [{"receiver_id": "rx0", "detections": []}]})
    assert s.snapshot()["dl"][0]["nb"] == 3          # decimated reports keep the last map

def test_tracks_trail_and_expiry():
    s = SensingState()
    for t in range(3):
        s.add_tracks({"time_s": t, "tracks": [{"track_id": 5, "confirmed": True, "position": [t, 0, 1]}]})
    assert s.snapshot()["tracks"]["5"]["trail"] == [[0, 0, 1], [1, 0, 1], [2, 0, 1]]
    s.add_tracks({"time_s": 3, "tracks": []})
    assert "5" not in s.snapshot()["tracks"]

def test_pipeline_from_log():
    s = SensingState()
    s.add_log_line("[PHY] SENSING_GATE open rnti=0x4601")
    s.add_log_line("[PHY] SENSING_GATE stats open=1 admitted=10 rejected=2 gate_discarded_rows=0")
    s.add_log_line("[PHY] SENSING: BRANCHFO d_vs_br0=[0.0 3.2 -1.1 4.0] Hz (EMA")
    p = s.snapshot()["pipeline"]
    assert p["gate_open"] is True and p["admitted"] == 10 and p["branchfo_hz"] == [0.0, 3.2, -1.1, 4.0]

if __name__ == "__main__":
    test_maps_and_ul(); test_ul_detection_key_names(); test_map_persists_when_report_has_none()
    test_tracks_trail_and_expiry(); test_pipeline_from_log()
    print("test_sensing_view: PASS")
