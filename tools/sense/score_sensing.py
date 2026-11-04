#!/usr/bin/env python3
"""Offline sensing scorer (two-modes spec section 6). Usage:
  score_sensing.py <records.jsonl>... [--min-prb 128] [--json out.json] [--log run.log]
One records file per cell. Reads the `type":"rs"` lines of sensing mode (nr_sense_rs_to_json, schema 2); a line is
scored only when it carries the quality block (valid_cfr_prbs). A CFR is wideband when valid_cfr_prbs >= --min-prb
(the record's own `wideband` flag is ignored so the threshold can be re-cut offline). Metrics are per cell (PCI) and per
link = (dir, tx, layer/n_layers, prg_size); links are never pooled, cells are never merged. The aggregate is its own
line. Windows are 100 ms and 1 s on a grid starting at the earliest scored record of the whole run (one process, one
monotonic clock). Several records at the same instant count as one event. No absolute CFR claims: relative metrics only."""
import argparse, json, math, os, re, sys

W100, W1S = 100_000_000, 1_000_000_000
HIST_BIN = 32

def pct(v, p):
    """Linear-interpolated percentile (p in 0..1) of a list; None when empty."""
    s = sorted(v)
    if not s: return None
    k = (len(s) - 1) * p; f = math.floor(k); c = min(f + 1, len(s) - 1)
    return s[f] + (s[c] - s[f]) * (k - f)

def dist(v):
    return {"n": len(v), "p10": pct(v, .1), "p50": pct(v, .5), "p90": pct(v, .9)}

def _int(d, k):
    v = d.get(k)
    if isinstance(v, bool) or not isinstance(v, int): raise ValueError(k)
    return v

def _num(d, k):
    v = d.get(k)
    if v is None: return None
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v): raise ValueError(k)
    return float(v)

def parse_file(path, st):
    """-> list of scored records (dicts). st counts malformed / ignored lines."""
    out = []
    with open(path, errors="replace") as f:
        for ln in f:
            if not ln.strip(): continue
            try:
                d = json.loads(ln)
                if not isinstance(d, dict): raise ValueError("object")
                if d.get("type") != "rs": st["ignored_non_rs"] += 1; continue
                if d.get("valid_cfr_prbs") is None: st["rs_without_quality"] += 1; continue
                vp = _int(d, "valid_cfr_prbs")
                mask = bytes.fromhex(d.get("valid_mask") or "")
                if d.get("dir") not in ("DL", "UL"): raise ValueError("dir")
                if _int(d, "t_mono_ns") < 0 or vp < 0: raise ValueError("range")
                pl = d.get("layer"); nl = d.get("n_layers"); prg = d.get("prg_size")
                tx = d.get("tx")
                for x in (pl, nl, prg):
                    if x is not None and (isinstance(x, bool) or not isinstance(x, int)): raise ValueError("int")
                if tx is not None and (isinstance(tx, bool) or not isinstance(tx, (int, str))): raise ValueError("tx")
                first = d.get("first_prb"); first = 0 if first is None else first
                if isinstance(first, bool) or not isinstance(first, int) or first < 0: raise ValueError("first_prb")
                out.append({"t": d["t_mono_ns"], "pci": _int(d, "pci"), "dir": d["dir"], "tx": tx, "layer": pl,
                            "n_layers": nl, "prg": prg, "vp": vp, "mask": mask, "first": first,
                            "snr": _num(d, "snr_db_p50"), "resid": _num(d, "resid_db"), "recon": _num(d, "recon_conf")})
            except (ValueError, TypeError):  # JSONDecodeError is a ValueError
                st["malformed_lines"] += 1
    return out

def cover(times, t0, n, w):
    """Set of window indices (grid of n windows of width w from t0) holding >= 1 time."""
    return {(t - t0) // w for t in times}

def avail(times, t0, tmax):
    out = {}
    for name, w in (("w100ms", W100), ("w1s", W1S)):
        n = (tmax - t0) // w + 1 if tmax is not None else 0
        c = len(cover(times, t0, n, w)) if n else 0
        out[name] = {"windows": n, "covered": c, "fraction": c / n if n else None}
    return out

def gaps(times):
    u = sorted(set(times))
    g = [(b - a) / 1e9 for a, b in zip(u, u[1:])]
    return {"n": len(g), "p50": pct(g, .5), "p95": pct(g, .95), "max": max(g) if g else None}

def occupancy(wb, t0):
    rows = {}; width = 0
    for r in wb:
        row = rows.setdefault((r["t"] - t0) // W1S, {})
        for p in range(len(r["mask"]) * 8):
            if r["mask"][p // 8] >> (p % 8) & 1:
                row[r["first"] + p] = row.get(r["first"] + p, 0) + 1; width = max(width, r["first"] + p + 1)
    n = max(rows) + 1 if rows else 0
    return {"window_s": 1.0, "prbs": width, "rows": [[rows.get(i, {}).get(p, 0) for p in range(width)] for i in range(n)]}

def score_link(recs, min_prb, t0, tmax, span_s):
    wb = [r for r in recs if r["vp"] >= min_prb]
    r0 = recs[0]
    hist = {}
    for r in recs:
        b = str(r["vp"] // HIST_BIN * HIST_BIN); hist[b] = hist.get(b, 0) + 1
    n = len(recs)
    return {"dir": r0["dir"], "tx": r0["tx"], "layer": r0["layer"], "n_layers": r0["n_layers"], "prg_size": r0["prg"],
            "n_produced": n, "n_wideband": len(wb),
            "valid_cfr_rate": sum(r["vp"] > 0 for r in recs) / n, "wideband_rate": len(wb) / n,
            "gap_s": gaps([r["t"] for r in wb]),
            "availability": avail([r["t"] for r in wb], t0, tmax),
            "valid_prbs_per_s_wideband": sum(r["vp"] for r in wb) / span_s if span_s else None,
            "valid_prbs_per_s_all": sum(r["vp"] for r in recs) / span_s if span_s else None,
            "valid_cfr_prbs_hist": dict(sorted(hist.items(), key=lambda kv: int(kv[0]))),
            "snr_db_p50": dist([r["snr"] for r in recs if r["snr"] is not None]),
            "resid_db": dist([r["resid"] for r in recs if r["resid"] is not None]),
            "recon_conf": dist([r["recon"] for r in recs if r["recon"] is not None]),
            "occupancy": occupancy(wb, t0)}

def score(paths, min_prb=128):
    st = {"malformed_lines": 0, "ignored_non_rs": 0, "rs_without_quality": 0}
    per_cell = {}  # (file index, pci) -> records; a record file is one cell, never merged with another
    for i, p in enumerate(paths):
        for r in parse_file(p, st): per_cell.setdefault((i, r["pci"]), []).append(r)
    allr = [r for v in per_cell.values() for r in v]
    t0 = min((r["t"] for r in allr), default=None); tmax = max((r["t"] for r in allr), default=None)
    span_s = (tmax - t0) / 1e9 if allr else 0
    pcis = [k[1] for k in per_cell]
    cells = []
    for key in sorted(per_cell):
        recs = per_cell[key]; label = str(key[1]) if pcis.count(key[1]) == 1 else f"{key[1]}@{os.path.basename(paths[key[0]])}"
        lk = {}
        for r in recs: lk.setdefault((r["dir"], str(r["tx"]), r["layer"], r["n_layers"], r["prg"]), []).append(r)
        wbt = [r["t"] for r in recs if r["vp"] >= min_prb]
        cells.append({"label": label, "pci": key[1], "file": paths[key[0]], "n_produced": len(recs), "n_wideband": len(wbt),
                      "gap_s": gaps(wbt), "availability": avail(wbt, t0, tmax), "wbt": wbt,
                      "links": [score_link(v, min_prb, t0, tmax, span_s) for k, v in sorted(lk.items(), key=lambda kv: str(kv[0]))]})
    allwb = [t for c in cells for t in c["wbt"]]
    ag = {"gap_s": gaps(allwb), "availability": avail(allwb, t0, tmax), "n_wideband": len(allwb),
          "wideband_share": {c["label"]: (c["n_wideband"] / len(allwb) if allwb else None) for c in cells},
          "rescue_fraction": None, "rescue_split": None, "rescue_windows": None}
    if len(cells) > 1 and tmax is not None:  # rescue is only defined with >= 2 cells
        n = (tmax - t0) // W100 + 1
        cov = {c["label"]: cover(c["wbt"], t0, n, W100) for c in cells}
        only = {l: sum(1 for w in range(n) if w in s and not any(w in o for ol, o in cov.items() if ol != l)) for l, s in cov.items()}
        tot = sum(only.values())
        ag["rescue_windows"] = only; ag["rescue_fraction"] = tot / n
        ag["rescue_split"] = {l: (v / tot if tot else None) for l, v in only.items()}
    for c in cells: del c["wbt"]
    return {"min_prb": min_prb, "files": list(paths), "t0_mono_ns": t0, "span_s": span_s, **st, "cells": cells, "aggregate": ag}

def parse_log(path):
    """Last/max of the S5 per-cell stability keys from ISAC_METRICS lines (null where the key is absent)."""
    out = {}
    with open(path, errors="replace") as f:
        for ln in re.sub(r"\x1b\[[0-9;]*m", "", f.read()).splitlines():
            i = ln.find("ISAC_METRICS ")
            if i < 0: continue
            try: d = json.loads(ln[i + 13:])
            except ValueError: continue  # truncated line
            if not isinstance(d, dict): continue
            c = out.setdefault(d.get("cell", 0), {"violations_last": None, "violations_max": None,
                                                  "disabled_skips_last": None, "units_enabled_series": []})
            for key, name in (("sense_violations", "violations"), ("sense_disabled_skips", "disabled_skips")):
                if isinstance(d.get(key), (int, float)):
                    c[name + "_last"] = d[key]
                    if name == "violations": c["violations_max"] = max(d[key], c["violations_max"] or 0)
            if isinstance(d.get("sense_units_enabled"), (int, float)): c["units_enabled_series"].append(d["sense_units_enabled"])
    return out

def f3(x): return "n/a" if x is None else f"{x:.3f}"

def report(res):
    p = print
    p(f"min_prb={res['min_prb']} span={res['span_s']:.3f}s malformed={res['malformed_lines']} "
      f"non_rs={res['ignored_non_rs']} rs_without_quality={res['rs_without_quality']}")
    for c in res["cells"]:
        a = c["availability"]
        p(f"pci {c['label']}: produced={c['n_produced']} wideband={c['n_wideband']} gap p50/p95/max={f3(c['gap_s']['p50'])}/"
          f"{f3(c['gap_s']['p95'])}/{f3(c['gap_s']['max'])} s avail100ms={f3(a['w100ms']['fraction'])} avail1s={f3(a['w1s']['fraction'])}")
        for l in c["links"]:
            g = l["gap_s"]
            p(f"  link {l['dir']} tx={l['tx']} layer={l['layer']}/{l['n_layers']} prg={l['prg_size']}: produced={l['n_produced']} "
              f"wb={l['n_wideband']} gap={f3(g['p50'])}/{f3(g['p95'])}/{f3(g['max'])} avail100ms={f3(l['availability']['w100ms']['fraction'])} "
              f"avail1s={f3(l['availability']['w1s']['fraction'])} PRB/s wb={f3(l['valid_prbs_per_s_wideband'])} "
              f"snr_p50={f3(l['snr_db_p50']['p50'])} resid_p50={f3(l['resid_db']['p50'])} recon_p50={f3(l['recon_conf']['p50'])} "
              f"valid_rate={f3(l['valid_cfr_rate'])} hist={l['valid_cfr_prbs_hist']}")
    a = res["aggregate"]
    p(f"AGGREGATE (union of cells): wb={a['n_wideband']} gap p50/p95/max={f3(a['gap_s']['p50'])}/{f3(a['gap_s']['p95'])}/"
      f"{f3(a['gap_s']['max'])} s avail100ms={f3(a['availability']['w100ms']['fraction'])} avail1s={f3(a['availability']['w1s']['fraction'])} "
      f"rescue_fraction={f3(a['rescue_fraction'])} rescue_split={a['rescue_split']}")

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("records", nargs="+"); ap.add_argument("--min-prb", type=int, default=128)
    ap.add_argument("--json"); ap.add_argument("--log")
    a = ap.parse_args(argv)
    res = score(a.records, a.min_prb)
    if a.log: res["stability"] = {str(k): v for k, v in parse_log(a.log).items()}
    report(res)
    if a.log: print("STABILITY", json.dumps(res["stability"]))
    if a.json:
        with open(a.json, "w") as f: json.dump(res, f)
    return 0

if __name__ == "__main__":
    sys.exit(main())
