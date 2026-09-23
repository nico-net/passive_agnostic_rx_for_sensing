import sys, pathlib, types
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import realtime_chain as rc

made = {"loc": [], "s8": [], "s9": []}
class S8:
    def __init__(self, *a, **k): made["s8"].append(k.get("ul_session"))
    def feed(self, rec): return []
class Loc:
    def __init__(self, rx, rr, vr, w, ul_session=None, tx_position=None, max_range_m=None, ul_advance=None):
        made["loc"].append((rr, vr, ul_session, max_range_m))
    def feed(self, rec): return {"position_m": [1, 2, 3], "position_sigma_m": [1, 1, 1]}
class S9:
    def __init__(self, tx, ue, rx, rate): made["s9"].append(rate)
    def step(self, t, legs): return None
class T10:
    def feed(self, m): return None
    def flush(self): return []
rc.Stage8Stream, rc.LocaliserStream, rc.Stage9Stream, rc.TrackerStream = S8, Loc, S9, T10

def rep(t, sessions):
    return {"midpoint_air_time_s": t,
            "spatial_receivers": [{"range_res_m": 2.0, "vel_res_mps": 0.5, "range_max_m": 800.0} for _ in range(4)],
            "uplink_sessions": [{"pusch_session_id": s} for s in sessions]}

ch = rc.RealtimeChain([0, 0, 0], [[0, 0, 0]] * 4, None, max_range_m=None)
ch.feed(rep(0.1, []))
assert made["s9"] == [0.5], made
assert made["loc"] == [], "no UE sessions yet"
ch.feed(rep(0.2, [0x4601]))
assert made["loc"] == [(2.0, 0.5, 0x4601, 800.0)], made["loc"]
assert 0x4601 in made["s8"]
ch.feed(rep(0.3, [0x4601, 0x4602]))
assert [x[2] for x in made["loc"]] == [0x4601, 0x4602]
assert set(ch.status()["sessions"]) == {0x4601, 0x4602}
print("test_realtime_chain_agnostic: PASS")
