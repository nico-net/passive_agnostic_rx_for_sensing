#!/usr/bin/env python3
"""Score DGX rfsim/OTA arm dirs (rx/rx.log with a seconds prefix per line, rx/time.txt) -> JSON.

Post-convergence keys (added for the postconv regression gate; legacy keys unchanged):
  contexts[]      one entry per "Technique D CONVERGED rnti=R tda=T" line: rnti, tda, conv_s, ttc_s, trials,
                  trials_ok. ttc_s = conv time - time of the FIRST log line carrying "rnti=R" (first sighting of
                  that RNTI; contexts of one RNTI share it). trials/trials_ok come from the matching
                  "SWEEP: rnti=R CONVERGED tda=T ... (ok/total trials on the winner" line (None if absent).
  ttc_by_tda      {tda: ttc_s}, worst (max) over RNTIs
  n_contexts      len(contexts)
  postconv_*      The log does NOT attribute PDSCHQ grants to contexts, so the post-convergence window starts at the
                  first periodic cumulative "PDSCHQ queued=.. decoded=.. crc_ok=.." sample at or after the time
                  the LAST context converged (all contexts that ever converge have converged). postconv_crc_pct =
                  (final ok - sample ok) / (final decoded - sample decoded); None if no grants fall after it.
                  Samples are ~20 s apart, so up to ~20 s of post-convergence grants are excluded (conservative).
  search_crc_pct  CRC over everything up to that sample (search phase, includes <= ~20 s of post-conv grants).
  ldpc_zero_tb    zero_tb of the last LDPCDIAG line.
"""
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
             crc_pct=None, scanq_queued=None, scanq_drop_full=None, drop_full_pct=None, cpu_pct=None, max_rss_kb=None,
             contexts=[], ttc_by_tda={}, n_contexts=0, ldpc_zero_tb=None, postconv_t_s=None, postconv_decoded=None,
             postconv_crc_pct=None, search_crc_pct=None)
    try:
        with open(os.path.join(rxd, "rx.log"), errors="replace") as f:
            lines = [ANSI.sub("", l) for l in f]
    except FileNotFoundError:
        lines = []
    first_seen, convs, trials, samples = {}, [], {}, []
    for l in lines:
        t = _t(l)
        for r in re.findall(r"rnti=(0x[0-9a-fA-F]+)", l):
            if t is not None and r not in first_seen:
                first_seen[r] = t
        m = re.search(r"SWEEP: rnti=(0x[0-9a-fA-F]+) CONVERGED tda=(\d+) .*?\((\d+)/(\d+) trials", l)
        if m:
            trials[(m.group(1), int(m.group(2)))] = (int(m.group(4)), int(m.group(3)))
        m = re.search(r"Technique D CONVERGED rnti=(0x[0-9a-fA-F]+) tda=(\d+)", l)
        if m and t is not None:
            convs.append((m.group(1), int(m.group(2)), t))
        m = re.search(r"PDSCHQ queued=\d+ decoded=(\d+) crc_ok=(\d+)", l)
        if m and t is not None:
            samples.append((t, int(m.group(1)), int(m.group(2))))
        m = re.search(r"LDPCDIAG .*zero_tb=(\d+)", l)
        if m:
            s["ldpc_zero_tb"] = int(m.group(1))
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
    for r, tda, t in convs:
        tr = trials.get((r, tda), (None, None))
        s["contexts"].append(dict(rnti=r, tda=tda, conv_s=t, trials=tr[0], trials_ok=tr[1],
                                  ttc_s=round(t - first_seen[r], 3) if r in first_seen else None))
        v = s["contexts"][-1]["ttc_s"]
        if v is not None:
            s["ttc_by_tda"][str(tda)] = max(v, s["ttc_by_tda"].get(str(tda), v))
    s["n_contexts"] = len(convs)
    if convs and samples:
        t_all = max(t for _, _, t in convs)
        base = next((x for x in sorted(samples) if x[0] >= t_all), None)
        fin = max(samples)
        if base and fin[1] > base[1]:
            s["postconv_t_s"], s["postconv_decoded"] = base[0], fin[1] - base[1]
            s["postconv_crc_pct"] = round(100.0 * (fin[2] - base[2]) / (fin[1] - base[1]), 2)
        if base and base[1]:
            s["search_crc_pct"] = round(100.0 * base[2] / base[1], 2)
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

# Gate defaults, calibrated in README.txt ("Postconv gate calibration"); each overridable via the env var named.
GATE_DEFAULTS = dict(GATE_MODE="postconv", GATE_NCTX_MIN="2", GATE_POSTCONV_CRC_MIN="99.5", GATE_TTC_MAX_TDA0="12",
                     GATE_TTC_MAX_TDA2="47", GATE_CRC_FLOOR="93.0", GATE_DROP_MAX="1.0", GATE_CRC_MIN="98.0")

def evaluate_gate(s, env=os.environ):
    """-> (ok, [(name, value, op, limit, passed)]). GATE_MODE=legacy: n_converged>=1, overall CRC>=GATE_CRC_MIN,
    drop_full<=GATE_DROP_MAX (the pre-K39 criterion). GATE_MODE=postconv (default): see GATE_DEFAULTS."""
    g = lambda k: float(env.get(k, GATE_DEFAULTS[k]))
    mode = env.get("GATE_MODE", GATE_DEFAULTS["GATE_MODE"])
    ge = lambda n, v, lim: (n, v, ">=", lim, v is not None and v >= lim)
    le = lambda n, v, lim: (n, v, "<=", lim, v is not None and v <= lim)
    drop = le("drop_full", s["drop_full_pct"], g("GATE_DROP_MAX"))
    if mode == "legacy":
        c = [ge("n_converged", s["n_converged"], 1), ge("crc", s["crc_pct"], g("GATE_CRC_MIN")), drop]
    elif mode == "postconv":
        c = [ge("n_contexts", s["n_contexts"], g("GATE_NCTX_MIN")), ge("postconv_crc", s["postconv_crc_pct"], g("GATE_POSTCONV_CRC_MIN"))]
        bounds = {k[len("GATE_TTC_MAX_TDA"):]: float(v) for k, v in dict(GATE_DEFAULTS, **env).items() if k.startswith("GATE_TTC_MAX_TDA")}
        tdas = sorted(set(bounds) | set(s["ttc_by_tda"]))
        c += [le("ttc_tda" + t, s["ttc_by_tda"].get(t), bounds[t]) for t in tdas if t in bounds]
        c += [ge("crc_floor", s["crc_pct"], g("GATE_CRC_FLOOR")), drop]
    else:
        raise SystemExit("unknown GATE_MODE=%r (postconv|legacy)" % mode)
    return all(x[4] for x in c), c

def gate_line(s, env=os.environ):
    ok, c = evaluate_gate(s, env)
    def fmt(x):
        n, v, op, lim, p = x
        vs = "None" if v is None else (("%.4f" % v) if n == "drop_full" else ("%.3f" % v) if n.startswith("ttc") else ("%.2f" % v) if isinstance(v, float) else str(v))
        return "%s=%s%s%s%s" % (n, vs, op if p else {">=": "<", "<=": ">"}[op], lim, "" if p else "(FAIL)")
    return ("PASS " if ok else "FAIL ") + s["arm"] + " [" + env.get("GATE_MODE", GATE_DEFAULTS["GATE_MODE"]) + "] " + " ".join(fmt(x) for x in c), ok

if __name__ == "__main__":
    gate = "--gate" in sys.argv
    args = [a for a in sys.argv[1:] if a not in ("--json", "--gate")]
    rc = 0
    for a in args:
        s = score(a)
        if gate:
            print(json.dumps(s, sort_keys=True), file=sys.stderr)
            line, ok = gate_line(s)
            print(line); rc |= 0 if ok else 1
        else:
            print(json.dumps(s, sort_keys=True))
    sys.exit(rc)
