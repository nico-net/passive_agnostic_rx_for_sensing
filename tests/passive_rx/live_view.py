#!/usr/bin/env python3
"""Live view of an adaptive passive-RX capture.

Reads ONLY the receiver's own log -- no gNB access, no extra instrumentation, no rebuild,
so it cannot perturb the run it is watching. Tails incrementally: the log reaches hundreds
of megabytes and must never be re-read from the start.

Usage:  python3 live_view.py [capture_dir]      (default: newest adaptive_ul_dl_mrc2.*)
"""
import glob, os, re, sys, time

CAP = "/home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.*"
BOLD, DIM, RED, GRN, YEL, CYN, RST = (
    "\033[1m", "\033[2m", "\033[31m", "\033[32m", "\033[33m", "\033[36m", "\033[0m")

PATTERNS = [
    ("sync",     re.compile(r"Initial sync successful, PCI: (\d+)")),
    ("sib1",     re.compile(r"SIB1 common facts PCI=(\d+) DL-BWP=(\S+) DL-TDAs=(\d+) UL-BWP=(\S+) UL-TDAs=(\d+)")),
    ("seed",     re.compile(r"UL discovery seeded from SIB1: UL-BWP=(\S+) TDAs=(\d+)")),
    ("ullen",    re.compile(r"UL automatic DCI length locked: (\d+) rnti=(0x[0-9a-f]+)")),
    ("armed",    re.compile(r"UL discovery width armed: raw=(\d+) classes=(\d+) rnti=(0x[0-9a-f]+)")),
    ("split",    re.compile(r"UL width classing: (\d+)/(\d+) sampled payloads split")),
    ("conv",     re.compile(r"UL width search converged: class=(\d+)")),
    ("iconv",    re.compile(r"UL interpretation search converged: class=(\d+) tda=(\d+)")),
    ("prog",     re.compile(r"UL (width|interp) progress rnti=(\S+) classes=(\d+) trials=(\d+) "
                            r"min_per_class=(\d+)/(\d+) best=class(\d+) (\d+)/(\d+) winner=(-?\d+)")),
    ("techd",    re.compile(r"Technique D operational rnti=(\S+) config=\S+ tda=(\d+) crc=(\d+)/(\d+)")),
    ("census",   re.compile(r"monitor summary: occasions=(\d+) candidates=(\d+) accepts=(\d+) .*?"
                            r"dci10\[accepts=(\d+).*?dci01\[accepts=(\d+) rejects=(\d+)\]"
                            r"(?:.*?dci00\[accepts=(\d+) rejects=(\d+)\])?"
                            r"(?:.*?ulscan\[sched=(\d+) crc_hit=(\d+) disc=(\d+)\])?")),
    ("pusch",    re.compile(r"pusch_passive\[try=(\d+) crc_ok=(\d+) \(([\d.]+)%\).*?unsup=(\d+)")),
    ("puschq",   re.compile(r"PUSCHQ queued=(\d+) decoded=(\d+) crc_ok=(\d+) "
                            r"dropped\[full=(\d+) stale=(\d+)\] max_lag_slots=(\d+)/(\d+)")),
    ("book",     re.compile(r"pusch_book\[parked=(\d+) claimed=(\d+) expired=(\d+)")),
    ("rf",       re.compile(r"RFCENSUS slots=\d+ ssb_slots=\d+ pbch_ok=(\d+) pbch_fail=(\d+)")),
    ("stall",    re.compile(r"(RXDISCONT|RFSTALL)")),
]

def bar(done, total, width=28):
    if total <= 0:
        return DIM + "-" * width + RST
    f = max(0.0, min(1.0, done / total))
    n = int(f * width)
    col = GRN if f >= 1.0 else (YEL if f > 0.25 else RED)
    return col + "#" * n + DIM + "." * (width - n) + RST

def main():
    d = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(CAP), key=os.path.getmtime)
    log = os.path.join(d, "run.log")
    st = {"stall": 0, "lengths": {}, "armed": {}}
    fh = open(log, "rb")
    # Seed from the tail of what already exists before following. One-shot lines (sync banner,
    # SIB1 facts, length locks) are printed once and would otherwise read as "never happened"
    # for a viewer attached mid-run. Bounded so a 300 MB log is never re-read in full.
    size = os.path.getsize(log)
    fh.seek(max(0, size - 8 * 1024 * 1024))
    t0 = time.time()
    while True:
        for raw in fh.read().split(b"\n"):
            line = raw.decode("utf-8", "replace")
            for key, rx in PATTERNS:
                m = rx.search(line)
                if not m:
                    continue
                if key == "ullen":
                    st["lengths"][m.group(2)] = m.group(1)
                elif key == "armed":
                    st["armed"][m.group(3)] = (m.group(1), m.group(2))
                elif key == "stall":
                    st["stall"] += 1
                else:
                    st[key] = m.groups()
                break
        render(d, st, time.time() - t0)
        time.sleep(2)

def render(d, st, elapsed):
    o = ["\033[H\033[J"]
    o.append(f"{BOLD}adaptive passive RX -- live{RST}   {os.path.basename(d)}   "
             f"watching {int(elapsed//60)}m{int(elapsed%60):02d}s\n")

    sync = st.get("sync"); sib1 = st.get("sib1"); rf = st.get("rf")
    acq = f"{GRN}PCI {sync[0]}{RST}" if sync else f"{YEL}acquiring{RST}"
    o.append(f"{BOLD}ACQUISITION{RST}  sync={acq}")
    if sib1:
        o.append(f"   SIB1 DL-BWP={sib1[1]} ({sib1[2]} TDAs)  UL-BWP={sib1[3]} ({sib1[4]} TDAs)")
    if rf:
        bad = int(rf[1])
        o.append(f"   PBCH ok={rf[0]} fail={(RED if bad else GRN)}{bad}{RST}"
                 f"   discont/stall={(RED if st['stall'] else GRN)}{st['stall']}{RST}")

    c = st.get("census")
    if c:
        o.append(f"\n{BOLD}BLIND PDCCH{RST}  occasions={int(c[0]):,}  candidates={int(c[1]):,}")
        o.append(f"   DL 1_0 accepts={int(c[3]):,}"
                 + (f"   0_0 accepts={int(c[6]):,}" if c[6] else "")
                 + f"   {CYN}UL 0_1 accepts={int(c[4]):,}{RST}")
        if c[9]:
            o.append(f"   ulscan sched={int(c[8]):,} crc_hit={int(c[9]):,} -> controller={int(c[10]):,}")

    td = st.get("techd")
    if td:
        o.append(f"\n{BOLD}DL interpretation{RST}  rnti={td[0]} tda={td[1]}  "
                 f"PDSCH CRC {GRN}{int(td[2]):,}{RST}/{int(td[3]):,}")

    o.append(f"\n{BOLD}UL DISCOVERY{RST}")
    if st.get("seed"):
        o.append(f"   seeded from SIB1: UL-BWP={st['seed'][0]} TDAs={st['seed'][1]}")
    for rnti, ln in sorted(st["lengths"].items()):
        a = st["armed"].get(rnti)
        extra = f"raw={a[0]} classes={a[1]}" if a else f"{DIM}not armed{RST}"
        o.append(f"   rnti={rnti}  DCI 0_1 len={ln}  {extra}")
    if st.get("split"):
        o.append(f"   {YEL}late class splits {st['split'][0]}/{st['split'][1]} sampled{RST}"
                 f" {DIM}(diagnostic; TB CRC remains the authority){RST}")

    p = st.get("prog")
    if p:
        which, rnti, ncls, trials, minpc, need, best, passes, btr, win = p
        o.append(f"   {which} rnti={rnti} classes={ncls} trials={int(trials):,}")
        o.append(f"   slowest class {bar(int(minpc), int(need))} {minpc}/{need}"
                 f"   best=class{best} {passes}/{btr}")
        o.append(f"   winner: " + (f"{GRN}{BOLD}class {win}{RST}" if int(win) >= 0
                                   else f"{DIM}not yet{RST}"))
    if st.get("conv"):
        o.append(f"   {GRN}{BOLD}WIDTH SEARCH CONVERGED -- class {st['conv'][0]}{RST}")
    if st.get("iconv"):
        o.append(f"   {GRN}{BOLD}INTERPRETATION CONVERGED -- class {st['iconv'][0]} "
                 f"tda={st['iconv'][1]}{RST}")

    pu, pq, bk = st.get("pusch"), st.get("puschq"), st.get("book")
    if pu or pq:
        o.append(f"\n{BOLD}PASSIVE UL DECODE{RST}")
        if pu:
            o.append(f"   attempts={int(pu[0]):,}  CRC ok={GRN}{int(pu[1]):,}{RST} ({pu[2]}%)"
                     f"  unsupported={int(pu[3]):,}")
        if bk:
            o.append(f"   grants parked={int(bk[0]):,} claimed={int(bk[1]):,} expired={bk[2]}")
        if pq:
            lag, margin = int(pq[5]), int(pq[6])
            lc = RED if lag > margin else GRN
            o.append(f"   queue decoded={int(pq[1]):,} stale-dropped={int(pq[4]):,} "
                     f"full-dropped={pq[3]}  lag={lc}{lag}{RST}/{margin} slots")
    o.append(f"\n{DIM}reads the receiver's own log only; Ctrl-C to stop{RST}")
    print("\n".join(o), flush=True)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
