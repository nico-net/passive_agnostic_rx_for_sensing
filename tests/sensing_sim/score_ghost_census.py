#!/usr/bin/env python3
"""Ghost census: classify every reported detection against ground truth as real / harmonic / other.

This is step 1 of GHOST_KINEMATIC_CONSISTENCY_HANDOVER.md's validation plan, and it is deliberately
run BEFORE implementing anything: if a scene produces almost no integer-k harmonics, a harmonic
rejector is unmeasurable on it and any A/B result would be "correcting nothing changes nothing" (the
trap CLAUDE.md 9 records for the sync-injection work).

Classes, in priority order, for a detection at (dR, rate):
  real       -- lands on some object's (dR, range_rate) trajectory curve
  harmonic k -- lands at that object's dR while its rate is k x the object's true rate, k in 2..K
  mirror     -- lands at that object's dR with the NEGATED true rate (conjugate image, k = -1)
  other      -- everything else, split into in-band (a plausible target range) and far

Every harmonic is further split into:
  paired   -- its fundamental was ALSO detected in the same CPI  (same-CPI rejectors can see it)
  orphaned -- it was not                                          (only a track-level/kinematic test can)
That split is the whole point of the census: the orphaned count is the size of the gap Phase B of
the handover exists to close, and the paired count is the size of the gap Phase A closes.

Matching is TIME-ALIGNMENT-FREE, exactly as in score_run.py / score_azimuth.py: a report's
cpi_start_time_utc_ns is host wall clock while the gt lines carry simulated time, and rfsim runs tens
of times slower than real time, so the two cannot be compared directly. A detection is instead tested
against every point of each object's whole trajectory curve.

Searching a whole curve inflates the chance of an accidental match, so this script MEASURES that
inflation rather than hand-waving it: it Monte-Carlos uniformly-random points over the receiver's own
observable (range, velocity) rectangle through the identical classifier and reports the resulting
chance-match probability per class. Read every class count against its own chance line -- a harmonic
fraction at or below chance is not evidence of harmonics.

Usage: score_ghost_census.py <run_dir> [range_tol_m] [vel_tol_mps] [--max-k K] [--csv PREFIX]
"""
import json
import random
import re
import sys

import numpy as np

MAX_K_DEFAULT = 4
N_NULL = 20000  # Monte-Carlo samples for the chance-match null
# A real harmonic shares its parent's bearing exactly; this is the slack allowed for two independent
# AoA estimates of the same wavefront (measured sigma on this harness is ~2 deg, so ~5 sigma).
BEARING_CORROB_DEG = 10.0


def load_curves(ue_log):
    """{obj: (dR[], rate[])} finely resampled, from the SENSING_CHANNEL gt lines."""
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
    curves = {}
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
    return curves


class Classifier:
    """Classify a (range, velocity) detection against the ground-truth trajectory curves.

    Returns (label, obj, k):
      ("real", oid, 1) / ("harmonic", oid, k) / ("mirror", oid, -1) / ("other", None, 0)

    The velocity tolerance for a k-th harmonic is vel_tol/k, not vel_tol: a detection at k x the true
    rate, measured to +/-vel_tol, pins the FUNDAMENTAL to +/-vel_tol/k. Using the looser bound would
    manufacture harmonics out of noise at high k -- exactly the direction of error that would make
    this census say "yes, build it" when it should say no.
    """

    def __init__(self, curves, range_tol, vel_tol, max_k):
        self.curves = curves
        self.rtol = range_tol
        self.vtol = vel_tol
        self.max_k = max_k

    def _hit(self, rng, vel_fund, vtol):
        """Any object whose curve passes through (rng, vel_fund) within tolerance."""
        for oid, (c_r, c_v) in self.curves.items():
            if np.any((np.abs(c_r - rng) <= self.rtol) & (np.abs(c_v - vel_fund) <= vtol)):
                return oid
        return None

    def __call__(self, rng, vel):
        oid = self._hit(rng, vel, self.vtol)
        if oid is not None:
            return ("real", oid, 1)
        # Harmonics: the detection sits at k x the fundamental's rate, same range.
        for k in range(2, self.max_k + 1):
            oid = self._hit(rng, vel / k, self.vtol / k)
            if oid is not None:
                return ("harmonic", oid, k)
        # Conjugate image: same range, negated rate.
        oid = self._hit(rng, -vel, self.vtol)
        if oid is not None:
            return ("mirror", oid, -1)
        return ("other", None, 0)

    def range_band(self, rng):
        """True if this range is one at which SOME object actually exists (an in-band false alarm)."""
        for _oid, (c_r, _c_v) in self.curves.items():
            if np.any(np.abs(c_r - rng) <= self.rtol):
                return True
        return False


def wrap180(d):
    return (d + 180.0) % 360.0 - 180.0


def main():
    argv = sys.argv[1:]
    csv_prefix = None
    max_k = MAX_K_DEFAULT
    if "--csv" in argv:
        i = argv.index("--csv")
        csv_prefix = argv[i + 1]
        del argv[i:i + 2]
    if "--max-k" in argv:
        i = argv.index("--max-k")
        max_k = int(argv[i + 1])
        del argv[i:i + 2]

    run_dir = argv[0].rstrip("/")
    rtol = float(argv[1]) if len(argv) > 1 else 15.0
    vtol = float(argv[2]) if len(argv) > 2 else 3.0

    curves = load_curves(f"{run_dir}/logs/ue.log")
    if not curves:
        print("NO GROUND TRUTH FOUND (no SENSING_CHANNEL gt lines)")
        return 1
    reports = [json.loads(l) for l in open(f"{run_dir}/oaiue_reports.jsonl") if l.strip()]
    if not reports:
        print("no reports")
        return 1

    clf = Classifier(curves, rtol, vtol, max_k)

    n_det = 0
    counts = {"real": 0, "harmonic": 0, "mirror": 0, "other_inband": 0, "other_far": 0}
    per_k = {}
    paired = orphaned = 0
    # Phase A's premise: a paired harmonic shares its parent's BEARING as well as its range. Measured
    # here rather than assumed, because it is what decides whether a position anchor is a stronger
    # anchor than a range-bin anchor on this data.
    bearing_deltas = []
    per_cpi_rows = []

    for rep in reports:
        dets = rep["detections"]
        labs = [clf(d["bistatic_range_m"], d["bistatic_velocity_mps"]) for d in dets]
        # Which (obj) fundamentals are present in THIS CPI, for the paired/orphaned split.
        fundamentals = {}
        for d, (lab, oid, _k) in zip(dets, labs):
            if lab == "real":
                fundamentals.setdefault(oid, []).append(d)
        row = {"real": 0, "harmonic": 0, "mirror": 0, "other": 0}
        for d, (lab, oid, k) in zip(dets, labs):
            n_det += 1
            if lab == "other":
                key = "other_inband" if clf.range_band(d["bistatic_range_m"]) else "other_far"
                counts[key] += 1
                row["other"] += 1
                continue
            counts[lab] += 1
            row[lab] += 1
            if lab == "harmonic":
                per_k[k] = per_k.get(k, 0) + 1
                parents = fundamentals.get(oid, [])
                if parents:
                    paired += 1
                    az = d.get("azimuth_deg")
                    pz = parents[0].get("azimuth_deg")
                    if az is not None and pz is not None:
                        bearing_deltas.append(abs(wrap180(az - pz)))
                else:
                    orphaned += 1
        per_cpi_rows.append(row)

    # ---- Chance-match null: the SAME classifier over uniformly-random (range, velocity) points.
    rmax = max((r.get("range_max_m") or 0.0) for r in reports)
    vmax = max((r.get("vel_max_mps") or 0.0) for r in reports)
    if rmax <= 0 or vmax <= 0:
        rmax = max(d["bistatic_range_m"] for r in reports for d in r["detections"]) or 1.0
        vmax = max(abs(d["bistatic_velocity_mps"]) for r in reports for d in r["detections"]) or 1.0
    rng_gen = random.Random(12345)
    null = {"real": 0, "harmonic": 0, "mirror": 0, "other": 0}
    for _ in range(N_NULL):
        lab, _o, _k = clf(rng_gen.uniform(0.0, rmax), rng_gen.uniform(-vmax, vmax))
        null[lab if lab != "other" else "other"] += 1
    p_null = {k: v / N_NULL for k, v in null.items()}

    n_cpi = len(reports)
    pct = lambda c: 100.0 * c / max(n_det, 1)

    if csv_prefix is not None:
        print(f"{csv_prefix},{n_cpi},{n_det},{counts['real']},{counts['harmonic']},{paired},"
              f"{orphaned},{counts['mirror']},{counts['other_inband']},{counts['other_far']},"
              f"{p_null['real']:.4f},{p_null['harmonic']:.4f}")
        return 0

    print(f"=== ghost census: {run_dir}  (tol: range=+/-{rtol} m  vel=+/-{vtol} m/s  max_k={max_k}) ===")
    print(f"objects: {sorted(curves)}   CPIs: {n_cpi}   detections: {n_det} "
          f"({n_det / max(n_cpi, 1):.1f} per CPI)")
    print()
    print(f"  real            {counts['real']:5d}  ({pct(counts['real']):5.1f} %)   "
          f"chance {100 * p_null['real']:.1f} %")
    print(f"  harmonic (k>=2) {counts['harmonic']:5d}  ({pct(counts['harmonic']):5.1f} %)   "
          f"chance {100 * p_null['harmonic']:.1f} %")
    for k in sorted(per_k):
        print(f"      k={k}: {per_k[k]}")
    print(f"      paired (fundamental co-detected this CPI): {paired}   -> Phase A can reach these")
    print(f"      orphaned (fundamental absent this CPI):    {orphaned}   -> only Phase B/C can")
    print(f"  mirror (k=-1)   {counts['mirror']:5d}  ({pct(counts['mirror']):5.1f} %)   "
          f"chance {100 * p_null['mirror']:.1f} %")
    print(f"  other in-band   {counts['other_inband']:5d}  ({pct(counts['other_inband']):5.1f} %)")
    print(f"  other far       {counts['other_far']:5d}  ({pct(counts['other_far']):5.1f} %)")
    print()
    # A REAL harmonic inherits its parent's bearing exactly (the handover's §2 argument: the Doppler
    # FFT is linear and identical per antenna, so the steering coefficient passes straight through).
    # A curve-search coincidence does not. So bearing agreement is an INDEPENDENT corroboration of the
    # (dR, k*rate) label -- it uses an axis the classifier never looked at -- and splitting on it is a
    # far better estimate of the true harmonic count than the raw label.
    corroborated = sum(1 for b in bearing_deltas if b <= BEARING_CORROB_DEG)
    if bearing_deltas:
        b = sorted(bearing_deltas)
        print(f"  |bearing(ghost) - bearing(parent)| over {len(b)} paired harmonics: "
              f"median {b[len(b) // 2]:.2f} deg, p90 {b[int(0.9 * (len(b) - 1))]:.2f} deg, max {b[-1]:.2f} deg")
        print(f"    within {BEARING_CORROB_DEG:.0f} deg of the parent (a REAL harmonic must be): "
              f"{corroborated}/{len(b)}")
        print("    (a ghost inherits its parent's bearing exactly, so this is an independent check on "
              "an axis\n     the classifier never used -- and it is also what makes a POSITION anchor "
              "as tight as range+bearing)")
    else:
        print("  no paired harmonic carried both bearings -- Phase A's position anchor is untestable here")
    print()
    expected_by_chance = p_null["harmonic"] * n_det
    excess = counts["harmonic"] - expected_by_chance
    print(f"VERDICT: {counts['harmonic']} harmonic-labelled detections; {expected_by_chance:.1f} expected "
          f"by chance alone\n         (excess {excess:+.1f}), of which {corroborated} are corroborated by "
          f"the bearing test\n         = {corroborated / max(n_cpi, 1):.2f} genuine harmonics per CPI.")
    print(f"         paired {paired} (Phase A can reach) / orphaned {orphaned} (only Phase B/C can).")
    if corroborated == 0 or corroborated / max(n_cpi, 1) < 0.25:
        print("  -> too few genuine harmonics to A/B a rejector on this scene: an arm that changes")
        print("     nothing here would say nothing about the rejector. Get a scene that makes them first.")
    else:
        print("  -> a measurable harmonic population. An A/B of a same-CPI (Phase A) rejector is")
        print("     meaningful here.")
    if orphaned == 0:
        print("  -> ORPHANED count is ZERO: Phase B's specific target case does not occur on this")
        print("     capture, so this scene cannot validate it however long it runs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
