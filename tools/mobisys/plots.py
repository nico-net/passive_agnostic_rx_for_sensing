#!/usr/bin/env python3
"""Figures from persisted exporter datasets only. A figure whose input is absent is listed in MISSING.txt, never faked.

  python3 plots.py --lab LABDIR --commercial COMDIR --out PLOTDIR [--sessions DIR DIR ...]

Per figure: <name>.{pdf,svg,png} + <name>.csv (the plotted data) + <name>.json (inputs/config). Proportions carry
Wilson 95 % intervals; generalisation claims need several --sessions (a session is the unit, not a grant).
"""
import argparse
import csv
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import store  # noqa: E402
from groundtruth import wilson  # noqa: E402
from hist import Hist  # noqa: E402

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except Exception:  # noqa: BLE001
    plt = None

C = ("#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#7f7f7f")


class Src:
    def __init__(self, path):
        self.path = path
        self.man = _j(os.path.join(path, "manifest.json")) or {}
        self.sum = _j(os.path.join(path, "summary.json")) or {}
        self.gt = _j(os.path.join(path, "groundtruth", "summary.json"))
        self._ds = {}

    def ds(self, name):
        if name not in self._ds:
            self._ds[name] = store.read_dataset(self.path, name)
        return self._ds[name]


def _j(p):
    try:
        with open(p) as f:
            return json.load(f)
    except OSError:
        return None


def cold_start_stats(summaries):
    """Sessions are the unit; a session that never reached CRC-OK is a failure in the denominator."""
    n = len(summaries)
    k = sum(1 for s in summaries if (s.get("cold_start") or {}).get("success"))
    lo, hi = wilson(k, n)
    return {"sessions": n, "successes": k, "rate": k / n if n else None, "ci95": [lo, hi]}


# ---- builders: (src) -> (rows, draw(fig)) or None when the input is absent
def f_latency_cdf(s):
    h = {}
    for r in s.ds("latency_histograms"):
        h.setdefault(r["metric"], Hist()).merge(Hist.from_rows([(r["bucket_lo_us"], r["bucket_hi_us"], r["count"])]))
    h = {m: x for m, x in h.items() if m in ("e2e_tot", "legacy_dec_us") and x.n}
    if not h:
        return None
    rows, series = [], {}
    for m, x in h.items():
        acc, xs, ys = 0, [], []
        for lo, hi, c in x.to_rows():
            acc += c
            xs.append(hi), ys.append(acc / x.n)
            rows.append({"metric": m, "latency_us": hi, "cdf": acc / x.n})
        series[m] = (xs, ys)

    def draw(fig):
        ax = fig.subplots()
        for i, (m, (xs, ys)) in enumerate(series.items()):
            ax.step(xs, ys, where="post", color=C[i], label=m)
        ax.set_xscale("log"), ax.set_xlabel("latency (us)"), ax.set_ylabel("CDF"), ax.legend()
        ax.set_title("e2e_tot = slot arrival to decode end; legacy_dec_us = decode call only", fontsize=7)
    return rows, draw


def f_crc_by_mcs(s):
    agg = {}
    for r in s.ds("cell_aggregates"):
        if r["dir"] == "dl":
            a = agg.setdefault(r["mcs_bucket"], [0, 0])
            a[0] += r["n_crc_ok"]
            a[1] += r["n_grants"]
    if not agg:
        return None
    order = [m for m in ("0-4", "5-9", "10-16", "17-27", "na") if m in agg]
    rows = []
    for m in order:
        lo, hi = wilson(*agg[m])
        rows.append({"mcs_bucket": m, "crc_ok": agg[m][0], "n": agg[m][1], "rate": agg[m][0] / agg[m][1], "ci_lo": lo, "ci_hi": hi})

    def draw(fig):
        ax = fig.subplots()
        x = range(len(rows))
        ax.bar(x, [r["rate"] for r in rows], color=C[0],
               yerr=[[r["rate"] - r["ci_lo"] for r in rows], [r["ci_hi"] - r["rate"] for r in rows]], capsize=3)
        ax.set_xticks(list(x), [r["mcs_bucket"] for r in rows]), ax.set_ylim(0, 1.05)
        ax.set_xlabel("MCS bucket"), ax.set_ylabel("DL CRC-OK rate (Wilson 95 %)")
    return rows, draw


def f_grants_timeline(s):
    w = {}
    for r in s.ds("cell_aggregates"):
        a = w.setdefault(r["window_start_s"], [0, 0])
        a[0] += r["n_grants"]
        a[1] += r["n_crc_ok"]
    if not w:
        return None
    ks = sorted(w)
    rows = [{"window_start_s": k, "n_grants": w[k][0], "n_crc_ok": w[k][1]} for k in ks]

    def draw(fig):
        ax = fig.subplots()
        t = [(k - ks[0]) / 60 for k in ks]
        ax.plot(t, [w[k][0] for k in ks], "o-", color=C[0], label="grants (unsuppressed)")
        ax.plot(t, [w[k][1] for k in ks], "s-", color=C[2], label="CRC OK")
        ax.set_xlabel("minutes since first window"), ax.set_ylabel("per 60-s window"), ax.legend()
    return rows, draw


def f_cpu_rss(s):
    m = [r for r in s.ds("system_telemetry") if r.get("src") == "metrics" and r.get("proc_utime_us") is not None]
    if len(m) < 2:
        return None
    rows = []
    for a, b in zip(m, m[1:]):
        dt = (b["t_mono_ns"] - a["t_mono_ns"]) / 1e3 if a.get("t_mono_ns") and b.get("t_mono_ns") else None
        cpu = ((b["proc_utime_us"] + b["proc_stime_us"]) - (a["proc_utime_us"] + a["proc_stime_us"])) / dt * 100 if dt else None
        rows.append({"t_s": b["t_s"], "cpu_pct_of_one_core": cpu, "maxrss_mb": (b.get("proc_maxrss_kb") or 0) / 1024})

    def draw(fig):
        ax = fig.subplots()
        ax.plot([r["t_s"] for r in rows], [r["cpu_pct_of_one_core"] for r in rows], color=C[0], label="receiver CPU (% of 1 core)")
        ax.set_xlabel("exporter time (s)"), ax.set_ylabel("CPU %")
        ax2 = ax.twinx()
        ax2.plot([r["t_s"] for r in rows], [r["maxrss_mb"] for r in rows], color=C[1], label="max RSS (MB)")
        ax2.set_ylabel("MB")
        fig.legend(loc="upper right")
    return rows, draw


def f_discovery(s):
    ms = (s.sum.get("cold_start") or {}).get("milestones_s")
    if not ms:
        return None
    rows = sorted(({"milestone": k, "t_s": v} for k, v in ms.items()), key=lambda r: r["t_s"])

    def draw(fig):
        ax = fig.subplots()
        ax.barh([r["milestone"] for r in rows], [r["t_s"] for r in rows], color=C[0])
        ax.invert_yaxis(), ax.set_xlabel("seconds since first exporter line (arrival time, not event time)")
    return rows, draw


def f_groundtruth(s):
    g = s.gt
    if not g:
        return None
    rows = [{"class": k, "n": v} for k, v in g["classes"].items()]
    rows += [{"class": "false_positive_dci", "n": g["fp_dci"]}]

    def draw(fig):
        ax = fig.subplots()
        ax.bar([r["class"] for r in rows], [r["n"] for r in rows], color=C[0])
        ax.tick_params(axis="x", rotation=25)
        ax.set_ylabel("count (gNB MAC-scheduled grants)")
        ax.set_title("precision %.3f recall %.3f F1 %.3f" % (g["precision"] or 0, g["recall"] or 0, g["f1"] or 0))
    return rows, draw


def f_retry(s):
    c = s.sum.get("counts")
    if not c or not (c.get("crc_fail") or c.get("rescued")):
        return None
    rows = [{"what": "first-pass CRC fail", "n": c.get("crc_fail", 0)}, {"what": "rescued by retry (unique)", "n": c.get("rescued", 0)},
            {"what": "retry lines without a seen failure", "n": c.get("retry_orphan", 0)}]

    def draw(fig):
        ax = fig.subplots()
        ax.bar([r["what"] for r in rows], [r["n"] for r in rows], color=C[1])
        ax.tick_params(axis="x", rotation=15)
    return rows, draw


FIGS = (("latency_cdf", f_latency_cdf, "latency_histograms"), ("crc_by_mcs", f_crc_by_mcs, "cell_aggregates"),
        ("grants_timeline", f_grants_timeline, "cell_aggregates"), ("cpu_rss", f_cpu_rss, "system_telemetry (metrics rows with proc_*)"),
        ("discovery_milestones", f_discovery, "summary.json cold_start"), ("groundtruth_classes", f_groundtruth, "groundtruth/summary.json (lab)"),
        ("retry_attribution", f_retry, "summary.json counts"))


def _csv(path, rows):
    keys = sorted({k for r in rows for k in r})
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        w.writerows(rows)


def draw_all(s, out, label, missing):
    synthetic = s.man.get("provenance") == "synthetic"
    os.makedirs(out, exist_ok=True)
    for name, fn, need in FIGS:
        res = fn(s)
        if res is None:
            missing.append("%s/%s: input absent (%s)" % (label, name, need))
            continue
        rows, draw = res
        _csv(os.path.join(out, name + ".csv"), rows)
        with open(os.path.join(out, name + ".json"), "w") as f:
            json.dump({"figure": name, "source": label, "mode": s.man.get("mode"), "provenance": s.man.get("provenance"),
                       "host_label": s.man.get("host_label"), "data_format": s.man.get("format"), "needs": need,
                       "matplotlib": getattr(matplotlib, "__version__", None) if plt else None}, f, indent=1)
        if plt is None:
            missing.append("%s/%s: matplotlib not importable (csv written, figure not drawn)" % (label, name))
            continue
        fig = plt.figure(figsize=(6, 3.6), constrained_layout=True)
        draw(fig)
        if synthetic:
            fig.text(0.5, 0.5, "SYNTHETIC", fontsize=46, color="gray", alpha=0.25, ha="center", va="center", rotation=25)
        for ext in ("pdf", "svg", "png"):
            fig.savefig(os.path.join(out, "%s.%s" % (name, ext)), dpi=110)
        plt.close(fig)


def sessions_figure(dirs, out, missing):
    sums = [x for x in (_j(os.path.join(d, "summary.json")) for d in dirs) if x]
    if len(sums) < 2:
        missing.append("cold_start_success: needs >= 2 --sessions (a session is the unit)")
        return
    st = cold_start_stats(sums)
    _csv(os.path.join(out, "cold_start_success.csv"), [dict(st, ci_lo=st["ci95"][0], ci_hi=st["ci95"][1], ci95=None)])
    if plt is None:
        missing.append("cold_start_success: matplotlib not importable")
        return
    fig = plt.figure(figsize=(3.6, 3.6), constrained_layout=True)
    ax = fig.subplots()
    ax.bar(["cold start"], [st["rate"]], yerr=[[st["rate"] - st["ci95"][0]], [st["ci95"][1] - st["rate"]]], capsize=4, color=C[2])
    ax.set_ylim(0, 1.05), ax.set_ylabel("success (k/n sessions, Wilson 95 %%): %d/%d" % (st["successes"], st["sessions"]))
    for ext in ("pdf", "svg", "png"):
        fig.savefig(os.path.join(out, "cold_start_success." + ext), dpi=110)
    plt.close(fig)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--lab")
    ap.add_argument("--commercial")
    ap.add_argument("--sessions", nargs="*", default=[])
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    missing = []
    for label, d in (("lab", a.lab), ("commercial", a.commercial)):
        if d:
            draw_all(Src(d), os.path.join(a.out, label), label, missing)
    os.makedirs(a.out, exist_ok=True)
    if a.sessions:
        sessions_figure(a.sessions, a.out, missing)
    with open(os.path.join(a.out, "MISSING.txt"), "w") as f:
        f.write("\n".join(missing) + ("\n" if missing else ""))
    print("plots: %d figure(s) not drawn" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
