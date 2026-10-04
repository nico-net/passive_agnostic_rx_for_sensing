#!/usr/bin/env python3
"""Score R13 run directories; gNB logs are read here only as validation truth."""
import argparse
import collections
import json
from pathlib import Path
import re
import statistics

STAMP = re.compile(r"^(\d+\.\d+) (.*)$")
EPOCH = re.compile(r"CONFIG_EPOCH \d+ -> \d+ class=(\w+) cause=(\w+)")
GNB_SIZE = re.compile(r"(?:Filling Format 1_1 DCI of size |DCI11_WIDTHS total=)(\d+)")
LEN = re.compile(r"DCI 1_1 (?:additional length|length locked).*?\blen=(\d+)|DCI length RELOCK .*?\bnew=(\d+)")
RX_KEYS = ("CONFIG_EPOCH", "DCI 1_1", "DCI length RELOCK", "BWP RESOLVED",
           "Technique D CONVERGED", "ACQ_STATE", "SIB1 decoded")
RESTART_SCENARIOS = ("same_cell_restart_size_change", "cell_restart")
GNB_KEYS = ("Filling Format 1_1 DCI of size", "DCI11_WIDTHS total=")


def lines(path, keep):
    if not path.exists():
        return []
    out = []
    with path.open(errors="replace") as source:
        for raw in source:
            if not any(key in raw for key in keep):
                continue
            m = STAMP.match(raw)
            if m:
                out.append((float(m[1]), m[2]))
    return out


def mode(vals):
    return collections.Counter(vals).most_common(1)[0][0] if vals else None


def read_jsonl(path):
    if not path.exists():
        return []
    out = []
    for s in path.read_text(errors="replace").splitlines():
        if s.strip():
            try:
                out.append(json.loads(s))
            except json.JSONDecodeError:
                pass  # a truncated final record must not abort scoring other runs
    return out


def first_after(rows, start, pat):
    return next((t for t, s in rows if t >= start and re.search(pat, s)), None)


def score_run(root):
    root = Path(root)
    ev = read_jsonl(root / "events.jsonl")
    start = next((x for x in ev if x.get("event") == "start"), {})
    apply_event = next((x for x in ev if x.get("event") == "apply"), None)
    if not start or not apply_event:
        return dict(host=start.get("host", "unknown"), scenario=start.get("scenario", "unknown"), sib=start.get("sib", "unknown"),
                    reconf=start.get("reconf", "unknown"), status="INCOMPLETE", recovery_s=None, recovery_origin=None, recovery_from_apply_s=None, target_s=None,
                    epoch_count=0, false_soft_per_h=None, false_hard=None, stale_dci_winners=0,
                    td_truth_audited=False, gnb_dci_11_bits_before=None, gnb_dci_11_bits_after=None,
                    dropped_epoch={}, compute_path={}, crnti_grants_per_s=None,
                    errors=["scenario did not reach apply"])
    ready = next((x["t"] for x in ev if x["event"] == "ready"), start["t"])
    apply = apply_event["t"]
    applied = next((x["t"] for x in ev if x["event"] == "applied"), apply)
    end = next((x["t"] for x in ev if x["event"] == "end"), None)
    scenario, sib, reconf = start["scenario"], start["sib"], start["reconf"]
    # Preserve scoring of earlier tooling output while naming its actual semantics.
    if scenario == "dedicated_change":
        scenario = "same_cell_restart_size_change"
    rx = lines(root / "rx/rx.log", RX_KEYS)
    scored_rx = [(t, s) for t, s in rx if end is None or t <= end]
    before = lines(root / "gnb/before.log", GNB_KEYS)
    after = lines(root / "gnb/after.log", GNB_KEYS)
    errors = []
    campaign_meta = root / "run.json"
    if campaign_meta.exists():
        try:
            run_meta = json.loads(campaign_meta.read_text())
            if (run_meta.get("rc") != 0 or run_meta.get("timed_out") or run_meta.get("escalation_stage", 0)
                    or run_meta.get("status") != "done"):
                errors.append("campaign command failed, timed out or escalated past SIGINT")
        except json.JSONDecodeError:
            errors.append("campaign run.json is malformed")
    if end is None:
        errors.append("run did not reach end (receiver may have exited)")
    if not rx:
        errors.append("no timestamped receiver log")
    pre_sizes = [int(GNB_SIZE.search(s)[1]) for t, s in before if t < apply and GNB_SIZE.search(s)]
    post_log = after if scenario in RESTART_SCENARIOS else before
    timed_sizes = [(t, int(GNB_SIZE.search(s)[1])) for t, s in post_log
                   if t >= apply and (end is None or t <= end) and GNB_SIZE.search(s)]
    post_sizes = [n for _, n in timed_sizes]
    truth_pre, truth_post = mode(pre_sizes), mode(post_sizes)
    if truth_pre is None:
        errors.append("no pre-change gNB DCI 1_1 size at MAC debug level")
    validation_start = (timed_sizes[0][0] if timed_sizes and scenario in RESTART_SCENARIOS
                        else apply)
    if scenario != "stable" and truth_post is None:
        errors.append("no post-change gNB DCI 1_1 size at MAC debug level")
    if scenario == "stable" and truth_post is not None and truth_post != truth_pre:
        errors.append("stable-cell gNB DCI 1_1 size changed")
    if scenario == "same_cell_restart_size_change" and truth_pre is not None and truth_post == truth_pre:
        errors.append("dedicated variant did not change the gNB DCI 1_1 size")

    window = 60 if scenario in RESTART_SCENARIOS else 30
    window_end = apply - 2 if scenario == "stable" else max(apply, applied) + window
    epochs = []
    for t, s in scored_rx:
        m = EPOCH.search(s)
        if m:
            epochs.append(dict(t=t, class_=m[1], cause=m[2], in_window=apply - 2 <= t <= window_end))
    false_soft = sum(e["class_"] == "SOFT" and not e["in_window"] and e["t"] >= ready for e in epochs)
    false_hard = sum(e["class_"].startswith("HARD") and not e["in_window"] and e["t"] >= ready for e in epochs)
    expected_class = {"bwp_switch": "SOFT", "cell_restart": "HARD_RESET"}.get(scenario)
    # This arm includes RF continuity loss; it does not cover a live dedicated-config update.
    if reconf == "off":
        epoch_ok = not epochs
    elif scenario == "stable":
        epoch_ok = True  # the false-bump rate below is the stable-cell gate
    elif scenario == "same_cell_restart_size_change":
        epoch_ok = (any(e["class_"] == "HARD_REVERIFY" and e["cause"] == "CONTINUITY_LOSS"
                        and e["in_window"] for e in epochs)
                    and all(e["class_"] != "HARD_RESET" for e in epochs))
    elif scenario == "bwp_switch":
        epoch_ok = (any(e["class_"] == "SOFT" and e["cause"] == "BWP_CHANGE" and e["in_window"] for e in epochs)
                    and all(not e["class_"].startswith("HARD") for e in epochs))
    else:
        epoch_ok = any(e["class_"] == expected_class and e["cause"] == "CELL_IDENTITY_CHANGE"
                       and e["in_window"] for e in epochs)

    recovery_origin = "validation_start" if scenario in RESTART_SCENARIOS else "apply"
    recovery_origin_t = validation_start if scenario in RESTART_SCENARIOS and timed_sizes else (
        None if scenario in RESTART_SCENARIOS else apply)
    # A recovery requires fresh post-event evidence, never a pre-event lock.
    if scenario == "stable":
        recovery = 0.0
        milestones = {}
    else:
        length_pat = r"DCI length RELOCK|DCI 1_1 (?:additional length|length locked)"
        patterns = {"bwp_switch": [r"BWP RESOLVED", length_pat,
                                   r"Technique D CONVERGED"],
                    "same_cell_restart_size_change": [length_pat, r"Technique D CONVERGED"],
                    "cell_restart": [r"ACQ_STATE .* -> (?:PDCCH_LOCKED|CORESET_VERIFIED|CELL_CONFIGURED|DL_CONVERGED|UL_CONVERGED|TRACKING)",
                                     length_pat,
                                     r"Technique D CONVERGED"]}[scenario]
        milestones = {p: first_after(scored_rx, apply if p.startswith("ACQ_STATE") else validation_start, p)
                      for p in patterns}
        prior = [t for p, t in milestones.items() if p != r"Technique D CONVERGED"]
        if prior and all(t is not None for t in prior):
            milestones[r"Technique D CONVERGED"] = first_after(scored_rx, max(prior), r"Technique D CONVERGED")
        recovery = round(max(milestones.values()) - recovery_origin_t, 3) if recovery_origin_t is not None and all(v is not None for v in milestones.values()) else None
        if recovery is None:
            errors.append("missing fresh recovery milestone")

    recovery_from_apply = (round(max(milestones.values()) - apply, 3) if milestones
                           and all(v is not None for v in milestones.values()) else (0.0 if scenario == "stable" else None))

    # Ground-truth comparison is intentionally limited to DCI 1_1 widths emitted by the gNB.
    # A separate operator audit is needed for TDRA/DMRS winner semantics.
    winners = []
    for t, s in scored_rx:
        if t < validation_start:
            continue
        m = LEN.search(s)
        if m:
            n = int(m[1] or m[2])
            fallback = truth_pre if scenario == "bwp_switch" else truth_post
            expected = next((size for ts, size in reversed(timed_sizes) if ts <= t), fallback)
            winners.append(dict(t=t, dci_11_bits=n, gnb_bits=expected,
                                matches_gnb=(n == expected) if expected else None))
    stale_dci = sum(w["matches_gnb"] is False for w in winners)
    audit_path = root / "td_truth_audit.json"
    try:
        audit = json.loads(audit_path.read_text()) if audit_path.exists() else None
    except json.JSONDecodeError:
        audit = None
    td_audited = bool(isinstance(audit, dict) and audit.get("source") == "gnb-log" and audit.get("checked") is True
                      and type(audit.get("stale_winners")) is int and audit["stale_winners"] >= 0)
    stale_total = stale_dci + audit["stale_winners"] if td_audited else None

    metrics = read_jsonl(root / "metrics.jsonl")
    dropped = {}
    for key in ("scanq_drop_epoch", "pdcch_inline_drop_epoch", "pdschq_drop_epoch", "puschq_drop_epoch"):
        dropped[key] = max(0, int(metrics[-1].get(key, 0)) - int(metrics[0].get(key, 0))) if len(metrics) >= 2 else None
    backends = collections.Counter()
    if metrics:
        b = metrics[-1].get("td_cb0_backend", {})
        for k in ("cpu", "gpu"):
            backends[k] = int(b.get(k, 0))
    if not metrics:
        errors.append("missing ISAC_METRICS")
    if sib == "sa" and not any(t < apply and "SIB1 decoded" in s for t, s in rx):
        errors.append("SA arm has no pre-apply SIB1 decode")
    uectx = read_jsonl(root / "uectx.jsonl") if reconf == "on" else []
    snaps = [x for x in uectx if x.get("type") == "ue_snapshot"]
    sib1_values = [x.get("cfg", {}).get("SIB1_HASH") for x in snaps]
    if reconf == "on" and not snaps:
        errors.append("no UeContext snapshots")
    if reconf == "on" and sib == "sa" and not any(x is not None for x in sib1_values):
        errors.append("SA UeContext never recorded SIB1 evidence")
    if reconf == "on" and sib == "sib1less" and any(x is not None for x in sib1_values):
        errors.append("SIB1-less UeContext contains SIB1 evidence")
    pci_before = metrics[0].get("pci") if metrics else None
    pci_after = metrics[-1].get("pci") if metrics else None
    if scenario == "cell_restart" and pci_after != 1:
        errors.append("receiver did not report new PCI 1")
    compute_effective = "gpu" if backends["gpu"] else "cpu" if backends["cpu"] else "unknown"
    if compute_effective == "unknown":
        errors.append("CB0 compute backend unmeasured")
    if int(start.get("gpu", 0)) and compute_effective != "gpu":
        errors.append("GPU requested but CB0 backend did not run on GPU")
    grant_rate = None
    metrics_elapsed = None
    if len(metrics) >= 2:
        elapsed = (int(metrics[-1].get("t_mono_ns", 0)) - int(metrics[0].get("t_mono_ns", 0))) / 1e9
        if elapsed > 0:
            metrics_elapsed = elapsed
            grant_rate = round((int(metrics[-1].get("pdcch_accepts_c", 0))
                                - int(metrics[0].get("pdcch_accepts_c", 0))) / elapsed, 2)
    if grant_rate is None or grant_rate < 100:
        errors.append("C-RNTI grants/s below 100 or unmeasured")
    if end is not None and (metrics_elapsed is None or metrics_elapsed + 10 < end - ready):
        errors.append("metrics do not cover the scored observation window")

    observed_h = max((end or apply) - ready, 1) / 3600
    soft_rate = false_soft / observed_h
    if scenario == "stable" and observed_h < 0.25:
        errors.append("stable-cell window shorter than 15 minutes")
    target_s = 30 if scenario in RESTART_SCENARIOS else 10
    target_ok = scenario == "stable" or (recovery is not None and recovery <= target_s)
    if not td_audited:
        errors.append("TD winner comparison against gNB log needs td_truth_audit.json")
    if not winners and scenario != "stable":
        errors.append("no post-change receiver DCI 1_1 length winner")
    if not epoch_ok:
        errors.append("expected epoch class absent or unexpected epoch in control/stable arm")
    if not target_ok:
        errors.append(f"recovery exceeds {target_s}s or is absent")
    if stale_total:
        errors.append("stale winner found")
    if soft_rate > 1 or false_hard:
        errors.append("false epoch target failed")
    result = dict(host=start.get("host", "unknown"), scenario=scenario, sib=sib, reconf=reconf, apply_t=apply,
                  ground_truth_start_t=validation_start,
                  scenario_window_end_t=window_end,
                  recovery_s=recovery, recovery_origin=recovery_origin, recovery_origin_t=recovery_origin_t,
                  recovery_from_apply_s=recovery_from_apply, target_s=target_s, target_ok=target_ok, milestones=milestones,
                  epochs=epochs, epoch_count=len(epochs), epoch_ok=epoch_ok,
                  false_soft=false_soft, false_soft_per_h=round(soft_rate, 3), false_hard=false_hard,
                  dropped_epoch=dropped, gnb_dci_11_bits_before=truth_pre, gnb_dci_11_bits_after=truth_post,
                  receiver_pci_before=pci_before, receiver_pci_after=pci_after,
                  receiver_dci_winners=winners, stale_dci_winners=stale_dci,
                  stale_winners=stale_total, td_truth_audited=td_audited,
                  crnti_grants_per_s=grant_rate,
                  metrics_elapsed_s=metrics_elapsed,
                  compute_path=dict(td_cb0_backend=dict(backends), gpu_requested=bool(int(start.get("gpu", 0))),
                                    effective=compute_effective),
                  errors=errors)
    result["status"] = "PASS" if not errors and reconf == "on" else ("CONTROL" if not errors else "INCOMPLETE")
    return result


def write_run(root):
    r = score_run(root)
    root = Path(root)
    (root / "score_r13.json").write_text(json.dumps(r, indent=2) + "\n")
    human = (f"[{r['host']} SA rfsim] {r['scenario']}/{r['sib']}/ISAC_RECONF={r['reconf']}: {r['status']}\n"
             f"recovery={r['recovery_s']}s origin={r['recovery_origin']} apply_elapsed={r['recovery_from_apply_s']}s target={r['target_s']}s; epochs={r['epoch_count']} "
             f"false SOFT/h={r['false_soft_per_h']} false HARD={r['false_hard']}; "
             f"stale DCI={r['stale_dci_winners']} TD audited={r['td_truth_audited']}\n"
             f"gNB DCI 1_1 bits {r['gnb_dci_11_bits_before']} -> {r['gnb_dci_11_bits_after']}; "
             f"C-RNTI grants/s={r['crnti_grants_per_s']}; "
             f"dropped_epoch={r['dropped_epoch']}; compute={r['compute_path']}\n"
             f"issues: {', '.join(r['errors']) or 'none'}\n")
    (root / "score_r13.txt").write_text(human)
    print(human, end="")
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", type=Path, help="run directory or campaign root")
    a = ap.parse_args()
    dirs = sorted((a.path / "runs").iterdir()) if (a.path / "runs").is_dir() else [a.path]
    rows = [write_run(d) for d in dirs if (d / "events.jsonl").exists()]
    if not rows:
        ap.error("no R13 run directories with events.jsonl")
    if (a.path / "runs").is_dir():
        groups = collections.defaultdict(list)
        for r in rows:
            groups[(r["host"], r["scenario"], r["sib"], r["reconf"])].append(r)
        by_arm = {"/".join(k): dict(runs=len(v), pass_count=sum(x["status"] == "PASS" for x in v),
                                   control_count=sum(x["status"] == "CONTROL" for x in v),
                                   median_recovery_s=statistics.median(x["recovery_s"] for x in v if x["recovery_s"] is not None)
                                   if any(x["recovery_s"] is not None for x in v) else None,
                                   r13_pass=len(v) >= 5 and all(x["status"] == "PASS" for x in v))
                  for k, v in groups.items()}
        required = [("sens6", scenario, sib, flag)
                    for scenario in ("stable", "bwp_switch", "same_cell_restart_size_change", "cell_restart")
                    for sib in ("sa", "sib1less") for flag in ("on", "off")]
        missing = ["/".join(k) for k in required if k not in groups or len(groups[k]) < 5]
        enabled_ok = all(by_arm.get("/".join(k), {}).get("r13_pass", False)
                         for k in required if k[-1] == "on")
        summary = dict(host="sens6", r13_pass=not missing and enabled_ok,
                       missing_arms=missing, groups=by_arm)
        (a.path / "r13_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
