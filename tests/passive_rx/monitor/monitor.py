#!/usr/bin/env python3
"""Live monitor for the passive ISAC receiver.

Subscribes to the receiver's DetectionReport ZeroMQ PUB socket ([sensing] report_endpoint) and
serves a single-page dashboard. The browser polls /state; CPIs arrive every ~1-30 s, so polling is
well inside the data rate and needs no streaming machinery.

  ./monitor.py --connect tcp://127.0.0.1:5556 --port 8080

--connect may be repeated, but this is a SINGLE-RECEIVER tool: track positions come from the
receiver's own AoA fix (range ellipse + bearing ray), not from cross-receiver fusion. Extra
endpoints are shown side by side, never fused.
"""

import argparse
import json
import re
import subprocess
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import zmq

from paper_stats import PaperStats

# nr_isac_source_t order (nr_isac.h). PUSCH_DMRS is the only uplink source, and its geometry is
# UE->target->rx, not gNB->target->rx -- which is why the UI reports it separately and never maps it.
SOURCE_NAMES = ["csi_rs", "pdsch_dmrs", "pdsch_data", "blind", "pusch_dmrs", "pusch_data"]
UL_SOURCES = {"pusch_dmrs", "pusch_data"}

HISTORY = 200  # CPIs kept per receiver for the sparklines / track trails


class ReportStore:
    """Latest state per receiver, plus a bounded history. Written by SUB threads, read by HTTP."""

    def __init__(self):
        self._lock = threading.Lock()
        self._rx = {}

    def add(self, endpoint, rep):
        rx_id = rep.get("rx_id") or endpoint
        with self._lock:
            st = self._rx.setdefault(
                rx_id,
                {"endpoint": endpoint, "history": deque(maxlen=HISTORY), "trails": {}, "cpi_count": 0,
                 "gaps": deque(maxlen=8)},
            )
            st["latest"] = rep
            now = time.time()
            # Observed inter-report gap. The CPI closes on ROW COUNT, so its wall-clock cadence
            # swings with traffic -- ~4 s at cpi_slots=128 but 150-200 s at 1024. A fixed staleness
            # threshold is therefore meaningless; the UI derives one from these gaps instead.
            if st.get("last_seen") is not None:
                st["gaps"].append(now - st["last_seen"])
            st["last_seen"] = now
            st["cpi_count"] += 1
            st["history"].append(
                {
                    "t": rep.get("cpi_start_time_utc_ns", 0),
                    "n_det": len(rep.get("detections") or []),
                    "cfo_hz": (rep.get("sync", {}).get("cfo", {}) or {}).get("cfo_hz"),
                    "sto_frac": (rep.get("sync", {}).get("sto", {}) or {}).get("mean_frac_bin"),
                    "sfo_ppm": (rep.get("sync", {}).get("sfo", {}) or {}).get("sfo_ppm"),
                    "n_flywheel": (rep.get("sync", {}).get("sto", {}) or {}).get("n_flywheel"),
                    # Per-CPI LOS seed. The one number that says whether the receiver is looking at
                    # the direct path at all: a comb replica seeds it at nof_range/2 while n_valid /
                    # n_flywheel still read perfectly healthy, so it cannot be inferred from them.
                    "los_range_m": (rep.get("sync", {}).get("sto", {}) or {}).get("los_range_m"),
                    "los_seed_bin": (rep.get("sync", {}).get("sto", {}) or {}).get("los_seed_bin"),
                    "n_seed_rejected": (rep.get("sync", {}).get("sto", {}) or {}).get("n_seed_rejected"),
                    "n_valid": (rep.get("sync", {}).get("sto", {}) or {}).get("n_valid"),
                }
            )
            # Track trails: keep each track's fixed positions so the map can draw its path.
            for tr in rep.get("tracks") or []:
                pos = tr.get("position")
                if not pos:
                    continue
                trail = st["trails"].setdefault(str(tr["track_id"]), deque(maxlen=60))
                trail.append({"x": pos[0], "y": pos[1], "v": tr.get("bistatic_velocity_mps", 0.0)})
            # Drop trails whose track is gone, so a long run does not accumulate dead paths.
            live = {str(t["track_id"]) for t in (rep.get("tracks") or [])}
            for tid in [k for k in st["trails"] if k not in live]:
                del st["trails"][tid]

    def snapshot(self):
        with self._lock:
            out = {}
            for rx_id, st in self._rx.items():
                out[rx_id] = {
                    "endpoint": st["endpoint"],
                    "latest": st.get("latest"),
                    "last_seen": st.get("last_seen"),
                    "cpi_count": st["cpi_count"],
                    "cadence_s": (sorted(st["gaps"])[len(st["gaps"]) // 2] if st["gaps"] else None),
                    "history": list(st["history"]),
                    "trails": {k: list(v) for k, v in st["trails"].items()},
                }
            return out


_PROC_CACHE = {"t": 0.0, "n": 0, "cpu": None}


def _softmodem_alive():
    """Number of running receiver processes, cached for a second.

    The page polls at 1 Hz; forking a process scan per request would be silly, and the answer
    cannot change meaningfully faster than that."""
    now = time.time()
    if now - _PROC_CACHE["t"] > 1.0:
        try:
            out = subprocess.run(["pgrep", "-cx", "nr-uesoftmodem"], capture_output=True, text=True)
            _PROC_CACHE["n"] = int(out.stdout.strip() or 0)
            cpu = subprocess.run(["ps", "-o", "%cpu=", "-C", "nr-uesoftmodem"], capture_output=True, text=True)
            _PROC_CACHE["cpu"] = sum(float(x) for x in cpu.stdout.split()) if cpu.stdout.strip() else None
        except Exception:
            _PROC_CACHE["n"] = -1   # unknown, which must not be reported as zero
        _PROC_CACHE["t"] = now
    return _PROC_CACHE["n"]


class LogTail:
    """Tails the receiver log for the health facts that never reach the report: PBCH lock, X410
    stream state, blind-PDCCH accepts, PUSCH/PDSCH CRC. The report is a DSP artifact and says
    nothing about whether the radio is actually alive -- which is the failure mode that matters."""

    PATTERNS = [
        # PDSCHQ, not pdsch_decode[]. DL decoding runs through the QUEUE, so the in-line
        # counter reads near-zero by construction and the panel showed 0.06 % against a
        # real 1.3 %. Same counter trap the runbook documents; the dashboard had it too.
        ("pdsch_crc", re.compile(r"PDSCHQ queued=\d+ decoded=(\d+) crc_ok=(\d+)")),
        # Kept so an in-line-path run (no queue) still reports something rather than nothing.
        ("pdsch_crc_inline", re.compile(r"pdsch_decode\[try=(\d+) crc_ok=(\d+)")),
        # DL TRANSPORT-BLOCK rate. NOT segs_decoded: those counters are summed only over FAILING
        # TBs (nr_pdsch_passive_decode.c:302-303), so that ratio FALLS as decoding improves.
        ("ldpc", re.compile(r"LDPCDIAG ok=(\d+) seg_fail=(\d+) tb_fail=(\d+) zero_tb=(\d+)")),
        ("pusch", re.compile(r"pusch_passive\[try=(\d+) crc_ok=(\d+)")),
        ("dci", re.compile(r"dci01\[accepts=(\d+) rejects=(\d+)")),
        ("blind", re.compile(r"accepts=(\d+)\s+cfr_submits=(\d+)")),
        ("occ", re.compile(r"occ\[([^\]]*)\]")),
        ("overflow", re.compile(r"[Oo]verflow|OOOO|\bO\b")),
        ("sync_lost", re.compile(r"out.of.sync|sync lost|LOST SYNC", re.I)),
        ("pbch_ok", re.compile(r"PBCH.*(?:decoded|CRC OK)|MIB decoded", re.I)),
        # ---- Agnostic discovery surface. These are the verdicts the receiver reaches on its own:
        # what state acquisition is in, and which cell parameters it has PROVEN off the air rather
        # than been told. They are one-shot or rare lines, so they are captured as state, not rates.
        ("acq_state", re.compile(r"ACQ_STATE (\w+) -> (\w+)")),
        ("cell_det", re.compile(r"Cell Detected with GSCN: (\d+), SSB SC offset: (\d+)")),
        ("cfo", re.compile(r"Measured Carrier Frequency offset (-?\d+) Hz")),
        ("carrier", re.compile(r"ACQ carrier (CONFIRMED|MISMATCH)")),
        ("retune", re.compile(r"ISAC_ACQ_RETUNE (\{[^}]*\})")),
        ("conv", re.compile(r"Technique D CONVERGED rnti=0x([0-9a-f]+) tda=(\d+) S=(\d+) L=(\d+) mask=0x([0-9a-f]+) table=(\d+)")),
        ("searchspace", re.compile(r"SEARCH_SPACE INFERRED \[([^\]]+)\].*?monitored AL=\{([^}]*)\}")),
        ("scrambling", re.compile(r"PDCCH_SCRAMBLING_ID CONFIRMED \[([^\]]+)\] n_id=(\d+)")),
        ("rank", re.compile(r"RANK IDENTIFIED from DCI DM-RS ports: modal_layers=(\d+).*?=> (\w+)")),
        ("perrnti", re.compile(r"PDSCHQ per-rnti (.+)$")),
        # Acquisition PROGRESS. Before sync there are no SENSING lines at all, so without these the
        # dashboard reads "waiting" on every field while the receiver is in fact sweeping hard.
        ("scan", re.compile(r"Scanning GSCN: (\d+), with SSB offset: (\d+)")),
        ("polar", re.compile(r"polar decoding wrong")),
        ("pbch_try", re.compile(r"pbch not decoded on any branch")),
        ("synch_fail", re.compile(r"synch Failed")),
        ("rbmap", re.compile(r"RBMAP dl grants=(\d+) peak=(\d+) occ=(\d+) crc=(\d+)")),
        ("ulprb", re.compile(r"PUSCHDIAG \S+ rnti=0x([0-9a-f]+) k2=\d+ prb=(\d+)\+(\d+).*?status=(\d)")),
        ("rfstall", re.compile(r"RFSTALL ([^(]*)\(?")),
        ("mpm_claim", re.compile(r"ERROR_CODE_OVERFLOW|Out of sequence")),
        ("radio_open", re.compile(r"can't open the radio device|rx xport timed out")),
        ("cfo_void", re.compile(r"VOID_CFO_MISLOCK")),
        ("tdd_derived", re.compile(r"TDD from SIB1 (DERIVED|ABSENT|REJECTED)")),
        ("csirs_conf", re.compile(r'CSIRS_BLIND CONFIRMED after \d+ slots -- csirs_monitor = "([^"]+)"')),
        ("dci11_layout", re.compile(r"DCI11_LAYOUT n=\d+ observed \| (\d+) of (\d+) layouts")),
        ("dci01_layout", re.compile(r"DCI01_LAYOUT n=\d+ observed \| (\d+) of (\d+) layouts")),
    ]

    def __init__(self, path, maxlines=4000):
        self.last_line_at = None
        self.path = Path(path)
        self.follow_latest = True   # re-resolve to the newest run.log under the captures tree
        self.lines = deque(maxlen=maxlines)
        self.counters = {"overflow": 0, "sync_lost": 0, "pbch_ok": 0}
        self.stats = {}
        self.fault = None            # last actionable fault (title/action/detail/at)
        self.rbmap = None            # DL: density digits per RB, from the receiver's own counter
        self.ul_occ = [0] * 275      # UL: accumulated per-RB grant count (from PUSCHDIAG)
        self.ul_ok = [0] * 275
        self.ul_grants = 0
        self.agnostic = {}           # what the receiver has derived off the air, with first-seen time
        self.started_at = time.time()
        self.skipped_bytes = 0   # backlog dropped to stay live; shown on the page
        self.paper = PaperStats()
        self.key_lines = deque(maxlen=120)   # the LIVE pane: only the lines an operator acts on
        self._lock = threading.Lock()

    def run(self):
        """Follow the log, with two behaviours an operator actually needs.

        SEED FROM HISTORY. Seeking straight to EOF means a dashboard opened after a run, or
        restarted during one, shows an empty panel for a log full of answers. Ingest the tail that
        already exists first, so the state is right the moment the page loads.

        FOLLOW THE NEWEST CAPTURE. Every run writes a new directory, so a fixed path goes stale as
        soon as the next arm starts and the dashboard then reports a finished run forever.
        """
        cur, f, pending, nlines = None, None, "", 0
        try:
            while True:
                nxt = self._latest() or self.path
                if nxt != cur and nxt.exists():
                    if f:
                        f.close()
                    cur, self.path, pending = nxt, nxt, ""
                    f = nxt.open("r", errors="replace")
                    # The paper statistics need the WHOLE file (one-shot lines such as "Cell
                    # Detected" and "DCI11_LAYOUT armed" sit at the top), ~1 s per 40 MB; the
                    # health tail only needs the last few thousand lines.
                    with self._lock:
                        self.paper.begin_run(nxt, live=False)   # seeded lines carry no time
                        for line in f:
                            self.paper.ingest(line)
                    f.seek(0)
                    self._paper_pause = True     # the tail below is already counted
                    for line in f.readlines()[-self.lines.maxlen:]:
                        self._ingest(line.rstrip())
                    self._paper_pause = False
                    f.seek(0, 2)
                    with self._lock:
                        self.paper.cur["live"] = True
                if f is None:
                    time.sleep(1.0)
                    continue
                # KEEPING UP. The receiver writes ~10k lines/s under load and each line costs a
                # couple of dozen regex searches, so a tail that insists on reading every line falls
                # progressively behind and the panel shows minutes-old numbers -- indistinguishable
                # from a frozen dashboard, and the reason "the CRC is stuck" was reported three
                # times. A live panel owes the operator the PRESENT: when the backlog passes a few
                # MB, skip to near the end and resume there. Nothing downstream is cumulative-from-
                # zero (every stat is an absolute counter printed by the receiver), so dropping
                # backlog costs no state.
                nlines += 1
                if (nlines % 512) == 0:
                    try:
                        size, pos = self.path.stat().st_size, f.tell()
                        if size - pos > 4 << 20:
                            f.seek(max(0, size - (256 << 10)))
                            f.readline()          # discard the partial line at the seek point
                            pending = ""
                            self.skipped_bytes += size - pos
                    except OSError:
                        pass
                chunk = f.readline()
                if not chunk:
                    time.sleep(0.25)
                    continue
                # PARTIAL LINES. readline() on a file another process is appending to returns
                # whatever has been flushed -- frequently half a line. Ingesting that half silently
                # breaks every pattern anchored past the split point: measured 2026-09-15, the
                # decoder panel froze at the run's first PDSCHQ sample while the log pane kept
                # scrolling, because the long PDSCHQ lines were the ones being cut. Hold the
                # remainder until its newline arrives.
                pending += chunk
                if not pending.endswith("\n"):
                    continue
                for ln in pending.splitlines():
                    self._ingest(ln)
                pending = ""
        finally:
            if f:
                f.close()

    def _latest(self):
        """Newest sibling <captures>/*/run.log, or None if that layout does not apply."""
        if not getattr(self, "follow_latest", True):
            return None
        try:
            root = self.path.parent.parent
            runs = [d / "run.log" for d in root.iterdir() if (d / "run.log").is_file()]
        except OSError:
            return None
        return max(runs, key=lambda q: q.stat().st_mtime) if runs else None

    # X410 / receiver faults the operator can act on, with the action. Keyed by pattern name so a
    # banner always carries a remedy instead of a log excerpt nobody can act on.
    FAULTS = {
        "mpm_claim": ("X410 stream out of sequence (stale MPM claim)",
                      "A previous receiver was SIGKILLed and left the claim behind. "
                      "ssh root@128.178.122.174 'systemctl restart usrp-hwd', then wait 200 s before starting."),
        "radio_open": ("X410 will not open (mgmt_portal timeout / device busy)",
                       "MPM is still coming up or another process holds the device. "
                       "Check 'systemctl is-active usrp-hwd' on the X410, kill any nr-uesoftmodem, settle 200 s."),
        "rfstall":    ("RF stall — stream stopped or the timing loop ran away",
                       "If pbch_ok is 0 the stream died: restart the run. If power is healthy it is the "
                       "timing runaway (runbook 4.5) — restart; it is a start-up lottery at 4 antennas."),
        "cfo_void":   ("CFO mis-lock — this capture is void",
                       "CFO is estimated once at acquisition; a bad lock reads 0 % CRC for the whole run. "
                       "Abort and restart (never retune a live radio)."),
        "overflow":   ("UHD/NIC overflow",
                       "Host could not keep up with the stream. Check NIC ring/MTU (9000) and CPU load; "
                       "reduce antennas or probes if it repeats."),
    }

    # Lines that are pure volume and match no pattern. Checked before the regex sweep because at
    # ~10k lines/s the sweep itself is what makes the tail fall behind.
    SKIP = ("CSIRS_BLIND", "FEPDIAG", "TIMEMUT", "rnti_seen", "PRECLIP", "TSYNC_OBS")

    KEY = re.compile(r"ACQ_STATE|ACQ_EVENT|Cell Detected|Measured Carrier|PREFERRED|PDSCHQ|CHESTDIAG|"
                     r"RFSTALL|DISCOVER|autodiscover|DMRS_ORACLE|ORACLE_GATE|HARQC|CORESET|SIB1|"
                     r"DCI11_LAYOUT|VOID|BRANCHFO|RANK IDENTIFIED|Technique D|carrier (CONFIRMED|MISMATCH)")

    def _ingest(self, line):
        with self._lock:
            if not getattr(self, "_paper_pause", False):
                self.paper.ingest(line)
            if self.KEY.search(line):
                self.key_lines.append(time.strftime("%H:%M:%S ") + line.replace("\033", "")[-300:])
        for junk in self.SKIP:
            if junk in line:
                self.last_line_at = time.time()
                return
        with self._lock:
            for name, pat in self.PATTERNS:
                m = pat.search(line)
                if not m:
                    continue
                if name in self.FAULTS and name not in self.counters:
                    title, action = self.FAULTS[name]
                    self.fault = {"title": title, "action": action,
                                  "detail": line.strip()[-160:], "at": time.time()}
                if name == "rbmap":
                    self.rbmap = {"grants": int(m.group(1)), "peak": int(m.group(2)),
                                  "occ": m.group(3), "crc": m.group(4), "at": time.time()}
                    continue
                if name == "ulprb":
                    try:
                        start, n, ok = int(m.group(2)), int(m.group(3)), m.group(4) == "0"
                    except ValueError:
                        continue
                    for rb in range(start, min(start + n, len(self.ul_occ))):
                        self.ul_occ[rb] += 1
                        if ok:
                            self.ul_ok[rb] += 1
                    self.ul_grants += 1
                    continue
                if name in ("tdd_derived", "csirs_conf", "dci11_layout", "dci01_layout"):
                    self.agnostic[name] = (m.group(1) if m.lastindex == 1
                                           else "/".join(m.groups()))
                    self.agnostic.setdefault(name + "_at", time.time())
                    continue
                if name in self.counters:
                    self.counters[name] += 1
                elif name == "pdsch_crc":
                    try_n, ok_n = int(m.group(1)), int(m.group(2))
                    # A queue-path sample always wins: an in-line counter of 0/0 must never
                    # overwrite a real queue measurement on a run that uses both.
                    if name == "pdsch_crc" or self.stats.get("pdsch_try", 0) == 0:
                        self.stats["pdsch_try"] = try_n
                        self.stats["pdsch_ok"] = ok_n
                    self.stats["pdsch_rate"] = (100.0 * ok_n / try_n) if try_n else None
                elif name == "ldpc":
                    ok, sf, tf, zt = (int(m.group(i)) for i in (1, 2, 3, 4))
                    tot = ok + sf + tf
                    self.stats["dl_ok"] = ok
                    self.stats["dl_fail"] = sf + tf
                    self.stats["dl_zero_tb"] = zt
                    self.stats["dl_tb_rate"] = (100.0 * ok / tot) if tot else None
                elif name == "pusch":
                    t, o = int(m.group(1)), int(m.group(2))
                    self.stats["ul_try"] = t
                    self.stats["ul_ok"] = o
                    self.stats["ul_rate"] = (100.0 * o / t) if t else None
                elif name == "dci":
                    self.stats["dci_accepts"] = int(m.group(1))
                    self.stats["dci_rejects"] = int(m.group(2))
                elif name == "blind":
                    self.stats["blind_accepts"] = int(m.group(1))
                    self.stats["blind_submits"] = int(m.group(2))
                elif name == "occ":
                    self.stats["occ"] = m.group(1)
                elif name == "acq_state":
                    self.stats["acq_state"] = m.group(2)
                    self.stats.setdefault("acq_path", [])
                    if not self.stats["acq_path"] or self.stats["acq_path"][-1] != m.group(2):
                        self.stats["acq_path"] = (self.stats["acq_path"] + [m.group(2)])[-12:]
                elif name == "cell_det":
                    self.stats["gscn"] = int(m.group(1))
                    self.stats["ssb_sc"] = int(m.group(2))
                elif name == "cfo":
                    self.stats["cfo_hz"] = int(m.group(1))
                elif name == "carrier":
                    self.stats["carrier"] = m.group(1)
                elif name == "retune":
                    self.stats["retune"] = m.group(1)
                elif name == "conv":
                    self.stats["converged"] = {
                        "rnti": "0x" + m.group(1), "tda": int(m.group(2)), "S": int(m.group(3)),
                        "L": int(m.group(4)), "mask": "0x" + m.group(5), "table": int(m.group(6))}
                elif name == "searchspace":
                    self.stats.setdefault("search_space", {})[m.group(1)] = m.group(2)
                elif name == "scrambling":
                    self.stats.setdefault("scrambling", {})[m.group(1)] = int(m.group(2))
                elif name == "rank":
                    self.stats["rank"] = {"layers": int(m.group(1)), "probe": m.group(2)}
                elif name == "perrnti":
                    self.stats["per_rnti"] = m.group(1).strip()[:300]
                elif name == "scan":
                    self.stats["scan_gscn"] = int(m.group(1))
                    self.stats["scan_count"] = self.stats.get("scan_count", 0) + 1
                elif name == "polar":
                    self.stats["polar_attempts"] = self.stats.get("polar_attempts", 0) + 1
                elif name == "pbch_try":
                    self.stats["pbch_attempts"] = self.stats.get("pbch_attempts", 0) + 1
                elif name == "synch_fail":
                    self.stats["synch_failed"] = self.stats.get("synch_failed", 0) + 1
            if any(k in line for k in ("SENSING", "ERROR", "WARN", "overflow", "sync",
                                       "Scanning GSCN", "Cell Detected", "pbch", "PBCH")):
                # Wall-clock stamp: the receiver's own lines carry no time, so without this there is
                # no way to tell a line from this second from one ten minutes old.
                self.lines.append(time.strftime("%H:%M:%S ") + line[-400:])
            self.last_line_at = time.time()

    def snapshot(self):
        with self._lock:
            # LIVENESS, not just the last values. A panel that keeps displaying the final numbers
            # of a dead run is indistinguishable from a live one that has not changed, and that
            # ambiguity has cost real debugging time: a stalled receiver, a run still settling and
            # a healthy idle cell all looked identical. Publish what the operator has to know --
            # which file is being read, how long since it grew, and whether a receiver process
            # exists at all -- so the page can say which of those it is.
            age = (time.time() - self.last_line_at) if self.last_line_at else None
            return {"counters": dict(self.counters), "stats": dict(self.stats),
                    "path": str(self.path),
                    "age_s": age,
                    "proc_alive": _softmodem_alive(),
                    # When the receiver last wrote ANYTHING. The report stream only proves a CPI
                    # closed, and a receiver can be up, streaming and visibly unwell without
                    # closing one -- an arm that loses PBCH lock in a few seconds emits one CPI and
                    # then nothing. Reading that as "no receiver" hides exactly the state an
                    # operator most needs to see, so liveness is tracked separately from reports.
                    "last_line_at": self.last_line_at,
                    "fault": self.fault,
                    "rbmap": self.rbmap,
                    "ul": self._ul_strip(),
                    "agnostic": dict(self.agnostic),
                    "started_at": self.started_at,
                    "skipped_bytes": self.skipped_bytes,
                    "lines": list(self.lines)[-400:]}

    def paper_snapshot(self):
        with self._lock:
            out = self.paper.snapshot()
            out["key_lines"] = list(self.key_lines)
            out["proc_alive"] = _softmodem_alive()
            out["cpu_pct"] = _PROC_CACHE["cpu"]
            out["now"] = time.time()
            return out

    def _ul_strip(self):
        """UL occupancy on the same 0-9 axis as the DL map, then DECAY so the strip tracks the
        recent past rather than the whole run (the DL side gets this for free: the receiver resets
        its counters when it prints)."""
        mx = max(self.ul_occ) if self.ul_occ else 0
        if mx <= 0:
            return None
        occ = "".join(str((v * 9 + mx // 2) // mx) for v in self.ul_occ)
        crc = "".join(str((self.ul_ok[i] * 9 + self.ul_occ[i] // 2) // self.ul_occ[i]) if self.ul_occ[i] else "0"
                      for i in range(len(self.ul_occ)))
        out = {"grants": self.ul_grants, "peak": mx, "occ": occ, "crc": crc}
        self.ul_occ = [v * 3 // 4 for v in self.ul_occ]
        self.ul_ok = [v * 3 // 4 for v in self.ul_ok]
        return out



class DecoderLogTail(LogTail):
    """Use final worker outcomes and independent timestamps for decoder health."""

    _QUEUE = re.compile(
        r"\bP(DSCH|USCH)Q queued=(\d+) decoded=(\d+) crc_ok=(\d+)"
        r"(?: \([\d.]+%\))? dropped\[full=(\d+) stale=(\d+)\]"
        r" max_lag_slots=(\d+)/(\d+)")

    def __init__(self, path, maxlines=4000):
        self._decoder_lock = threading.Lock()
        self._decoder_stats = {}
        self._decoder_seen = {}
        self._decoder_previous = {}
        super().__init__(path, maxlines)

    def _ingest(self, line):
        super()._ingest(line)
        match = self._QUEUE.search(line)
        if not match:
            return
        direction = "dl" if match[1] == "DSCH" else "ul"
        queued, tried, ok, full, stale, lag, margin = map(int, match.groups()[1:])
        now = time.time()
        with self._decoder_lock:
            previous = self._decoder_previous.get(direction)
            rate = 100.0 * ok / tried if tried else None
            recent = None
            if previous and tried > previous[0] and ok >= previous[1]:
                recent = 100.0 * (ok - previous[1]) / (tried - previous[0])
            self._decoder_previous[direction] = (tried, ok)
            self._decoder_seen[direction] = now
            self._decoder_stats.update({
                direction + "_queued": queued,
                direction + "_try": tried,
                direction + "_ok": ok,
                direction + "_fail": tried - ok,
                direction + "_drop_full": full,
                direction + "_drop_stale": stale,
                direction + "_max_lag": lag,
                direction + "_sample_margin": margin,
                direction + "_recent_rate": recent,
            })
            if direction == "dl":
                self._decoder_stats.update(
                    dl_tb_rate=rate, pdsch_try=tried, pdsch_ok=ok, pdsch_rate=rate)
            else:
                self._decoder_stats["ul_rate"] = rate

    def snapshot(self):
        result = super().snapshot()
        with self._decoder_lock:
            result["stats"].update(self._decoder_stats)
            result["decoder_seen"] = dict(self._decoder_seen)
        return result


def replay_thread(path, store, rate_hz):
    """Feed a recorded report JSONL through the same store as the live path, for testing the GUI
    with no receiver attached. Same code path as ZMQ from add() onward, so what you see is what a
    live run renders."""
    period = 1.0 / max(0.01, rate_hz)
    while True:
        with open(path, "r", errors="replace") as f:
            n = 0
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    store.add("replay:" + path, json.loads(line))
                    n += 1
                except json.JSONDecodeError:
                    continue
                time.sleep(period)
        print(f"[monitor] replay {path}: {n} reports, looping")


def sub_thread(endpoint, store):
    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.SUB)
    sock.setsockopt_string(zmq.SUBSCRIBE, "")
    sock.connect(endpoint)
    print(f"[monitor] subscribed to {endpoint}")
    while True:
        try:
            msg = sock.recv(copy=True)
        except zmq.ZMQError as e:
            print(f"[monitor] {endpoint}: {e}")
            time.sleep(1.0)
            continue
        for line in msg.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                store.add(endpoint, json.loads(line))
            except json.JSONDecodeError as e:
                print(f"[monitor] bad JSON from {endpoint}: {e}")


def make_handler(store, logtail, html_path):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            # monitor.html is re-read from disk on every request precisely so an edit shows up
            # without a restart -- but with no cache header the browser is free to keep serving the
            # copy it already has, which makes an applied fix look like it was never made. Cost of
            # revalidating a ~60 kB page against localhost is nil.
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/fast"):
                # The 75 ms (one CPI) poll. /state carries 400 log lines and every receiver report;
                # sending that 13x a second is what made the old page feel heavy, so the live panel
                # polls this instead: counters, the two RB strips, the fault banner, the timers.
                lg = logtail.snapshot() if logtail else {}
                payload = {
                    "now": time.time(),
                    "stats": lg.get("stats", {}),
                    "counters": lg.get("counters", {}),
                    "rbmap": lg.get("rbmap"),
                    "ul": lg.get("ul"),
                    "fault": lg.get("fault"),
                    "agnostic": lg.get("agnostic", {}),
                    "age_s": lg.get("age_s"),
                    "proc_alive": lg.get("proc_alive"),
                    "path": lg.get("path"),
                    "last_line_at": lg.get("last_line_at"),
                    "started_at": lg.get("started_at"),
                }
                self._send(200, json.dumps(payload).encode(), "application/json")
            elif self.path.startswith("/paper"):
                self._send(200, json.dumps(logtail.paper_snapshot() if logtail else {}).encode(),
                           "application/json")
            elif self.path.startswith("/state"):
                payload = {
                    "receivers": store.snapshot(),
                    "log": logtail.snapshot() if logtail else None,
                    "source_names": SOURCE_NAMES,
                    "ul_sources": sorted(UL_SOURCES),
                    "now": time.time(),
                }
                self._send(200, json.dumps(payload).encode(), "application/json")
            elif self.path in ("/", "/index.html"):
                self._send(200, html_path.read_bytes(), "text/html; charset=utf-8")
            else:
                self._send(404, b"not found", "text/plain")

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--connect", action="append", default=[], metavar="ENDPOINT",
                    help="receiver report_endpoint, e.g. tcp://127.0.0.1:5556 (repeatable)")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port (default 8080)")
    ap.add_argument("--bind", default="0.0.0.0", help="HTTP bind address (default 0.0.0.0, for SSH access)")
    ap.add_argument("--log", help="receiver log file to tail for radio/decode health")
    ap.add_argument("--seed", metavar="FILE.jsonl",
                    help="backfill the store from a report JSONL at startup, then subscribe live. "
                         "ZeroMQ PUB/SUB does not replay, so without this a monitor restart shows an "
                         "EMPTY page until the next CPI closes -- up to several minutes at a long "
                         "cpi_slots, which reads as a dead receiver.")
    ap.add_argument("--replay", metavar="FILE.jsonl",
                    help="replay a recorded report JSONL instead of subscribing (testing, no receiver needed)")
    ap.add_argument("--replay-rate", type=float, default=2.0,
                    help="reports per second when replaying (default 2)")
    args = ap.parse_args()

    store = ReportStore()
    if args.seed:
        n = 0
        try:
            with open(args.seed) as fh:
                for line in fh:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        store.add("seed:" + args.seed, json.loads(line))
                        n += 1
                    except (ValueError, TypeError):
                        continue  # a torn last line while the receiver is mid-write
        except OSError as e:
            print(f"[monitor] seed {args.seed}: {e}")
        # Seeded reports all land at once, so their ARRIVAL gaps are ~0 and would peg the derived
        # staleness threshold at its floor. Recover the real cadence from the reports' own
        # cpi_start_time_utc_ns instead -- that is wall-clock truth and survives the restart.
        with store._lock:
            for st in store._rx.values():
                st["gaps"].clear()
                t = sorted(h["t"] for h in st["history"] if h.get("t"))
                for a, b in zip(t, t[1:]):
                    gap = (b - a) / 1e9
                    if 0.0 < gap < 3600.0:
                        st["gaps"].append(gap)
        print(f"[monitor] seeded {n} reports from {args.seed}")
    if args.replay:
        threading.Thread(target=replay_thread, args=(args.replay, store, args.replay_rate),
                         daemon=True).start()
    else:
        for ep in (args.connect or ["tcp://127.0.0.1:5556"]):
            threading.Thread(target=sub_thread, args=(ep, store), daemon=True).start()

    logtail = None
    if args.log:
        logtail = DecoderLogTail(args.log)
        threading.Thread(target=logtail.run, daemon=True).start()

    html_path = Path(__file__).with_name("monitor.html")
    if not html_path.exists():
        raise SystemExit(f"missing {html_path}")

    srv = ThreadingHTTPServer((args.bind, args.port), make_handler(store, logtail, html_path))
    print(f"[monitor] http://{args.bind}:{args.port}/  (ssh -L {args.port}:localhost:{args.port} sens6)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
