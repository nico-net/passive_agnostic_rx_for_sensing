#!/usr/bin/env python3
"""Full report for the AoA factorial: {2 rx, 3 rx} x {AoA on/off} x {gated/ungated}.

Emits, for every cell of the factorial:
  * fused world-track precision, world error (all + matched), confirmed-update count, distinct ids
  * PER-OBJECT track coverage -- the metric that caught the "100% precision on half the scene"
    trap: an arm that rejects everything scores perfect precision while tracking nothing
  * repetition-level spread (SD, min, max, per-rep values), because this harness's fused precision
    was measured swinging 8-58% across IDENTICAL repetitions
  * Welch t-tests for the comparisons the experiment exists to answer
  * per-array-receiver bearing accuracy, so a mirrored/misconfigured array can never be mistaken
    for a fusion result again
  * main effects of each factor, marginalised over the others

Usage: report_aoa_factorial.py <root_dir> [--json out.json]
"""
import csv
import json
import math
import os
import sys

ARMS = [
    ("2rx_noaoa_open", 2, 0, 0, "2 rx  no AoA  ungated"),
    ("2rx_noaoa_gated", 2, 0, 1, "2 rx  no AoA  GATED"),
    ("2rx_aoa_open", 2, 1, 0, "2 rx  AoA     ungated"),
    ("2rx_aoa_gated", 2, 1, 1, "2 rx  AoA     GATED"),
    ("3rx_noaoa_open", 3, 0, 0, "3 rx  no AoA  ungated"),
    ("3rx_noaoa_gated", 3, 0, 1, "3 rx  no AoA  GATED"),
    ("3rx_aoa_open", 3, 1, 0, "3 rx  AoA     ungated"),
    ("3rx_aoa_gated", 3, 1, 1, "3 rx  AoA     GATED"),
]
NOBJ = 4


def f(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def stats(v):
    v = [x for x in v if x is not None]
    if not v:
        return dict(n=0, mean=None, sd=None, min=None, max=None, median=None)
    n = len(v)
    m = sum(v) / n
    sd = math.sqrt(sum((x - m) ** 2 for x in v) / (n - 1)) if n > 1 else 0.0
    s = sorted(v)
    med = s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
    return dict(n=n, mean=m, sd=sd, min=s[0], max=s[-1], median=med)


def welch(a, b):
    """Welch's t-test. Returns (t, dof, two-sided p) or None when undefined."""
    a = [x for x in a if x is not None]
    b = [x for x in b if x is not None]
    if len(a) < 2 or len(b) < 2:
        return None
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((x - ma) ** 2 for x in a) / (len(a) - 1)
    vb = sum((x - mb) ** 2 for x in b) / (len(b) - 1)
    se2 = va / len(a) + vb / len(b)
    if se2 <= 0:
        return (float("inf") if ma != mb else 0.0), float("nan"), (0.0 if ma != mb else 1.0)
    t = (ma - mb) / math.sqrt(se2)
    dof = se2 ** 2 / ((va / len(a)) ** 2 / (len(a) - 1) + (vb / len(b)) ** 2 / (len(b) - 1))
    # Two-sided p via the incomplete beta (continued fraction), no SciPy dependency.
    x = dof / (dof + t * t)
    p = _betainc(dof / 2.0, 0.5, x)
    return t, dof, max(0.0, min(1.0, p))


def _betainc(a, b, x):
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    lbeta = math.lgamma(a) + math.lgamma(b) - math.lgamma(a + b)
    front = math.exp(math.log(x) * a + math.log(1 - x) * b - lbeta) / a
    fv, c, d = 1.0, 1.0, 0.0
    for i in range(0, 200):
        m = i // 2
        if i == 0:
            num = 1.0
        elif i % 2 == 0:
            num = (m * (b - m) * x) / ((a + 2 * m - 1) * (a + 2 * m))
        else:
            num = -((a + m) * (a + b + m) * x) / ((a + 2 * m) * (a + 2 * m + 1))
        d = 1.0 + num * d
        d = 1e-30 if abs(d) < 1e-30 else d
        d = 1.0 / d
        c = 1.0 + num / c
        c = 1e-30 if abs(c) < 1e-30 else c
        fv *= c * d
        if abs(1.0 - c * d) < 1e-10:
            break
    return front * (fv - 1.0)


def load(root):
    res, az = [], []
    rp = os.path.join(root, "results.csv")
    if os.path.exists(rp):
        res = list(csv.DictReader(open(rp)))
    ap = os.path.join(root, "azimuth.csv")
    if os.path.exists(ap):
        az = list(csv.DictReader(open(ap)))
    return res, az


def collect(res):
    """{arm: {metric: [per-rep values]}}"""
    out = {}
    for r in res:
        a = out.setdefault(r["arm"], {})
        a.setdefault("rep", []).append(int(r["rep"]))
        a.setdefault("precision", []).append(f(r["precision_pct"]))
        a.setdefault("median_err", []).append(f(r["median_err_m"]))
        a.setdefault("matched_err", []).append(f(r["matched_err_m"]))
        a.setdefault("confirmed", []).append(f(r["confirmed"]))
        a.setdefault("matched", []).append(f(r["matched"]))
        a.setdefault("ids", []).append(f(r["ids"]))
        covered = 0
        for o in range(NOBJ):
            v = f(r.get(f"obj{o}", 0)) or 0.0
            a.setdefault(f"obj{o}", []).append(v)
            covered += 1 if v > 0 else 0
        a.setdefault("objs_covered", []).append(float(covered))
    return out


def main():
    root = sys.argv[1]
    res, az = load(root)
    if not res:
        print("no results yet")
        return 1
    by = collect(res)
    reps = sorted({int(r["rep"]) for r in res})

    print("=" * 108)
    print("AoA FACTORIAL — {2 rx, 3 rx} x {AoA on/off} x {gated, ungated}")
    print("scene: 4 objects, all different + non-constant speeds, curved paths, obj0/obj1 intersect")
    print(f"repetitions: {len(reps)}   (one 3-receiver capture per rep; all 8 arms scored offline from it)")
    print("=" * 108)

    # ---- main table
    print()
    print(f"{'arm':26s} {'precision %':>17s} {'world err m':>17s} {'matched err m':>15s} "
          f"{'objs':>6s} {'conf':>6s} {'ids':>5s}")
    print("-" * 108)
    for key, _n, _a, _g, label in ARMS:
        d = by.get(key)
        if not d:
            continue
        p, e, me = stats(d["precision"]), stats(d["median_err"]), stats(d["matched_err"])
        oc, cf, idn = stats(d["objs_covered"]), stats(d["confirmed"]), stats(d["ids"])
        pm = f"{p['mean']:6.1f}±{p['sd']:5.1f}" if p["mean"] is not None else "      n/a"
        em = f"{e['mean']:7.1f}±{e['sd']:6.1f}" if e["mean"] is not None else "       n/a"
        mm = f"{me['mean']:6.2f}±{me['sd']:5.2f}" if me["mean"] is not None else "      n/a"
        print(f"{label:26s} {pm:>17s} {em:>17s} {mm:>15s} "
              f"{(oc['mean'] or 0):5.2f}/{NOBJ} {(cf['mean'] or 0):6.0f} {(idn['mean'] or 0):5.1f}")
    print()
    print("objs = mean number of the 4 objects tracked at all. THE metric that stops an arm scoring")
    print("       perfect precision by rejecting nearly everything -- read it beside precision, never alone.")

    # ---- per-object coverage
    print()
    print("PER-OBJECT track coverage (mean confirmed updates matched to each object)")
    print(f"{'arm':26s}" + "".join(f"{'obj'+str(o):>10s}" for o in range(NOBJ)) +
          f"{'reps w/ all 4':>15s}")
    print("-" * 108)
    for key, _n, _a, _g, label in ARMS:
        d = by.get(key)
        if not d:
            continue
        cells = "".join(f"{(stats(d[f'obj{o}'])['mean'] or 0):10.1f}" for o in range(NOBJ))
        allfour = sum(1 for i in range(len(d["rep"])) if all(d[f"obj{o}"][i] > 0 for o in range(NOBJ)))
        print(f"{label:26s}{cells}{allfour:>10d}/{len(d['rep']):<4d}")
    print()
    print("obj0/obj1 are the INTERSECTING pair -- if a mechanism breaks on crossing targets, it shows here.")

    # ---- significance
    print()
    print("SIGNIFICANCE (Welch t-test on per-rep precision; the comparisons this experiment exists for)")
    print("-" * 108)
    cmps = [
        ("Does AoA help at 2 rx?", "2rx_noaoa_open", "2rx_aoa_open"),
        ("Does AoA help at 3 rx?", "3rx_noaoa_open", "3rx_aoa_open"),
        ("Does the gate help at 2 rx (AoA on)?", "2rx_aoa_open", "2rx_aoa_gated"),
        ("Does the gate help at 3 rx (AoA on)?", "3rx_aoa_open", "3rx_aoa_gated"),
        ("THE CLAIM: 2 rx + AoA + gate  vs  3 rx no AoA", "3rx_noaoa_open", "2rx_aoa_gated"),
        ("Does a 3rd receiver still add anything, given AoA?", "2rx_aoa_gated", "3rx_aoa_gated"),
    ]
    for label, ka, kb in cmps:
        a = by.get(ka, {}).get("precision", [])
        b = by.get(kb, {}).get("precision", [])
        sa, sb = stats(a), stats(b)
        w = welch(a, b)
        if w is None or sa["mean"] is None or sb["mean"] is None:
            print(f"  {label:52s} insufficient data")
            continue
        t, dof, p = w
        sig = "***" if p < 0.001 else "**" if p < 0.01 else "*" if p < 0.05 else "ns"
        print(f"  {label:52s} {sa['mean']:5.1f}% vs {sb['mean']:5.1f}%   "
              f"delta {sb['mean']-sa['mean']:+6.1f}pp   p={p:.4f} {sig}")
    print("  (*** p<0.001  ** p<0.01  * p<0.05  ns = not significant)")

    # ---- main effects
    print()
    print("MAIN EFFECTS on precision (each factor marginalised over the other two)")
    print("-" * 108)
    for name, idx in (("receivers", 1), ("AoA", 2), ("gating", 3)):
        levels = {}
        for key, nrx, aoa, gated, _l in ARMS:
            lv = (nrx, aoa, gated)[idx - 1]
            levels.setdefault(lv, []).extend(by.get(key, {}).get("precision", []))
        parts = []
        for lv in sorted(levels):
            s = stats(levels[lv])
            if s["mean"] is not None:
                parts.append(f"{name}={lv}: {s['mean']:5.1f}%±{s['sd']:4.1f}")
        if len(parts) == 2:
            ks = sorted(levels)
            w = welch(levels[ks[0]], levels[ks[1]])
            pp = f"   p={w[2]:.4f}" if w else ""
            print(f"  {name:12s} " + "   ".join(parts) + pp)

    # ---- bearing quality
    if az:
        print()
        print("PER-RECEIVER BEARING ACCURACY (validity check: a mirrored array invalidates the AoA arms)")
        print(f"{'rx':6s} {'n reps':>7s} {'median |e| deg':>16s} {'p90 deg':>10s} {'max deg':>10s} "
              f"{'CRB sigma deg':>14s}")
        print("-" * 108)
        for rx in ("rx1", "rx2"):
            rows = [r for r in az if r["rx"] == rx]
            if not rows:
                continue
            med = stats([f(r["median_err"]) for r in rows])
            p90 = stats([f(r["p90_err"]) for r in rows])
            mx = stats([f(r["max_err"]) for r in rows])
            sg = stats([f(r["median_sigma"]) for r in rows])
            print(f"{rx:6s} {len(rows):7d} {(med['mean'] or 0):9.2f}±{(med['sd'] or 0):5.2f} "
                  f"{(p90['mean'] or 0):10.2f} {(mx['mean'] or 0):10.2f} {(sg['mean'] or 0):14.2f}")
        print("  A median far above a few degrees means the array is mirrored/misconfigured and every")
        print("  AoA arm in this batch is invalid -- check before reading any fusion number.")

    # ---- per-rep detail
    print()
    print("PER-REP precision (the spread is the point: this harness swings 8-58% across identical reps)")
    print("-" * 108)
    for key, _n, _a, _g, label in ARMS:
        d = by.get(key)
        if not d:
            continue
        order = sorted(range(len(d["rep"])), key=lambda i: d["rep"][i])
        vals = " ".join(f"{(d['precision'][i] or 0):3.0f}" for i in order)
        print(f"  {label:26s} {vals}")

    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
        blob = {
            "reps": len(reps),
            "arms": {
                k: {m: stats(v) for m, v in d.items() if m != "rep"} | {"per_rep": d}
                for k, d in by.items()
            },
            "azimuth": az,
        }
        json.dump(blob, open(out, "w"), indent=1, default=str)
        print(f"\nwrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
