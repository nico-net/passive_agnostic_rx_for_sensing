#!/usr/bin/env python3
"""Is the single-receiver ~50 % precision ceiling an INFORMATION limit, or unexploited features?

The claim under test (recorded in GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md 7.11) is that with one
receiver an in-band false detection is indistinguishable from a real target. That claim was inferred
from tracker-knob sweeps all plateauing near 51 %, which only proves THOSE knobs fail -- it is not a
measurement of the information actually present. This script measures it directly.

Method: label every recorded detection real/ghost against ground truth (at-epoch, so a target passing
through its own harmonic locus later in the run cannot mislabel a ghost as real -- see
eval_harmonic_pos.py's cpi_epoch), compute a per-feature ROC AUC, and report which features separate.

AUC reading: 0.5 = the feature carries NOTHING (the information-limit hypothesis); >0.6 = exploitable;
<0.4 = exploitable with the sign flipped. A Mann-Whitney U AUC is used because it is rank-based, so it
is invariant to any monotone rescaling and needs no distributional assumption.

Feature groups, chosen to cover the axes the question names:
  per-detection   snr, azimuth_std, range, |velocity|, declared range/rate sigma
  CPI-context     detections in the CPI, SNR rank and SNR relative to the CPI median, nearest
                  neighbour in range, whether some other detection in the CPI explains it as a
                  k-harmonic (the same-CPI relation harmonic_reject uses)
  temporal        persistence: how many neighbouring CPIs hold a detection at a compatible
                  (range, rate). A real target persists smoothly; a chance false alarm does not --
                  but a SCHEDULING harmonic also persists, which is exactly why M-of-N does not kill
                  it, so this feature is expected to separate real-vs-noise but NOT real-vs-harmonic.
  signal-quality  declared sigmas (SNR-derived, so partly redundant with snr -- reported to confirm
                  they add nothing independent rather than assuming it)

The ghost class is split into HARMONIC and OTHER, and AUCs are reported separately for each, because
they are different problems: if a feature separates real-vs-other but not real-vs-harmonic, then the
ceiling is specifically a harmonic-discrimination problem and the fix must target harmonics.

Usage: analyze_separability.py <run_dir> [run_dir2 ...]
"""
import json
import math
import re
import sys

import numpy as np


# ----------------------------------------------------------------- ground truth + labelling

def load_curves(ue_log):
    gt = {}
    pat = re.compile(
        r"SENSING_CHANNEL gt: t=([\d.]+)s .*?obj(\d+).*?dR=([-\d.]+)m range_rate=([-\d.]+)m/s"
    )
    with open(ue_log, errors="replace") as f:
        for line in f:
            m = pat.search(line)
            if m:
                gt.setdefault(int(m.group(2)), []).append(
                    (float(m.group(1)), float(m.group(3)), float(m.group(4)))
                )
    curves, tf = {}, None
    for oid, pts in gt.items():
        pts = sorted(set(pts))
        if len(pts) < 2:
            continue
        ts = np.array([p[0] for p in pts])
        tf = np.linspace(ts[0], ts[-1], 2000)
        curves[oid] = (
            np.interp(tf, ts, np.array([p[1] for p in pts])),
            np.interp(tf, ts, np.array([p[2] for p in pts])),
        )
    return curves, tf


def curve_times(curves, rng, vel, rtol, vtol, tf):
    out = []
    for c_r, c_v in curves.values():
        hit = (np.abs(c_r - rng) <= rtol) & (np.abs(c_v - vel) <= vtol)
        if hit.any():
            out.extend(tf[hit].tolist())
    return out


def cpi_epochs(reports, curves, rtol, vtol, tf, win):
    """Per-CPI simulated epoch, voted from that CPI's own detections (see eval_harmonic_pos.py)."""
    epochs = []
    for rep in reports:
        cand = []
        for d in rep["detections"]:
            cand.extend(curve_times(curves, d["bistatic_range_m"], d["bistatic_velocity_mps"],
                                    rtol, vtol, tf))
        if not cand:
            epochs.append(None)
            continue
        cand.sort()
        best_n, best_t, j = 0, None, 0
        for i, t0 in enumerate(cand):
            while j < len(cand) and cand[j] <= t0 + win:
                j += 1
            if j - i > best_n:
                best_n, best_t = j - i, cand[i + (j - i) // 2]
        epochs.append(best_t)
    return epochs


def label(rep_det, ep, curves, rtol, vtol, tf, max_k=4):
    """'real' | 'harmonic' | 'other', evaluated AT the CPI's own epoch."""
    rng, vel = rep_det["bistatic_range_m"], rep_det["bistatic_velocity_mps"]

    def at_epoch(v, tol):
        if ep is None:
            return False
        return any(abs(t - ep) <= 0.6 for t in curve_times(curves, rng, v, rtol, tol, tf))

    if at_epoch(vel, vtol):
        return "real"
    for k in range(2, max_k + 1):
        if at_epoch(vel / k, vtol / k):
            return "harmonic"
    return "other"


# ----------------------------------------------------------------- statistics

def auc(pos, neg):
    """Mann-Whitney U AUC: P(random positive ranks above random negative). Ties count 0.5."""
    pos, neg = np.asarray(pos, float), np.asarray(neg, float)
    pos, neg = pos[np.isfinite(pos)], neg[np.isfinite(neg)]
    if len(pos) == 0 or len(neg) == 0:
        return float("nan"), 0, 0
    allv = np.concatenate([pos, neg])
    order = allv.argsort()
    ranks = np.empty(len(allv), float)
    ranks[order] = np.arange(1, len(allv) + 1)
    # average ranks over ties
    _, inv, cnt = np.unique(allv, return_inverse=True, return_counts=True)
    sums = np.zeros(len(cnt))
    np.add.at(sums, inv, ranks)
    ranks = (sums / cnt)[inv]
    r_pos = ranks[: len(pos)].sum()
    a = (r_pos - len(pos) * (len(pos) + 1) / 2.0) / (len(pos) * len(neg))
    return a, len(pos), len(neg)


def informative(a):
    """How far from 0.5, i.e. how much a feature separates regardless of sign."""
    return abs(a - 0.5) if a == a else 0.0


# ----------------------------------------------------------------- feature extraction

def build(reports, curves, tf, rtol, vtol):
    epochs = cpi_epochs(reports, curves, rtol, vtol, tf, 0.6)
    rows = []
    for idx, rep in enumerate(reports):
        dets = rep["detections"]
        if not dets:
            continue
        snrs = np.array([d["snr_db"] for d in dets])
        med = float(np.median(snrs))
        order = (-snrs).argsort()
        rank = np.empty(len(dets), int)
        rank[order] = np.arange(len(dets))
        rres = rep.get("range_res_m") or 1.0
        vres = rep.get("vel_res_mps") or 1.0
        for i, d in enumerate(dets):
            rng, vel = d["bistatic_range_m"], d["bistatic_velocity_mps"]
            # nearest other detection in range, within this CPI
            others = [abs(rng - o["bistatic_range_m"]) for j, o in enumerate(dets) if j != i]
            nn = min(others) if others else float("nan")
            # does another detection in this CPI explain it as a k-harmonic? (same relation
            # harmonic_reject tests, expressed as a feature rather than a hard filter)
            harm_rel = 0.0
            for j, o in enumerate(dets):
                if j == i:
                    continue
                vo, vc = abs(o["bistatic_velocity_mps"]), abs(vel)
                if vo < 1e-3 or vc <= vo:
                    continue
                if abs(rng - o["bistatic_range_m"]) > 4 * rres:
                    continue
                ratio = vc / vo
                if 2 <= round(ratio) <= 4 and abs(ratio - round(ratio)) <= 0.15:
                    harm_rel = 1.0
                    break
            # temporal persistence: neighbouring CPIs holding a compatible detection
            pers = 0
            for off in (-2, -1, 1, 2):
                k = idx + off
                if 0 <= k < len(reports):
                    for o in reports[k]["detections"]:
                        if (abs(o["bistatic_range_m"] - rng) <= rtol
                                and abs(o["bistatic_velocity_mps"] - vel) <= vtol):
                            pers += 1
                            break
            rows.append({
                "label": label(d, epochs[idx], curves, rtol, vtol, tf),
                "snr_db": d["snr_db"],
                "azimuth_std_deg": d.get("azimuth_std_deg", float("nan")),
                "range_m": rng,
                "abs_vel": abs(vel),
                "vel_bins_from_zero": abs(vel) / vres,
                "range_std_m": d.get("range_std_m", float("nan")),
                "n_det_in_cpi": float(len(dets)),
                "snr_rank_in_cpi": float(rank[i]),
                "snr_minus_cpi_median": d["snr_db"] - med,
                "nn_range_gap_bins": nn / rres if nn == nn else float("nan"),
                "has_harmonic_parent": harm_rel,
                "persistence_4cpi": float(pers),
                # The receiver's OWN adaptive posterior (det_quality.h), as shipped on the wire. Note
                # this is measured on SURVIVORS only -- the gate has already dropped everything below
                # its own boundary -- so a low AUC here means "little information LEFT", not "the gate
                # is useless".
                "p_real": d.get("p_real", float("nan")),
            })
    return rows


FEATURES = ["snr_db", "azimuth_std_deg", "range_m", "abs_vel", "vel_bins_from_zero",
            "range_std_m", "n_det_in_cpi", "snr_rank_in_cpi", "snr_minus_cpi_median",
            "nn_range_gap_bins", "has_harmonic_parent", "persistence_4cpi", "p_real"]


def main():
    rows = []
    for run_dir in sys.argv[1:]:
        run_dir = run_dir.rstrip("/")
        reports = [json.loads(l) for l in open(f"{run_dir}/oaiue_reports.jsonl") if l.strip()]
        curves, tf = load_curves(f"{run_dir}/logs/ue.log")
        if not reports or not curves:
            print(f"{run_dir}: missing reports or ground truth")
            continue
        r = build(reports, curves, tf, 15.0, 3.0)
        print(f"{run_dir}: {len(reports)} CPIs, {len(r)} detections")
        rows += r
    if not rows:
        return 1

    lab = np.array([r["label"] for r in rows])
    n_real = int((lab == "real").sum())
    n_harm = int((lab == "harmonic").sum())
    n_oth = int((lab == "other").sum())
    print(f"\nlabels (at-epoch): real={n_real}  harmonic={n_harm}  other={n_oth}  total={len(rows)}")
    print(f"=> a perfect ghost rejector would take precision to 100 %; doing nothing gives "
          f"{100.0 * n_real / len(rows):.1f} %\n")

    def col(name):
        return np.array([r[name] for r in rows], float)

    print(f"{'feature':<24} {'AUC real-vs-ALL':>16} {'real-vs-HARMONIC':>18} {'real-vs-OTHER':>15}")
    print("-" * 76)
    ranked = []
    for f in FEATURES:
        v = col(f)
        a_all, np_, nn_ = auc(v[lab == "real"], v[lab != "real"])
        a_h, _, _ = auc(v[lab == "real"], v[lab == "harmonic"])
        a_o, _, _ = auc(v[lab == "real"], v[lab == "other"])
        ranked.append((informative(a_all), f, a_all, a_h, a_o))
        fmt = lambda x: f"{x:.3f}" if x == x else "  n/a"
        print(f"{f:<24} {fmt(a_all):>16} {fmt(a_h):>18} {fmt(a_o):>15}")

    ranked.sort(reverse=True)
    print("\nmost informative (|AUC - 0.5|):")
    for inf, f, a_all, a_h, a_o in ranked[:5]:
        verdict = ("carries real information" if inf >= 0.10 else
                   "marginal" if inf >= 0.05 else "essentially nothing")
        print(f"  {f:<24} |AUC-0.5| = {inf:.3f}   {verdict}")

    print("\nINTERPRETATION")
    best_h = max((informative(r[3]), r[1]) for r in ranked)
    best_o = max((informative(r[4]), r[1]) for r in ranked)
    print(f"  best real-vs-HARMONIC separator: {best_h[1]} (|AUC-0.5| = {best_h[0]:.3f})")
    print(f"  best real-vs-OTHER    separator: {best_o[1]} (|AUC-0.5| = {best_o[0]:.3f})")
    if best_h[0] < 0.05:
        print("  -> harmonics are NOT separable by any feature measured here: for THIS observation")
        print("     model the ceiling is a genuine information limit, and beating it needs a new")
        print("     observable (RVM peak shape, slow-time amplitude/phase, scheduling statistics).")
    else:
        print("  -> harmonics ARE partially separable: the ceiling is NOT purely informational and")
        print("     the feature above is exploitable by a per-detection classifier.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
