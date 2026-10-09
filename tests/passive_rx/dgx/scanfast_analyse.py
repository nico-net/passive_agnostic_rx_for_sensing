#!/usr/bin/env python3
"""Summarize one SCAN-FAST replay log without reading the raw IQ capture."""
import collections
import json
import re
import sys


def summarize(path):
    timing = None
    fine = None
    pass_stats = None
    bank_verified = []
    counts = collections.Counter()
    with open(path, encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if "SCANTIME ded-only" in line:
                match = re.search(r"occasions=(\d+) p50<=([\d.]+)us p99<=([\d.]+)us max=([\d.]+)us", line)
                if match:
                    timing = dict(zip(("occasions", "p50_us", "p99_us", "max_us"),
                                      (int(match[1]), float(match[2]), float(match[3]), float(match[4]))))
            elif "SCANTIME_FINE ded-only" in line:
                match = re.search(r"occasions=(\d+) p50<=(\d+)us p99<=(\d+)us over500=(\d+)", line)
                if match:
                    fine = dict(zip(("occasions", "p50_us", "p99_us", "over500"), map(int, match.groups())))
            elif "SCANPASS" in line:
                match = re.search(r"llr_cache hit=(\d+) miss=(\d+) bad=(\d+) gpu_equal=(\d+) mismatch=(\d+) error=(\d+)", line)
                if match:
                    pass_stats = dict(zip(("llr_hit", "llr_miss", "llr_bad", "gpu_equal", "gpu_mismatch", "gpu_error"),
                                          map(int, match.groups())))
                    audit = re.search(r"screen_audit reject=(\d+) hit=(\d+)", line)
                    if audit:
                        pass_stats.update(audit_reject=int(audit[1]), audit_hit=int(audit[2]))
                    score = re.search(r"min_score=([-\d.]+)", line)
                    if score:
                        pass_stats["audit_min_score"] = float(score[1])
                    dmrs = re.search(r"min_dmrs=([-\d.]+) no_dmrs=(\d+)", line)
                    if dmrs:
                        pass_stats.update(audit_min_dmrs=float(dmrs[1]), audit_no_dmrs=int(dmrs[2]))
                    admitted = re.search(r"screen_audit.*accepted=(\d+) min_accepted_score=([-\d.]+)", line)
                    if admitted:
                        pass_stats.update(audit_accepted=int(admitted[1]),
                                          audit_min_accepted_score=float(admitted[2]))
            elif "VERIFIED -> bank add" in line:
                match = re.search(r"nid=(\d+) VERIFIED -> bank add index=(\d+) rnti=0x([0-9a-fA-F]+)", line)
                if match:
                    bank_verified.append(dict(nid=int(match[1]), index=int(match[2]), owner=f"0x{int(match[3], 16):04x}"))
            elif "DCIRAW" in line:
                match = re.search(r"rnti=0x([0-9a-fA-F]+)", line)
                if match:
                    counts[f"0x{int(match[1], 16):04x}"] += 1
    return {"log": path, "scantime": timing, "scantime_fine": fine, "scanpass": pass_stats,
            "banks": bank_verified,
            "dci_total": sum(counts.values()), "dci_by_rnti": dict(sorted(counts.items()))}


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        print(json.dumps(summarize(argument), sort_keys=True))
