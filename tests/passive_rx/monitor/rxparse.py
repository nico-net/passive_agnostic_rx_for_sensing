#!/usr/bin/env python3
"""Pure parsing and aggregation for the receiver dashboard (no I/O, no threads: unit-testable).

RunState ingests the four things the receiver writes and keeps the aggregates every dashboard page needs:
  feed_line(line, t)   run.log lines (stable prefixes, PROJECT_MEMORY §11); t = run-relative seconds or None
  feed_metrics(obj)    ISAC_METRICS schema-1 JSON objects (metrics.jsonl or the "SENSING: ISAC_METRICS {...}" log line)
  feed_obs(obj)        per-grant observation JSON objects (obs.jsonl, schema 1, §21)
  feed_sense(obj)      sensing export schema-2 records (session / grant / rs)
  feed_cfr(frame)      one decoded NRCF v1 CFR frame header + downsampled IQ (see monitor.py)
snapshot(thr, host) returns one JSON-able dict; tiles(), alerts() and summary_md() derive from it.

Every KPI keeps its source so the page can say where a number comes from. Unknown stays None, never 0.
The list of log prefixes and JSON keys used here is mirrored in docs/superpowers/reports/csd/DASHBOARD-*.md.
"""
import json
import math
import re
from collections import deque

# --------------------------------------------------------------------------- constants

MCS_BUCKETS = (("0-4", 0, 4), ("5-9", 5, 9), ("10-16", 10, 16), ("17-27", 17, 27))
QM_NAME = {1: "pi/2-BPSK", 2: "QPSK", 4: "16QAM", 6: "64QAM", 8: "256QAM", 10: "1024QAM"}
LAT_BOUNDS_US = (100, 250, 500, 1000, 2000, 4000, 8000, 16000)  # same bins as metrics pdschq_latency_us_bounds
SYNC_STAGES = ("Searching", "PSS/SSS", "MIB", "SIB1", "CORESET", "RNTIs", "DL converged", "UL locked")
SYNC_HELP = ("looking for a cell (PSS correlation over the GSCN raster)",
             "a cell's sync signals were found (PCI known)",
             "the broadcast channel decoded (PBCH CRC, frame timing known)",
             "the system information block decoded (operator, carrier, TDD)",
             "a control-channel region (CORESET) is verified",
             "user identities (RNTIs) are being recovered from control messages",
             "downlink data decodes with CRC OK for at least one RNTI",
             "uplink layout locked (PUSCH can be interpreted)")
DEFAULT_THRESHOLDS = {
    "p99_target_us": 500.0,    # decode-time target (PROJECT_MEMORY merge gate: decode < 0.5 ms)
    "stale_pct": 10.0,          # PDSCH queue stale drops, % of queued
    "drop_full_pct": 2.5,       # scan-queue full drops (4-RX gate, CLAUDE.md)
    "crc_warn_pct": 80.0,       # DL CRC-OK below this = amber
    "crc_bad_pct": 50.0,        # below this = red
    "gpu_budget_pct": 95.0,     # GPU discovery budget used
    "silent_s": 60.0,           # live receiver silent for more than this
    "ram_floor_gb": 16.0,       # MemAvailable floor (host rule, PROJECT_MEMORY)
    "temp_c": 80.0,             # thermal gate (heavy.sh)
}
EVENT_CAP = 800
NEW_RNTI_EVENTS = 40          # first N new RNTIs become timeline events; later ones are only counted
RNTI_CAP = 4096               # per cell and direction; later RNTIs go to "other"
SERIES_CAP = 4000


def _num(v):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) else None


def _pct(a, b):
    return 100.0 * a / b if a is not None and b else None


def _int(s, default=None):
    try:
        return int(s, 0) if isinstance(s, str) and s.lower().startswith("0x") else int(s)
    except (TypeError, ValueError):
        return default


def mcs_bucket(mcs):
    if mcs is None:
        return None
    for name, lo, hi in MCS_BUCKETS:
        if lo <= mcs <= hi:
            return name
    return None


def hist_percentiles(counts, bounds=LAT_BOUNDS_US, qs=(0.5, 0.9, 0.99)):
    """Upper-bound percentiles of a bounded histogram (last bin = above the last bound -> None = '>bound')."""
    n = sum(counts)
    out = []
    for q in qs:
        if not n:
            out.append(None)
            continue
        c = 0
        val = None
        for i, k in enumerate(counts):
            c += k
            if c >= q * n:
                val = bounds[i] if i < len(bounds) else float("inf")
                break
        out.append(val)
    return out


def exact_percentiles(vals, qs=(0.5, 0.9, 0.99)):
    v = sorted(x for x in vals if x is not None)
    if not v:
        return [None] * len(qs)
    return [v[min(len(v) - 1, int(round(q * (len(v) - 1))))] for q in qs]


def freq_mhz(x):
    """Frequencies arrive in Hz, kHz or MHz depending on the line; normalise to MHz."""
    x = _num(x)
    if x is None or x <= 0:
        return None
    if x > 1e8:
        return x / 1e6
    if x > 1e5:
        return x / 1e3
    return x


# --------------------------------------------------------------------------- per-grant aggregation

class GrantAgg:
    """Grants of one direction of one cell from one source (obs.jsonl or EVMGRANT log lines)."""

    def __init__(self):
        self.n = self.ok = 0
        self.by_mcs = {b[0]: [0, 0] for b in MCS_BUCKETS}
        self.by_nl = {}
        self.mcs_hist = [0] * 32
        self.qm = {}
        self.rnti = {}
        self.other = [0, 0]
        self.recent = deque(maxlen=4000)   # (t, ok, tbs_bits) for windowed throughput
        self.dec_us = deque(maxlen=4000)   # EVMGRANT dec_us, for exact recent percentiles
        self.lat_hist = [0] * (len(LAT_BOUNDS_US) + 1)
        self.mcs_tables = {}
        self.dmrs_ids = {}
        self.first_t = self.last_t = None

    def add(self, t, rnti, mcs=None, nl=None, nb_rb=None, tbs=None, crc=None, qm=None, snr=None, dec_us=None,
            mcs_table=None, dmrs_id=None):
        ok = 1 if crc else 0
        self.n += 1
        self.ok += ok
        if t is not None:
            self.first_t = t if self.first_t is None else min(self.first_t, t)
            self.last_t = t if self.last_t is None else max(self.last_t, t)
            self.recent.append((t, ok, (tbs or 0) * ok))
        b = mcs_bucket(mcs)
        if b:
            self.by_mcs[b][0] += ok
            self.by_mcs[b][1] += 1
        if mcs is not None and 0 <= mcs < 32:
            self.mcs_hist[mcs] += 1
        if nl:
            s = self.by_nl.setdefault(int(nl), [0, 0])
            s[0] += ok
            s[1] += 1
        if qm:
            self.qm[qm] = self.qm.get(qm, 0) + 1
        if mcs_table is not None and ok:
            self.mcs_tables[mcs_table] = self.mcs_tables.get(mcs_table, 0) + 1
        if dmrs_id is not None and ok:
            self.dmrs_ids[dmrs_id] = self.dmrs_ids.get(dmrs_id, 0) + 1
        if dec_us is not None:
            self.dec_us.append(dec_us)
            i = 0
            while i < len(LAT_BOUNDS_US) and dec_us > LAT_BOUNDS_US[i]:
                i += 1
            self.lat_hist[i] += 1
        if rnti is None:
            return False
        r = self.rnti.get(rnti)
        new = r is None
        if new:
            if len(self.rnti) >= RNTI_CAP:
                self.other[0] += ok
                self.other[1] += 1
                return False
            r = self.rnti[rnti] = {"n": 0, "ok": 0, "mcs": [0] * 4, "nl": [0] * 5, "prb_sum": 0, "prb_n": 0,
                                   "bits_ok": 0, "qm": {}, "first_t": t, "last_t": t, "snr_sum": 0.0, "snr_n": 0}
        r["n"] += 1
        r["ok"] += ok
        if b:
            r["mcs"][[x[0] for x in MCS_BUCKETS].index(b)] += 1
        if nl and 0 < nl <= 4:
            r["nl"][int(nl)] += 1
        if nb_rb is not None:
            r["prb_sum"] += nb_rb
            r["prb_n"] += 1
        r["bits_ok"] += (tbs or 0) * ok
        if qm:
            r["qm"][qm] = r["qm"].get(qm, 0) + 1
        if snr is not None:
            r["snr_sum"] += snr
            r["snr_n"] += 1
        if t is not None:
            r["last_t"] = t
            if r["first_t"] is None:
                r["first_t"] = t
        return new

    def window_tput(self, now_t, span=20.0):
        """bits/s of CRC-OK TBS over the last `span` seconds of run time, and grants/s."""
        if now_t is None or not self.recent:
            return None, None
        lo = now_t - span
        bits = sum(x[2] for x in self.recent if x[0] >= lo)
        n = sum(1 for x in self.recent if x[0] >= lo)
        oldest = max(lo, self.recent[0][0])
        dt = max(now_t - oldest, 1.0)
        return bits / dt, n / dt

    def snap(self, now_t, top=200):
        span = (self.last_t - self.first_t) if (self.first_t is not None and self.last_t is not None) else None
        rows = []
        for rnti, r in sorted(self.rnti.items(), key=lambda kv: -kv[1]["n"])[:top]:
            rs = (r["last_t"] - r["first_t"]) if (r["first_t"] is not None and r["last_t"] is not None) else None
            qm = max(r["qm"].items(), key=lambda kv: kv[1])[0] if r["qm"] else None
            rows.append({
                "rnti": "0x%04x" % rnti, "grants": r["n"], "crc_ok": r["ok"], "crc_pct": _pct(r["ok"], r["n"]),
                "mcs_buckets": dict(zip([b[0] for b in MCS_BUCKETS], r["mcs"])),
                "layers": {str(i): r["nl"][i] for i in range(1, 5) if r["nl"][i]},
                "prb_mean": (r["prb_sum"] / r["prb_n"]) if r["prb_n"] else None,
                "modulation": QM_NAME.get(qm, str(qm) if qm else None),
                "snr_db": (r["snr_sum"] / r["snr_n"]) if r["snr_n"] else None,
                "tput_kbps": (r["bits_ok"] / rs / 1e3) if rs and rs > 0.5 else None,
                "first_t": r["first_t"], "last_t": r["last_t"]})
        bps, gps = self.window_tput(now_t if now_t is not None else self.last_t)
        p = exact_percentiles(self.dec_us)
        return {
            "grants": self.n, "crc_ok": self.ok, "crc_pct": _pct(self.ok, self.n), "rnti_count": len(self.rnti),
            "rnti_other": {"grants": self.other[1], "crc_ok": self.other[0]},
            "by_mcs": {k: {"ok": v[0], "n": v[1], "pct": _pct(v[0], v[1])} for k, v in self.by_mcs.items()},
            "by_nl": {str(k): {"ok": v[0], "n": v[1], "pct": _pct(v[0], v[1])} for k, v in sorted(self.by_nl.items())},
            "mcs_hist": self.mcs_hist, "modulation": {QM_NAME.get(k, str(k)): v for k, v in sorted(self.qm.items())},
            "mcs_tables": {str(k): v for k, v in self.mcs_tables.items()},
            "dmrs_ids": {str(k): v for k, v in sorted(self.dmrs_ids.items(), key=lambda kv: -kv[1])[:16]},
            "tput_bps_recent": bps, "grants_per_s_recent": gps,
            "tput_bps_run": (sum(r["bits_ok"] for r in self.rnti.values()) / span) if span and span > 0.5 else None,
            "dec_us": {"n": len(self.dec_us), "p50": p[0], "p90": p[1], "p99": p[2],
                       "max": max(self.dec_us) if self.dec_us else None, "hist": self.lat_hist,
                       "bounds": LAT_BOUNDS_US},
            "rntis": rows, "span_s": span}


# --------------------------------------------------------------------------- log line regexes

RX = {
    "sync": re.compile(r"Initial sync successful, PCI: (\d+)"),
    "cell_det": re.compile(r"Cell Detected with GSCN: (\d+), SSB SC offset: (\d+), SSB Ref: ([\d.]+)"
                           r"(?:, PSS Corr peak: (-?\d+) dB, PSS Corr Average: (-?\d+))?"),
    "cfo": re.compile(r"Measured Carrier Frequency offset (-?\d+) Hz"),
    "mib": re.compile(r"MIB fields sfn=(\d+) k_ssb=(\d+)(.*?) scs_common=(\d+) coreset0=(\d+) ss0=(\d+) "
                      r"dmrs_typeA=(\d+) barred=(\d+)"),
    "sib1_cell": re.compile(r"SIB1 CELL mcc=(\d+) mnc=(\d+) cell_identity=(0x[0-9a-fA-F]+) tac=(-?\d+) n_plmn=(\d+)"),
    "sib1_cfg": re.compile(r"cell config extracted from SIB1 -- PCI=(\d+) DL: (\d+) PRB @ SCS=(\d+) "
                           r"offsetToPointA=(-?\d+) UL: (\d+) PRB @ SCS=(\d+) absoluteFrequencyPointA=(-?\d+)"),
    "carrier_ok": re.compile(r"ACQ carrier CONFIRMED from SIB1: (\d+) PRB mu=(\d+), Point A at grid subcarrier (-?\d+), "
                             r"carrier ends at (-?\d+).*?derived carrier centre ([\d.]+) MHz"),
    "carrier_nosib": re.compile(r"ACQ carrier CONFIRMED without SIB1 carrier info: (\d+) PRB mu=(\d+).*?centre ([\d.]+) MHz"),
    "carrier_bad": re.compile(r"ACQ carrier MISMATCH: SIB1 says (\d+) PRB mu=(\d+) \(Point A at grid subcarrier (-?\d+)"),
    "carrier_hyp": re.compile(r"ACQ carrier HYPOTHESIS without SIB1 carrier info: Point A at grid subcarrier (-?\d+)"),
    "retune": re.compile(r"ISAC_ACQ_RETUNE (\{[^}]*\})"),
    "tdd": re.compile(r"TDD from SIB1 (\w+)(?:: p1 period=(\d+) slots dl=(\d+)\+(\d+)sym ul=(\d+)\+(\d+)sym)?"),
    "css0": re.compile(r"CSS0 autoconf from MIB/SIB1 -- coreset\(groups=(\d+) dur=(\d+) .*?shift=(\d+) scramb=(\d+)\) "
                       r"bwp=\[(\d+)\.\.(\d+)\).*?ss\(period=(\d+) offset=(\d+) dur=(\d+) symb=(\d+)\)"),
    "bank_add": re.compile(r"multi-CORESET bank add index=(\d+) offset=(-?\d+) span=(\d+) symbol=(\d+) "
                           r"mapping=(\d+)/(\d+)/(\d+) len=(\d+)"),
    "bank_stale": re.compile(r"CORESET bank STALE index=(\d+)"),
    "bank_removed": re.compile(r"CORESET bank REMOVED index=(\d+)"),
    "ss_inf": re.compile(r"SEARCH_SPACE INFERRED \[([^\]]+)\].*?monitored AL=\{([^}]*)\}.*?total_confirmed=(\d+)"),
    "scramb": re.compile(r"PDCCH_SCRAMBLING_ID CONFIRMED \[([^\]]+)\] n_id=(\d+)"),
    "dci11_len": re.compile(r"DCI 1_1 length locked coreset=(\S+) rnti=(0x[0-9a-f]+) len=(\d+)"),
    "dci01_len": re.compile(r"UL automatic DCI length locked: (\d+) rnti=(0x[0-9a-f]+)"),
    "csirs_conf": re.compile(r'CSIRS_BLIND CONFIRMED #(\d+) after (\d+) slots -- csirs_monitor = "([^"]+)"'),
    "acq_state": re.compile(r"ACQ_STATE (\w+) -> (\w+)"),
    "epoch": re.compile(r"CONFIG_EPOCH (\d+) -> (\d+) class=(\S+) cause=(\S+)"),
    "summary": re.compile(r"monitor summary: occasions=(\d+) candidates=(\d+) accepts=(\d+) "
                          r"dci10\[accepts=(\d+) C=(\d+) TC=(\d+) SI=(\d+) RA=(\d+) P=(\d+)\] "
                          r"dci01\[accepts=(\d+) rejects=(\d+)\] dci00\[accepts=(\d+) rejects=(\d+)\]"
                          r"(?:.*?held\[energy=(\d+) dmrs=(\d+) persist=(\d+) snr=(\d+) mismatch=(\d+) rnti_set=(\d+)\])?"
                          r"(?:.*?scanq\[queued=(\d+) done=(\d+) drop_full=(\d+) drop_stale=(\d+) maxlag=(\d+)\])?"),
    "pdschq": re.compile(r"PDSCHQ queued=(\d+) decoded=(\d+) crc_ok=(\d+).*?dropped\[full=(\d+) stale=(\d+)\] "
                         r"max_lag_slots=(\d+)/(\d+)"),
    "puschq": re.compile(r"PUSCHQ queued=(\d+) decoded=(\d+) crc_ok=(\d+) dropped\[full=(\d+) stale=(\d+)"),
    "perrnti": re.compile(r"0x([0-9a-f]+):(\d+)/(\d+)"),
    "ldpc": re.compile(r"LDPCDIAG ok=(\d+) seg_fail=(\d+) tb_fail=(\d+) zero_tb=(\d+)"),
    "pusch": re.compile(r"pusch_passive\[try=(\d+) crc_ok=(\d+).*?uci\[trials=(\d+) rescued=(\d+)"),
    "chpow": re.compile(r"ch(\d+)=(-?[\d.]+)"),
    "rfchan": re.compile(r"RFCHAN ch(\d+) subdev=\S+ port=(\S+) gain=([\d.]+) .*?freq=([\d.]+) MHz rate=([\d.]+) Msps"),
    "rfcensus": re.compile(r"RFCENSUS slots=(\d+) ssb_slots=(\d+) pbch_ok=(\d+) pbch_fail=(\d+)"),
    "pdtim_stage": re.compile(r"(\w+)\[n=(\d+) mean=([\d.]+)us cpu=([\d.]+)us max=([\d.]+)us"),
    "pdtimpct": re.compile(r"(\w+)\[n=(\d+) p50=(\d+) p90=(\d+) p99=(\d+)\]"),
    "pdtimcls": re.compile(r"(real|retry|probe)\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) max=([\d.]+)"
                           r" gt5ms=(\d+) gt20ms=(\d+)"),
    "evm": re.compile(r"EVMGRANT rnti=0x([0-9a-f]+) (\d+)\.(\d+) crc=(\d) mcs=(\d+) tbl=(\d+) Qm=(\d+) Nl=(\d+) "
                      r"rb=(\d+)\+(\d+) .*?snr=(-?[\d.]+)"),
    "p99age": re.compile(r"P99AGE slots\(([^)]*)\) pdsch_enq_real=\[([\d,]*)\] pdsch_enq_probe=\[([\d,]*)\] scan_deq=\[([\d,]*)\] "
                         r"scanq_fresh=(\d+) retry_chains=(\d+) retry_yields=(\d+) retry_gated=(\d+)"),
    "evm_tail": re.compile(r"dec_us=(\d+).*?tbs=(\d+)"),
    "ul_lock": re.compile(r"ULLAYOUT (?:LOCK|BWP_LOCK) (?:len=(\d+) )?bwp=(\d+)@(\d+)"),
    "ul_crc_lock": re.compile(r"ULLAYOUT_CRC_LOCK( RELOCK)? len=(\d+) bwp=(\d+)@(\d+).*?crc_ok=(\d+) trials=(\d+)"),
    "ul_prior": re.compile(r"ULLAYOUT PRIOR_(CONFIRM|ADOPT|DEMOTE|EXHAUSTED)(?: bwp=(\d+)@(\d+))?"),
    "ul_stats": re.compile(r"ULLAYOUT STATS bwp=(searching|PROVISIONAL|LOCKED) (\d+)@(\d+) probes\[booked=(\d+) "
                           r"fed=(\d+)\] votes=(\d+).*?hyps=(-?\d+)"),
    "ul_reset": re.compile(r"ULLAYOUT reset: (.*)"),
    "td_conv": re.compile(r"Technique D CONVERGED rnti=(?:0x)?([0-9a-f]+) tda=(\d+) S=(\d+) L=(\d+) mask=(0x[0-9a-f]+) table=(\d+)"),
    "new_ctx": re.compile(r"SWEEP: new per-RNTI context rnti=0x([0-9a-f]+)"),
    "cmdline": re.compile(r"CMDLINE: (.*)"),
    "rfstall": re.compile(r"RFSTALL (.*)"),
}
CMD_TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"')


def classify_ssname(name):
    return "dedicated" if "USS" in name or "dedicated" in name.lower() else "common"


# --------------------------------------------------------------------------- run state

class RunState:
    """Everything learned from one run. Single-threaded: the caller serialises feed_* and snapshot()."""

    def __init__(self):
        self.lines = 0
        self.events = deque(maxlen=EVENT_CAP)
        self.events_dropped = 0
        self.t_last = None
        # radio
        self.chpow = {}
        self.chpow_series = deque(maxlen=600)
        self.rfchan = {}
        self.rfstall = []
        self.rfcensus = None
        # sync / acquisition
        self.acq_state = None
        self.acq_path = []
        self.stage_done = [None] * len(SYNC_STAGES)    # t (or True) when reached since the last loss
        self.stage_done[0] = True
        self.sync_losses = 0
        self.sync_count = 0
        # cells
        self.cells = {}
        self.cur_pci = None
        self._pending = {}
        self.tuned_mhz = None
        self.scan_cells = []
        # dl/ul
        self.pdcch = {}
        self.pdschq = {}
        self.pdschq_rnti = {}
        self.puschq = {}
        self.ldpc = {}
        self.pusch = {}
        self.td_conv = {}
        self.new_ctx = 0
        self.grants = {"obs": {}, "log": {}}   # src -> pci -> dir -> GrantAgg
        self.rnti_seen = set()
        self.new_rnti_events = 0
        self.ul = {"state": None, "bwp": None, "how": None, "line": None, "dci01_len": None, "stats": None}
        # timing
        self.pdtim = {}
        self.pdtimpct = {}
        self.pdtimcls = {}
        self.p99age = {}
        self.lat_series = deque(maxlen=SERIES_CAP)
        # metrics
        self.metrics_last = None
        self.metrics_prev = None
        self.t0_ns = None          # run clock origin (CLOCK_MONOTONIC ns); the reader may preset it from the log
        self.metrics_seen = set()
        self.series = deque(maxlen=SERIES_CAP)
        self.metrics_n = 0
        # sensing export
        self.sense = {"records": 0, "by_type": {}, "by_source": {}, "first_t": None, "last_t": None,
                      "session": None, "srs": 0, "prach": 0, "bad": 0}
        self.cfr = {"frames": 0, "by_source": {}, "last": {}, "heat": {}, "gaps": 0, "last_id": None}
        # configuration
        self.cmdline = None
        self.env = None
        self.cfg_path = None

    # ---------------- helpers
    def _event(self, t, kind, text, sev="info"):
        if len(self.events) == self.events.maxlen:
            self.events_dropped += 1
        self.events.append({"t": t, "kind": kind, "sev": sev, "text": text})

    def _cell(self, pci=None):
        pci = self.cur_pci if pci is None else pci
        key = str(pci) if pci is not None else "?"
        c = self.cells.get(key)
        if c is None:
            c = self.cells[key] = {"pci": pci, "facts": {}, "coresets": {}, "search_spaces": {}, "scrambling": {},
                                   "csirs": {}, "dci_len": {}, "first_t": None}
        return c

    def _fact(self, name, value, how, state="confirmed", t=None, pci=None):
        if pci is None and self.cur_pci is None:
            self._pending[name] = (name, value, how, state)   # learned before the PCI: attached at sync
            return
        c = self._cell(pci)
        c["facts"][name] = {"value": value, "how": how, "state": state, "t": t}

    def _stage(self, i, t):
        if self.stage_done[i] is None:
            self.stage_done[i] = t if t is not None else True

    def _rel(self, tm):
        """Run-relative seconds of a t_mono_ns value; the first value seen sets the origin if none was preset."""
        if tm is None:
            return None
        if self.t0_ns is None:
            self.t0_ns = tm
        t = (tm - self.t0_ns) / 1e9
        if self.t_last is None or t > self.t_last:
            self.t_last = t
        return t

    def _agg(self, src, pci, d):
        return self.grants[src].setdefault(str(pci) if pci is not None else "?", {}).setdefault(d, GrantAgg())

    # ---------------- log lines
    def feed_line(self, line, t=None):
        self.lines += 1
        if t is not None:
            self.t_last = t
        i = line.find("SENSING: ")
        if i >= 0:
            rest = line[i + 9:]
            tok = rest.split(" ", 1)[0].split("[", 1)[0]
            h = _SENSING.get(tok)
            if h:
                h(self, rest, t)
            return
        for key, h in _PLAIN:
            if key in line:
                h(self, line, t)
                return

    # SENSING handlers ---------------------------------------------------------
    def _h_metrics(self, rest, t):
        j = rest.find("{")
        if j < 0:
            return
        try:
            obj = json.loads(rest[j:])
        except ValueError:
            return
        if isinstance(obj, dict):
            self.feed_metrics(obj)

    def _h_chpow(self, rest, t):
        vals = {"ch" + a: float(b) for a, b in RX["chpow"].findall(rest)}
        if vals:
            self.chpow = vals
            self.chpow_series.append({"t": t, **vals})

    def _h_rfstall(self, rest, t):
        m = RX["rfstall"].search(rest)
        txt = (m.group(1) if m else rest).strip()[:200]
        self.rfstall.append({"t": t, "text": txt})
        if len(self.rfstall) > 200:
            del self.rfstall[0]
        self._event(t, "rfstall", "RF stall: " + txt, "bad")

    def _h_rfcensus(self, rest, t):
        m = RX["rfcensus"].search(rest)
        if m:
            self.rfcensus = {"slots": int(m[1]), "ssb_slots": int(m[2]), "pbch_ok": int(m[3]), "pbch_fail": int(m[4]), "t": t}

    def _h_acq(self, rest, t):
        if rest.startswith("ACQ_STATE"):
            m = RX["acq_state"].search(rest)
            if not m:
                return
            old, new = m[1], m[2]
            self.acq_state = new
            if not self.acq_path or self.acq_path[-1] != new:
                self.acq_path = (self.acq_path + [new])[-16:]
            if new == "LOST":
                self.sync_losses += 1
                self.stage_done = [True] + [None] * (len(SYNC_STAGES) - 1)
                self._event(t, "sync_lost", "Sync lost (%s -> LOST)" % old, "bad")
            else:
                self._event(t, "state", "Acquisition state %s -> %s" % (old, new))
                idx = {"PBCH_LOCKED": 2, "SIB1_DECODED": 3, "CORESET_VERIFIED": 4, "DL_CONVERGED": 6,
                       "UL_CONVERGED": 7}.get(new)
                if idx:
                    self._stage(idx, t)
        elif rest.startswith("ACQ_EVENT pbch_locked"):
            self._stage(2, t)
        elif rest.startswith("ACQ carrier"):
            m = RX["carrier_ok"].search(rest)
            if m:
                self._fact("carrier_nrb", int(m[1]), "SIB1 (carrier CONFIRMED)", t=t)
                self._fact("carrier_centre_mhz", float(m[5]), "SIB1 (derived carrier centre)", t=t)
                self._fact("point_a_sc", int(m[3]), "SIB1 offsetToPointA", t=t)
                self._fact("carrier_end_sc", int(m[4]), "SIB1 carrier width", t=t)
                self._fact("carrier_verdict", "CONFIRMED", "SIB1 vs receiver grid", t=t)
                return
            m = RX["carrier_nosib"].search(rest)
            if m:
                self._fact("carrier_nrb", int(m[1]), "receiver grid (no SIB1 carrier info)", "provisional", t)
                self._fact("carrier_centre_mhz", float(m[3]), "receiver grid", "provisional", t)
                self._fact("carrier_verdict", "CONFIRMED without SIB1", "grid assumption", "provisional", t)
                return
            m = RX["carrier_bad"].search(rest)
            if m:
                self._fact("carrier_nrb", int(m[1]), "SIB1 (MISMATCH with receiver grid)", t=t)
                self._fact("point_a_sc", int(m[3]), "SIB1 offsetToPointA", t=t)
                self._fact("carrier_verdict", "MISMATCH", "SIB1 vs receiver grid", "confirmed", t)
                self._event(t, "carrier", "Carrier MISMATCH: SIB1 says %s PRB, the receiver grid differs" % m[1], "warn")
                return
            m = RX["carrier_hyp"].search(rest)
            if m:
                self._fact("point_a_sc", int(m[1]), "hypothesis (no SIB1 carrier info)", "provisional", t)

    def _h_retune(self, rest, t):
        m = RX["retune"].search(rest)
        if m:
            self._event(t, "reconfig", "Retune requested " + m[1], "warn")

    def _h_mib(self, rest, t):
        m = RX["mib"].search(rest)
        if not m:
            return
        self._stage(2, t)
        self._fact("k_ssb", int(m[2]), "MIB", t=t)
        self._fact("scs_common_khz", (15, 30, 60, 120)[int(m[4])] if int(m[4]) < 4 else int(m[4]), "MIB", t=t)
        self._fact("coreset0_index", int(m[5]), "MIB pdcch-ConfigSIB1", t=t)
        self._fact("ss0_index", int(m[6]), "MIB pdcch-ConfigSIB1", t=t)
        self._fact("dmrs_typeA_pos", int(m[7]), "MIB", t=t)
        self._fact("cell_barred", bool(int(m[8])), "MIB", t=t)

    def _h_sib1(self, rest, t):
        m = RX["sib1_cell"].search(rest)
        if m:
            first = "plmn" not in self._cell()["facts"]
            self._stage(3, t)
            self._fact("plmn", "%s-%s" % (m[1], m[2]), "SIB1", t=t)
            self._fact("cell_identity", m[3], "SIB1", t=t)
            self._fact("tac", None if int(m[4]) < 0 else int(m[4]), "SIB1", "confirmed" if int(m[4]) >= 0 else "unknown", t)
            if first:
                self._event(t, "sib1", "SIB1 decoded: PLMN %s-%s, cell id %s" % (m[1], m[2], m[3]), "good")

    def _h_tdd(self, rest, t):
        m = RX["tdd"].search(rest)
        if not m:
            return
        if m[1] == "DERIVED" and m[2]:
            self._fact("tdd", "period %s slots: DL %s slots + %s sym, UL %s slots + %s sym" % (m[2], m[3], m[4], m[5], m[6]),
                       "SIB1 tdd-UL-DL-ConfigurationCommon", t=t)
        else:
            self._fact("tdd", m[1], "SIB1", "unknown" if m[1] != "DERIVED" else "confirmed", t)

    def _h_css0(self, rest, t):
        m = RX["css0"].search(rest)
        if m:
            self._stage(4, t)
            self._fact("coreset0", "%s RB groups, %s symbol(s), BWP RB %s..%s, scrambling %s" % (
                int(m[1]), m[2], m[5], int(m[6]) - 1, m[4]), "MIB (CORESET#0 table)", t=t)
            self._fact("ss0", "period %s slots, offset %s, duration %s, first symbol %s" % (m[7], m[8], m[9], m[10]),
                       "MIB (searchSpaceZero table)", t=t)

    def _h_bank(self, rest, t):
        m = RX["bank_add"].search(rest)
        c = self._cell()
        if m:
            self._stage(4, t)
            c["coresets"][m[1]] = {"index": int(m[1]), "rb_offset": int(m[2]), "span_rb": int(m[3]), "symbol": int(m[4]),
                                    "mapping": "%s/%s/%s" % (m[5], m[6], m[7]), "dci_len": int(m[8]),
                                    "state": "confirmed", "how": "blind discovery (verified by fresh dedicated DCIs)", "t": t}
            self._event(t, "coreset", "Dedicated CORESET #%s verified: RB %s +%s, DCI length %s" % (m[1], m[2], m[3], m[8]),
                        "good")
            return
        m = RX["bank_stale"].search(rest)
        if m and m[1] in c["coresets"]:
            c["coresets"][m[1]]["state"] = "provisional"
            c["coresets"][m[1]]["how"] += "; STALE (no recent DCIs)"
            self._event(t, "coreset", "CORESET #%s stale (no recent DCIs while traffic is seen elsewhere)" % m[1], "warn")
        m = RX["bank_removed"].search(rest)
        if m and m[1] in c["coresets"]:
            c["coresets"][m[1]]["state"] = "removed"
            self._event(t, "coreset", "CORESET #%s removed" % m[1], "warn")

    def _h_ss(self, rest, t):
        m = RX["ss_inf"].search(rest)
        if m:
            self._cell()["search_spaces"][m[1]] = {"al": m[2], "confirmed": int(m[3]), "t": t,
                                                   "how": "inferred from CRC-recovered grants (bounded, not exact)",
                                                   "state": "provisional"}

    def _h_scramb(self, rest, t):
        m = RX["scramb"].search(rest)
        if m:
            self._stage(5, t)
            self._cell()["scrambling"][m[1]] = {"n_id": int(m[2]), "how": "CRC-recovered RNTI (proof of the value in use)",
                                                "state": "confirmed", "t": t}
            self._event(t, "rnti", "PDCCH scrambling id %s confirmed [%s]" % (m[2], m[1]), "good")

    def _h_dci(self, rest, t):
        m = RX["dci11_len"].search(rest)
        if m:
            self._cell()["dci_len"]["DL 1_1 (CORESET %s)" % m[1]] = {"len": int(m[3]), "rnti": m[2], "t": t}

    def _h_csirs(self, rest, t):
        m = RX["csirs_conf"].search(rest)
        if m:
            self._cell()["csirs"][m[1]] = {"resource": m[3], "after_slots": int(m[2]), "t": t,
                                           "how": "blind CSI-RS search (CONFIRMED)", "state": "confirmed"}
            self._event(t, "csirs", "CSI-RS resource #%s confirmed after %s slots" % (m[1], m[2]), "good")

    def _h_epoch(self, rest, t):
        m = RX["epoch"].search(rest)
        if m:
            self._event(t, "reconfig", "Cell configuration epoch %s -> %s (%s, cause %s)" % (m[1], m[2], m[3], m[4]), "warn")

    def _h_blind(self, rest, t):
        m = RX["summary"].search(rest)
        if not m:
            return
        g = [None if x is None else int(x) for x in m.groups()]
        self.pdcch = {"occasions": g[0], "candidates": g[1], "accepts": g[2],
                      "dci10": {"accepts": g[3], "C": g[4], "TC": g[5], "SI": g[6], "RA": g[7], "P": g[8]},
                      "dci01": {"accepts": g[9], "rejects": g[10]}, "dci00": {"accepts": g[11], "rejects": g[12]},
                      "held": None if g[13] is None else dict(zip(("energy", "dmrs", "persist", "snr", "mismatch", "rnti_set"), g[13:19])),
                      "scanq": None if g[19] is None else dict(zip(("queued", "done", "drop_full", "drop_stale", "maxlag"), g[19:24])),
                      "t": t, "src": "log summary"}

    def _h_pdschq(self, rest, t):
        if rest.startswith("PDSCHQ per-rnti"):
            for r, ok, d in RX["perrnti"].findall(rest):
                self.pdschq_rnti[r] = {"ok": int(ok), "decoded": int(d)}
            return
        m = RX["pdschq"].search(rest)
        if m:
            q, d, ok, full, stale, lag, spf = map(int, m.groups())
            self.pdschq = {"queued": q, "decoded": d, "crc_ok": ok, "drop_full": full, "drop_stale": stale,
                           "max_lag": lag, "slots_per_frame": spf, "t": t}

    def _h_puschq(self, rest, t):
        m = RX["puschq"].search(rest)
        if m:
            q, d, ok, full, stale = map(int, m.groups())
            self.puschq = {"queued": q, "decoded": d, "crc_ok": ok, "drop_full": full, "drop_stale": stale, "t": t}

    def _h_ldpc(self, rest, t):
        m = RX["ldpc"].search(rest)
        if m:
            ok, sf, tf, zt = map(int, m.groups())
            self.ldpc = {"ok": ok, "seg_fail": sf, "tb_fail": tf, "zero_tb": zt, "tb_rate": _pct(ok, ok + sf)}

    def _h_pusch(self, rest, t):
        m = RX["pusch"].search(rest)
        if m:
            tr, ok, ut, ur = map(int, m.groups())
            self.pusch = {"try": tr, "crc_ok": ok, "uci_trials": ut, "uci_rescued": ur, "t": t}
            if ok:
                self._stage(7, t)

    def _h_pdtim(self, rest, t):
        if rest.startswith("PDTIMPCT"):
            d = {k: {"n": int(n), "p50": int(a), "p90": int(b), "p99": int(c)} for k, n, a, b, c in RX["pdtimpct"].findall(rest)}
            if d:
                self.pdtimpct = d
                s = d.get("sum")
                if s:
                    self.lat_series.append({"t": t, "p50": s["p50"], "p90": s["p90"], "p99": s["p99"], "src": "PDTIMPCT sum"})
        elif rest.startswith("PDTIMCLSST"):
            return
        elif rest.startswith("PDTIMCLS"):
            d = {}
            for k, n, mean, a, b, c, mx, g5, g20 in RX["pdtimcls"].findall(rest):
                d[k] = {"n": int(n), "mean": float(mean), "p50": int(a), "p90": int(b), "p99": int(c), "max": float(mx),
                        "gt5ms": int(g5), "gt20ms": int(g20)}
            if d:
                self.pdtimcls = d
                r = d.get("real")
                if r:
                    self.lat_series.append({"t": t, "p50": r["p50"], "p90": r["p90"], "p99": r["p99"], "max": r["max"],
                                            "src": "PDTIMCLS real"})
        elif rest.startswith("PDTIM "):
            d = {k: {"n": int(n), "mean": float(a), "cpu": float(b), "max": float(c)}
                 for k, n, a, b, c in RX["pdtim_stage"].findall(rest)}
            if d:
                self.pdtim = d

    def _h_p99age(self, rest, t):
        """P99AGE (nr_pdsch_passive_queue.c, ISAC_PDTIM_CLASS=1): cumulative job-age histograms in slots."""
        m = RX["p99age"].search(rest)
        if m:
            bins = m.group(1).split("/")
            h = {k: [int(x) for x in v.split(",") if x] for k, v in zip(("pdsch_enq_real", "pdsch_enq_probe", "scan_deq"), m.groups()[1:4])}
            self.p99age = dict(bins=bins, hist=h, scanq_fresh=int(m.group(5)),
                               retry_chains=int(m.group(6)), retry_yields=int(m.group(7)), retry_gated=int(m.group(8)), t=t)

    def _h_evm(self, rest, t):
        m = RX["evm"].search(rest)
        if not m:
            return
        tail = RX["evm_tail"].search(rest)
        rnti = int(m[1], 16)
        dec_us = float(tail[1]) if tail else None
        tbs = int(tail[2]) if tail else None
        self._agg("log", self.cur_pci, "DL").add(
            t, rnti, mcs=int(m[5]), nl=int(m[8]), nb_rb=int(m[10]), tbs=tbs, crc=int(m[4]), qm=int(m[7]),
            snr=float(m[11]), dec_us=dec_us, mcs_table=int(m[6]))
        self._note_rnti(rnti, int(m[4]), t, "EVMGRANT")

    def _h_ul(self, rest, t):
        if rest.startswith("ULLAYOUT_CRC_LOCK"):
            m = RX["ul_crc_lock"].search(rest)
            if m:
                self.ul.update(state="LOCKED", bwp="%s RB @ start %s" % (m[3], m[4]),
                               how="CRC lock (%s CRC-OK of %s trials)" % (m[5], m[6]), line=rest[:200])
                self._stage(7, t)
                self._event(t, "ul_lock", "UL layout CRC-locked: BWP %s@%s" % (m[3], m[4]), "good")
            return
        m = RX["ul_stats"].search(rest)
        if m:
            self.ul["stats"] = {"state": m[1], "bwp": "%s@%s" % (m[2], m[3]), "probes_booked": int(m[4]),
                                "probes_fed": int(m[5]), "votes": int(m[6]), "hyps": int(m[7]), "t": t}
            if m[1] != "searching" and self.ul["state"] != "LOCKED":
                self.ul.update(state=m[1], bwp="%s RB @ start %s" % (m[2], m[3]),
                               how="DM-RS energy votes (%s votes over %s hypotheses)" % (m[6], m[7]))
            return
        m = RX["ul_prior"].search(rest)
        if m:
            if m[1] == "CONFIRM":
                self.ul.update(state="LOCKED", bwp="%s RB @ start %s" % (m[2], m[3]), how="prior confirmed by PUSCH CRC",
                               line=rest[:200])
                self._stage(7, t)
            elif m[1] == "ADOPT" and self.ul["state"] != "LOCKED":
                self.ul.update(state="PROVISIONAL", bwp="%s RB @ start %s" % (m[2], m[3]),
                               how="prior adopted (SIB1/DL geometry), not yet CRC-confirmed", line=rest[:200])
            return
        m = RX["ul_reset"].search(rest)
        if m:
            self.ul.update(state=None, bwp=None, how=None, line=rest[:200])
            self._event(t, "reconfig", "UL layout reset: " + m[1][:120], "warn")
            return
        m = RX["ul_lock"].search(rest)
        if m:
            kind = "BWP lock" if "BWP_LOCK" in rest else "layout lock"
            self.ul.update(state="LOCKED", bwp="%s RB @ start %s" % (m[2], m[3]),
                           how="%s (DCI 0_1 statistics%s)" % (kind, ", DCI length %s" % m[1] if m[1] else ""),
                           line=rest[:200])
            self._stage(7, t)
            self._event(t, "ul_lock", "UL %s: BWP %s@%s" % (kind, m[2], m[3]), "good")

    def _h_td(self, rest, t):
        m = RX["td_conv"].search(rest)
        if m:
            r = "0x" + m[1]
            first = r not in self.td_conv
            self.td_conv[r] = {"tda": int(m[2]), "S": int(m[3]), "L": int(m[4]), "dmrs_mask": m[5], "mcs_table": int(m[6]),
                               "t": t, "pci": self.cur_pci}
            self._stage(6, t)
            if first:
                self._event(t, "dl_conv", "DL interpretation converged for RNTI %s (TDA %s, S=%s L=%s)" % (r, m[2], m[3], m[4]),
                            "good")

    # plain (non-SENSING) handlers ---------------------------------------------
    def _p_sync(self, line, t):
        m = RX["sync"].search(line)
        if not m:
            return
        pci = int(m[1])
        self.sync_count += 1
        self.cur_pci = pci
        c = self._cell(pci)
        if c["first_t"] is None:
            c["first_t"] = t
        self._stage(1, t)
        self._stage(2, t)
        self._fact("pci", pci, "PSS/SSS + PBCH CRC (initial sync)", t=t)
        for v in self._pending.values():
            self._fact(*v, t=t)
        self._pending = {}
        if self.tuned_mhz is not None:
            self._fact("tuned_centre_mhz", self.tuned_mhz, "receiver tuning (RFCHAN / -C)", t=t)
        self._event(t, "sync", "Sync gained on PCI %d" % pci, "good")

    def _p_cell_det(self, line, t):
        m = RX["cell_det"].search(line)
        if m:
            self._stage(1, t)
            # a detection belongs to the cell the next "Initial sync successful" names
            for name, v, how in (("gscn", int(m[1]), "PSS detection (GSCN raster)"), ("ssb_sc_offset", int(m[2]), "PSS detection"),
                                 ("ssb_freq_mhz", freq_mhz(float(m[3])), "PSS detection (SSB reference)")):
                self._pending[name] = (name, v, how, "confirmed")

    def _p_cfo(self, line, t):
        m = RX["cfo"].search(line)
        if m:
            self._pending["cfo_hz"] = ("cfo_hz", int(m[1]), "PSS/SSS estimate at acquisition", "confirmed")

    def _p_sib1_dec(self, line, t):
        if "SIB1 decoded" in line:
            self._stage(3, t)

    def _p_sib1_cfg(self, line, t):
        m = RX["sib1_cfg"].search(line)
        if m:
            self._fact("dl_nrb", int(m[2]), "SIB1 servingCellConfigCommon", t=t)
            self._fact("scs_khz", 15 * (1 << int(m[3])), "SIB1 servingCellConfigCommon", t=t)
            self._fact("offset_to_point_a", int(m[4]), "SIB1", t=t)
            self._fact("ul_nrb", int(m[5]), "SIB1 servingCellConfigCommon", t=t)

    def _p_rfchan(self, line, t):
        m = RX["rfchan"].search(line)
        if m:
            self.rfchan["ch" + m[1]] = {"port": m[2], "gain_db": float(m[3]), "freq_mhz": float(m[4]), "rate_msps": float(m[5])}
            self.tuned_mhz = float(m[4])

    def _p_cmdline(self, line, t):
        m = RX["cmdline"].search(line)
        if m and self.cmdline is None:
            self.set_cmdline(CMD_TOKEN.findall(m[1]) or m[1].split())

    def _p_newctx(self, line, t):
        if RX["new_ctx"].search(line):
            self.new_ctx += 1

    def _p_dci01(self, line, t):
        m = RX["dci01_len"].search(line)
        if m:
            self.ul["dci01_len"] = int(m[1])
            self._cell()["dci_len"]["UL 0_1"] = {"len": int(m[1]), "rnti": m[2], "t": t}

    def _p_td(self, line, t):
        if "CONVERGED" in line:
            self._h_td(line, t)

    def _p_synchfail(self, line, t):
        if not self.events or self.events[-1]["kind"] != "sync_fail":   # one event per burst of failures
            self._event(t, "sync_fail", "Initial sync attempt failed (searching continues)", "warn")

    # ---------------- other inputs
    def set_cmdline(self, argv):
        self.cmdline = list(argv)
        for i, a in enumerate(argv):
            if a == "-O" and i + 1 < len(argv):
                self.cfg_path = argv[i + 1]
            if a == "-C" and i + 1 < len(argv) and self.tuned_mhz is None:
                self.tuned_mhz = freq_mhz(_int(argv[i + 1]) or 0)

    def _note_rnti(self, rnti, ok, t, src):
        if ok:
            self._stage(6, t)
        if rnti not in self.rnti_seen:
            self.rnti_seen.add(rnti)
            self._stage(5, t)
            if self.new_rnti_events < NEW_RNTI_EVENTS:
                self.new_rnti_events += 1
                self._event(t, "new_rnti", "New RNTI 0x%04x seen (%s)" % (rnti, src))

    def feed_metrics(self, obj):
        tm = _num(obj.get("t_mono_ns"))
        key = (tm, obj.get("abs_slot"))
        if tm is not None:
            if key in self.metrics_seen:
                return            # the same snapshot from the log line and from metrics.jsonl
            self.metrics_seen.add(key)
        self.metrics_n += 1
        prev, self.metrics_prev, self.metrics_last = self.metrics_last, self.metrics_last, obj
        t = self._rel(tm)
        st = obj.get("acq_state")
        if st and st != self.acq_state and isinstance(st, str):
            self.acq_state = st
        if _num(obj.get("acq_pbch_locks")):
            self._stage(2, t)
        if _num(obj.get("acq_sib1_decodes")):
            self._stage(3, t)
        if _num(obj.get("pdcch_accepts_c")):
            self._stage(5, t)
        if _num(obj.get("pdschq_crc_ok")):
            self._stage(6, t)
        if _num(obj.get("pusch_crc_ok")):
            self._stage(7, t)
        if _num(obj.get("pci")) and self.cur_pci is None:
            self.cur_pci = int(obj["pci"])
        if prev is not None:
            for k, label in (("ldpc_cuda_breaker_trips", "LDPC CUDA breaker tripped"),
                             ("acq_sync_losses", "Sync loss counted by the receiver"),
                             ("gpu_fep_disabled", "GPU FEP disabled after self-check failures")):
                a, b = _num(prev.get(k)), _num(obj.get(k))
                if a is not None and b is not None and b > a:
                    self._event(t, k, "%s (%s -> %s)" % (label, int(a), int(b)), "bad")
        self.series.append(window(prev, obj, t))

    def feed_obs(self, o, t=None):
        if not isinstance(o, dict):
            return
        if t is None:
            t = self._rel(_num(o.get("t_mono_ns")))
        d = "UL" if o.get("dir") == "UL" else "DL"
        rnti = _num(o.get("rnti"))
        crc = _num(o.get("crc"))
        pci = _num(o.get("pci"))
        self._agg("obs", int(pci) if pci is not None else self.cur_pci, d).add(
            t, int(rnti) if rnti is not None else None, mcs=_num(o.get("mcs")), nl=_num(o.get("nl")),
            nb_rb=_num(o.get("nb_rb")), tbs=_num(o.get("tbs")), crc=crc, qm=_num(o.get("qm")),
            snr=_num(o.get("snr_db")), mcs_table=_num(o.get("mcs_table")), dmrs_id=_num(o.get("dmrs_scrambling_id")))
        if rnti is not None:
            self._note_rnti(int(rnti), crc, t, "grant " + d)
        if d == "UL" and crc:
            self._stage(7, t)
        ch = freq_mhz(_num(o.get("carrier_hz")))
        if ch and self.tuned_mhz is None:
            self.tuned_mhz = ch

    def feed_sense(self, r, t=None):
        s = self.sense
        if not isinstance(r, dict) or r.get("schema") != 2:
            s["bad"] += 1
            return
        s["records"] += 1
        typ = r.get("type") or "?"
        s["by_type"][typ] = s["by_type"].get(typ, 0) + 1
        tm = _num(r.get("t_mono_ns"))
        if tm is not None:
            s["first_t"] = tm if s["first_t"] is None else min(s["first_t"], tm)
            s["last_t"] = tm if s["last_t"] is None else max(s["last_t"], tm)
        if typ == "rs":
            src = r.get("source") or "?"
            s["by_source"][src] = s["by_source"].get(src, 0) + 1
        elif typ == "session":
            s["session"] = {k: r.get(k) for k in ("rx_id", "n_rx", "rx_gain_db", "carrier_dl_hz", "fs_hz", "scs_khz",
                                                  "n_rb_dl", "build") if k in r}

    def feed_cfr(self, f):
        """f: dict(frame_id, source, n_ant, n_re, abs_slot, noise_var, k=[...], mag=[[...]], ph=[[...]]) downsampled."""
        c = self.cfr
        c["frames"] += 1
        src = f["source"]
        c["by_source"][src] = c["by_source"].get(src, 0) + 1
        if c["last_id"] is not None and f["frame_id"] > c["last_id"] + 1:
            c["gaps"] += f["frame_id"] - c["last_id"] - 1
        c["last_id"] = f["frame_id"]
        c["last"][src] = f
        h = c["heat"].setdefault(src, deque(maxlen=64))
        h.append([round(sum(row) / len(row), 3) if row else None for row in zip(*f["mag"])] if f["mag"] else [])

    # ---------------- snapshot
    def grants_view(self, pci_key=None):
        """Pick the richer per-grant source per cell: obs.jsonl if it has grants, else EVMGRANT log lines."""
        out = {}
        keys = set(self.grants["obs"]) | set(self.grants["log"])
        for k in keys:
            if pci_key not in (None, "", "all") and k != pci_key:
                continue
            o = self.grants["obs"].get(k, {})
            src, g = ("obs", o) if sum(a.n for a in o.values()) else ("log", self.grants["log"].get(k, {}))
            out[k] = {"src": "obs.jsonl (per-grant observations)" if src == "obs" else "run.log EVMGRANT lines",
                      **{d: a.snap(self.t_last) for d, a in g.items()}}
            lg = self.grants["log"].get(k, {}).get("DL")
            if src == "obs" and lg and lg.dec_us and "DL" in out[k]:
                # obs.jsonl carries no decode time: per-grant decode times always come from EVMGRANT
                out[k]["DL"]["dec_us"] = lg.snap(self.t_last, top=0)["dec_us"]
        return out

    def snapshot(self):
        m = self.metrics_last or {}
        stages = []
        cur = 0
        for i, name in enumerate(SYNC_STAGES):
            done = self.stage_done[i]
            if done is not None:
                cur = i
            stages.append({"name": name, "done": done is not None, "t": done if isinstance(done, (int, float)) and done is not True else None,
                           "help": SYNC_HELP[i]})
        q = {}
        if m.get("pdschq_queued") is not None:
            q = {k: m.get("pdschq_" + k) for k in ("queued", "decoded", "crc_ok", "drop_full", "drop_stale", "drop_epoch",
                                                   "drop_narrow", "stale_after_decode", "max_lag")}
            q["src"] = "ISAC_METRICS"
            q["reasons"] = m.get("pdschq_reasons")
            q["latency"] = m.get("pdschq_latency")
        elif self.pdschq:
            q = dict(self.pdschq, src="PDSCHQ log line")
        sq = {}
        if m.get("scanq_queued") is not None:
            sq = {k: m.get("scanq_" + k) for k in ("queued", "processed", "drop_full", "drop_stale", "drop_epoch", "max_lag")}
            sq["src"] = "ISAC_METRICS"
        elif self.pdcch.get("scanq"):
            s = self.pdcch["scanq"]
            sq = {"queued": s["queued"], "processed": s["done"], "drop_full": s["drop_full"], "drop_stale": s["drop_stale"],
                  "max_lag": s["maxlag"], "src": "blind PDCCH summary line"}
        pd = dict(self.pdcch)
        if m.get("pdcch_occasions") is not None:
            pd.update(occasions=m.get("pdcch_occasions"), candidates=m.get("pdcch_candidates"),
                      accepts=m.get("pdcch_accepts"), accepts_c=m.get("pdcch_accepts_c"),
                      src=("ISAC_METRICS + summary line" if self.pdcch else "ISAC_METRICS"))
        pusch = {"try": m.get("pusch_try"), "crc_ok": m.get("pusch_crc_ok"), "src": "ISAC_METRICS"} \
            if m.get("pusch_try") is not None else dict(self.pusch, src="pusch_passive log line") if self.pusch else {}
        last = {}
        for w in list(self.series)[-3:][::-1]:      # newest value per key; a short final window may lack one
            for k, v in w.items():
                last.setdefault(k, v) if v is not None else None
        return {
            "lines": self.lines, "t_last": self.t_last,
            "radio": {"chpow": self.chpow, "chpow_series": list(self.chpow_series)[-120:], "rfchan": self.rfchan,
                      "rfstall_n": len(self.rfstall), "rfstall": self.rfstall[-10:], "rfcensus": self.rfcensus},
            "sync": {"stages": stages, "current": cur, "acq_state": self.acq_state, "acq_path": self.acq_path,
                     "sync_losses": max(self.sync_losses, int(_num(m.get("acq_sync_losses")) or 0)),
                     "sync_count": self.sync_count, "cur_pci": self.cur_pci},
            "cells": self.cells, "cur_pci": self.cur_pci, "tuned_mhz": self.tuned_mhz,
            "pdcch": pd, "pdschq": q, "pdschq_rnti": self.pdschq_rnti, "scanq": sq, "puschq": self.puschq,
            "ldpc": self.ldpc or ({"ok": m.get("ldpc_ok"), "seg_fail": m.get("ldpc_seg_fail"), "tb_fail": m.get("ldpc_tb_fail"),
                                   "zero_tb": m.get("ldpc_zero_tb")} if m.get("ldpc_ok") is not None else {}),
            "pusch": pusch, "ul": self.ul, "td_conv": self.td_conv, "new_ctx": self.new_ctx,
            "grants": self.grants_view(), "rnti_seen": len(self.rnti_seen),
            "timing": {"pdtim": self.pdtim, "pdtimpct": self.pdtimpct, "pdtimcls": self.pdtimcls, "p99age": self.p99age,
                       "series": list(self.lat_series)[-400:]},
            "metrics": m, "metrics_n": self.metrics_n, "series": list(self.series)[-720:], "window": last,
            "sense": dict(self.sense, export={k: m.get(k) for k in ("sense_cfr_drop", "sense_rec_drop", "sense_io_errors",
                                                                   "sense_cfr_no_sink", "sense_rec_count", "sense_write_count",
                                                                   "prach_drops", "prach_skipped", "srs_jobs_dropped")}),
            "cfr": {"frames": self.cfr["frames"], "by_source": self.cfr["by_source"], "gaps": self.cfr["gaps"],
                    "last": self.cfr["last"], "heat": {k: list(v) for k, v in self.cfr["heat"].items()}},
            "csirs_metrics": {k: m.get(k) for k in ("csirs_candidates", "csirs_confirmed", "csirs_revoked", "zp_exported",
                                                    "zp_revoked", "csirs_time_to_confirm_resources")},
            "events": sorted(self.events, key=lambda e: -1.0 if e["t"] is None else e["t"]), "events_dropped": self.events_dropped,
            "config": {"cmdline": self.cmdline, "cfg_path": self.cfg_path, "env": self.env},
        }


def window(a, b, t):
    """Rates over one metrics period (two consecutive cumulative snapshots). Negative delta = counter reset -> None."""
    out = {"t": t, "acq_state": b.get("acq_state"), "gpu_budget_pct": _num(b.get("gpu_budget_used_pct"))}
    if a is None:
        return out
    ta, tb = _num(a.get("t_mono_ns")), _num(b.get("t_mono_ns"))
    dt = (tb - ta) / 1e9 if ta is not None and tb is not None and tb > ta else None
    if dt is not None and dt < 2.0:
        return out          # a snapshot right after the previous one (e.g. at shutdown): too short for a rate

    def d(k):
        x, y = _num(a.get(k)), _num(b.get(k))
        if x is None or y is None or y < x:
            return None
        return y - x

    dec, ok, q, st, fl = d("pdschq_decoded"), d("pdschq_crc_ok"), d("pdschq_queued"), d("pdschq_drop_stale"), d("pdschq_drop_full")
    sq, sf, ss = d("scanq_queued"), d("scanq_drop_full"), d("scanq_drop_stale")
    pt, po = d("pusch_try"), d("pusch_crc_ok")
    if ok is not None and dec:
        out["crc_pct"] = 100.0 * ok / dec
    if dec is not None and dt:
        out["decoded_per_s"] = dec / dt
    if q is not None and dt:
        out["grants_per_s"] = q / dt
    if st is not None and q:
        out["stale_pct"] = 100.0 * st / q
    if fl is not None and q:
        out["pdsch_full_pct"] = 100.0 * fl / q
    if sf is not None and sq:
        out["drop_full_pct"] = 100.0 * sf / sq
    if ss is not None and sq:
        out["scan_stale_pct"] = 100.0 * ss / sq
    if po is not None and pt:
        out["pusch_crc_pct"] = 100.0 * po / pt
    occ = d("pdcch_occasions")
    if occ is not None and dt:
        out["pdcch_occ_per_s"] = occ / dt
    acc = d("pdcch_accepts")
    if acc is not None and dt:
        out["dci_per_s"] = acc / dt
    out["dt_s"] = dt
    return out


_SENSING = {
    "ISAC_METRICS": RunState._h_metrics, "CHPOW": RunState._h_chpow, "RFSTALL": RunState._h_rfstall,
    "RFCENSUS": RunState._h_rfcensus, "ACQ_STATE": RunState._h_acq, "ACQ_EVENT": RunState._h_acq,
    "ACQ": RunState._h_acq, "ISAC_ACQ_RETUNE": RunState._h_retune, "MIB": RunState._h_mib, "SIB1": RunState._h_sib1,
    "TDD": RunState._h_tdd, "CSS0": RunState._h_css0, "multi-CORESET": RunState._h_bank, "CORESET": RunState._h_bank,
    "SEARCH_SPACE": RunState._h_ss, "PDCCH_SCRAMBLING_ID": RunState._h_scramb, "DCI": RunState._h_dci,
    "CSIRS_BLIND": RunState._h_csirs, "CONFIG_EPOCH": RunState._h_epoch, "blind": RunState._h_blind,
    "PDSCHQ": RunState._h_pdschq, "PUSCHQ": RunState._h_puschq, "LDPCDIAG": RunState._h_ldpc,
    "pusch_passive": RunState._h_pusch, "PDTIM": RunState._h_pdtim, "PDTIMPCT": RunState._h_pdtim,
    "PDTIMCLS": RunState._h_pdtim, "PDTIMCLSST": RunState._h_pdtim, "EVMGRANT": RunState._h_evm, "P99AGE": RunState._h_p99age,
    "ULLAYOUT": RunState._h_ul, "ULLAYOUT_CRC_LOCK": RunState._h_ul, "Technique": RunState._h_td,
}
_PLAIN = (
    ("Initial sync successful", RunState._p_sync), ("Cell Detected with GSCN", RunState._p_cell_det),
    ("Measured Carrier Frequency offset", RunState._p_cfo), ("SIB1 decoded", RunState._p_sib1_dec),
    ("cell config extracted from SIB1", RunState._p_sib1_cfg), ("RFCHAN ch", RunState._p_rfchan),
    ("CMDLINE:", RunState._p_cmdline), ("SWEEP: new per-RNTI context", RunState._p_newctx),
    ("UL automatic DCI length locked", RunState._p_dci01), ("Technique D CONVERGED", RunState._p_td),
    ("synch Failed", RunState._p_synchfail),
)

# Lines the Logs page highlights (same prefixes the parser uses).
KEY_LINE = re.compile(r"Initial sync successful|Cell Detected|SIB1 (CELL|decoded)|MIB fields|ACQ_STATE|ACQ carrier|"
                      r"CONFIG_EPOCH|CSS0 autoconf|CORESET bank|multi-CORESET|SEARCH_SPACE INFERRED|PDCCH_SCRAMBLING_ID|"
                      r"CSIRS_BLIND CONFIRMED|Technique D CONVERGED|ULLAYOUT (LOCK|BWP_LOCK|PRIOR_CONFIRM)|ULLAYOUT_CRC_LOCK|"
                      r"RFSTALL|TDD from SIB1|ISAC_ACQ_RETUNE|DCI 1_1 length locked|UL automatic DCI length")


# --------------------------------------------------------------------------- derived views (tested)

def cell_rows(snap, key):
    """Cell-information table rows: item, value, how it was obtained, evidence state."""
    c = snap["cells"].get(key)
    if not c:
        return []
    f = c["facts"]
    rows = []

    def add(item, name, unit=""):
        x = f.get(name)
        if x is None:
            rows.append({"item": item, "value": None, "how": "not learned yet", "state": "unknown"})
        else:
            v = x["value"]
            rows.append({"item": item, "value": (("%s %s" % (v, unit)).strip() if v is not None else None), "how": x["how"],
                         "state": x["state"]})

    add("Cell identity (PCI)", "pci")
    add("Operator code (PLMN, MCC-MNC)", "plmn")
    add("Cell identifier (NCI)", "cell_identity")
    add("Tracking area code (TAC)", "tac")
    add("Receiver tuning centre", "tuned_centre_mhz", "MHz")
    add("SSB frequency", "ssb_freq_mhz", "MHz")
    add("GSCN (sync raster index)", "gscn")
    add("SSB subcarrier offset in the grid", "ssb_sc_offset")
    add("k_SSB (MIB)", "k_ssb")
    add("Frequency offset at acquisition (CFO)", "cfo_hz", "Hz")
    add("Subcarrier spacing (SCS, common)", "scs_common_khz", "kHz")
    add("Carrier width (N_RB)", "carrier_nrb", "PRB")
    add("Carrier centre", "carrier_centre_mhz", "MHz")
    add("Point A (grid subcarrier)", "point_a_sc")
    add("Carrier end (grid subcarrier)", "carrier_end_sc")
    add("Carrier check verdict", "carrier_verdict")
    add("DL bandwidth (SIB1)", "dl_nrb", "PRB")
    add("UL bandwidth (SIB1)", "ul_nrb", "PRB")
    add("TDD pattern", "tdd")
    add("Cell barred flag (MIB)", "cell_barred")
    add("PDSCH DM-RS type-A position (MIB)", "dmrs_typeA_pos")
    add("CORESET#0 index (MIB)", "coreset0_index")
    add("CORESET#0", "coreset0")
    add("Search space #0 (SS#0)", "ss0")
    for k, cs in sorted(c["coresets"].items()):
        rows.append({"item": "Dedicated CORESET #%s" % k,
                     "value": "RB offset %s, %s RB, symbol %s, DCI length %s" % (cs["rb_offset"], cs["span_rb"], cs["symbol"], cs["dci_len"]),
                     "how": cs["how"], "state": cs["state"]})
    for k, ss in sorted(c["search_spaces"].items()):
        rows.append({"item": "Search space %s" % k, "value": "aggregation levels {%s}, %s confirmed" % (ss["al"], ss["confirmed"]),
                     "how": ss["how"], "state": ss["state"]})
    for k, s in sorted(c["scrambling"].items()):
        rows.append({"item": "PDCCH scrambling id [%s]" % k, "value": s["n_id"], "how": s["how"], "state": s["state"]})
    for k, d in sorted(c["dci_len"].items()):
        rows.append({"item": "DCI length %s" % k, "value": "%s bits" % d["len"], "how": "blind length search (locked)",
                     "state": "confirmed"})
    for k, cs in sorted(c["csirs"].items()):
        rows.append({"item": "CSI-RS resource #%s" % k, "value": cs["resource"], "how": cs["how"], "state": cs["state"]})
    g = snap["grants"].get(key, {}).get("DL")
    if g and g.get("dmrs_ids"):
        rows.append({"item": "PDSCH DM-RS scrambling ids (CRC-OK grants)", "value": ", ".join(g["dmrs_ids"]),
                     "how": "per-grant observations", "state": "confirmed"})
    if g and g.get("mcs_tables"):
        rows.append({"item": "MCS table (CRC-OK grants)", "value": ", ".join("table %s: %s" % kv for kv in g["mcs_tables"].items()),
                     "how": "solver / Technique D (CRC verified)", "state": "confirmed"})
    tables = {v["mcs_table"] for v in snap["td_conv"].values() if str(v.get("pci")) == key}
    if tables:
        rows.append({"item": "MCS table (converged RNTIs)", "value": ", ".join(map(str, sorted(tables))),
                     "how": "Technique D convergence (TB CRC)", "state": "confirmed"})
    ul = snap["ul"]
    rows.append({"item": "Uplink BWP (PUSCH layout)", "value": ul.get("bwp"), "how": ul.get("how") or "not locked",
                 "state": {"LOCKED": "confirmed", "PROVISIONAL": "provisional"}.get(ul.get("state"), "unknown")})
    return rows


def ul_reason(snap):
    """Plain-language reason the uplink is (not) locked."""
    ul, pd = snap["ul"], snap["pdcch"]
    if ul.get("state") == "LOCKED":
        return "UL layout locked: %s, via %s." % (ul["bwp"], ul["how"])
    if ul.get("state") == "PROVISIONAL" and (snap["pusch"].get("crc_ok") or 0) > 0:
        return ("UL layout provisional by its statistics (%s), but %s PUSCH grants passed CRC with the layout in use."
                % (ul["bwp"], snap["pusch"]["crc_ok"]))
    if ul.get("state") == "PROVISIONAL":
        return "UL layout provisional: %s (%s). The BWP start is not CRC-confirmed yet." % (ul["bwp"], ul["how"])
    dci01 = ((pd.get("dci01") or {}).get("accepts")) or 0
    if not snap["sync"]["stages"][2]["done"]:
        return "No UL yet: the receiver is not synchronised to a cell."
    st = ul.get("stats")
    if st:
        return "Searching the UL layout: %s votes over %s hypotheses, BWP start unknown." % (st["votes"], st["hyps"])
    if not dci01 and not ul.get("dci01_len"):
        return "No UL DCI (format 0_1) seen yet: UL grants are needed to learn the uplink layout."
    return ("UL DCIs are seen (%s accepted) but no UL layout lock line yet: the lock may be disabled "
            "(ISAC_UL_LAYOUT_LOCK unset) or still collecting evidence." % dci01)


def latency_view(snap):
    """Best available decode-time numbers (us) and their source."""
    tm = snap["timing"]
    r = tm["pdtimcls"].get("real")
    if r:
        return {"p50": r["p50"], "p90": r["p90"], "p99": r["p99"], "max": r["max"], "mean": r["mean"], "n": r["n"],
                "src": "PDTIMCLS real (decode body, us)"}
    s = tm["pdtimpct"].get("sum")
    if s:
        return {"p50": s["p50"], "p90": s["p90"], "p99": s["p99"], "max": None, "mean": None, "n": s["n"],
                "src": "PDTIMPCT sum (fep+chest+demod+ldpc, us)"}
    best = None
    for k, g in snap["grants"].items():
        dl = g.get("DL")
        if dl and (dl.get("dec_us") or {}).get("n") and (best is None or dl["dec_us"]["n"] > best["n"]):
            best = dict(dl["dec_us"], src="EVMGRANT dec_us (last %d grants)" % dl["dec_us"]["n"])
    return best or {"p50": None, "p90": None, "p99": None, "max": None, "n": 0,
                    "src": "none (enable ISAC_PDCCH_TIMING for PDTIM lines, or EVMGRANT)"}


def dl_totals(snap, key=None):
    """Aggregate DL grant counters (per cell or over all cells) plus the queue view."""
    n = ok = 0
    gps = 0.0
    bps = 0.0
    have_rate = False
    for k, g in snap["grants"].items():
        if key not in (None, "", "all") and k != key:
            continue
        dl = g.get("DL")
        if not dl:
            continue
        n += dl["grants"]
        ok += dl["crc_ok"]
        if dl["grants_per_s_recent"] is not None:
            gps += dl["grants_per_s_recent"]
            bps += dl["tput_bps_recent"] or 0.0
            have_rate = True
    return {"grants": n, "crc_ok": ok, "crc_pct": _pct(ok, n), "grants_per_s": gps if have_rate else None,
            "tput_bps": bps if have_rate else None}


def tiles(snap, thr=None, host=None):
    """Home tiles: id, title, value, level (ok|warn|bad|idle), sentence, target page."""
    thr = dict(DEFAULT_THRESHOLDS, **(thr or {}))
    host = host or {}
    w = snap["window"] or {}
    out = []

    # Radio
    stalls = snap["radio"]["rfstall_n"]
    pw = snap["radio"]["chpow"]
    pws = ", ".join("%s %.1f dBFS" % (k, v) for k, v in sorted(pw.items())) if pw else None
    silent = host.get("silent_s")
    if stalls:
        lvl, s = "bad", "RF stall seen %d time(s); last: %s" % (stalls, snap["radio"]["rfstall"][-1]["text"][:80])
    elif host.get("live") and silent is not None and silent > thr["silent_s"]:
        lvl, s = "bad", "Receiver silent for %.0f s (no new log lines)." % silent
    elif snap["lines"] == 0:
        lvl, s = "idle", "No receiver log read yet."
    else:
        lvl, s = "ok", "Radio streaming, no stalls." + (" Antenna power: %s." % pws if pws else
                                                         " Per-antenna power not logged (set ISAC_USRP_CHPOW_S=N).")
    out.append({"id": "radio", "title": "Radio", "value": ("%d stall(s)" % stalls) if stalls else ("streaming" if snap["lines"] else "-"),
                "level": lvl, "sentence": s, "page": "perf" if stalls else "spectrum"})

    # Sync
    sy = snap["sync"]
    cur = sy["current"]
    name = SYNC_STAGES[cur]
    if sy["acq_state"] == "LOST":
        lvl, s = "bad", "Sync lost; the receiver is re-acquiring (%d loss(es) so far)." % sy["sync_losses"]
    elif cur >= 5:
        lvl, s = "ok", "Synchronised to PCI %s and %s." % (sy["cur_pci"], SYNC_HELP[cur])
    elif cur >= 1:
        lvl, s = "warn", "Partly acquired: %s; next: %s." % (SYNC_HELP[cur], SYNC_HELP[cur + 1])
    else:
        lvl, s = ("warn" if snap["lines"] else "idle"), "Searching for a cell."
    out.append({"id": "sync", "title": "Sync", "value": name, "level": lvl, "sentence": s, "page": "cell"})

    # DL
    tot = dl_totals(snap)
    crc = w.get("crc_pct") if w.get("crc_pct") is not None else tot["crc_pct"]
    if not tot["grants"] and not snap["pdschq"].get("decoded"):
        lvl, s, val = "idle", "No PDSCH decoded yet: waiting for an RNTI to be solved.", "-"
    else:
        if crc is None and snap["pdschq"].get("decoded"):
            crc = _pct(snap["pdschq"].get("crc_ok"), snap["pdschq"].get("decoded"))
        lvl = "ok" if crc is not None and crc >= thr["crc_warn_pct"] else "warn" if crc is not None and crc >= thr["crc_bad_pct"] else "bad"
        gps = w.get("decoded_per_s") or tot["grants_per_s"]
        val = "%.0f %%" % crc if crc is not None else "-"
        s = "DL is decoding %s of the grants it sees (CRC OK)%s." % (
            val, ", %.0f grants/s" % gps if gps else "")
    out.append({"id": "dl", "title": "Downlink", "value": val, "level": lvl, "sentence": s, "page": "dl"})

    # UL
    p = snap["pusch"]
    upct = _pct(p.get("crc_ok"), p.get("try")) if p.get("try") else None
    ul_state = snap["ul"].get("state")
    if upct is not None:
        lvl = "ok" if upct >= thr["crc_warn_pct"] else "warn" if upct >= thr["crc_bad_pct"] or ul_state else "bad"
        s, val = "UL PUSCH CRC OK on %.0f %% of %d attempts. %s" % (upct, p["try"], ul_reason(snap)), "%.0f %%" % upct
    else:
        lvl = "ok" if ul_state == "LOCKED" else "warn" if ul_state else "idle"
        s, val = ul_reason(snap), (ul_state or "not locked").lower()
    out.append({"id": "ul", "title": "Uplink", "value": val, "level": lvl, "sentence": s, "page": "ul"})

    # Decode health
    lat = latency_view(snap)
    stale = w.get("stale_pct")
    if stale is None and snap["pdschq"].get("queued"):
        stale = _pct(snap["pdschq"].get("drop_stale"), snap["pdschq"].get("queued"))
    if lat["p99"] is None and stale is None:
        lvl, s, val = "idle", "No decode timing yet (needs PDTIM/EVMGRANT lines or PDSCH queue counters).", "-"
    else:
        bad = (lat["p99"] is not None and lat["p99"] > thr["p99_target_us"]) or (stale is not None and stale > thr["stale_pct"])
        lvl = "warn" if bad else "ok"
        if stale is not None and stale > 3 * thr["stale_pct"]:
            lvl = "bad"
        val = ("p99 %.2f ms" % (lat["p99"] / 1000.0)) if lat["p99"] is not None else "-"
        s = "Decode p50 %s / p99 %s vs %.2f ms target; %s stale drops." % (
            "%.2f ms" % (lat["p50"] / 1000.0) if lat["p50"] is not None else "n/a",
            "%.2f ms" % (lat["p99"] / 1000.0) if lat["p99"] is not None else "n/a", thr["p99_target_us"] / 1000.0,
            "%.1f %%" % stale if stale is not None else "unknown")
    out.append({"id": "decode", "title": "Decode health", "value": val, "level": lvl, "sentence": s, "page": "perf"})

    # Compute
    gpu = _num((snap["metrics"] or {}).get("gpu_budget_used_pct"))
    parts, lvl = [], "idle"
    if gpu is not None:
        parts.append("GPU budget %.0f %%" % gpu)
        lvl = "bad" if gpu >= thr["gpu_budget_pct"] else "ok"
    if host.get("rx_cpu_pct") is not None:
        parts.append("receiver CPU %.0f %%" % host["rx_cpu_pct"])
        lvl = "ok" if lvl == "idle" else lvl
    if host.get("temp_c") is not None:
        parts.append("%.0f C" % host["temp_c"])
        if host["temp_c"] >= thr["temp_c"]:
            lvl = "bad"
    if host.get("mem_avail_gb") is not None:
        parts.append("%.0f GB free RAM" % host["mem_avail_gb"])
        if host["mem_avail_gb"] < thr["ram_floor_gb"]:
            lvl = "bad"
    s = (", ".join(parts) + ".") if parts else "No compute figures (offline run, or no GPU metrics)."
    out.append({"id": "compute", "title": "Compute", "value": ("%.0f %% GPU" % gpu) if gpu is not None else (parts[0] if parts else "-"),
                "level": lvl, "sentence": s, "page": "perf"})
    return out


def alerts(snap, thr=None, host=None):
    """Active alerts: list of {id, sev, text}. Thresholds come from the page settings."""
    thr = dict(DEFAULT_THRESHOLDS, **(thr or {}))
    host = host or {}
    w = snap["window"] or {}
    m = snap["metrics"] or {}
    out = []
    if snap["sync"]["acq_state"] == "LOST":
        out.append({"id": "sync_lost", "sev": "bad", "text": "Sync lost: the receiver is re-acquiring."})
    if snap["radio"]["rfstall_n"]:
        out.append({"id": "rfstall", "sev": "bad", "text": "RF stall (RFSTALL) seen %d time(s): %s" % (
            snap["radio"]["rfstall_n"], snap["radio"]["rfstall"][-1]["text"][:100])})
    lat = latency_view(snap)
    if lat["p99"] is not None and lat["p99"] > thr["p99_target_us"]:
        out.append({"id": "p99", "sev": "warn", "text": "Decode p99 %.2f ms is above the %.2f ms target (%s)." % (
            lat["p99"] / 1000.0, thr["p99_target_us"] / 1000.0, lat["src"])})
    stale = w.get("stale_pct")
    if stale is not None and stale > thr["stale_pct"]:
        out.append({"id": "stale", "sev": "warn", "text": "PDSCH stale drops %.1f %% in the last window (threshold %.0f %%)." % (
            stale, thr["stale_pct"])})
    df = w.get("drop_full_pct")
    if df is not None and df > thr["drop_full_pct"]:
        out.append({"id": "drop_full", "sev": "warn", "text": "Scan queue full drops %.1f %% (threshold %.1f %%)." % (
            df, thr["drop_full_pct"])})
    gpu = _num(m.get("gpu_budget_used_pct"))
    if gpu is not None and gpu >= thr["gpu_budget_pct"]:
        out.append({"id": "gpu", "sev": "bad", "text": "GPU discovery budget exhausted (%.0f %% used)." % gpu})
    if _num(m.get("ldpc_cuda_breaker_trips")):
        out.append({"id": "ldpc_breaker", "sev": "bad", "text": "LDPC CUDA breaker tripped %d time(s) (decoder fell back)." %
                    int(m["ldpc_cuda_breaker_trips"])})
    if host.get("live") and host.get("silent_s") is not None and host["silent_s"] > thr["silent_s"]:
        out.append({"id": "silent", "sev": "bad", "text": "Receiver silent for %.0f s (threshold %.0f s)." % (
            host["silent_s"], thr["silent_s"])})
    if host.get("mem_avail_gb") is not None and host["mem_avail_gb"] < thr["ram_floor_gb"]:
        out.append({"id": "ram", "sev": "bad", "text": "Free RAM %.1f GB below the %.0f GB floor." % (
            host["mem_avail_gb"], thr["ram_floor_gb"])})
    if host.get("temp_c") is not None and host["temp_c"] >= thr["temp_c"]:
        out.append({"id": "temp", "sev": "bad", "text": "Temperature %.0f C at or above the %.0f C limit." % (
            host["temp_c"], thr["temp_c"])})
    return out


def kpis(snap, key=None):
    """The compact KPI set used by run history / A-B compare and the run summary (one cell: never pooled)."""
    if key in (None, "", "all") and snap["cur_pci"] is not None:
        key = str(snap["cur_pci"])
    g = {}
    for k, v in snap["grants"].items():
        if key in (None, "", "all") or k == key:
            g = v.get("DL") or g
            if g:
                break
    lat = latency_view(snap)
    q = snap["pdschq"]
    return {
        "dl_grants": dl_totals(snap, key)["grants"], "dl_crc_pct": dl_totals(snap, key)["crc_pct"],
        "by_mcs": {b: (g.get("by_mcs", {}).get(b) if g else None) for b, _, _ in MCS_BUCKETS},
        "stale_pct": _pct(q.get("drop_stale"), q.get("queued")) if q.get("queued") else None,
        "p50_us": lat["p50"], "p99_us": lat["p99"], "lat_src": lat["src"],
        "pusch_crc_pct": _pct(snap["pusch"].get("crc_ok"), snap["pusch"].get("try")) if snap["pusch"].get("try") else None,
        "rntis": snap["rnti_seen"], "sync_losses": snap["sync"]["sync_losses"], "rfstall": snap["radio"]["rfstall_n"],
        "pci": snap["cur_pci"], "stage": SYNC_STAGES[snap["sync"]["current"]],
    }


def summary_md(snap, label, name, thr=None, host=None):
    """One-click run summary (markdown) with the run's evidence label on every number."""
    k = kpis(snap)
    f = lambda v, fmt="%.1f": "n/a" if v is None else fmt % v
    lines = ["# Run summary: %s" % name, "", "Evidence label: `%s`" % label, "",
             "All numbers below carry this label. Do not merge them with runs of another host, cell, bandwidth or commit.", "",
             "## Status", ""]
    for t in tiles(snap, thr, host):
        lines.append("- **%s** (%s): %s" % (t["title"], t["level"], t["sentence"]))
    lines += ["", "## Key numbers `%s`" % label, "", "| KPI | value |", "|---|---|",
              "| Cell (PCI) | %s |" % (k["pci"] if k["pci"] is not None else "n/a"),
              "| Furthest sync stage | %s |" % k["stage"],
              "| DL grants (per-grant source) | %s |" % k["dl_grants"],
              "| DL CRC-OK | %s %% |" % f(k["dl_crc_pct"]),
              "| PDSCH stale drops (cumulative) | %s %% |" % f(k["stale_pct"]),
              "| Decode p50 / p99 | %s / %s us (%s) |" % (f(k["p50_us"], "%.0f"), f(k["p99_us"], "%.0f"), k["lat_src"]),
              "| PUSCH CRC-OK | %s %% |" % f(k["pusch_crc_pct"]),
              "| RNTIs seen | %s |" % k["rntis"], "| Sync losses | %s |" % k["sync_losses"], "| RF stalls | %s |" % k["rfstall"],
              "", "### DL CRC-OK by MCS bucket `%s`" % label, "", "| MCS | CRC-OK | n |", "|---|---|---|"]
    for b, v in k["by_mcs"].items():
        lines.append("| %s | %s | %s |" % (b, (f(v["pct"]) + " %") if v else "n/a", v["n"] if v else 0))
    a = alerts(snap, thr, host)
    lines += ["", "## Alerts", ""] + (["- %s: %s" % (x["sev"], x["text"]) for x in a] or ["- none"])
    lines += ["", "## Timeline (first 30 events)", ""]
    for e in snap["events"][:30]:
        lines.append("- %s %s" % ("t+%.1f s" % e["t"] if isinstance(e["t"], (int, float)) else "t=?", e["text"]))
    return "\n".join(lines) + "\n"
