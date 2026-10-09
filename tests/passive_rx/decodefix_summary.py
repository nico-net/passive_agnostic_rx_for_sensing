#!/usr/bin/env python3
"""Summarize DECODEDIAG independently of probes; never pool decoder backends."""
import collections
import re
import sys

rows = []
rescues = {}
for path in sys.argv[1:]:
    with open(path, errors="replace") as src:
        for line in src:
            if "SENSING: DECODETDRA " in line:
                d = dict(re.findall(r"(\w+)=([^\s]+)", line))
                rescues[(path, d["rnti"], d["abs"])] = int(d["after"])
            if "SENSING: DECODEDIAG " not in line:
                continue
            d = dict(re.findall(r"(\w+)=([^\s]+)", line))
            if d.get("probe") != "0" or d.get("attempt", "0") != "0" or int(d["rnti"], 16) <= 127 or d.get("rnti") == "0xffff":
                continue
            d["file"] = path
            rows.append(d)

def summarize(fields, select=lambda r: True):
    bins = collections.defaultdict(list)
    for r in rows:
        if select(r):
            bins[tuple(r[f] for f in fields)].append(r)
    print(" | ".join(fields + ["pass/total", "CRC %", "after pass/total", "after %", "mean SNR", "G!=produced", "G>last nonzero"]))
    for key, values in sorted(bins.items()):
        ok = sum(int(r["crc"]) for r in values)
        n = len(values)
        after = sum(int(r["crc"]) or rescues.get((r["file"], r["rnti"], r["abs"]), 0) for r in values)
        print(" | ".join(map(str, key + (f"{ok}/{n}", f"{100*ok/n:.1f}", f"{after}/{n}", f"{100*after/n:.1f}",
              f"{sum(float(r['snr']) for r in values)/n:.2f}",
              sum(r["G"] != r["produced"] for r in values),
              sum(int(r["G"]) > int(r["llr_have"]) for r in values)))))

for r in rows:
    g = int(r["G"])
    r["size"] = "<8k" if g < 8000 else "8-16k" if g <= 16000 else ">16k"
print("All full dedicated attempts:")
summarize(["decoder", "nl", "mcs", "size"])
print("SNR >= 6 dB:")
summarize(["decoder", "nl", "mcs", "size"], lambda r: float(r["snr"]) >= 6)
print("Low MCS, high SNR diagnostic partitions:")
for field in ["rv", "combined", "C", "cb", "dmrsid", "dataid", "S", "L", "csi_re", "ssb_re"]:
    summarize(["decoder", field], lambda r: float(r["snr"]) >= 6 and int(r["mcs"]) <= 4)
