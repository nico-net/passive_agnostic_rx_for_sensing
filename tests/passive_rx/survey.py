#!/usr/bin/env python3
# tests/passive_rx/survey.py
"""Site survey -> engine conf keys + realtime_chain geometry, from ONE file (spec §7).
Frame: ENU metres, origin at the X410. ch<i> = X410 RX channel i = --ue-nb-ant-rx order."""
import argparse, json, math, re, sys

def die(msg):
    print(f"survey: {msg}", file=sys.stderr); sys.exit(2)

def fmt(x):
    return f"{x:g}"

def fmt_real(x):
    # libconfig types a bare "120" as INT; config_lookup_float() rejects that and OAI
    # silently falls back to its compiled-in default. Real-typed keys always need '.'.
    s = fmt(x)
    return s if any(c in s for c in ".eEn") else s + ".0"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("survey"); ap.add_argument("--geometry", required=True)
    ap.add_argument("--apply", nargs=2, metavar=("TEMPLATE", "OUT"), required=True)
    ap.add_argument("--report-path")
    a = ap.parse_args()
    s = json.load(open(a.survey))
    gnb = s.get("gnb_m"); ants = s.get("rx_antennas_m") or {}
    rx = []
    for i in range(4):
        p = ants.get(f"ch{i}")
        if p is None: die(f"missing ch{i}")
        if len(p) != 3 or not all(isinstance(v, (int, float)) and math.isfinite(v) for v in p): die(f"ch{i} is not [x,y,z]")
        rx.append([float(v) for v in p])
    if not gnb or len(gnb) != 3: die("gnb_m must be [x,y,z]")
    for i in range(4):
        for j in range(i + 1, 4):
            if math.dist(rx[i], rx[j]) < 1.0: die(f"ch{i} and ch{j} are co-located (<1 m): stage 9 needs separated receivers")
        if math.dist(rx[i], gnb) < 1.0: die(f"ch{i} co-located with the gNB")
    max_range = float(s.get("max_range_m", 0.0))
    if max_range <= 0: die("max_range_m missing or <= 0 in survey (required by the sensing engine)")
    json.dump({"transmitter_position_m": gnb, "receiver_positions_m": {f"rx{i}": rx[i] for i in range(4)},
               "max_range_m": max_range or None}, open(a.geometry, "w"), indent=1)
    keys = {"spatial_rx_positions": '"' + ";".join(",".join(fmt(v) for v in p) for p in rx) + '"',
            "tx_pos_x": fmt_real(gnb[0]), "tx_pos_y": fmt_real(gnb[1]), "tx_pos_z": fmt_real(gnb[2])}
    keys["max_range_m"] = fmt_real(max_range)
    keys["rvm_max_range_m"] = fmt_real(max_range)
    if a.report_path: keys["report_path"] = f'"{a.report_path}"'
    text = open(a.apply[0]).read()
    m = re.search(r"^\s*sensing\s*=\s*\{", text, re.M)
    if not m: die("template has no 'sensing = {' block")
    end = text.index("};", m.end())  # assumes no "};" inside a comment/string before the block end
    body = text[m.end():end]
    for k, v in keys.items():
        line = f"  {k} = {v};"
        body, n = re.subn(rf"^\s*{k}\s*=.*?;\s*$", line, body, flags=re.M)
        if n == 0: body = body.rstrip("\n") + "\n" + line + "\n"
    open(a.apply[1], "w").write(text[:m.end()] + body + text[end:])

if __name__ == "__main__":
    main()
