#!/usr/bin/env python3
"""Score DGX rfsim/OTA arm dirs (rx/rx.log with a seconds prefix per line, rx/time.txt) -> JSON."""
import json, os, re, sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")

def _t(line):
    try:
        return float(line.split(" ", 1)[0])
    except ValueError:
        return None

def score(arm):
    rxd = os.path.join(arm, "rx") if os.path.isdir(os.path.join(arm, "rx")) else arm
    s = dict(arm=os.path.basename(os.path.normpath(arm)), sync_s=None, first_crnti_s=None, conv_s=None, ttc_s=None,
             n_converged=0, bank_len=None, ldpc_ok=None, ldpc_seg_fail=None, pdsch_decoded=None, pdsch_crc_ok=None,
             crc_pct=None, scanq_queued=None, scanq_drop_full=None, drop_full_pct=None, cpu_pct=None, max_rss_kb=None)
    try:
        with open(os.path.join(rxd, "rx.log"), errors="replace") as f:
            lines = [ANSI.sub("", l) for l in f]
    except FileNotFoundError:
        lines = []
    for l in lines:
        if s["sync_s"] is None and "Initial sync successful" in l:
            s["sync_s"] = _t(l)
        if s["first_crnti_s"] is None and "rnti=0x1234" in l:
            s["first_crnti_s"] = _t(l)
        if "Technique D CONVERGED" in l:
            s["n_converged"] += 1
            if s["conv_s"] is None:
                s["conv_s"] = _t(l)
        m = re.search(r"bank add .*len=(\d+)", l)
        if m and s["bank_len"] is None:
            s["bank_len"] = int(m.group(1))
        m = re.search(r"LDPCDIAG ok=(\d+) seg_fail=(\d+)", l)
        if m:
            s["ldpc_ok"], s["ldpc_seg_fail"] = int(m.group(1)), int(m.group(2))
        m = re.search(r"PDSCHQ queued=\d+ decoded=(\d+) crc_ok=(\d+)", l)
        if m:
            s["pdsch_decoded"], s["pdsch_crc_ok"] = int(m.group(1)), int(m.group(2))
        m = re.search(r"scanq\[queued=(\d+) done=\d+ drop_full=(\d+)", l)
        if m:
            s["scanq_queued"], s["scanq_drop_full"] = int(m.group(1)), int(m.group(2))
    if s["first_crnti_s"] is not None and s["conv_s"] is not None:
        s["ttc_s"] = round(s["conv_s"] - s["first_crnti_s"], 3)
    if s["pdsch_decoded"]:
        s["crc_pct"] = round(100.0 * s["pdsch_crc_ok"] / s["pdsch_decoded"], 2)
    if s["scanq_queued"]:
        s["drop_full_pct"] = round(100.0 * s["scanq_drop_full"] / s["scanq_queued"], 4)
    try:
        with open(os.path.join(rxd, "time.txt")) as f:
            for l in f:
                if "Percent of CPU" in l:
                    s["cpu_pct"] = int(l.rsplit(":", 1)[1].strip().rstrip("%") or 0)
                if "Maximum resident" in l:
                    s["max_rss_kb"] = int(l.rsplit(":", 1)[1])
    except FileNotFoundError:
        pass
    return s

if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--json"]
    for a in args:
        print(json.dumps(score(a), sort_keys=True))
