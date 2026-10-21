"""Line parsers grounded in the receiver's C format strings (file:line of each in the comment above it).

parse_line(line) -> (kind, dict) | None. Deliberately drops DCI/TB payload fields (UL_PUSCH_CRC tb=...): they are
never captured, so nothing downstream can persist them. Unavailable values are None, never 0.
"""
import json
import re

ANSI = re.compile(r"\x1b\[[0-9;]*m")


class SfnUnwrap:
    """(frame 0..1023, slot) -> monotone ext_slot. Chooses the unwrapped frame nearest the latest seen one, so late
    (async) lines from just before a wrap stay before it and a real wrap advances by 1024 frames."""

    def __init__(self, slots_per_frame=20):
        self.spf, self.ref = slots_per_frame, None

    def __call__(self, frame, slot):
        if self.ref is None:
            self.ref = frame
        ext = self.ref + ((frame - self.ref + 512) % 1024) - 512
        self.ref = max(self.ref, ext)
        return ext * self.spf + slot


def _num(v):
    try:
        return int(v)
    except ValueError:
        try:
            return float(v)
        except ValueError:
            return v


def _us(v):  # E2E_US / first_body: -1 means "timestamp missing" -> unavailable
    return None if v < 0 else v


# nr_pdsch_passive_queue.c:1642 EVMGRANT; the e2e=/es= tail at :1626-1634; stg= from nr_pdsch_passive_decode.c:4082
_EVM = re.compile(r"EVMGRANT rnti=0x([0-9a-fA-F]+) (\d+)\.(\d+) (.*)")
_E2E_KEYS = ("arr2enq", "qwait", "pre", "dec", "first_body", "bodies", "tot", "lag")


def parse_evmgrant(line):
    m = _EVM.search(line)
    if not m:
        return None
    kv = {}
    for tok in m.group(4).split():
        k, sep, v = tok.partition("=")
        if sep:
            kv[k] = v
    try:
        rb = kv["rb"].split("+")
        r = {"rnti": int(m.group(1), 16), "frame": int(m.group(2)), "slot": int(m.group(3)), "crc": int(kv["crc"]),
             "mcs": int(kv["mcs"]), "tbl": int(kv["tbl"]), "qm": int(kv["Qm"]), "nl": int(kv["Nl"]),
             "rb_start": int(rb[0]), "rb_n": int(rb[1]), "nsym": int(kv["nsym"]), "evm": float(kv["evm"]),
             "snr": float(kv["snr"]), "nbrx": int(kv["nbrx"]), "probe": int(kv["probe"]), "legacy_dec_us": float(kv["dec_us"]),
             "rv": int(kv["rv"]), "ndi": int(kv["ndi"]), "pid": int(kv["pid"]), "tbs": int(kv["tbs"]),
             "skip": int(kv["skip"])}
    except (KeyError, ValueError, IndexError):
        return None
    e = kv.get("e2e")
    r["e2e"] = None
    if e:
        v = [int(x) for x in e.split(",")]
        if len(v) == 8:
            r["e2e"] = {k: (x if k in ("bodies", "lag") else _us(x)) for k, x in zip(_E2E_KEYS, v)}
    es = kv.get("es")
    r["es"] = None
    if es:
        sym, lead = (int(x) for x in es.split(","))
        r["es"] = {"sym": sym, "lead_us": _us(lead)}
    stg = {}
    if "stg" in kv:  # wall/cpu/gpu us per stage; wall is what is compared across stages
        for part in kv["stg"].split(","):
            name, _, val = part.partition(":")
            if name in ("fep", "chest", "demod", "ldpc") and "/" in val:
                stg[name] = int(val.split("/")[0])
    r["stg"] = stg or None
    return "evmgrant", r


# nr_pdcch_blind_monitor_rt.c:3470 (rnti_seen) and :7187 (ul_seen)
_SEEN = re.compile(r"blind PDCCH (rnti|ul)_seen utc_ns=(\d+) rnti=0x([0-9a-fA-F]+) sfn=(\d+) slot=(\d+) fmt=(\S+) cce=(\d+) al=(\d+)")


def parse_seen(line):
    m = _SEEN.search(line)
    if not m:
        return None
    fmt = m.group(6)
    return "dci_seen", {"utc_ns": int(m.group(2)), "rnti": int(m.group(3), 16), "frame": int(m.group(4)),
                        "slot": int(m.group(5)), "fmt": fmt, "dir": "dl" if fmt in ("1_0", "1_1") else "ul",
                        "cce": int(m.group(7)), "al": int(m.group(8))}


# nr_pusch_passive_monitor_rt.c:99: tb=<hex payload> exists on this line and is intentionally not matched
_ULCRC = re.compile(r"UL_PUSCH_CRC rnti=0x([0-9a-fA-F]+) ok=(\d) mcs=(\d+) prb=(\d+)\+(\d+) .*? snr_db=(-?[\d.]+)")


def parse_ul_crc(line):
    m = _ULCRC.search(line)
    if not m:
        return None
    return "ul_crc", {"rnti": int(m.group(1), 16), "ok": int(m.group(2)), "mcs": int(m.group(3)),
                      "rb_start": int(m.group(4)), "rb_n": int(m.group(5)), "snr": float(m.group(6))}


# nr_passive_metrics.c:228 emits "SENSING: ISAC_METRICS {json}"
def parse_metrics(line):
    i = line.find("ISAC_METRICS {")
    if i < 0:
        return None
    try:
        o = json.loads(line[i + 13:])
    except ValueError:
        return None
    return ("metrics", o) if isinstance(o, dict) else None


# nr_pdsch_passive_decode.c:326 PDTIMCLS (legacy dashboard decode-body latency)
_PDTIMCLS = re.compile(r"(real|retry|probe)\[n=(\d+) mean=([\d.]+) p50=(\d+) p90=(\d+) p99=(\d+) max=([\d.]+)")


def parse_pdtimcls(line):
    d = {m.group(1): {"n": int(m.group(2)), "p50": int(m.group(4)), "p90": int(m.group(5)), "p99": int(m.group(6))}
         for m in _PDTIMCLS.finditer(line)}
    return ("pdtimcls", d) if d else None


# Q:2153 RECOVLATE, nr_pusch_passive_queue.c:495 UL_RETRY, Q:1686 LAYOUT_PROBE
_RECOV = re.compile(r"RECOVLATE rnti=0x([0-9a-fA-F]+) (\d+)\.(\d+) abs=(-?\d+) pid=(\d+) ndi=(\d+) tbs=(\d+) recovered_cfg=(\d+) tot_us=(-?\d+)")
_ULRETRY = re.compile(r"UL_RETRY rnti=0x([0-9a-fA-F]+) CRC OK with cdm_groups=(\d+) S=(\d+) L=(\d+) .*?tot_us=(-?\d+)")
_LPROBE = re.compile(r"LAYOUT_PROBE n=(\d+) cb0_ok=(\d+)")


def parse_retry(line):
    m = _RECOV.search(line)
    if m:
        return "retry", {"kind": "recovlate", "dir": "dl", "rnti": int(m.group(1), 16), "frame": int(m.group(2)),
                         "slot": int(m.group(3)), "harq_pid": int(m.group(5)), "tbs": int(m.group(7)),
                         "recovered_cfg": bool(int(m.group(8))), "tot_us": _us(int(m.group(9)))}
    m = _ULRETRY.search(line)
    if m:
        return "retry", {"kind": "ul_retry", "dir": "ul", "rnti": int(m.group(1), 16), "cdm": int(m.group(2)),
                         "s": int(m.group(3)), "l": int(m.group(4)), "tot_us": _us(int(m.group(5)))}
    m = _LPROBE.search(line)
    if m:
        return "retry", {"kind": "layout_probe", "n": int(m.group(1)), "cb0_ok": int(m.group(2))}
    return None


# ---- configuration / RF-health lines (audit_lifecycle.md section 4)
_CFG = re.compile(r"CONFIG_EPOCH (\d+) -> (\d+) class=(\w+) cause=(\w+) scope=(\w+)")  # nr_passive_cfg_epoch.c:132
_CFGGAP = re.compile(r"CONFIG_EPOCH continuity gap_samples=(-?\d+) gap_ms=(-?[\d.]+)")  # :136


def parse_config(line):
    m = _CFG.search(line)
    if m:
        return "config", {"kind": "epoch_bump", "epoch_from": int(m.group(1)), "epoch_to": int(m.group(2)),
                          "cls": m.group(3), "cause": m.group(4)}
    m = _CFGGAP.search(line)
    if m:
        return "config", {"kind": "continuity_gap", "gap_samples": int(m.group(1)), "gap_ms": float(m.group(2))}
    return None


# (needle, regex, kind) -> slot_health; field names are the dataset's columns
_HEALTH = (
    ("RXDISCONT abs_slot=", re.compile(r"RXDISCONT abs_slot=(-?\d+) frame=(\d+) slot=(\d+) expected=\d+ actual=\d+ delta=(-?\d+) .*? n=(\d+)"),
     "rxdiscont", ("abs_slot", "frame", "slot", "delta", "n")),  # nr-ue.c:1705
    ("RFTSDISC n=", re.compile(r"RFTSDISC n=(\d+) .*? delta=([+-]?\d+)"), "rftsdisc", ("n", "delta")),  # usrp_lib.cpp:2481
    ("RFCENSUS slots=", re.compile(r"RFCENSUS slots=(\d+) ssb_slots=\d+ pbch_ok=(\d+) pbch_fail=(\d+) .*? frame=(\d+)"),
     "rfcensus", ("slots", "pbch_ok", "pbch_fail", "frame")),  # nr-ue.c:2002
    ("USRP_RING_RECOVER attempt=", re.compile(r"USRP_RING_RECOVER attempt=(\d+)"), "usrp_ring_recover", ("attempt",)),  # :1278
    ("USRP_RING_RESTART ", re.compile(r"USRP_RING_RESTART (\d+) "), "usrp_ring_restart", ("attempt",)),  # :1321
    ("RFSTALL", re.compile(r"RFSTALL"), "rfstall", ()),
    ("RX_RECOVER epoch=", re.compile(r"RX_RECOVER epoch=\d+ gap_samples=(-?\d+) resync_ms=(-?\d+)"), "rx_recover",
     ("gap_samples", "resync_ms")),  # nr-ue.c:1328
)


def parse_health(line):
    for needle, rx, kind, names in _HEALTH:
        if needle in line:
            m = rx.search(line)
            if m:
                d = {"kind": kind}
                d.update({n: int(g) for n, g in zip(names, m.groups())})
                return "health", d
    return None


# ---- discovery lifecycle (audit_lifecycle.md section 1): (needle, regex, event, ((group index, field), ...))
_LIFE = (
    ("Starting sync detection", None, "sync_start", ()),  # nr-ue.c:247
    ("Cell Detected with GSCN", None, "cell_detected", ()),  # nr_initial_sync.c:828
    ("Initial sync successful, PCI:", re.compile(r"PCI: (\d+)"), "sync_ok", ((1, "pci"),)),  # :941
    ("pbch decoded sucessfully", None, "pbch_ok", ()),  # :167 (sic)
    ("ACQ_EVENT pbch_locked", None, "pbch_locked", ()),
    ("ACQ_EVENT sib1_decoded", None, "sib1_decoded", ()),
    ("CSS0 autoconf from", None, "css0_autoconf", ()),
    ("CORESET VERIFIED", None, "coreset_verified", ()),
    ("multi-CORESET bank add", None, "coreset_bank_add", ()),
    ("DCI 1_1 length locked", re.compile(r"rnti=0x([0-9a-fA-F]+) len=(\d+)"), "dci_len_locked", ((1, "rnti16"), (2, "ival"))),
    ("BWP RESOLVED", None, "bwp_resolved", ()),
    ("Technique D CONVERGED", re.compile(r"rnti=0x([0-9a-fA-F]+) tda=(\d+) S=(\d+) L=(\d+)"), "td_converged",
     ((1, "rnti16"), (2, "ival"), (3, "ival2"))),
    ("FIELDBOOK PROMOTED", None, "fieldbook_promoted", ()),
    ("FIELDBOOK WITHDRAWN", None, "fieldbook_withdrawn", ()),
    ("ULLAYOUT LOCK", None, "ul_locked", ()),
    ("ULLAYOUT BWP_LOCK", None, "ul_locked", ()),
)
_ACQ = re.compile(r"ACQ_STATE (\w+) -> (\w+)")  # nr_passive_acq_state.c:106


def parse_life(line):
    if "ACQ_STATE " in line:
        m = _ACQ.search(line)
        if m:
            return "life", {"event": "acq_state", "state_from": m.group(1), "state_to": m.group(2)}
    for needle, rx, ev, fields in _LIFE:
        if needle in line:
            d = {"event": ev}
            if rx:
                m = rx.search(line)
                if not m:
                    return None
                for gi, name in fields:
                    d[name] = int(m.group(gi), 16) if name == "rnti16" else int(m.group(gi))
                if "rnti16" in d:
                    d["rnti"] = d.pop("rnti16")
            return "life", d
    return None


# fast pre-dispatch: first matching needle decides, so cheap lines cost one substring test per entry
_DISPATCH = (
    ("EVMGRANT", parse_evmgrant), ("_seen utc_ns=", parse_seen), ("ISAC_METRICS {", parse_metrics),
    ("UL_PUSCH_CRC", parse_ul_crc), ("PDTIMCLS", parse_pdtimcls), ("RECOVLATE", parse_retry), ("UL_RETRY", parse_retry),
    ("LAYOUT_PROBE", parse_retry), ("CONFIG_EPOCH", parse_config),
)
_HEALTH_NEEDLES = tuple(h[0] for h in _HEALTH)
_LIFE_NEEDLES = ("ACQ_STATE ",) + tuple(x[0] for x in _LIFE)


def parse_line(line):
    if "\x1b" in line:
        line = ANSI.sub("", line)
    for needle, fn in _DISPATCH:
        if needle in line:
            return fn(line)
    for n in _HEALTH_NEEDLES:
        if n in line:
            return parse_health(line)
    for n in _LIFE_NEEDLES:
        if n in line:
            return parse_life(line)
    return None
