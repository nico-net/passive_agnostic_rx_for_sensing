#!/usr/bin/env python3
"""Score one reconf_bed run: python3 -I score_reconf.py <out_dir> [--lenient]

Inputs: <out_dir>/run.log (passive receiver, every line prefixed with a Unix wall stamp by stamp.py) and
<out_dir>/events.txt (wall-stamped bed events written by run_reconf.sh). gNB logs are NOT read; the expected
cell parameters come from the scenario line of events.txt and are validation truth only.
Prints PASS / FAIL / WARN / INFO per criterion; exit 1 if any FAIL. --lenient turns FAIL into WARN.
Thresholds are PROVISIONAL (no live-reconfiguration measurement exists yet, PROJECT_MEMORY section 25); see README.md.
"""
import json
import re
import sys
from pathlib import Path

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
STAMP = re.compile(r"^(\d+\.\d+) (.*)$")
EPOCH = re.compile(r"CONFIG_EPOCH (\d+) -> (\d+) class=(\w+) cause=(\w+)")
EVM = re.compile(r"EVMGRANT rnti=0x([0-9a-fA-F]+) \S+ crc=(\d+) mcs=(\d+).*? Nl=(\d+).*?\bprobe=(-?\d+)")
METRICS = re.compile(r"ISAC_METRICS (\{.*\})")
LOCK = r"DCI 1_1 length locked|DCI length RELOCK"

# Provisional thresholds (seconds unless noted). Basis: audit 2026-10-30 [MEASURED, OTA]: RX discontinuity
# recovery p95 1.1 s / max 1.3 s with state kept; a new cell needs SSB sync + MIB + SIB1 (160 ms period) + CORESET
# verification + solver re-solve, budgeted at 3x the 15 s S0 limit; S3 only needs the solver to re-lock.
T = dict(
    settle=25.0,          # ignore epochs in the first seconds after receiver start (initial acquisition)
    min_pre_crc=200,      # CRC-ok grants in the 30 s before the change (baseline decode alive)
    reconnect=30.0,       # old-server exit + gNB B start delay + this = max time to "reconnected" line
    drain=1.0,            # decodes this long after the old server exit are still in-flight work
    recover={"S0": 15.0, "S1": 45.0, "S2": 45.0, "S2c": 45.0, "S3": 30.0},
    epoch_wait=30.0,      # gNB B ready -> expected epoch
    lock_wait=45.0,       # epoch -> DCI length lock
    sustain_n=50,         # CRC-ok grants in the 20 s after first recovery
    post_ok_frac=0.90,    # CRC-ok / (CRC-ok + CRC-fail) over the last 30 s of the run
    soft_false_per_min=5.0,
    param_frac=0.80,      # fraction of post-recovery CRC-ok grants showing the new Nl / MCS (S3)
)


def parse_log(text):
    out = []
    for raw in text.splitlines():
        m = STAMP.match(raw)
        if m:
            out.append((float(m[1]), ANSI.sub("", m[2])))
    return out


def parse_events(text):
    ev, meta = {}, {}
    for raw in text.splitlines():
        p = raw.split()
        if len(p) < 2:
            continue
        t, label = float(p[0]), p[1]
        if label == "scenario":
            meta["scenario"] = p[2]
            for kv in p[3:]:
                k, _, v = kv.partition("=")
                meta[k] = v
        else:
            ev.setdefault(label, t)
    return ev, meta


def first(rows, t0, pat, t1=float("inf")):
    return next((t for t, s in rows if t0 <= t <= t1 and re.search(pat, s)), None)


def metrics_records(rows):
    out = []
    for t, s in rows:
        m = METRICS.search(s)
        if m:
            try:
                out.append((t, json.loads(m[1])))
            except ValueError:
                pass
    return out


def score(rows, ev, meta, lenient=False):
    res = []

    def add(name, ok, detail, kind="FAIL"):
        res.append((name, "PASS" if ok else ("WARN" if lenient and kind == "FAIL" else kind), detail))

    def info(name, detail):
        res.append((name, "INFO", detail))

    sc = meta.get("scenario", "?")
    miss = [k for k in ("rx_start", "gnbA_stop_sent", "gnbA_exited", "gnbB_start") if k not in ev]
    if sc not in T["recover"] or miss:
        add("bed_complete", False, f"scenario={sc} missing events {miss}")
        return res
    t_rx, t_stop, t_exit, t_bstart = ev["rx_start"], ev["gnbA_stop_sent"], ev["gnbA_exited"], ev["gnbB_start"]
    t_ready = ev.get("gnbB_ready", t_bstart)
    t_end = ev.get("end", rows[-1][0] if rows else t_ready)
    add("bed_complete", "gnbB_ready" in ev, "gNB B produced reconfig.raw" if "gnbB_ready" in ev else "gNB B never ready")

    epochs = []
    for t, s in rows:
        m = EPOCH.search(s)
        if m:
            epochs.append(dict(t=t, cls=m[3], cause=m[4]))
    grants = []
    for t, s in rows:
        m = EVM.search(s)
        if m:
            grants.append(dict(t=t, crc=int(m[2]), mcs=int(m[3]), nl=int(m[4]), probe=int(m[5])))
    grants = [g for g in grants if g["probe"] == 0]  # probe grants are not decodes of a scheduled TB
    ok_g = [g for g in grants if g["crc"] == 1]

    # --- baseline: alive and decoding before the change ---
    pre = [g for g in ok_g if t_stop - 30 <= g["t"] < t_stop]
    add("baseline_decode", len(pre) >= T["min_pre_crc"], f"{len(pre)} CRC-ok grants in 30 s before change (>= {T['min_pre_crc']})")

    # --- false epochs before the change (static cell) ---
    fe = [e for e in epochs if t_rx + T["settle"] <= e["t"] < t_stop]
    fh = [e for e in fe if e["cls"].startswith("HARD")]
    fs = [e for e in fe if e["cls"] == "SOFT"]
    span = max(t_stop - t_rx - T["settle"], 1.0)
    add("no_false_hard_before_change", not fh,
        f"{len(fh)} HARD epochs in {span:.0f} s on a static cell " + (str([(e["cls"], e["cause"]) for e in fh[:3]]) if fh else ""))
    rate = len(fs) * 60.0 / span
    add("false_soft_rate", rate <= T["soft_false_per_min"],
        f"{len(fs)} SOFT epochs = {rate:.1f}/min (<= {T['soft_false_per_min']}; audit OTA max 28/min)", kind="WARN")

    # --- the single client process survived the server restart ---
    rec = first(rows, t_exit, r"rfsim passive client reconnected")
    add("client_reconnected", rec is not None and rec - t_exit <= (t_bstart - t_exit) + T["reconnect"],
        "no 'rfsim passive client reconnected' line" if rec is None
        else f"reconnect {rec - t_exit:.1f} s after gNB A exit (gNB B started {t_bstart - t_exit:.1f} s after exit)")
    crash = first(rows, t_rx, r"Assertion|Segmentation fault|SEGV", t_end)
    alive = bool(rows) and rows[-1][0] >= t_end - 12
    add("receiver_no_crash", crash is None and alive,
        "assert/segv line" if crash else ("alive to the end" if alive else "log ended before the run end"))

    # --- stale credit while no server exists ---
    stale = [g for g in ok_g if t_exit + T["drain"] < g["t"] < t_ready]
    add("no_stale_credit_in_gap", not stale,
        f"{len(stale)} CRC-ok grants between old-server exit+{T['drain']:.0f}s and gNB B ready (must be 0)")

    # --- epochs in the change window ---
    win = [e for e in epochs if t_stop - 1 <= e["t"] <= t_ready + 60]
    info("epochs_in_window", ", ".join(f"{e['t'] - t_stop:+.1f}s {e['cls']}/{e['cause']}" for e in win) or "none")
    hard_reset = [e for e in win if e["cls"] == "HARD_RESET"]
    ident = [e for e in win if e["cause"] == "CELL_IDENTITY_CHANGE"]
    sibmib = [e for e in win if e["cause"] in ("SIB1_CHANGE", "MIB_CHANGE") and e["cls"].startswith("HARD")]
    t_exp = None
    if sc == "S0":
        bad = [e for e in win if e["cause"] in ("CELL_IDENTITY_CHANGE", "SIB1_CHANGE", "MIB_CHANGE") or e["cls"] == "HARD_RESET"]
        add("S0_no_identity_or_sib_epoch", not bad, f"forbidden epochs: {[(e['cls'], e['cause']) for e in bad]}")
        add("S0_gap_noted", bool(win), "continuity/RX_RECOVERY epoch seen" if win else "no epoch at all for a multi-second gap", kind="WARN")
    elif sc == "S1":
        e = next((e for e in ident if e["cls"] == "HARD_RESET"), None)
        add("S1_hard_reset_identity", e is not None,
            "no CONFIG_EPOCH HARD_RESET/CELL_IDENTITY_CHANGE in window" if not e else f"at +{e['t'] - t_ready:.1f}s after gNB B ready")
        if e:
            t_exp = e["t"]
            add("S1_epoch_latency", e["t"] - t_ready <= T["epoch_wait"], f"{e['t'] - t_ready:.1f} s (<= {T['epoch_wait']:.0f})")
            sr = first(rows, e["t"] - 1, r"PDSCH_SOLVER reset on new cell identity")
            add("S1_solver_reset", sr is not None, "'PDSCH_SOLVER reset on new cell identity' " + ("absent" if sr is None else f"at {sr - e['t']:+.2f}s vs epoch"))
            sb = first(rows, e["t"] - 1, r"SIB1 decoded|ACQ_STATE .* -> CELL_CONFIGURED")
            add("S1_new_sib1_or_configured", sb is not None, "no SIB1 decode / CELL_CONFIGURED after the epoch" if sb is None else f"at +{sb - e['t']:.1f}s")
        want = meta.get("expect_pci")
        pcis = [r.get("pci") for t, r in metrics_records(rows) if t > t_ready + 5]
        add("S1_metrics_pci", bool(pcis) and want is not None and pcis[-1] == int(want),
            f"last ISAC_METRICS pci={pcis[-1] if pcis else None}, expected {want}")
    elif sc in ("S2", "S2c"):
        e = sibmib[0] if sibmib else None
        add(f"{sc}_hard_sib1_or_mib_epoch", e is not None,
            "no HARD_* SIB1_CHANGE/MIB_CHANGE epoch in window" if not e else f"{e['cls']}/{e['cause']} at +{e['t'] - t_ready:.1f}s")
        add(f"{sc}_pci_unchanged", not ident, "CELL_IDENTITY_CHANGE raised although PCI is unchanged" if ident else "no identity change")
        add(f"{sc}_no_hard_reset", not hard_reset, f"{len(hard_reset)} HARD_RESET")
        if e:
            t_exp = e["t"]
    elif sc == "S3":
        add("S3_no_hard_reset_or_identity", not hard_reset and not ident,
            f"HARD_RESET={len(hard_reset)} identity={len(ident)} (rank/MCS is not a cell change)")
        n_ded = sum(e["cause"] == "DEDICATED_CHANGE_SUSPECTED" for e in win)
        info("S3_dedicated_change_suspected", f"{n_ded} DEDICATED_CHANGE_SUSPECTED epochs (optional: phy-test sends no RRC reconfiguration)")

    # --- recovery of decoding on the new cell ---
    t0 = t_exp if t_exp else t_ready
    first_ok = next((g for g in ok_g if g["t"] >= t0), None)
    lim = T["recover"][sc]
    if first_ok is None:
        add("recovery_crc_ok", False, f"no CRC-ok grant after {'the epoch' if t_exp else 'gNB B ready'}")
    else:
        add("recovery_crc_ok", first_ok["t"] - t_ready <= lim,
            f"first CRC-ok {first_ok['t'] - t_ready:.2f} s after gNB B ready (limit {lim:.0f} s)")
        sus = [g for g in ok_g if first_ok["t"] <= g["t"] <= first_ok["t"] + 20]
        add("recovery_sustained", len(sus) >= T["sustain_n"], f"{len(sus)} CRC-ok grants in the 20 s after first (>= {T['sustain_n']})")
    if t_exp:
        n = len([g for g in ok_g if t_ready <= g["t"] < t_exp])
        info("crc_ok_before_expected_epoch", f"{n} CRC-ok grants between gNB B ready and the epoch")
    tail = [g for g in grants if g["t"] >= t_end - 30]
    tail_ok = sum(g["crc"] == 1 for g in tail)
    add("tail_crc_fraction", len(tail) >= 50 and tail_ok / len(tail) >= T["post_ok_frac"],
        f"{tail_ok}/{len(tail)} CRC-ok in the last 30 s (need >= 50 grants and >= {T['post_ok_frac']:.0%})")

    # --- DCI length re-lock after the change ---
    if sc in ("S1", "S2", "S2c"):
        base = t_exp if t_exp else t_ready
        tl = first(rows, base - 0.5, LOCK)
        add(f"{sc}_dci_length_lock", tl is not None and tl - base <= T["lock_wait"],
            "no 'DCI 1_1 length locked'/RELOCK after the epoch" if tl is None else f"{tl - base:.1f} s after epoch (limit {T['lock_wait']:.0f})")

    # --- S3: grants show the new rank / MCS ---
    if sc == "S3" and first_ok:
        post = [g for g in ok_g if g["t"] >= first_ok["t"]]
        nl, mcs = int(meta["expect_nl"]), int(meta["expect_mcs"])
        fn = sum(g["nl"] == nl for g in post) / len(post)
        fm = sum(g["mcs"] == mcs for g in post) / len(post)
        add("S3_new_rank_seen", fn >= T["param_frac"], f"{fn:.0%} of {len(post)} post-change CRC-ok grants have Nl={nl}")
        add("S3_new_mcs_seen", fm >= T["param_frac"], f"{fm:.0%} have mcs={mcs}")

    # --- epoch-drop counters (informational: drops are the stale-work rejection working) ---
    recs = metrics_records(rows)
    if recs:
        last = recs[-1][1]
        keys = ("scanq_drop_epoch", "pdcch_inline_drop_epoch", "pdschq_drop_epoch", "puschq_drop_epoch")
        info("epoch_drop_counters", " ".join(f"{k}={last.get(k)}" for k in keys))
    else:
        info("epoch_drop_counters", "no ISAC_METRICS line (ISAC_METRICS=1 missing in env?)")
    return res


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    d = Path(argv[1])
    rows = parse_log((d / "run.log").read_text(errors="replace"))
    ev, meta = parse_events((d / "events.txt").read_text())
    res = score(rows, ev, meta, "--lenient" in argv)
    for name, st, detail in res:
        print(f"{st:5s} {name}: {detail}")
    bad = [n for n, st, _ in res if st == "FAIL"]
    print(f"RESULT {meta.get('scenario', '?')}: {'FAIL ' + ','.join(bad) if bad else 'PASS'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
