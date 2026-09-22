#!/usr/bin/env python3
"""Stage 2 of extended-object detection: per-receiver temporal memory over CLEAN components.

Consumes the native replay report JSONL (which already carries every CLEAN component per
receiver per CPI) and produces one object-level measurement per receiver per CPI.

Ground truth is NOT read here. Scoring is a separate step.

Every parameter is a declared system quantity -- nothing is fitted to any capture:

  N_LAG                  fixed-lag window, declared latency budget (10 CPIs = 0.75 s)
  range_res / sqrt(12)   measurement floor, from the declared range resolution
  maximum_target_speed   cross-CPI family association bound, declared surveillance limit
  GATE_SIGMA             2 sigma, a conventional statistical quantile declared up front

Stage 1 (family tagging by range) already exists in clean_detector.cc; this module consumes
its output (bulk_component_iteration) and adds what stage 2 requires:
  * temporal memory (the fixed-lag buffer)
  * a coherent bulk rate, from the identity d(range)/dt = range rate
  * an honest range sigma, from the window's own fit residual floored at the resolution cell
"""
from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass, field

N_LAG = 10          # declared latency budget, in CPIs
import os as _os_s8
SPLIT_RATE = _os_s8.environ.get("STAGE8_SPLIT_RATE", "0") == "1"   # Rule 3: Doppler-resolved co-range objects are separate families
GATE_SIGMA = 2.0    # conventional 2-sigma acceptance quantile
UL_SESSION = None   # None = primary session block; int = pusch_session_id in uplink_sessions
LEG = "dl"          # 'dl' (gNB illuminator) or 'ul' (UE illuminator; DTD/DFS relative to the direct path)


@dataclass
class Family:
    """One range family within a single receiver/CPI: a bulk plus its micro-Doppler members."""
    range_m: float
    members: list = field(default_factory=list)   # each: (rate_mps, score, is_anchor)
    min_res: float = 0.0                          # finest rate resolution among members (long dwells), 0 = CPI default
    split: bool = False                           # secondary rate cluster of a co-range family (must corroborate its own range evolution)

    @property
    def n(self) -> int:
        return len(self.members)

    @property
    def rate_spread(self) -> float:
        if self.n < 2:
            return 0.0
        rates = [m[0] for m in self.members]
        return max(rates) - min(rates)


@dataclass
class ObjectMeasurement:
    """One physical object as seen by one receiver in one CPI."""
    time_s: float
    receiver: int
    range_m: float
    range_sigma_m: float
    rate_mps: float
    rate_source: str          # "fine" (a corroborated component) or "coarse" (range evolution)
    rate_spread_mps: float    # micro-Doppler extent, retained rather than discarded
    component_count: int
    window_samples: int


import os as _os
# (2026-09-22) Nested long dwells (STAGE8_MULTI_DWELL=1): the engine reports 150/300 ms CLEAN sets per
# receiver at the closing CPI (report['spatial_receivers'][i]['long_dwells']). Their components are added
# to the families of the CPI at the window midpoint; families are then split into rate clusters (gap >
# one CPI rate cell) so two co-range objects with distinct rates become two objects; the static rule
# uses the finest dwell's notch for families that carry a long-dwell member.
MULTI_DWELL = _os.environ.get("STAGE8_MULTI_DWELL", "0") == "1"
# (2026-09-22) UL accumulated dwells (STAGE8_UL_DWELL=1): the engine's UL accumulation gate
# (NR_ISAC_UL_DWELL=1) reports one ~0.3 s / 90-120-row CLEAN set per PUSCH session and receiver
# (uplink_sessions[k].receivers[i]['long_dwells']). Their components are injected into the families
# of the CPI at the window midpoint exactly like the DL nested dwells. Reason this is needed on UL
# and not only on DL: a 75 ms UL CPI holds 14-31 rows, so the UE direct path leaks into every
# Doppler bin over range bins 0-3 and every UL component is rejected as a skirt (measured: 0 UL
# objects in 1272 receiver-CPIs; with the gate, person recall 91% within 20 m of the UE).
UL_DWELL = _os.environ.get("STAGE8_UL_DWELL", "0") == "1"
# (2026-09-22) LONG-DWELL AUTHORITY (STAGE8_DWELL_AUTHORITY=1). Measured defect: a long dwell
# resolves two co-range objects in Doppler (300 ms cell 0.28 m/s vs 1.14 m/s at 75 ms, so a 1-bin
# separation becomes ~4 bins), but its components are injected into the midpoint CPI and then pass
# through the SAME co-range merge and the SAME 75 ms rate-cell split, which puts the pair back
# together (measured: two cars in one range bin in 59% of CPIs; bike above threshold in 99% of CPIs
# and emitted in 3%). When a family carries long-dwell members, the finest dwell present is the
# authority: it sets the rate-cluster gap AND blocks the co-range merge of two families whose rates
# it can separate. No new constant -- the gap is that dwell's own declared rate cell.
DWELL_AUTHORITY = _os.environ.get("STAGE8_DWELL_AUTHORITY", "0") == "1"
# (2026-09-20) Physical Doppler support for components (declared, no tuning): the bistatic range
# rate of any point on a real object is bounded by 2 x the declared maximum object speed
# (|d/dt(|p-tx|+|p-rx|)| <= 2|v|).  Detections beyond that bound cannot be targets; measured at
# the 33 m set: false CLEAN detections are spread uniformly over 0-100 m/s (structured residue of
# strong components after CLEAN), true ones sit below 6 m/s.  STAGE8_SPEED_BOUND=0 disables.
SPEED_BOUND = _os.environ.get("STAGE8_SPEED_BOUND", "1") == "1"
MAX_SPEED_MPS = 50.0     # set from --maximum-target-speed-mps in main()


def group_families(detections, range_res_m, rate_res_mps):
    """Stage 1 output, reconstructed from the report.

    clean_detector.cc tags co-range components with bulk_component_iteration = anchor.iteration
    and leaves the anchor at -1. The report preserves both, so families regroup exactly.
    """
    if SPEED_BOUND:
        detections = [d for d in detections if abs(d["bistatic_velocity_mps"]) <= 2.0 * MAX_SPEED_MPS]
    anchors = {}
    for d in detections:
        if d.get("bulk_component_iteration", -1) < 0:
            anchors[d.get("source_component_iteration", id(d))] = Family(
                range_m=d["bistatic_range_m"],
                members=[(d["bistatic_velocity_mps"], d.get("score", 0.0), True)],
            )
    orphans = []
    for d in detections:
        tag = d.get("bulk_component_iteration", -1)
        if tag < 0:
            continue
        fam = anchors.get(tag)
        if fam is None:
            orphans.append(d)
            continue
        fam.members.append((d["bistatic_velocity_mps"], d.get("score", 0.0), False))
    # A tagged component whose anchor is absent, or an untagged stray, still has to go somewhere:
    # attach to the nearest family within one resolution cell, else stand alone.
    for d in orphans:
        best, best_dr = None, None
        for fam in anchors.values():
            dr = abs(d["bistatic_range_m"] - fam.range_m)
            if dr <= range_res_m and (best_dr is None or dr < best_dr):
                best, best_dr = fam, dr
        if best is not None:
            best.members.append((d["bistatic_velocity_mps"], d.get("score", 0.0), False))
        else:
            anchors[("orphan", id(d))] = Family(
                range_m=d["bistatic_range_m"],
                members=[(d["bistatic_velocity_mps"], d.get("score", 0.0), True)],
            )
    families = list(anchors.values())
    # Rule 1 is applied BEFORE rule 2 so that a static-residual family can never absorb a
    # moving family lying within one range cell of it (measured: runner rx1 at ~3 m excess
    # range lost 14% of its blocks to the bin-0 residual when merged first).
    # Static = inside the zero-Doppler bin, whose extent is +-half a rate cell.  A full cell
    # (first version) discarded targets whose true rate was 1.2-2 m/s but measured just under
    # 1.14 (runner rx1: 14% of blocks); half a cell is the bin's physical footprint.
    return _merge_and_split(families, range_res_m, rate_res_mps)


def _merge_and_split(families, range_res_m, rate_res_mps):
    families = [f for f in families if any(abs(m[0]) >= 0.5 * (f.min_res or rate_res_mps) for m in f.members)]
    # (2026-09-19) Rule 2: merge families whose anchor ranges lie within one range cell -- the
    # same physical object split by the bulk tagging (e.g. a rotor line anchored separately
    # from the body).  One resolution cell is the declared criterion stage 7 already applies
    # to components.  Measured before: 0-0.25 duplicate same-range blocks per CPI.
    families.sort(key=lambda f: -max(m[1] for m in f.members))     # strongest anchor first
    merged = []
    for fam in families:
        def compatible(h):
            if abs(h.range_m - fam.range_m) > range_res_m:
                return False
            if not DWELL_AUTHORITY:
                return True
            # finest rate cell available between the two families; if their rates are separated by
            # more than that cell, the finer dwell has already resolved them and they must not merge.
            res = min(x for x in (h.min_res or rate_res_mps, fam.min_res or rate_res_mps))
            gap = min(abs(a[0] - b[0]) for a in h.members for b in fam.members)
            return gap <= res
        host = next((h for h in merged if compatible(h)), None)
        if host is None:
            if DWELL_AUTHORITY:
                # The merge may have been blocked by the rate test rather than by range. The family
                # that survives that block is a SECONDARY cluster of a co-range object, exactly like
                # a rate-cluster split, so it must prove its own range evolution in emit() before it
                # counts as an object (without this, the extent components of one car -- which span
                # several long-dwell rate cells -- became independent primary objects: measured
                # four_classes car1 49.0% -> 41.4%, extra fragments on persons3_bike person1).
                blocked = [h for h in merged if abs(h.range_m - fam.range_m) <= range_res_m]
                if blocked and max(m[1] for m in fam.members) < max(max(m[1] for m in h.members) for h in blocked):
                    fam.split = True
            merged.append(fam)
        else:
            host.members.extend((m[0], m[1], False) for m in fam.members)
            host.min_res = min(x for x in (host.min_res, fam.min_res) if x) if (host.min_res or fam.min_res) else 0.0
    if MULTI_DWELL or UL_DWELL:
        # rate-cluster split: members of one co-range family whose rates are separated by more than one
        # CPI rate cell are distinct objects (two cars in adjacent lanes: 1.9 m/s apart at the same range)
        split = []
        for fam in merged:
            ms = sorted(fam.members, key=lambda m: m[0]); groups = [[ms[0]]]
            gap_res = (fam.min_res or rate_res_mps) if DWELL_AUTHORITY else rate_res_mps
            for m in ms[1:]:
                if m[0] - groups[-1][-1][0] > gap_res: groups.append([m])
                else: groups[-1].append(m)
            if len(groups) == 1: split.append(fam); continue
            strongest = max(range(len(groups)), key=lambda gi: max(m[1] for m in groups[gi]))
            for gi, grp in enumerate(groups):
                anchor = max(grp, key=lambda m: m[1])
                split.append(Family(range_m=fam.range_m, members=[(m[0], m[1], m is anchor) for m in grp], min_res=fam.min_res, split=(gi != strongest)))
        merged = split
    # (2026-09-21) Rule 3 (STAGE8_SPLIT_RATE=1): two objects inside one range cell but resolved in
    # Doppler are two objects. The CLEAN bulk tagging and Rule 2 group co-range components regardless
    # of rate, which merged a drone (|rate| ~1.7 m/s) into a co-range car family and kept the car's
    # rate (measured: 307 drone detections -> 88 stage-8 measurements, 14% with the drone's rate).
    # Moving members (|rate| >= 1 cell) are clustered by rate with a gap of 2 cells (the declared
    # resolution, twice); each extra cluster becomes its own family at the same range. Members inside
    # the zero-Doppler cell (static leftovers, extent components) stay with the strongest cluster.
    if SPLIT_RATE:
        split = []
        for fam in merged:
            moving = sorted((m for m in fam.members if abs(m[0]) >= rate_res_mps), key=lambda m: m[0])
            static = [m for m in fam.members if abs(m[0]) < rate_res_mps]
            clusters = []
            for m in moving:
                if clusters and m[0] - clusters[-1][-1][0] <= 2.0 * rate_res_mps:
                    clusters[-1].append(m)
                else:
                    clusters.append([m])
            if len(clusters) <= 1:
                split.append(fam); continue
            clusters.sort(key=lambda c: -max(x[1] for x in c))     # strongest cluster keeps the static members
            for j, c in enumerate(clusters):
                members = [(x[0], x[1], k == 0) for k, x in enumerate(sorted(c, key=lambda x: -x[1]))] + (static if j == 0 else [])
                split.append(Family(range_m=fam.range_m, members=members))
        merged = split
    # (2026-09-19) Rule 1: a family whose every member has |rate| below one rate cell is
    # indistinguishable from the static residual for a moving-target detector; it carries no
    # target information and is not emitted.  Physical rule, no constant: the rate cell is the
    # declared resolution.  Measured before: 0.2-0.7 zero-Doppler blocks per CPI.
    return merged


def weighted_line_fit(times, ranges, sigma):
    """OLS slope/intercept plus the window's own residual scatter. sigma is the shared floor."""
    n = len(times)
    tbar = sum(times) / n
    rbar = sum(ranges) / n
    stt = sum((t - tbar) ** 2 for t in times)
    if stt <= 0.0:
        return rbar, 0.0, 0.0, float("inf")
    slope = sum((t - tbar) * (r - rbar) for t, r in zip(times, ranges)) / stt
    intercept = rbar - slope * tbar
    resid = [r - (intercept + slope * t) for t, r in zip(times, ranges)]
    dof = max(n - 2, 1)
    resid_sigma = math.sqrt(sum(x * x for x in resid) / dof)
    slope_sigma = sigma / math.sqrt(stt)
    return intercept, slope, resid_sigma, slope_sigma


def chain_families(window, max_step_m):
    """Follow one family forward through the window.

    Association bound is physical: between consecutive CPIs the bistatic range cannot move
    further than maximum_target_speed * dt. Not a tuned gate.
    """
    if not window or not window[0][1]:
        return []
    chains = []
    for seed in window[0][1]:
        chain = [(window[0][0], seed)]
        current = seed
        for t, families in window[1:]:
            if not families:
                continue
            cand = min(families, key=lambda f: abs(f.range_m - current.range_m))
            if abs(cand.range_m - current.range_m) <= max_step_m:
                chain.append((t, cand))
                current = cand
        chains.append(chain)
    return chains


def emit(window, receiver, range_res_m, rate_res_mps, max_speed_mps):
    """Produce object measurements for the OLDEST CPI in the window (fixed-lag emission)."""
    floor = range_res_m / math.sqrt(12.0)
    dt_bound = max_speed_mps * 0.075           # declared CPI cadence
    out = []
    for chain in chain_families(window, dt_bound):
        if len(chain) < 2:
            t0, fam0 = chain[0]
            if getattr(fam0, "split", False):
                continue   # a secondary rate cluster needs its own range evolution to count as an object
            out.append(ObjectMeasurement(
                time_s=t0, receiver=receiver, range_m=fam0.range_m, range_sigma_m=floor,
                rate_mps=fam0.members[0][0], rate_source="coarse",
                rate_spread_mps=fam0.rate_spread, component_count=fam0.n, window_samples=1))
            continue
        times = [t for t, _ in chain]
        ranges = [f.range_m for _, f in chain]
        _, slope, resid_sigma, slope_sigma = weighted_line_fit(times, ranges, floor)

        # Honest range sigma: the window's own scatter, floored at the resolution cell.
        # Mildly optimistic (measured median z ~1.1-1.8); deliberately NOT corrected by a
        # factor fitted to any capture. The floor covers the degenerate case where the object
        # sits inside one range cell for the whole window and the residual collapses.
        range_sigma = max(resid_sigma, floor)

        t0, fam0 = chain[0]
        # Two-tier bulk rate: prefer a component that corroborates the object's own range
        # evolution; fall back to the evolution itself rather than naming a sideband as bulk.
        # (2026-09-19 fix) The block rate is ALWAYS a measured member Doppler.  The window's
        # range slope (a derivative of 3 m cells over 0.75 s, +-2 m/s) is used only to choose
        # WHICH member is the bulk (body vs sideband); it is never emitted as the rate itself.
        # Measured before the fix: 9-29% of target blocks carried the slope ('coarse') with
        # ~+-2 m/s error while the object's own Doppler was within one cell.
        agree = [m for m in fam0.members if abs(m[0] - slope) <= GATE_SIGMA * slope_sigma]
        if agree:
            rate = min(agree, key=lambda m: abs(m[0] - slope))[0]
            source = "fine"
        else:
            if getattr(fam0, "split", False):
                continue   # secondary cluster whose Doppler does not match its range evolution: a sideband, not an object
            rate = min(fam0.members, key=lambda m: abs(m[0] - slope))[0]
            source = "nearest"
        out.append(ObjectMeasurement(
            time_s=t0, receiver=receiver, range_m=fam0.range_m, range_sigma_m=range_sigma,
            rate_mps=rate, rate_source=source, rate_spread_mps=fam0.rate_spread,
            component_count=fam0.n, window_samples=len(chain)))
    return out


class Stage8Stream:
    """Streaming stage 8 (2026-09-21): feed one report per CPI, get the measurements emitted for the
    CPI that just left the fixed-lag window (latency N_LAG CPIs). Same body as the offline loop."""
    def __init__(self, max_speed_mps, leg=None, ul_session=None):
        self.max_speed = max_speed_mps; self.buffers = {}
        self.leg = LEG if leg is None else leg; self.ul_session = UL_SESSION if ul_session is None else ul_session

    def feed(self, rec):
        objects = []
        t = rec["midpoint_air_time_s"]
        for idx, batch in enumerate(rec.get("spatial_receivers") or []):
            rr = batch.get("range_res_m") or 0.0
            vr = batch.get("vel_res_mps") or 0.0
            if rr <= 0.0 or vr <= 0.0:
                continue
            if self.leg == "ul":
                if self.ul_session is None:
                    ul = batch.get("uplink_dtd_dfs") or {}
                else:   # explicit PUSCH session: uplink_sessions[k].receivers[idx] (no mobility prior)
                    sess = [x for x in (rec.get("uplink_sessions") or []) if x.get("pusch_session_id") == self.ul_session]
                    ul = (sess[0]["receivers"][idx] if sess and idx < len(sess[0]["receivers"]) else {}) or {}
                if not ul.get("valid"):
                    continue
                dets = [dict(d, bistatic_range_m=d["delta_path_range_m"],
                             bistatic_velocity_mps=d["delta_path_range_rate_mps"])
                        for d in (ul.get("detections") or [])]
                rr = ul.get("range_res_m") or rr; vr = ul.get("vel_res_mps") or vr
            else:
                dets = batch.get("detections") or []
            fams = group_families(dets, rr, vr)
            buf = self.buffers.setdefault(idx, [])
            buf.append((t, fams))
            long_dwell_sets = []
            if MULTI_DWELL and self.leg != "ul":
                long_dwell_sets = batch.get("long_dwells") or []
            elif UL_DWELL and self.leg == "ul":
                # UL detections carry delta_path_* names; normalise to the common keys first.
                for L in ul.get("long_dwells") or []:
                    long_dwell_sets.append(dict(L, detections=[
                        dict(d, bistatic_range_m=d["delta_path_range_m"],
                             bistatic_velocity_mps=d["delta_path_range_rate_mps"])
                        for d in (L.get("detections") or [])]))
            if long_dwell_sets:
                for L in long_dwell_sets:
                    n_cpi = max(1, int(round(L["dwell_s"] / 0.075)))
                    target = buf[-(n_cpi // 2 + 1)] if len(buf) >= n_cpi // 2 + 1 else buf[-1]
                    vr_long = 299792458.0 / (3.49944e9 * L["dwell_s"]) if L["dwell_s"] > 0 else vr
                    extra = [Family(range_m=d["bistatic_range_m"], members=[(d["bistatic_velocity_mps"], d.get("score", 0.0), True)], min_res=vr_long)
                             for d in L.get("detections") or []]
                    if extra:
                        # re-run the co-range merge + rate-cluster split with the long-dwell components added
                        merged_dets = [{"bistatic_range_m": f.range_m, "bistatic_velocity_mps": m[0], "score": m[1], "bulk_component_iteration": -1, "source_component_iteration": k}
                                       for k, f in enumerate(target[1]) for m in f.members]
                        base = [Family(range_m=f.range_m, members=list(f.members), min_res=f.min_res) for f in target[1]] + extra
                        target_fams = _merge_and_split(base, rr, vr)
                        buf[buf.index(target)] = (target[0], target_fams)
            if len(buf) > N_LAG + 1:
                buf.pop(0)
            if len(buf) == N_LAG + 1:           # fixed-lag: emit oldest once window is full
                objects.extend(emit(buf, idx, rr, vr, self.max_speed))
        return objects


def run(report_path, max_speed_mps):
    reports = [json.loads(line) for line in open(report_path) if line.strip()]
    stream = Stage8Stream(max_speed_mps)
    objects = []
    for rec in reports:
        objects.extend(stream.feed(rec))
    return objects


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True)
    ap.add_argument("--maximum-target-speed-mps", type=float, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--leg", choices=("dl", "ul"), default="dl")
    ap.add_argument("--ul-session", type=int, default=None, help="pusch_session_id to read from uplink_sessions")
    args = ap.parse_args()
    global LEG, UL_SESSION, MAX_SPEED_MPS
    LEG = args.leg; UL_SESSION = args.ul_session; MAX_SPEED_MPS = float(args.maximum_target_speed_mps)
    objects = run(args.report, args.maximum_target_speed_mps)
    with open(args.out, "w") as stream:
        for o in objects:
            stream.write(json.dumps(dict(o.__dict__, leg=LEG, ul_session=UL_SESSION)) + "\n")
    fine = sum(1 for o in objects if o.rate_source == "fine")
    print(f"{len(objects)} object measurements  ({fine} fine / {len(objects)-fine} coarse)")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    raise SystemExit(main())
