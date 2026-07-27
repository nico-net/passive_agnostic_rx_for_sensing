#!/usr/bin/env python3
"""Aggregate the sync_correction ON/OFF A/B into a verdict.

The question is narrow and worth stating precisely: on the CURRENT pipeline, does enabling the
Phase 1-4 STO/CFO/SFO correction stack help, hurt, or do nothing to per-receiver detection and
tracking? The standing "it is net-negative" claim dates from 2026-07-23 and predates most of the
current DSP, so this exists to re-establish or retire it rather than to assume it.

Usage: report_sync_ab.py results.csv
"""
import csv
import math
import sys

FIELDS = [
    ("raw_prec", "raw detection precision %", True),
    ("trk_prec", "confirmed-track precision %", True),
    ("cov0", "obj0 detection coverage %", True),
    ("cov1", "obj1 detection coverage %", True),
    ("cov2", "obj2 detection coverage %", True),
    ("cov3", "obj3 detection coverage %", True),
    ("det_cpis", "CPIs with detections", True),
    ("raw_total", "raw detections", None),
    ("trk_total", "track occurrences", None),
]


def fv(x):
    try:
        v = float(x)
        return v if math.isfinite(v) else None
    except (TypeError, ValueError):
        return None


def stats(v):
    v = [x for x in v if x is not None]
    if not v:
        return None, None, 0
    m = sum(v) / len(v)
    sd = math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1)) if len(v) > 1 else 0.0
    return m, sd, len(v)


def welch(a, b):
    a = [x for x in a if x is not None]
    b = [x for x in b if x is not None]
    if len(a) < 2 or len(b) < 2:
        return None
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((x - ma) ** 2 for x in a) / (len(a) - 1)
    vb = sum((x - mb) ** 2 for x in b) / (len(b) - 1)
    se2 = va / len(a) + vb / len(b)
    if se2 <= 0:
        return 1.0 if ma == mb else 0.0
    t = (ma - mb) / math.sqrt(se2)
    dof = se2 ** 2 / ((va / len(a)) ** 2 / (len(a) - 1) + (vb / len(b)) ** 2 / (len(b) - 1))
    x = dof / (dof + t * t)
    return max(0.0, min(1.0, _betainc(dof / 2.0, 0.5, x)))


def _betainc(a, b, x):
    if x <= 0:
        return 0.0
    if x >= 1:
        return 1.0
    lb = math.lgamma(a) + math.lgamma(b) - math.lgamma(a + b)
    front = math.exp(math.log(x) * a + math.log(1 - x) * b - lb) / a
    fv_, c, d = 1.0, 1.0, 0.0
    for i in range(200):
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
        fv_ *= c * d
        if abs(1.0 - c * d) < 1e-10:
            break
    return front * (fv_ - 1.0)


def main():
    rows = list(csv.DictReader(open(sys.argv[1])))
    if not rows:
        print("no results")
        return 1
    off = [r for r in rows if r["sync"] == "0"]
    on = [r for r in rows if r["sync"] == "1"]
    print("=" * 88)
    print("sync_correction A/B  (single receiver, 4-object scene, identical config otherwise)")
    print(f"repetitions: {len(off)} OFF / {len(on)} ON")
    print("=" * 88)
    print(f"{'metric':32s} {'sync OFF':>17s} {'sync ON':>17s} {'delta':>9s} {'p':>9s}")
    print("-" * 88)
    verdict_bits = []
    for key, label, higher_better in FIELDS:
        a = [fv(r[key]) for r in off]
        b = [fv(r[key]) for r in on]
        ma, sa, na = stats(a)
        mb, sb, nb = stats(b)
        if ma is None or mb is None:
            continue
        p = welch(a, b)
        ps = f"{p:.4f}" if p is not None else "   n/a"
        star = ""
        if p is not None and p < 0.05:
            star = "*"
            if higher_better is not None:
                better = "ON" if (mb > ma) == higher_better else "OFF"
                verdict_bits.append((label, better, mb - ma, p))
        print(f"{label:32s} {ma:9.1f}±{sa:6.1f} {mb:9.1f}±{sb:6.1f} {mb-ma:+9.1f} {ps:>8s}{star}")

    fa = stats([fv(r["fly_mean"]) for r in on])
    la = stats([fv(r["locked_mean"]) for r in on])
    print()
    print(f"sync-ON tracker internals: locked rows/CPI {la[0]:.1f}  flywheel rows/CPI {fa[0]:.1f}"
          if la[0] is not None else "sync-ON internals unavailable")
    if la[0] and fa[0] is not None and (la[0] + fa[0]) > 0:
        print(f"  => flywheel engaged on {100*fa[0]/(la[0]+fa[0]):.0f}% of tracked rows")

    print()
    print("VERDICT")
    if not verdict_bits:
        print("  No metric differs significantly (p<0.05). On this scene and this pipeline, sync")
        print("  correction is NEUTRAL -- the 2026-07-23 'net-negative' finding does not reproduce,")
        print("  so it should be retired rather than carried forward as a known defect.")
    else:
        worse = [v for v in verdict_bits if v[1] == "OFF"]
        better = [v for v in verdict_bits if v[1] == "ON"]
        if worse:
            print("  sync ON is significantly WORSE on:")
            for lab, _b, d, p in worse:
                print(f"    - {lab}: {d:+.1f} (p={p:.4f})")
        if better:
            print("  sync ON is significantly BETTER on:")
            for lab, _b, d, p in better:
                print(f"    - {lab}: {d:+.1f} (p={p:.4f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
