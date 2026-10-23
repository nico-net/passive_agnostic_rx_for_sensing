#!/usr/bin/env python3
"""PROPOSED solver-based rfsim gate (not yet the gate; rfsim_regress.sh and score_rx.py are untouched).
Since 0fb22ecbd0 the always-on PDSCH solver handles every C-RNTI DCI 1_1, so `Technique D CONVERGED` lines (n_contexts, ttc_tda*,
reopens, postconv window) no longer exist. This scorer takes them from the solver's own log lines and keeps crc_floor and drop_full.
  n_solved        RNTIs with `solve=` in the last `PDSCH_SOLVER per-rnti` line               >= GATE_NSOLVED_MIN (1: the bed has one C-RNTI)
  solve_s         worst time from the RNTI's first grant to its solve (signal time, s)      <= GATE_SOLVE_MAX    (PROVISIONAL 10)
  post_solve_crc  full TB CRC ok / full decodes of the solved RNTIs (solver counters)        >= GATE_POSTSOLVE_CRC_MIN (99.8)
  post_solve_tr   full decodes after the solve                                               >= GATE_POSTSOLVE_MIN (5000)
  crc_floor, drop_full  unchanged (overall PDSCHQ CRC >= 93.4, scan-queue drops <= 2.5 %)
usage: score_rx_solver.py [--json] [--gate] <arm dir>..."""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import score_rx
DEF = dict(GATE_NSOLVED_MIN="1", GATE_SOLVE_MAX="10", GATE_POSTSOLVE_CRC_MIN="99.8", GATE_POSTSOLVE_MIN="5000",
           GATE_CRC_FLOOR="93.4", GATE_DROP_MAX="2.5")
ENT = re.compile(r"0x([0-9a-f]{4}):g(\d+),(solve|unsolved_age)=([\d.]+)ms,full(\d+)/(\d+)")

def score(arm):
    s = score_rx.score(arm)
    rxd = os.path.join(arm, "rx") if os.path.isdir(os.path.join(arm, "rx")) else arm
    last = None
    try:
        with open(os.path.join(rxd, "rx.log"), errors="replace") as f:
            for l in f:
                if "PDSCH_SOLVER per-rnti" in l:
                    last = score_rx.ANSI.sub("", l)
    except FileNotFoundError:
        pass
    rn = []
    for m in ENT.finditer(last or ""):
        rn.append(dict(rnti="0x" + m.group(1), grants=int(m.group(2)), solved=m.group(3) == "solve",
                       t_ms=float(m.group(4)), full_ok=int(m.group(5)), full_tr=int(m.group(6))))
    sv = [r for r in rn if r["solved"]]
    s["solver_rntis"] = rn
    s["n_solved"] = len(sv)
    s["solve_s"] = round(max(r["t_ms"] for r in sv) / 1000.0, 3) if sv else None
    tr = sum(r["full_tr"] for r in sv)
    s["post_solve_tr"] = tr
    s["post_solve_crc_pct"] = round(100.0 * sum(r["full_ok"] for r in sv) / tr, 2) if tr else None
    return s

def gate(s, env=os.environ):
    g = lambda k: float(env.get(k, DEF[k]))
    ge = lambda n, v, lim: (n, v, ">=", lim, v is not None and v >= lim)
    le = lambda n, v, lim: (n, v, "<=", lim, v is not None and v <= lim)
    c = [ge("n_solved", s["n_solved"], g("GATE_NSOLVED_MIN")), le("solve_s", s["solve_s"], g("GATE_SOLVE_MAX")),
         ge("post_solve_crc", s["post_solve_crc_pct"], g("GATE_POSTSOLVE_CRC_MIN")), ge("post_solve_tr", s["post_solve_tr"], g("GATE_POSTSOLVE_MIN")),
         ge("crc_floor", s["crc_pct"], g("GATE_CRC_FLOOR")), le("drop_full", s["drop_full_pct"], g("GATE_DROP_MAX"))]
    ok = all(x[4] for x in c)
    f = lambda x: "%s=%s%s%s%s" % (x[0], "None" if x[1] is None else x[1], x[2] if x[4] else {">=": "<", "<=": ">"}[x[2]], x[3], "" if x[4] else "(FAIL)")
    return ("PASS " if ok else "FAIL ") + s["arm"] + " [solver] " + " ".join(f(x) for x in c), ok

if __name__ == "__main__":
    do_gate = "--gate" in sys.argv
    rc = 0
    for a in [a for a in sys.argv[1:] if a not in ("--json", "--gate")]:
        s = score(a)
        if do_gate:
            line, ok = gate(s); print(line); rc |= 0 if ok else 1
        else:
            print(json.dumps(s, sort_keys=True))
    sys.exit(rc)
