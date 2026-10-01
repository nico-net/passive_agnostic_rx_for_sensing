#!/usr/bin/env python3
"""Campaign runner: manifest + per-run capture + verdict + summary. Stops children with SIGINT (never SIGKILL first).

Env knobs (defaults in parens): CAMPAIGN_GRACE_S (30) = seconds past --secs before SIGINT is sent;
CAMPAIGN_TERM_GRACE_S (30) = seconds after SIGINT before escalating to SIGTERM, and again before a last-resort SIGKILL.
"""
import argparse, datetime, json, os, platform, signal, socket, subprocess, sys, threading, time
HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
from verdict import verdict  # noqa: E402

def utcnow():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f") + "Z"

def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception as e:  # noqa: BLE001 - manifest must never fail because a probe failed
        return "error: %s" % e

def cmd_new(a):
    d = os.path.join(a.root, datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d") + "_" + a.name)
    os.makedirs(os.path.join(d, "runs"), exist_ok=False)
    m = dict(created_utc=utcnow(), site=a.site, cell=a.cell, notes=a.notes,
             hostname=socket.gethostname(), arch=platform.machine(), kernel=platform.release(),
             git_commit=sh("git -C %s rev-parse HEAD" % REPO), git_branch=sh("git -C %s rev-parse --abbrev-ref HEAD" % REPO),
             git_dirty=bool(sh("git -C %s status --porcelain --untracked-files=no" % REPO)),
             sens6_frozen_ok=subprocess.run(["git", "-C", REPO, "diff", "--quiet", "sens6-frozen-2026-09-30", "--",
                                             "tests/passive_rx/captures", "tests/passive_rx/*.conf",
                                             "tests/passive_rx/sens6_host_snapshot_2026-09-30"]).returncode == 0,
             uhd_version=sh("uhd_config_info --version 2>/dev/null | head -1"), lscpu=sh("lscpu -e"),
             nvidia=sh("nvidia-smi --query-gpu=name,driver_version,compute_cap --format=csv,noheader 2>/dev/null"),
             cmdline=sh("cat /proc/cmdline"), ulimit_r=sh("bash -c 'ulimit -r'"))
    json.dump(m, open(os.path.join(d, "manifest.json"), "w"), indent=2)
    print(d)

def _nic_sampler(nic, path, stop):
    with open(path, "w") as f:
        f.write("epoch,rx_missed_errors,rx_packets\n")
        while not stop.is_set():
            try:
                miss = open("/sys/class/net/%s/statistics/rx_missed_errors" % nic).read().strip()
                pk = open("/sys/class/net/%s/statistics/rx_packets" % nic).read().strip()
                f.write("%d,%s,%s\n" % (time.time(), miss, pk)); f.flush()
            except OSError:
                pass
            stop.wait(1.0)

def _write_json_atomic(path, obj):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f); f.flush(); os.fsync(f.fileno())
    os.replace(tmp, path)

def _killpg(p, sig):
    try:
        os.killpg(p.pid, sig)
    except (ProcessLookupError, PermissionError):
        pass

def _reader(p, log, t0):
    for line in p.stdout:
        log.write("%.3f %s" % (time.time() - t0, line) if not line[:1].isdigit() else line); log.flush()

def cmd_run(a):
    grace = float(os.environ.get("CAMPAIGN_GRACE_S", "30")); term_grace = float(os.environ.get("CAMPAIGN_TERM_GRACE_S", "30"))
    runs = os.path.join(a.campaign, "runs"); n = len(os.listdir(runs)) + 1
    rd = os.path.join(runs, "%03d_%s" % (n, a.arm)); os.makedirs(rd)
    env = dict(os.environ, ISAC_METRICS_PATH=os.path.join(rd, "metrics.jsonl"), ISAC_OBS_PATH=os.path.join(rd, "obs.jsonl"))
    open(os.path.join(rd, "cmd.txt"), "w").write(" ".join(a.command) + "\n")
    open(os.path.join(rd, "env.txt"), "w").write("\n".join("%s=%s" % kv for kv in sorted(env.items()) if kv[0].startswith(("ISAC_", "NR_", "LDPC"))) + "\n")
    rj = dict(arm=a.arm, secs=a.secs, start_utc=utcnow(), status="running", expect_sib1=a.expect_sib1)
    _write_json_atomic(os.path.join(rd, "run.json"), rj)
    stop = threading.Event()
    if a.nic:
        threading.Thread(target=_nic_sampler, args=(a.nic, os.path.join(rd, "nic.csv"), stop), daemon=True).start()
    interrupted = threading.Event()
    # handlers only set a flag; the main loop below does the signalling (no work in signal context)
    for sg in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sg, lambda *_: interrupted.set())
    t0 = time.time(); timed_out = False
    with open(os.path.join(rd, "rx.log"), "w") as log:
        p = subprocess.Popen(a.command, cwd=rd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             start_new_session=True)
        rt = threading.Thread(target=_reader, args=(p, log, t0), daemon=True); rt.start()
        deadline = t0 + a.secs + grace
        stage, stage_t = 0, None   # 0 running; 1 SIGINT sent; 2 SIGTERM sent; 3 SIGKILL (last resort)
        while p.poll() is None:
            now = time.time()
            if stage == 0 and (interrupted.is_set() or now > deadline):
                timed_out = not interrupted.is_set()
                _killpg(p, signal.SIGINT); stage, stage_t = 1, now
            elif stage in (1, 2) and now - stage_t > term_grace:
                _killpg(p, signal.SIGTERM if stage == 1 else signal.SIGKILL); stage, stage_t = stage + 1, now
            time.sleep(0.1)
        rt.join(timeout=5)   # a grandchild may hold the pipe open; do not hang on it
    stop.set()
    rj.update(end_utc=utcnow(), rc=p.returncode, timed_out=timed_out, escalation_stage=stage,
              status="interrupted" if interrupted.is_set() else "done", wall_s=round(time.time() - t0, 1))
    _write_json_atomic(os.path.join(rd, "run.json"), rj)
    v = verdict(rd); json.dump(v, open(os.path.join(rd, "verdict.json"), "w"), indent=2)
    with open(os.path.join(a.campaign, "index.jsonl"), "a") as f:
        f.write(json.dumps(dict(run=os.path.basename(rd), arm=a.arm, verdict=v["verdict"], score=v["score"])) + "\n")
    print(rd, v["verdict"])

def cmd_summarize(a):
    ip = os.path.join(a.campaign, "index.jsonl")
    runs = os.path.join(a.campaign, "runs")
    has_runs = os.path.isdir(runs) and any(os.path.isdir(os.path.join(runs, n)) for n in os.listdir(runs))
    if not os.path.exists(ip) and not has_runs:
        sys.exit("summarize: %s not found and no run dirs (nothing to summarize)" % ip)
    rows = []
    if os.path.exists(ip):
        with open(ip) as f:
            rows = [json.loads(l) for l in f if l.strip()]
    seen = {r["run"] for r in rows}
    for name in sorted(os.listdir(runs)) if os.path.isdir(runs) else []:
        if name in seen or not os.path.isdir(os.path.join(runs, name)):
            continue
        v = verdict(os.path.join(runs, name))   # run dir without an index line: runner died before finishing
        v["verdict"] = "INTERRUPTED"
        rows.append(dict(run=name, arm=name.split("_", 1)[-1], verdict="INTERRUPTED", score=v["score"]))
    by = {}
    for r in rows:
        by.setdefault(r["arm"], []).append(r)
    summ = {arm: dict(n=len(rs), valid=sum(r["verdict"] == "VALID" for r in rs),
                      crc_pct=[r["score"]["crc_pct"] for r in rs], ttc_s=[r["score"]["ttc_s"] for r in rs],
                      drop_full_pct=[r["score"]["drop_full_pct"] for r in rs], verdicts=[r["verdict"] for r in rs])
            for arm, rs in by.items()}
    json.dump(summ, open(os.path.join(a.campaign, "summary.json"), "w"), indent=2)
    with open(os.path.join(a.campaign, "summary.md"), "w") as f:
        f.write("| arm | runs | VALID | CRC % | ttc s | drop_full % | verdicts |\n|---|---|---|---|---|---|---|\n")
        for arm, s in summ.items():
            f.write("| %s | %d | %d | %s | %s | %s | %s |\n" % (arm, s["n"], s["valid"], s["crc_pct"], s["ttc_s"], s["drop_full_pct"], s["verdicts"]))
    print(os.path.join(a.campaign, "summary.md"))

def main():
    ap = argparse.ArgumentParser(); sub = ap.add_subparsers(dest="c", required=True)
    n = sub.add_parser("new"); n.add_argument("--name", required=True); n.add_argument("--site", required=True)
    n.add_argument("--cell", required=True); n.add_argument("--notes", default="")
    n.add_argument("--root", default="/home/nicola/NICOLA/campaigns"); n.set_defaults(f=cmd_new)
    r = sub.add_parser("run"); r.add_argument("campaign"); r.add_argument("--arm", required=True)
    r.add_argument("--secs", type=int, required=True); r.add_argument("--nic"); r.add_argument("--expect-sib1", action="store_true")
    r.set_defaults(f=cmd_run)
    s = sub.add_parser("summarize"); s.add_argument("campaign"); s.set_defaults(f=cmd_summarize)
    argv = sys.argv[1:]
    cmd = []
    if "--" in argv:   # everything after the first "--" is the child command (argparse REMAINDER would swallow our options)
        i = argv.index("--"); argv, cmd = argv[:i], argv[i + 1:]
    a = ap.parse_args(argv)
    if a.c == "run":
        if not cmd:
            ap.error("run: command required after --")
        a.command = cmd
    a.f(a)

if __name__ == "__main__":
    main()
