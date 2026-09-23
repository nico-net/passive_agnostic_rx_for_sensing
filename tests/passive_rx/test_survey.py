import json, subprocess, sys, tempfile, pathlib
HERE = pathlib.Path(__file__).parent
CONF = 'uicc0 = { imsi = "0"; };\nsensing = {\n  enable = 1;\n  tx_pos_x = 0.0;\n};\n'

def run(survey, tmp):
    s = tmp / "s.json"; s.write_text(json.dumps(survey)); t = tmp / "t.conf"; t.write_text(CONF)
    return subprocess.run([sys.executable, str(HERE / "survey.py"), str(s), "--geometry", str(tmp / "g.json"),
                           "--apply", str(t), str(tmp / "o.conf"), "--report-path", "/x/r.jsonl"],
                          capture_output=True, text=True)

GOOD = {"gnb_m": [120, 80, 25], "rx_antennas_m": {"ch0": [0, 0, 2], "ch1": [40, 0, 2], "ch2": [0, 40, 2], "ch3": [40, 40, 6]},
        "max_range_m": 600}

def test_good():
    with tempfile.TemporaryDirectory() as d:
        d = pathlib.Path(d); r = run(GOOD, d); assert r.returncode == 0, r.stderr
        g = json.loads((d / "g.json").read_text())
        assert g["transmitter_position_m"] == [120, 80, 25] and g["receiver_positions_m"]["rx3"] == [40, 40, 6]
        conf = (d / "o.conf").read_text()
        assert 'spatial_rx_positions = "0,0,2;40,0,2;0,40,2;40,40,6";' in conf
        assert "tx_pos_x = 120;" in conf and conf.count("tx_pos_x") == 1
        assert 'report_path = "/x/r.jsonl";' in conf and "rvm_max_range_m = 600;" in conf
        assert conf.index("spatial_rx_positions") < conf.index("};", conf.index("sensing"))

def test_colocated_rejected():
    bad = json.loads(json.dumps(GOOD)); bad["rx_antennas_m"]["ch1"] = [0.3, 0, 2]
    with tempfile.TemporaryDirectory() as d:
        r = run(bad, pathlib.Path(d)); assert r.returncode == 2 and "co-located" in r.stderr

def test_missing_channel_rejected():
    bad = json.loads(json.dumps(GOOD)); del bad["rx_antennas_m"]["ch3"]
    with tempfile.TemporaryDirectory() as d:
        r = run(bad, pathlib.Path(d)); assert r.returncode == 2 and "ch3" in r.stderr

if __name__ == "__main__":
    test_good(); test_colocated_rejected(); test_missing_channel_rejected(); print("test_survey: PASS")
