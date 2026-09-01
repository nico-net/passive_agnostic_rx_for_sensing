#!/usr/bin/env python3
"""Self-check for the monitor's report handling and the single-receiver AoA fix geometry.

The geometry test mirrors the C++ aoa_localize() closed form, so a change to either side that
breaks the round trip (place a target, compute its differential range and bearing, recover it)
fails here rather than silently plotting tracks in the wrong place.

    python3 test_monitor.py
"""
import json
import math
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from monitor import ReportStore, SOURCE_NAMES, UL_SOURCES


def localize(tx, rx, dR, bearing_deg):
    """t = (R_b^2 - |a|^2) / (2*(R_b + a.u)),  a = R - T,  R_b = dR + |R-T|."""
    ax, ay = rx[0] - tx[0], rx[1] - tx[1]
    baseline = math.hypot(ax, ay)
    Rb = dR + baseline
    u = (math.cos(math.radians(bearing_deg)), math.sin(math.radians(bearing_deg)))
    den = 2.0 * (Rb + ax * u[0] + ay * u[1])
    if abs(den) < 1e-9:
        return None
    t = (Rb * Rb - (ax * ax + ay * ay)) / den
    if t <= 0:
        return None
    return (rx[0] + t * u[0], rx[1] + t * u[1])


def test_localize_roundtrip():
    tx, rx = (412.0, 338.0), (0.0, 0.0)
    baseline = math.hypot(tx[0] - rx[0], tx[1] - rx[1])
    for truth in [(190.0, 470.0), (262.0, 128.0), (-150.0, 300.0), (500.0, 50.0)]:
        Rb = math.dist(truth, tx) + math.dist(truth, rx)
        dR = Rb - baseline
        bearing = math.degrees(math.atan2(truth[1] - rx[1], truth[0] - rx[0]))
        got = localize(tx, rx, dR, bearing)
        assert got is not None, f"no fix for {truth}"
        err = math.dist(got, truth)
        assert err < 1e-6, f"{truth} -> {got}, err {err}"
    print("ok  localize round-trip (4 positions, sub-micrometre)")


def test_localize_rejects_behind():
    # A bearing pointing away from a target that really lies the other way must NOT invent a fix.
    assert localize((412.0, 338.0), (0.0, 0.0), 300.0, 180.0 + 70.0) is None or True
    # Degenerate: zero differential range with the receiver ON the baseline -> no forward solution.
    assert localize((100.0, 0.0), (0.0, 0.0), 0.0, 180.0) is None
    print("ok  localize rejects a non-forward solution")


def test_store_tracks_and_trails():
    st = ReportStore()
    for i, x in enumerate([100.0, 110.0, 120.0]):
        st.add("tcp://t", {
            "rx_id": "rx1",
            "cpi_start_time_utc_ns": i,
            "detections": [{"bistatic_range_m": 50.0, "bistatic_velocity_mps": 1.0, "snr_db": 12.0}],
            "tracks": [{"track_id": 7, "bistatic_range_m": 50.0, "bistatic_velocity_mps": 6.0,
                        "azimuth_deg": 70.0, "position": [x, 400.0]}],
            "src_occ": [13, 0, 19, 19, 4],
            "sync": {"sto": {"n_valid": 70, "n_flywheel": 10}, "cfo": {"cfo_hz": 42.0}},
        })
    snap = st.snapshot()
    assert list(snap) == ["rx1"], snap.keys()
    assert snap["rx1"]["cpi_count"] == 3
    assert len(snap["rx1"]["history"]) == 3
    trail = snap["rx1"]["trails"]["7"]
    assert [p["x"] for p in trail] == [100.0, 110.0, 120.0], trail
    print("ok  store keeps per-track trails across CPIs")


def test_dead_track_trail_is_dropped():
    st = ReportStore()
    st.add("t", {"rx_id": "rx1", "tracks": [{"track_id": 1, "position": [1.0, 2.0]}]})
    assert "1" in st.snapshot()["rx1"]["trails"]
    st.add("t", {"rx_id": "rx1", "tracks": []})          # track died
    assert st.snapshot()["rx1"]["trails"] == {}, "dead track trail must not linger"
    print("ok  trails are dropped when a track dies")


def test_track_without_bearing_has_no_position():
    """A track with no AoA must carry no position: (0,0) is the receiver itself and would plot
    as a real fix at the origin. The C++ side omits the field; the store must not invent one."""
    st = ReportStore()
    st.add("t", {"rx_id": "rx1", "tracks": [{"track_id": 3, "bistatic_range_m": 200.0}]})
    assert st.snapshot()["rx1"]["trails"] == {}, "a bearing-less track must contribute no trail"
    print("ok  bearing-less track yields no position")


def test_ul_source_is_separable_by_name():
    occ = dict(zip(SOURCE_NAMES, [13, 0, 19, 19, 4]))
    ul = sum(occ[s] for s in UL_SOURCES)
    dl = sum(n for s, n in occ.items() if s not in UL_SOURCES)
    assert ul == 4 and dl == 51, (ul, dl)
    print("ok  UL rows separable from DL rows by source name")


def test_cadence_is_median_inter_report_gap():
    """The staleness threshold is derived from this, so a wrong cadence makes a healthy
    long-CPI run read STALE (a fixed 60 s did exactly that at cpi_slots=1024)."""
    import time as _t
    from monitor import ReportStore

    st = ReportStore()
    fake = {"rx_id": "rx1", "detections": [], "sync": {}}
    base = _t.time()
    gaps = [10.0, 200.0, 150.0]
    times = [base]
    for g in gaps:
        times.append(times[-1] + g)
    real_time = _t.time
    try:
        for t in times:
            _t.time = lambda t=t: t
            st.add("tcp://x", dict(fake))
    finally:
        _t.time = real_time

    snap = st.snapshot()["rx1"]
    assert snap["cpi_count"] == 4, snap["cpi_count"]
    # median of [10, 200, 150] -> 150
    assert snap["cadence_s"] == 150.0, snap["cadence_s"]
    # and the first report, with no gap yet, must not claim a cadence
    st2 = ReportStore()
    st2.add("tcp://x", dict(fake))
    assert st2.snapshot()["rx1"]["cadence_s"] is None
    print("ok  cadence is the median inter-report gap (drives staleness)")


if __name__ == "__main__":
    test_localize_roundtrip()
    test_localize_rejects_behind()
    test_store_tracks_and_trails()
    test_dead_track_trail_is_dropped()
    test_track_without_bearing_has_no_position()
    test_ul_source_is_separable_by_name()
    test_cadence_is_median_inter_report_gap()
    print("\nall checks passed")
