#!/usr/bin/env python3
"""ota_stats.py -- statistics over passive-receiver run logs (stdlib + numpy only).

Usage:
  tools/ota_stats.py LOG_OR_DIR... [--name NAME] [--md out.md] [--json out.json]
  tools/ota_stats.py --compare A_LOG... --vs B_LOG... [--name-a A --name-b B] [--md ..] [--json ..]
  (a DIR is resolved to DIR/run.log)

TIME BASE
  All times are RECEIVER time: 1 slot = 0.5 ms (mu=1, 20 slots/frame).  A "frame.slot" stamp
  (sfn*20+slot, wraps every 1024 frames = 10.24 s) is unwrapped against the receiver's own
  abs_slot counter (ISAC_METRICS "abs_slot", 20000 slots per 10 s of t_mono_ns) and against the
  running PHY-thread frame stamps, so it is a monotonic slot count since the radio stream
  started.  Convergence times are reported relative to t_sync = abs_slot of the first
  ISAC_METRICS line after the first "Initial sync successful" (the receiver emits it at its
  first monitored occasion, <= a few slots after sync).  Events whose log line carries no
  frame.slot (SOLVED, bank add) are stamped with the latest PHY-thread frame stamp, i.e. log
  order (resolution: a few ms; asynchronous logging can add tens of ms).
  Wall-clock is never used.
"""
import argparse
import json
import math
import os
import re
import sys
from collections import defaultdict

import numpy as np

ANSI = re.compile(r"\x1b\[[0-9;]*m")
SLOT_S = 0.0005
WRAP = 1024 * 20
SI_RNTI = 0xFFFF
MIN_PERSIST = 20          # a "persistent" RNTI has >= this many distinct DCI slots

RE_SEEN = re.compile(r"rnti_seen utc_ns=\d+ rnti=0x([0-9a-f]+) sfn=(\d+) slot=(\d+) fmt=(\w+)")
RE_DCIRAW = re.compile(r"DCIRAW (\d+)\.(\d+) rnti=0x([0-9a-f]+)")
RE_EVM = re.compile(
    r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) tbl=(\d+) Qm=(\d+) Nl=(\d+) "
    r"rb=(\d+)\+(\d+) nsym=(\d+) evm=\S+ snr=(-?[\d.]+) br_snr=\S+ .*?probe=(\d)")
RE_SOLVED = re.compile(r"PDSCH_SOLVER rnti=0x([0-9a-f]+) SOLVED")
RE_LOCK = re.compile(r"DCI (\d_\d) length locked coreset=\d+ rnti=0x([0-9a-f]+)")
RE_BANK = re.compile(r"VERIFIED -> bank add index=(\d+) rnti=0x([0-9a-f]+)")
RE_FFT = re.compile(r"FFTADDR frame=(\d+) slot=(\d+)")
RE_SCAN = re.compile(r"SCANTIME(_FINE)? (\S+) occasions=(\d+) p50<=(\d+)us p99<=(\d+)us")
RE_PDTIM_ST = re.compile(r"(fep|chest|alloc|demod|ldpc)\[n=(\d+) mean=([\d.]+)us")
RE_GPUFEP = re.compile(r"GPU_EXACT_FEP calls=(\d+) fallback=(\d+) .*?check_ok=(\d+) check_bad=(\d+)")
RE_CHPOW = re.compile(r"CHPOW sample=\d+ dBFS ch0=(-?[\d.]+) ch1=(-?[\d.]+) ch2=(-?[\d.]+) ch3=(-?[\d.]+)")

SNR_BINS = ["<6", "6-10", ">=10"]
BUCKETS = [("0-4", 0, 4), ("5-9", 5, 9), ("10-16", 10, 16), ("17+", 17, 99)]
NL_LABELS = {1: "Nl=1", 2: "Nl=2", 3: "Nl>=3"}


def snr_bin(s):
    return 0 if s < 6 else (1 if s < 10 else 2)


def rb_class(abs160, rb_start):
    if rb_start >= 52:
        return "C"
    return "A" if abs160 in (15, 16, 20, 21) else "B"


class Clock:
    """Unwraps frame*20+slot stamps against a running reference (receiver slots)."""

    def __init__(self):
        self.ref = None

    def unwrap(self, x):
        if self.ref is None:
            return x
        return self.ref + ((x - self.ref + WRAP // 2) % WRAP) - WRAP // 2


def parse_run(path):
    r = {"path": path, "name": os.path.basename(os.path.dirname(os.path.abspath(path)))}
    clk = Clock()
    dci = defaultdict(set)           # rnti -> {unwrapped slot} (fmt 1_1)
    dci_fmt = defaultdict(lambda: defaultdict(set))  # fmt -> rnti -> slots
    solved = {}                      # rnti -> slot (first)
    crc_first = {}                   # rnti -> slot
    crc_any = set()
    evm_rntis = set()
    counters = defaultdict(lambda: [0, 0])   # (mcs, nl, snrbin, cls) -> [n, ok]
    n_evm = n_probe = n_si = 0
    seen_evm = set()
    sync_lines = []
    t_sync = None
    pending_sync = False
    bank_adds = []
    locks = defaultdict(set)
    scan = {}
    pdtim = None
    gpufep = None
    chpow = []
    rfstall = recov = reinit = 0
    metrics_first = metrics_last = None
    n_metrics = 0
    clock_off = []
    last_slot = None
    pend_events = []                 # events logged before any slot reference exists

    def stamp(slot_unw):
        nonlocal last_slot
        last_slot = slot_unw

    with open(path, "r", errors="replace") as f:
        for raw in f:
            if "SENSING" not in raw and "Initial sync" not in raw:
                continue
            line = ANSI.sub("", raw)
            if "Initial sync successful" in line:
                sync_lines.append(last_slot)
                if t_sync is None:
                    pending_sync = True
                continue
            if "rnti_seen" in line:
                m = RE_SEEN.search(line)
                if m:
                    rn, sfn, sl, fmt = int(m.group(1), 16), int(m.group(2)), int(m.group(3)), m.group(4)
                    t = clk.unwrap(sfn * 20 + sl)
                    clk.ref = t
                    stamp(t)
                    dci_fmt[fmt][rn].add(t)
                    if fmt == "1_1":
                        dci[rn].add(t)
                    if pending_sync:
                        t_sync, pending_sync = t, False
                continue
            if "EVMGRANT" in line:
                m = RE_EVM.search(line)
                if not m:
                    continue
                rn = int(m.group(1), 16)
                fr, sl = int(m.group(2)), int(m.group(3))
                crc, mcs, nl = int(m.group(4)), int(m.group(5)), int(m.group(8))
                rbs, probe = int(m.group(9)), int(m.group(13))
                snr = float(m.group(12))
                if probe == 1:
                    n_probe += 1
                    continue
                if rn == SI_RNTI:
                    n_si += 1
                    continue
                t = clk.unwrap(fr * 20 + sl)
                key = (rn, t, rbs, int(m.group(10)))
                if key in seen_evm:
                    continue
                seen_evm.add(key)
                n_evm += 1
                evm_rntis.add(rn)
                nlc = min(nl, 3)
                sb = snr_bin(snr)
                cls = ""
                if mcs <= 4 and sb == 2:
                    cls = rb_class((fr * 20 + sl) % 160, rbs)
                c = counters[(mcs, nlc, sb, cls)]
                c[0] += 1
                c[1] += crc
                if crc:
                    crc_any.add(rn)
                    if rn not in crc_first:
                        crc_first[rn] = t
                continue
            if "ISAC_METRICS " in line:
                try:
                    j = json.loads(line[line.index("ISAC_METRICS ") + 13:])
                except Exception:
                    continue
                a = j.get("abs_slot")
                if a is not None:
                    if clk.ref is not None:
                        clock_off.append((clk.ref - a) % WRAP)
                    clk.ref = a
                    stamp(a)
                    if pending_sync:
                        t_sync, pending_sync = a, False
                n_metrics += 1
                if metrics_first is None:
                    metrics_first = j
                metrics_last = j
                continue
            if "FFTADDR" in line:
                m = RE_FFT.search(line)
                if m:
                    t = clk.unwrap(int(m.group(1)) * 20 + int(m.group(2)))
                    clk.ref = t
                    stamp(t)
                continue
            if "PDSCH_SOLVER rnti=" in line and " SOLVED" in line:
                m = RE_SOLVED.search(line)
                if m:
                    solved.setdefault(int(m.group(1), 16), last_slot)
                continue
            if "length locked" in line:
                m = RE_LOCK.search(line)
                if m:
                    locks[m.group(1)].add(int(m.group(2), 16))
                continue
            if "VERIFIED -> bank add" in line:
                m = RE_BANK.search(line)
                if m:
                    bank_adds.append(last_slot)
                continue
            if "DCIRAW" in line:
                m = RE_DCIRAW.search(line)
                if m:
                    t = clk.unwrap(int(m.group(1)) * 20 + int(m.group(2)))
                    dci_fmt["raw"][int(m.group(3), 16)].add(t)
                continue
            if "SCANTIME" in line:
                m = RE_SCAN.search(line)
                if m:
                    scan[("fine" if m.group(1) else "coarse", m.group(2))] = (
                        int(m.group(3)), int(m.group(4)), int(m.group(5)))
                continue
            if "PDTIM calls=" in line:
                pdtim = {s: (int(n), float(mu)) for s, n, mu in RE_PDTIM_ST.findall(line)}
                pdtim["calls"] = int(re.search(r"calls=(\d+)", line).group(1))
                continue
            if "GPU_EXACT_FEP calls=" in line:
                m = RE_GPUFEP.search(line)
                if m:
                    gpufep = dict(calls=int(m.group(1)), fallback=int(m.group(2)),
                                  check_ok=int(m.group(3)), check_bad=int(m.group(4)))
                continue
            if "CHPOW sample=" in line:
                m = RE_CHPOW.search(line)
                if m:
                    chpow.append([float(x) for x in m.groups()])
                continue
            if "RFSTALL" in line:
                rfstall += 1
                if "recovered" in line:
                    recov += 1
                continue

    r["clock_offset_slots_med"] = float(np.median(clock_off)) if clock_off else None
    if t_sync is None:                      # no metrics: fall back on first DCI
        allt = [min(s) for s in dci.values()]
        t_sync = min(allt) if allt else 0
    r["t_sync_slot"] = t_sync
    r["t_sync_s"] = t_sync * SLOT_S
    r["n_sync_lines"] = len(sync_lines)
    rel = lambda t: None if t is None else (t - t_sync) * SLOT_S

    # --- UEs --------------------------------------------------------------
    dci_c = {rn: s for rn, s in dci.items() if rn != SI_RNTI}
    pers = {rn for rn, s in dci_c.items() if len(s) >= MIN_PERSIST}
    ul = dci_fmt.get("0_1", {})
    r["ues"] = dict(
        dci11=len(dci_c), dci11_persistent=len(pers),
        dci_any_fmt=len({rn for fm in ("1_0", "1_1") for rn in dci_fmt.get(fm, {}) if rn != SI_RNTI}),
        ul_dci01=len(ul), ul_dci01_logged=bool(ul or locks.get("0_1")),
        solved=len(solved), solved_persistent=len([x for x in solved if x in pers]),
        crc_ok=len(crc_any), crc_ok_persistent=len([x for x in crc_any if x in pers]),
        evm_rntis=len(evm_rntis), lock_rntis_11=len(locks.get("1_1", ())))
    # --- convergence (seconds after t_sync) -----------------------------------
    conv = dict(
        first_bank_add=[rel(bank_adds[0])] if bank_adds else [],
        n_bank_adds=len(bank_adds),
        first_dci={}, solve={}, first_crc={}, solve_lag={}, crc_lag={})
    for rn, s in dci_c.items():
        conv["first_dci"][rn] = rel(min(s))
    for rn, t in solved.items():
        if t is not None:
            conv["solve"][rn] = rel(t)
    for rn, t in crc_first.items():
        conv["first_crc"][rn] = rel(t)
    for rn, t in conv["solve"].items():
        if rn in conv["first_dci"] and t is not None:
            conv["solve_lag"][rn] = t - conv["first_dci"][rn]
    for rn, t in conv["first_crc"].items():
        if rn in conv["first_dci"] and t is not None:
            conv["crc_lag"][rn] = t - conv["first_dci"][rn]
    r["conv"] = conv
    r["persistent_rntis"] = sorted(pers)
    # --- activity (DCI 1_1, distinct slots, 1 s bins on the receiver clock) ------
    act = {}
    allbins = defaultdict(int)
    for rn, s in dci_c.items():
        arr = np.array(sorted(s), dtype=np.int64)
        bins = np.unique(arr // 2000)
        span = bins[-1] - bins[0] + 1
        act[rn] = dict(n=len(arr), active_time_s=float((arr[-1] - arr[0]) * SLOT_S),
                       active_s=int(len(bins)), grants_per_active_s=float(len(arr) / len(bins)),
                       duty=float(len(bins) / span), first_s=rel(arr[0]), last_s=rel(arr[-1]))
        if rn in pers:
            for b in bins:
                allbins[int(b)] += 1
    r["activity"] = act
    if allbins:
        b0, b1 = min(allbins), max(allbins)
        r["concurrent_active_ues_per_s"] = float(np.mean([allbins.get(b, 0) for b in range(b0, b1 + 1)]))
    else:
        r["concurrent_active_ues_per_s"] = 0.0
    # --- decoding -----------------------------------------------------------
    r["counters"] = {f"{k[0]}|{k[1]}|{k[2]}|{k[3]}": v for k, v in counters.items()}
    r["evm"] = dict(decoded=n_evm, probes_excluded=n_probe, si_excluded=n_si)
    # --- processing -----------------------------------------------------------
    r["scan"] = {f"{k[0]}:{k[1]}": v for k, v in scan.items()}
    r["pdtim"] = pdtim
    r["gpu_fep"] = gpufep
    if chpow:
        c = np.array(chpow)
        r["chpow_dbfs_median"] = [float(x) for x in np.median(c, axis=0)]
        r["chpow_dbfs_max"] = [float(x) for x in np.max(c, axis=0)]
    # --- stability --------------------------------------------------------------
    ml = metrics_last or {}
    mf = metrics_first or {}
    dur = None
    if metrics_last and metrics_first and "t_mono_ns" in ml:
        dur = (ml["t_mono_ns"] - mf["t_mono_ns"]) / 1e9
    r["stab"] = dict(rfstall=rfstall, rfstall_recovered=recov, sync_lines=len(sync_lines),
                     acq_sync_losses=ml.get("acq_sync_losses"), acq_state=ml.get("acq_state"),
                     metrics_span_s=dur, n_metrics=n_metrics,
                     sync_stream_s=r["t_sync_s"])
    keys = ["scanq_queued", "scanq_processed", "scanq_drop_full", "scanq_drop_stale",
            "pdschq_decoded", "pdschq_crc_ok", "pdschq_drop_full", "pdschq_drop_stale",
            "gpu_fep_selfcheck_fail", "gpu_fep_disabled", "pdcch_accepts", "pdcch_occasions"]
    r["metrics_last"] = {k: ml.get(k) for k in keys}
    ex = os.path.join(os.path.dirname(path), "rx_exit.txt")
    r["rx_exit"] = open(ex).read().strip() if os.path.exists(ex) else None
    return r


# ---------------------------------------------------------------- aggregation
def desc(xs):
    xs = [x for x in xs if x is not None and not (isinstance(x, float) and math.isnan(x))]
    if not xs:
        return dict(n=0)
    a = np.array(xs, dtype=float)
    return dict(n=len(a), mean=float(a.mean()), std=float(a.std(ddof=1)) if len(a) > 1 else 0.0,
                min=float(a.min()), p25=float(np.percentile(a, 25)), median=float(np.median(a)),
                p75=float(np.percentile(a, 75)), p90=float(np.percentile(a, 90)), max=float(a.max()))


def wilson(k, n, z=1.96):
    if n == 0:
        return (float("nan"), float("nan"))
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (100 * max(0, c - h), 100 * min(1, c + h))


def cell(runs, mcs_range=None, snrbins=None, nls=None, cls=None, mcs_set=None):
    """Return per-run [(n, ok)] and pooled (n, ok) for the selection."""
    per = []
    for r in runs:
        n = ok = 0
        for k, v in r["counters"].items():
            m, nl, sb, c = k.split("|")
            m, nl, sb = int(m), int(nl), int(sb)
            if mcs_range and not (mcs_range[0] <= m <= mcs_range[1]):
                continue
            if snrbins is not None and sb not in snrbins:
                continue
            if nls is not None and nl not in nls:
                continue
            if cls is not None and c != cls:
                continue
            n += v[0]
            ok += v[1]
        per.append((n, ok))
    return per, (sum(p[0] for p in per), sum(p[1] for p in per))


def cell_summary(runs, min_n=20, **kw):
    per, (n, ok) = cell(runs, **kw)
    pr = [100 * o / nn for nn, o in per if nn >= min_n]
    lo, hi = wilson(ok, n)
    return dict(n=n, ok=ok, pct=(100 * ok / n if n else None), ci_lo=lo, ci_hi=hi,
                run_std=(float(np.std(pr, ddof=1)) if len(pr) > 1 else None), runs_used=len(pr),
                run_min=(min(pr) if pr else None), run_max=(max(pr) if pr else None))


def aggregate(runs):
    A = {"n_runs": len(runs), "runs": [r["name"] for r in runs]}
    # UEs
    A["ues"] = {k: desc([r["ues"][k] for r in runs]) for k in
                ("dci11", "dci11_persistent", "dci_any_fmt", "solved", "solved_persistent",
                 "crc_ok", "crc_ok_persistent", "evm_rntis")}
    A["ues"]["ul_dci01_logged_in_any_run"] = any(r["ues"]["ul_dci01_logged"] for r in runs)
    A["ues"]["ul_dci01"] = desc([r["ues"]["ul_dci01"] for r in runs])
    # convergence: pooled over RNTIs; also per-run medians
    A["conv"] = {}
    for sel in ("all", "persistent"):
        d = {}
        for key in ("first_dci", "solve", "first_crc", "solve_lag", "crc_lag"):
            pooled, permed = [], []
            for r in runs:
                v = [t for rn, t in r["conv"][key].items()
                     if t is not None and (sel == "all" or rn in set(r["persistent_rntis"]))]
                pooled += v
                if v:
                    permed.append(float(np.median(v)))
            d[key] = dict(pooled=desc(pooled), per_run_median=desc(permed))
        A["conv"][sel] = d
    A["conv"]["first_bank_add"] = desc([r["conv"]["first_bank_add"][0] for r in runs
                                        if r["conv"]["first_bank_add"]])
    A["conv"]["n_bank_adds"] = desc([r["conv"]["n_bank_adds"] for r in runs])
    # first-of-run
    fo = {}
    for key in ("first_dci", "solve", "first_crc"):
        fo[key] = desc([min(v for v in r["conv"][key].values() if v is not None)
                        for r in runs if any(v is not None for v in r["conv"][key].values())])
    A["conv"]["first_of_run"] = fo
    # activity
    A["activity"] = {}
    for sel in ("all", "persistent"):
        d = {}
        for fld in ("active_time_s", "active_s", "grants_per_active_s", "duty"):
            pooled, meds = [], []
            for r in runs:
                v = [a[fld] for rn, a in r["activity"].items()
                     if sel == "all" or rn in set(r["persistent_rntis"])]
                pooled += v
                if v:
                    meds.append(float(np.median(v)))
            d[fld] = dict(pooled=desc(pooled), per_run_median=desc(meds))
        A["activity"][sel] = d
    A["activity"]["concurrent_active_ues_per_s"] = desc([r["concurrent_active_ues_per_s"] for r in runs])
    # decoding
    D = {"mcs": {}, "bucket": {}, "bucket_x_snr": {}, "bucket_x_nl_x_snr": {}, "mcs_dist": {}, "class": {}}
    allsel = cell(runs)[1][0]
    for m in range(32):
        D["mcs"][m] = cell_summary(runs, mcs_range=(m, m))
        n = D["mcs"][m]["n"]
        D["mcs_dist"][m] = dict(n=n, pct=100 * n / allsel if allsel else 0.0)
    for name, lo, hi in BUCKETS:
        D["bucket"][name] = cell_summary(runs, mcs_range=(lo, hi))
        for sb in range(3):
            D["bucket_x_snr"][f"{name}|{SNR_BINS[sb]}"] = cell_summary(runs, mcs_range=(lo, hi), snrbins={sb})
            for nl in (1, 2, 3):
                D["bucket_x_nl_x_snr"][f"{name}|{NL_LABELS[nl]}|{SNR_BINS[sb]}"] = cell_summary(
                    runs, mcs_range=(lo, hi), snrbins={sb}, nls={nl})
    for sb in range(3):
        D["bucket_x_snr"][f"all|{SNR_BINS[sb]}"] = cell_summary(runs, snrbins={sb})
    D["bucket_x_snr"]["all|all"] = cell_summary(runs)
    for c in "ABC":
        D["class"][c] = cell_summary(runs, mcs_range=(0, 4), snrbins={2}, cls=c)
    D["decoded"] = desc([r["evm"]["decoded"] for r in runs])
    D["probes_excluded"] = sum(r["evm"]["probes_excluded"] for r in runs)
    D["si_excluded"] = sum(r["evm"]["si_excluded"] for r in runs)
    A["decoding"] = D
    # processing
    P = {}
    for key in sorted({k for r in runs for k in r["scan"]}):
        P["scan:" + key + ":p50_us"] = desc([r["scan"][key][1] for r in runs if key in r["scan"]])
        P["scan:" + key + ":p99_us"] = desc([r["scan"][key][2] for r in runs if key in r["scan"]])
    for st in ("fep", "chest", "alloc", "demod", "ldpc"):
        P["pdtim:" + st + "_mean_us"] = desc([r["pdtim"][st][1] for r in runs if r["pdtim"] and st in r["pdtim"]])
    P["gpu_fep_check_bad"] = desc([r["gpu_fep"]["check_bad"] for r in runs if r["gpu_fep"]])
    P["gpu_fep_check_ok"] = desc([r["gpu_fep"]["check_ok"] for r in runs if r["gpu_fep"]])
    P["gpu_fep_fallback"] = desc([r["gpu_fep"]["fallback"] for r in runs if r["gpu_fep"]])
    for k in ("scanq_drop_full", "scanq_drop_stale", "pdschq_drop_full", "pdschq_drop_stale",
              "gpu_fep_selfcheck_fail"):
        P["metrics:" + k] = desc([r["metrics_last"].get(k) for r in runs])
    ratio = [100 * r["metrics_last"]["scanq_drop_full"] / r["metrics_last"]["scanq_queued"]
             for r in runs if r["metrics_last"].get("scanq_queued")]
    P["scanq_drop_full_pct"] = desc(ratio)
    for ch in range(4):
        P[f"chpow_ch{ch}_median_dbfs"] = desc([r["chpow_dbfs_median"][ch] for r in runs if "chpow_dbfs_median" in r])
    A["processing"] = P
    # stability
    A["stability"] = dict(
        rfstall=desc([r["stab"]["rfstall"] for r in runs]),
        rfstall_recovered=desc([r["stab"]["rfstall_recovered"] for r in runs]),
        sync_lines=desc([r["stab"]["sync_lines"] for r in runs]),
        acq_sync_losses=desc([r["stab"]["acq_sync_losses"] for r in runs]),
        sync_time_s=desc([r["stab"]["sync_stream_s"] for r in runs]),
        duration_s=desc([r["stab"]["metrics_span_s"] for r in runs]),
        rx_exit=[r["rx_exit"] for r in runs])
    return A


# ---------------------------------------------------------------- rendering
def f(x, d=1):
    return "-" if x is None or (isinstance(x, float) and math.isnan(x)) else f"{x:.{d}f}"


def dtxt(s, d=1):
    if not s or s.get("n", 0) == 0:
        return "-"
    return f"{f(s['mean'], d)} +/- {f(s['std'], d)} [{f(s['min'], d)}, {f(s['max'], d)}] (n={s['n']})"


def mdtable(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out) + "\n"


def drow(label, s, d=2):
    if not s or s.get("n", 0) == 0:
        return [label, 0, "-", "-", "-", "-", "-", "-"]
    return [label, s["n"], f(s["median"], d), f(s["p25"], d) + "-" + f(s["p75"], d), f(s["p90"], d),
            f(s["mean"], d), f(s["std"], d), f(s["min"], d) + "-" + f(s["max"], d)]


DHEAD = ["quantity", "n", "median", "IQR", "p90", "mean", "std", "min-max"]


def cellfmt(c):
    if c["n"] == 0:
        return "-"
    sd = "" if c["run_std"] is None else f" sd_run={f(c['run_std'])}"
    return f"{f(c['pct'])}% [{f(c['ci_lo'])},{f(c['ci_hi'])}] n={c['n']}{sd}"


def render_set(title, A, per_run=None):
    o = [f"## {title}\n", f"Runs ({A['n_runs']}): {', '.join(A['runs'])}\n"]
    U = A["ues"]
    o.append("### 1. UEs (distinct RNTIs per run; mean +/- std [min, max] across runs)\n")
    o.append(mdtable(["quantity", "across runs"], [
        ["RNTIs with >=1 DL C-RNTI DCI 1_1 (log: `blind PDCCH rnti_seen ... fmt=1_1`, SI-RNTI excluded)", dtxt(U["dci11"])],
        [f"... of which persistent (>= {MIN_PERSIST} distinct DCI slots)", dtxt(U["dci11_persistent"])],
        ["RNTIs with DCI fmt 1_0 or 1_1 (non-SI)", dtxt(U["dci_any_fmt"])],
        ["RNTIs with `PDSCH_SOLVER ... SOLVED`", dtxt(U["solved"])],
        ["... solved and persistent", dtxt(U["solved_persistent"])],
        ["RNTIs with >=1 CRC-OK decode (`EVMGRANT crc=1 probe=0`, non-SI)", dtxt(U["crc_ok"])],
        ["... CRC-OK and persistent", dtxt(U["crc_ok_persistent"])],
        ["RNTIs with any non-probe EVMGRANT", dtxt(U["evm_rntis"])],
        ["UL DCI 0_1 RNTIs", "not logged by the receiver (rnti_seen only emits fmt 1_0/1_1)"
         if not U["ul_dci01_logged_in_any_run"] else dtxt(U["ul_dci01"])]]))
    o.append("A \"persistent\" RNTI has >= %d distinct DCI slots; the all-RNTI rows include short-lived and "
             "possibly spurious RNTIs (a 16-bit RNTI seen a handful of times).\n" % MIN_PERSIST)
    o.append("### 2. Convergence, seconds after `Initial sync successful` (receiver slot clock, 0.5 ms/slot)\n")
    C = A["conv"]
    rows = [drow("first dedicated CORESET bank add (per run)", C["first_bank_add"])]
    for sel in ("all", "persistent"):
        for key, lab in (("first_dci", "first DCI 1_1 per RNTI"), ("solve", "solve time per RNTI"),
                         ("first_crc", "first CRC-OK per RNTI"),
                         ("solve_lag", "solve minus first DCI (receiver lag, s)"),
                         ("crc_lag", "first CRC-OK minus first DCI (receiver lag, s)")):
            rows.append(drow(f"{lab} [{sel} RNTIs, pooled]", C[sel][key]["pooled"]))
            rows.append(drow(f"{lab} [{sel}, per-run medians]", C[sel][key]["per_run_median"]))
    for key, lab in (("first_dci", "first DCI of any RNTI"), ("solve", "first solve of any RNTI"),
                     ("first_crc", "first CRC-OK of any RNTI")):
        rows.append(drow(lab + " (per run)", C["first_of_run"][key]))
    o.append(mdtable(DHEAD, rows))
    o.append("Pooled = every RNTI of every run in one sample (spread = between-RNTI); per-run medians = one value "
             "per run (spread = between-run). Bank-add and solve events have no own frame.slot and use the latest "
             "PHY-thread stamp. first DCI per RNTI is dominated by when each UE starts being scheduled (cell load / UE arrival), not by the receiver; the two minus-first-DCI rows isolate the receiver-attributable lag (negative = solved/decoded before a DCI was logged for it). Solve / first-CRC only exist for RNTIs that reached that state (censored otherwise).\n")
    o.append("### 3. Activity per RNTI (DCI 1_1, distinct slots; 1 s bins of the receiver clock)\n")
    Act = A["activity"]
    rows = []
    for sel in ("persistent", "all"):
        for fld, lab, d in (("active_time_s", "active time = last-first DCI (s)", 1),
                            ("active_s", "active seconds (bins with >=1 DCI)", 1),
                            ("grants_per_active_s", "DCI/s over active seconds", 1),
                            ("duty", "duty cycle = active s / span s", 2)):
            rows.append(drow(f"{lab} [{sel}]", Act[sel][fld]["pooled"], d))
    o.append(mdtable(DHEAD, rows))
    o.append("Mean concurrently-active persistent RNTIs per second: " + dtxt(Act["concurrent_active_ues_per_s"], 2) +
             " (across runs). Statistics are pooled over RNTIs of all runs; DCI = PDCCH detections (not PDSCH grants decoded).\n")
    D = A["decoding"]
    o.append("### 4. Decoding (EVMGRANT lines, probe=0, SI-RNTI 0xffff excluded; one line = one decode attempt)\n")
    o.append(f"Decode attempts: {dtxt(D['decoded'], 0)} per run; excluded probe=1 lines: {D['probes_excluded']}, "
             f"SI-RNTI lines: {D['si_excluded']}. Cell format: pooled CRC-OK % [95% Wilson CI] n sd_run "
             "(std of per-run % across runs with n>=20).\n")
    rows = []
    for name, _, _ in BUCKETS:
        rows.append([name] + [cellfmt(D["bucket_x_snr"].get(f"{name}|{s}", D["bucket"][name]))
                              for s in SNR_BINS] + [cellfmt(D["bucket"][name])])
    rows.append(["all"] + [cellfmt(D["bucket_x_snr"][f"all|{s}"]) for s in SNR_BINS] + [cellfmt(D["bucket_x_snr"]["all|all"])])
    o.append("**CRC-OK % by MCS bucket and SNR bin (all Nl)**\n")
    o.append(mdtable(["MCS bucket"] + [f"SNR {s} dB" for s in SNR_BINS] + ["all SNR"], rows))
    o.append("**CRC-OK % by MCS bucket x Nl x SNR bin**\n")
    rows = []
    for name, _, _ in BUCKETS:
        for nl in (1, 2, 3):
            rows.append([name, NL_LABELS[nl]] + [cellfmt(D["bucket_x_nl_x_snr"][f"{name}|{NL_LABELS[nl]}|{s}"])
                                                 for s in SNR_BINS])
    o.append(mdtable(["MCS bucket", "layers"] + [f"SNR {s} dB" for s in SNR_BINS], rows))
    o.append("**Per MCS index (0-31): share of transmitted decode attempts and CRC-OK %**\n")
    rows = []
    for m in range(32):
        c, d = D["mcs"][m], D["mcs_dist"][m]
        if c["n"] == 0:
            continue
        rows.append([m, f"{f(d['pct'], 2)}%", c["n"], f"{f(c['pct'])}%", f"[{f(c['ci_lo'])},{f(c['ci_hi'])}]",
                     f(c["run_std"]) if c["run_std"] is not None else "-"])
    o.append(mdtable(["MCS", "% of grants", "n", "CRC-OK %", "95% CI", "sd across runs"], rows))
    o.append("MCS index is as logged (`mcs=`), table given by `tbl=` (almost all tbl=1, 256QAM table). "
             "MCS 28-31 are reserved/retransmission indices of the table.\n")
    o.append("**Class split, MCS 0-4 and SNR >= 10 dB** (A: abs slot%160 in {15,16,20,21} and RB start<52; "
             "B: other slots, RB start<52; C: RB start>=52; abs slot = (frame*20+slot)%160)\n")
    o.append(mdtable(["class", "CRC-OK % [95% CI] n sd_run"], [[c, cellfmt(D["class"][c])] for c in "ABC"]))
    o.append("### 5. Processing (final cumulative value in each run; across-run mean +/- std [min,max])\n")
    P = A["processing"]
    rows = [[k, dtxt(v, 1)] for k, v in P.items()]
    o.append(mdtable(["quantity", "across runs"], rows))
    o.append("SCANTIME p50/p99 are histogram upper bounds in us (`p50<=`), `coarse` = SCANTIME, `fine` = SCANTIME_FINE; "
             "`ded-only` = dedicated-CORESET scan, `full` = incl. CSS0. PDTIM = per-stage mean per call (cumulative). "
             "GPU check_bad = bit-exactness check failures of GPU FEP vs CPU (last GPU_EXACT_FEP line).\n")
    o.append("### 6. Stability\n")
    S = A["stability"]
    o.append(mdtable(["quantity", "across runs"], [
        ["RFSTALL lines", dtxt(S["rfstall"], 1)], ["RFSTALL recovered", dtxt(S["rfstall_recovered"], 1)],
        ["`Initial sync successful` lines", dtxt(S["sync_lines"], 1)],
        ["acq_sync_losses (last ISAC_METRICS)", dtxt(S["acq_sync_losses"], 1)],
        ["sync time = abs_slot of sync, s since stream start", dtxt(S["sync_time_s"], 2)],
        ["ISAC_METRICS span (t_mono_ns), s", dtxt(S["duration_s"], 1)],
        ["rx_exit", ", ".join(str(x) for x in S["rx_exit"])]]))
    if per_run:
        o.append("### Per-run summary\n")
        rows = []
        for r in per_run:
            u = r["ues"]
            rows.append([r["name"], u["dci11"], u["dci11_persistent"], u["solved"], u["crc_ok"], r["evm"]["decoded"],
                         f(r["t_sync_s"], 2), r["stab"]["rfstall"], r["stab"]["sync_lines"], r["rx_exit"]])
        o.append(mdtable(["run", "RNTI DCI1_1", "persistent", "solved", "CRC-OK", "decodes", "sync s", "RFSTALL",
                          "sync lines", "exit"], rows))
    return "\n".join(o)


def newcombe(k1, n1, k2, n2):
    if n1 == 0 or n2 == 0:
        return None
    p1, p2 = k1 / n1, k2 / n2
    l1, u1 = (x / 100 for x in wilson(k1, n1))
    l2, u2 = (x / 100 for x in wilson(k2, n2))
    d = p2 - p1
    lo = d - math.sqrt((p2 - l2) ** 2 + (u1 - p1) ** 2)
    hi = d + math.sqrt((u2 - p2) ** 2 + (p1 - l1) ** 2)
    return 100 * d, 100 * lo, 100 * hi


def render_compare(na, A, nb, B):
    o = [f"## Comparison {na} (A) vs {nb} (B)\n",
         f"A = {na}: {A['n_runs']} runs; B = {nb}: {B['n_runs']} runs. Diff = B - A, 95% Newcombe CI for "
         "proportions (pooled over runs; treats decodes as independent, so CIs are optimistic). "
         "With so few runs the across-run std is indicative only.\n"]
    rows = []
    for k, lab in (("dci11", "RNTIs DCI 1_1"), ("dci11_persistent", "persistent RNTIs"), ("solved", "solved"),
                   ("crc_ok", "CRC-OK RNTIs"), ("crc_ok_persistent", "CRC-OK persistent")):
        rows.append([lab, dtxt(A["ues"][k]), dtxt(B["ues"][k])])
    o.append("### UEs\n")
    o.append(mdtable(["quantity", f"A {na}", f"B {nb}"], rows))
    o.append("### Convergence medians (s after sync, pooled RNTIs, persistent RNTIs)\n")
    rows = []
    for key, lab in (("first_dci", "first DCI"), ("solve", "solve"), ("first_crc", "first CRC-OK")):
        a, b = A["conv"]["persistent"][key]["pooled"], B["conv"]["persistent"][key]["pooled"]
        rows.append([lab, f"{f(a.get('median'))} (p90 {f(a.get('p90'))}, n={a['n']})",
                     f"{f(b.get('median'))} (p90 {f(b.get('p90'))}, n={b['n']})"])
    a, b = A["conv"]["first_bank_add"], B["conv"]["first_bank_add"]
    rows.append(["first bank add (per run)", dtxt(a, 2), dtxt(b, 2)])
    o.append(mdtable(["event", f"A {na}", f"B {nb}"], rows))
    o.append("### Activity (persistent RNTIs, pooled medians)\n")
    rows = []
    for fld, lab in (("active_time_s", "active time s"), ("grants_per_active_s", "DCI per active s"),
                     ("duty", "duty cycle")):
        a, b = A["activity"]["persistent"][fld]["pooled"], B["activity"]["persistent"][fld]["pooled"]
        rows.append([lab, f(a.get("median"), 2), f(b.get("median"), 2)])
    rows.append(["concurrent active UEs/s", f(A["activity"]["concurrent_active_ues_per_s"].get("mean"), 2),
                 f(B["activity"]["concurrent_active_ues_per_s"].get("mean"), 2)])
    o.append(mdtable(["quantity", f"A {na}", f"B {nb}"], rows))
    o.append("### Decoding: CRC-OK % (A, B, diff B-A with 95% CI)\n")
    rows = []
    for key in (f"{b}|{s}" for b, _, _ in BUCKETS for s in SNR_BINS):
        ca, cb = A["decoding"]["bucket_x_snr"][key], B["decoding"]["bucket_x_snr"][key]
        nc = newcombe(ca["ok"], ca["n"], cb["ok"], cb["n"])
        sig = "" if nc is None else (" *" if nc[1] > 0 or nc[2] < 0 else "")
        rows.append([key.replace("|", " / SNR "), cellfmt(ca), cellfmt(cb),
                     "-" if nc is None else f"{f(nc[0])} [{f(nc[1])},{f(nc[2])}]{sig}"])
    for key in ("all|<6", "all|6-10", "all|>=10", "all|all"):
        ca, cb = A["decoding"]["bucket_x_snr"][key], B["decoding"]["bucket_x_snr"][key]
        nc = newcombe(ca["ok"], ca["n"], cb["ok"], cb["n"])
        sig = "" if nc is None else (" *" if nc[1] > 0 or nc[2] < 0 else "")
        rows.append([key.replace("|", " / SNR "), cellfmt(ca), cellfmt(cb),
                     "-" if nc is None else f"{f(nc[0])} [{f(nc[1])},{f(nc[2])}]{sig}"])
    for c in "ABC":
        ca, cb = A["decoding"]["class"][c], B["decoding"]["class"][c]
        nc = newcombe(ca["ok"], ca["n"], cb["ok"], cb["n"])
        sig = "" if nc is None else (" *" if nc[1] > 0 or nc[2] < 0 else "")
        rows.append([f"MCS0-4 >=10 dB class {c}", cellfmt(ca), cellfmt(cb),
                     "-" if nc is None else f"{f(nc[0])} [{f(nc[1])},{f(nc[2])}]{sig}"])
    o.append(mdtable(["cell", f"A {na}", f"B {nb}", "diff B-A [95% CI]"], rows))
    o.append("`*` = CI excludes 0.\n")
    o.append("### Processing and stability\n")
    rows = []
    for k in A["processing"]:
        if k in B["processing"] and (k.startswith(("scan:fine:ded-only", "scan:coarse:ded-only", "pdtim", "gpu_fep_check", "scanq_drop"))):
            rows.append([k, dtxt(A["processing"][k]), dtxt(B["processing"][k])])
    for k in ("rfstall", "sync_lines", "acq_sync_losses", "sync_time_s"):
        rows.append([k, dtxt(A["stability"][k], 2), dtxt(B["stability"][k], 2)])
    o.append(mdtable(["quantity", f"A {na}", f"B {nb}"], rows))
    return "\n".join(o)


def resolve(p):
    return os.path.join(p, "run.log") if os.path.isdir(p) else p


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--name", default="set")
    ap.add_argument("--compare", nargs="+", metavar="A", help="logs of set A")
    ap.add_argument("--vs", nargs="+", metavar="B", help="logs of set B (with --compare)")
    ap.add_argument("--name-a", default="A")
    ap.add_argument("--name-b", default="B")
    ap.add_argument("--md")
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    out_md, out_js = [], {}
    if a.compare:
        if not a.vs:
            ap.error("--compare needs --vs")
        ra = [parse_run(resolve(p)) for p in a.compare]
        rb = [parse_run(resolve(p)) for p in a.vs]
        Aa, Bb = aggregate(ra), aggregate(rb)
        out_md += [render_set(a.name_a, Aa, ra), render_set(a.name_b, Bb, rb), render_compare(a.name_a, Aa, a.name_b, Bb)]
        out_js = {a.name_a: {"aggregate": Aa, "runs": ra}, a.name_b: {"aggregate": Bb, "runs": rb}}
    else:
        if not a.logs:
            ap.error("no logs")
        rs = [parse_run(resolve(p)) for p in a.logs]
        Aa = aggregate(rs)
        out_md.append(render_set(a.name, Aa, rs))
        out_js = {a.name: {"aggregate": Aa, "runs": rs}}
    md = "\n".join(out_md)
    if a.md:
        with open(a.md, "w") as fh:
            fh.write(md)
    else:
        print(md)
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(out_js, fh, indent=1, default=lambda o: sorted(o) if isinstance(o, set) else str(o))
    return 0


if __name__ == "__main__":
    sys.exit(main())
