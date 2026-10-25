#!/usr/bin/env python3
"""Lab only: convert OCUDU (srsRAN-family) gNB ground truth to the CSV that groundtruth.py --gnb-csv reads.
Validation ground truth only, never receiver input.

  python3 ocudu_gt.py --log gnb.log[.gz] --out sched.csv [--pci N] [--include-common] [--slots-per-frame 20]
  python3 ocudu_gt.py --pcap gnb_mac.pcap --out mac.csv --subframe-ok     # coarse, see below
  python3 ocudu_gt.py --pcap gnb_mac.pcap --summary                       # counts only, no CSV

Source 1 (preferred): the SCHED "Slot decisions" record (lib/scheduler/logging/scheduler_result_logger.cpp).
  log.mac_level=info  -> one line per slot, "DL: ue= c-rnti= h_id= ss_id= rb= k1= cw[0]: newtx= rv= tbs= [ri= dl_bo=]"
                         and "UL: ue= rnti= h_id= ss_id= rb= newtx= rv= tbs= (k2=|msg3_delay=)"; no MCS.
  log.mac_level=debug -> multi-line record: "- DL PDCCH: ...", "- UE PDSCH: ... tbs= mcs= rv= nrtx= k1= [ri=]",
                         "- UE PUSCH: ... nof_layers= ... k2=" (adds MCS, symbols, DCI format/CCE/AL).
  The [F.S] slot of the record is the PDSCH/PUSCH slot. DL: the PDCCH is in the same record (k0=0 in this
  scheduler; dci_frame/dci_slot are filled from the DL PDCCH entry when the log is debug). UL: dci slot = slot - k2.
  This is what the MAC scheduled, not what the PHY emitted (same caveat as the OAI parser).
Source 2: the MAC pcap (DLT 252 exported-PDU "mac-nr-framed", or the "mac-nr" UDP framing). OCUDU writes only
  MAC_NR_FRAME_SUBFRAME_TAG = (SFN << 4) | subframe (lib/pcap/mac_pcap_impl.cpp:107): at SCS > 15 kHz the slot inside the
  subframe is lost, so rows carry slot = subframe * slots_per_subframe (first candidate) and slot_exact=0. groundtruth.py
  joins on the exact slot, so such a CSV under-counts matches; it is written only with --subframe-ok.
  A MAC_NR_FRAME_SLOT_TAG (0x07) record, if a future build writes one, gives an exact slot.
Output columns (groundtruth.py reads frame,slot,rnti,dir and ignores the rest): see COLS. dir is lowercase dl/ul.
"""
import argparse
import collections
import csv
import gzip
import re
import struct
import sys

COLS = ["frame", "slot", "rnti", "dir", "kind", "pci", "ue", "h_id", "newtx", "rv", "nrtx", "tbs", "tbs_cw1", "mcs",
        "mcs_cw1", "layers", "rb", "symb", "k1", "k2", "dci_frame", "dci_slot", "dci_format", "cce", "al", "slot_exact",
        "t"]

_TS = re.compile(r"^(\d{4}-\d\d-\d\dT\S+) \[(\S+)\s*\] \[(\w)\] (.*)$")
_HDR = re.compile(r"^\[\s*(\d+)\.(\d+)\] Slot decisions pci=(\d+) [^:]*:\s?(.*)$")
_RNTI = r"(0x[0-9a-fA-F]+)"
_DL_I = re.compile(r"DL: ue=(\d+) c-rnti=" + _RNTI + r" h_id=(\d+) ss_id=(\d+) rb=(\S+) k1=(\d+) cw\[0\]: newtx=(\w+) "
                   r"rv=(\d+) tbs=(\d+)(?: cw\[1\]: newtx=(\w+) rv=(\d+) tbs=(\d+))?(?: ri=(\d+))?")
_UL_I = re.compile(r"UL: ue=(\d+) rnti=" + _RNTI + r" h_id=(\d+) ss_id=(\d+) rb=(\S+) newtx=(\w+) rv=(\d+) tbs=(\d+)"
                   r"(?: msg3_delay=(\d+)| k2=(\d+))?")
_RAR_I = re.compile(r"(?:RAR|MsgB): (?:ra|msgb)-rnti=" + _RNTI + r" rb=(\S+) tbs=(\d+)")
_PDSCH_D = re.compile(r"^- UE PDSCH: ue=(\d+) c-rnti=" + _RNTI + r" h_id=(\d+) rb=(\S+) symb=(\S+) cw\[0\]: tbs=(\d+) "
                      r"mcs=(\d+) rv=(\d+) nrtx=(\d+) k1=(\d+)(?: cw\[1\]: tbs=(\d+) mcs=(\d+) rv=(\d+))?(?: ri=(\d+))?")
_PUSCH_D = re.compile(r"^- UE PUSCH: ue=(\d+) t?c-rnti=" + _RNTI + r" h_id=(\d+) rb=(\S+) symb=(\S+) tbs=(\d+) rv=(\d+) "
                      r"nrtx=(\d+) nof_layers=(\d+)[^\n]*?(?: msg3_delay=(\d+)| k2=(\d+))")
_RAR_D = re.compile(r"^- (?:RAR|MsgB) PDSCH: (?:ra|msgb)-rnti=" + _RNTI + r" rb=(\S+) symb=(\S+) tbs=(\d+) mcs=(\d+) rv=(\d+)")
_PDCCH_D = re.compile(r"^- (DL|UL) PDCCH: rnti=" + _RNTI + r" type=(\S+) cs_id=\d+ ss_id=\d+ format=(\S+) cce=(\d+) al=(\d+)"
                      r" dci: (.*)$")
_KV = re.compile(r"(\w+)=(\S+)")


def _open(path):
    with open(path, "rb") as f:
        gz = f.read(2) == b"\x1f\x8b"
    return gzip.open(path, "rt", errors="replace") if gz else open(path, errors="replace")


def _b(v):
    return None if v is None else int(v in ("true", "1"))


def _row(**kw):
    r = dict.fromkeys(COLS)
    r.update(kw)
    return r


class _Rec:
    """One SCHED Slot decisions record (header + its '- ' continuation lines)."""

    def __init__(self, t, level, frame, slot, pci):
        self.t, self.level, self.frame, self.slot, self.pci = t, level, frame, slot, pci
        self.rows, self.pdcch = [], []


def _info_entries(rec, rest, include_common, spf):
    for m in _DL_I.finditer(rest):
        ue, rnti, hid, _ss, rb, k1, nt, rv, tbs, nt1, _rv1, tbs1, ri = m.groups()
        rec.rows.append(_row(dir="dl", kind="pdsch", ue=int(ue), rnti=int(rnti, 16), h_id=int(hid), rb=rb, k1=int(k1),
                             newtx=int(_b(nt) or _b(nt1) or 0), rv=int(rv), tbs=int(tbs),
                             tbs_cw1=int(tbs1) if tbs1 else None, layers=int(ri) if ri else None))
    for m in _UL_I.finditer(rest):
        ue, rnti, hid, _ss, rb, nt, rv, tbs, m3, k2 = m.groups()
        rec.rows.append(_row(dir="ul", kind="msg3" if m3 else "pusch", ue=int(ue), rnti=int(rnti, 16), h_id=int(hid), rb=rb,
                             newtx=_b(nt), rv=int(rv), tbs=int(tbs), k2=int(k2) if k2 else None))
    if include_common:
        for m in _RAR_I.finditer(rest):
            rec.rows.append(_row(dir="dl", kind="rar", rnti=int(m.group(1), 16), rb=m.group(2), tbs=int(m.group(3))))


def _debug_entry(rec, line, include_common):
    m = _PDSCH_D.match(line)
    if m:
        ue, rnti, hid, rb, sy, tbs, mcs, rv, nrtx, k1, tbs1, mcs1, _rv1, ri = m.groups()
        rec.rows.append(_row(dir="dl", kind="pdsch", ue=int(ue), rnti=int(rnti, 16), h_id=int(hid), rb=rb, symb=sy,
                             tbs=int(tbs), mcs=int(mcs), rv=int(rv), nrtx=int(nrtx), newtx=int(nrtx == "0"), k1=int(k1),
                             tbs_cw1=int(tbs1) if tbs1 else None, mcs_cw1=int(mcs1) if mcs1 else None,
                             layers=int(ri) if ri else None))
        return
    m = _PUSCH_D.match(line)
    if m:
        ue, rnti, hid, rb, sy, tbs, rv, nrtx, nl, m3, k2 = m.groups()
        rec.rows.append(_row(dir="ul", kind="msg3" if m3 else "pusch", ue=int(ue), rnti=int(rnti, 16), h_id=int(hid), rb=rb,
                             symb=sy, tbs=int(tbs), rv=int(rv), nrtx=int(nrtx), newtx=int(nrtx == "0"), layers=int(nl),
                             k2=int(k2) if k2 else None))
        return
    m = _PDCCH_D.match(line)
    if m:
        d, rnti, typ, fmt, cce, al, dci = m.groups()
        kv = dict(_KV.findall(dci))
        rec.pdcch.append({"dir": d.lower(), "rnti": int(rnti, 16), "type": typ, "format": fmt, "cce": int(cce),
                          "al": int(al), "h_id": int(kv["h_id"]) if "h_id" in kv else None,
                          "mcs": int(kv["mcs"]) if "mcs" in kv else None})
        return
    if include_common:
        m = _RAR_D.match(line)
        if m:
            rnti, rb, sy, tbs, mcs, rv = m.groups()
            rec.rows.append(_row(dir="dl", kind="rar", rnti=int(rnti, 16), rb=rb, symb=sy, tbs=int(tbs), mcs=int(mcs),
                                 rv=int(rv)))


def _finish(rec, spf, out, pci):
    if rec is None or (pci is not None and rec.pci != pci):
        return
    dl_dci = collections.defaultdict(list)
    for p in rec.pdcch:
        if p["dir"] == "dl":
            dl_dci[p["rnti"]].append(p)
    for r in rec.rows:
        r.update(frame=rec.frame, slot=rec.slot, pci=rec.pci, slot_exact=1, t=rec.t)
        if r["dir"] == "dl":
            c = dl_dci.get(r["rnti"])
            if c:  # same record = same slot; prefer the PDCCH with the same HARQ id
                p = next((p for p in c if p["h_id"] == r["h_id"]), c[0])
                c.remove(p)
                r.update(dci_frame=rec.frame, dci_slot=rec.slot, dci_format=p["format"], cce=p["cce"], al=p["al"])
                if r["mcs"] is None:
                    r["mcs"] = p["mcs"]
        elif r["k2"] is not None:
            e = rec.frame * spf + rec.slot - r["k2"]
            r.update(dci_frame=(e // spf) % 1024, dci_slot=e % spf)
        out.append(r)


def read_log(path, spf=20, pci=None, include_common=False, stats=None):
    """Rows in log order. stats (dict) gets counters: records, dropped_segment (logger 'exceeded segment size')."""
    out, rec = [], None
    st = stats if stats is not None else {}
    st.setdefault("records", 0)
    st.setdefault("dropped_segment", 0)
    st.setdefault("levels", collections.Counter())
    with _open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            m = _TS.match(line)
            if not m:
                if rec is not None and line.startswith("- "):
                    _debug_entry(rec, line, include_common)
                continue
            _finish(rec, spf, out, pci)
            rec = None
            t, logger, level, rest = m.groups()
            if logger != "SCHED":
                continue
            if "exceeded segment size" in rest:
                st["dropped_segment"] += 1
            h = _HDR.match(rest)
            if not h:
                continue
            st["records"] += 1
            st["levels"][level] += 1
            rec = _Rec(t, level, int(h.group(1)), int(h.group(2)), int(h.group(3)))
            if h.group(4):
                _info_entries(rec, h.group(4), include_common, spf)
    _finish(rec, spf, out, pci)
    return out


# ---- MAC pcap -------------------------------------------------------------------------------------------------
_RNTI_TYPE = {0: "none", 1: "p", 2: "ra", 3: "c", 4: "si", 5: "cs"}


def parse_mac_nr(buf):
    """Wireshark mac-nr framing after the start string: radioType, direction, rntiType, then tag/value pairs up to
    MAC_NR_PAYLOAD_TAG (0x01). Returns a dict or None."""
    if len(buf) < 3:
        return None
    d = {"radio": buf[0], "dir": "dl" if buf[1] == 1 else "ul", "rnti_type": _RNTI_TYPE.get(buf[2], str(buf[2]))}
    i = 3
    sizes = {0x02: 2, 0x03: 2, 0x04: 2, 0x05: 1, 0x06: 1, 0x07: 4}
    while i < len(buf):
        tag = buf[i]
        i += 1
        if tag == 0x01:
            d["payload_len"] = len(buf) - i
            return d
        n = sizes.get(tag)
        if n is None or i + n > len(buf):
            return None
        v = buf[i:i + n]
        i += n
        if tag == 0x02:
            d["rnti"] = struct.unpack(">H", v)[0]
        elif tag == 0x03:
            d["ueid"] = struct.unpack(">H", v)[0]
        elif tag == 0x04:
            x = struct.unpack(">H", v)[0]
            d["frame"], d["subframe"] = x >> 4, x & 0xF
        elif tag == 0x06:
            d["harq"] = v[0]
        elif tag == 0x07:
            d["frame"], d["slot"] = struct.unpack(">HH", v)
    return None


def _pcap_payloads(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            raise ValueError("short pcap header")
        magic = gh[:4]
        if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
            e = "<"
        elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
            e = ">"
        else:
            raise ValueError("not a classic pcap (pcapng unsupported)")
        nano = magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
        dlt = struct.unpack(e + "I", gh[20:24])[0] & 0x0FFFFFFF
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            sec, frac, incl, _orig = struct.unpack(e + "IIII", rh)
            data = f.read(incl)
            if len(data) < incl:
                return  # truncated tail (copied head of a live file)
            yield sec + frac / (1e9 if nano else 1e6), dlt, data


def _exported_pdu(data):
    """DLT 252: TLVs (2-byte tag, 2-byte len, big endian), tag 12 = protocol name, tag 0 = end."""
    i, proto = 0, None
    while i + 4 <= len(data):
        tag, ln = struct.unpack(">HH", data[i:i + 4])
        i += 4
        if tag == 0:
            return proto, data[i:]
        if tag == 12:
            proto = data[i:i + ln].split(b"\0")[0].decode("ascii", "replace")
        i += ln
    return proto, None


def read_pcap(path, slots_per_subframe=2, all_rnti=False, stats=None):
    out = []
    st = stats if stats is not None else {}
    for k in ("records", "mac_nr", "skipped_rnti_type", "unparsed"):
        st.setdefault(k, 0)
    st.setdefault("dlt", set())
    for ts, dlt, data in _pcap_payloads(path):
        st["records"] += 1
        st["dlt"].add(dlt)
        body = None
        if dlt == 252:
            proto, body = _exported_pdu(data)
            if proto not in ("mac-nr-framed", "mac-nr"):
                body = None
        else:
            j = data.find(b"mac-nr")
            if j >= 0 and not data.startswith(b"-framed", j + 6):
                body = data[j + 6:]
        d = parse_mac_nr(body) if body is not None else None
        if d is None or "frame" not in d:
            st["unparsed"] += 1
            continue
        st["mac_nr"] += 1
        if d["rnti_type"] != "c" and not all_rnti:
            st["skipped_rnti_type"] += 1
            continue
        exact = "slot" in d
        slot = d["slot"] if exact else d["subframe"] * slots_per_subframe
        out.append(_row(frame=d["frame"], slot=slot, rnti=d.get("rnti", 0), dir=d["dir"], kind="mac_pdu_" + d["rnti_type"],
                        ue=d.get("ueid"), h_id=d.get("harq"), tbs=d.get("payload_len"), slot_exact=int(exact),
                        t="%.6f" % ts))
    return out


def write_csv(rows, path):
    f = sys.stdout if path == "-" else open(path, "w", newline="")
    try:
        w = csv.DictWriter(f, fieldnames=COLS, lineterminator="\n")
        w.writeheader()
        for r in rows:
            r = dict(r)
            r["rnti"] = "0x%04x" % r["rnti"]
            w.writerow({k: ("" if v is None else v) for k, v in r.items()})
    finally:
        if f is not sys.stdout:
            f.close()


def summarize(rows, spf):
    s = {"rows": len(rows), "by_dir": dict(collections.Counter(r["dir"] for r in rows)),
         "by_kind": dict(collections.Counter(r["kind"] for r in rows)),
         "rnti": {"0x%04x" % k: v for k, v in sorted(collections.Counter(r["rnti"] for r in rows).items())}}
    if rows:
        fr = [r["frame"] for r in rows]
        sl = [r["slot"] for r in rows]
        s.update(frame_min=min(fr), frame_max=max(fr), slot_min=min(sl), slot_max=max(sl),
                 slot_hist=dict(sorted(collections.Counter(sl).items())), sfn_wraps=_wraps(rows),
                 t_first=rows[0]["t"], t_last=rows[-1]["t"])
        dl = [r for r in rows if r["dir"] == "dl"]
        s["dl_with_mcs"] = sum(r["mcs"] is not None for r in dl)
        s["dl_with_layers"] = sum(r["layers"] is not None for r in dl)
        s["dl_newtx"] = sum(r["newtx"] == 1 for r in dl)
    return s


def _wraps(rows):
    n, prev = 0, None
    for r in rows:
        if prev is not None and r["frame"] < prev - 512:
            n += 1
        prev = r["frame"]
    return n


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--log", help="OCUDU gnb log (plain or .gz), log.mac_level info or debug")
    src.add_argument("--pcap", help="OCUDU MAC pcap (pcap.mac_type dlt or udp)")
    ap.add_argument("--out", help="CSV path for groundtruth.py --gnb-csv ('-' = stdout)")
    ap.add_argument("--summary", action="store_true", help="print a JSON summary to stderr")
    ap.add_argument("--pci", type=int, help="keep only this cell (log source)")
    ap.add_argument("--slots-per-frame", type=int, default=20, help="20 = SCS 30 kHz")
    ap.add_argument("--include-common", action="store_true", help="also RAR/MsgB rows (log) / non-C-RNTI PDUs (pcap)")
    ap.add_argument("--subframe-ok", action="store_true",
                    help="pcap: write the CSV although slot is only subframe-accurate (slot_exact=0)")
    a = ap.parse_args(argv)
    if not a.out and not a.summary:
        ap.error("need --out and/or --summary")
    st = {}
    if a.log:
        rows = read_log(a.log, a.slots_per_frame, a.pci, a.include_common, st)
        st["levels"] = dict(st["levels"])
        if not st["records"]:
            sys.stderr.write("ocudu_gt: no SCHED 'Slot decisions' records: is log.mac_level info or debug?\n")
            return 2
    else:
        sps = max(1, a.slots_per_frame // 10)
        rows = read_pcap(a.pcap, sps, a.include_common, st)
        st["dlt"] = sorted(st["dlt"])
        if a.out and sps > 1 and any(not r["slot_exact"] for r in rows) and not a.subframe_ok:
            sys.stderr.write("ocudu_gt: pcap gives only SFN+subframe; at %d slots/subframe groundtruth.py's exact-slot join"
                             " would miss ~%d/%d of grants. Use --log (SCHED Slot decisions) or pass --subframe-ok.\n"
                             % (sps, sps - 1, sps))
            return 2
    if a.out:
        write_csv(rows, a.out)
    if a.summary:
        import json
        s = summarize(rows, a.slots_per_frame)
        s["source"] = st
        sys.stderr.write(json.dumps(s, indent=1, default=str) + "\n")
    if a.out and a.out != "-":
        print("ocudu_gt: %d rows (%d dl, %d ul) -> %s" % (len(rows), sum(r["dir"] == "dl" for r in rows),
                                                         sum(r["dir"] == "ul" for r in rows), a.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
