#!/usr/bin/env python3
"""Offline sensing scorer (two-modes spec section 6). Usage:
  score_sensing.py <records.jsonl>... [--min-prb 128] [--json out.json] [--log run.log]
One records file per cell. Reads the `type":"rs"` lines of sensing mode (nr_sense_rs_to_json, schema 2); a line is
scored only when it carries the quality block (valid_cfr_prbs). A CFR is wideband when valid_cfr_prbs >= --min-prb
(the record's own `wideband` flag is ignored so the threshold can be re-cut offline). Metrics are per cell (PCI) and per
link = (dir, tx, layer/n_layers, prg_size); links are never pooled, cells are never merged. The aggregate is its own
line. Windows are 100 ms and 1 s on a grid starting at the earliest scored record of the whole run (one process, one
monotonic clock; files whose time ranges neither chain-overlap nor fit in 2x the longest file are refused unless
--allow-mixed-clocks; a combined span over 24 h is always refused). Availability and PRBs/s of every link use this
run-wide grid/span, not the link's own span. A link is keyed by (dir, tx, layer, n_layers, prg_size, ssb_beam,
port_mask): records of unknown layer (key absent) or unlatched prg_size (null) form their own link, separate from
known ones. The occupancy map rows are absolute-second aligned (floor(t_mono_ns/1e9)). Several records at the same instant count as one event. No absolute CFR claims: relative metrics only."""
import argparse, json, math, os, re, sys
from array import array

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

T_MAX_NS = 4 * 10**17      # ~12.7 years of monotonic ns: larger is a corrupt timestamp (counted malformed)
MAX_1S_WINDOWS = 86400     # refuse a combined span of more than 24 h (corrupt-timestamp guard, not overridable)

class ScoreError(Exception): pass

class Link:
    """Compact per-link accumulator: scalars, histogram, occupancy counters and array('q'/'d') columns."""
    def __init__(self, r):
        self.dir, self.tx, self.layer, self.n_layers, self.prg = r["dir"], r["tx"], r["layer"], r["n_layers"], r["prg"]
        self.ssb_beam, self.port_mask = r["ssb_beam"], r["port_mask"]
        self.n = self.n_valid = self.n_wb = self.sum_all = self.sum_wb = 0
        self.hist = {}; self.occ = {}; self.width = 0
        self.wbt = array("q"); self.snr = array("d"); self.resid = array("d"); self.recon = array("d")

def link_key(r):
    return (r["dir"], str(r["tx"]), r["layer"], r["n_layers"], r["prg"], r["ssb_beam"], r["port_mask"])

def opt_int(d, k):
    v = d.get(k)
    if v is not None and (isinstance(v, bool) or not isinstance(v, int)): raise ValueError(k)
    return v

def parse_line(ln, st):
    """-> record dict, or None (ignored / malformed, counted in st)."""
    try:
        d = json.loads(ln)
        if not isinstance(d, dict): raise ValueError("object")
        if d.get("type") != "rs": st["ignored_non_rs"] += 1; return None
        if d.get("valid_cfr_prbs") is None: st["rs_without_quality"] += 1; return None
        vp = _int(d, "valid_cfr_prbs"); t = _int(d, "t_mono_ns")
        mask = bytes.fromhex(d.get("valid_mask") or "")
        if d.get("dir") not in ("DL", "UL"): raise ValueError("dir")
        if not 0 <= t < T_MAX_NS or vp < 0: raise ValueError("range")
        tx = d.get("tx")
        if tx is not None and (isinstance(tx, bool) or not isinstance(tx, (int, str))): raise ValueError("tx")
        first = d.get("first_prb"); first = 0 if first is None else first
        if isinstance(first, bool) or not isinstance(first, int) or first < 0: raise ValueError("first_prb")
        return {"t": t, "pci": _int(d, "pci"), "dir": d["dir"], "tx": tx, "layer": opt_int(d, "layer"),
                "n_layers": opt_int(d, "n_layers"), "prg": opt_int(d, "prg_size"), "ssb_beam": opt_int(d, "ssb_beam"),
                "port_mask": opt_int(d, "port_mask"), "vp": vp, "mask": mask, "first": first,
                "snr": _num(d, "snr_db_p50"), "resid": _num(d, "resid_db"), "recon": _num(d, "recon_conf")}
    except (ValueError, TypeError):  # JSONDecodeError is a ValueError
        st["malformed_lines"] += 1; return None

def fold(lk, r, min_prb):
    lk.n += 1; lk.sum_all += r["vp"]; lk.n_valid += r["vp"] > 0
    b = str(r["vp"] // HIST_BIN * HIST_BIN); lk.hist[b] = lk.hist.get(b, 0) + 1
    for name in ("snr", "resid", "recon"):
        if r[name] is not None: getattr(lk, name).append(r[name])
    if r["vp"] >= min_prb:
        lk.n_wb += 1; lk.sum_wb += r["vp"]; lk.wbt.append(r["t"])
        sec = r["t"] // W1S; m = r["mask"]   # occupancy rows are absolute-second aligned (shortcut: no run-wide t0 in one pass)
        for p in range(len(m) * 8):
            if m[p // 8] >> (p % 8) & 1:
                k = (sec, r["first"] + p); lk.occ[k] = lk.occ.get(k, 0) + 1; lk.width = max(lk.width, r["first"] + p + 1)

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

def occupancy(lk):
    if not lk.occ: return {"window_s": 1.0, "prbs": 0, "first_sec_abs": None, "rows": []}
    s0 = min(k[0] for k in lk.occ); n = max(k[0] for k in lk.occ) - s0 + 1
    if n > MAX_1S_WINDOWS: raise ScoreError(f"occupancy span {n} s exceeds {MAX_1S_WINDOWS} s (corrupt timestamps?)")
    rows = [[0] * lk.width for _ in range(n)]
    for (sec, p), c in lk.occ.items(): rows[sec - s0][p] = c
    return {"window_s": 1.0, "prbs": lk.width, "first_sec_abs": s0, "rows": rows}

def score_link(lk, t0, tmax, span_s):
    return {"dir": lk.dir, "tx": lk.tx, "layer": lk.layer, "n_layers": lk.n_layers, "prg_size": lk.prg,
            "ssb_beam": lk.ssb_beam, "port_mask": lk.port_mask, "n_produced": lk.n, "n_wideband": lk.n_wb,
            "valid_cfr_rate": lk.n_valid / lk.n, "wideband_rate": lk.n_wb / lk.n,
            "gap_s": gaps(lk.wbt), "availability": avail(lk.wbt, t0, tmax),
            "valid_prbs_per_s_wideband": lk.sum_wb / span_s if span_s else None,
            "valid_prbs_per_s_all": lk.sum_all / span_s if span_s else None,
            "valid_cfr_prbs_hist": dict(sorted(lk.hist.items(), key=lambda kv: int(kv[0]))),
            "snr_db_p50": dist(lk.snr), "resid_db": dist(lk.resid), "recon_conf": dist(lk.recon),
            "occupancy": occupancy(lk)}

def check_clocks(ranges, allow):
    """ranges: per-file (tmin, tmax). Mixed runs/hosts have unrelated monotonic clocks: refuse unless the ranges
    chain-overlap or the combined span is <= 2x the longest single-file span."""
    if len(ranges) < 2 or allow: return
    rs = sorted(ranges); comb = max(r[1] for r in rs) - rs[0][0]
    longest = max(r[1] - r[0] for r in rs); end = rs[0][1]; chained = True
    for lo, hi in rs[1:]:
        if lo > end: chained = False
        end = max(end, hi)
    if not chained and comb > 2 * longest:
        raise ScoreError(f"record files do not share a clock: ranges {rs} (combined span {comb/1e9:.1f} s, longest single "
                         f"file {longest/1e9:.1f} s); different runs/hosts must not be mixed (--allow-mixed-clocks overrides)")

def score(paths, min_prb=128, allow_mixed_clocks=False):
    st = {"malformed_lines": 0, "ignored_non_rs": 0, "rs_without_quality": 0}
    links = {}   # (file index, pci) -> {link key -> Link}; a record file is one cell, never merged with another
    ranges = []
    for i, p in enumerate(paths):
        lo = hi = None
        with open(p, errors="replace") as f:
            for ln in f:
                if not ln.strip(): continue
                r = parse_line(ln, st)
                if r is None: continue
                lo = r["t"] if lo is None else min(lo, r["t"]); hi = r["t"] if hi is None else max(hi, r["t"])
                cell = links.setdefault((i, r["pci"]), {})
                lk = cell.get(link_key(r))
                if lk is None: lk = cell[link_key(r)] = Link(r)
                fold(lk, r, min_prb)
        if lo is not None: ranges.append((lo, hi))
    check_clocks(ranges, allow_mixed_clocks)
    t0 = min((r[0] for r in ranges), default=None); tmax = max((r[1] for r in ranges), default=None)
    if t0 is not None and (tmax - t0) // W1S + 1 > MAX_1S_WINDOWS:
        raise ScoreError(f"combined span {(tmax - t0)/1e9:.0f} s exceeds {MAX_1S_WINDOWS} s (corrupt timestamps or mixed clocks)")
    span_s = (tmax - t0) / 1e9 if ranges else 0
    pcis = [k[1] for k in links]
    cells = []
    for key in sorted(links):
        label = str(key[1]) if pcis.count(key[1]) == 1 else f"{key[1]}@{os.path.basename(paths[key[0]])}"
        lks = [v for _, v in sorted(links[key].items(), key=lambda kv: str(kv[0]))]
        wbt = [t for lk in lks for t in lk.wbt]
        cells.append({"label": label, "pci": key[1], "file": paths[key[0]], "n_produced": sum(l.n for l in lks),
                      "n_wideband": len(wbt), "gap_s": gaps(wbt), "availability": avail(wbt, t0, tmax), "wbt": wbt,
                      "links": [score_link(l, t0, tmax, span_s) for l in lks]})
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
    ap.add_argument("--json"); ap.add_argument("--log"); ap.add_argument("--allow-mixed-clocks", action="store_true")
    a = ap.parse_args(argv)
    try: res = score(a.records, a.min_prb, a.allow_mixed_clocks)
    except ScoreError as e:
        print(f"score_sensing: error: {e}", file=sys.stderr); return 2
    if a.log: res["stability"] = {str(k): v for k, v in parse_log(a.log).items()}
    report(res)
    if a.log: print("STABILITY", json.dumps(res["stability"]))
    if a.json:
        with open(a.json, "w") as f: json.dump(res, f)
    return 0

if __name__ == "__main__":
    sys.exit(main())
