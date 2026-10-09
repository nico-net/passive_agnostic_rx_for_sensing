#!/usr/bin/env python3
"""CSI-FULL: every dedicated full decode that carried CSI-RS exclusions (csi_re>0) or ran in a TRS slot:
per grant, attempt-0 outcome and whether any recovery attempt passed."""
import re, sys, collections
D = re.compile(r"DECODEDIAG rnti=0x([0-9a-f]+) fs=(\d+)\.(\d+) abs=(-?\d+) probe=(\d) crc=(\d) nl=(\d+) mcs=(\d+) .*?snr=(-?[\d.]+) .*?csi_re=(\d+) .*?rb=(\d+)\+(\d+) .*?attempt=(-?\d+)")
for f in sys.argv[1:]:
    g = collections.OrderedDict()
    for line in open(f, errors="replace"):
        m = D.search(line)
        if not m or m.group(5) != "0" or m.group(1) == "ffff":
            continue
        ph = (int(m.group(2)) * 20 + int(m.group(3))) % 160
        k = (m.group(1), m.group(2), m.group(3))
        e = g.setdefault(k, dict(ph=ph, a0=None, any=0, csi0=None, snr=None, nl=m.group(7), mcs=m.group(8), rb=f"{m.group(11)}+{m.group(12)}"))
        if m.group(13) == "0" and e["a0"] is None:
            e["a0"] = int(m.group(6)); e["csi0"] = int(m.group(10)); e["snr"] = float(m.group(9))
        e["any"] |= int(m.group(6))
    print("==", f)
    T = collections.Counter()
    for k, e in g.items():
        if e["ph"] not in (15, 16, 20, 21) or e["a0"] is None:
            continue
        T[("csi" if e["csi0"] else "nocsi", e["a0"], e["any"])] += 1
        print(f"  0x{k[0]} {k[1]}.{k[2]} ph={e['ph']} nl={e['nl']} mcs={e['mcs']} rb={e['rb']} snr={e['snr']:.1f} csi_re={e['csi0']} crc0={e['a0']} any={e['any']}")
    print("  summary (csi?, crc attempt0, any):", dict(T))
