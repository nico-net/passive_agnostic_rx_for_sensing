#!/usr/bin/env python3
"""Score one OTA run directory: verdict, SSB search, carrier check, per-RNTI DL, UL, tracker,
convergence time (seconds from receiver start, via nic.csv line-count timeline), verdicts."""
import re, sys, os, csv
def score(d):
    L = open(os.path.join(d, "run.log"), errors="replace").read()
    lines = L.splitlines()
    out = {"dir": os.path.basename(d)}
    v = open(os.path.join(d, "verdict.txt")).read().strip() if os.path.exists(os.path.join(d, "verdict.txt")) else ""
    out["verdict"] = re.search(r"verdict=(\S+)", v).group(1) if "verdict=" in v else "?"
    # nic.csv: epoch,rx_missed,rx_packets,loglines -> map a log line index to seconds since start
    tl = []
    try:
        for row in csv.reader(open(os.path.join(d, "nic.csv"))):
            if len(row) >= 4: tl.append((int(row[0]), int(row[3])))
    except Exception: pass
    t0 = tl[0][0] if tl else None
    def t_of(lineno):
        if not tl: return None
        for ep, n in tl:
            if n >= lineno: return ep - t0
        return tl[-1][0] - t0
    m = re.search(r"Cell Detected with GSCN: (\d+), SSB SC offset: (\d+), SSB Ref: ([0-9.]+)", L)
    out["ssb_scan"] = f"GSCN {m.group(1)} sc {m.group(2)} ssref {float(m.group(3))/1e6:.2f} MHz" if m else "n/a"
    m = re.search(r"ACQ carrier (CONFIRMED|MISMATCH)[^\n]*?(derived carrier centre|carrier centre is) ([0-9.]+) MHz[^\n]*?started[^0-9]*([0-9.]+) MHz", L)
    out["carrier"] = f"{m.group(1)} derived={m.group(3)} started={m.group(4)}" if m else "n/a"
    m = re.search(r"SSB found at subcarrier (\d+)", L); out["ssb_sc_carrier_check"] = m.group(1) if m else "n/a"
    conv = [(i, re.search(r"rnti=0x([0-9a-f]+)", l).group(1)) for i, l in enumerate(lines) if "Technique D CONVERGED" in l]
    out["dl_converged"] = {r: t_of(i) for i, r in conv}
    pref = re.findall(r"layout family PREFERRED by TB CRC: rnti=0x([0-9a-f]+) layout_id=(\d+) \((\d+) candidates\)", L)
    out["layout_pref"] = pref
    cen = [l for l in lines if "PDSCHQ per-rnti" in l]
    out["dl_per_rnti_end"] = cen[-1].split("per-rnti")[1].strip() if cen else "n/a"
    m = re.findall(r"pusch_passive\[try=(\d+) crc_ok=(\d+) \(([0-9.]+)%\)", L); out["ul"] = f"{m[-1][1]}/{m[-1][0]} ({m[-1][2]}%)" if m else "n/a"
    m = re.findall(r"PDSCHQ queued=\d+ decoded=(\d+) crc_ok=(\d+)", L)
    if m and conv:
        i0 = conv[0][0]; before = re.findall(r"PDSCHQ queued=\d+ decoded=(\d+) crc_ok=(\d+)", "\n".join(lines[:i0]))
        if before:
            d0, c0 = map(int, before[-1]); d1, c1 = map(int, m[-1])
            out["dl_post_conv"] = f"{c1-c0}/{d1-d0} ({100*(c1-c0)/max(1,d1-d0):.1f}%)"
    out["dl_total"] = f"{m[-1][1]}/{m[-1][0]}" if m else "n/a"
    out["acq"] = " > ".join(x.split(" -> ")[1] for x in re.findall(r"ACQ_STATE (\w+ -> \w+)", L))
    verd = {k: ("Y" if re.search(p, L) else "-") for k, p in [
        ("dmrs_dl", r"DMRS_ID PDSCH CONFIRMED n_id=2"), ("dmrs_ul", r"DMRS_ID PUSCH CONFIRMED n_id=2"),
        ("xoh", r"XOVERHEAD CONFIRMED = 0"), ("scr_dl", r"DATA_SCRAMBLING_ID PDSCH CONFIRMED"),
        ("scr_ul", r"DATA_SCRAMBLING_ID PUSCH CONFIRMED"), ("mismatch_logged", r"MISMATCH")]}
    out["verdicts"] = verd
    m = re.findall(r"DL_RANK_PROBE n=(\d+).*?big/crcfail n=(\d+) mean=([0-9.]+) low=(\d+)", L)
    out["rank_probe"] = f"n={m[-1][0]} bigfail mean={m[-1][2]} low={m[-1][3]}" if m else "n/a"
    out["rxdiscont"] = len(re.findall(r"RXDISCONT abs_slot", L)); out["lost"] = len(re.findall(r"-> LOST", L))
    out["rntis"] = sorted(set(re.findall(r"rnti=0x([0-9a-f]{4})", L)), key=lambda r: -L.count("rnti=0x" + r))[:3]
    return out
for d in sys.argv[1:]:
    o = score(d)
    for k, v in o.items(): print(f"  {k}: {v}")
    print()
