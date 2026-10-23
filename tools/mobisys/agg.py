"""Stateful aggregation of parsed receiver lines into dataset rows and the run summary. No I/O, no threads.

emit(dataset, row) receives lab rows and window aggregates. In commercial mode no per-grant/per-UE/RNTI row is
ever produced here (policy.Sink would refuse it anyway), and windows are k-suppressed (policy.K_COMMERCIAL).
"""
import collections

import parse
import policy
from hist import Hist

PEND_MAX = 8192      # recent failed grants kept to attribute a later rescue (bounded memory)
UE_SET_MAX = 65536   # distinct-RNTI counter per window, discarded at window close, never persisted
CAL_MAX = 64
SPAN_TOL_US = 4      # each E2E_US term is truncated to us; serial sum may differ from tot by a few us

_MCS = ((4, "0-4"), (9, "5-9"), (16, "10-16"), (27, "17-27"))
_MILESTONES = ("sync_start", "sync_ok", "pbch_ok", "pbch_locked", "sib1_decoded", "css0_autoconf", "coreset_verified",
               "first_dci", "dci_len_locked", "td_converged", "first_crc_ok", "ul_locked")


def mcs_bucket(m):
    for hi, name in _MCS:
        if m is not None and 0 <= m <= hi:
            return name
    return "na"


def nl_bucket(n):
    return "na" if n is None or n < 1 else ("1" if n == 1 else "2" if n == 2 else "3-4" if n <= 4 else "na")


def ue_bucket(n):
    return "0" if n == 0 else "1-9" if n < 10 else "10-49" if n < 50 else "50+"


def union_us(intervals):
    """Total covered length of (start, end) intervals: overlapping spans are counted once."""
    tot, cur_s, cur_e = 0, None, None
    for s, e in sorted(intervals):
        if cur_e is None or s > cur_e:
            if cur_e is not None:
                tot += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    return tot + (cur_e - cur_s if cur_e is not None else 0)


def _enum(v, name):
    return v if v in policy.ENUMS[name] else policy.ENUMS[name][-1]


class _Win:
    def __init__(self):
        self.grp = {}
        self.lat = {}
        self.ue = set()
        self.accepts = self.grants = self.ok = 0

    def group(self, key):
        g = self.grp.get(key)
        if g is None:
            g = self.grp[key] = {"n": 0, "ok": 0, "snr": 0.0, "snr_n": 0, "e2e": Hist(), "dec": Hist()}
        return g

    def hist(self, dir_, metric):
        return self.lat.setdefault((dir_, metric), Hist())


class Aggregator:
    def __init__(self, emit, mode="commercial", slots_per_frame=20):
        self.emit, self.mode = emit, mode
        self.lab = mode == "lab"
        self.k = 1 if self.lab else policy.K_COMMERCIAL
        self.unwrap = parse.SfnUnwrap(slots_per_frame)
        self.seq = 0
        self.pci = None
        self.acq_state = None
        self.win_idx = None
        self.win = _Win()
        self.pend = collections.OrderedDict()
        self.rescued = set()
        self.c = collections.Counter()
        self.g = {m: Hist() for m in ("e2e_tot", "legacy_dec_us", "arr2enq", "qwait", "pre", "dec_span", "serial")}
        self.pdtim_real = None
        self.milestones = {}
        self.cal = collections.deque(maxlen=CAL_MAX)
        self.last_metrics = {}

    # ---------------------------------------------------------------- entry
    def feed_line(self, line, now, t_s):
        self.c["lines"] += 1
        r = parse.parse_line(line)
        if r is None:
            return
        kind, d = r
        self.c["parsed_" + kind] += 1
        w = int(now // policy.WINDOW_S)
        if self.win_idx is None:
            self.win_idx = w
        elif w > self.win_idx:
            self._close_window()
            self.win_idx = w
        getattr(self, "_on_" + kind)(d, t_s)

    def close(self):
        if self.win_idx is not None:
            self._close_window()

    def _row(self, dataset, row):
        self.seq += 1
        row["seq"] = self.seq
        self.emit(dataset, row)

    def _mark(self, name, t_s):
        if name in _MILESTONES and name not in self.milestones:
            self.milestones[name] = round(t_s, 3)

    # ---------------------------------------------------------------- handlers
    def _on_evmgrant(self, d, t_s):
        ext = self.unwrap(d["frame"], d["slot"])
        e2e, es, stg = d["e2e"] or {}, d["es"], d["stg"] or {}
        outcome = "crc_ok" if d["crc"] == 1 else "crc_fail"
        parts = [e2e.get(k) for k in ("arr2enq", "qwait", "pre", "dec")]
        serial = sum(parts) if all(p is not None for p in parts) else None
        tot = e2e.get("tot")
        bad_span = serial is not None and tot is not None and serial > tot + SPAN_TOL_US
        if bad_span:
            self.c["span_inconsistent"] += 1
        if d["probe"]:
            self.c["probe_decodes"] += 1
        else:
            self.c["grants"] += 1
            self.c[outcome] += 1
            self.c["grants_dl"] += 1
            if d["crc"] == 1:
                if "first_crc_ok" not in self.milestones and self.lab:
                    self._ev("first_crc_ok", t_s, rnti=d["rnti"])
                self._mark("first_crc_ok", t_s)
            else:
                if len(self.pend) >= PEND_MAX:
                    self.pend.popitem(last=False)
                self.pend[(d["rnti"], d["frame"], d["slot"], d["pid"])] = self.seq + 1
            self._window_grant("dl", d["mcs"], d["nl"], d["crc"] == 1, d["snr"], tot, d["legacy_dec_us"], d["rnti"])
            for name, v in (("e2e_tot", tot), ("legacy_dec_us", d["legacy_dec_us"]), ("arr2enq", e2e.get("arr2enq")),
                            ("qwait", e2e.get("qwait")), ("pre", e2e.get("pre")), ("dec_span", e2e.get("dec")),
                            ("serial", serial)):
                if v is not None:
                    self.g[name].add(v)
                    self.win.hist("dl", name).add(v)
        if self.lab:
            row = {"t_s": round(t_s, 6), "ext_slot": ext, "frame": d["frame"], "slot": d["slot"], "rnti": d["rnti"],
                   "dir": "dl", "outcome": outcome, "probe": bool(d["probe"]), "mcs": d["mcs"], "tbl": d["tbl"],
                   "qm": d["qm"], "nl": d["nl"], "rb_start": d["rb_start"], "rb_n": d["rb_n"], "nsym": d["nsym"],
                   "snr_db": d["snr"], "evm_pct": d["evm"], "rv": d["rv"], "ndi": d["ndi"], "harq_pid": d["pid"],
                   "tbs": d["tbs"], "nbrx": d["nbrx"], "early": es is not None,
                   "legacy_dec_us": int(d["legacy_dec_us"]), "e2e_tot_us": tot, "arr2enq_us": e2e.get("arr2enq"),
                   "qwait_us": e2e.get("qwait"), "pre_us": e2e.get("pre"), "dec_span_us": e2e.get("dec"),
                   "serial_us": serial, "first_body_us": e2e.get("first_body"), "bodies": e2e.get("bodies"),
                   "lag_slots": e2e.get("lag"), "es_lead_us": es["lead_us"] if es else None,
                   "fep_us": stg.get("fep"), "chest_us": stg.get("chest"), "demod_us": stg.get("demod"),
                   "ldpc_us": stg.get("ldpc"), "span_inconsistent": bad_span}
            self._row("rx_grants", row)

    def _window_grant(self, dir_, mcs, nl, ok, snr, e2e_tot, dec_us, rnti):
        w = self.win
        w.grants += 1
        w.ok += ok
        if len(w.ue) < UE_SET_MAX:
            w.ue.add(rnti)
        g = w.group((dir_, mcs_bucket(mcs), nl_bucket(nl)))
        g["n"] += 1
        g["ok"] += ok
        if snr is not None:
            g["snr"] += snr
            g["snr_n"] += 1
        if e2e_tot is not None:
            g["e2e"].add(e2e_tot)
        if dec_us is not None:
            g["dec"].add(dec_us)

    def _on_ul_crc(self, d, t_s):
        self.c["grants"] += 1
        self.c["grants_ul"] += 1
        self.c["crc_ok" if d["ok"] else "crc_fail"] += 1
        self._window_grant("ul", d["mcs"], None, d["ok"], d["snr"], None, None, d["rnti"])

    def _on_dci_seen(self, d, t_s):
        ext = self.unwrap(d["frame"], d["slot"])
        self.c["dci_accepts"] += 1
        self.win.accepts += 1
        if len(self.win.ue) < UE_SET_MAX:
            self.win.ue.add(d["rnti"])
        self._mark("first_dci", t_s)
        if self.lab:
            if self.c["dci_accepts"] == 1:
                self._ev("first_dci", t_s, rnti=d["rnti"])
            self._row("dci_attempts", {"t_s": round(t_s, 6), "ext_slot": ext, "frame": d["frame"], "slot": d["slot"],
                                       "rnti": d["rnti"], "dir": d["dir"], "fmt": _enum(d["fmt"], "fmt"), "cce": d["cce"],
                                       "al": d["al"], "utc_ns": d["utc_ns"]})

    def _ev(self, event, t_s, **kw):
        row = {"t_s": round(t_s, 6), "event": event, "pci": self.pci}
        row.update(kw)
        self._row("discovery_events", row)

    def _on_life(self, d, t_s):
        ev = d["event"]
        self._mark(ev, t_s)
        if ev == "acq_state":
            self.acq_state = _enum(d["state_to"], "acq_state")
        if self.lab:
            kw = {k: d[k] for k in ("rnti", "ival", "ival2") if k in d}
            if ev == "acq_state":
                kw["state_from"], kw["state_to"] = _enum(d["state_from"], "acq_state"), self.acq_state
            if ev == "sync_ok":
                kw["pci"] = d["pci"]
            self._ev(ev, t_s, **kw)
        if ev == "sync_ok":
            self.pci = d["pci"]

    def _on_config(self, d, t_s):
        self.c["config_" + d["kind"]] += 1
        if self.lab:
            row = {"t_s": round(t_s, 6), "kind": d["kind"]}
            if d["kind"] == "epoch_bump":
                row.update(epoch_from=d["epoch_from"], epoch_to=d["epoch_to"], cls=_enum(d["cls"], "cfg_class"),
                           cause=_enum(d["cause"], "cfg_cause"))
            else:
                row.update(gap_samples=d["gap_samples"], gap_ms=d["gap_ms"])
            self._row("configuration_events", row)

    def _on_health(self, d, t_s):
        self.c["health_" + d["kind"]] += 1
        if self.lab:
            row = {"t_s": round(t_s, 6)}
            row.update(d)
            self._row("slot_health", row)

    def _on_retry(self, d, t_s):
        kind = d["kind"]
        self.c["retry_" + kind] += 1
        attributed, orig = False, None
        if kind == "recovlate":
            key = (d["rnti"], d["frame"], d["slot"], d["harq_pid"])
            if key in self.rescued:
                self.c["retry_dup"] += 1  # same grant reported twice: counted once
            elif key in self.pend:
                orig = self.pend.pop(key)
                self.rescued.add(key)
                if len(self.rescued) > PEND_MAX:
                    self.rescued.pop()
                self.c["rescued"] += 1
                attributed = True
            else:
                self.c["retry_orphan"] += 1  # original failure unseen/aged out: not added to any success count
        if self.lab:
            row = {"t_s": round(t_s, 6), "kind": kind, "attributed": attributed}
            row.update({k: v for k, v in d.items() if k != "kind"})
            if orig is not None:
                row["orig_seq"] = orig
            if "frame" in d:
                row["ext_slot"] = self.unwrap(d["frame"], d["slot"])
            self._row("retry_events", row)

    def _on_pdtimcls(self, d, t_s):
        if "real" in d:
            self.pdtim_real = d["real"]  # cumulative since receiver start; kept as reported (legacy_pdtim_real)

    def _on_metrics(self, m, t_s):
        self.c["metrics"] += 1
        self.last_metrics = {k: m[k] for k in ("pdschq_queued", "pdschq_decoded", "pdschq_crc_ok", "scanq_queued",
                                              "scanq_drop_full", "scanq_drop_stale", "pdschq_drop_full",
                                              "pdschq_drop_stale") if isinstance(m.get(k), (int, float))}
        if isinstance(m.get("pci"), int) and m["pci"] >= 0:
            self.pci = m["pci"]
        st = m.get("acq_state")
        if isinstance(st, str):
            self.acq_state = _enum(st, "acq_state")
        if isinstance(m.get("t_real_ns"), int) and isinstance(m.get("t_mono_ns"), int):
            self.cal.append(m["t_real_ns"] - m["t_mono_ns"])
        row = {"t_s": round(t_s, 6), "src": "metrics", "acq_state": self.acq_state}
        for k, v in m.items():
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                continue
            if k in policy.METRIC_INT or k in policy.METRIC_FLOAT or k.startswith(policy.DYN_PREFIXES):
                row[k] = v
        self.emit("system_telemetry", row)

    # ---------------------------------------------------------------- windows
    def _close_window(self):
        w, ws = self.win, (self.win_idx or 0) * policy.WINDOW_S
        self.win = _Win()
        # complementary suppression: discovery_summary publishes window totals, so a lone suppressed combo per dir would
        # be recoverable by subtraction; hide the smallest published combo of that dir too.
        small = {k for k, g in w.grp.items() if g["n"] < self.k}
        for d in {k[0] for k in small}:
            if sum(1 for k in small if k[0] == d) == 1:
                shown = [k for k in w.grp if k[0] == d and k not in small]
                if shown:
                    small.add(min(shown, key=lambda k: (w.grp[k]["n"], k)))
        for (dir_, mb, nb), g in sorted(w.grp.items()):
            if (dir_, mb, nb) in small:
                self.c["suppressed_rows"] += 1
                self.c["suppressed_events"] += g["n"]
                continue
            self.emit("cell_aggregates", {
                "window_start_s": ws, "pci": self.pci, "dir": dir_, "mcs_bucket": mb, "nl_bucket": nb, "n_grants": g["n"],
                "n_crc_ok": g["ok"], "n_crc_fail": g["n"] - g["ok"],
                "snr_db_mean": g["snr"] / g["snr_n"] if g["snr_n"] else None,
                "e2e_tot_p50_us": g["e2e"].quantile(0.5), "e2e_tot_p99_us": g["e2e"].quantile(0.99),
                "legacy_dec_p50_us": g["dec"].quantile(0.5), "legacy_dec_p99_us": g["dec"].quantile(0.99)})
        for (dir_, metric), h in sorted(w.lat.items()):
            if h.n < self.k:
                self.c["suppressed_rows"] += 1
                self.c["suppressed_events"] += h.n
                continue
            for lo, hi, c in h.to_rows():
                self.emit("latency_histograms", {"window_start_s": ws, "pci": self.pci, "dir": dir_, "metric": metric,
                                                 "bucket_lo_us": lo, "bucket_hi_us": hi, "count": c})
        if w.accepts or w.grants:
            if w.accepts < self.k and w.grants < self.k:
                self.c["suppressed_rows"] += 1
                self.c["suppressed_events"] += w.accepts + w.grants
            else:
                self.emit("discovery_summary", {
                    "window_start_s": ws, "pci": self.pci, "acq_state": self.acq_state or "UNKNOWN",
                    "ue_bucket": ue_bucket(len(w.ue)), "n_dci_accepts": w.accepts, "n_grants": w.grants, "n_crc_ok": w.ok})

    # ---------------------------------------------------------------- summary
    def summary(self):
        c = self.c
        g = c["grants"]
        ok_final = c["crc_ok"] + c["rescued"]
        shown = g >= self.k
        lat = {n: ({"n": h.n, "p50": h.quantile(0.5), "p90": h.quantile(0.9), "p99": h.quantile(0.99)}
                   if h.n >= self.k else {"n": None, "p50": None, "p90": None, "p99": None}) for n, h in self.g.items()}
        first_ok = self.milestones.get("first_crc_ok")
        cal = sorted(self.cal)
        return {
            "counts": dict(c), "grants": g, "crc_ok": c["crc_ok"], "crc_fail": c["crc_fail"], "rescued_unique": c["rescued"],
            "crc_ok_after_retry": ok_final,
            "crc_ok_rate": (c["crc_ok"] / g) if shown else None,
            "crc_ok_rate_after_retry": (ok_final / g) if shown else None,
            "latency_us": lat,
            "latency_defs": {"e2e_tot": "EVMGRANT e2e tot = slot arrival -> decode end",
                             "legacy_dec_us": "EVMGRANT dec_us = decode call start -> end",
                             "legacy_pdtim_real": "PDTIMCLS real = sum of FEP+CHEST+DEMOD+LDPC wall of a grant's first body, cumulative since receiver start",
                             "serial": "arr2enq+qwait+pre+dec; first_body and stage walls are nested inside dec and never added"},
            "legacy_pdtim_real": self.pdtim_real,
            "cold_start": {"attempts": 1, "observed_from_start": "sync_start" in self.milestones,
                           "success": first_ok is not None, "time_to_first_crc_ok_s": first_ok,
                           "milestones_s": dict(self.milestones),
                           "note": "t_s = exporter arrival time of the line, not event time"},
            "clock_offset_real_minus_mono_ns": cal[len(cal) // 2] if cal else None,
            "last_metrics": self.last_metrics,
        }
