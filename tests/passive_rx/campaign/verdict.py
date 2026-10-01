#!/usr/bin/env python3
"""Machine-readable verdict of one campaign run dir (receiver-oriented; sensing CPIs are not required).

Layout: the runner captures the wrapper command's stdout as <run>/rx.log. If the command is dgx/rfsim_arm.sh
(`rfsim_arm.sh ./arm N`), the real receiver log is <run>/arm/rx/rx.log; arm_dir() prefers such a sub-dir
(<run>/*/rx/rx.log, or <run>/*/rx.log) over the wrapper's stdout. Otherwise the run dir itself is scored.
"""
import json, os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dgx"))
from score_rx import score  # noqa: E402

FAULT = re.compile(r"RFSTALL|RFTSDISC|RXDISCONT|rx xport timed out|No CHDR|device_init failed|Assertion")

def arm_dir(run_dir):
    """Directory whose rx/rx.log (or rx.log) is the receiver log: an arm sub-dir if present, else run_dir."""
    try:
        subs = sorted(e for e in os.listdir(run_dir) if os.path.isdir(os.path.join(run_dir, e)))
    except FileNotFoundError:
        return run_dir
    for e in subs:
        p = os.path.join(run_dir, e)
        if os.path.exists(os.path.join(p, "rx", "rx.log")) or os.path.exists(os.path.join(p, "rx.log")):
            return p
    return run_dir

def _rx_log_path(d):
    p = os.path.join(d, "rx", "rx.log")
    return p if os.path.isdir(os.path.join(d, "rx")) else os.path.join(d, "rx.log")

def _last_json(path):
    last = None
    try:
        for l in open(path, errors="replace"):
            l = l.strip()
            if l.startswith("{"):
                try:
                    last = json.loads(l)
                except json.JSONDecodeError:
                    continue
    except FileNotFoundError:
        pass
    return last

def _nic_loss(run_dir):
    try:
        rows = [l.strip().split(",") for l in open(os.path.join(run_dir, "nic.csv")) if l[:1].isdigit()]
        return int(rows[-1][1]) - int(rows[0][1]) if len(rows) >= 2 else 0
    except (FileNotFoundError, IndexError, ValueError):
        return 0

def verdict(run_dir):
    ad = arm_dir(run_dir)
    s = score(ad)
    reasons, v = [], "VALID"
    try:
        with open(os.path.join(run_dir, "run.json")) as f:
            rj = json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        rj = {}
    lp = _rx_log_path(ad)
    log = ""
    if os.path.exists(lp):
        with open(lp, errors="replace") as f:
            log = f.read()
    if rj.get("status") == "interrupted":
        v = "INTERRUPTED"; reasons.append("runner interrupted")
    elif FAULT.search(log):
        v = "VOID_RFSTALL"; reasons.append(FAULT.search(log).group(0))
    elif _nic_loss(run_dir) > 0:
        v = "VOID_NIC_LOSS"; reasons.append("nic rx_missed_errors delta=%d" % _nic_loss(run_dir))
    elif s["sync_s"] is None:
        v = "VOID_NO_SYNC"; reasons.append("no 'Initial sync successful'")
    elif rj.get("expect_sib1") and "SIB1 decoded" not in log:
        v = "VOID_NO_SIB1"; reasons.append("expect_sib1 set but no SIB1")
    return {"verdict": v, "reasons": reasons, "score": s, "scored_dir": os.path.relpath(ad, run_dir),
            "last_metrics": _last_json(os.path.join(run_dir, "metrics.jsonl"))}

if __name__ == "__main__":
    for d in sys.argv[1:]:
        print(json.dumps(verdict(d), sort_keys=True))
