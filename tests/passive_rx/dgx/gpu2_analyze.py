#!/usr/bin/env python3
"""Summarize paired offline GPU2 replay logs. No receiver or device access."""
import collections
import json
import math
import re
import statistics
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def percentile(values, q):
    if not values:
        return None
    values = sorted(values)
    return round(values[max(0, math.ceil(q * len(values)) - 1)], 1)


def read(path):
    dci = collections.Counter()
    crc = collections.Counter()
    evm = collections.defaultdict(list)
    slot = {}
    scan = None
    gpu_check = []
    with open(path, errors="replace") as stream:
        for raw in stream:
            line = ANSI.sub("", raw)
            if "SCANTIME_FINE ded-only" in line:
                m = re.search(r"occasions=(\d+) p50<=(\d+)us p99<=(\d+)us over500=(\d+)", line)
                if m:
                    scan = dict(zip(("n", "p50_us", "p99_us", "over500"), map(int, m.groups())))
            if "DCIRAW " in line:
                m = re.search(r"rnti=(0x[0-9a-fA-F]+)", line)
                if m:
                    dci[m.group(1).lower()] += 1
            if "EVMGRANT " in line:
                m = re.search(r"rnti=(0x[0-9a-fA-F]+).*?crc=(\d+).*?snr=([-\d.]+)", line)
                if m:
                    rnti, passed, snr = m.groups()
                    rnti = rnti.lower()
                    crc[rnti] += int(passed)
                    evm[rnti].append(float(snr))
            if "PDSCH_SLOT_TIME " in line:
                m = re.search(r"slot=(\d+) jobs=(\d+) gpu_jobs=(\d+) start_ns=(\d+) end_ns=(\d+)", line)
                if m:
                    key, jobs, gpu, start, end = map(int, m.groups())
                    if key not in slot:
                        slot[key] = [start, end, 0, 0]
                    row = slot[key]
                    row[0] = min(row[0], start)
                    row[1] = max(row[1], end)
                    row[2] += jobs
                    row[3] += gpu
            if "GPU_SELFCHECK" in line:
                gpu_check.append(line.strip())
    times = [(v[1] - v[0]) / 1000 for v in slot.values()]
    return {
        "scan": scan,
        "decode_slots": len(slot),
        "decode_jobs": sum(v[2] for v in slot.values()),
        "gpu_jobs": sum(v[3] for v in slot.values()),
        "decode_p50_us": percentile(times, .5),
        "decode_p99_us": percentile(times, .99),
        "decode_max_us": percentile(times, 1),
        "dci": dict(sorted(dci.items())),
        "crc_ok": dict(sorted(crc.items())),
        "evm_snr": {k: {"n": len(v), "median_db": round(statistics.median(v), 2)}
                    for k, v in sorted(evm.items())},
        "gpu_selfcheck_tail": gpu_check[-5:],
    }


def paired(cpu_path, gpu_path):
    pattern = re.compile(
        r"EVMGRANT rnti=(0x[0-9a-fA-F]+) (\d+)\.(\d+) crc=(\d+) mcs=(\d+)"
        r".*?Nl=(\d+) rb=(\d+\+\d+).*?snr=([-\d.]+)"
    )

    def rows(path):
        out = {}
        with open(path, errors="replace") as stream:
            for line in stream:
                match = pattern.search(line)
                if match:
                    rnti, frame, slot, crc, mcs, nl, rb, snr = match.groups()
                    out[(rnti.lower(), frame, slot, mcs, nl, rb)] = (int(crc), float(snr))
        return out

    cpu, gpu = rows(cpu_path), rows(gpu_path)
    common = cpu.keys() & gpu.keys()
    differences = [abs(cpu[key][1] - gpu[key][1]) for key in common]
    return {
        "cpu_rows": len(cpu), "gpu_rows": len(gpu), "common_rows": len(common),
        "crc_disagreements": sum(cpu[key][0] != gpu[key][0] for key in common),
        "snr_over_0_1_db": sum(value > 0.1 for value in differences),
        "max_abs_snr_db": round(max(differences, default=0), 2),
        "key_fields": "RNTI, frame, slot, MCS, layers, RB allocation",
        "key_limit": "Does not include scrambling hypothesis, RV or HARQ history",
    }


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: gpu2_analyze.py replay/run.log [...] | --pair cpu/run.log gpu/run.log")
    if sys.argv[1] == "--pair":
        if len(sys.argv) != 4:
            sys.exit("usage: gpu2_analyze.py --pair cpu/run.log gpu/run.log")
        print(json.dumps(paired(sys.argv[2], sys.argv[3]), indent=2))
    else:
        print(json.dumps({path: read(path) for path in sys.argv[1:]}, indent=2))
